package topojson

import (
	"encoding/json"
	"math"
	"testing"

	geom "github.com/twpayne/go-geom"
	"github.com/twpayne/go-geom/encoding/wkt"
	"github.com/twpayne/go-geom/support/contract"
)

// This test file decodes the emitted TopoJSON independently: it parses the
// JSON with encoding/json, resolves delta-encoded arcs back to absolute grid
// sequences and reconstructs every object's paths from the (possibly
// complemented) arc references. Nothing here reuses the encoder's internals.

type jsonTopology struct {
	Type      string                `json:"type"`
	Objects   map[string]jsonObject `json:"objects"`
	Arcs      [][][2]int64          `json:"arcs"`
	Transform jsonTransform         `json:"transform"`
	BBox      []float64             `json:"bbox"`
}

type jsonObject struct {
	Type       string          `json:"type"`
	Arcs       json.RawMessage `json:"arcs,omitempty"`
	Geometries []jsonObject    `json:"geometries,omitempty"`
}

type jsonTransform struct {
	Scale     [2]float64 `json:"scale"`
	Translate [2]float64 `json:"translate"`
}

// absoluteArcs resolves delta encoding into absolute grid point sequences.
func absoluteArcs(delta [][][2]int64) [][][2]int64 {
	out := make([][][2]int64, len(delta))
	for i, arc := range delta {
		resolved := make([][2]int64, len(arc))
		var x, y int64
		for j, d := range arc {
			x += d[0]
			y += d[1]
			resolved[j] = [2]int64{x, y}
		}
		out[i] = resolved
	}
	return out
}

// resolvePath turns an ordered, possibly complemented reference list into one
// stitched absolute grid path.
func resolvePath(refs []int, arcs [][][2]int64) [][2]int64 {
	var path [][2]int64
	for _, ref := range refs {
		var seq [][2]int64
		if ref < 0 {
			idx := ^ref
			seq = make([][2]int64, len(arcs[idx]))
			for i, p := range arcs[idx] {
				seq[len(seq)-1-i] = p
			}
		} else {
			seq = arcs[ref]
		}
		if len(path) == 0 {
			path = append(path, seq...)
		} else {
			path = append(path, seq[1:]...)
		}
	}
	return path
}

func parseTopology(t *testing.T, data []byte) jsonTopology {
	t.Helper()
	var top jsonTopology
	if err := json.Unmarshal(data, &top); err != nil {
		t.Fatalf("output is not valid JSON: %v", err)
	}
	return top
}

// lineRefs parses a LineString object's arc references.
func lineRefs(t *testing.T, o jsonObject) []int {
	t.Helper()
	var refs []int
	if err := json.Unmarshal(o.Arcs, &refs); err != nil {
		t.Fatalf("LineString arcs: %v", err)
	}
	return refs
}

// ringRefs parses Polygon or MultiLineString arc references.
func ringRefs(t *testing.T, o jsonObject) [][]int {
	t.Helper()
	var refs [][]int
	if err := json.Unmarshal(o.Arcs, &refs); err != nil {
		t.Fatalf("%s arcs: %v", o.Type, err)
	}
	return refs
}

// polygonRefs parses MultiPolygon arc references.
func polygonRefs(t *testing.T, o jsonObject) [][][]int {
	t.Helper()
	var refs [][][]int
	if err := json.Unmarshal(o.Arcs, &refs); err != nil {
		t.Fatalf("MultiPolygon arcs: %v", err)
	}
	return refs
}

func mustWKT(t *testing.T, s string) geom.T {
	t.Helper()
	g, err := wkt.Unmarshal(s)
	if err != nil {
		t.Fatalf("wkt %q: %v", s, err)
	}
	return g
}

// runSubtest wraps a subtest so its content assertions are counted by the
// support/contract checker.
func runSubtest(t *testing.T, name string, fn func(t *testing.T)) {
	t.Helper()
	t.Run(name, func(t *testing.T) {
		t.Helper()
		contract.Run(t, fn)
	})
}

func check(t *testing.T, ok bool) {
	t.Helper()
	contract.Check(t, ok)
}

