package chromedp

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/chromedp/cdproto"
	"github.com/chromedp/cdproto/cdp"
	"github.com/chromedp/cdproto/dom"
	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/cdproto/runtime"
	"github.com/chromedp/cdproto/target"
	"github.com/chromedp/chromedp/task_support/taskcheck"
)

// nodeSpec is one node of the fake document of a session.
type nodeSpec struct {
	id              int64
	name            string
	nodeType        int64
	frameID         string
	attrs           []string
	children        []*nodeSpec
	contentDocument *nodeSpec
}

// sessionScript is the fake state behind one CDP session.
type sessionScript struct {
	frameID string
	doc     *nodeSpec
	texts   map[int64]string
}

// seenCommand records a command that the production Browser/Target pump sent.
type seenCommand struct {
	session string
	method  string
	params  []byte
}

// replyGate blocks the replies to the commands of a session until ch closes.
type replyGate struct {
	session string
	method  string
	ch      chan struct{}
}

// frameHarness is an offline browser over the channel transport. It replies to
// the production message pumps with scripted documents, and the tests inject
// the events (auto-attach, execution contexts, detaches) in a controlled order.
type frameHarness struct {
	ctx    context.Context
	cancel context.CancelFunc
	w      *frameWire
	b      *Browser
	root   *Target

	mu     sync.Mutex
	cmds   []seenCommand
	script map[string]*sessionScript
	gates  []replyGate

	// watchers receive every command, for tests that need to order events on
	// a specific wire message. Sends never block.
	watchers []chan seenCommand
}

func newFrameHarness(t *testing.T) rootSetup {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	w := newFrameWire()
	b, err := NewBrowserTransport(ctx, w)
	if err != nil {
		cancel()
		t.Fatalf("NewBrowserTransport: %v", err)
	}
	h := &frameHarness{
		ctx:    ctx,
		cancel: cancel,
		w:      w,
		b:      b,
		script: map[string]*sessionScript{},
	}
	t.Cleanup(func() {
		cancel()
		w.Close()
	})
	go h.pump()
	return rootSetup{h: h, t: t}
}

// rootSetup finishes the creation of the root target after the harness exists.
type rootSetup struct {
	h *frameHarness
	t *testing.T
}

// startRoot creates the root page target the way WithTargetID does, and feeds
// it the frame, execution context and document events.
func (s rootSetup) startRoot(script *sessionScript) *frameHarness {
	h, t := s.h, s.t
	h.script["main-session"] = script

	tgt, err := h.b.newTarget(h.ctx, "main-target", "main-session")
	if err != nil {
		t.Fatalf("newTarget: %v", err)
	}
	go tgt.run(h.ctx)
	h.root = tgt

	tgt.pageEvent(&page.EventFrameNavigated{Frame: &cdp.Frame{ID: "main-frame", URL: "https://form.example/"}})
	tgt.runtimeEvent(&runtime.EventExecutionContextCreated{Context: &runtime.ExecutionContextDescription{
		ID:       1,
		UniqueID: "main-context",
		AuxData:  frameJSON(map[string]any{"frameId": "main-frame", "isDefault": true}),
	}})
	tgt.documentUpdated(h.ctx)
	return h
}

// pump answers every command of every session. Each command gets its own
// goroutine, so a gate that delays the initialization of one session never
// blocks the replies or the routing of another session.
func (h *frameHarness) pump() {
	for {
		select {
		case <-h.ctx.Done():
			return
		case m := <-h.w.commands:
			go h.answer(m)
		}
	}
}

func (h *frameHarness) answer(m *cdproto.Message) {
	h.mu.Lock()
	rec := seenCommand{session: string(m.SessionID), method: string(m.Method), params: append([]byte(nil), m.Params...)}
	h.cmds = append(h.cmds, rec)
	watchers := append([]chan seenCommand(nil), h.watchers...)
	var gate *replyGate
	for i := range h.gates {
		g := &h.gates[i]
		if g.session == string(m.SessionID) && g.method == string(m.Method) {
			gate = g
		}
	}
	h.mu.Unlock()

	for _, ch := range watchers {
		select {
		case ch <- rec:
		default:
		}
	}
	if gate != nil {
		select {
		case <-gate.ch:
		case <-h.ctx.Done():
			return
		}
	}

	result := h.replyFor(m)
	select {
	case h.w.incoming <- frameReply(m, result):
	case <-h.ctx.Done():
	}
}

