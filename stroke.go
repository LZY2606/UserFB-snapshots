package fauxgl

import (
	"fmt"
	"math"
)

// StrokeStyle configures how Context.DrawPolyline rasterizes a polyline.
//
// Width and all dash lengths are measured in final screen pixels.
type StrokeStyle struct {
	// Width is the stroke width in screen pixels. It must be finite and
	// strictly positive.
	Width float64
	// Dashes is an optional dash pattern. When non-empty it must have a
	// positive, even number of entries, each finite and strictly positive,
	// alternating solid and empty runs. An empty slice draws a solid stroke.
	Dashes []float64
	// DashOffset shifts the dash pattern along the path. Positive values start
	// the path partway into the pattern; negative values are taken modulo the
	// pattern period.
	DashOffset float64
	// Closed appends a final segment from the last vertex back to the first.
	Closed bool
}

func validStrokeLength(x float64) bool {
	return !math.IsNaN(x) && !math.IsInf(x, 0) && x > 0
}

func (s StrokeStyle) validate() error {
	if !validStrokeLength(s.Width) {
		return fmt.Errorf("fauxgl: invalid stroke width %v", s.Width)
	}
	if len(s.Dashes) > 0 {
		if len(s.Dashes)%2 != 0 {
			return fmt.Errorf("fauxgl: dash pattern must have an even number of entries")
		}
		for _, d := range s.Dashes {
			if !validStrokeLength(d) {
				return fmt.Errorf("fauxgl: invalid dash length %v", d)
			}
		}
	}
	if math.IsNaN(s.DashOffset) || math.IsInf(s.DashOffset, 0) {
		return fmt.Errorf("fauxgl: invalid dash offset %v", s.DashOffset)
	}
	return nil
}

// strokePeriod returns the dash pattern period. The second result reports
// whether a dash pattern is in use at all.
func (s StrokeStyle) strokePeriod() (float64, bool) {
	if len(s.Dashes) == 0 {
		return 0, false
	}
	var period float64
	for _, d := range s.Dashes {
		period += d
	}
	return period, true
}

// strokeOn reports whether a point at the given global screen arc length is
// painted. Dash runs are left-closed and right-open.
func (s StrokeStyle) strokeOn(length, period float64) bool {
	if period <= 0 {
		return true
	}
	pos := math.Mod(length+s.DashOffset, period)
	if pos < 0 {
		pos += period
	}
	var acc float64
	for i, d := range s.Dashes {
		if pos < acc+d {
			return i%2 == 0
		}
		acc += d
	}
	return false
}

// strokeEpsilon returns a small length used to sample the dash phase just
// before and just after a polyline vertex.
func (s StrokeStyle) strokeEpsilon(period float64) float64 {
	if period <= 0 {
		return 0
	}
	eps := period * 1e-9
	minDash := math.Inf(1)
	for _, d := range s.Dashes {
		if d < minDash {
			minDash = d
		}
	}
	if e := minDash * 1e-6; e < eps {
		eps = e
	}
	if eps <= 0 {
		eps = 1e-12
	}
	return eps
}

// strokePoint is a projected polyline vertex in screen space, together with
// the running screen arc length at that vertex.
type strokePoint struct {
	vertex Vertex
	screen Vector
	length float64
}

// strokePathEdge is one nonzero polyline edge.
type strokePathEdge struct {
	ia, ib int
	a, b   Vector // screen positions
	za, zb float64
	va, vb Vertex
	lenA   float64 // running screen arc length at a
	lenB   float64 // running screen arc length at b
	order  int
	normal Vector
}

// strokePiece is one butt-capped, screen-space piece of an edge to rasterize.
// Its endpoints are the result of frustum and canvas clipping, while t0/t1
// are parameters on the original projected edge so attributes and depth stay
// anchored to that edge.
type strokePiece struct {
	a, b   Vector
	t0, t1 float64
	za, zb float64
	va, vb Vertex
	lenA   float64
	lenB   float64
	order  int
	seq    int
}

// strokeJoin is a bevel wedge at an interior polyline vertex.
type strokeJoin struct {
	center Vector
	vertex Vertex
	depth  float64
	n1, n2 Vector
	order  int
	seq    int
}

// strokeCandidate is the winning coverage at one pixel.
type strokeCandidate struct {
	depth float64
	order int
	seq   int
	piece *strokePiece
	join  *strokeJoin
	t     float64
}

// invalidStrokeOutput reports whether a transformed vertex is unusable.
func invalidStrokeOutput(o VectorW) bool {
	for _, x := range [...]float64{o.X, o.Y, o.Z, o.W} {
		if math.IsNaN(x) || math.IsInf(x, 0) {
			return true
		}
	}
	return false
}

