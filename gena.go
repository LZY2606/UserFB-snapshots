package goupnp

// GENA (General Event Notification Architecture) subscription support.
//
// This implements the single-subscription part of UPnP eventing: one
// SUBSCRIBE request followed by NOTIFY callbacks delivered to an
// http.Handler. Renewal and automatic resubscription are deliberately not
// implemented.

import (
	"bytes"
	"context"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	// EventXMLNamespace is the XML namespace used by UPnP event
	// propertyset documents.
	EventXMLNamespace = "urn:schemas-upnp-org:event-1-0"

	eventNT  = "upnp:event"
	eventNTS = "upnp:propchange"
)

// DefaultEventTimeout is the subscription duration requested in the
// initial SUBSCRIBE request.
const DefaultEventTimeout = 1800 * time.Second

// EventStateHandler is invoked for every confirmed event notification.
//
// changed contains the variables carried by this particular notification.
// snapshot is an independent copy of all variables observed by the
// subscription so far. Both maps are fresh copies: the handler may retain or
// modify them without affecting the subscription.
type EventStateHandler func(changed, snapshot map[string]string)

// EventSubscription is a single GENA event subscription.
//
// Create a subscription with NewEventSubscription, attach it to the callback
// address as an http.Handler, and then call Start to send the SUBSCRIBE
// request. A device is allowed to deliver its initial SEQ=0 NOTIFY before the
// SUBSCRIBE response is received; the subscription holds that event until
// Start has obtained the SID, and only publishes it if the SID matches.
type EventSubscription struct {
	// Service is the service whose event URL receives the SUBSCRIBE request.
	Service *Service

	// CallbackURL is the absolute URL the device should send NOTIFY requests
	// to. It must be served with this EventSubscription as the http.Handler.
	CallbackURL *url.URL

	// Handler receives confirmed events. It is called serially, in event
	// commit order, and without any internal state lock held.
	Handler EventStateHandler

	// HTTPClient performs the SUBSCRIBE request. If nil, HTTPClientDefault is
	// used.
	HTTPClient *http.Client

	// commitMu serializes event processing through the point where the
	// delivery is queued. It is intentionally separate from mu so that a
	// handler can call Snapshot without deadlocking.
	commitMu sync.Mutex

	// mu guards the subscription state below. It is never held while a
	// handler runs.
	mu sync.Mutex

	started bool
	failed  bool
	sid     string
	timeout time.Duration

	// pending is the one as-yet-unconfirmed initial event.
	pending *pendingEvent

	// last event sequence and contents, for duplicate/ordering checks.
	hasLast  bool
	lastSeq  uint32
	lastVars map[string]string

	// values is the accumulated snapshot.
	values map[string]string

	deliveryOnce sync.Once
	deliveryCh   chan eventDelivery
}

type pendingEvent struct {
	sid  string
	vars map[string]string
}

var _ http.Handler = (*EventSubscription)(nil)

type eventDelivery struct {
	changed  map[string]string
	snapshot map[string]string
}

// NewEventSubscription creates a subscription which uses HTTPClientDefault
// for its SUBSCRIBE request. An optional client may be supplied to override
// that default.
func NewEventSubscription(service *Service, callbackURL *url.URL, handler EventStateHandler, clients ...*http.Client) *EventSubscription {
	var client *http.Client
	if len(clients) > 0 {
		client = clients[0]
	}
	return newEventSubscription(service, callbackURL, handler, client)
}

// NewEventSubscriptionWithClient is like NewEventSubscription, but takes the
// HTTP client explicitly.
func NewEventSubscriptionWithClient(service *Service, callbackURL *url.URL, handler EventStateHandler, client *http.Client) *EventSubscription {
	return newEventSubscription(service, callbackURL, handler, client)
}

// NewEventSubscriptionWithHTTPClient is an alias for
// NewEventSubscriptionWithClient.
func NewEventSubscriptionWithHTTPClient(service *Service, callbackURL *url.URL, handler EventStateHandler, client *http.Client) *EventSubscription {
	return newEventSubscription(service, callbackURL, handler, client)
}