// replyFor computes the result of a command from the script of its session.
func (h *frameHarness) replyFor(m *cdproto.Message) any {
	session := string(m.SessionID)
	h.mu.Lock()
	sc := h.script[session]
	h.mu.Unlock()

	switch string(m.Method) {
	case "Runtime.enable", "Page.enable", "DOM.enable", "CSS.enable",
		"Target.setAutoAttach", "Page.setLifecycleEventsEnabled",
		"Runtime.runIfWaitingForDebugger", "DOM.focus", "Runtime.releaseObject":
		return map[string]any{}

	case "Page.getFrameTree":
		return map[string]any{"frameTree": map[string]any{
			"frame": map[string]any{"id": sc.frameID, "url": "https://" + sc.frameID + ".example/", "loaderId": "loader-" + sc.frameID},
		}}

	case "DOM.getDocument":
		return map[string]any{"root": nodeJSON(sc.doc)}

	case "DOM.querySelector":
		var p struct {
			NodeID   int64  `json:"nodeId"`
			Selector string `json:"selector"`
		}
		_ = unmarshalFrame(m.Params, &p)
		return map[string]any{"nodeId": findScoped(sc.doc, p.NodeID, p.Selector)}

	case "DOM.querySelectorAll":
		var p struct {
			NodeID   int64  `json:"nodeId"`
			Selector string `json:"selector"`
		}
		_ = unmarshalFrame(m.Params, &p)
		return map[string]any{"nodeIds": findAllScoped(sc.doc, p.NodeID, p.Selector)}

	case "DOM.getBoxModel":
		var p struct {
			NodeID int64 `json:"nodeId"`
		}
		_ = unmarshalFrame(m.Params, &p)
		return map[string]any{"model": map[string]any{
			"content": []int64{0, 0, 10, 0, 10, 10, 0, 10},
			"padding": []int64{0, 0, 10, 0, 10, 10, 0, 10},
			"border":  []int64{0, 0, 10, 0, 10, 10, 0, 10},
			"margin":  []int64{0, 0, 10, 0, 10, 10, 0, 10},
			"width":   10, "height": 10,
		}}

	case "DOM.resolveNode":
		var p struct {
			NodeID int64 `json:"nodeId"`
		}
		_ = unmarshalFrame(m.Params, &p)
		return map[string]any{"object": map[string]any{"objectId": fmt.Sprintf("obj-%d", p.NodeID)}}

	case "Runtime.callFunctionOn":
		// The visibility checks want a boolean. Every other call returns the
		// text of the node. The fake knows the node from the object id.
		var p struct {
			ObjectID string `json:"objectId"`
		}
		_ = unmarshalFrame(m.Params, &p)
		var id int64
		fmt.Sscanf(p.ObjectID, "obj-%d", &id)
		if strings.Contains(string(m.Params), "offsetWidth") {
			return map[string]any{"result": map[string]any{"type": "boolean", "value": true}}
		}
		return map[string]any{"result": map[string]any{"type": "string", "value": sc.texts[id]}}

	case "Input.dispatchKeyEvent":
		return map[string]any{}
	}
	return map[string]any{}
}

// nodeJSON converts a scripted node to the protocol shape.
func nodeJSON(n *nodeSpec) map[string]any {
	m := map[string]any{
		"nodeId": n.id, "backendNodeId": n.id, "nodeType": n.nodeType,
		"nodeName": n.name, "localName": strings.ToLower(n.name), "nodeValue": "",
	}
	if n.frameID != "" {
		m["frameId"] = n.frameID
	}
	if len(n.attrs) > 0 {
		m["attributes"] = n.attrs
	}
	if n.contentDocument != nil {
		m["contentDocument"] = nodeJSON(n.contentDocument)
	}
	if len(n.children) > 0 {
		kids := make([]any, len(n.children))
		for i, c := range n.children {
			kids[i] = nodeJSON(c)
		}
		m["children"] = kids
	}
	return m
}

// indexNodes indexes every node of a scripted document by its ID, including
// content documents. Node IDs are unique within a session.
func indexNodes(root *nodeSpec) map[int64]*nodeSpec {
	idx := map[int64]*nodeSpec{}
	var walk func(n *nodeSpec)
	walk = func(n *nodeSpec) {
		idx[n.id] = n
		if n.contentDocument != nil {
			walk(n.contentDocument)
		}
		for _, c := range n.children {
			walk(c)
		}
	}
	walk(root)
	return idx
}

// findScoped returns the first match inside the subtree of startID.
func findScoped(root *nodeSpec, startID int64, selector string) int64 {
	ids := findAllScoped(root, startID, selector)
	if len(ids) == 0 {
		return 0
	}
	return int64(ids[0])
}

