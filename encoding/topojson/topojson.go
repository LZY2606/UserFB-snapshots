// Package topojson encodes groups of go-geom geometries into a single
// [TopoJSON] topology whose geometries share one deduplicated set of arcs.
//
// Unlike encoding each geometry independently, [Marshal] quantizes all the
// objects together, splits segments at every quantized vertex that falls in
// their interior, deduplicates the resulting undirected edges and emits the
// maximal arcs between breakpoints. Objects then reference those arcs, using
// the bitwise complement index (~i) for reverse traversal.
//
// # Supported input
//
// Marshal accepts XY geometries with SRID 0 of the following types:
//   - [geom.LineString]
//   - [geom.MultiLineString]
//   - [geom.Polygon]
//   - [geom.MultiPolygon]
//   - [geom.GeometryCollection], recursively, including nested collections
//
// Empty geometries and empty members keep their type and nesting. Point,
// MultiPoint, LinearRing and any other [geom.T] implementation are rejected,
// as are non-XY layouts, non-zero SRIDs, non-finite coordinates, absolute
// coordinates larger than 1e12 and inconsistent flat coordinate/ends/endss
// structures. Inputs are never modified and failed calls return nil bytes.
//
// # Quantization
//
// All non-empty objects share one XY bounding box. Each axis is mapped to the
// integer grid
//
//	q(v) = round((v-min)/(max-min) * (Q-1))
//
// where Q is the quantization argument (2..1000000) and half values with a
// non-negative fractional part round towards positive infinity. A zero-span
// axis maps every value to 0; its transform uses scale 1 and translate min, so
// the original coordinate is reconstructed exactly. Consecutive vertices that
// quantize to the same grid point are merged.
//
// # Topology
//
// The topology is built entirely on the integer grid, with all length and
// collinearity tests done in exact integer arithmetic: every input segment
// that contains another input vertex in its interior is split at that vertex,
// which covers T-junctions and collinear boundaries sampled at different
// densities. The resulting undirected small edges are deduplicated, so one
// geometric edge keeps a single identity. Vertices whose degree is not two and
// the ends of non-closed lines are breakpoints; maximal edge runs between
// breakpoints become arcs, while a standalone closed loop whose vertices all
// have degree two is stored as a single arc. Arcs get a canonical stored
// direction (and, for loops, a canonical cyclic start) independent of input
// order, and are numbered in lexicographic order of their canonical grid
// sequences.
//
// True crossings without an input vertex are not intersected, no polygon
// boolean operations are performed and holes are not reclassified.
//
// # Complexity
//
// With n input vertices and s interior split points, segment splitting takes
// O(n+s) gcd steps, graph construction O((n+s) log(n+s)) and JSON output
// O(n+s) for the delta-encoded arcs.
//
// [TopoJSON]: https://github.com/topojson/topojson-specification
package topojson

import (
	"encoding/json"
	"fmt"
	"math"
	"sort"

	geom "github.com/twpayne/go-geom"
)

// maxAbsoluteCoordinate is the largest absolute input coordinate accepted.
const maxAbsoluteCoordinate = 1e12

// ErrInvalidArgument is returned when an argument does not satisfy Marshal's
// contract: nil objects, unsupported types or layouts, non-finite coordinates,
// malformed flat coordinate structures, quantization values outside
// [2, 1000000], or geometries that degenerate after quantization.
type ErrInvalidArgument struct {
	reason string
}

// Error implements the error interface.
func (e *ErrInvalidArgument) Error() string {
	return "topojson: " + e.reason
}

func errInvalid(format string, args ...any) error {
	return &ErrInvalidArgument{reason: fmt.Sprintf(format, args...)}
}

// A transform is the TopoJSON quantization transform: an absolute coordinate
// is reconstructed as scale*quantized + translate.
type transform struct {
	Scale     [2]float64 `json:"scale"`
	Translate [2]float64 `json:"translate"`
}