// DrawPolyline rasterizes a polyline as one continuous stroke. Unlike a
// sequence of DrawLine calls, the dash phase accumulates over the entire
// path, no endpoint caps are extended, and bends use bevel joins instead of
// overlapping square caps.
//
// The vertex shader is invoked exactly once per input vertex. All transformed
// vertices must be finite with w > 0; otherwise an error is returned before
// any buffer is modified.
func (dc *Context) DrawPolyline(vertices []Vertex, style StrokeStyle) (RasterizeInfo, error) {
	var info RasterizeInfo

	if err := style.validate(); err != nil {
		return info, err
	}
	if len(vertices) == 0 {
		return info, nil
	}

	// invoke the vertex shader once per input vertex and validate all results
	// before touching any buffer
	projected := make([]Vertex, len(vertices))
	bad := false
	for i, v := range vertices {
		p := dc.Shader.Vertex(v)
		if invalidStrokeOutput(p.Output) || !(p.Output.W > 0) {
			bad = true
		}
		projected[i] = p
	}
	if bad {
		return RasterizeInfo{}, fmt.Errorf("fauxgl: transformed vertex is not finite or has w <= 0")
	}
	if len(vertices) < 2 {
		return info, nil
	}

	points := dc.projectStrokePath(projected)
	if !strokePathHasLength(points, style.Closed) {
		return info, nil
	}

	edges := dc.buildStrokeEdges(points, style.Closed)
	pieces := dc.buildStrokePieces(edges, style)
	joins := dc.buildStrokeJoins(points, edges, style)

	// collect deduplicated coverage candidates keyed by pixel index
	var candidates map[int]strokeCandidate
	seq := 0
	add := func(idx int, c strokeCandidate) {
		if candidates == nil {
			candidates = make(map[int]strokeCandidate)
		}
		if cur, ok := candidates[idx]; !ok || strokeCandidateWins(c, cur) {
			candidates[idx] = c
		}
	}

	period, dashed := style.strokePeriod()
	half := style.Width / 2
	for i := range pieces {
		pieces[i].seq = seq
		seq++
		dc.coverStrokePiece(&pieces[i], half, style, period, dashed, add)
	}
	for i := range joins {
		joins[i].seq = seq
		seq++
		dc.coverStrokeJoin(&joins[i], half, add)
	}

	info.TotalPixels = uint64(len(candidates))
	for idx, c := range candidates {
		x := idx % dc.Width
		y := idx / dc.Width
		var vertex Vertex
		if c.join != nil {
			vertex = c.join.vertex
		} else {
			vertex = strokeInterpolateVertex(c.piece.va, c.piece.vb, c.t)
		}
		info = info.Add(dc.commitStrokePixel(vertex, x, y, c.depth))
	}
	return info, nil
}

// strokeCandidateWins reports whether a should replace b. Nearer coverage
// wins; on a depth tie the edge later along the path wins, and construction
// order breaks any remaining tie.
func strokeCandidateWins(a, b strokeCandidate) bool {
	if a.depth != b.depth {
		return a.depth < b.depth
	}
	if a.order != b.order {
		return a.order > b.order
	}
	return a.seq > b.seq
}

// strokeScreenDistance is the 2D distance between two screen-space points.
// Screen depth is tracked separately and does not contribute to arc length.
func strokeScreenDistance(a, b Vector) float64 {
	return math.Hypot(a.X-b.X, a.Y-b.Y)
}

// projectStrokePath projects all transformed vertices to screen space and
// accumulates screen arc length over the entire path, before any clipping.
func (dc *Context) projectStrokePath(projected []Vertex) []strokePoint {
	points := make([]strokePoint, len(projected))
	var length float64
	for i, v := range projected {
		ndc := v.Output.DivScalar(v.Output.W).Vector()
		s := dc.screenMatrix.MulPosition(ndc)
		if i > 0 {
			length += strokeScreenDistance(points[i-1].screen, s)
		}
		points[i] = strokePoint{v, s, length}
	}
	return points
}

func strokePathHasLength(points []strokePoint, closed bool) bool {
	for i := 1; i < len(points); i++ {
		if strokeScreenDistance(points[i-1].screen, points[i].screen) > 0 {
			return true
		}
	}
	if closed && len(points) > 1 {
		return strokeScreenDistance(points[len(points)-1].screen, points[0].screen) > 0
	}
	return false
}

