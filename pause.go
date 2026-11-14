package gocrawl

import (
	"sync"
	"time"
)

// crawlerState is the control state of a Crawler for the Pause/Resume API.
type crawlerState uint8

const (
	stateNotRunning crawlerState = iota
	stateRunning
	statePausing
	statePaused
	stateFinishing
)

// crawlGate is the admission gate shared by the master and the workers. It
// coordinates the phase barrier (Pause) without holding a lock while waiting:
// parked workers block on resumeCh and are counted, the master waits for
// every live worker to be parked and for the response channel to be drained.
//
// Each worker holds exactly one registration from launch to termination,
// counted in active. A worker that is waiting while admission is closed is
// counted in parked as well, so quiescence is reached when active == parked
// and no worker response is left unprocessed. This makes the barrier immune to
// workers being idle, waiting for a crawl delay, or holding a batch: they all
// count as parked while they cannot start a new phase.
//
// The channels are used as broadcast signals: pauseCh is closed when a pause
// begins (admission closed), resumeCh is closed when a resume happens. They
// are re-created at the opposite transition, so that every pause round has
// fresh, open signals.
type crawlGate struct {
	mu       sync.Mutex
	open     bool
	active   int
	parked   int
	pauseCh  chan struct{}
	resumeCh chan struct{}
	notify   chan struct{}
}

// newCrawlGate returns an open gate, ready for the first launching of workers.
func newCrawlGate() *crawlGate {
	return &crawlGate{
		open:     true,
		pauseCh:  make(chan struct{}),
		resumeCh: make(chan struct{}),
		notify:   make(chan struct{}, 1),
	}
}

// signalWakeup wakes the master when the quiescence state may have changed.
// It is a non-blocking send, so workers never stall on it.
func (g *crawlGate) signalWakeup() {
	select {
	case g.notify <- struct{}{}:
	default:
	}
}

// addWorker registers a newly launched worker. It is called before the worker
// goroutine can possibly park, so that a concurrent Pause cannot observe a
// false quiescence.
func (g *crawlGate) addWorker() {
	g.mu.Lock()
	g.active++
	g.mu.Unlock()
}

// removeWorker unregisters a terminated worker. g.mu is locked internally.
func (g *crawlGate) removeWorker(w *worker) {
	g.mu.Lock()
	g.active--
	if w.parked {
		w.parked = false
		g.parked--
	}
	g.mu.Unlock()
	g.signalWakeup()
}

// park marks the worker as parked (waiting for admission to re-open). It
// freezes the remaining crawl delay and idle TTL from now. g.mu must be held.
func (g *crawlGate) park(w *worker, now time.Time) {
	if w.parked {
		return
	}
	w.parked = true
	g.parked++
	if !w.waitEnd.IsZero() {
		w.frozenWait = w.waitEnd.Sub(now)
		if w.frozenWait < 0 {
			w.frozenWait = 0
		}
		w.frozenWaitOK = true
		w.waitEnd = time.Time{}
	}
	if !w.idleDeadline.IsZero() {
		w.frozenIdle = w.idleDeadline.Sub(now)
		if w.frozenIdle < 0 {
			w.frozenIdle = 0
		}
		w.frozenIdleOK = true
		w.idleDeadline = time.Time{}
	}
	// Any worker that is waiting when admission closes freezes its idle
	// countdown, including one waiting for the crawl delay or between phases.
	w.idleWaiting = true
	g.signalWakeup()
}

// unpark marks the worker as running again and restarts the frozen crawl
// delay and idle TTL from their remaining duration. g.mu must be held.
func (g *crawlGate) unpark(w *worker, now time.Time) {
	if !w.parked {
		return
	}
	w.parked = false
	g.parked--
	if w.frozenWaitOK {
		w.waitEnd = now.Add(w.frozenWait)
		w.frozenWaitOK = false
	}
	if w.frozenIdleOK {
		w.idleDeadline = now.Add(w.frozenIdle)
		w.frozenIdleOK = false
	}
	g.signalWakeup()
}

// beginPause closes admission for new phases. It is idempotent: if the gate
// is already closed, nothing happens.
func (g *crawlGate) beginPause() {
	g.mu.Lock()
	if g.open {
		g.open = false
		close(g.pauseCh)
	}
	g.mu.Unlock()
	g.signalWakeup()
}

