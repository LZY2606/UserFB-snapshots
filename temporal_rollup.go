package qframe

import (
	"math"
	"sort"

	"github.com/tobgu/qframe/config/newqf"
	"github.com/tobgu/qframe/internal/ecolumn"
	"github.com/tobgu/qframe/internal/icolumn"
	"github.com/tobgu/qframe/internal/scolumn"
	"github.com/tobgu/qframe/qerrors"
	"github.com/tobgu/qframe/types"
)

// TemporalRollup keeps an in-memory, continuously maintained temporal
// association between fact events and versioned classifications together with
// the rollup state of the resulting label groups.
//
// Facts and classification versions can both be corrected. NewTemporalRollup
// builds the full index once; Apply then applies one joint correction batch,
// withdrawing only the contributions that actually change and adding the
// final ones. The object is intended for sequential, single-goroutine use: it
// is not safe for concurrent access and has no persistence layer.
//
// # Index layout and interval interaction
//
// A fact belongs to the last version of the same business key whose "from" is
// not greater than the fact's "at"; a version therefore owns the half-open
// interval [from, nextFrom). Endpoints are compared directly, never widened by
// adding one, so MinInt and MaxInt event times are safe. If no version
// qualifies, the fact belongs to a single unmatched group whose label is NULL.
// The empty string is an ordinary, non-NULL label and stays distinct from that
// unmatched group.
//
// Three ordered indexes, all AVL trees, maintain the association:
//
//   - keys: business key -> the key's temporalTree;
//   - per key, versions: "from" -> current group id, supporting predecessor
//     lookup (classification) and lower-bound lookup (interval boundaries);
//   - per key, facts: (at, id) -> stored fact, supporting ordered range sweeps.
//
// A version insert or delete only changes the adjacent intervals. Before the
// tree edit the affected half-open span [from, successorFrom) is derived from
// the predecessor/successor versions and exactly the facts reached through
// that span of the fact tree are marked involved; facts outside the span are
// never visited. All aggregate movement is deferred to a single finalize pass:
// every involved fact withdraws its pre-batch contribution at most once and
// adds its final contribution at most once, so a fact that leaves a group and
// comes back within the same batch, or is corrected together with the versions
// governing its timeline, produces no spurious noise.
//
// Distinct entity counts are deletable per-group counts: a group maps each
// entity id to the number of facts currently contributing it. One entity copy
// leaving while another copy stays therefore does not decrease the count.
//
// # Complexity
//
// Construction is O(N log N). For an Apply batch with E input rows and delete
// identities and A deduplicated involved facts (directly corrected facts plus
// the facts dominated by a changed version interval on either the old or new
// timeline), work is O((E + A) log(N + 2)) where N is the larger pre/post
// stored population. Snapshot enumerates only groups that still hold events;
// a differential output enumerates only groups touched by the batch.
type TemporalRollup struct {
	keys     *avlTree[string, *temporalTree]
	facts    map[int]*factRecord
	versions map[versionIdentity]struct{}

	groups       []*labelGroup
	labelToGroup map[string]groupID
	stats        TemporalStats
}

// VersionKey identifies a classification version by its business key and the
// inclusive start time of its interval.
type VersionKey struct {
	Key  string
	From int
}

// TemporalStats reports the work performed by the most recent Apply call.
// The counters are reset at the start of every Apply; work done before a
// failed validation is counted as well.
type TemporalStats struct {
	// FactVisits counts stored facts that are read or copied, including
	// validation/location lookups, direct corrections and interval sweeps.
	FactVisits int

	// IndexSteps counts ordered-index node visits and comparisons, including
	// predecessor classification, lower-bound boundary lookup and range sweeps.
	IndexSteps int

	// AggregateOps counts actual withdrawals and additions of a single fact
	// contribution to a label group. One fact contributes at most two.
	AggregateOps int
}

type groupID int

// idNull is the single unmatched group carrying the NULL label.
const idNull groupID = 0

type labelGroup struct {
	label    *string // nil for the unmatched (NULL label) group
	rows     int
	sum      int
	entities map[int]int
}

type factRecord struct {
	id     int
	key    string
	at     int
	amount int
	entity int
}

type versionRecord struct {
	key   string
	from  int
	label string // non-NULL by validation; NULL labels only arise from unmatched facts
}

