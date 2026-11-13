package topojson

import (
	"sort"
)

// iPoint is a quantized grid vertex.
type iPoint struct {
	X int64
	Y int64
}

// edgeKey identifies an undirected small edge by its endpoint grid points.
type edgeKey struct {
	from iPoint
	to   iPoint
}

func makeEdgeKey(a, b iPoint) edgeKey {
	if comparePoints(a, b) > 0 {
		a, b = b, a
	}
	return edgeKey{from: a, to: b}
}

// graphEdge is one deduplicated undirected small edge, remembered in the
// direction in which its canonical arc stores it.
type graphEdge struct {
	arcID      int
	storedFrom iPoint
	storedTo   iPoint
}

// segment is one input edge before lattice splitting.
type segment struct {
	from iPoint
	to   iPoint
}

// comparePoints compares two grid points lexicographically on X then Y.
func comparePoints(a, b iPoint) int {
	if a.X != b.X {
		if a.X < b.X {
			return -1
		}
		return 1
	}
	if a.Y != b.Y {
		if a.Y < b.Y {
			return -1
		}
		return 1
	}
	return 0
}

// collectSegments appends every edge of p to segments. Closed rings include
// the edge back to their first point; open lines do not. lineEndpoints records
// only the overall first and last point of each open line.
func collectSegments(p *plan, segments *[]segment, lineEndpoints map[iPoint]struct{}) {
	switch p.typ {
	case "LineString", "MultiLineString":
		for _, r := range p.rings {
			collectRingSegments(r, segments)
			if len(r) >= 2 {
				lineEndpoints[r[0]] = struct{}{}
				lineEndpoints[r[len(r)-1]] = struct{}{}
			}
		}
	case "Polygon":
		for _, r := range p.rings {
			collectRingSegments(r, segments)
		}
	case "MultiPolygon":
		for _, rings := range p.polygons {
			for _, r := range rings {
				collectRingSegments(r, segments)
			}
		}
	case "GeometryCollection":
		for _, child := range p.geoms {
			collectSegments(child, segments, lineEndpoints)
		}
	}
}

func collectRingSegments(r ring, segments *[]segment) {
	if len(r) < 2 {
		return
	}
	for i := 0; i+1 < len(r); i++ {
		if r[i] == r[i+1] {
			continue
		}
		*segments = append(*segments, segment{from: r[i], to: r[i+1]})
	}
}

// latticePoints returns every grid point strictly between a and b. The
// enumeration uses exact integer arithmetic: the primitive grid step is the
// segment vector divided by the gcd of its coordinate deltas.
func latticePoints(a, b iPoint) []iPoint {
	dx := b.X - a.X
	dy := b.Y - a.Y
	steps := gcd(iabs(dx), iabs(dy))
	if steps <= 1 {
		return nil
	}
	points := make([]iPoint, 0, steps-1)
	sx, sy := dx/steps, dy/steps
	for k := int64(1); k < steps; k++ {
		points = append(points, iPoint{X: a.X + k*sx, Y: a.Y + k*sy})
	}
	return points
}

func gcd(a, b int64) int64 {
	for b != 0 {
		a, b = b, a%b
	}
	return a
}

func iabs(v int64) int64 {
	if v < 0 {
		return -v
	}
	return v
}

