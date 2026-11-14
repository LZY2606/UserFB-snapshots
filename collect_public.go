package mastodon

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sort"
	"strings"
	"sync"
)

// ErrCollectOverflow is returned by [Client.CollectPublic] when the bounded
// queue holding streaming events that have not been delivered yet is full.
// Callers can test for it with errors.Is.
var ErrCollectOverflow = errors.New("mastodon: collect public event queue overflow")

// CollectPublic collects the public timeline and then keeps delivering live
// public events until the context is cancelled or the stream ends.
//
// A streaming connection is opened first.  Only after the server returned
// HTTP 200 is the history requested, so events that happen while the history
// is being downloaded are buffered and delivered after the history.  The
// history is requested through /api/v1/timelines/public (or the local
// variant), following the server's next.max_id links.
//
// A non-empty since restricts both history and regular "update" stream
// events to statuses whose numeric ID is strictly greater than since.
// "status.update" (edit) and "delete" events are never filtered or
// deduplicated.
//
// emit is called synchronously and serially.  It must not retain the event
// after returning; the collector does not use the event afterwards.  At most
// capacity live events may wait for delivery.  If the queue is full,
// ErrCollectOverflow is returned instead of dropping an event.
//
// Cancellation, pagination errors, stream read errors, overflow and emit
// errors close the streaming response and stop the background reader.  An
// error while the history is still being prepared means that no history is
// delivered.  Once delivery has started, events already delivered are not
// retracted.
func (c *Client) CollectPublic(ctx context.Context, isLocal bool, since ID, capacity int, emit func(Event) error) error {
	if emit == nil {
		return errors.New("mastodon: CollectPublic requires a non-nil emit function")
	}
	if capacity < 1 {
		return fmt.Errorf("mastodon: CollectPublic capacity must be at least 1, got %d", capacity)
	}
	if since != "" {
		if err := validateCollectID(string(since)); err != nil {
			return fmt.Errorf("mastodon: invalid since ID: %w", err)
		}
	}

	u, err := c.collectStreamingURL(isLocal)
	if err != nil {
		return err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return err
	}
	if c.Config.AccessToken != "" {
		req.Header.Set("Authorization", "Bearer "+c.Config.AccessToken)
	}
	if c.UserAgent != "" {
		req.Header.Set("User-Agent", c.UserAgent)
	}

	resp, err := c.Do(req)
	if err != nil {
		return err
	}
	if resp.StatusCode != http.StatusOK {
		err = parseAPIError("bad request", resp)
		resp.Body.Close()
		return err
	}

	runCtx, cancel := context.WithCancel(ctx)
	queue := make(chan Event, capacity)
	term := newCollectTerminal()

	defer func() {
		// Stop the background reader and close this collection's response.
		cancel()
		resp.Body.Close()
		<-term.done
	}()

	go func() {
		err := collectReadStream(runCtx, resp.Body, since, queue)
		term.finish(err)
		// A terminal stream event makes any in-flight history request
		// pointless, so abort it as well.
		cancel()
	}()

	history, err := c.collectHistory(runCtx, isLocal, since, term)
	if err != nil {
		return err
	}
	// If the stream ended (including a normal EOF) while the history was
	// still being prepared, the collection failed and no history is
	// delivered.
	if err := term.result(); err != nil {
		return err
	}

	delivered := make(map[string]struct{}, len(history))
	if err := collectEmitHistory(ctx, term, history, delivered, emit); err != nil {
		return err
	}

	return collectRealtime(ctx, term, queue, delivered, emit)
}

func (c *Client) collectStreamingURL(isLocal bool) (*url.URL, error) {
	u, err := url.Parse(c.Config.Server)
	if err != nil {
		return nil, err
	}
	p := "/api/v1/streaming/public"
	if isLocal {
		p += "/local"
	}
	return u.JoinPath(p), nil
}

// collectTerminal records the one error that ended the streaming reader.  A
// normal end of stream is recorded as io.EOF.
type collectTerminal struct {
	once sync.Once
	mu   sync.Mutex
	err  error
	done chan struct{}
}

func newCollectTerminal() *collectTerminal {
	return &collectTerminal{done: make(chan struct{})}
}

func (t *collectTerminal) finish(err error) {
	if err == nil {
		err = io.EOF
	}
	t.once.Do(func() {
		t.mu.Lock()
		t.err = err
		t.mu.Unlock()
		close(t.done)
	})
}

func (t *collectTerminal) result() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.err
}