// strokePathEdges lists the polyline edges as vertex index pairs.
func strokePathEdges(n int, closed bool) [][2]int {
	edges := make([][2]int, 0, n)
	for i := 1; i < n; i++ {
		edges = append(edges, [2]int{i - 1, i})
	}
	if closed && n > 1 {
		edges = append(edges, [2]int{n - 1, 0})
	}
	return edges
}

func (dc *Context) buildStrokeEdges(points []strokePoint, closed bool) []strokePathEdge {
	pairs := strokePathEdges(len(points), closed)
	edges := make([]strokePathEdge, 0, len(pairs))
	for k, pair := range pairs {
		a := points[pair[0]]
		b := points[pair[1]]
		d := b.screen.Sub(a.screen)
		l := strokeScreenDistance(a.screen, b.screen)
		if l <= 0 {
			// coincident projected points consume no length and draw nothing
			continue
		}
		lenB := b.length
		if closed && pair[0] == len(points)-1 && pair[1] == 0 {
			// the closing edge continues from the last vertex, not from zero
			lenB = a.length + l
		}
		edges = append(edges, strokePathEdge{
			ia:     pair[0],
			ib:     pair[1],
			a:      a.screen,
			b:      b.screen,
			za:     a.screen.Z,
			zb:     b.screen.Z,
			va:     a.vertex,
			vb:     b.vertex,
			lenA:   a.length,
			lenB:   lenB,
			order:  k,
			normal: Vector{-d.Y / l, d.X / l, 0},
		})
	}
	return edges
}

// buildStrokePieces clips each edge to the near/far planes and to the canvas
// (widened by half the stroke width so a stroke centered just off-canvas can
// still reach visible pixels). Arc length and dash phase are preserved.
func (dc *Context) buildStrokePieces(edges []strokePathEdge, style StrokeStyle) []strokePiece {
	half := style.Width / 2
	x0 := -half
	y0 := -half
	x1 := float64(dc.Width) + half
	y1 := float64(dc.Height) + half
	var pieces []strokePiece
	for _, e := range edges {
		t0, t1, ok := strokeDepthInterval(e.za, e.zb)
		if !ok {
			continue
		}
		if !clipStrokeCanvas(e.a, e.b, x0, y0, x1, y1, &t0, &t1) {
			continue
		}
		if t1 <= t0 {
			continue
		}
		pieces = append(pieces, strokePiece{
			a:     strokeLerp(e.a, e.b, t0),
			b:     strokeLerp(e.a, e.b, t1),
			t0:    t0,
			t1:    t1,
			za:    e.za,
			zb:    e.zb,
			va:    e.va,
			vb:    e.vb,
			lenA:  e.lenA,
			lenB:  e.lenB,
			order: e.order,
		})
	}
	return pieces
}

func strokeLerp(a, b Vector, t float64) Vector {
	return a.Add(b.Sub(a).MulScalar(t))
}

// strokeDepthInterval clips the linear window depth to [0, 1], returning the
// parameter range on the projected edge.
func strokeDepthInterval(za, zb float64) (float64, float64, bool) {
	t0, t1 := 0.0, 1.0
	d := zb - za
	if d == 0 {
		if za < 0 || za > 1 {
			return 0, 0, false
		}
		return t0, t1, true
	}
	ta := -za / d
	tb := (1 - za) / d
	if ta > tb {
		ta, tb = tb, ta
	}
	if ta > t0 {
		t0 = ta
	}
	if tb < t1 {
		t1 = tb
	}
	if t1 <= t0 {
		return 0, 0, false
	}
	return t0, t1, true
}

// clipStrokeCanvas clips the segment a->b to the canvas rectangle using a
// Liang-Barsky clip, narrowing [t0, t1].
func clipStrokeCanvas(a, b Vector, x0, y0, x1, y1 float64, t0, t1 *float64) bool {
	d := b.Sub(a)
	checks := [4][2]float64{
		{-d.X, a.X - x0},
		{d.X, x1 - a.X},
		{-d.Y, a.Y - y0},
		{d.Y, y1 - a.Y},
	}
	for _, c := range checks {
		p, q := c[0], c[1]
		if p == 0 {
			if q < 0 {
				return false
			}
			continue
		}
		r := q / p
		if p < 0 {
			if r > *t0 {
				*t0 = r
			}
		} else {
			if r < *t1 {
				*t1 = r
			}
		}
		if *t0 > *t1 {
			return false
		}
	}
	return true
}

