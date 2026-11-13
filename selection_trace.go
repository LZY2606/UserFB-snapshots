package readability

import (
	"golang.org/x/net/html"
)

// Event kinds recorded in SelectionAttempt.Events.
const (
	// SelectionEventInitial is recorded when a node is initialized with
	// its combined tag and class weight score.
	SelectionEventInitial = "initial"
	// SelectionEventContribution is recorded when a scored element adds
	// part of its score to one of its ancestors.
	SelectionEventContribution = "contribution"
	// SelectionEventDensity is recorded when a candidate's score is
	// scaled down by its link density.
	SelectionEventDensity = "density"
)

// SelectionNode identifies a node that took part in content selection.
// Nodes that were present in the document tree received by ParseDocument
// are numbered in preorder starting from 1 and keep their Path. Nodes
// created later by the parser itself are marked Synthetic, receive an ID
// that never collides with the original numbering, and have a nil Path.
type SelectionNode struct {
	// ID is the unique identifier of the node inside a SelectionTrace.
	ID int `json:"id"`
	// Path locates an original node inside the tree that was passed to
	// ParseDocument: it lists the child index (counting children of all
	// node types, zero based) to follow at each level, starting from the
	// root that was passed in. It is nil for synthetic nodes.
	Path []int `json:"path,omitempty"`
	// Synthetic is true for nodes that were created by the parser while
	// preparing or scoring the document, instead of coming from the
	// original tree.
	Synthetic bool `json:"synthetic"`
}

// SelectionEvent is a single score write that happened while the parser
// was looking for the main content. Events are recorded in the exact
// order in which the writes were performed.
type SelectionEvent struct {
	// Kind is one of SelectionEventInitial, SelectionEventContribution
	// or SelectionEventDensity.
	Kind string `json:"kind"`
	// NodeID is the node whose score was written.
	NodeID int `json:"nodeID"`
	// SourceID is the scored element the contribution came from.
	// It is 0 for events that are not contributions.
	SourceID int `json:"sourceID"`
	// Level is the zero based level of the node inside the ancestor
	// sequence of the scored element. It is -1 for events that are
	// not contributions.
	Level int `json:"level"`
	// Before is the score read from the node right before the write.
	Before float64 `json:"before"`
	// Operand is the value the write was computed from: the combined
	// tag and class weight for "initial", the divided paragraph score
	// increment for "contribution" and the link density for "density".
	Operand float64 `json:"operand"`
	// After is the score as stored by setContentScore, i.e. formatted
	// with four decimal places and read back.
	After float64 `json:"after"`
}

// SelectionAttempt describes one real attempt of the parser to extract
// the main content. Attempts are stored in the order they were executed.
type SelectionAttempt struct {
	// Index is the zero based position of the attempt in execution order.
	Index int `json:"index"`
	// StripUnlikelys is the value of the flag when this attempt started.
	StripUnlikelys bool `json:"stripUnlikelys"`
	// UseWeightClasses is the value of the flag when this attempt started.
	UseWeightClasses bool `json:"useWeightClasses"`
	// CleanConditionally is the value of the flag when this attempt started.
	CleanConditionally bool `json:"cleanConditionally"`
	// Events are the score writes performed during this attempt.
	Events []SelectionEvent `json:"events,omitempty"`
	// RankedIDs is the complete order of the candidates after their
	// scores were scaled by link density and sorted. It is not limited
	// to the top candidates.
	RankedIDs []int `json:"rankedIDs,omitempty"`
	// WinnerNodeID is the node that was actually used after ancestor
	// promotion, single-child lifting and the body fallback, before
	// sibling merging. It does not have to equal RankedIDs[0]. It is 0
	// for attempts where no candidate was formed.
	WinnerNodeID int `json:"winnerNodeID"`
	// CleanLength is the number of normalized Unicode characters of the
	// cleaned content of this attempt, the value that was compared
	// against CharThresholds.
	CleanLength int `json:"cleanLength"`
}

// SelectionTrace is the structured diagnostic recorded while selecting
// the main content of a document. It is only collected when
// Parser.ExplainSelection is enabled.
type SelectionTrace struct {
	// Nodes holds the definition of every node ID referenced by the
	// trace. Each ID is defined exactly once.
	Nodes []SelectionNode `json:"nodes"`
	// Attempts holds every attempt in the order it was executed.
	Attempts []SelectionAttempt `json:"attempts"`
	// ChosenAttempt is the Index of the attempt whose content was
	// returned, even when the attempts had to be sorted by length to
	// pick the longest one. It is -1 when the parse succeeded but no
	// content was found.
	ChosenAttempt int `json:"chosenAttempt"`
}

// selectionRecorder collects the SelectionTrace while grabArticle runs.
// Node identity is tracked through a pointer map so that nothing has to
// be written into the original tree or into the output HTML.
type selectionRecorder struct {
	nodeIDs  map[*html.Node]int
	nodes    []SelectionNode
	attempts []SelectionAttempt
	contents []*html.Node
	current  *SelectionAttempt
	chosen   int
}

func newSelectionRecorder() *selectionRecorder {
	return &selectionRecorder{
		nodeIDs: make(map[*html.Node]int),
		chosen:  -1,
	}
}