// collectHistory downloads every history page and returns the statuses in
// ascending numeric ID order.  Nothing is emitted here.
func (c *Client) collectHistory(ctx context.Context, isLocal bool, since ID, term *collectTerminal) ([]*Status, error) {
	var collected []*Status
	seen := make(map[string]struct{})
	var upper, prevMax ID
	first := true
	cursor := ID("")

	for {
		if err := collectCtxErr(ctx, term); err != nil {
			return nil, err
		}

		// Every history request carries the original since and local
		// options.  The cursor is the next.max_id from the previous Link.
		pg := &Pagination{SinceID: since, MaxID: cursor}
		statuses, err := c.GetTimelinePublic(ctx, isLocal, pg)
		if err != nil {
			if cerr := collectCtxErr(ctx, term); cerr != nil {
				return nil, cerr
			}
			return nil, err
		}

		if len(statuses) == 0 {
			// An empty page ends the history.
			break
		}

		for _, st := range statuses {
			if st == nil {
				return nil, errors.New("mastodon: public timeline returned a null status")
			}
			if err := validateCollectID(string(st.ID)); err != nil {
				return nil, fmt.Errorf("mastodon: invalid status ID %q: %w", st.ID, err)
			}
		}

		if first {
			upper = statuses[0].ID
			for _, st := range statuses[1:] {
				if compareCollectID(string(st.ID), string(upper)) > 0 {
					upper = st.ID
				}
			}
		}

		for _, st := range statuses {
			id := string(st.ID)
			if since != "" && compareCollectID(id, string(since)) <= 0 {
				continue
			}
			if !first && upper != "" && compareCollectID(id, string(upper)) > 0 {
				// Entries newer than this round's first page belong to the
				// overlap with the live stream or to a later round.
				continue
			}
			key := canonicalCollectID(id)
			if _, ok := seen[key]; ok {
				continue
			}
			seen[key] = struct{}{}
			collected = append(collected, st)
		}
		first = false

		// Only the max_id of the Link is used; its host and path are
		// intentionally ignored and the configured Server is used instead.
		next := pg.MaxID
		if next == "" {
			break
		}
		if err := validateCollectID(string(next)); err != nil {
			return nil, fmt.Errorf("mastodon: invalid pagination max_id %q: %w", next, err)
		}
		if prevMax != "" && compareCollectID(string(next), string(prevMax)) >= 0 {
			return nil, fmt.Errorf("mastodon: pagination max_id %q did not decrease from %q", next, prevMax)
		}
		prevMax = next
		cursor = next
	}

	if err := collectCtxErr(ctx, term); err != nil {
		return nil, err
	}

	sort.Slice(collected, func(i, j int) bool {
		return compareCollectID(string(collected[i].ID), string(collected[j].ID)) < 0
	})
	return collected, nil
}

func collectEmitHistory(ctx context.Context, term *collectTerminal, statuses []*Status, delivered map[string]struct{}, emit func(Event) error) error {
	for _, st := range statuses {
		// The history is fully prepared at this point, so a stream that
		// has ended normally must not truncate it.  Only caller
		// cancellation or a hard stream failure stops new callbacks.
		if err := ctx.Err(); err != nil {
			return err
		}
		if terr := term.result(); terr != nil && !errors.Is(terr, io.EOF) {
			return terr
		}
		// Record the ID before calling emit.  After emit returns the event
		// must not be used again.
		delivered[canonicalCollectID(string(st.ID))] = struct{}{}
		if err := emit(&UpdateEvent{Status: st}); err != nil {
			return err
		}
	}
	return nil
}

func collectRealtime(ctx context.Context, term *collectTerminal, queue <-chan Event, delivered map[string]struct{}, emit func(Event) error) error {
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case ev := <-queue:
			if err := ctx.Err(); err != nil {
				return err
			}
			if terr := term.result(); terr != nil {
				if errors.Is(terr, io.EOF) {
					if err := collectDeliver(ev, delivered, emit); err != nil {
						return err
					}
					return collectDrain(ctx, queue, delivered, emit)
				}
				return terr
			}
			if err := collectDeliver(ev, delivered, emit); err != nil {
				return err
			}
		case <-term.done:
			terr := term.result()
			if errors.Is(terr, io.EOF) {
				return collectDrain(ctx, queue, delivered, emit)
			}
			return terr
		}
	}
}

// collectDrain delivers events that were already read before a normal end of
// stream.  No new callbacks are started once the context is cancelled.
func collectDrain(ctx context.Context, queue <-chan Event, delivered map[string]struct{}, emit func(Event) error) error {
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case ev := <-queue:
			if err := collectDeliver(ev, delivered, emit); err != nil {
				return err
			}
		default:
			return io.EOF
		}
	}
}

