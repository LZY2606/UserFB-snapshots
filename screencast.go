package godet

import (
	"encoding/base64"
	"encoding/json"
	"log"
	"sync"
)

// screencastFrameEvent is the DevTools protocol event carrying screencast frames.
const screencastFrameEvent = "Page.screencastFrame"

// ScreencastFrame represents a single frame of a screencast.
// Data contains the raw (base64 decoded) image bytes as sent by the browser;
// the image itself is not parsed. Metadata keeps the fields provided by the
// browser as-is.
type ScreencastFrame struct {
	SessionID int    // Screencast session ID (used to acknowledge the frame)
	Data      []byte // Decoded image data
	Metadata  Params // Frame metadata as provided by the browser
}

// screencastState is the state machine of a Screencast stream.
type screencastState int

const (
	screencastStarting screencastState = iota // start command sent, waiting for the reply
	screencastRunning                         // stream is running
	screencastStopping                        // stop command sent, waiting for the reply and pending acks
	screencastStopped                         // stream stopped cleanly (terminal)
	screencastFailed                          // stream failed (terminal)
)

// screencastEarlyFrame is a frame received before the start reply:
// it is acknowledged but only delivered once the stream is running.
type screencastEarlyFrame struct {
	frame ScreencastFrame
	acked bool
}

// screencastAckTask is a request to acknowledge a frame.
type screencastAckTask struct {
	sessionID int
	early     *screencastEarlyFrame // set if the frame was received before the start reply
}

// screencastAcks is an unbounded queue of pending frame acknowledgements,
// also tracking the number of in-flight acks and the first ack error.
type screencastAcks struct {
	mu      sync.Mutex
	cond    sync.Cond
	queue   []screencastAckTask
	pending int
	err     error
	closed  bool
}

func newScreencastAcks() *screencastAcks {
	a := &screencastAcks{}
	a.cond.L = &a.mu
	return a
}

func (a *screencastAcks) push(t screencastAckTask) {
	a.mu.Lock()
	if !a.closed {
		a.queue = append(a.queue, t)
		a.pending++
		a.cond.Signal()
	}
	a.mu.Unlock()
}

func (a *screencastAcks) pop() (screencastAckTask, bool) {
	a.mu.Lock()
	for len(a.queue) == 0 && !a.closed {
		a.cond.Wait()
	}
	if len(a.queue) == 0 {
		a.mu.Unlock()
		return screencastAckTask{}, false
	}
	t := a.queue[0]
	a.queue = a.queue[1:]
	a.mu.Unlock()
	return t, true
}

func (a *screencastAcks) done(err error) {
	a.mu.Lock()
	if err != nil && a.err == nil {
		a.err = err
	}
	a.pending--
	a.cond.Broadcast()
	a.mu.Unlock()
}

// waitIdle blocks until all queued acks have completed and returns the first
// ack error, if any.
func (a *screencastAcks) waitIdle() error {
	a.mu.Lock()
	for a.pending > 0 {
		a.cond.Wait()
	}
	err := a.err
	a.mu.Unlock()
	return err
}

func (a *screencastAcks) close() {
	a.mu.Lock()
	a.closed = true
	a.cond.Broadcast()
	a.mu.Unlock()
}

// Screencast represents an active screencast stream.
// A RemoteDebugger can own at most one stream at a time; see StartScreencast.
type Screencast struct {
	remote *RemoteDebugger
	frames chan ScreencastFrame // mailbox: holds at most the latest undelivered frame

	acks *screencastAcks

	mu           sync.Mutex
	state        screencastState
	early        []*screencastEarlyFrame // frames received before the start reply
	err          error                   // fatal stream error (ack failure)
	stopDone     chan struct{}           // non-nil while a stop is in flight
	stopErr      error                   // result of the last stop attempt
	framesClosed bool
}

// StartScreencast starts a screencast on the current tab (Page.startScreencast
// with format "png" and everyNthFrame 1) and returns the stream.
//
// A RemoteDebugger can own at most one stream at a time: calling StartScreencast
// while a stream is starting, running or stopping fails with ErrorScreencastActive
// without sending any command. Different connections are independent.
//
// Frames received before the start reply are acknowledged but not delivered;
// once the stream is running the latest acknowledged one is delivered.
//
// If the browser replies with a protocol error, no frame is published, the
// connection ownership is released and the returned error (*ResponseError)
// preserves the code and message returned by the browser.
func (remote *RemoteDebugger) StartScreencast() (*Screencast, error) {
	remote.Lock()
	if remote.screencastErr != nil {
		err := remote.screencastErr
		remote.Unlock()
		return nil, err
	}
	if remote.screencast != nil {
		remote.Unlock()
		return nil, ErrorScreencastActive
	}
	s := newScreencast(remote)
	remote.screencast = s
	remote.Unlock()

	_, err := remote.sendRawReplyRequest("Page.startScreencast", Params{
		"format":        "png",
		"everyNthFrame": 1,
	})
	if err != nil {
		// start failed: don't publish frames and release the ownership
		remote.Lock()
		if remote.screencast == s {
			remote.screencast = nil
		}
		remote.Unlock()

		s.mu.Lock()
		if s.state != screencastFailed {
			s.state = screencastFailed
			s.err = err
		}
		s.closeFramesLocked()
		s.mu.Unlock()
		s.acks.close()
		return nil, err
	}

	if err := s.startDone(); err != nil {
		return nil, err
	}
	return s, nil
}

