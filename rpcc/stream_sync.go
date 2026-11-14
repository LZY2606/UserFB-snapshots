package rpcc

import (
	"errors"
	"fmt"
	"sort"
	"sync"
)

// syncMessageStore writes one message into a
// messageWriter and waits for message.next
// to be called before loading the next.
type syncMessageStore struct {
	mu      sync.Mutex
	writers map[string]streamWriter
	members map[string]*streamClient
	unsubs  map[string]func()
	backlog []*message
	pending bool
	// inflight points at the message written to a writer, if any.
	inflight *message
	closers  []func()
	detached bool
	// managed is set for groups created by SyncPreserve. Streams in a
	// managed group let the group drive Close so that the remaining
	// members can progress; legacy Sync groups keep their original
	// close behavior.
	managed bool
}

func newSyncMessageStore() *syncMessageStore {
	return &syncMessageStore{
		writers: make(map[string]streamWriter),
		members: make(map[string]*streamClient),
		unsubs:  make(map[string]func()),
	}
}

func (s *syncMessageStore) subscribe(method string, w streamWriter, conn *Conn) (unsubscribe func(), err error) {
	s.mu.Lock()
	if _, ok := s.writers[method]; ok {
		s.mu.Unlock()
		return nil, fmt.Errorf("%s already subscribed", method)
	}
	s.mu.Unlock()

	remove, err := conn.listen(method, s)
	if err != nil {
		return nil, err
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	if s.detached {
		remove()
		return nil, errors.New("message store is closed")
	}
	if _, ok := s.writers[method]; ok {
		remove()
		return nil, fmt.Errorf("%s already subscribed", method)
	}

	s.writers[method] = w
	if sc, ok := w.(*streamClient); ok {
		s.members[method] = sc
	}

	var once sync.Once
	var unsub func()
	unsub = func() {
		once.Do(func() {
			remove() // Prevent new messages for writer.

			s.mu.Lock()
			defer s.mu.Unlock()

			delete(s.writers, method)
			delete(s.members, method)
			delete(s.unsubs, method)
			if len(s.writers) == 0 {
				// Either close has been called
				// or all streams have closed.
				s.writers = nil
				s.backlog = nil
				s.closers = nil
			}
		})
	}
	s.unsubs[method] = unsub
	s.closers = append(s.closers, unsub)

	return unsub, nil
}

func (s *syncMessageStore) close() {
	s.mu.Lock()
	closers := s.closers
	s.closers = nil
	s.detached = true
	s.writers = nil
	s.members = nil
	s.unsubs = nil
	s.backlog = nil
	s.pending = false
	s.inflight = nil
	s.mu.Unlock()

	for _, c := range closers {
		c()
	}
}

// takeBacklog removes and returns all queued messages that have not yet
// been written to a writer. The group is left with no pending message; it
// is meant to be called immediately before the group is closed.
func (s *syncMessageStore) takeBacklog() []*message {
	s.mu.Lock()
	defer s.mu.Unlock()

	msgs := s.backlog
	s.backlog = nil
	// The in-flight message lives in the member stream's buffer and is
	// collected from there, so that it is not duplicated.
	s.pending = false
	s.inflight = nil
	return msgs
}

// liveMembers returns the streams that are currently members of the
// store. Closed streams have already been removed.
func (s *syncMessageStore) liveMembers() []*streamClient {
	s.mu.Lock()
	defer s.mu.Unlock()

	members := make([]*streamClient, 0, len(s.members))
	for _, sc := range s.members {
		members = append(members, sc)
	}
	return members
}

// removeStream detaches a closed stream from the store. Messages queued
// for the stream are discarded and, if the stream was holding up
// delivery, the next live member is allowed to progress.
func (s *syncMessageStore) removeStream(sc *streamClient) {
	s.mu.Lock()
	unsub := s.unsubs[sc.method]
	delete(s.writers, sc.method)
	delete(s.members, sc.method)
	delete(s.unsubs, sc.method)

	if len(s.backlog) > 0 {
		backlog := s.backlog[:0]
		for _, m := range s.backlog {
			if m.method != sc.method {
				backlog = append(backlog, m)
			}
		}
		for i := len(backlog); i < len(s.backlog); i++ {
			s.backlog[i] = nil
		}
		s.backlog = backlog
	}

	if s.pending && s.inflight != nil && s.inflight.method == sc.method {
		// Discard the message that was written to the closed stream
		// instead of acknowledging it, then progress to the next
		// member. Calling next would advance the store twice.
		s.inflight = nil
		s.pending = false
		s.loadLocked()
	}
	s.mu.Unlock()

	if unsub != nil {
		unsub() // Prevent new messages for the closed member.
	}

	// Unconsumed messages of the closed stream are removed.
	sc.discardBuffered()
}

// write implements messageWriter, the message is stored
// in a messageWriter if there are no pending messages,
// otherwise appended to backlog.
func (s *syncMessageStore) write(m message) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.detached {
		return
	}

	if _, ok := s.writers[m.method]; !ok {
		// Exit early if writer has already
		// unsubscribed or store is closed.
		return
	}

	m.next = s.load
	if s.pending {
		s.backlog = append(s.backlog, &m)
		return
	}

	s.deliverLocked(&m)
}