// buildStrokeJoins creates bevel wedges at vertices where consecutive solid
// parts of the path meet, i.e. where the dash phase is continuous through the
// vertex. Closed paths also consider the seam between the last and first edge.
func (dc *Context) buildStrokeJoins(points []strokePoint, edges []strokePathEdge, style StrokeStyle) []strokeJoin {
	if len(edges) == 0 {
		return nil
	}
	period, dashed := style.strokePeriod()
	eps := style.strokeEpsilon(period)

	total := 0.0
	if style.Closed && len(points) > 1 {
		total = points[len(points)-1].length + strokeScreenDistance(points[len(points)-1].screen, points[0].screen)
	}

	var joins []strokeJoin
	n := len(edges)
	for k := 0; k < n; k++ {
		e1 := &edges[k]
		var e2 *strokePathEdge
		var before, after float64
		if k+1 < n {
			e2 = &edges[k+1]
			before = e1.lenB - eps
			after = e2.lenA
		} else if style.Closed {
			e2 = &edges[0]
			before = total - eps
			after = e2.lenA
		} else {
			// open endpoint: flat cap, no join
			continue
		}

		// a straight-through or fully reversed vertex needs no bevel
		cross := e1.normal.Cross(e2.normal)
		if math.Abs(cross.Z) <= 1e-9 {
			continue
		}
		// the wedge belongs to a vertex inside the frustum depth range
		z := e1.zb
		if z < 0 || z > 1 {
			continue
		}
		// connect only when the dash phase is solid on both sides
		if dashed {
			if !style.strokeOn(before, period) || !style.strokeOn(after, period) {
				continue
			}
		}
		order := e1.order
		if e2.order > order {
			order = e2.order
		}
		joins = append(joins, strokeJoin{
			center: e1.b,
			vertex: points[e1.ib].vertex,
			depth:  z,
			n1:     e1.normal,
			n2:     e2.normal,
			order:  order,
		})
	}
	return joins
}

// coverStrokePiece offers every canvas pixel whose center lies inside the
// stroke quad of a piece and whose projected arc length falls in a solid dash
// run. The dash test uses global arc length, so phase never resets per edge.
func (dc *Context) coverStrokePiece(p *strokePiece, half float64, style StrokeStyle, period float64, dashed bool, add func(int, strokeCandidate)) {
	dx := p.b.X - p.a.X
	dy := p.b.Y - p.a.Y
	xyLen := math.Hypot(dx, dy)
	if xyLen == 0 {
		return
	}
	n := Vector{-dy / xyLen * half, dx / xyLen * half, 0}
	poly := [4]Vector{
		p.a.Add(n),
		p.b.Add(n),
		p.b.Sub(n),
		p.a.Sub(n),
	}
	x0, y0, x1, y1 := dc.strokeScanBounds(poly[:])
	lenDelta := p.lenB - p.lenA
	for y := y0; y < y1; y++ {
		for x := x0; x < x1; x++ {
			point := Vector{float64(x) + 0.5, float64(y) + 0.5, 0}
			if !strokePointInConvex(point, poly[:]) {
				continue
			}
			// parameter of the perpendicular foot, clamped to this solid piece
			t := strokeProjectParameter2D(p.a, p.b, point)
			tFull := p.t0 + t*(p.t1-p.t0)
			if dashed {
				length := p.lenA + lenDelta*tFull
				if !style.strokeOn(length, period) {
					continue
				}
			}
			depth := p.za + tFull*(p.zb-p.za)
			add(y*dc.Width+x, strokeCandidate{
				depth: depth,
				order: p.order,
				seq:   p.seq,
				piece: p,
				t:     tFull,
			})
		}
	}
}

// coverStrokeJoin offers every pixel inside the outer bevel triangle of a
// join. The triangle fills the gap left between the two butt-capped edge
// quads, so edges are never drawn as independent square-capped lines.
func (dc *Context) coverStrokeJoin(j *strokeJoin, half float64, add func(int, strokeCandidate)) {
	// The left normal of each edge points to the inside of a left turn. The
	// outer bevel is therefore on the negative side of a left turn and on the
	// positive side of a right turn.
	var tri [3]Vector
	if j.n1.Cross(j.n2).Z > 0 {
		tri = [3]Vector{j.center, j.center.Sub(j.n1.MulScalar(half)), j.center.Sub(j.n2.MulScalar(half))}
	} else {
		tri = [3]Vector{j.center, j.center.Add(j.n1.MulScalar(half)), j.center.Add(j.n2.MulScalar(half))}
	}
	poly := tri[:]
	x0, y0, x1, y1 := dc.strokeScanBounds(poly)
	for y := y0; y < y1; y++ {
		for x := x0; x < x1; x++ {
			point := Vector{float64(x) + 0.5, float64(y) + 0.5, 0}
			if !strokePointInConvex(point, poly) {
				continue
			}
			add(y*dc.Width+x, strokeCandidate{
				depth: j.depth,
				order: j.order,
				seq:   j.seq,
				join:  j,
			})
		}
	}
}