func newEventSubscription(service *Service, callbackURL *url.URL, handler EventStateHandler, client *http.Client) *EventSubscription {
	var callback *url.URL
	if callbackURL != nil {
		c := *callbackURL
		callback = &c
	}
	return &EventSubscription{
		Service:     service,
		CallbackURL: callback,
		Handler:     handler,
		HTTPClient:  client,
	}
}

// Start sends the initial SUBSCRIBE request. It returns the SID and timeout
// confirmed by the device. A timeout of zero means the device reported
// "infinite".
//
// The initial event may already have arrived at ServeHTTP before Start
// returns; in that case it is published only after the SID from the response
// has been checked.
func (sub *EventSubscription) Start(ctx context.Context) (sid string, timeout time.Duration, err error) {
	if sub == nil {
		return "", 0, errors.New("goupnp: nil EventSubscription")
	}
	if err := sub.validate(); err != nil {
		sub.abort()
		return "", 0, err
	}

	client := sub.HTTPClient
	if client == nil {
		client = HTTPClientDefault
	}

	sid, timeout, err = sub.subscribe(ctx, client)
	if err != nil {
		sub.abort()
		return "", 0, err
	}

	sub.commitMu.Lock()
	sub.mu.Lock()
	if sub.failed {
		sub.mu.Unlock()
		sub.commitMu.Unlock()
		sub.abort()
		return "", 0, errors.New("goupnp: event subscription failed")
	}
	if sub.started {
		sub.mu.Unlock()
		sub.commitMu.Unlock()
		return "", 0, errors.New("goupnp: event subscription already started")
	}

	sub.started = true
	sub.sid = sid
	sub.timeout = timeout

	var delivery *eventDelivery
	if sub.pending != nil {
		pending := sub.pending
		sub.pending = nil
		if pending.sid == sid {
			// This is the first confirmed event and therefore has SEQ 0.
			delivery = sub.commitEvent(0, pending.vars)
		}
	}
	sub.mu.Unlock()

	if delivery != nil {
		sub.enqueue(*delivery)
	}
	sub.commitMu.Unlock()
	return sid, timeout, nil
}

func (sub *EventSubscription) validate() error {
	if sub.Service == nil {
		return errors.New("goupnp: event subscription has no service")
	}
	if !sub.Service.EventSubURL.Ok && sub.Service.EventSubURL.URL.String() == "" {
		return errors.New("goupnp: service has no event subscription URL")
	}
	if !sub.Service.EventSubURL.URL.IsAbs() {
		return errors.New("goupnp: service event subscription URL is not absolute")
	}
	if sub.CallbackURL == nil || sub.CallbackURL.Scheme == "" || sub.CallbackURL.Host == "" {
		return errors.New("goupnp: event subscription callback URL must be absolute")
	}
	return nil
}