// deliverLocked writes m to the writer responsible for its method and
// marks delivery as pending. The caller must hold s.mu.
func (s *syncMessageStore) deliverLocked(m *message) {
	s.pending = true
	s.inflight = m
	s.writers[m.method].write(*m)
}

// load writes the next message into a messageWriter,
// resets pending status if backlog is empty.
func (s *syncMessageStore) load() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.loadLocked()
}

// loadLocked is like load, but the caller must hold s.mu.
func (s *syncMessageStore) loadLocked() {
	if s.detached {
		return
	}

	s.inflight = nil

	// Keep going until we can write the next message
	// or the backlog is empty.
	for {
		if len(s.backlog) == 0 {
			s.pending = false
			return
		}

		m := s.backlog[0]
		s.backlog[0] = nil // Remove reference from underlying array.
		s.backlog = s.backlog[1:]

		// Check if the writer has already unsubscribed.
		if _, ok := s.writers[m.method]; ok {
			s.deliverLocked(m)
			return
		}
	}
}

// Sync takes two or more streams and sets them into synchronous operation,
// relative to each other. This operation cannot be undone. If an error is
// returned this function is no-op and the streams will continue in asynchronous
// operation.
//
// All streams must belong to the same Conn and they must not be closed. Passing
// multiple streams of the same method to Sync is not supported and will return
// an error.
//
// A stream that is closed is removed and has no further affect on the streams
// that were synchronized.
//
// When two streams, A and B, are in sync they will both receive messages in the
// order that they arrived on Conn. If a message for both A and B arrives, in
// that order, it will not be possible to receive the message from B before the
// message from A has been received.
//
// Sync clears any buffered messages from the passed streams. Use
// SyncPreserve to keep and merge such messages instead.
func Sync(s ...Stream) (err error) {
	if len(s) < 2 {
		return errors.New("rpcc: Sync: two or more streams must be provided")
	}

	set := make(map[Stream]struct{})
	for _, ss := range s {
		if _, ok := set[ss]; ok {
			return errors.New("rpcc: Sync: same instance of stream passed multiple times")
		}
		set[ss] = struct{}{}
	}

	store := newSyncMessageStore()
	defer func() {
		if err != nil {
			store.close()
		}
	}()

	var conn *Conn
	var swap []func()

	for _, ss := range s {
		sc, ok := ss.(*streamClient)
		if !ok {
			return fmt.Errorf("rpcc: Sync: bad Stream type: %T", ss)
		}
		if conn == nil {
			conn = sc.conn
		}
		if sc.conn != conn {
			return errors.New("rpcc: Sync: all Streams must share same Conn")
		}

		// The Stream lock must be held until the
		// swap has been done for all streams.
		sc.mu.Lock()
		defer sc.mu.Unlock()

		if sc.remove == nil {
			return errors.New("rpcc: Sync: Stream is closed")
		}

		// Allow store to manage messages to streamClient.
		unsub, err := store.subscribe(sc.method, sc, sc.conn)
		if err != nil {
			return errors.New("rpcc: Sync: " + err.Error())
		}

		// Delay listener swap until all Streams have been
		// processed so that we can abort on error.
		swap = append(swap, func() {
			sc.remove() // Prevent direct events from Conn.
			sc.remove = func() {
				unsub()         // Remove from store on Close.
				sc.mbuf.clear() // Ensure pending message is processed.
			}
			sc.group = store

			// Clear stream messages to prevent sync issues.
			sc.mbuf.clear()
		})
	}

	// Perform swap, mutex lock (streamClient.mu) is still active.
	for _, s := range swap {
		s()
	}

	return nil
}

