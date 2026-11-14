package chromedp

import (
	"context"
	"errors"
	"sync"
	"time"

	"github.com/chromedp/cdproto/cdp"
	"github.com/chromedp/cdproto/css"
	"github.com/chromedp/cdproto/dom"
	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/cdproto/runtime"
	"github.com/chromedp/cdproto/target"
)

// frameTarget is one session that a target adopted for an iframe of its frame
// subtree, together with the frame ID of the iframe in that session. A process
// swap replaces the session, so a frame ID can map to a sequence of these.
type frameTarget struct {
	target *Target

	idMu sync.RWMutex
	// frameID is the main frame of the session. The init goroutine sets it
	// after Page.getFrameTree. Queries can read it while a session is still
	// initializing, so the lock guards it.
	frameID cdp.FrameID

	// ready closes after the session enabled its domains, loaded its frame
	// tree and document, and received the execution context of its frame.
	ready chan struct{}
	// dead closes when the browser detached the session.
	dead chan struct{}
}

// setFrameID records the main frame of the session.
func (ft *frameTarget) setFrameID(id cdp.FrameID) {
	ft.idMu.Lock()
	ft.frameID = id
	ft.idMu.Unlock()
}

// FrameID returns the main frame of the session. It is empty until the init
// goroutine learned it from Page.getFrameTree.
func (ft *frameTarget) FrameID() cdp.FrameID {
	ft.idMu.RLock()
	defer ft.idMu.RUnlock()
	return ft.frameID
}

// frameQuery is the state that the FromFrame option adds to a selector.
type frameQuery struct {
	// node is the iframe element in the document of the target, or of an
	// adopted frame subtree of it.
	node *Node

	// started marks that the after func of the query, or its last func, has
	// begun. From then on a detach is an error, and not a reason to look for a
	// new session.
	started bool
	mu      sync.Mutex
}

// markStarted notes that the query reached its funcs. They run once.
func (q *frameQuery) markStarted() {
	q.mu.Lock()
	q.started = true
	q.mu.Unlock()
}

// didStart reports whether the query reached its funcs.
func (q *frameQuery) didStart() bool {
	q.mu.Lock()
	defer q.mu.Unlock()
	return q.started
}

// initFrameTarget enables the domains of an adopted session and loads its frame
// tree and document. It runs on its own goroutine, so waiting for the replies
// never blocks the routing of the browser messages. Events that arrive before
// the replies, such as Runtime.executionContextCreated, go through the target
// pump and are not lost. The frame target becomes ready only after the frame
// and its execution context and document are all known.
func (t *Target) initFrameTarget(ctx context.Context, ft *frameTarget, waitingForDebugger bool) {
	defer close(ft.ready)

	steps := []struct {
		method string
		params any
	}{
		{runtime.Enable.Method, cdp.Empty{}},
		{page.Enable.Method, page.EnableParams{}},
		{dom.Enable.Method, dom.EnableParams{}},
		{css.Enable.Method, cdp.Empty{}},
		// An adopted iframe adopts its own iframes and workers.
		{target.SetAutoAttach.Method, target.SetAutoAttachParams{AutoAttach: true, Flatten: ptr(true)}},
		{page.SetLifecycleEventsEnabled.Method, page.SetLifecycleEventsEnabledParams{Enabled: true}},
	}
	for _, s := range steps {
		if err := ft.target.Call(ctx, s.method, s.params, nil); err != nil {
			return
		}
	}
	if waitingForDebugger {
		if err := ft.target.Call(ctx, runtime.RunIfWaitingForDebugger.Method, cdp.Empty{}, nil); err != nil {
			return
		}
	}

	tree, err := cdp.Call(ctx, ft.target, page.GetFrameTree, cdp.Empty{})
	if err != nil || tree.FrameTree == nil {
		return
	}
	ft.setFrameID(tree.FrameTree.Frame.ID)
	frameID := ft.FrameID()

	ft.target.frameMu.Lock()
	ft.target.frames[tree.FrameTree.Frame.ID] = &Frame{Frame: tree.FrameTree.Frame}
	ft.target.cur = tree.FrameTree.Frame.ID
	ft.target.frameMu.Unlock()

	ft.target.loadDocument(ctx, frameID)

	// Do not report ready before the execution context of the frame exists.
	// The Runtime.executionContextCreated event can arrive before any reply
	// above, or after this point.
	if !ft.target.waitFrameReady(ctx, frameID) {
		return
	}
}