// findAllScoped returns every match inside the subtree of startID, the way a
// DOM.querySelectorAll call on that node behaves.
func findAllScoped(root *nodeSpec, startID int64, selector string) []cdp.NodeID {
	start := indexNodes(root)[startID]
	if start == nil {
		return nil
	}
	var ids []cdp.NodeID
	var walk func(n *nodeSpec)
	walk = func(n *nodeSpec) {
		if n.nodeType == NodeTypeElement && matches(n, selector) {
			ids = append(ids, cdp.NodeID(n.id))
		}
		if n.contentDocument != nil {
			walk(n.contentDocument)
		}
		for _, c := range n.children {
			walk(c)
		}
	}
	walk(start)
	return ids
}

func matches(n *nodeSpec, selector string) bool {
	switch {
	case strings.HasPrefix(selector, "#"):
		return attr(n, "id") == strings.TrimPrefix(selector, "#")
	case strings.HasPrefix(selector, "."):
		return attr(n, "class") == strings.TrimPrefix(selector, ".")
	default:
		return strings.EqualFold(n.name, selector)
	}
}

func attr(n *nodeSpec, name string) string {
	for i := 0; i+1 < len(n.attrs); i += 2 {
		if n.attrs[i] == name {
			return n.attrs[i+1]
		}
	}
	return ""
}

func unmarshalFrame(b []byte, v any) error {
	return json.Unmarshal(b, v)
}

// session returns the target that the browser routes a session to.
func (h *frameHarness) session(id string) *Target {
	tgt, _ := h.b.targetForSession(target.SessionID(id))
	return tgt
}

// addScript registers the fake state of a session.
func (h *frameHarness) addScript(session string, sc *sessionScript) {
	h.mu.Lock()
	h.script[session] = sc
	h.mu.Unlock()
}

// gateReplies holds every reply of method on session until release is called.
func (h *frameHarness) gateReplies(session, method string) (ch chan struct{}, release func()) {
	ch = make(chan struct{})
	h.mu.Lock()
	h.gates = append(h.gates, replyGate{session: session, method: method, ch: ch})
	h.mu.Unlock()
	return ch, func() { close(ch) }
}

// watch returns a channel that receives the commands as they reach the wire.
func (h *frameHarness) watch() chan seenCommand {
	ch := make(chan seenCommand, 256)
	h.mu.Lock()
	h.watchers = append(h.watchers, ch)
	h.mu.Unlock()
	return ch
}

// countCommands returns how many commands of method the session sent.
func (h *frameHarness) countCommands(session, method string) int {
	h.mu.Lock()
	defer h.mu.Unlock()
	n := 0
	for _, c := range h.cmds {
		if c.session == session && c.method == method {
			n++
		}
	}
	return n
}

// sessionsOf returns the session IDs that sent at least one command.
func (h *frameHarness) sessionsOf(method string) []string {
	h.mu.Lock()
	defer h.mu.Unlock()
	seen := map[string]struct{}{}
	var out []string
	for _, c := range h.cmds {
		if c.method != method {
			continue
		}
		if _, ok := seen[c.session]; !ok {
			seen[c.session] = struct{}{}
			out = append(out, c.session)
		}
	}
	return out
}

// event builds a session-scoped protocol event.
func frameEvent(session target.SessionID, method string, params any) *cdproto.Message {
	return &cdproto.Message{SessionID: session, Method: cdproto.MethodType(method), Params: frameJSON(params)}
}

// browserEvent builds a browser-level protocol event without a session.
func browserEvent(method string, params any) *cdproto.Message {
	return &cdproto.Message{Method: cdproto.MethodType(method), Params: frameJSON(params)}
}

// send pushes a protocol message into the pump of the browser.
func (h *frameHarness) send(m *cdproto.Message) {
	select {
	case h.w.incoming <- m:
	case <-h.ctx.Done():
	}
}

// attach emits the auto-attach event of an iframe target on parentSession.
func (h *frameHarness) attach(parentSession, childSession, childTargetID, frameID, typ string) {
	h.send(frameEvent(target.SessionID(parentSession), "Target.attachedToTarget", &target.EventAttachedToTarget{
		SessionID: target.SessionID(childSession),
		TargetInfo: &target.Info{
			TargetID:      target.ID(childTargetID),
			Type:          typ,
			URL:           "https://" + frameID + ".example/",
			ParentID:      target.ID("main-target"),
			ParentFrameID: cdp.FrameID(frameID),
		},
	}))
}

// detachSession detaches a session at browser level, as Chrome does for a
// process swap.
func (h *frameHarness) detachSession(childSession string) {
	h.send(browserEvent("Target.detachedFromTarget", &target.EventDetachedFromTarget{
		SessionID: target.SessionID(childSession),
	}))
}

