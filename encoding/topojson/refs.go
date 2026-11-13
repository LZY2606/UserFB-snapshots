package topojson

import (
	"encoding/json"
)

// buildObject converts a validated plan into its TopoJSON object, translating
// every quantized path into ordered, direction-aware arc references.
func (b *builder) buildObject(
	p *plan, edgeLookup map[edgeKey]*graphEdge, breakpoints map[iPoint]bool,
) (*topoObject, error) {
	obj := &topoObject{Type: p.typ}
	switch p.typ {
	case "LineString":
		if len(p.rings) == 0 {
			return obj, nil
		}
		refs, err := b.ringReferences(p.rings[0], false, edgeLookup, breakpoints)
		if err != nil {
			return nil, err
		}
		obj.Arcs = mustMarshalArcs(refs)
	case "MultiLineString":
		if len(p.rings) == 0 {
			return obj, nil
		}
		groups := make([][]int, 0, len(p.rings))
		for _, r := range p.rings {
			refs, err := b.ringReferences(r, false, edgeLookup, breakpoints)
			if err != nil {
				return nil, err
			}
			if refs == nil {
				refs = []int{}
			}
			groups = append(groups, refs)
		}
		obj.Arcs = mustMarshalArcs(groups)
	case "Polygon":
		if len(p.rings) == 0 {
			return obj, nil
		}
		groups := make([][]int, 0, len(p.rings))
		for _, r := range p.rings {
			refs, err := b.ringReferences(r, true, edgeLookup, breakpoints)
			if err != nil {
				return nil, err
			}
			groups = append(groups, refs)
		}
		obj.Arcs = mustMarshalArcs(groups)
	case "MultiPolygon":
		if len(p.polygons) == 0 {
			return obj, nil
		}
		polygons := make([][][]int, 0, len(p.polygons))
		for _, rings := range p.polygons {
			groups := make([][]int, 0, len(rings))
			for _, r := range rings {
				refs, err := b.ringReferences(r, true, edgeLookup, breakpoints)
				if err != nil {
					return nil, err
				}
				groups = append(groups, refs)
			}
			polygons = append(polygons, groups)
		}
		obj.Arcs = mustMarshalArcs(polygons)
	case "GeometryCollection":
		obj.Geometries = make([]*topoObject, len(p.geoms))
		for i, child := range p.geoms {
			childObj, err := b.buildObject(child, edgeLookup, breakpoints)
			if err != nil {
				return nil, err
			}
			obj.Geometries[i] = childObj
		}
	}
	return obj, nil
}

func mustMarshalArcs(v any) json.RawMessage {
	data, err := json.Marshal(v)
	if err != nil {
		// Only slices of integers are marshalled, which cannot fail.
		panic(err)
	}
	return data
}

// expandPoints returns the grid vertices actually traversed by a path,
// inserting the same interior input vertices the topology graph was split at.
// Consecutive duplicates are merged. Closed rings return their cyclic vertex
// list once, without repeating the first point at the end.
func (b *builder) expandPoints(r ring, closed bool) []iPoint {
	var pts []iPoint
	add := func(p iPoint) {
		if len(pts) == 0 || pts[len(pts)-1] != p {
			pts = append(pts, p)
		}
	}
	for i := 0; i+1 < len(r); i++ {
		from, to := r[i], r[i+1]
		add(from)
		for _, mid := range b.splitPoints(from, to) {
			add(mid)
		}
	}
	if !closed {
		add(r[len(r)-1])
	}
	return pts
}

// ringReferences maps one quantized path onto whole maximal arcs. The path is
// cut at graph breakpoints; closed rings are cyclically rotated to start at a
// breakpoint, which preserves their traversal direction. Reverse traversal of
// an arc uses the bitwise complement of its index, never a negative index.
func (b *builder) ringReferences(
	r ring, closed bool, edgeLookup map[edgeKey]*graphEdge, breakpoints map[iPoint]bool,
) ([]int, error) {
	if len(r) == 0 {
		return nil, nil
	}
	pts := b.expandPoints(r, closed)
	if len(pts) < 2 {
		return nil, errInvalid("internal error: quantized path has no edges")
	}

	if closed {
		first := -1
		for i, p := range pts {
			if breakpoints[p] {
				first = i
				break
			}
		}
		if first > 0 {
			rotated := make([]iPoint, len(pts))
			copy(rotated, pts[first:])
			copy(rotated[len(pts)-first:], pts[:first])
			pts = rotated
		}
	}

	edgeCount := len(pts) - 1
	if closed {
		edgeCount = len(pts)
	}

	var refs []int
	runRef := 0
	haveRun := false
	for i := 0; i < edgeCount; i++ {
		from := pts[i]
		var to iPoint
		if closed {
			to = pts[(i+1)%len(pts)]
		} else {
			to = pts[i+1]
		}
		if from == to {
			continue
		}
		edge, ok := edgeLookup[makeEdgeKey(from, to)]
		if !ok {
			return nil, errInvalid("internal error: quantized edge missing from topology graph")
		}
		ref := edge.arcID
		if edge.storedFrom == to && edge.storedTo == from {
			ref = ^edge.arcID
		}
		if !haveRun {
			runRef, haveRun = ref, true
		} else if ref != runRef {
			refs = append(refs, runRef)
			runRef = ref
		}
	}
	if haveRun {
		refs = append(refs, runRef)
	}
	if len(refs) == 0 {
		return nil, errInvalid("internal error: path produced no arc references")
	}
	return refs, nil
}