type versionIdentity struct {
	key  string
	from int
}

type factPoint struct {
	at int
	id int
}

// temporalTree holds one key's ordered versions and ordered facts.
type temporalTree struct {
	versions *avlTree[int, groupID]
	facts    *avlTree[factPoint, *factRecord]
}

func newTemporalTree() *temporalTree {
	return &temporalTree{
		versions: &avlTree[int, groupID]{},
		facts:    &avlTree[factPoint, *factRecord]{},
	}
}

// classifyAt resolves the group governing an event time on this key: the
// version with the largest "from" <= at, or the unmatched group.
func (t *temporalTree) classifyAt(at int, st *TemporalStats) groupID {
	node := t.versions.predecessor(at, lessInt, st)
	if node == nil {
		return idNull
	}
	return node.val
}

// successorFrom returns the "from" strictly after from, or nil when there is
// no successor. A nil result means the interval is unbounded on the right and
// still contains events at MaxInt; intervals are never widened by adding one
// to an endpoint.
func (t *temporalTree) successorFrom(from int, st *TemporalStats) *int {
	node := t.versions.lowerBound(func(v int) bool { return v > from }, st)
	if node == nil {
		return nil
	}
	hi := node.key
	return &hi
}

// ---------- AVL ordered index ----------

// avlTree is a height-balanced binary search tree with unique keys. Every node
// visit and key comparison performed through it is reported on st. The AVL
// guarantee keeps every path at O(log size) even when timelines are appended
// in ascending order.
type avlTree[K comparable, V any] struct {
	root *avlNode[K, V]
	size int
}

type avlNode[K comparable, V any] struct {
	key         K
	val         V
	height      int
	left, right *avlNode[K, V]
}

func nodeHeight[K comparable, V any](n *avlNode[K, V]) int {
	if n == nil {
		return 0
	}
	return n.height
}

func updateHeight[K comparable, V any](n *avlNode[K, V]) {
	h := nodeHeight(n.left)
	if nodeHeight(n.right) > h {
		h = nodeHeight(n.right)
	}
	n.height = h + 1
}

func balanceFactor[K comparable, V any](n *avlNode[K, V]) int {
	return nodeHeight(n.left) - nodeHeight(n.right)
}

func rotateRight[K comparable, V any](n *avlNode[K, V]) *avlNode[K, V] {
	left := n.left
	n.left = left.right
	left.right = n
	updateHeight(n)
	updateHeight(left)
	return left
}

func rotateLeft[K comparable, V any](n *avlNode[K, V]) *avlNode[K, V] {
	right := n.right
	n.right = right.left
	right.left = n
	updateHeight(n)
	updateHeight(right)
	return right
}

func rebalance[K comparable, V any](n *avlNode[K, V]) *avlNode[K, V] {
	updateHeight(n)
	if bf := balanceFactor(n); bf > 1 {
		if balanceFactor(n.left) < 0 {
			n.left = rotateLeft(n.left)
		}
		return rotateRight(n)
	} else if bf < -1 {
		if balanceFactor(n.right) > 0 {
			n.right = rotateRight(n.right)
		}
		return rotateLeft(n)
	}
	return n
}

func (m *avlTree[K, V]) insert(key K, val V, less func(a, b K) bool, st *TemporalStats) {
	m.root = m.insertNode(m.root, key, val, less, st)
}

func (m *avlTree[K, V]) insertNode(n *avlNode[K, V], key K, val V, less func(a, b K) bool, st *TemporalStats) *avlNode[K, V] {
	if n == nil {
		st.IndexSteps++
		m.size++
		return &avlNode[K, V]{key: key, val: val, height: 1}
	}
	st.IndexSteps++
	switch {
	case less(key, n.key):
		n.left = m.insertNode(n.left, key, val, less, st)
	case less(n.key, key):
		n.right = m.insertNode(n.right, key, val, less, st)
	default:
		n.val = val // identity upsert: replace value, keep node
		return n
	}
	return rebalance(n)
}

func (m *avlTree[K, V]) get(key K, less func(a, b K) bool, st *TemporalStats) (*avlNode[K, V], bool) {
	node := m.root
	for node != nil {
		st.IndexSteps++
		switch {
		case less(key, node.key):
			node = node.left
		case less(node.key, key):
			node = node.right
		default:
			return node, true
		}
	}
	return nil, false
}