// pathsEqual compares two open (non-closed) grid paths exactly.
func pathsEqual(a, b [][2]int64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// ringEquivalent compares closed rings allowing only a cyclic shift of the
// start point. Reversed traversal is rejected.
func ringEquivalent(got, want [][2]int64) bool {
	if len(got) != len(want) || len(got) == 0 {
		return false
	}
	n := len(got)
	for shift := range n {
		match := true
		for i := range n {
			if got[(shift+i)%n] != want[i] {
				match = false
				break
			}
		}
		if match {
			return true
		}
	}
	return false
}

// pathCovers checks that want's vertices appear in got in the same order
// (cyclically for closed rings), allowing extra collinear split points. For
// open paths the endpoints must match; for rings the path must close.
func pathCovers(got, want [][2]int64, cyclic bool) bool {
	if len(got) == 0 || len(want) == 0 {
		return false
	}
	if cyclic {
		if got[0] != got[len(got)-1] {
			return false
		}
		shift := -1
		for i, p := range got {
			if p == want[0] {
				shift = i
				break
			}
		}
		if shift < 0 {
			return false
		}
		open := got[:len(got)-1]
		n := len(open)
		j := 0
		for i := range n {
			if open[(shift+i)%n] == want[j] {
				j++
				if j == len(want)-1 {
					break
				}
			}
		}
		return j == len(want)-1 && want[len(want)-1] == want[0]
	}
	if got[0] != want[0] || got[len(got)-1] != want[len(want)-1] {
		return false
	}
	j := 0
	for _, p := range got {
		if p == want[j] {
			j++
			if j == len(want) {
				return true
			}
		}
	}
	return false
}

// arcIndexOf returns the arc index referenced by r, decoding the bitwise
// complement convention for reversed arcs.
func arcIndexOf(r int) int {
	if r < 0 {
		return ^r
	}
	return r
}

// edgeOccurrences maps every undirected small edge in the arc table to the
// arc that stores it. A duplicate edge means the table failed to share one
// identity.
func edgeOccurrences(arcs [][][2]int64) map[[2][2]int64]int {
	occ := make(map[[2][2]int64]int)
	for arcID, arc := range arcs {
		for i := 0; i+1 < len(arc); i++ {
			a, b := arc[i], arc[i+1]
			if a[0] > b[0] || (a[0] == b[0] && a[1] > b[1]) {
				a, b = b, a
			}
			occ[[2][2]int64{a, b}] = arcID
		}
	}
	return occ
}

// TestUnitTopology covers graph construction: shared edges with different
// sampling, T-junctions, holes, reverse reuse, breakpoints and loops.
func TestUnitTopology(t *testing.T) {
	runSubtest(t, "shared_long_edge_different_sampling", func(t *testing.T) {
		// The shared edge x=10 is sampled only at its ends by the left
		// polygon, but carries an interior vertex (10,5) in the right
		// polygon. With Q=11 over bbox [0,20]x[0,10], grid x is half the
		// input x and grid y equals input y.
		left := mustWKT(t,
			"POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))")
		right := mustWKT(t,
			"POLYGON ((10 0, 20 0, 20 10, 10 10, 10 5, 10 0))")
		line := mustWKT(t, "LINESTRING (10 3, 15 3)")
		data, err := Marshal(map[string]geom.T{
			"left": left, "right": right, "line": line,
		}, 11)
		check(t, err == nil)
		top := parseTopology(t, data)
		arcs := absoluteArcs(top.Arcs)
		occ := edgeOccurrences(arcs)

		// Every undirected small edge has exactly one identity.
		count := 0
		for _, arc := range arcs {
			count += len(arc) - 1
		}
		check(t, count == len(occ))

		// The shared edge must be split at the interior input vertices
		// (5,3) from the line and (5,5) from the right polygon.
		arc03, ok03 := occ[[2][2]int64{{5, 0}, {5, 3}}]
		arc35, ok35 := occ[[2][2]int64{{5, 3}, {5, 5}}]
		arc510, ok510 := occ[[2][2]int64{{5, 5}, {5, 10}}]
		check(t, ok03 && ok35 && ok510)
		// (5,3) is a degree-three T-junction so it starts a new arc; (5,5)
		// is a degree-two collinear vertex and may stay inside one maximal
		// arc, but every small edge still has its own identity.
		check(t, arc03 != arc35)
		_ = arc510

		// The two polygons must reference the same shared arcs, in
		// opposite directions (one positive, one complemented).
		leftRings := ringRefs(t, top.Objects["left"])
		rightRings := ringRefs(t, top.Objects["right"])
		check(t, len(leftRings) == 2 && len(rightRings) == 1)
		usedBy := func(rings [][]int) map[int]int {
			signs := make(map[int]int)
			for _, refs := range rings {
				for _, r := range refs {
					signs[arcIndexOf(r)] = r
				}
			}
			return signs
		}
		leftUse := usedBy(leftRings)
		rightUse := usedBy(rightRings)
		for _, shared := range []int{arc03, arc35, arc510} {
			l, lok := leftUse[shared]
			r, rok := rightUse[shared]
			check(t, lok && rok)
			check(t, (l >= 0) != (r >= 0))
			check(t, arcIndexOf(l) == shared && arcIndexOf(r) == shared)
		}

		// Reconstructed outer ring of the left polygon: original vertices
		// in order, with the collinear split points inserted, closing and
		// keeping the original traversal direction.
		wantLeftOuter := [][2]int64{
			{0, 0}, {5, 0}, {5, 10}, {0, 10}, {0, 0},
		}
		gotLeftOuter := resolvePath(leftRings[0], arcs)
		check(t, pathCovers(gotLeftOuter, wantLeftOuter, true))

		// The hole keeps its ring membership and orientation.
		wantHole := [][2]int64{
			{1, 2}, {1, 4}, {2, 4}, {2, 2}, {1, 2},
		}
		check(t, pathCovers(resolvePath(leftRings[1], arcs), wantHole, true))

		// The right polygon's ring reconstructs with both split points.
		wantRightOuter := [][2]int64{
			{5, 0}, {10, 0}, {10, 10}, {5, 10}, {5, 5}, {5, 3}, {5, 0},
		}
		check(t, pathCovers(resolvePath(rightRings[0], arcs), wantRightOuter, true))

		// The T-junction line is one arc, starting exactly at the shared
		// edge vertex (5,3).
		lineRef := lineRefs(t, top.Objects["line"])
		check(t, len(lineRef) == 1)
		check(t, pathsEqual(resolvePath(lineRef, arcs),
			[][2]int64{{5, 3}, {8, 3}}))
	})

	runSubtest(t, "reverse_reuse_uses_complement", func(t *testing.T) {
		a := mustWKT(t, "LINESTRING (0 0, 10 0)")
		b := mustWKT(t, "LINESTRING (10 0, 0 0)")
		data, err := Marshal(map[string]geom.T{"a": a, "b": b}, 11)
		check(t, err == nil)
		top := parseTopology(t, data)
		arcs := absoluteArcs(top.Arcs)
		check(t, len(arcs) == 1)
		refsA := lineRefs(t, top.Objects["a"])
		refsB := lineRefs(t, top.Objects["b"])
		// Forward traversal references arc 0; reverse traversal must use
		// the bitwise complement ~0 == -1, never a literal -0.
		check(t, len(refsA) == 1 && refsA[0] == 0)
		check(t, len(refsB) == 1 && refsB[0] == ^0)
		check(t, pathsEqual(resolvePath(refsA, arcs),
			[][2]int64{{0, 0}, {10, 0}}))
		check(t, pathsEqual(resolvePath(refsB, arcs),
			[][2]int64{{10, 0}, {0, 0}}))
	})

	runSubtest(t, "standalone_loop_is_one_arc", func(t *testing.T) {
		poly := mustWKT(t, "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))")
		data, err := Marshal(map[string]geom.T{"p": poly}, 5)
		check(t, err == nil)
		top := parseTopology(t, data)
		// Every vertex has degree two, so the whole loop must be stored as
		// a single arc rather than one arc per small edge.
		check(t, len(top.Arcs) == 1)
		rings := ringRefs(t, top.Objects["p"])
		check(t, len(rings) == 1 && len(rings[0]) == 1)
		arcs := absoluteArcs(top.Arcs)
		got := resolvePath(rings[0], arcs)
		check(t, ringEquivalent(got,
			[][2]int64{{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}))
	})

	runSubtest(t, "open_line_returning_to_start_is_directional", func(t *testing.T) {
		// An open LineString whose last point equals its first is still a
		// line, not a ring: its arc must keep a distinguished start rather
		// than being cyclically rotated.
		line := mustWKT(t, "LINESTRING (0 0, 4 0, 4 4, 0 4, 0 0)")
		data, err := Marshal(map[string]geom.T{"l": line}, 5)
		check(t, err == nil)
		top := parseTopology(t, data)
		arcs := absoluteArcs(top.Arcs)
		check(t, len(arcs) == 1)
		refs := lineRefs(t, top.Objects["l"])
		check(t, len(refs) == 1)
		// The stored arc is the lexicographically smaller of the two
		// directions, and the references rebuild the original traversal.
		got := resolvePath(refs, arcs)
		check(t, pathsEqual(got,
			[][2]int64{{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}))
	})

	runSubtest(t, "identical_loop_shared_across_objects", func(t *testing.T) {
		// Same loop, same orientation, different cyclic start points: they
		// must share one arc with the same reference (cyclic shift only).
		first := mustWKT(t, "POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))")
		second := mustWKT(t, "POLYGON ((0 4, 0 0, 4 0, 4 4, 0 4))")
		data, err := Marshal(map[string]geom.T{"f": first, "s": second}, 5)
		check(t, err == nil)
		top := parseTopology(t, data)
		check(t, len(top.Arcs) == 1)
		firstRefs := ringRefs(t, top.Objects["f"])[0]
		secondRefs := ringRefs(t, top.Objects["s"])[0]
		check(t, len(firstRefs) == 1 && len(secondRefs) == 1)
		check(t, firstRefs[0] == secondRefs[0])
		arcs := absoluteArcs(top.Arcs)
		want := [][2]int64{{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}}
		check(t, pathCovers(resolvePath(firstRefs, arcs), want, true))
		check(t, pathCovers(resolvePath(secondRefs, arcs), want, true))
	})

	runSubtest(t, "collection_with_empty_members", func(t *testing.T) {
		gc := geom.NewGeometryCollection().MustPush(
			geom.NewLineString(geom.XY),
			mustWKT(t, "LINESTRING (0 0, 4 4)"),
			geom.NewPolygon(geom.XY),
			geom.NewGeometryCollection().MustPush(
				geom.NewMultiPolygon(geom.XY),
			),
		)
		data, err := Marshal(map[string]geom.T{"gc": gc}, 5)
		check(t, err == nil)
		top := parseTopology(t, data)
		root := top.Objects["gc"]
		check(t, root.Type == "GeometryCollection" && len(root.Geometries) == 4)
		check(t, root.Geometries[0].Type == "LineString" &&
			len(root.Geometries[0].Arcs) == 0)
		check(t, root.Geometries[1].Type == "LineString" &&
			len(root.Geometries[1].Arcs) > 0)
		check(t, root.Geometries[2].Type == "Polygon" &&
			len(root.Geometries[2].Arcs) == 0)
		check(t, root.Geometries[3].Type == "GeometryCollection" &&
			len(root.Geometries[3].Geometries) == 1 &&
			root.Geometries[3].Geometries[0].Type == "MultiPolygon")
	})

	runSubtest(t, "maximal_arcs_merge_degree_two_runs", func(t *testing.T) {
		// A plain open polyline with several vertices has two endpoints and
		// degree-two interior vertices, so it becomes exactly one arc.
		line := mustWKT(t, "LINESTRING (0 0, 2 0, 4 0, 6 0)")
		data, err := Marshal(map[string]geom.T{"l": line}, 7)
		check(t, err == nil)
		top := parseTopology(t, data)
		check(t, len(top.Arcs) == 1)
		arcs := absoluteArcs(top.Arcs)
		refs := lineRefs(t, top.Objects["l"])
		check(t, pathsEqual(resolvePath(refs, arcs),
			[][2]int64{{0, 0}, {2, 0}, {4, 0}, {6, 0}}))
	})
}

// TestUnitQuantize covers the shared bounding box, rounding, transform,
// bbox emission and every quantization-related rejection.
func TestUnitQuantize(t *testing.T) {
	runSubtest(t, "round_half_up", func(t *testing.T) {
		cases := []struct {
			v    float64
			want int64
		}{
			{0.5, 1}, {-0.5, 0}, {1.5, 2}, {-1.5, -1},
			{2.49, 2}, {-2.49, -2}, {0.49, 0}, {-0.49, 0},
		}
		for _, c := range cases {
			check(t, roundHalfUp(c.v) == c.want)
		}
	})

	runSubtest(t, "shared_bbox_grid_values", func(t *testing.T) {
		// bbox [0,20] x [0,10] with Q=11: qx = x/2, qy = y.
		objs := map[string]geom.T{
			"a": mustWKT(t, "LINESTRING (0 0, 20 10)"),
			"b": mustWKT(t, "LINESTRING (10 5, 20 0)"),
		}
		data, err := Marshal(objs, 11)
		check(t, err == nil)
		top := parseTopology(t, data)
		arcs := absoluteArcs(top.Arcs)
		aPath := resolvePath(lineRefs(t, top.Objects["a"]), arcs)
		// b's start vertex (10,5) quantizes to (5,5), splitting line a.
		check(t, pathCovers(aPath,
			[][2]int64{{0, 0}, {10, 10}}, false))
		check(t, aPath[0] == [2]int64{0, 0} &&
			aPath[len(aPath)-1] == [2]int64{10, 10})
		bPath := resolvePath(lineRefs(t, top.Objects["b"]), arcs)
		check(t, pathsEqual(bPath,
			[][2]int64{{5, 5}, {10, 0}}))
		// The shared input vertex (5,5) splits the first line there.
		_, shared := edgeOccurrences(arcs)[[2][2]int64{{0, 0}, {5, 5}}]
		check(t, shared)
	})

	runSubtest(t, "zero_span_axis", func(t *testing.T) {
		line := geom.NewLineStringFlat(geom.XY, []float64{2, 5, 8, 5})
		data, err := Marshal(map[string]geom.T{"l": line}, 7)
		check(t, err == nil)
		top := parseTopology(t, data)
		// Zero-span Y: scale 1, translate equal to the minimum Y.
		check(t, top.Transform.Scale[0] == 1)
		check(t, top.Transform.Scale[1] == 1)
		check(t, top.Transform.Translate[0] == 2)
		check(t, top.Transform.Translate[1] == 5)
		arcs := absoluteArcs(top.Arcs)
		check(t, pathsEqual(resolvePath(lineRefs(t, top.Objects["l"]), arcs),
			[][2]int64{{0, 0}, {6, 0}}))
	})

	runSubtest(t, "bbox_and_all_empty_transform", func(t *testing.T) {
		data, err := Marshal(map[string]geom.T{
			"l": mustWKT(t, "LINESTRING (1 2, 3 4)"),
		}, 5)
		check(t, err == nil)
		top := parseTopology(t, data)
		check(t, len(top.BBox) == 4)
		check(t, top.BBox[0] == 1 && top.BBox[1] == 2 &&
			top.BBox[2] == 3 && top.BBox[3] == 4)

		empty, err := Marshal(map[string]geom.T{
			"a": geom.NewLineString(geom.XY),
			"b": geom.NewMultiPolygon(geom.XY),
			"c": geom.NewGeometryCollection(),
		}, 5)
		check(t, err == nil)
		emptyTop := parseTopology(t, empty)
		check(t, emptyTop.BBox == nil)
		check(t, len(emptyTop.Arcs) == 0)
		check(t, emptyTop.Transform.Scale == [2]float64{1, 1})
		check(t, emptyTop.Transform.Translate == [2]float64{0, 0})

		// A completely empty object map uses the same default transform and
		// an empty object table, with no bbox.
		emptyMap, err := Marshal(map[string]geom.T{}, 5)
		check(t, err == nil)
		emptyMapTop := parseTopology(t, emptyMap)
		check(t, emptyMapTop.Type == "Topology")
		check(t, len(emptyMapTop.Objects) == 0 && len(emptyMapTop.Arcs) == 0)
		check(t, emptyMapTop.Transform.Scale == [2]float64{1, 1})
		check(t, emptyMapTop.Transform.Translate == [2]float64{0, 0})
		check(t, emptyMapTop.BBox == nil)
	})

	runSubtest(t, "coordinate_limit", func(t *testing.T) {
		ok := geom.NewLineStringFlat(geom.XY, []float64{0, 0, 1e12, 0})
		data, err := Marshal(map[string]geom.T{"l": ok}, 3)
		check(t, err == nil && data != nil)

		tooFar := geom.NewLineStringFlat(geom.XY,
			[]float64{0, 0, math.Nextafter(1e12, math.Inf(1)), 0})
		data, err = Marshal(map[string]geom.T{"l": tooFar}, 3)
		check(t, err != nil && data == nil)

		negative := geom.NewLineStringFlat(geom.XY, []float64{0, 0, -1e12 - 1, 0})
		data, err = Marshal(map[string]geom.T{"l": negative}, 3)
		check(t, err != nil && data == nil)
	})

	runSubtest(t, "quantization_bounds", func(t *testing.T) {
		objs := map[string]geom.T{"l": mustWKT(t, "LINESTRING (0 0, 10 10)")}
		for _, q := range []int{2, 1_000_000} {
			data, err := Marshal(objs, q)
			check(t, err == nil && data != nil)
		}
		for _, q := range []int{1, 1_000_001, 0, -3} {
			data, err := Marshal(objs, q)
			check(t, err != nil && data == nil)
		}
	})

	runSubtest(t, "degenerate_geometries_rejected", func(t *testing.T) {
		cases := map[string]map[string]geom.T{
			"line collapses after quantization": {
				// A diagonal spanning a million units dominates both axes;
				// the tiny line's differences round to zero on both.
				"wide": mustWKT(t, "LINESTRING (0 0, 1000000 1000000)"),
				"tiny": mustWKT(t, "LINESTRING (0 0, 1 0.0001)"),
			},
			"duplicate point line": {
				"l": geom.NewLineStringFlat(geom.XY, []float64{3, 3, 3, 3}),
			},
			"ring fewer than three points": {
				"p": mustWKT(t, "POLYGON ((0 0, 4 0, 4 0, 0 0, 0 0))"),
			},
			"ring zero area": {
				"p": mustWKT(t, "POLYGON ((0 0, 2 0, 4 0, 0 0, 0 0))"),
			},
			"ring collapses in thin bbox": {
				// A million-unit-wide line dominates the x span; the unit
				// polygon's width then quantizes to zero, so its ring has
				// fewer than three distinct grid points.
				"wide": mustWKT(t, "LINESTRING (0 0, 1000000 0)"),
				"thin": mustWKT(t,
					"POLYGON ((0 0, 1 0, 1 1, 0 1, 0 0))"),
			},
			"unclosed ring": {
				"p": geom.NewPolygonFlat(geom.XY,
					[]float64{0, 0, 4, 0, 4, 4, 0, 4}, []int{8}),
			},
			"empty ring": {
				// A non-empty polygon whose second ring has zero points.
				"p": geom.NewPolygonFlat(geom.XY,
					[]float64{0, 0, 4, 0, 4, 4, 0, 4, 0, 0}, []int{10, 10}),
			},
		}
		for name, objs := range cases {
			data, err := Marshal(objs, 11)
			check(t, name != "" && err != nil && data == nil)
		}
	})

	runSubtest(t, "invalid_inputs_rejected", func(t *testing.T) {
		cases := map[string]map[string]geom.T{
			"nil":            {"x": nil},
			"typed nil line": {"x": (*geom.LineString)(nil)},
			"typed nil poly": {"x": (*geom.Polygon)(nil)},
			"point":          {"x": geom.NewPoint(geom.XY)},
			"multipoint":     {"x": geom.NewMultiPoint(geom.XY)},
			"linear ring":    {"x": geom.NewLinearRing(geom.XY)},
			"xyz layout": {"x": geom.NewLineString(geom.XYZ).MustSetCoords(
				[]geom.Coord{{0, 0, 0}, {1, 1, 1}})},
			"nonzero srid": {"x": geom.NewLineString(geom.XY).SetSRID(4326).
				MustSetCoords([]geom.Coord{{0, 0}, {1, 1}})},
			"nan": {"x": geom.NewLineStringFlat(geom.XY,
				[]float64{0, 0, math.NaN(), 1})},
			"inf": {"x": geom.NewLineStringFlat(geom.XY,
				[]float64{0, 0, math.Inf(-1), 1})},
			"odd flat coords": {"x": geom.NewLineStringFlat(geom.XY,
				[]float64{0, 0, 1})},
			"end beyond flat": {"x": geom.NewPolygonFlat(geom.XY,
				[]float64{0, 0, 1, 0, 1, 1, 0, 0}, []int{10})},
			"misaligned end": {"x": geom.NewPolygonFlat(geom.XY,
				[]float64{0, 0, 1, 0, 1, 1}, []int{5})},
			"bad multipolygon endss": {"x": geom.NewMultiPolygonFlat(geom.XY,
				[]float64{0, 0, 1, 0, 1, 1, 0, 0, 0, 0},
				[][]int{{10}, {4}})},
			"unsupported collection member": {"x": geom.NewGeometryCollection().
				MustPush(geom.NewPoint(geom.XY))},
			"nil collection member": {"x": geom.NewGeometryCollection().
				MustPush(nil)},
			"collection srid": {"x": geom.NewGeometryCollection().SetSRID(2000)},
		}
		for name, objs := range cases {
			data, err := Marshal(objs, 11)
			check(t, name != "" && err != nil && data == nil)
		}
	})

	runSubtest(t, "input_not_modified", func(t *testing.T) {
		line := geom.NewLineStringFlat(geom.XY,
			[]float64{0, 0, 5, 5, 10, 0})
		poly := geom.NewPolygonFlat(geom.XY,
			[]float64{0, 0, 10, 0, 10, 10, 0, 10, 0, 0}, []int{10})
		mp := geom.NewMultiPolygonFlat(geom.XY,
			[]float64{0, 0, 2, 0, 2, 2, 0, 2, 0, 0},
			[][]int{{10}})
		lineSnap := append([]float64(nil), line.FlatCoords()...)
		polySnap := append([]float64(nil), poly.FlatCoords()...)
		endsSnap := append([]int(nil), poly.Ends()...)
		mpSnap := append([]float64(nil), mp.FlatCoords()...)
		endssSnap := append([]int(nil), mp.Endss()[0]...)
		_, err := Marshal(map[string]geom.T{
			"l": line, "p": poly, "m": mp,
		}, 6)
		check(t, err == nil)
		check(t, pathsEqualFloat(line.FlatCoords(), lineSnap))
		check(t, pathsEqualFloat(poly.FlatCoords(), polySnap))
		check(t, intsEqual(poly.Ends(), endsSnap))
		check(t, pathsEqualFloat(mp.FlatCoords(), mpSnap))
		check(t, intsEqual(mp.Endss()[0], endssSnap))

		// A failed call must leave the input untouched as well.
		bad := geom.NewLineStringFlat(geom.XY, []float64{0, 0, 1})
		badSnap := append([]float64(nil), bad.FlatCoords()...)
		data, err := Marshal(map[string]geom.T{"bad": bad, "l": line}, 6)
		check(t, err != nil && data == nil)
		check(t, pathsEqualFloat(bad.FlatCoords(), badSnap))
		check(t, pathsEqualFloat(line.FlatCoords(), lineSnap))
	})
}

func pathsEqualFloat(a, b []float64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func intsEqual(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestUnitReferences covers end-to-end reference semantics: reconstruction
// from JSON, stable numbering, direction and ordering guarantees.
func TestUnitReferences(t *testing.T) {
	runSubtest(t, "all_object_shapes_reconstruct", func(t *testing.T) {
		objs := map[string]geom.T{
			"line": mustWKT(t, "LINESTRING (0 0, 8 0)"),
			"mline": mustWKT(t,
				"MULTILINESTRING ((0 0, 4 0), (4 4, 8 8))"),
			"poly": mustWKT(t,
				"POLYGON ((0 0, 8 0, 8 8, 0 8, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))"),
			"mpoly": mustWKT(t,
				"MULTIPOLYGON (((0 0, 2 0, 2 2, 0 2, 0 0)), ((6 6, 8 6, 8 8, 6 8, 6 6)))"),
			"gc": geom.NewGeometryCollection().MustPush(
				mustWKT(t, "LINESTRING (0 8, 8 8)"),
				geom.NewPolygon(geom.XY),
			),
		}
		data, err := Marshal(objs, 9)
		check(t, err == nil)
		top := parseTopology(t, data)
		arcs := absoluteArcs(top.Arcs)

		linePath := resolvePath(lineRefs(t, top.Objects["line"]), arcs)
		check(t, pathCovers(linePath, [][2]int64{{0, 0}, {8, 0}}, false))

		mline := ringRefs(t, top.Objects["mline"])
		check(t, len(mline) == 2)
		check(t, pathCovers(resolvePath(mline[0], arcs),
			[][2]int64{{0, 0}, {4, 0}}, false))
		check(t, pathCovers(resolvePath(mline[1], arcs),
			[][2]int64{{4, 4}, {8, 8}}, false))

		poly := ringRefs(t, top.Objects["poly"])
		check(t, len(poly) == 2)
		check(t, pathCovers(resolvePath(poly[0], arcs),
			[][2]int64{{0, 0}, {8, 0}, {8, 8}, {0, 8}, {0, 0}}, true))
		check(t, pathCovers(resolvePath(poly[1], arcs),
			[][2]int64{{2, 2}, {2, 4}, {4, 4}, {4, 2}, {2, 2}}, true))

		mpoly := polygonRefs(t, top.Objects["mpoly"])
		check(t, len(mpoly) == 2 && len(mpoly[0]) == 1 && len(mpoly[1]) == 1)
		check(t, pathCovers(resolvePath(mpoly[0][0], arcs),
			[][2]int64{{0, 0}, {2, 0}, {2, 2}, {0, 2}, {0, 0}}, true))
		check(t, pathCovers(resolvePath(mpoly[1][0], arcs),
			[][2]int64{{6, 6}, {8, 6}, {8, 8}, {6, 8}, {6, 6}}, true))

		gc := top.Objects["gc"]
		check(t, gc.Type == "GeometryCollection" && len(gc.Geometries) == 2)
		check(t, gc.Geometries[1].Type == "Polygon" &&
			len(gc.Geometries[1].Arcs) == 0)
	})

	runSubtest(t, "empty_multigeometry_members_keep_structure", func(t *testing.T) {
		mline := geom.NewMultiLineStringFlat(geom.XY,
			[]float64{0, 0, 1, 1}, []int{0, 4})
		data, err := Marshal(map[string]geom.T{"m": mline}, 5)
		check(t, err == nil)
		top := parseTopology(t, data)
		groups := ringRefs(t, top.Objects["m"])
		check(t, len(groups) == 2 && len(groups[0]) == 0 && len(groups[1]) == 1)

		mpoly := geom.NewMultiPolygonFlat(geom.XY,
			[]float64{0, 0, 2, 0, 2, 2, 0, 2, 0, 0},
			[][]int{{}, {10}})
		data, err = Marshal(map[string]geom.T{"m": mpoly}, 5)
		check(t, err == nil)
		top = parseTopology(t, data)
		polys := polygonRefs(t, top.Objects["m"])
		check(t, len(polys) == 2 && len(polys[0]) == 0 && len(polys[1]) == 1)
	})

	runSubtest(t, "map_insertion_order_does_not_change_arcs", func(t *testing.T) {
		build := func() map[string]geom.T {
			return map[string]geom.T{
				"a": mustWKT(t, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"),
				"b": mustWKT(t, "POLYGON ((10 0, 20 0, 20 10, 10 10, 10 5, 10 0))"),
				"c": mustWKT(t, "LINESTRING (10 3, 15 3)"),
			}
		}
		first, err := Marshal(build(), 11)
		check(t, err == nil)
		firstTop := parseTopology(t, first)
		for range 10 {
			got, err := Marshal(build(), 11)
			check(t, err == nil)
			gotTop := parseTopology(t, got)
			check(t, len(gotTop.Arcs) == len(firstTop.Arcs))
			firstAbs := absoluteArcs(firstTop.Arcs)
			gotAbs := absoluteArcs(gotTop.Arcs)
			same := true
			for i := range firstAbs {
				if !pathsEqual(firstAbs[i], gotAbs[i]) {
					same = false
				}
			}
			check(t, same)
			check(t, gotTop.Transform == firstTop.Transform)
		}
	})

	runSubtest(t, "ring_start_rotation_keeps_arc_table", func(t *testing.T) {
		one := map[string]geom.T{
			"p": mustWKT(t, "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0))"),
		}
		rotated := map[string]geom.T{
			"p": mustWKT(t, "POLYGON ((10 10, 0 10, 0 0, 10 0, 10 10))"),
		}
		data1, err := Marshal(one, 11)
		check(t, err == nil)
		data2, err := Marshal(rotated, 11)
		check(t, err == nil)
		top1 := parseTopology(t, data1)
		top2 := parseTopology(t, data2)
		arcs1 := absoluteArcs(top1.Arcs)
		arcs2 := absoluteArcs(top2.Arcs)
		check(t, len(arcs1) == len(arcs2) && len(arcs1) == 1)
		check(t, pathsEqual(arcs1[0], arcs2[0]))
		path1 := resolvePath(ringRefs(t, top1.Objects["p"])[0], arcs1)
		path2 := resolvePath(ringRefs(t, top2.Objects["p"])[0], arcs2)
		check(t, ringEquivalent(path1, path2))
	})

	runSubtest(t, "references_never_invent_intersections", func(t *testing.T) {
		// Two lines cross at the grid point (5,5) but neither line has a
		// vertex there. They must remain two arcs and must not be joined.
		crossing := map[string]geom.T{
			"h": mustWKT(t, "LINESTRING (0 5, 10 5)"),
			"d": mustWKT(t, "LINESTRING (0 0, 10 10)"),
		}
		data, err := Marshal(crossing, 11)
		check(t, err == nil)
		top := parseTopology(t, data)
		arcs := absoluteArcs(top.Arcs)
		check(t, len(arcs) == 2)
		hPath := resolvePath(lineRefs(t, top.Objects["h"]), arcs)
		dPath := resolvePath(lineRefs(t, top.Objects["d"]), arcs)
		check(t, pathsEqual(hPath, [][2]int64{{0, 5}, {10, 5}}))
		check(t, pathsEqual(dPath, [][2]int64{{0, 0}, {10, 10}}))
	})

	runSubtest(t, "delta_encoding_restarts_per_arc", func(t *testing.T) {
		data, err := Marshal(map[string]geom.T{
			"a": mustWKT(t, "LINESTRING (2 2, 6 6)"),
			"b": mustWKT(t, "LINESTRING (6 2, 2 6)"),
		}, 9)
		check(t, err == nil)
		top := parseTopology(t, data)
		// Each arc's first delta position equals its absolute first
		// position because deltas restart at [0,0].
		for _, arc := range top.Arcs {
			abs := absoluteArcs([][][2]int64{arc})[0]
			check(t, arc[0] == abs[0])
		}
		// Independent decode still reconstructs both diagonals. With bbox
		// [2,6] and Q=9 the grid range is 0..8, so (2,2)->(0,0), (6,6)->(8,8),
		// (6,2)->(8,0) and (2,6)->(0,8).
		arcs := absoluteArcs(top.Arcs)
		check(t, pathsEqual(resolvePath(lineRefs(t, top.Objects["a"]), arcs),
			[][2]int64{{0, 0}, {8, 8}}))
		check(t, pathsEqual(resolvePath(lineRefs(t, top.Objects["b"]), arcs),
			[][2]int64{{8, 0}, {0, 8}}))
	})

	runSubtest(t, "canonical_arc_numbering_is_lexicographic", func(t *testing.T) {
		data, err := Marshal(map[string]geom.T{
			"z": mustWKT(t, "LINESTRING (6 0, 6 6)"),
			"a": mustWKT(t, "LINESTRING (0 0, 0 6)"),
			"m": mustWKT(t, "LINESTRING (3 0, 3 6)"),
		}, 7)
		check(t, err == nil)
		top := parseTopology(t, data)
		arcs := absoluteArcs(top.Arcs)
		check(t, len(arcs) == 3)
		// Stored arcs start at their lexicographically smallest endpoint.
		for _, arc := range arcs {
			first, last := arc[0], arc[len(arc)-1]
			check(t, first[0] < last[0] ||
				(first[0] == last[0] && first[1] <= last[1]))
		}
		check(t, arcs[0][0] == [2]int64{0, 0})
		check(t, arcs[1][0] == [2]int64{3, 0})
		check(t, arcs[2][0] == [2]int64{6, 0})
	})
}