// topoObject is one entry of the topology's objects map.
type topoObject struct {
	Type       string          `json:"type"`
	Arcs       json.RawMessage `json:"arcs,omitempty"`
	Geometries []*topoObject   `json:"geometries,omitempty"`
}

// topology is the serialized TopoJSON topology.
type topology struct {
	Type      string                 `json:"type"`
	Objects   map[string]*topoObject `json:"objects"`
	Arcs      [][][2]int64           `json:"arcs"`
	Transform transform              `json:"transform"`
	BBox      []float64              `json:"bbox,omitempty"`
}

// Marshal encodes objects as a TopoJSON topology. quantization is the number
// of quantized values per axis and must be between 2 and 1000000 inclusive.
//
// The object names, GeometryCollection and multi-geometry member order, and
// the outer-ring/hole structure of polygons are preserved. Empty geometries
// keep their type and structure. On any error Marshal returns nil bytes and
// leaves the inputs untouched.
func Marshal(objects map[string]geom.T, quantization int) ([]byte, error) {
	if quantization < 2 || quantization > 1_000_000 {
		return nil, errInvalid("quantization must be between 2 and 1000000, got %d", quantization)
	}

	names := make([]string, 0, len(objects))
	for name := range objects {
		names = append(names, name)
	}
	sort.Strings(names)

	for _, name := range names {
		if err := validateGeometry(objects[name]); err != nil {
			return nil, fmt.Errorf("topojson: object %q: %w", name, err)
		}
	}

	minX, minY, maxX, maxY, hasCoords := boundsOf(objects, names)

	builder := &builder{
		quantization:  quantization,
		minX:          minX,
		minY:          minY,
		maxX:          maxX,
		maxY:          maxY,
		hasCoords:     hasCoords,
		inputVertices: make(map[iPoint]bool),
	}

	plans := make(map[string]*plan, len(names))
	planList := make([]*plan, 0, len(names))
	for _, name := range names {
		p, err := builder.buildPlan(objects[name])
		if err != nil {
			return nil, fmt.Errorf("topojson: object %q: %w", name, err)
		}
		plans[name] = p
		planList = append(planList, p)
	}

	arcs, edgeLookup, breakpoints, err := builder.buildTopology(planList)
	if err != nil {
		return nil, err
	}

	topoObjects := make(map[string]*topoObject, len(names))
	for _, name := range names {
		obj, err := builder.buildObject(plans[name], edgeLookup, breakpoints)
		if err != nil {
			return nil, err
		}
		topoObjects[name] = obj
	}

	arcPositions := make([][][2]int64, len(arcs))
	for i, arc := range arcs {
		arcPositions[i] = deltaEncode(arc)
	}

	top := topology{
		Type:    "Topology",
		Objects: topoObjects,
		Arcs:    arcPositions,
		Transform: transform{
			Scale: [2]float64{builder.scaleX, builder.scaleY},
		},
	}
	if hasCoords {
		top.Transform.Translate = [2]float64{minX, minY}
		top.BBox = []float64{minX, minY, maxX, maxY}
	}

	data, err := json.Marshal(&top)
	if err != nil {
		return nil, err
	}
	return data, nil
}

// validateGeometry recursively checks g's type, layout, SRID, finiteness and
// flat structure without touching any of g's data.
func validateGeometry(g geom.T) error {
	if g == nil {
		return errInvalid("nil geometry")
	}
	switch g := g.(type) {
	case *geom.LineString:
		if g == nil {
			return errInvalid("nil geometry")
		}
		return validateFlat1(g, "LineString")
	case *geom.MultiLineString:
		if g == nil {
			return errInvalid("nil geometry")
		}
		return validateFlat2(g, "MultiLineString")
	case *geom.Polygon:
		if g == nil {
			return errInvalid("nil geometry")
		}
		return validateFlat2(g, "Polygon")
	case *geom.MultiPolygon:
		if g == nil {
			return errInvalid("nil geometry")
		}
		return validateFlat3(g)
	case *geom.GeometryCollection:
		if g == nil {
			return errInvalid("nil geometry")
		}
		if g.SRID() != 0 {
			return errInvalid("GeometryCollection has unsupported SRID %d", g.SRID())
		}
		for i, sub := range g.Geoms() {
			if err := validateGeometry(sub); err != nil {
				return fmt.Errorf("GeometryCollection member %d: %w", i, err)
			}
		}
		return nil
	default:
		return errInvalid("unsupported geometry type %T", g)
	}
}