// buildTopology splits every segment at its interior lattice points, builds
// the deduplicated undirected graph and extracts the canonical arcs. It
// returns the arcs as absolute grid sequences, a lookup from each small edge
// to the arc that contains it and the set of graph breakpoints.
func (b *builder) buildTopology(plans []*plan) ([][]iPoint, map[edgeKey]*graphEdge, map[iPoint]bool, error) {
	var segments []segment
	lineEndpoints := make(map[iPoint]struct{})
	for _, p := range plans {
		collectSegments(p, &segments, lineEndpoints)
	}
	if len(segments) == 0 {
		b.setTransform()
		return nil, make(map[edgeKey]*graphEdge), make(map[iPoint]bool), nil
	}

	edgeSet := make(map[edgeKey]struct{})
	endpoints := lineEndpoints
	addEdge := func(a, c iPoint) {
		if a == c {
			return
		}
		edgeSet[makeEdgeKey(a, c)] = struct{}{}
	}

	for _, s := range segments {
		interior := b.splitPoints(s.from, s.to)
		prev := s.from
		for _, p := range interior {
			addEdge(prev, p)
			prev = p
		}
		addEdge(prev, s.to)
	}

	adjacency := make(map[iPoint][]iPoint)
	for key := range edgeSet {
		adjacency[key.from] = append(adjacency[key.from], key.to)
		adjacency[key.to] = append(adjacency[key.to], key.from)
	}
	for id := range adjacency {
		sort.Slice(adjacency[id], func(i, j int) bool {
			return comparePoints(adjacency[id][i], adjacency[id][j]) < 0
		})
	}

	isBreakpoint := func(p iPoint) bool {
		if len(adjacency[p]) != 2 {
			return true
		}
		_, isEndpoint := endpoints[p]
		return isEndpoint
	}

	usedEdges := make(map[edgeKey]bool)
	type walkedArc struct {
		points []iPoint
		closed bool
	}
	var walked []walkedArc

	// Open chains: start at every breakpoint and walk each incident edge
	// through degree-two vertices until the next breakpoint.
	breakpoints := make([]iPoint, 0)
	for p := range adjacency {
		if isBreakpoint(p) {
			breakpoints = append(breakpoints, p)
		}
	}
	sort.Slice(breakpoints, func(i, j int) bool {
		return comparePoints(breakpoints[i], breakpoints[j]) < 0
	})
	for _, start := range breakpoints {
		for _, next := range adjacency[start] {
			key := makeEdgeKey(start, next)
			if usedEdges[key] {
				continue
			}
			usedEdges[key] = true
			path := []iPoint{start, next}
			prev, cur := start, next
			for !isBreakpoint(cur) {
				neighbors := adjacency[cur]
				following := neighbors[0]
				if following == prev {
					following = neighbors[1]
				}
				stepKey := makeEdgeKey(cur, following)
				if usedEdges[stepKey] {
					break
				}
				usedEdges[stepKey] = true
				path = append(path, following)
				prev, cur = cur, following
			}
			// An open chain is canonicalized directionally even if its end
			// happens to coincide with its start.
			walked = append(walked, walkedArc{points: path, closed: false})
		}
	}

	// Remaining edges form standalone closed loops whose vertices all have
	// degree two. Each loop becomes one arc rather than one arc per edge.
	loopKeys := make([]edgeKey, 0)
	for key := range edgeSet {
		if !usedEdges[key] {
			loopKeys = append(loopKeys, key)
		}
	}
	sort.Slice(loopKeys, func(i, j int) bool {
		if c := comparePoints(loopKeys[i].from, loopKeys[j].from); c != 0 {
			return c < 0
		}
		return comparePoints(loopKeys[i].to, loopKeys[j].to) < 0
	})
	for _, key := range loopKeys {
		if usedEdges[key] {
			continue
		}
		start := key.from
		prev, cur := start, key.to
		usedEdges[key] = true
		path := []iPoint{start, cur}
		for cur != start {
			neighbors := adjacency[cur]
			following := neighbors[0]
			if following == prev {
				following = neighbors[1]
			}
			stepKey := makeEdgeKey(cur, following)
			usedEdges[stepKey] = true
			path = append(path, following)
			prev, cur = cur, following
		}
		walked = append(walked, walkedArc{points: path, closed: true})
	}

	// Canonicalize each arc's stored direction independently of how it was
	// walked, then number the arcs lexicographically.
	type canonicalArc struct {
		points []iPoint
		closed bool
	}
	canonical := make([]canonicalArc, len(walked))
	for i, w := range walked {
		canonical[i] = canonicalArc{
			points: canonicalSequence(w.points, w.closed),
			closed: w.closed,
		}
	}
	sort.Slice(canonical, func(i, j int) bool {
		return comparePointSequences(canonical[i].points, canonical[j].points) < 0
	})

	breakpointSet := make(map[iPoint]bool, len(breakpoints))
	for _, p := range breakpoints {
		breakpointSet[p] = true
	}

	arcs := make([][]iPoint, len(canonical))
	lookup := make(map[edgeKey]*graphEdge, len(edgeSet))
	for arcID, c := range canonical {
		arcs[arcID] = c.points
		for i := 0; i+1 < len(c.points); i++ {
			from, to := c.points[i], c.points[i+1]
			lookup[makeEdgeKey(from, to)] = &graphEdge{
				arcID:      arcID,
				storedFrom: from,
				storedTo:   to,
			}
		}
	}

	b.setTransform()
	return arcs, lookup, breakpointSet, nil
}

// canonicalSequence returns the lexicographically smallest representation of
// an arc: both directions for an open arc, and every cyclic start in both
// directions for a closed loop. Closed loops keep their repeated first point
// at the end.
func canonicalSequence(seq []iPoint, closed bool) []iPoint {
	if !closed {
		reversed := reversePoints(seq)
		if comparePointSequences(reversed, seq) < 0 {
			return reversed
		}
		return append([]iPoint(nil), seq...)
	}

	open := seq[:len(seq)-1]
	best := make([]iPoint, len(open)+1)
	copy(best, open)
	best[len(open)] = open[0]
	candidate := make([]iPoint, len(open)+1)
	for start := range open {
		for direction := 0; direction < 2; direction++ {
			for i := range open {
				var p iPoint
				if direction == 0 {
					p = open[(start+i)%len(open)]
				} else {
					p = open[(start-i+len(open))%len(open)]
				}
				candidate[i] = p
			}
			candidate[len(open)] = candidate[0]
			if comparePointSequences(candidate, best) < 0 {
				best = append([]iPoint(nil), candidate...)
			}
		}
	}
	return best
}

func reversePoints(seq []iPoint) []iPoint {
	out := make([]iPoint, len(seq))
	for i, p := range seq {
		out[len(seq)-1-i] = p
	}
	return out
}

// comparePointSequences compares two point sequences lexicographically.
func comparePointSequences(a, b []iPoint) int {
	n := min(len(a), len(b))
	for i := range n {
		if c := comparePoints(a[i], b[i]); c != 0 {
			return c
		}
	}
	switch {
	case len(a) < len(b):
		return -1
	case len(a) > len(b):
		return 1
	default:
		return 0
	}
}

// deltaEncode converts an absolute grid arc to TopoJSON delta-encoded
// positions; the first point is relative to [0, 0] and every arc restarts the
// delta accumulation.
func deltaEncode(arc []iPoint) [][2]int64 {
	out := make([][2]int64, len(arc))
	var prevX, prevY int64
	for i, p := range arc {
		out[i] = [2]int64{p.X - prevX, p.Y - prevY}
		prevX, prevY = p.X, p.Y
	}
	return out
}