func (m *avlTree[K, V]) delete(key K, less func(a, b K) bool, st *TemporalStats) bool {
	var removed bool
	m.root, removed = m.deleteNode(m.root, key, less, st)
	if removed {
		m.size--
	}
	return removed
}

// deleteNode returns the (possibly rebalanced) subtree and whether a node with
// the key was removed. The size bookkeeping lives in delete so that removing a
// node with two children (which swaps in its in-order successor) is counted
// exactly once rather than once for the match and once for the successor.
func (m *avlTree[K, V]) deleteNode(n *avlNode[K, V], key K, less func(a, b K) bool, st *TemporalStats) (*avlNode[K, V], bool) {
	if n == nil {
		return nil, false
	}
	st.IndexSteps++
	switch {
	case less(key, n.key):
		left, removed := m.deleteNode(n.left, key, less, st)
		n.left = left
		return rebalance(n), removed
	case less(n.key, key):
		right, removed := m.deleteNode(n.right, key, less, st)
		n.right = right
		return rebalance(n), removed
	default:
		if n.left == nil {
			return n.right, true
		}
		if n.right == nil {
			return n.left, true
		}
		// In-order successor replaces the removed node.
		succ := n.right
		for succ.left != nil {
			st.IndexSteps++
			succ = succ.left
		}
		st.IndexSteps++
		n.key, n.val = succ.key, succ.val
		right, _ := m.deleteNode(n.right, succ.key, less, st)
		n.right = right
		return rebalance(n), true
	}
}

// predecessor returns the node with the largest key <= target, or nil.
func (m *avlTree[K, V]) predecessor(target K, less func(a, b K) bool, st *TemporalStats) *avlNode[K, V] {
	var result *avlNode[K, V]
	node := m.root
	for node != nil {
		st.IndexSteps++
		if less(node.key, target) {
			result, node = node, node.right
		} else if less(target, node.key) {
			node = node.left
		} else {
			return node
		}
	}
	return result
}

// lowerBound returns the first node for which atLeast(key) is true, or nil.
func (m *avlTree[K, V]) lowerBound(atLeast func(K) bool, st *TemporalStats) *avlNode[K, V] {
	var result *avlNode[K, V]
	node := m.root
	for node != nil {
		st.IndexSteps++
		if atLeast(node.key) {
			result, node = node, node.left
		} else {
			node = node.right
		}
	}
	return result
}

// visitRange calls visit for every node with lo <= key < hi, in key order.
// The open end is given as a predicate so callers keyed by a composite point
// can compare only the relevant field: an open end of MaxInt then still
// includes every representable event time instead of excluding the endpoint.
// Only the path to the lower bound and the nodes inside the span are touched.
func (m *avlTree[K, V]) visitRange(lo K, less func(a, b K) bool, belowHi func(K) bool, st *TemporalStats, visit func(K, V)) {
	stack := make([]*avlNode[K, V], 0, nodeHeight(m.root)+1)
	node := m.root
	for node != nil {
		st.IndexSteps++
		if !less(node.key, lo) { // node.key >= lo
			stack = append(stack, node)
			node = node.left
		} else {
			node = node.right
		}
	}
	for len(stack) > 0 {
		cur := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		st.IndexSteps++ // open-end comparison
		if !belowHi(cur.key) {
			return
		}
		visit(cur.key, cur.val)
		node = cur.right
		for node != nil {
			st.IndexSteps++
			stack = append(stack, node)
			node = node.left
		}
	}
}

func (m *avlTree[K, V]) len() int { return m.size }

// ---------- Ordering helpers ----------

func lessInt(a, b int) bool { return a < b }

func lessString(a, b string) bool { return a < b }

func lessFactPoint(a, b factPoint) bool {
	if a.at != b.at {
		return a.at < b.at
	}
	return a.id < b.id
}

// ---------- Construction ----------

