// Copyright 2011 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// File contains automatic referral chasing for Search operations.
//
// When LDAPConnection.ReferralPolicy is set, Search and SearchWithHandler
// follow SearchResultReference URIs automatically: the URIs inside one
// reference are alternative servers for the same branch and are tried in
// order, while distinct references are independent branches that all have
// to complete. Results are buffered and only delivered to the caller's
// handler once the whole selected graph succeeded. When ReferralPolicy is
// nil the legacy behaviour is kept and references are delivered one by one.
package ldap

import (
	"errors"
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"
)

// ErrReferralLoop is returned when every URI of a SearchResultReference
// resolves to a query that is already active in the current referral chain.
var ErrReferralLoop = errors.New("ldap: referral loop detected")

// ReferralPolicy enables automatic referral chasing when set on an
// LDAPConnection. The policy is captured once per Search/SearchWithHandler
// call and does not change for the duration of that call.
type ReferralPolicy struct {
	// Dial builds a ready to use connection for one referral URL,
	// including any TLS the scheme requires. The returned connection is
	// owned by the calling search and is Closed exactly once when the
	// call finishes. No state (e.g. binds) is copied from the connection
	// that received the reference.
	Dial func(*url.URL) (*LDAPConnection, error)
}

// referralQueryKey identifies one executed query: the normalized endpoint
// plus every decoded query parameter. Hostnames are case-insensitive and
// default ports are equivalent to explicit ones.
type referralQueryKey struct {
	scheme string
	host   string
	port   int
	baseDN string
	scope  int
	filter string
	attrs  string
}

func referralKeyFor(scheme, host string, port int, req *SearchRequest) referralQueryKey {
	return referralQueryKey{
		scheme: scheme,
		host:   strings.ToLower(host),
		port:   port,
		baseDN: req.BaseDN,
		scope:  req.Scope,
		filter: req.Filter,
		attrs:  strings.Join(req.Attributes, "\x1f"),
	}
}

func referralDefaultPort(scheme string) int {
	if scheme == "ldaps" {
		return 636
	}
	return 389
}

// referralEndpoint derives the normalized endpoint of a connection. A
// STARTTLS upgraded connection still counts as a plain ldap endpoint.
func (l *LDAPConnection) referralEndpoint() (scheme, host string, port int) {
	scheme = "ldap"
	if l.IsSSL {
		scheme = "ldaps"
	}
	host, portText, err := net.SplitHostPort(l.Addr)
	if err != nil {
		return scheme, l.Addr, referralDefaultPort(scheme)
	}
	port, err = strconv.Atoi(portText)
	if err != nil {
		port = referralDefaultPort(scheme)
	}
	return scheme, host, port
}

// referralRequestFromURL parses one referral URI per RFC 4516 and builds the
// effective SearchRequest. The DN must be explicitly present; attributes,
// scope and filter are inherited from the parent query when missing or
// empty, explicit values override. Each field is percent-decoded exactly
// once ('+' is never turned into a space). All other search parameters are
// kept from the parent query.
func referralRequestFromURL(rawURL string, parent *SearchRequest) (child *SearchRequest, key referralQueryKey, u *url.URL, err error) {
	u, err = url.Parse(rawURL)
	if err != nil {
		return nil, referralQueryKey{}, nil, err
	}
	scheme := strings.ToLower(u.Scheme)
	if scheme != "ldap" && scheme != "ldaps" {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: unsupported referral scheme %q", u.Scheme)
	}
	if u.User != nil {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q contains userinfo", rawURL)
	}
	if u.Fragment != "" {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q contains a fragment", rawURL)
	}
	host := u.Hostname()
	if host == "" {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has no host", rawURL)
	}
	port := referralDefaultPort(scheme)
	if portText := u.Port(); portText != "" {
		port, err = strconv.Atoi(portText)
		if err != nil {
			return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has an invalid port: %v", rawURL, err)
		}
	}
	rawPath := u.EscapedPath()
	if !strings.HasPrefix(rawPath, "/") {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has no DN", rawURL)
	}
	baseDN, err := url.PathUnescape(rawPath[1:])
	if err != nil {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has an invalid DN: %v", rawURL, err)
	}
	if baseDN == "" {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has an empty DN", rawURL)
	}

	// RFC 4516: attributes ? scope ? filter ? extensions
	fields := strings.Split(u.RawQuery, "?")
	if len(fields) > 3 && strings.Join(fields[3:], "?") != "" {
		return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q contains extensions", rawURL)
	}

	attributes := parent.Attributes
	if fields[0] != "" {
		attributes = nil
		for _, attr := range strings.Split(fields[0], ",") {
			decoded, decodeErr := url.PathUnescape(attr)
			if decodeErr != nil {
				return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has an invalid attribute: %v", rawURL, decodeErr)
			}
			attributes = append(attributes, decoded)
		}
	}

	scope := parent.Scope
	if len(fields) > 1 && fields[1] != "" {
		switch strings.ToLower(fields[1]) {
		case "base":
			scope = ScopeBaseObject
		case "one":
			scope = ScopeSingleLevel
		case "sub":
			scope = ScopeWholeSubtree
		default:
			return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has an invalid scope %q", rawURL, fields[1])
		}
	}

	filter := parent.Filter
	if len(fields) > 2 && fields[2] != "" {
		filter, err = url.PathUnescape(fields[2])
		if err != nil {
			return nil, referralQueryKey{}, nil, fmt.Errorf("ldap: referral URL %q has an invalid filter: %v", rawURL, err)
		}
	}

	child = &SearchRequest{
		BaseDN:       baseDN,
		Scope:        scope,
		DerefAliases: parent.DerefAliases,
		SizeLimit:    parent.SizeLimit,
		TimeLimit:    parent.TimeLimit,
		TypesOnly:    parent.TypesOnly,
		Filter:       filter,
		Attributes:   attributes,
	}
	return child, referralKeyFor(scheme, host, port, child), u, nil
}