func newScreencast(remote *RemoteDebugger) *Screencast {
	s := &Screencast{
		remote: remote,
		frames: make(chan ScreencastFrame, 1),
		acks:   newScreencastAcks(),
	}
	go s.ackLoop()
	return s
}

// Frames returns the channel of screencast frames.
// The channel holds at most the most recent frame not yet taken: new frames
// replace older ones. The channel is closed when the stream stops or fails.
func (s *Screencast) Frames() <-chan ScreencastFrame {
	return s.frames
}

// Err returns the fatal error that closed the stream (e.g. a failed frame
// acknowledgement), or nil if the stream is still running or stopped cleanly.
func (s *Screencast) Err() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.err
}

// Stop stops the screencast (Page.stopScreencast).
//
// Concurrent calls share a single stop attempt: only one Page.stopScreencast
// command is sent and overlapping callers obtain the same result. Frames
// received after the stop request are still acknowledged but not delivered.
// Stop returns after the stop reply is received and all frames received so
// far have been acknowledged successfully; at that point the mailbox is
// cleared, the Frames channel is closed and the connection ownership is
// released, so a new stream can be started. After a successful stop, calling
// Stop again returns immediately.
//
// If the browser replies with a protocol error the stream is kept alive and
// Stop can be retried; the returned error (*ResponseError) preserves the code
// and message returned by the browser.
func (s *Screencast) Stop() error {
	s.mu.Lock()
	switch s.state {
	case screencastStopped:
		err := s.stopErr
		s.mu.Unlock()
		return err
	case screencastFailed:
		err := s.err
		s.mu.Unlock()
		return err
	case screencastStopping:
		// a stop is already in flight: wait for its result
		done := s.stopDone
		s.mu.Unlock()
		<-done
		s.mu.Lock()
		err := s.stopErr
		s.mu.Unlock()
		return err
	}

	s.state = screencastStopping
	s.stopDone = make(chan struct{})
	done := s.stopDone
	s.mu.Unlock()

	_, err := s.remote.sendRawReplyRequest("Page.stopScreencast", nil)
	if err != nil {
		// stop failed: keep the stream alive, it can be stopped again
		s.mu.Lock()
		if s.state == screencastStopping {
			s.state = screencastRunning
		}
		s.stopErr = err
		s.stopDone = nil
		s.mu.Unlock()
		close(done)
		return err
	}

	// wait for the acks of all frames received so far
	ackErr := s.acks.waitIdle()
	if ackErr != nil {
		s.fail(ackErr)

		s.mu.Lock()
		s.stopErr = ackErr
		s.stopDone = nil
		s.mu.Unlock()
		close(done)
		return ackErr
	}

	s.mu.Lock()
	if s.state == screencastFailed {
		// failed concurrently while stopping
		err := s.err
		s.stopErr = err
		s.stopDone = nil
		s.mu.Unlock()
		close(done)
		return err
	}

	s.state = screencastStopped
	s.stopErr = nil
	s.stopDone = nil
	s.closeFramesLocked()
	s.mu.Unlock()

	s.remote.Lock()
	if s.remote.screencast == s {
		s.remote.screencast = nil
	}
	s.remote.Unlock()

	s.acks.close()
	close(done)
	return nil
}