// execContext sends the execution context event for the frame of a session.
func (h *frameHarness) execContext(session, frameID string, ctxID runtime.ExecutionContextID) {
	h.send(frameEvent(target.SessionID(session), "Runtime.executionContextCreated", &runtime.EventExecutionContextCreated{
		Context: &runtime.ExecutionContextDescription{
			ID:       ctxID,
			UniqueID: session + "-ctx",
			AuxData:  frameJSON(map[string]any{"frameId": frameID, "isDefault": true}),
		},
	}))
}

// waitFor blocks until the session sends a command of method, or the test ends.
func (h *frameHarness) waitFor(t *testing.T, session, method string) {
	t.Helper()
	ch := h.watch()
	defer func() {
		h.mu.Lock()
		h.watchers = deleteChan(h.watchers, ch)
		h.mu.Unlock()
	}()
	for {
		select {
		case c := <-ch:
			if c.session == session && c.method == method {
				return
			}
		case <-h.ctx.Done():
			t.Fatalf("timed out waiting for %s on %s", method, session)
		}
	}
}

func deleteChan(s []chan seenCommand, ch chan seenCommand) []chan seenCommand {
	for i, c := range s {
		if c == ch {
			return append(s[:i], s[i+1:]...)
		}
	}
	return s
}

// rootDoc is the document of the page target.
func rootDoc() *nodeSpec {
	return &nodeSpec{id: 1, name: "#document", nodeType: NodeTypeDocument, children: []*nodeSpec{
		{id: 2, name: "IFRAME", nodeType: NodeTypeElement, frameID: "oopif-frame-a", attrs: []string{"id", "frame-a"}},
		{id: 22, name: "IFRAME", nodeType: NodeTypeElement, frameID: "oopif-frame-b", attrs: []string{"id", "frame-b"}},
		{id: 30, name: "IFRAME", nodeType: NodeTypeElement, frameID: "local-frame", attrs: []string{"id", "local"}, contentDocument: &nodeSpec{
			id: 40, name: "#document", nodeType: NodeTypeDocument, children: []*nodeSpec{
				{id: 41, name: "INPUT", nodeType: NodeTypeElement, attrs: []string{"id", "local"}},
			},
		}},
		{id: 3, name: "INPUT", nodeType: NodeTypeElement, attrs: []string{"id", "normal"}},
	}}
}

// childDoc is the document of an OOPIF session. Sessions a and b use the same
// node IDs on purpose: the same NodeID is a different element per session.
func childDoc() *nodeSpec {
	return &nodeSpec{id: 10, name: "#document", nodeType: NodeTypeDocument, children: []*nodeSpec{
		{id: 5, name: "INPUT", nodeType: NodeTypeElement, attrs: []string{"id", "inside"}},
		{id: 6, name: "BUTTON", nodeType: NodeTypeElement, attrs: []string{"class", "act"}},
		{id: 9, name: "BUTTON", nodeType: NodeTypeElement, attrs: []string{"class", "act"}},
		// Nested OOPIF inside this iframe.
		{id: 7, name: "IFRAME", nodeType: NodeTypeElement, frameID: "nested-frame", attrs: []string{"id", "nested"}},
	}}
}

func nestedDoc() *nodeSpec {
	return &nodeSpec{id: 20, name: "#document", nodeType: NodeTypeDocument, children: []*nodeSpec{
		{id: 8, name: "INPUT", nodeType: NodeTypeElement, attrs: []string{"id", "deep"}},
	}}
}

// waitForExecContext waits until a session mirrors the execution context of
// its frame.
func (h *frameHarness) waitForExecContext(t *testing.T, session, frameID string, id runtime.ExecutionContextID) {
	t.Helper()
	child := h.session(session)
	if child == nil {
		t.Fatalf("no session %s", session)
	}
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		child.frameMu.RLock()
		got := child.execContexts[cdp.FrameID(frameID)]
		child.frameMu.RUnlock()
		if got == id {
			return
		}
		select {
		case <-h.ctx.Done():
			t.Fatalf("timed out waiting for execution context %d on %s", id, session)
		case <-time.After(2 * time.Millisecond):
		}
	}
	t.Fatalf("execution context %d never arrived on %s", id, session)
}

// rootNode returns a node from the mirror of the root target.
func (h *frameHarness) rootNode(t *testing.T, id cdp.NodeID) *Node {
	t.Helper()
	h.root.frameMu.RLock()
	f := h.root.frames[h.root.cur]
	h.root.frameMu.RUnlock()
	f.RLock()
	defer f.RUnlock()
	n := f.Nodes[id]
	if n == nil {
		t.Fatalf("root mirror has no node %d", id)
	}
	return n
}

