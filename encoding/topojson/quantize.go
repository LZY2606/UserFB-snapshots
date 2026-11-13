package topojson

import (
	"math"
	"math/big"

	geom "github.com/twpayne/go-geom"
)

// A plan is the quantized, segment-validated representation of one input
// geometry. An empty plan keeps only its type and nesting.
type plan struct {
	typ      string
	rings    []ring   // LineString: one open ring; Polygon: outer ring then holes
	polygons [][]ring // MultiPolygon: polygons of rings
	geoms    []*plan  // GeometryCollection members, in order
}

// A ring is one quantized coordinate sequence. Open rings describe lines;
// closed rings (first point equal to last) describe polygon boundaries.
type ring = []iPoint

// A builder owns the quantization parameters used while turning one group of
// objects into a topology.
type builder struct {
	quantization int
	minX, minY   float64
	maxX, maxY   float64
	hasCoords    bool

	scaleX, scaleY float64

	// inputVertices is the set of quantized grid points that occur as
	// vertices in the inputs. Segment splitting only ever happens at these
	// points, never at an arbitrary grid intersection.
	inputVertices map[iPoint]bool
}

// quantize maps one XY coordinate to the shared integer grid.
func (b *builder) quantize(x, y float64) iPoint {
	var qx, qy int64
	if b.hasCoords {
		if b.maxX > b.minX {
			qx = quantizeAxis(x, b.minX, b.maxX, b.quantization)
		}
		if b.maxY > b.minY {
			qy = quantizeAxis(y, b.minY, b.maxY, b.quantization)
		}
	}
	return iPoint{X: qx, Y: qy}
}

// quantizeAxis implements round((v-min)/(max-min)*(Q-1)) with half values
// whose fractional part is non-negative rounded towards positive infinity.
func quantizeAxis(v, minV, maxV float64, quantization int) int64 {
	scaled := (v - minV) / (maxV - minV) * float64(quantization-1)
	return roundHalfUp(scaled)
}

// roundHalfUp rounds to the nearest integer, with half values rounded towards
// positive infinity (so -0.5 becomes 0 rather than -1).
func roundHalfUp(v float64) int64 {
	return int64(math.Floor(v + 0.5))
}

// setTransform computes the dequantization transform. Zero-span axes use
// scale 1 with the axis minimum as translate.
func (b *builder) setTransform() {
	qMinus1 := float64(b.quantization - 1)
	if b.hasCoords && b.maxX > b.minX {
		b.scaleX = (b.maxX - b.minX) / qMinus1
	} else {
		b.scaleX = 1
	}
	if b.hasCoords && b.maxY > b.minY {
		b.scaleY = (b.maxY - b.minY) / qMinus1
	} else {
		b.scaleY = 1
	}
}

// quantizeCoords maps a flat XY coordinate slice to a ring, merging
// consecutive duplicate quantized vertices. Every retained grid point is
// registered as an input vertex.
func (b *builder) quantizeCoords(flatCoords []float64) ring {
	r := make(ring, 0, len(flatCoords)/2)
	for i := 0; i+1 < len(flatCoords); i += 2 {
		p := b.quantize(flatCoords[i], flatCoords[i+1])
		if len(r) == 0 || r[len(r)-1] != p {
			r = append(r, p)
			b.inputVertices[p] = true
		}
	}
	return r
}

// splitPoints returns the quantized input vertices lying strictly inside the
// segment from a to b. The candidate positions are enumerated exactly on the
// integer lattice, then filtered to actual input vertices, so crossings
// without an input vertex never create new points.
func (b *builder) splitPoints(a, c iPoint) []iPoint {
	candidates := latticePoints(a, c)
	if len(candidates) == 0 {
		return nil
	}
	points := make([]iPoint, 0, len(candidates))
	for _, p := range candidates {
		if b.inputVertices[p] {
			points = append(points, p)
		}
	}
	return points
}