// NewTemporalRollup builds the temporal association and rollup state from the
// currently visible rows of facts and versions. Both frames may be views
// produced by Filter, Sort or Slice and may carry extra columns which are
// ignored. Zero-row frames must still carry the typed columns. Construction is
// allowed to build every index from scratch; a failure returns no usable
// object.
func NewTemporalRollup(facts, versions QFrame) (*TemporalRollup, error) {
	factRecs, err := extractFacts(facts)
	if err != nil {
		return nil, err
	}
	versionRecs, err := extractVersions(versions)
	if err != nil {
		return nil, err
	}

	r := &TemporalRollup{
		keys:         &avlTree[string, *temporalTree]{},
		facts:        map[int]*factRecord{},
		versions:     map[versionIdentity]struct{}{},
		labelToGroup: map[string]groupID{},
		groups: []*labelGroup{
			{label: nil, entities: map[int]int{}},
		},
	}

	seenVersions := map[versionIdentity]struct{}{}
	for i := range versionRecs {
		v := versionRecs[i]
		id := versionIdentity{key: v.key, from: v.from}
		if _, dup := seenVersions[id]; dup {
			return nil, qerrors.New("NewTemporalRollup", "duplicate version identity (key=%q, from=%d)", v.key, v.from)
		}
		seenVersions[id] = struct{}{}
		tree := r.ensureKeyTree(v.key)
		gid := r.groupForLabel(v.label)
		tree.versions.insert(v.from, gid, lessInt, &r.stats)
		r.versions[id] = struct{}{}
	}

	seenFacts := map[int]struct{}{}
	for i := range factRecs {
		f := factRecs[i]
		if _, dup := seenFacts[f.id]; dup {
			return nil, qerrors.New("NewTemporalRollup", "duplicate fact id %d", f.id)
		}
		seenFacts[f.id] = struct{}{}
		r.storeNewFact(&f)
	}

	// Stats describe Apply work only; construction bookkeeping is not exposed.
	r.stats = TemporalStats{}
	return r, nil
}

func (r *TemporalRollup) ensureKeyTree(key string) *temporalTree {
	st := &r.stats
	if node, ok := r.keys.get(key, lessString, st); ok {
		return node.val
	}
	tree := newTemporalTree()
	r.keys.insert(key, tree, lessString, st)
	return tree
}

func (r *TemporalRollup) keyTree(key string, st *TemporalStats) *temporalTree {
	if node, ok := r.keys.get(key, lessString, st); ok {
		return node.val
	}
	return nil
}

func (r *TemporalRollup) groupForLabel(label string) groupID {
	if gid, ok := r.labelToGroup[label]; ok {
		return gid
	}
	copy := label
	g := &labelGroup{label: &copy, entities: map[int]int{}}
	r.groups = append(r.groups, g)
	gid := groupID(len(r.groups) - 1)
	r.labelToGroup[copy] = gid
	return gid
}

// storeNewFact indexes a fact that has no prior contribution and adds it to
// its governing group. It is used at construction time, where no batch ledger
// exists.
func (r *TemporalRollup) storeNewFact(f *factRecord) {
	st := &r.stats
	tree := r.ensureKeyTree(f.key)
	gid := tree.classifyAt(f.at, st)
	stored := r.indexFact(tree, f)
	r.groups[gid].add(stored, st)
}

// indexFact installs a fact in its key tree and the fact map without touching
// any group aggregate. Aggregate movement inside Apply is handled exactly once
// in the finalize ledger.
func (r *TemporalRollup) indexFact(tree *temporalTree, f *factRecord) *factRecord {
	stored := *f
	tree.facts.insert(factPoint{at: f.at, id: f.id}, &stored, lessFactPoint, &r.stats)
	r.facts[f.id] = &stored
	return &stored
}

// ---------- Group contribution primitives ----------

func (g *labelGroup) add(f *factRecord, st *TemporalStats) {
	g.rows++
	g.sum += f.amount
	g.entities[f.entity]++
	st.AggregateOps++
}

func (g *labelGroup) withdraw(f *factRecord, st *TemporalStats) {
	g.rows--
	g.sum -= f.amount
	if n := g.entities[f.entity]; n <= 1 {
		delete(g.entities, f.entity)
	} else {
		g.entities[f.entity] = n - 1
	}
	st.AggregateOps++
}

// ---------- Apply ----------