// nodeOf returns a node from the mirror of an adopted session.
func (h *frameHarness) nodeOf(t *testing.T, session string, id cdp.NodeID) *Node {
	t.Helper()
	child := h.session(session)
	if child == nil {
		t.Fatalf("no session %s", session)
	}
	child.frameMu.RLock()
	f := child.frames[child.cur]
	child.frameMu.RUnlock()
	f.RLock()
	defer f.RUnlock()
	n := f.Nodes[id]
	if n == nil {
		t.Fatalf("session %s mirror has no node %d", session, id)
	}
	return n
}

// TestFrameTargetAdoption verifies that the browser loop adopts the iframe
// sessions that auto-attach brings, initializes them without blocking
// routing, keeps early execution context events, and rejects non-iframe
// targets.
func TestFrameTargetAdoption(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newFrameHarness(t).startRoot(&sessionScript{frameID: "main-frame", doc: rootDoc()})

		iframeA := h.rootNode(t, 2)
		iframeLocal := h.rootNode(t, 30)

		// The child documents are not pre-registered: adoption must create
		// everything from the auto-attach event.
		h.addScript("child-a", &sessionScript{frameID: "oopif-frame-a", doc: childDoc(), texts: map[int64]string{5: "inside-a"}})
		h.addScript("child-nested", &sessionScript{frameID: "nested-frame", doc: nestedDoc()})

		// Hold the first initialization reply to prove that waiting for it
		// neither blocks routing nor loses the early execution context event.
		_, release := h.gateReplies("child-a", "Runtime.enable")

		h.attach("main-session", "child-a", "iframe-target-a", "oopif-frame-a", "iframe")
		// The execution context event arrives immediately after the attach,
		// before the initialization replies.
		h.execContext("child-a", "oopif-frame-a", 101)

		// The child target exists after the auto-attach event was routed, and
		// the early event reached it while its initialization is still held.
		h.waitFor(t, "child-a", "Runtime.enable")
		c.Check(h.session("child-a") != nil, "browser adopted the child-a session")
		h.waitForExecContext(t, "child-a", "oopif-frame-a", 101)
		child := h.session("child-a")
		child.frameMu.RLock()
		earlyCtx := child.execContexts[cdp.FrameID("oopif-frame-a")]
		child.frameMu.RUnlock()
		c.Check(earlyCtx == runtime.ExecutionContextID(101), "early execution context event was not lost, got %d", earlyCtx)

		// A worker that auto-attaches is not adopted.
		h.attach("main-session", "worker-1", "worker-target-1", "oopif-frame-a", "worker")
		c.Check(h.session("worker-1") == nil, "worker target is not adopted")

		// A popup is a page target, not an iframe, and is not adopted either.
		h.attach("main-session", "popup-1", "popup-target-1", "oopif-frame-a", "page")
		c.Check(h.session("popup-1") == nil, "popup target is not adopted")

		// An attach event for a parent session the browser does not know must
		// not create a borrowable session.
		h.send(frameEvent("foreign-session", "Target.attachedToTarget", &target.EventAttachedToTarget{
			SessionID:  "stolen-child",
			TargetInfo: &target.Info{TargetID: "stolen", Type: "iframe", ParentFrameID: "oopif-frame-a"},
		}))
		c.Check(h.session("stolen-child") == nil, "iframe of a foreign parent session is not borrowed")

		// Before initialization finishes, the frame query is still waiting.
		done := make(chan []*Node, 1)
		go func() {
			nodes, err := Nodes(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
			if err != nil {
				done <- nil
				return
			}
			done <- nodes
		}()
		release()

		var nodes []*Node
		select {
		case nodes = <-done:
		case <-h.ctx.Done():
			t.Fatal("FromFrame query did not finish after initialization")
		}
		c.Check(len(nodes) == 1 && nodes[0].NodeID == 5, "query found node 5 in the adopted frame, got %v", nodes)
		c.Check(h.countCommands("child-a", "DOM.querySelector") >= 1, "lookup ran on the child session")
		c.Check(h.sessionsOf("DOM.querySelector")[0] == "child-a", "lookup did not run on the parent session")

		// Nested iframe: the attach event arrives on the child session, and
		// adoption recurses.
		h.attach("child-a", "child-nested", "iframe-target-nested", "nested-frame", "iframe")
		h.execContext("child-nested", "nested-frame", 201)
		nestedIframe := h.nodeOf(t, "child-a", 7)
		deep, err := Nodes(CSS("#deep"), FromFrame(nestedIframe))(h.ctx, h.root)
		c.Check(err == nil && len(deep) == 1 && deep[0].NodeID == 8, "nested frame query routed to the nested session: %v", err)
		c.Check(h.countCommands("child-nested", "DOM.querySelector") >= 1, "nested lookup ran on the nested session")

		// Same-process iframe uses the parent session and the content document.
		local, err := Nodes(CSS("#local"), FromFrame(iframeLocal))(h.ctx, h.root)
		c.Check(err == nil && len(local) == 1 && local[0].NodeID == 41, "same-process frame query: %v", err)
		c.Check(h.countCommands("main-session", "DOM.querySelector") >= 1, "same-process lookup stayed on the root session")

		// Ordinary root queries keep working as the control case.
		normal, err := Nodes(CSS("#normal"))(h.ctx, h.root)
		c.Check(err == nil && len(normal) == 1 && normal[0].NodeID == 3, "root control query: %v", err)

		// Selector types that FromFrame does not support fail at once.
		_, err = Nodes(Search("inside"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err != nil, "Search with FromFrame is rejected")
		_, err = Nodes(JSPath("document.body"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err != nil, "JSPath with FromFrame is rejected")
		_, err = Nodes(NodeIDs{5}, FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err != nil, "NodeIDs with FromFrame is rejected")
		_, err = Nodes(CSS("#inside"), FromFrame(iframeA), FromNode(iframeA))(h.ctx, h.root)
		c.Check(err != nil, "FromFrame and FromNode together are rejected")
	})
}

// TestFrameQueryRouting verifies that every follow-up operation runs on the
// selected frame target, including when sibling frames reuse node IDs.
func TestFrameQueryRouting(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newFrameHarness(t).startRoot(&sessionScript{frameID: "main-frame", doc: rootDoc()})
		h.addScript("child-a", &sessionScript{frameID: "oopif-frame-a", doc: childDoc(), texts: map[int64]string{5: "text-a"}})
		h.addScript("child-b", &sessionScript{frameID: "oopif-frame-b", doc: childDoc(), texts: map[int64]string{5: "text-b"}})

		h.attach("main-session", "child-a", "iframe-target-a", "oopif-frame-a", "iframe")
		h.execContext("child-a", "oopif-frame-a", 101)
		h.attach("main-session", "child-b", "iframe-target-b", "oopif-frame-b", "iframe")
		h.execContext("child-b", "oopif-frame-b", 102)

		iframeA := h.rootNode(t, 2)
		iframeB := h.rootNode(t, 22)

		// CSS, CSSAll and ID all accept FromFrame.
		one, err := Nodes(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err == nil && len(one) == 1, "CSS lookup in frame a: %v", err)
		all, err := Nodes(CSSAll(".act"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err == nil && len(all) == 2, "CSSAll lookup in frame a: %v", err)
		byID, err := Nodes(ID("inside"), FromFrame(iframeB))(h.ctx, h.root)
		c.Check(err == nil && len(byID) == 1, "ID lookup in frame b: %v", err)

		// The same NodeID in two sibling sessions is two different elements.
		nodesA, err := Nodes(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err == nil, "frame a nodes: %v", err)
		nodesB, err := Nodes(CSS("#inside"), FromFrame(iframeB))(h.ctx, h.root)
		c.Check(err == nil, "frame b nodes: %v", err)
		c.Check(len(nodesA) == 1 && len(nodesB) == 1, "both siblings returned one node")
		c.Check(nodesA[0].NodeID == nodesB[0].NodeID && nodesA[0] != nodesB[0],
			"same NodeID %d in two sessions are different node objects", nodesA[0].NodeID)

		// TextContent reads on the child sessions, never on the parent.
		textA, err := TextContent(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err == nil && textA == "text-a", "text from frame a: %q %v", textA, err)
		textB, err := TextContent(CSS("#inside"), FromFrame(iframeB))(h.ctx, h.root)
		c.Check(err == nil && textB == "text-b", "text from frame b: %q %v", textB, err)
		for _, method := range []string{"DOM.resolveNode", "Runtime.callFunctionOn"} {
			got := h.sessionsOf(method)
			c.Check(len(got) == 2 && got[0] != "main-session" && got[1] != "main-session",
				"%s ran only on child sessions, got %v", method, got)
		}

		// SendKeys types on the child session only.
		_, err = SendKeys(CSS("#inside"), "a", FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err == nil, "SendKeys in frame a: %v", err)
		c.Check(h.countCommands("child-a", "Input.dispatchKeyEvent") == 3, "one rune sends keyDown, char and keyUp to child-a")
		c.Check(h.countCommands("main-session", "Input.dispatchKeyEvent") == 0, "no key events leaked to the parent session")
		c.Check(h.countCommands("child-a", "DOM.focus") == 1, "focus ran on the child session")

		// QueryAfter hands the func the real child target, once.
		runs := 0
		got, err := QueryAfter(CSS("#inside"), func(ctx context.Context, qt *Target, nodes []*Node) (string, error) {
			runs++
			if string(qt.SessionID) != "child-b" {
				return "", fmt.Errorf("func got session %s", qt.SessionID)
			}
			return "ok", nil
		}, FromFrame(iframeB))(h.ctx, h.root)
		c.Check(err == nil && got == "ok" && runs == 1, "QueryAfter ran once on the child-b target: %q %v", got, err)

		// A manually attached target (the path WithTargetID uses) still routes
		// its commands through the browser pump.
		manual, err := h.b.newTarget(h.ctx, "manual-target", "manual-session")
		c.Check(err == nil, "manual target creation: %v", err)
		go manual.run(h.ctx)
		h.addScript("manual-session", &sessionScript{frameID: "manual-frame", doc: childDoc()})
		manual.pageEvent(&page.EventFrameNavigated{Frame: &cdp.Frame{ID: "manual-frame"}})
		manual.runtimeEvent(&runtime.EventExecutionContextCreated{Context: &runtime.ExecutionContextDescription{
			ID: 301, UniqueID: "manual-ctx", AuxData: frameJSON(map[string]any{"frameId": "manual-frame", "isDefault": true}),
		}})
		manual.documentUpdated(h.ctx)
		mnodes, err := Nodes(CSS("#inside"))(h.ctx, manual)
		c.Check(err == nil && len(mnodes) == 1 && mnodes[0].NodeID == 5, "manual WithTargetID-style session still works: %v", err)
		c.Check(h.countCommands("manual-session", "DOM.querySelector") >= 1, "manual session commands are routed")
	})
}