// closeAll puts the gate in its terminal, closed state when a Run ends. Any
// worker that is still parked is released, so Stop can never deadlock on a
// paused worker.
func (g *crawlGate) closeAll() {
	g.mu.Lock()
	if g.open {
		g.open = false
		close(g.pauseCh)
	}
	// Replacing resumeCh releases everyone currently parked; a subsequent
	// close is harmless for workers that already left.
	select {
	case <-g.resumeCh:
		// already closed
	default:
		close(g.resumeCh)
	}
	g.mu.Unlock()
	g.signalWakeup()
}

// resume opens admission again and wakes all parked workers.
func (g *crawlGate) resume() {
	g.mu.Lock()
	if !g.open {
		g.open = true
		close(g.resumeCh)
		g.resumeCh = make(chan struct{})
		g.pauseCh = make(chan struct{})
	}
	g.mu.Unlock()
	g.signalWakeup()
}

// setWaitEnd records the deadline of the crawl delay that starts now. g.mu
// is used because a pause may be freezing the deadline concurrently.
func (g *crawlGate) setWaitEnd(w *worker, end time.Time) {
	g.mu.Lock()
	w.waitEnd = end
	w.frozenWaitOK = false
	g.mu.Unlock()
}

// waitDeadline returns the crawl delay deadline, if one is pending.
func (g *crawlGate) waitDeadline(w *worker) (time.Time, bool) {
	g.mu.Lock()
	defer g.mu.Unlock()
	return w.waitEnd, !w.waitEnd.IsZero()
}

// clearWaitDeadline consumes the pending crawl delay.
func (g *crawlGate) clearWaitDeadline(w *worker) {
	g.mu.Lock()
	w.waitEnd = time.Time{}
	g.mu.Unlock()
}

// armIdle sets the idle deadline of a worker that is about to wait for a new
// phase. resumed reports that this wait continues an idle wait interrupted by
// a pause: the deadline restored by unpark is then kept. Any other wait is a
// fresh one and gets a full TTL.
func (g *crawlGate) armIdle(w *worker, resumed bool) {
	g.mu.Lock()
	defer g.mu.Unlock()

	if !resumed || w.idleDeadline.IsZero() {
		w.idleDeadline = time.Now().Add(w.opts.WorkerIdleTTL)
	}
}

// remainingIdleW returns the remaining idle TTL under the gate lock.
func (g *crawlGate) remainingIdleW(w *worker) time.Duration {
	g.mu.Lock()
	defer g.mu.Unlock()
	if w.idleDeadline.IsZero() {
		return w.opts.WorkerIdleTTL
	}
	d := time.Until(w.idleDeadline)
	if d < 0 {
		return 0
	}
	return d
}

// resetIdleDeadline clears the idle deadline when the worker becomes active.
func (g *crawlGate) resetIdleDeadline(w *worker) {
	g.mu.Lock()
	w.idleDeadline = time.Time{}
	w.frozenIdleOK = false
	g.mu.Unlock()
}

// pauseSignal returns the channel that is closed when the current pause round
// begins, and the channel closed when it ends. They must be read while
// admission is open (from a running worker); after a pause/resume cycle they
// are replaced, so a fresh read is required for each wait.
func (g *crawlGate) pauseSignal() (<-chan struct{}, <-chan struct{}) {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.pauseCh, g.resumeCh
}

// isOpen reports whether admission is currently open.
func (g *crawlGate) isOpen() bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.open
}

// quiescent reports whether admission is closed, every live worker is parked,
// and no worker response is left unprocessed.
func (g *crawlGate) quiescent(pushLen int) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	return !g.open && g.active == g.parked && pushLen == 0
}

// Pause stops the launching of new crawling phases and waits until all
// previously started phases have completed and their results have been
// handed over to the master crawler. The Run call keeps running: Start and
// End are not called again, the visited set and the per-host state are
// preserved, and URLContexts are not re-created.
//
// Pause must be called from a control goroutine, outside of any Extender
// hook. Multiple concurrent Pause calls wait for the same quiescence round;
// calling Pause while already paused returns immediately.
func (c *Crawler) Pause() error {
	c.ctrlMu.Lock()

	switch c.ctrlState {
	case stateNotRunning:
		c.ctrlMu.Unlock()
		return ErrNotRunning
	case statePaused:
		c.ctrlMu.Unlock()
		return nil
	case statePausing:
		done, stop, runDone := c.pauseDone, c.stop, c.runDone
		stopped := c.terminalErr == ErrInterrupted || stopRequested(stop)
		c.ctrlMu.Unlock()
		return c.waitPause(done, stop, runDone, stopped)
	case stateFinishing:
		c.ctrlMu.Unlock()
		return ErrNotRunning
	}

	// Running: start a new quiescence round. Closing admission and changing
	// the state are done atomically with respect to the master's finalize
	// logic.
	done := make(chan struct{})
	c.pauseDone = done
	c.ctrlState = statePausing
	c.gate.beginPause()
	stop, runDone := c.stop, c.runDone
	c.ctrlMu.Unlock()

	return c.waitPause(done, stop, runDone, false)
}