// Apply performs one joint correction batch. Deletions are applied first, then
// upserts by identity; deleting and re-upserting the same identity in one
// batch replaces it. Every record ends up associated according to the final
// state of the batch, independent of input ordering. The returned frame is the
// net differential against the Snapshot before the call, with columns
// label, d_rows, d_sum, d_entities.
//
// On validation failure the rollup keeps its previous data and indices
// untouched, the returned QFrame carries Err and a non-nil error is returned;
// a later, valid batch still works.
func (r *TemporalRollup) Apply(factUpserts, versionUpserts QFrame, factDeletes []int, versionDeletes []VersionKey) (QFrame, error) {
	r.stats = TemporalStats{}
	st := &r.stats

	// ---- Validation phase: no stored state is mutated before this point. ----
	factRecs, err := extractFacts(factUpserts)
	if err != nil {
		return QFrame{Err: err}, err
	}
	versionRecs, err := extractVersions(versionUpserts)
	if err != nil {
		return QFrame{Err: err}, err
	}

	seenFactIDs := map[int]struct{}{}
	for _, f := range factRecs {
		if _, dup := seenFactIDs[f.id]; dup {
			err = qerrors.New("Apply", "duplicate fact id %d in upsert input", f.id)
			return QFrame{Err: err}, err
		}
		seenFactIDs[f.id] = struct{}{}
	}
	seenVersionIDs := map[versionIdentity]struct{}{}
	for _, v := range versionRecs {
		id := versionIdentity{key: v.key, from: v.from}
		if _, dup := seenVersionIDs[id]; dup {
			err = qerrors.New("Apply", "duplicate version identity (key=%q, from=%d) in upsert input", v.key, v.from)
			return QFrame{Err: err}, err
		}
		seenVersionIDs[id] = struct{}{}
	}

	// Before-state is captured lazily for the groups that are actually
	// mutated, so an Apply never enumerates groups its batch does not touch.
	before := map[groupID]groupState{}
	recordBefore := func(gid groupID) {
		if _, ok := before[gid]; ok {
			return
		}
		g := r.groups[gid]
		before[gid] = groupState{rows: g.rows, sum: g.sum, entities: len(g.entities)}
	}
	withdrawFrom := func(gid groupID, f *factRecord) {
		recordBefore(gid)
		r.groups[gid].withdraw(f, st)
	}
	addTo := func(gid groupID, f *factRecord) {
		recordBefore(gid)
		r.groups[gid].add(f, st)
	}

	// ---- Planning phase ----------------------------------------------------
	// Every fact whose contribution may change is captured here while all
	// indices still hold the pre-batch state, so oldGroup is always the group
	// the fact contributed before the batch. No tree is mutated in this phase.
	type involvement struct {
		oldRecord factRecord
		oldGroup  groupID
	}
	led := map[int]*involvement{}

	capture := func(fid int, f *factRecord) {
		if _, ok := led[fid]; ok {
			return
		}
		tree := r.keyTree(f.key, st)
		og := idNull
		if tree != nil {
			og = tree.classifyAt(f.at, st)
		}
		led[fid] = &involvement{oldRecord: *f, oldGroup: og}
		// Every distinct stored event reached for validation, location, an
		// interval sweep or a direct correction is counted once.
		st.FactVisits++
	}

	sweep := func(tree *temporalTree, lo int, hi *int) {
		belowHi := func(p factPoint) bool {
			if hi == nil {
				return true // unbounded final interval
			}
			return p.at < *hi
		}
		tree.facts.visitRange(factPoint{at: lo, id: math.MinInt}, lessFactPoint, belowHi, st,
			func(_ factPoint, f *factRecord) {
				capture(f.id, f)
			})
	}

	// Version edit plan, deduplicated by identity. A delete and an upsert of
	// the same (key, from) in one batch means "replace this version": the
	// upsert wins and both the old and the new adjacent spans are swept.
	type versionEdit struct {
		rec     versionRecord
		deleted bool
		upsert  bool
	}
	versionEdits := map[versionIdentity]*versionEdit{}
	planVersion := func(id versionIdentity) *versionEdit {
		e := versionEdits[id]
		if e == nil {
			e = &versionEdit{}
			versionEdits[id] = e
		}
		return e
	}
	for _, dk := range versionDeletes {
		id := versionIdentity{key: dk.Key, from: dk.From}
		planVersion(id).deleted = true
	}
	for i := range versionRecs {
		id := versionIdentity{key: versionRecs[i].key, from: versionRecs[i].from}
		e := planVersion(id)
		e.upsert = true
		e.rec = versionRecs[i]
	}

	// Collect every affected span per key against the untouched timelines,
	// then merge overlapping spans so a fact covered by several adjacent edits
	// is visited at most once. Without merging, dense edits on one key could
	// re-sweep the same facts again and again and break the O(A) visit bound.
	type span struct {
		lo int
		hi *int // nil means the unbounded final interval
	}
	spansByKey := map[string][]span{}
	addSpan := func(key string, lo int, hi *int) {
		spansByKey[key] = append(spansByKey[key], span{lo: lo, hi: hi})
	}
	for id, e := range versionEdits {
		tree := r.keyTree(id.key, st)
		if tree == nil {
			continue // brand new key: no stored facts can be dominated yet
		}
		if e.deleted {
			// A delete of a missing identity is a no-op and contributes no span.
			if _, ok := tree.versions.get(id.from, lessInt, st); ok {
				addSpan(id.key, id.from, tree.successorFrom(id.from, st))
			}
		}
		if e.upsert {
			// Independent of the delete side: replacing a missing version must
			// still sweep the (pre-batch) interval the new version will own.
			addSpan(id.key, id.from, tree.successorFrom(id.from, st))
		}
	}
	for key, spans := range spansByKey {
		sort.Slice(spans, func(a, b int) bool { return spans[a].lo < spans[b].lo })
		merged := make([]span, 0, len(spans))
		for _, s := range spans {
			if len(merged) == 0 {
				merged = append(merged, s)
				continue
			}
			last := &merged[len(merged)-1]
			if last.hi == nil {
				continue // already unbounded
			}
			if s.lo <= *last.hi {
				if s.hi == nil || *s.hi > *last.hi {
					last.hi = s.hi
				}
			} else {
				merged = append(merged, s)
			}
		}
		tree := r.keyTree(key, st)
		for _, s := range merged {
			sweep(tree, s.lo, s.hi)
		}
	}

	// Fact deletions and directly corrected facts are captured against the
	// pre-batch trees as well. The visit itself is counted inside capture.
	factRemoved := map[int]struct{}{}
	for _, id := range factDeletes {
		if _, done := factRemoved[id]; done {
			continue
		}
		f, ok := r.facts[id]
		if !ok {
			continue
		}
		factRemoved[id] = struct{}{}
		capture(id, f)
	}
	for i := range factRecs {
		if old, ok := r.facts[factRecs[i].id]; ok {
			capture(old.id, old)
		}
	}

	// ---- Mutation phase ----------------------------------------------------
	// From here on classifications answer the evolving, final timelines; the
	// pre-batch contributions are already safely held in the ledger.

	// 1. Version deletions and upserts.
	for id, e := range versionEdits {
		tree := r.ensureKeyTree(id.key)
		if e.deleted && !e.upsert {
			if tree.versions.delete(id.from, lessInt, st) {
				delete(r.versions, id)
			}
			continue
		}
		gid := r.groupForLabel(e.rec.label)
		tree.versions.insert(id.from, gid, lessInt, st)
		r.versions[id] = struct{}{}
	}

	// 2. Fact deletions.
	for id := range factRemoved {
		f := r.facts[id]
		tree := r.keyTree(f.key, st)
		tree.facts.delete(factPoint{at: f.at, id: f.id}, lessFactPoint, st)
		delete(r.facts, id)
	}

	// 3. Fact upserts; pure inserts are remembered for their single addition.
	pureInserts := map[int]struct{}{}
	for i := range factRecs {
		f := factRecs[i]
		if old, ok := r.facts[f.id]; ok {
			oldTree := r.keyTree(old.key, st)
			oldTree.facts.delete(factPoint{at: old.at, id: old.id}, lessFactPoint, st)
			delete(r.facts, old.id)
		} else if _, inLedger := led[f.id]; !inLedger {
			// A fact deleted earlier in this batch and now upserted is already
			// ledgered; do not add it twice.
			pureInserts[f.id] = struct{}{}
		}
		finalTree := r.ensureKeyTree(f.key)
		r.indexFact(finalTree, &factRecs[i])
	}

	// 4. Single finalize pass: every involved stored fact is withdrawn at most
	//    once and added at most once, classified against the final timelines.
	//    A fact that leaves a group and returns to it, or changes only fields
	//    irrelevant to its membership, therefore produces zero net movement.
	for fid, inv := range led {
		final, ok := r.facts[fid]
		if !ok {
			// Fact left the store entirely.
			withdrawFrom(inv.oldGroup, &inv.oldRecord)
			continue
		}
		finalTree := r.keyTree(final.key, st)
		newGroup := finalTree.classifyAt(final.at, st)
		if inv.oldGroup == newGroup && inv.oldRecord == *final {
			continue // swept or replaced without any contribution change
		}
		withdrawFrom(inv.oldGroup, &inv.oldRecord)
		addTo(newGroup, final)
	}
	for fid := range pureInserts {
		final := r.facts[fid]
		finalTree := r.keyTree(final.key, st)
		newGroup := finalTree.classifyAt(final.at, st)
		addTo(newGroup, final)
	}

	return r.buildDifferential(before), nil
}