func (sub *EventSubscription) subscribe(ctx context.Context, client *http.Client) (string, time.Duration, error) {
	eventURL := sub.Service.EventSubURL.URL.String()
	req, err := http.NewRequestWithContext(ctx, "SUBSCRIBE", eventURL, nil)
	if err != nil {
		return "", 0, fmt.Errorf("goupnp: building event SUBSCRIBE request: %w", err)
	}
	req.Header.Set("CALLBACK", "<"+sub.CallbackURL.String()+">")
	req.Header.Set("NT", eventNT)
	req.Header.Set("TIMEOUT", fmt.Sprintf("Second-%d", int64(DefaultEventTimeout/time.Second)))

	resp, err := client.Do(req)
	if err != nil {
		return "", 0, fmt.Errorf("goupnp: event SUBSCRIBE request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return "", 0, fmt.Errorf("goupnp: event SUBSCRIBE request got HTTP %s", resp.Status)
	}

	sid := strings.TrimSpace(resp.Header.Get("SID"))
	if sid == "" {
		return "", 0, errors.New("goupnp: event SUBSCRIBE response has empty SID")
	}
	timeout, err := parseEventTimeout(resp.Header.Get("TIMEOUT"))
	if err != nil {
		return "", 0, fmt.Errorf("goupnp: event SUBSCRIBE response has invalid TIMEOUT: %w", err)
	}
	return sid, timeout, nil
}

func (sub *EventSubscription) abort() {
	sub.commitMu.Lock()
	sub.mu.Lock()
	if !sub.started {
		sub.failed = true
		sub.pending = nil
	}
	sub.mu.Unlock()
	sub.commitMu.Unlock()
}

// ServeHTTP handles NOTIFY requests sent by the device to the callback
// address. It implements http.Handler.
func (sub *EventSubscription) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if sub == nil {
		http.Error(w, "nil event subscription", http.StatusInternalServerError)
		return
	}
	if r.Method != "NOTIFY" {
		w.Header().Set("Allow", "NOTIFY")
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}

	// Association checks which do not depend on our local subscription state.
	// UPnP values are case-insensitive tokens.
	if !strings.EqualFold(strings.TrimSpace(r.Header.Get("NT")), eventNT) ||
		!strings.EqualFold(strings.TrimSpace(r.Header.Get("NTS")), eventNTS) {
		w.WriteHeader(http.StatusPreconditionFailed)
		return
	}
	sid := strings.TrimSpace(r.Header.Get("SID"))
	if sid == "" {
		w.WriteHeader(http.StatusPreconditionFailed)
		return
	}

	// commitMu gives concurrent NOTIFY requests a single, serialized commit
	// order. The delivery is queued before commitMu is released, so delivery
	// order matches commit order.
	sub.commitMu.Lock()
	status, delivery := sub.handleNotify(sid, r.Header.Get("SEQ"), r.Body)
	if delivery != nil {
		sub.enqueue(*delivery)
	}
	sub.commitMu.Unlock()
	w.WriteHeader(status)
}

func (sub *EventSubscription) handleNotify(sid, seqHeader string, body io.Reader) (int, *eventDelivery) {
	sub.mu.Lock()
	defer sub.mu.Unlock()

	if sub.failed {
		return http.StatusPreconditionFailed, nil
	}
	if sub.started && sid != sub.sid {
		return http.StatusPreconditionFailed, nil
	}

	seq, err := parseEventSeq(seqHeader)
	if err != nil {
		return http.StatusBadRequest, nil
	}
	vars, err := parsePropertySet(body)
	if err != nil {
		return http.StatusBadRequest, nil
	}

	if !sub.started {
		// The subscribe response handshake is not complete yet. Hold one
		// initial event. It is not published (or even compared with our SID)
		// until Start has the SID from the response.
		if seq != 0 {
			return http.StatusPreconditionFailed, nil
		}
		if sub.pending != nil {
			if sub.pending.sid == sid && stringMapsEqual(sub.pending.vars, vars) {
				// Identical retransmission: acknowledge, but do not deliver
				// anything a second time.
				return http.StatusOK, nil
			}
			// A different initial event contradicts the one already held.
			return http.StatusPreconditionFailed, nil
		}
		sub.pending = &pendingEvent{sid: sid, vars: vars}
		return http.StatusOK, nil
	}

	if !sub.hasLast {
		if seq != 0 {
			return http.StatusPreconditionFailed, nil
		}
		return http.StatusOK, sub.commitEvent(seq, vars)
	}

	switch {
	case seq == sub.lastSeq:
		if stringMapsEqual(sub.lastVars, vars) {
			// Exact duplicate of the previous event. Acknowledge but do not
			// invoke the handler again.
			return http.StatusOK, nil
		}
		return http.StatusPreconditionFailed, nil
	case seq == nextEventSeq(sub.lastSeq):
		return http.StatusOK, sub.commitEvent(seq, vars)
	default:
		// A skipped, stale, or otherwise out-of-order event. State is left
		// unchanged so the correct successor can still be accepted.
		return http.StatusPreconditionFailed, nil
	}
}

// Snapshot returns an independent copy of all variables observed so far.
func (sub *EventSubscription) Snapshot() map[string]string {
	sub.mu.Lock()
	defer sub.mu.Unlock()
	return copyStringMap(sub.values)
}