// stopRequested reports whether the given stop channel has been closed. It is
// a plain channel read, it does not need the control lock.
func stopRequested(stop <-chan struct{}) bool {
	select {
	case <-stop:
		return true
	default:
		return false
	}
}

// wasStopped reports whether Stop was called for the current Run.
func (c *Crawler) wasStopped() bool {
	c.ctrlMu.Lock()
	defer c.ctrlMu.Unlock()
	return c.terminalErr == ErrInterrupted || stopRequested(c.stop)
}

// waitPause waits for the master to signal that the quiescence round is
// complete, while remaining interruptible by Stop and by the natural end of
// Run. stopped must report whether Stop had already been called when the
// control state was read, so that the Stop error wins over a Run completion.
func (c *Crawler) waitPause(done <-chan struct{}, stop <-chan struct{}, runDone <-chan struct{}, stopped bool) error {
	if stopped {
		select {
		case <-done:
			return ErrInterrupted
		default:
		}
	}
	select {
	case <-done:
		if stopRequested(stop) || c.wasStopped() {
			return ErrInterrupted
		}
		return nil
	case <-stop:
		return ErrInterrupted
	case <-runDone:
		if stopRequested(stop) || c.wasStopped() {
			return ErrInterrupted
		}
		return ErrNotRunning
	}
}

// Resume re-opens admission and wakes the workers parked by a successful
// Pause. Calling Resume while not paused is a successful no-op, so repeated
// Resume calls are harmless.
func (c *Crawler) Resume() error {
	c.ctrlMu.Lock()

	switch c.ctrlState {
	case stateNotRunning:
		c.ctrlMu.Unlock()
		return ErrNotRunning
	case statePaused:
		select {
		case <-c.stop:
			// Run is already being terminated by a concurrent Stop.
			c.ctrlMu.Unlock()
			return ErrNotRunning
		default:
		}
		c.ctrlState = stateRunning
		c.gate.resume()
		c.ctrlMu.Unlock()
		return nil
	case stateFinishing:
		// Run is being torn down by Stop or by its natural end.
		c.ctrlMu.Unlock()
		return ErrNotRunning
	case statePausing:
		// The controller is expected to call Resume only after Pause
		// returned. Be a harmless no-op if it does not.
		c.ctrlMu.Unlock()
		return nil
	default:
		c.ctrlMu.Unlock()
		return nil
	}
}

// tryFinishPause is called by the master loop. It completes a pending Pause
// round once every worker is parked and every worker response has been
// consumed.
func (c *Crawler) tryFinishPause() {
	c.ctrlMu.Lock()
	defer c.ctrlMu.Unlock()

	if c.ctrlState != statePausing {
		return
	}
	if !c.gate.quiescent(len(c.push)) {
		return
	}
	c.ctrlState = statePaused
	close(c.pauseDone)
}

// claimStop transitions the control state to Finishing for an explicit Stop.
// It returns false when a Run round has already finished, in which case the
// stop channel must not be closed again.
func (c *Crawler) claimStop() bool {
	c.ctrlMu.Lock()
	defer c.ctrlMu.Unlock()

	if c.ctrlState == stateNotRunning || c.ctrlState == stateFinishing {
		return false
	}
	c.ctrlState = stateFinishing
	c.terminalErr = ErrInterrupted
	// Wake a control caller waiting for quiescence or for Run to finish.
	c.closeRunDoneLocked()
	close(c.stop)
	if c.gate != nil {
		// Release any worker parked by a Pause so that the Run can wind down.
		c.gate.closeAll()
	}
	return true
}

// finishControl marks this Run as no longer running and releases any control
// caller waiting on a Pause or on Run's completion.
func (c *Crawler) finishControl() {
	c.ctrlMu.Lock()
	defer c.ctrlMu.Unlock()

	c.ctrlState = stateNotRunning
	if c.gate != nil {
		// Set a canonical closed state so a late Resume reports NotRunning
		// and no worker can be admitted by a stale open gate.
		c.gate.closeAll()
	}
	// Wake a control caller waiting for quiescence or for Run to finish.
	c.closeRunDoneLocked()
}

// terminalError returns the error that ended (or is ending) the current Run.
func (c *Crawler) terminalError() error {
	c.ctrlMu.Lock()
	defer c.ctrlMu.Unlock()
	return c.terminalErr
}