// referralDelivery is one buffered result plus its real origin.
type referralDelivery struct {
	result *DiscreteSearchResult
	info   *ConnectionInfo
}

// referralOutcome is the collected raw result of one executed query.
type referralOutcome struct {
	entries    []referralDelivery
	references [][]string
	controls   []Control
	doneInfo   *ConnectionInfo
}

// executeReferralQuery runs one SearchRequest on conn and collects entries,
// references and the final Done without involving the public handler.
func executeReferralQuery(conn *LDAPConnection, request *SearchRequest) (*referralOutcome, error) {
	messageID, ok := conn.nextMessageID()
	if !ok {
		return nil, NewLDAPError(ErrorClosing, "MessageID channel is closed.")
	}

	searchPacket, err := encodeSearchRequest(request)
	if err != nil {
		return nil, err
	}

	packet, err := requestBuildPacket(messageID, searchPacket, nil)
	if err != nil {
		return nil, err
	}

	channel, err := conn.sendMessage(packet)
	if err != nil {
		return nil, err
	}
	if channel == nil {
		return nil, NewLDAPError(ErrorNetwork, "Could not send message")
	}
	defer conn.finishMessage(messageID)

	connectionInfo := &ConnectionInfo{Conn: conn, MessageID: messageID}
	outcome := &referralOutcome{}

	for {
		packet, ok = <-channel
		if !ok {
			return nil, NewLDAPError(ErrorClosing, "Response Channel Closed")
		}
		if packet == nil {
			return nil, NewLDAPError(ErrorNetwork, "Could not retrieve message")
		}

		discreteSearchResult, err := decodeSearchResponse(packet)
		if err != nil {
			return nil, err
		}

		switch discreteSearchResult.SearchResultType {
		case SearchResultEntry:
			outcome.entries = append(outcome.entries, referralDelivery{discreteSearchResult, connectionInfo})
		case SearchResultReference:
			outcome.references = append(outcome.references, discreteSearchResult.Referrals)
		case SearchResultDone:
			outcome.controls = discreteSearchResult.Controls
			outcome.doneInfo = connectionInfo
			return outcome, nil
		}
	}
}

// referralSession holds the state of one referral chasing search call.
type referralSession struct {
	policy    *ReferralPolicy
	active    map[referralQueryKey]bool
	completed map[referralQueryKey]bool
	owned     []*LDAPConnection
}

// closeOwned closes every dialed connection exactly once.
func (s *referralSession) closeOwned() {
	seen := make(map[*LDAPConnection]bool, len(s.owned))
	for _, conn := range s.owned {
		if conn != nil && !seen[conn] {
			seen[conn] = true
			conn.Close()
		}
	}
	s.owned = nil
}