// State is an alias for Snapshot.
func (sub *EventSubscription) State() map[string]string {
	return sub.Snapshot()
}

// SID returns the SID confirmed by Start, or the empty string before Start
// has completed.
func (sub *EventSubscription) SID() string {
	sub.mu.Lock()
	defer sub.mu.Unlock()
	return sub.sid
}

// Timeout returns the subscription timeout confirmed by Start. Zero means
// "infinite".
func (sub *EventSubscription) Timeout() time.Duration {
	sub.mu.Lock()
	defer sub.mu.Unlock()
	return sub.timeout
}

// commitEvent records a confirmed event and returns its delivery. The caller
// must hold sub.mu.
func (sub *EventSubscription) commitEvent(seq uint32, vars map[string]string) *eventDelivery {
	if sub.values == nil {
		sub.values = make(map[string]string)
	}
	for name, value := range vars {
		sub.values[name] = value
	}
	sub.lastSeq = seq
	sub.hasLast = true
	sub.lastVars = copyStringMap(vars)

	return &eventDelivery{
		changed:  copyStringMap(vars),
		snapshot: copyStringMap(sub.values),
	}
}

// enqueue ensures the delivery worker exists and queues a delivery. The
// caller must hold commitMu.
func (sub *EventSubscription) enqueue(delivery eventDelivery) {
	sub.deliveryOnce.Do(func() {
		if sub.deliveryCh == nil {
			sub.deliveryCh = make(chan eventDelivery)
		}
		go sub.deliveryLoop(sub.Handler)
	})
	sub.deliveryCh <- delivery
}

// deliveryLoop invokes the handler serially. It holds no subscription state
// lock, so handlers may call Snapshot.
func (sub *EventSubscription) deliveryLoop(handler EventStateHandler) {
	for delivery := range sub.deliveryCh {
		if handler != nil {
			handler(delivery.changed, delivery.snapshot)
		}
	}
}

func nextEventSeq(seq uint32) uint32 {
	if seq == math.MaxUint32 {
		// SEQ wraps to 1; zero is only ever valid as the initial event.
		return 1
	}
	return seq + 1
}

func parseEventSeq(header string) (uint32, error) {
	s := strings.TrimSpace(header)
	if s == "" {
		return 0, errors.New("missing SEQ header")
	}
	v, err := strconv.ParseUint(s, 10, 32)
	if err != nil {
		return 0, err
	}
	return uint32(v), nil
}

func parseEventTimeout(header string) (time.Duration, error) {
	s := strings.TrimSpace(header)
	if s == "" {
		return 0, errors.New("missing TIMEOUT header")
	}
	if strings.EqualFold(s, "infinite") {
		return 0, nil
	}
	const prefix = "second-"
	if len(s) < len(prefix) || !strings.EqualFold(s[:len(prefix)], prefix) {
		return 0, fmt.Errorf("unrecognised TIMEOUT value %q", header)
	}
	seconds, err := strconv.ParseUint(s[len(prefix):], 10, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid TIMEOUT value %q: %w", header, err)
	}
	if seconds > uint64(math.MaxInt64)/uint64(time.Second) {
		return 0, fmt.Errorf("TIMEOUT value %q is too large", header)
	}
	return time.Duration(seconds) * time.Second, nil
}