// loadDocument retrieves the document root of a frame and mirrors its nodes.
func (t *Target) loadDocument(ctx context.Context, frameID cdp.FrameID) {
	t.frameMu.RLock()
	f := t.frames[frameID]
	t.frameMu.RUnlock()
	if f == nil {
		return
	}
	f.Lock()
	defer f.Unlock()

	if f.Root != nil {
		close(f.Root.Invalidated)
	}
	f.Nodes = make(map[cdp.NodeID]*Node)
	res, err := cdp.Call(ctx, t, dom.GetDocument, dom.GetDocumentParams{})
	if errors.Is(err, context.Canceled) || errors.Is(err, ErrTargetDetached) {
		return
	}
	if err != nil {
		t.errf("could not retrieve document root for %s: %v", frameID, err)
		return
	}
	f.Root = newNode(res.Root)
	f.Root.Invalidated = make(chan struct{})
	walk(f.Nodes, f.Root)
}

// waitFrameReady waits until the frame has a document root and an execution
// context. It returns false when the context or the session ends first.
func (t *Target) waitFrameReady(ctx context.Context, frameID cdp.FrameID) bool {
	for {
		select {
		case <-ctx.Done():
			return false
		case <-t.detached:
			return false
		default:
		}
		t.frameMu.RLock()
		execCtx := t.execContexts[frameID]
		f := t.frames[frameID]
		t.frameMu.RUnlock()
		var root *Node
		if f != nil {
			f.RLock()
			root = f.Root
			f.RUnlock()
		}
		if execCtx != 0 && root != nil {
			return true
		}
		if err := sleepContext(ctx, frameReadyPoll); err != nil {
			return false
		}
	}
}

// frameReadyPoll is how often a frame target that is initializing checks
// whether its frame and execution context are known.
const frameReadyPoll = 2 * time.Millisecond

// dropFrameTarget forgets an adopted session after the browser detached it.
// Queries that used the session wait for a new session of the same frame,
// unless their funcs already started.
func (t *Target) dropFrameTarget(sessionID target.SessionID) {
	t.frameMu.Lock()
	ft := t.frameSessions[sessionID]
	delete(t.frameSessions, sessionID)
	t.frameMu.Unlock()
	if ft != nil {
		close(ft.dead)
	}
}

// frameForNode resolves the target and frame in which the query of the iframe
// node n must run. For a same-process iframe it returns this target and the
// content document of the node. For an iframe that runs in another process it
// returns the adopted session whose main frame matches the FrameID of n. It
// blocks until the adopted session is ready, or until the context ends.
//
// The match is the real main FrameID of the child target, never a URL, target
// ID or node number. The returned node is the root of the document to query.
func (t *Target) frameForNode(ctx context.Context, n *Node) (*Target, *Frame, *Node, error) {
	if n == nil {
		return nil, nil, nil, errors.New("FromFrame needs an iframe node")
	}
	if n.ContentDocument != nil {
		// Same-process iframe: its content document and execution context live
		// in this target.
		frameID := n.FrameID
		if frameID == "" {
			frameID = t.enclosingFrame(n)
		}
		t.frameMu.RLock()
		f := t.frames[frameID]
		t.frameMu.RUnlock()
		if f == nil {
			f = t.frames[t.cur]
		}
		return t, f, n.ContentDocument, nil
	}

	frameID := n.FrameID
	if frameID == "" {
		frameID = t.enclosingFrame(n)
	}
	if frameID == "" {
		return nil, nil, nil, ErrFrameDetached
	}

	ft, err := t.waitFrameTarget(ctx, frameID, n)
	if err != nil {
		return nil, nil, nil, err
	}
	child := ft.target

	child.frameMu.RLock()
	f := child.frames[ft.FrameID()]
	child.frameMu.RUnlock()
	if f == nil {
		return nil, nil, nil, ErrFrameDetached
	}
	f.RLock()
	root := f.Root
	f.RUnlock()
	if root == nil {
		return nil, nil, nil, ErrFrameDetached
	}
	return child, f, root, nil
}