// buildPlan quantizes g and validates every non-empty line or ring on the
// integer grid.
func (b *builder) buildPlan(g geom.T) (*plan, error) {
	switch g := g.(type) {
	case *geom.LineString:
		p := &plan{typ: "LineString"}
		if g.Empty() {
			return p, nil
		}
		r := b.quantizeCoords(g.FlatCoords())
		if err := validateLine(r); err != nil {
			return nil, err
		}
		p.rings = []ring{r}
		return p, nil
	case *geom.MultiLineString:
		p := &plan{typ: "MultiLineString"}
		if g.Empty() {
			return p, nil
		}
		offset := 0
		for i, end := range g.Ends() {
			r := b.quantizeCoords(g.FlatCoords()[offset:end])
			if err := validateLine(r); err != nil {
				return nil, wrapMember("MultiLineString", i, err)
			}
			p.rings = append(p.rings, r)
			offset = end
		}
		return p, nil
	case *geom.Polygon:
		p := &plan{typ: "Polygon"}
		if g.Empty() {
			return p, nil
		}
		rings, err := b.quantizeRings(g.FlatCoords(), g.Ends())
		if err != nil {
			return nil, err
		}
		p.rings = rings
		return p, nil
	case *geom.MultiPolygon:
		p := &plan{typ: "MultiPolygon"}
		if g.Empty() {
			return p, nil
		}
		offset := 0
		for i, ends := range g.Endss() {
			if len(ends) == 0 {
				p.polygons = append(p.polygons, nil)
				continue
			}
			rings, err := b.quantizeRings(g.FlatCoords()[offset:ends[len(ends)-1]], relativeEnds(ends, offset))
			if err != nil {
				return nil, wrapMember("MultiPolygon", i, err)
			}
			p.polygons = append(p.polygons, rings)
			offset = ends[len(ends)-1]
		}
		return p, nil
	case *geom.GeometryCollection:
		p := &plan{typ: "GeometryCollection"}
		for i, sub := range g.Geoms() {
			child, err := b.buildPlan(sub)
			if err != nil {
				return nil, wrapMember("GeometryCollection", i, err)
			}
			p.geoms = append(p.geoms, child)
		}
		return p, nil
	default:
		return nil, errInvalid("unsupported geometry type %T", g)
	}
}

// quantizeRings maps the flat coordinates of a Polygon (or one MultiPolygon
// member) into closed rings, preserving outer-ring/hole order.
func (b *builder) quantizeRings(flatCoords []float64, ends []int) ([]ring, error) {
	rings := make([]ring, 0, len(ends))
	offset := 0
	for i, end := range ends {
		raw := flatCoords[offset:end]
		if err := validateInputRing(raw); err != nil {
			return nil, wrapMember("Polygon", i, err)
		}
		r := b.quantizeCoords(raw)
		if err := validateRing(r); err != nil {
			return nil, wrapMember("Polygon", i, err)
		}
		rings = append(rings, r)
		offset = end
	}
	return rings, nil
}

func relativeEnds(ends []int, offset int) []int {
	out := make([]int, len(ends))
	for i, end := range ends {
		out[i] = end - offset
	}
	return out
}

func wrapMember(container string, index int, err error) error {
	return errInvalid("%s member %d: %s", container, index, err.Error())
}

// validateInputRing checks a ring before quantization: it must be non-empty,
// hold whole XY pairs and be explicitly closed with matching endpoints.
func validateInputRing(flatCoords []float64) error {
	if len(flatCoords) == 0 {
		return errInvalid("polygon contains an empty ring")
	}
	if len(flatCoords) < 8 || len(flatCoords)%2 != 0 {
		return errInvalid("ring has malformed flat coordinates: %d values", len(flatCoords))
	}
	if flatCoords[0] != flatCoords[len(flatCoords)-2] ||
		flatCoords[1] != flatCoords[len(flatCoords)-1] {
		return errInvalid("ring is not explicitly closed")
	}
	return nil
}

// validateLine rejects non-empty quantized lines with fewer than two
// distinct points.
func validateLine(r ring) error {
	if len(r) == 0 {
		return nil
	}
	if len(r) < 2 {
		return errInvalid("line degenerated to a single quantized point")
	}
	return nil
}

// validateRing enforces at least three distinct quantized points and a
// non-zero signed area on the integer grid. Closure was already established on
// the input coordinates.
func validateRing(r ring) error {
	if len(r) == 0 {
		return errInvalid("polygon contains an empty ring")
	}
	if r[0] != r[len(r)-1] {
		return errInvalid("ring lost its closure after quantization")
	}
	// The closing vertex duplicates the first one; count the distinct
	// vertices of the open sequence.
	seen := make(map[iPoint]struct{}, len(r)-1)
	for _, p := range r[:len(r)-1] {
		seen[p] = struct{}{}
	}
	if len(seen) < 3 {
		return errInvalid("ring has fewer than three distinct quantized points")
	}
	if signedArea2(r).Sign() == 0 {
		return errInvalid("ring has zero directed area after quantization")
	}
	return nil
}

// signedArea2 returns twice the signed area of a closed ring. The arithmetic
// stays in the exact integer domain regardless of ring length or grid size.
func signedArea2(r ring) *big.Int {
	area := new(big.Int)
	cross := new(big.Int)
	for i := 0; i+1 < len(r); i++ {
		cross.SetInt64(r[i].X)
		cross.Mul(cross, big.NewInt(r[i+1].Y))
		area.Add(area, cross)
		cross.SetInt64(r[i+1].X)
		cross.Mul(cross, big.NewInt(r[i].Y))
		area.Sub(area, cross)
	}
	return area
}