// parsePropertySet parses a UPnP event propertyset. It rejects the entire
// document if any part is malformed, so a caller never observes a partially
// published event.
func parsePropertySet(r io.Reader) (map[string]string, error) {
	dec := xml.NewDecoder(r)
	root, err := findPropertySetRoot(dec)
	if err != nil {
		return nil, err
	}

	vars := make(map[string]string)
	for {
		tok, err := dec.Token()
		if err != nil {
			return nil, err
		}
		switch t := tok.(type) {
		case xml.StartElement:
			if t.Name.Space != EventXMLNamespace || t.Name.Local != "property" {
				return nil, fmt.Errorf("unexpected element %s in propertyset", elementName(t.Name))
			}
			name, value, err := parseProperty(dec, t)
			if err != nil {
				return nil, err
			}
			if _, duplicate := vars[name]; duplicate {
				return nil, fmt.Errorf("duplicate property %q", name)
			}
			vars[name] = value
		case xml.EndElement:
			if t.Name != root.Name {
				return nil, fmt.Errorf("unexpected end element %s", elementName(t.Name))
			}
			if err := expectDocumentEnd(dec); err != nil {
				return nil, err
			}
			return vars, nil
		case xml.CharData:
			if len(bytes.TrimSpace(t)) != 0 {
				return nil, errors.New("unexpected text in propertyset")
			}
		case xml.Comment, xml.ProcInst, xml.Directive:
			// Ignore markup which carries no event data.
		}
	}
}

func findPropertySetRoot(dec *xml.Decoder) (xml.StartElement, error) {
	for {
		tok, err := dec.Token()
		if err != nil {
			return xml.StartElement{}, err
		}
		switch t := tok.(type) {
		case xml.StartElement:
			if t.Name.Space != EventXMLNamespace || t.Name.Local != "propertyset" {
				return xml.StartElement{}, fmt.Errorf("unexpected root element %s", elementName(t.Name))
			}
			return t, nil
		case xml.CharData:
			if len(bytes.TrimSpace(t)) != 0 {
				return xml.StartElement{}, errors.New("unexpected text before propertyset")
			}
		case xml.Comment, xml.ProcInst, xml.Directive:
			// Prolog markup.
		}
	}
}

func parseProperty(dec *xml.Decoder, property xml.StartElement) (name, value string, err error) {
	const (
		beforeVariable = iota
		inVariable
		afterVariable
	)
	state := beforeVariable
	var varName xml.Name
	var text strings.Builder

	for {
		tok, err := dec.Token()
		if err != nil {
			return "", "", err
		}
		switch t := tok.(type) {
		case xml.StartElement:
			if state != beforeVariable {
				return "", "", fmt.Errorf("nested element %s in property value", elementName(t.Name))
			}
			state = inVariable
			varName = t.Name
			name = t.Name.Local
		case xml.CharData:
			switch state {
			case beforeVariable:
				if len(bytes.TrimSpace(t)) != 0 {
					return "", "", errors.New("unexpected text before property variable")
				}
			case inVariable:
				text.Write([]byte(t))
			case afterVariable:
				if len(bytes.TrimSpace(t)) != 0 {
					return "", "", errors.New("unexpected text after property variable")
				}
			}
		case xml.EndElement:
			switch state {
			case beforeVariable:
				return "", "", errors.New("property contains no variable")
			case inVariable:
				if t.Name != varName {
					return "", "", fmt.Errorf("unexpected end element %s", elementName(t.Name))
				}
				state = afterVariable
			case afterVariable:
				if t.Name != property.Name {
					return "", "", fmt.Errorf("unexpected end element %s", elementName(t.Name))
				}
				return name, text.String(), nil
			}
		case xml.Comment, xml.ProcInst, xml.Directive:
			// Ignore.
		}
	}
}

func expectDocumentEnd(dec *xml.Decoder) error {
	for {
		tok, err := dec.Token()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		switch t := tok.(type) {
		case xml.CharData:
			if len(bytes.TrimSpace(t)) != 0 {
				return errors.New("unexpected text after propertyset")
			}
		case xml.Comment, xml.ProcInst, xml.Directive:
			// Trailing markup is allowed.
		default:
			return errors.New("unexpected element after propertyset")
		}
	}
}

func elementName(name xml.Name) string {
	if name.Space == "" {
		return name.Local
	}
	return name.Space + ":" + name.Local
}

func copyStringMap(src map[string]string) map[string]string {
	dst := make(map[string]string, len(src))
	for name, value := range src {
		dst[name] = value
	}
	return dst
}

func stringMapsEqual(a, b map[string]string) bool {
	if len(a) != len(b) {
		return false
	}
	for name, value := range a {
		other, ok := b[name]
		if !ok || other != value {
			return false
		}
	}
	return true
}