// TestFrameProcessSwap verifies the behavior when an adopted session detaches:
// waiting for the new session, the ErrFrameDetached cases, and the one-shot
// rule for funcs and SendKeys.
func TestFrameProcessSwap(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newFrameHarness(t).startRoot(&sessionScript{frameID: "main-frame", doc: rootDoc()})
		h.addScript("child-a-1", &sessionScript{frameID: "oopif-frame-a", doc: childDoc(), texts: map[int64]string{5: "old"}})
		h.addScript("child-a-2", &sessionScript{frameID: "oopif-frame-a", doc: childDoc(), texts: map[int64]string{5: "new"}})
		h.addScript("child-b", &sessionScript{frameID: "oopif-frame-b", doc: childDoc()})

		h.attach("main-session", "child-a-1", "iframe-target-a-1", "oopif-frame-a", "iframe")
		h.execContext("child-a-1", "oopif-frame-a", 101)
		h.attach("main-session", "child-b", "iframe-target-b", "oopif-frame-b", "iframe")
		h.execContext("child-b", "oopif-frame-b", 102)

		iframeA := h.rootNode(t, 2)
		iframeB := h.rootNode(t, 22)

		old, err := Nodes(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err == nil && len(old) == 1, "query before the swap: %v", err)
		oldNode := old[0]

		// Hold the initialization of the new session so the query has to wait.
		_, release := h.gateReplies("child-a-2", "Runtime.enable")

		// The query starts before the new session exists: it must wait for the
		// new session of the same frame instead of using the old mirror.
		type queryResult struct {
			nodes []*Node
			err   error
		}
		resCh := make(chan queryResult, 1)
		go func() {
			ns, e := Nodes(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
			resCh <- queryResult{ns, e}
		}()

		h.detachSession("child-a-1")
		h.attach("main-session", "child-a-2", "iframe-target-a-2", "oopif-frame-a", "iframe")
		h.execContext("child-a-2", "oopif-frame-a", 111)

		// Still initializing: give the waiting query time to loop.
		time.Sleep(30 * time.Millisecond)
		select {
		case <-resCh:
			t.Fatal("query used the old mirror before the new session was ready")
		default:
		}
		release()

		var res queryResult
		select {
		case res = <-resCh:
		case <-h.ctx.Done():
			t.Fatal("query did not resume on the new session")
		}
		c.Check(res.err == nil && len(res.nodes) == 1, "query succeeded on the new session: %v", res.err)
		c.Check(res.nodes[0] != oldNode, "the new session returned a fresh node mirror, not the old one")
		c.Check(h.countCommands("child-a-2", "DOM.querySelector") >= 1, "the retry ran on the new session")

		// The same option object is reusable while the parent iframe lives.
		again, err := TextContent(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(err == nil && again == "new", "reused option reads the new document: %q %v", again, err)

		// The sibling frame keeps working across the swap.
		sibling, err := Nodes(CSS("#inside"), FromFrame(iframeB))(h.ctx, h.root)
		c.Check(err == nil && len(sibling) == 1, "sibling frame still works: %v", err)

		// After the func started, it runs exactly once and its error is the
		// one returned, even if the session detaches while it runs.
		runs := 0
		entered := make(chan struct{})
		funcGate := make(chan struct{})
		sentinel := errors.New("sentinel")
		funcErr := make(chan error, 1)
		go func() {
			_, e := QueryAfter(CSS("#inside"), func(ctx context.Context, qt *Target, nodes []*Node) (int, error) {
				runs++
				close(entered)
				<-funcGate
				return 0, sentinel
			}, FromFrame(iframeA))(h.ctx, h.root)
			funcErr <- e
		}()
		select {
		case <-entered:
		case <-h.ctx.Done():
			t.Fatal("QueryAfter func never started")
		}
		h.detachSession("child-a-2")
		close(funcGate)
		select {
		case e := <-funcErr:
			c.Check(errors.Is(e, sentinel), "the original func error is returned: %v", e)
		case <-h.ctx.Done():
			t.Fatal("QueryAfter did not return")
		}
		c.Check(runs == 1, "the func ran once after the detach, got %d calls", runs)

		// SendKeys that started typing does not re-query or replay after a
		// detach. Re-attach a fresh session first. The script must exist
		// before the attach, because the session initializes at once.
		h.addScript("child-a-3", &sessionScript{frameID: "oopif-frame-a", doc: childDoc()})
		h.attach("main-session", "child-a-3", "iframe-target-a-3", "oopif-frame-a", "iframe")
		h.execContext("child-a-3", "oopif-frame-a", 121)
		h.waitFor(t, "child-a-3", "DOM.getDocument")

		// Park the key-event replies so SendKeys stops in the middle of typing.
		_, releaseKeys := h.gateReplies("child-a-3", "Input.dispatchKeyEvent")
		sendErr := make(chan error, 1)
		go func() {
			_, e := SendKeys(CSS("#inside"), "a", FromFrame(iframeA))(h.ctx, h.root)
			sendErr <- e
		}()
		h.waitFor(t, "child-a-3", "Input.dispatchKeyEvent")
		c.Check(h.countCommands("child-a-3", "Input.dispatchKeyEvent") >= 1, "SendKeys reached the key phase")
		h.detachSession("child-a-3")
		releaseKeys()
		select {
		case e := <-sendErr:
			c.Check(errors.Is(e, ErrTargetDetached), "SendKeys returns the detach error: %v", e)
		case <-h.ctx.Done():
			t.Fatal("SendKeys did not return after the detach")
		}
		// A single attempt of one rune is three key events at most. A replay
		// doubles the count.
		c.Check(h.countCommands("child-a-3", "Input.dispatchKeyEvent") <= 3, "SendKeys was not replayed after the detach")

		// Parent iframe element removed: ErrFrameDetached, no root fallback.
		h.send(frameEvent("main-session", "DOM.childNodeRemoved", &dom.EventChildNodeRemoved{
			ParentNodeID: 1, NodeID: 2,
		}))
		_, err = Nodes(CSS("#inside"), FromFrame(iframeA))(h.ctx, h.root)
		c.Check(errors.Is(err, ErrFrameDetached), "removed iframe element gives ErrFrameDetached, got %v", err)

		// Whole parent document replaced: the old node is gone.
		h.send(frameEvent("main-session", "DOM.documentUpdated", &dom.EventDocumentUpdated{}))
		h.waitFor(t, "main-session", "DOM.getDocument")
		_, err = Nodes(CSS("#inside"), FromFrame(iframeB))(h.ctx, h.root)
		c.Check(errors.Is(err, ErrFrameDetached), "replaced parent document gives ErrFrameDetached, got %v", err)

		// A node that belongs to no target of the subtree.
		stranger := &Node{Node: &cdp.Node{NodeID: 999, FrameID: "somewhere-else"}}
		_, err = Nodes(CSS("#inside"), FromFrame(stranger))(h.ctx, h.root)
		c.Check(errors.Is(err, ErrFrameDetached), "foreign node gives ErrFrameDetached, got %v", err)
	})
}