// defineOriginal numbers the elements of the tree that was passed to
// ParseDocument in preorder, starting from 1, and records their paths.
func (r *selectionRecorder) defineOriginal(root *html.Node) {
	var walk func(n *html.Node, path []int)
	walk = func(n *html.Node, path []int) {
		if n.Type == html.ElementNode {
			id := len(r.nodes) + 1
			copied := make([]int, len(path))
			copy(copied, path)
			r.nodeIDs[n] = id
			r.nodes = append(r.nodes, SelectionNode{ID: id, Path: copied})
		}

		for i, child := 0, n.FirstChild; child != nil; i, child = i+1, child.NextSibling {
			childPath := make([]int, len(path)+1)
			copy(childPath, path)
			childPath[len(path)] = i
			walk(child, childPath)
		}
	}
	walk(root, nil)
}

// defineSynthetic assigns a fresh ID that cannot collide with the
// numbering of the original tree. When n is not nil the ID is also
// associated with that node.
func (r *selectionRecorder) defineSynthetic(n *html.Node) int {
	id := len(r.nodes) + 1
	r.nodes = append(r.nodes, SelectionNode{ID: id, Synthetic: true})
	if n != nil {
		r.nodeIDs[n] = id
	}
	return id
}

// idFor returns the ID of a node, assigning a synthetic one when the
// node was created by the parser and has not been seen before.
func (r *selectionRecorder) idFor(n *html.Node) int {
	if id, ok := r.nodeIDs[n]; ok {
		return id
	}
	return r.defineSynthetic(n)
}

// mapClone carries the IDs of a source tree over to its freshly made
// deep clone by walking both trees in parallel. Elements of the source
// tree that have no ID yet were created by the parser itself and are
// defined as synthetic, so that later clones of the same node keep the
// same ID.
func (r *selectionRecorder) mapClone(src, dst *html.Node) {
	if src.Type == html.ElementNode {
		id, ok := r.nodeIDs[src]
		if !ok {
			id = r.defineSynthetic(src)
		}
		r.nodeIDs[dst] = id
	}

	srcChild, dstChild := src.FirstChild, dst.FirstChild
	for srcChild != nil && dstChild != nil {
		r.mapClone(srcChild, dstChild)
		srcChild, dstChild = srcChild.NextSibling, dstChild.NextSibling
	}
}

// beginAttempt starts recording a new attempt with the flags that are
// set at the beginning of the attempt.
func (r *selectionRecorder) beginAttempt(f flags) {
	r.current = &SelectionAttempt{
		Index:              len(r.attempts),
		StripUnlikelys:     f.stripUnlikelys,
		UseWeightClasses:   f.useWeightClasses,
		CleanConditionally: f.cleanConditionally,
	}
}

// eventInitial records the initialization of a node score.
func (r *selectionRecorder) eventInitial(node *html.Node, operand, after float64) {
	if r.current == nil {
		return
	}
	r.current.Events = append(r.current.Events, SelectionEvent{
		Kind:     SelectionEventInitial,
		NodeID:   r.idFor(node),
		SourceID: 0,
		Level:    -1,
		Before:   0,
		Operand:  operand,
		After:    after,
	})
}

// eventContribution records a score increment that a scored element
// added to one of its ancestors.
func (r *selectionRecorder) eventContribution(node, source *html.Node, level int, before, operand, after float64) {
	if r.current == nil {
		return
	}
	r.current.Events = append(r.current.Events, SelectionEvent{
		Kind:     SelectionEventContribution,
		NodeID:   r.idFor(node),
		SourceID: r.idFor(source),
		Level:    level,
		Before:   before,
		Operand:  operand,
		After:    after,
	})
}

// eventDensity records the scaling of a candidate score by link density.
func (r *selectionRecorder) eventDensity(node *html.Node, before, operand, after float64) {
	if r.current == nil {
		return
	}
	r.current.Events = append(r.current.Events, SelectionEvent{
		Kind:     SelectionEventDensity,
		NodeID:   r.idFor(node),
		SourceID: 0,
		Level:    -1,
		Before:   before,
		Operand:  operand,
		After:    after,
	})
}

// setRanked records the complete candidate order after the scores were
// scaled by link density and sorted.
func (r *selectionRecorder) setRanked(candidates []*html.Node) {
	if r.current == nil {
		return
	}
	ids := make([]int, 0, len(candidates))
	for _, candidate := range candidates {
		ids = append(ids, r.idFor(candidate))
	}
	r.current.RankedIDs = ids
}

// setWinner records the node that is actually used as top candidate,
// before sibling merging. Attempts without any candidate get 0.
func (r *selectionRecorder) setWinner(node *html.Node, numCandidates int) {
	if r.current == nil {
		return
	}
	if numCandidates == 0 || node == nil {
		r.current.WinnerNodeID = 0
		return
	}
	r.current.WinnerNodeID = r.idFor(node)
}

// finishAttempt closes the current attempt, recording the normalized
// text length that was compared against CharThresholds and the content
// node that was produced.
func (r *selectionRecorder) finishAttempt(cleanLength int, content *html.Node) {
	if r.current == nil {
		return
	}
	r.current.CleanLength = cleanLength
	r.attempts = append(r.attempts, *r.current)
	r.contents = append(r.contents, content)
	r.current = nil
}

// choose marks the attempt whose content node is returned as the chosen
// one. The content nodes are compared by identity, so sorting the
// attempts by text length cannot change the original index.
func (r *selectionRecorder) choose(content *html.Node) {
	for i := len(r.contents) - 1; i >= 0; i-- {
		if r.contents[i] == content {
			r.chosen = r.attempts[i].Index
			return
		}
	}
}

// build assembles the trace that is handed out with the Article. The
// recorder is not used afterwards, so the returned slices are never
// mutated by later parses.
func (r *selectionRecorder) build() *SelectionTrace {
	return &SelectionTrace{
		Nodes:         r.nodes,
		Attempts:      r.attempts,
		ChosenAttempt: r.chosen,
	}
}