type groupState struct {
	rows     int
	sum      int
	entities int
}

func (r *TemporalRollup) buildDifferential(before map[groupID]groupState) QFrame {
	type row struct {
		label             *string
		dRows, dSum, dEnt int
	}
	rows := make([]row, 0, len(before))
	// Only groups that actually changed aggregate state were recorded.
	for gid, b := range before {
		g := r.groups[gid]
		dr := g.rows - b.rows
		ds := g.sum - b.sum
		de := len(g.entities) - b.entities
		if dr == 0 && ds == 0 && de == 0 {
			continue
		}
		rows = append(rows, row{label: g.label, dRows: dr, dSum: ds, dEnt: de})
	}
	sort.SliceStable(rows, func(a, b int) bool {
		la, lb := rows[a].label, rows[b].label
		if la == nil || lb == nil {
			return la == nil && lb != nil
		}
		return *la < *lb
	})

	labels := make([]*string, len(rows))
	dRows := make([]int, len(rows))
	dSum := make([]int, len(rows))
	dEnt := make([]int, len(rows))
	for i, x := range rows {
		if x.label != nil {
			s := *x.label
			labels[i] = &s
		}
		dRows[i], dSum[i], dEnt[i] = x.dRows, x.dSum, x.dEnt
	}
	return newGroupFrame([4]string{"label", "d_rows", "d_sum", "d_entities"}, labels, dRows, dSum, dEnt)
}