// waitFrameTarget returns the adopted session whose main frame is frameID,
// waiting through a process swap for the new session when the old one detached
// before the query reached its funcs. It searches the adopted sessions of this
// target and of every adopted subtree, so a nested iframe is found too.
//
// n is the iframe element that the query holds. When it leaves the document
// mirror (the element was removed or the document was replaced) the wait ends
// with ErrFrameDetached instead of waiting for a frame that no longer belongs
// to the target.
func (t *Target) waitFrameTarget(ctx context.Context, frameID cdp.FrameID, n *Node) (*frameTarget, error) {
	for {
		if !t.iframeNodeLive(n) {
			return nil, ErrFrameDetached
		}
		var match *frameTarget
		t.walkFrameSessions(func(ft *frameTarget) bool {
			if ft.FrameID() != frameID {
				return true
			}
			select {
			case <-ft.dead:
				// A dead session is replaced on the next loop.
			default:
				match = ft
			}
			return true
		})

		if match != nil {
			select {
			case <-ctx.Done():
				return nil, ctx.Err()
			case <-match.dead:
				// It detached while we waited for its initialization. The next
				// loop waits for the session that replaces it.
			case <-match.ready:
				select {
				case <-match.dead:
				default:
					return match, nil
				}
			}
		}
		if err := sleepContext(ctx, frameReadyPoll); err != nil {
			return nil, err
		}
	}
}

// currentFrame returns the frame of the current top-level document.
func (t *Target) currentFrame() *Frame {
	t.frameMu.RLock()
	defer t.frameMu.RUnlock()
	return t.frames[t.cur]
}

// walkFrameSessions calls fn for every adopted session of this target and of
// every adopted frame subtree. fn returning false stops the walk.
func (t *Target) walkFrameSessions(fn func(*frameTarget) bool) {
	t.frameMu.RLock()
	sessions := make([]*frameTarget, 0, len(t.frameSessions))
	for _, ft := range t.frameSessions {
		sessions = append(sessions, ft)
	}
	t.frameMu.RUnlock()
	for _, ft := range sessions {
		if !fn(ft) {
			return
		}
		ft.target.walkFrameSessions(fn)
	}
}

// nodeKnown reports whether n belongs to the node mirror of the frame.
func (f *Frame) nodeKnown(n *Node) bool {
	f.RLock()
	defer f.RUnlock()
	return f.Nodes[n.NodeID] == n
}

// iframeNodeLive reports whether the iframe element n is still part of the
// document mirror of its frame. A document update replaces the mirror, so an
// old node fails even when its node ID was reused. The iframe element can
// itself sit in an adopted frame subtree, so every adopted session is checked.
func (t *Target) iframeNodeLive(n *Node) bool {
	if n == nil {
		return false
	}
	top := t.currentFrame()
	if top != nil && top.nodeKnown(n) {
		return true
	}
	live := false
	t.walkFrameSessions(func(ft *frameTarget) bool {
		ft.target.frameMu.RLock()
		f := ft.target.frames[ft.FrameID()]
		ft.target.frameMu.RUnlock()
		if f != nil && f.nodeKnown(n) {
			live = true
			return false
		}
		return true
	})
	return live
}