func validateHeader(g geom.T, name string) error {
	if g.Layout() != geom.XY {
		return errInvalid("%s has unsupported layout %s", name, g.Layout())
	}
	if g.SRID() != 0 {
		return errInvalid("%s has unsupported SRID %d", name, g.SRID())
	}
	return nil
}

func validateFinite(flatCoords []float64, name string) error {
	if len(flatCoords)%2 != 0 {
		return errInvalid("%s has malformed flat coordinates: %d values is not a multiple of 2", name, len(flatCoords))
	}
	for _, v := range flatCoords {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return errInvalid("%s contains non-finite coordinate", name)
		}
		if math.Abs(v) > maxAbsoluteCoordinate {
			return errInvalid("%s coordinate %g exceeds absolute limit %g", name, v, maxAbsoluteCoordinate)
		}
	}
	return nil
}

func validateFlat1(g geom.T, name string) error {
	if err := validateHeader(g, name); err != nil {
		return err
	}
	return validateFinite(g.FlatCoords(), name)
}

func validateFlat2(g geom.T, name string) error {
	if err := validateHeader(g, name); err != nil {
		return err
	}
	flatCoords := g.FlatCoords()
	if err := validateFinite(flatCoords, name); err != nil {
		return err
	}
	offset := 0
	for i, end := range g.Ends() {
		if end < offset || end > len(flatCoords) || end%2 != 0 {
			return errInvalid("%s has malformed ends at index %d: %d", name, i, end)
		}
		offset = end
	}
	if offset != len(flatCoords) {
		return errInvalid("%s ends do not cover its flat coordinates", name)
	}
	return nil
}

func validateFlat3(g *geom.MultiPolygon) error {
	if err := validateHeader(g, "MultiPolygon"); err != nil {
		return err
	}
	flatCoords := g.FlatCoords()
	if err := validateFinite(flatCoords, "MultiPolygon"); err != nil {
		return err
	}
	offset := 0
	for i, ends := range g.Endss() {
		for j, end := range ends {
			if end < offset || end > len(flatCoords) || end%2 != 0 {
				return errInvalid("MultiPolygon has malformed endss at polygon %d ring %d: %d", i, j, end)
			}
			offset = end
		}
	}
	if offset != len(flatCoords) {
		return errInvalid("MultiPolygon endss do not cover its flat coordinates")
	}
	return nil
}

// boundsOf returns the XY bounding box of every non-empty object.
func boundsOf(objects map[string]geom.T, names []string) (minX, minY, maxX, maxY float64, hasCoords bool) {
	minX, minY = math.Inf(1), math.Inf(1)
	maxX, maxY = math.Inf(-1), math.Inf(-1)
	var visit func(geom.T)
	visit = func(g geom.T) {
		switch g := g.(type) {
		case *geom.LineString, *geom.Polygon, *geom.MultiLineString, *geom.MultiPolygon:
			flatCoords := g.FlatCoords()
			for i := 0; i+1 < len(flatCoords); i += 2 {
				x, y := flatCoords[i], flatCoords[i+1]
				if !hasCoords {
					minX, maxX, minY, maxY = x, x, y, y
					hasCoords = true
					continue
				}
				minX = min(minX, x)
				maxX = max(maxX, x)
				minY = min(minY, y)
				maxY = max(maxY, y)
			}
		case *geom.GeometryCollection:
			for _, sub := range g.Geoms() {
				visit(sub)
			}
		}
	}
	for _, name := range names {
		visit(objects[name])
	}
	return
}