// SyncPreserve takes two or more event streams and merges them, together with
// all notifications they have already buffered, into one synchronous group.
// The parameters and return value are the same as for Sync.
//
// Unlike Sync, buffered notifications are not discarded. All unconsumed
// messages from the streams, and from any synchronous group they already
// belong to, are merged and delivered in the order the connection received
// them (not in the order of any protocol timestamp). Notifications that
// arrived before the call, during the call and after it are each delivered
// exactly once.
//
// All live members of a group that any passed stream belongs to must be
// passed as well. Passing only some members of a group returns an error and
// leaves all subscriptions, buffers, readiness and group memberships
// untouched. Two or more existing groups may be merged, and independent
// streams may be added. Streams must share a Conn, must be distinct, must use
// distinct methods and must not be closed.
//
// The caller must not call RecvMsg or Close on the passed streams while
// SyncPreserve is running, and must keep their contexts valid. Network
// reception and other subscriptions continue normally.
//
// Ready keeps its meaning: it is closed only when a call to RecvMsg on that
// stream is non-blocking. A message queued behind a message of another stream
// does not signal Ready early.
//
// Closing a stream in the group discards its unconsumed messages and lets the
// remaining members continue. Other subscribers to the same method and
// ordinary RPC replies are unaffected.
func SyncPreserve(s ...Stream) (err error) {
	if len(s) < 2 {
		return errors.New("rpcc: SyncPreserve: two or more streams must be provided")
	}

	seen := make(map[*streamClient]struct{})
	seenMethod := make(map[string]struct{})
	var conn *Conn
	clients := make([]*streamClient, 0, len(s))
	groups := make(map[*syncMessageStore]struct{})

	for _, ss := range s {
		if ss == nil {
			return errors.New("rpcc: SyncPreserve: nil Stream")
		}
		sc, ok := ss.(*streamClient)
		if !ok || sc == nil {
			return fmt.Errorf("rpcc: SyncPreserve: bad Stream type: %T", ss)
		}
		if _, ok := seen[sc]; ok {
			return errors.New("rpcc: SyncPreserve: same instance of stream passed multiple times")
		}
		seen[sc] = struct{}{}

		if sc.isClosed() {
			return errors.New("rpcc: SyncPreserve: Stream is closed")
		}
		if _, ok := seenMethod[sc.method]; ok {
			return errors.New("rpcc: SyncPreserve: multiple streams with the same method: " + sc.method)
		}
		seenMethod[sc.method] = struct{}{}

		if conn == nil {
			conn = sc.conn
		}
		if sc.conn != conn {
			return errors.New("rpcc: SyncPreserve: all Streams must share same Conn")
		}

		if g := sc.groupStore(); g != nil {
			groups[g] = struct{}{}
		}
		clients = append(clients, sc)
	}

	// Every live member of an existing group must take part in the
	// merge; silently splitting a group would lose ordering guarantees.
	for g := range groups {
		for _, member := range g.liveMembers() {
			if _, ok := seen[member]; !ok {
				return errors.New("rpcc: SyncPreserve: all live members of an existing group must be provided")
			}
		}
	}

	conn.dispatchMu.Lock()
	defer conn.dispatchMu.Unlock()

	// Refuse early if the connection is already gone. Subscriptions below
	// are the only fallible part of the swap; nothing has been modified
	// yet, so an error leaves the original state intact.
	if conn.isClosed() {
		return ErrConnClosing
	}

	store := newSyncMessageStore()
	store.managed = true
	for _, sc := range clients {
		if sc.isClosed() {
			// A stream closed concurrently with the call: do not let it
			// participate in the merged group.
			store.close()
			return errors.New("rpcc: SyncPreserve: Stream is closed")
		}
		if _, err := store.subscribe(sc.method, sc, conn); err != nil {
			store.close()
			return errors.New("rpcc: SyncPreserve: " + err.Error())
		}
	}

	// Past this point the swap cannot fail. Collect everything that has
	// been received but not consumed. Messages are ordered by the sequence
	// assigned on the receiving side, which is the only ordering that
	// reflects the actual arrival on the connection.
	//
	// The message that each group is currently waiting on (if any) has
	// already been written to that group's member stream and is collected
	// from the stream buffer, not from the group backlog.
	var pending []*message
	for g := range groups {
		pending = append(pending, g.takeBacklog()...)
	}
	for _, sc := range clients {
		pending = append(pending, sc.takeBufferedForMerge()...)
	}
	sort.SliceStable(pending, func(i, j int) bool {
		return pending[i].seq < pending[j].seq
	})

	// Detach the old groups. This also removes their listeners from the
	// connection, making the merge boundary explicit. Only streams that are
	// still members of their group need to be re-homed; a stream that was
	// closed concurrently keeps its closed state.
	for g := range groups {
		g.close()
	}
	for _, sc := range clients {
		// Remove the stream's own listener, if it still has one. Streams
		// that were part of a group have already removed it, which is
		// harmless to repeat.
		sc.sc.remove(sc.subSeq)

		sc.mu.Lock()
		if sc.remove != nil {
			sc.group = store
			sc.remove = func() {} // The group manages unsubscribing now.
		}
		sc.mu.Unlock()
	}

	// Replay the buffered messages through the new group, in arrival
	// order. Notifications arriving after the swap enter the same queue
	// with higher sequence numbers, and are therefore delivered after
	// everything that was buffered.
	for _, m := range pending {
		store.write(*m)
	}

	return nil
}