// Snapshot returns the current rollup with columns label, rows, sum and
// entities. Only groups that still hold events are emitted; a group whose sum
// is zero but that still has events is kept. The NULL label sorts first and
// the remaining labels follow Go string order. An empty result keeps the
// declared column types.
func (r *TemporalRollup) Snapshot() QFrame {
	ids := make([]groupID, 0, len(r.groups))
	for id, g := range r.groups {
		if g.rows > 0 {
			ids = append(ids, groupID(id))
		}
	}
	sort.SliceStable(ids, func(a, b int) bool {
		la, lb := r.groups[ids[a]].label, r.groups[ids[b]].label
		if la == nil || lb == nil {
			return la == nil && lb != nil
		}
		return *la < *lb
	})

	labels := make([]*string, 0, len(ids))
	rows := make([]int, 0, len(ids))
	sums := make([]int, 0, len(ids))
	entities := make([]int, 0, len(ids))
	for _, id := range ids {
		g := r.groups[id]
		labels = append(labels, g.label)
		rows = append(rows, g.rows)
		sums = append(sums, g.sum)
		entities = append(entities, len(g.entities))
	}
	return newGroupFrame([4]string{"label", "rows", "sum", "entities"}, labels, rows, sums, entities)
}

// Stats returns the work counters of the most recent Apply call.
func (r *TemporalRollup) Stats() TemporalStats {
	return r.stats
}

func newGroupFrame(names [4]string, labels []*string, c1, c2, c3 []int) QFrame {
	// Every returned frame owns fresh slices and fresh string pointers; no
	// mutable internal storage ever escapes through a Snapshot or differential.
	labelCopy := make([]*string, len(labels))
	for i, p := range labels {
		if p != nil {
			s := *p
			labelCopy[i] = &s
		}
	}
	return New(map[string]types.DataSlice{
		names[0]: labelCopy,
		names[1]: append([]int(nil), c1...),
		names[2]: append([]int(nil), c2...),
		names[3]: append([]int(nil), c3...),
	}, newqf.ColumnOrder(names[0], names[1], names[2], names[3]))
}