// handleFrame processes a Page.screencastFrame event from the read loop.
// It never blocks the read loop: the frame is decoded, the mailbox is updated
// and the acknowledgement is queued for the ack loop.
func (s *Screencast) handleFrame(raw json.RawMessage) {
	var params Params
	if err := json.Unmarshal(raw, &params); err != nil {
		log.Println("screencast frame unmarshal:", err)
		return
	}

	frame := ScreencastFrame{SessionID: params.Int("sessionId")}
	deliver := true

	if md := params.Map("metadata"); md != nil {
		frame.Metadata = deepCopyParams(md)
	}

	if data := params.String("data"); data != "" {
		decoded, err := base64.StdEncoding.DecodeString(data)
		if err != nil {
			log.Println("screencast frame data:", err)
			deliver = false
		} else {
			frame.Data = decoded
		}
	}

	task := screencastAckTask{sessionID: frame.SessionID}

	s.mu.Lock()
	switch s.state {
	case screencastStarting:
		// acknowledge but don't deliver before the start reply
		if deliver {
			ef := &screencastEarlyFrame{frame: frame}
			s.early = append(s.early, ef)
			task.early = ef
		}
	case screencastRunning:
		if deliver {
			s.deliverLocked(frame)
		}
	case screencastStopping:
		// acknowledge but don't deliver while stopping
	default:
		// stopped or failed: nothing to do
		s.mu.Unlock()
		return
	}
	s.mu.Unlock()

	s.acks.push(task)
}

// deliverLocked replaces the mailbox content with the latest frame.
// It must be called with s.mu held and only while the stream is running.
func (s *Screencast) deliverLocked(frame ScreencastFrame) {
	select {
	case s.frames <- frame:
	default:
		select {
		case <-s.frames:
		default:
		}
		s.frames <- frame
	}
}

// closeFramesLocked drains the mailbox and closes the frames channel.
// It must be called with s.mu held.
func (s *Screencast) closeFramesLocked() {
	if s.framesClosed {
		return
	}
	s.framesClosed = true
	select {
	case <-s.frames:
	default:
	}
	close(s.frames)
}

// startDone marks the stream as running and delivers the latest early frame
// that has already been acknowledged. It returns the stream error if the
// stream failed while starting.
func (s *Screencast) startDone() error {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.state == screencastFailed {
		return s.err
	}

	s.state = screencastRunning

	var latest *screencastEarlyFrame
	kept := s.early[:0]
	for _, ef := range s.early {
		if ef.acked {
			latest = ef
		} else {
			kept = append(kept, ef)
		}
	}
	s.early = kept

	if latest != nil {
		s.deliverLocked(latest.frame)
	}
	return nil
}

// earlyAcked marks an early frame as acknowledged; if the stream is already
// running the frame becomes deliverable.
func (s *Screencast) earlyAcked(ef *screencastEarlyFrame) {
	s.mu.Lock()
	defer s.mu.Unlock()

	ef.acked = true
	if s.state != screencastRunning {
		return
	}

	for i, e := range s.early {
		if e == ef {
			s.early = append(s.early[:i], s.early[i+1:]...)
			break
		}
	}
	s.deliverLocked(ef.frame)
}

// fail closes the stream because of a fatal error (a failed frame
// acknowledgement) and prevents new streams on this connection.
func (s *Screencast) fail(err error) {
	s.mu.Lock()
	if s.state == screencastStopped || s.state == screencastFailed {
		s.mu.Unlock()
		return
	}
	s.state = screencastFailed
	if s.err == nil {
		s.err = err
	}
	s.mu.Unlock()

	s.remote.screencastFailed(s, err)
	s.acks.close()

	s.mu.Lock()
	s.closeFramesLocked()
	s.mu.Unlock()
}

// screencastFailed records a fatal stream error on the connection and
// releases the stream ownership; no new stream can be started afterwards.
func (remote *RemoteDebugger) screencastFailed(s *Screencast, err error) {
	remote.Lock()
	if remote.screencast == s {
		remote.screencast = nil
	}
	if remote.screencastErr == nil {
		remote.screencastErr = err
	}
	remote.Unlock()
}

// ackLoop acknowledges frames in arrival order, one at a time, waiting for
// each confirmation reply. It runs in its own goroutine so neither the read
// loop nor the consumer of Frames is involved.
func (s *Screencast) ackLoop() {
	for {
		task, ok := s.acks.pop()
		if !ok {
			return
		}

		_, err := s.remote.sendRawReplyRequest("Page.screencastFrameAck", Params{
			"sessionId": task.sessionID,
		})

		if err == nil && task.early != nil {
			s.earlyAcked(task.early)
		}

		s.acks.done(err)

		if err != nil {
			s.fail(err)
		}
	}
}

// deepCopyParams returns a deep copy of a Params map, so delivered frames are
// not affected by mutations of event parameters performed by user callbacks.
func deepCopyParams(m map[string]interface{}) Params {
	if m == nil {
		return nil
	}
	cp := make(Params, len(m))
	for k, v := range m {
		cp[k] = deepCopyValue(v)
	}
	return cp
}

func deepCopyValue(v interface{}) interface{} {
	switch t := v.(type) {
	case map[string]interface{}:
		return deepCopyParams(t)
	case []interface{}:
		cp := make([]interface{}, len(t))
		for i, e := range t {
			cp[i] = deepCopyValue(e)
		}
		return cp
	default:
		return v
	}
}