// strokeScanBounds returns the integer pixel range [x0, x1) x [y0, y1) to
// scan, clamped to the canvas before converting to integers so off-screen
// coordinates cannot overflow.
func (dc *Context) strokeScanBounds(poly []Vector) (int, int, int, int) {
	min := poly[0]
	max := poly[0]
	for _, p := range poly[1:] {
		min = min.Min(p)
		max = max.Max(p)
	}
	minX := Clamp(min.X, 0, float64(dc.Width))
	minY := Clamp(min.Y, 0, float64(dc.Height))
	maxX := Clamp(max.X, 0, float64(dc.Width))
	maxY := Clamp(max.Y, 0, float64(dc.Height))
	x0 := int(math.Floor(minX))
	y0 := int(math.Floor(minY))
	x1 := int(math.Ceil(maxX))
	y1 := int(math.Ceil(maxY))
	if x1 < x0 {
		x1 = x0
	}
	if y1 < y0 {
		y1 = y0
	}
	return x0, y0, x1, y1
}

// strokePointInConvex tests a point against a convex polygon using the sign
// of its edge cross products.
func strokePointInConvex(p Vector, poly []Vector) bool {
	var pos, neg bool
	n := len(poly)
	for i := 0; i < n; i++ {
		a := poly[i]
		b := poly[(i+1)%n]
		cross := (b.X-a.X)*(p.Y-a.Y) - (b.Y-a.Y)*(p.X-a.X)
		if cross > 1e-12 {
			pos = true
		} else if cross < -1e-12 {
			neg = true
		}
		if pos && neg {
			return false
		}
	}
	return true
}

// strokeProjectParameter2D returns the clamped parameter of the foot of the
// perpendicular from p onto the 2D centerline a->b.
func strokeProjectParameter2D(a, b, p Vector) float64 {
	dx := b.X - a.X
	dy := b.Y - a.Y
	l2 := dx*dx + dy*dy
	if l2 == 0 {
		return 0
	}
	return Clamp(((p.X-a.X)*dx+(p.Y-a.Y)*dy)/l2, 0, 1)
}

// strokeInterpolateVertex perspective-correctly interpolates vertex
// attributes along an edge at screen parameter t.
func strokeInterpolateVertex(va, vb Vertex, t float64) Vertex {
	wa := va.Output.W
	wb := vb.Output.W
	if wa == 0 {
		wa = 1
	}
	if wb == 0 {
		wb = 1
	}
	la := (1 - t) / wa
	lb := t / wb
	if la+lb == 0 {
		la, lb = 1-t, t
	}
	s := 1 / (la + lb)
	b := VectorW{la, lb, 0, s}
	return InterpolateVertexes(va, vb, vb, b)
}

// commitStrokePixel applies the fragment shader and the existing depth/color
// commit rules to one deduplicated stroke pixel.
func (dc *Context) commitStrokePixel(vertex Vertex, x, y int, depth float64) RasterizeInfo {
	var info RasterizeInfo
	i := y*dc.Width + x
	if i < 0 || i >= len(dc.DepthBuffer) {
		return info
	}
	bz := depth + dc.DepthBias
	if dc.ReadDepth && bz > dc.DepthBuffer[i] {
		return info
	}
	color := dc.Shader.Fragment(vertex)
	if color == Discard {
		return info
	}
	lock := &dc.locks[(x+y)&255]
	lock.Lock()
	defer lock.Unlock()
	if bz <= dc.DepthBuffer[i] || !dc.ReadDepth {
		info.UpdatedPixels++
		if dc.WriteDepth {
			dc.DepthBuffer[i] = depth
		}
		if dc.WriteColor {
			if dc.AlphaBlend && color.A < 1 {
				sr, sg, sb, sa := color.NRGBA().RGBA()
				a := (0xffff - sa) * 0x101
				j := dc.ColorBuffer.PixOffset(x, y)
				dr := &dc.ColorBuffer.Pix[j+0]
				dg := &dc.ColorBuffer.Pix[j+1]
				db := &dc.ColorBuffer.Pix[j+2]
				da := &dc.ColorBuffer.Pix[j+3]
				*dr = uint8((uint32(*dr)*a/0xffff + sr) >> 8)
				*dg = uint8((uint32(*dg)*a/0xffff + sg) >> 8)
				*db = uint8((uint32(*db)*a/0xffff + sb) >> 8)
				*da = uint8((uint32(*da)*a/0xffff + sa) >> 8)
			} else {
				dc.ColorBuffer.SetNRGBA(x, y, color.NRGBA())
			}
		}
	}
	return info
}