// ---------- Input extraction and validation ----------

func checkFrameErr(qf QFrame, op string) error {
	if qf.Err != nil {
		return qerrors.Propagate(op, qf.Err)
	}
	return nil
}

func requireColumn(qf QFrame, name string, allowed ...types.DataType) (namedColumn, error) {
	col, ok := qf.columnsByName[name]
	if !ok {
		return namedColumn{}, qerrors.New("TemporalRollup", "missing required column %q", name)
	}
	got := col.DataType()
	for _, t := range allowed {
		if got == t {
			return col, nil
		}
	}
	return namedColumn{}, qerrors.New("TemporalRollup", "column %q has type %s, expected one of %v", name, got, allowed)
}

// stringLikeColumn returns an accessor over the currently visible rows of a
// string or enum column. Enum values are read by their text and copied, so
// inputs with different enum dictionaries are interchangeable and no input
// pointer is retained.
func stringLikeColumn(qf QFrame, name string) (func(i int) *string, error) {
	if col, err := requireColumn(qf, name, types.String); err == nil {
		sc := col.Column.(scolumn.Column)
		view := sc.View(qf.index)
		return func(i int) *string {
			p := view.ItemAt(i)
			if p == nil {
				return nil
			}
			s := *p
			return &s
		}, nil
	}
	col, err := requireColumn(qf, name, types.Enum)
	if err != nil {
		return nil, qerrors.New("TemporalRollup", "column %q must be a non-null string or enum column", name)
	}
	ec := col.Column.(ecolumn.Column)
	view := ec.View(qf.index)
	return func(i int) *string {
		p := view.ItemAt(i)
		if p == nil {
			return nil
		}
		s := *p
		return &s
	}, nil
}

func intColumnView(qf QFrame, name string) (func(i int) int, error) {
	col, err := requireColumn(qf, name, types.Int)
	if err != nil {
		return nil, err
	}
	ic := col.Column.(icolumn.Column)
	view := ic.View(qf.index)
	return func(i int) int { return view.ItemAt(i) }, nil
}

func extractFacts(qf QFrame) ([]factRecord, error) {
	if err := checkFrameErr(qf, "TemporalRollup facts"); err != nil {
		return nil, err
	}
	idFn, err := intColumnView(qf, "id")
	if err != nil {
		return nil, err
	}
	atFn, err := intColumnView(qf, "at")
	if err != nil {
		return nil, err
	}
	amountFn, err := intColumnView(qf, "amount")
	if err != nil {
		return nil, err
	}
	entityFn, err := intColumnView(qf, "entity")
	if err != nil {
		return nil, err
	}
	keyFn, err := stringLikeColumn(qf, "key")
	if err != nil {
		return nil, err
	}

	recs := make([]factRecord, 0, qf.Len())
	for i := 0; i < qf.Len(); i++ {
		key := keyFn(i)
		if key == nil {
			return nil, qerrors.New("TemporalRollup", "fact key must not be NULL")
		}
		recs = append(recs, factRecord{
			id:     idFn(i),
			key:    *key,
			at:     atFn(i),
			amount: amountFn(i),
			entity: entityFn(i),
		})
	}
	return recs, nil
}

func extractVersions(qf QFrame) ([]versionRecord, error) {
	if err := checkFrameErr(qf, "TemporalRollup versions"); err != nil {
		return nil, err
	}
	fromFn, err := intColumnView(qf, "from")
	if err != nil {
		return nil, err
	}
	keyFn, err := stringLikeColumn(qf, "key")
	if err != nil {
		return nil, err
	}
	labelFn, err := stringLikeColumn(qf, "label")
	if err != nil {
		return nil, err
	}

	recs := make([]versionRecord, 0, qf.Len())
	for i := 0; i < qf.Len(); i++ {
		key := keyFn(i)
		if key == nil {
			return nil, qerrors.New("TemporalRollup", "version key must not be NULL")
		}
		label := labelFn(i)
		if label == nil {
			return nil, qerrors.New("TemporalRollup", "version label must not be NULL, the NULL label is reserved for unmatched facts")
		}
		recs = append(recs, versionRecord{key: *key, from: fromFn(i), label: *label})
	}
	return recs, nil
}