// chaseReferralQuery executes request on conn and resolves every reference
// branch depth-first in order. On success the buffered deliveries of the
// whole subgraph (own entries first, then the selected subgraphs) are
// returned; on any failure the buffered results are discarded by the caller.
func (s *referralSession) chaseReferralQuery(conn *LDAPConnection, request *SearchRequest, key referralQueryKey) ([]referralDelivery, error) {
	outcome, err := executeReferralQuery(conn, request)
	if err != nil {
		return nil, err
	}
	deliveries := append([]referralDelivery(nil), outcome.entries...)
	for _, group := range outcome.references {
		sub, err := s.resolveReferralGroup(group, request)
		if err != nil {
			return nil, err
		}
		deliveries = append(deliveries, sub...)
	}
	return deliveries, nil
}

// resolveReferralGroup tries the alternative URIs of one reference in
// received order. The first candidate whose own query and all of its
// successors succeed is selected; remaining alternatives are not tried.
func (s *referralSession) resolveReferralGroup(urls []string, parent *SearchRequest) ([]referralDelivery, error) {
	var firstErr error
	loops := 0
	for _, rawURL := range urls {
		childRequest, key, parsedURL, err := referralRequestFromURL(rawURL, parent)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			continue
		}
		if s.completed[key] {
			// This exact query already fully succeeded earlier in the
			// call; its deliveries were merged at the first occurrence.
			return nil, nil
		}
		if s.active[key] {
			// Same query already active in the call chain: a loop.
			loops++
			continue
		}
		conn, err := s.policy.Dial(parsedURL)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			continue
		}
		s.owned = append(s.owned, conn)
		s.active[key] = true
		deliveries, err := s.chaseReferralQuery(conn, childRequest, key)
		delete(s.active, key)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			continue
		}
		s.completed[key] = true
		return deliveries, nil
	}
	if firstErr != nil {
		return nil, firstErr
	}
	if loops > 0 {
		return nil, ErrReferralLoop
	}
	return nil, NewLDAPError(ErrorNetwork, "ldap: referral contains no usable URI")
}

// searchFollowingReferrals implements SearchWithHandler with automatic
// referral chasing. Results are buffered and only delivered to
// resultHandler once the root query and every selected branch succeeded:
// root entries first, then the selected subgraphs depth-first in reference
// order, and finally the root Done (with its Controls).
func searchFollowingReferrals(
	l *LDAPConnection, policy *ReferralPolicy,
	searchRequest *SearchRequest, resultHandler SearchResultHandler, errorChan chan<- error,
) error {
	if len(searchRequest.Controls) > 0 {
		err := NewLDAPError(ErrorInvalidArgument, "Referral following does not support SearchRequest Controls")
		return sendError(errorChan, err)
	}

	// Work on a copy so the caller's request and attribute list are
	// never modified.
	requestCopy := *searchRequest
	requestCopy.Attributes = append([]string(nil), searchRequest.Attributes...)
	requestCopy.Controls = nil
	searchRequest = &requestCopy

	session := &referralSession{
		policy:    policy,
		active:    map[referralQueryKey]bool{},
		completed: map[referralQueryKey]bool{},
	}
	defer session.closeOwned()

	scheme, host, port := l.referralEndpoint()
	rootKey := referralKeyFor(scheme, host, port, searchRequest)
	session.active[rootKey] = true

	rootOutcome, err := executeReferralQuery(l, searchRequest)
	if err != nil {
		return sendError(errorChan, err)
	}

	deliveries := append([]referralDelivery(nil), rootOutcome.entries...)
	for _, group := range rootOutcome.references {
		sub, err := session.resolveReferralGroup(group, searchRequest)
		if err != nil {
			return sendError(errorChan, err)
		}
		deliveries = append(deliveries, sub...)
	}
	delete(session.active, rootKey)
	session.completed[rootKey] = true

	for _, delivery := range deliveries {
		stop, err := resultHandler.ProcessDiscreteResult(delivery.result, delivery.info)
		if err != nil {
			return sendError(errorChan, err)
		}
		if stop {
			return sendError(errorChan, nil)
		}
	}

	done := &DiscreteSearchResult{
		SearchResultType: SearchResultDone,
		Controls:         rootOutcome.controls,
	}
	if _, err := resultHandler.ProcessDiscreteResult(done, rootOutcome.doneInfo); err != nil {
		return sendError(errorChan, err)
	}
	return sendError(errorChan, nil)
}