func collectDeliver(ev Event, delivered map[string]struct{}, emit func(Event) error) error {
	if ue, ok := ev.(*UpdateEvent); ok && ue.Status != nil {
		key := canonicalCollectID(string(ue.Status.ID))
		if _, ok := delivered[key]; ok {
			return nil
		}
		delivered[key] = struct{}{}
	}
	return emit(ev)
}

// collectCtxErr reports a terminal stream result first (an overflow or read
// failure must not be masked by the cancellation the reader performs), then
// caller cancellation.
func collectCtxErr(ctx context.Context, term *collectTerminal) error {
	if err := term.result(); err != nil {
		return err
	}
	return ctx.Err()
}

// collectReadStream parses the single-line SSE format used by Mastodon and
// pushes live events into queue without ever blocking.
func collectReadStream(ctx context.Context, r io.Reader, since ID, queue chan Event) error {
	br := bufio.NewReader(r)
	var name string
	var lineBuf bytes.Buffer

	for {
		line, isPrefix, err := br.ReadLine()
		if err != nil {
			if cerr := ctx.Err(); cerr != nil {
				return cerr
			}
			if errors.Is(err, io.EOF) {
				return io.EOF
			}
			return err
		}
		if isPrefix {
			lineBuf.Write(line)
			continue
		}
		if lineBuf.Len() > 0 {
			lineBuf.Write(line)
			line = lineBuf.Bytes()
			lineBuf.Reset()
		}

		token := strings.SplitN(string(line), ":", 2)
		if len(token) != 2 {
			continue
		}
		switch strings.TrimSpace(token[0]) {
		case "event":
			name = strings.TrimSpace(token[1])
		case "data":
			ev, err := collectDecodeEvent(name, token[1], since)
			if err != nil {
				return err
			}
			if ev == nil {
				continue
			}
			if err := collectEnqueue(ctx, queue, ev); err != nil {
				return err
			}
		}
	}
}

func collectDecodeEvent(name, data string, since ID) (Event, error) {
	switch name {
	case "update":
		var st Status
		if err := json.Unmarshal([]byte(data), &st); err != nil {
			return nil, fmt.Errorf("mastodon: could not decode update event: %w", err)
		}
		if err := validateCollectID(string(st.ID)); err != nil {
			return nil, fmt.Errorf("mastodon: invalid update event ID %q: %w", st.ID, err)
		}
		if since != "" && compareCollectID(string(st.ID), string(since)) <= 0 {
			return nil, nil
		}
		return &UpdateEvent{Status: &st}, nil
	case "status.update":
		var st Status
		if err := json.Unmarshal([]byte(data), &st); err != nil {
			return nil, fmt.Errorf("mastodon: could not decode status.update event: %w", err)
		}
		if err := validateCollectID(string(st.ID)); err != nil {
			return nil, fmt.Errorf("mastodon: invalid status.update event ID %q: %w", st.ID, err)
		}
		return &UpdateEditEvent{Status: &st}, nil
	case "delete":
		id := ID(strings.TrimSpace(data))
		if err := validateCollectID(string(id)); err != nil {
			return nil, fmt.Errorf("mastodon: invalid delete event ID %q: %w", id, err)
		}
		return &DeleteEvent{ID: id}, nil
	default:
		// Comments and unrelated event types are ignored.
		return nil, nil
	}
}

func collectEnqueue(ctx context.Context, queue chan<- Event, ev Event) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	select {
	case queue <- ev:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	default:
		if err := ctx.Err(); err != nil {
			return err
		}
		return ErrCollectOverflow
	}
}

// validateCollectID checks that s is a non-negative decimal ID.  The ID may
// be arbitrarily large; it is never converted to a fixed-size integer.
func validateCollectID(s string) error {
	if s == "" {
		return errors.New("empty ID")
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return fmt.Errorf("%q is not a non-negative decimal ID", s)
		}
	}
	return nil
}

// canonicalCollectID returns a form suitable for comparing and deduplicating
// decimal IDs.  Leading zeroes do not change the numeric value.
func canonicalCollectID(s string) string {
	i := 0
	for i < len(s)-1 && s[i] == '0' {
		i++
	}
	return s[i:]
}

// compareCollectID compares two validated, non-negative decimal IDs
// numerically.  It returns -1, 0, or +1.
func compareCollectID(a, b string) int {
	a = canonicalCollectID(a)
	b = canonicalCollectID(b)
	if len(a) != len(b) {
		if len(a) < len(b) {
			return -1
		}
		return 1
	}
	return strings.Compare(a, b)
}
