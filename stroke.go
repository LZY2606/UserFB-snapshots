package fauxgl

import (
	"errors"
	"math"
)

// StrokeStyle configures a stroked polyline. Width and dash lengths are
// measured in final screen pixels. An empty Dashes slice selects a solid
// stroke. Otherwise Dashes must hold a positive, even number of strictly
// positive, finite lengths that alternate between on and off segments.
type StrokeStyle struct {
	Width      float64
	Dashes     []float64
	DashOffset float64
	Closed     bool
}

// strokeNode is one transformed input vertex together with its projected
// screen position and the screen arc length accumulated at that point.
type strokeNode struct {
	vertex Vertex
	s      Vector
	length float64
}

// strokeEdge joins two consecutive stroke nodes.
type strokeEdge struct {
	a, b strokeNode
}

// strokeSlot is the winning coverage of a single pixel. A negative owner
// means the pixel was not covered. Odd owners belong to an edge
// (edge index (owner-1)/2), even owners to the bevel wedge at vertex
// owner/2. f is the screen fraction along the owning edge.
type strokeSlot struct {
	z        float64
	f        float64
	owner    int32
	coverage int32
}

// finiteVectorW reports whether every component is finite and W is positive.
func finiteVectorW(v VectorW) bool {
	return !math.IsNaN(v.X) && !math.IsInf(v.X, 0) &&
		!math.IsNaN(v.Y) && !math.IsInf(v.Y, 0) &&
		!math.IsNaN(v.Z) && !math.IsInf(v.Z, 0) &&
		!math.IsNaN(v.W) && !math.IsInf(v.W, 0) && v.W > 0
}

// strokeDist2 is the planar (X, Y) distance between two screen points; the
// screen Z component stores depth and must not enter arc length.
func strokeDist2(a, b Vector) float64 {
	return math.Hypot(a.X-b.X, a.Y-b.Y)
}

// strokeDir2 returns the planar direction from a to b as a unit vector.
func strokeDir2(a, b Vector) Vector {
	d := Vector{b.X - a.X, b.Y - a.Y, 0}
	return d.Normalize()
}

// strokeValidate checks the style without touching any render buffers.
func strokeValidate(style StrokeStyle) error {
	if math.IsNaN(style.Width) || math.IsInf(style.Width, 0) || style.Width <= 0 {
		return errors.New("fauxgl: stroke width must be finite and positive")
	}
	if math.IsNaN(style.DashOffset) || math.IsInf(style.DashOffset, 0) {
		return errors.New("fauxgl: stroke dash offset must be finite")
	}
	if len(style.Dashes) == 0 {
		return nil
	}
	if len(style.Dashes)%2 != 0 {
		return errors.New("fauxgl: stroke dash pattern must have an even number of entries")
	}
	for _, d := range style.Dashes {
		if math.IsNaN(d) || math.IsInf(d, 0) || d <= 0 {
			return errors.New("fauxgl: stroke dash lengths must be finite and strictly positive")
		}
	}
	return nil
}

// strokeDashOn reports whether the pattern is on at arc length s. Pattern
// intervals are half-open: a length landing exactly on a boundary belongs to
// the interval that starts there.
func strokeDashOn(style StrokeStyle, period, s float64) bool {
	phase := s - style.DashOffset
	phase -= math.Floor(phase/period) * period
	t := 0.0
	for i, d := range style.Dashes {
		if phase < t+d {
			return i%2 == 0
		}
		t += d
	}
	return (len(style.Dashes)-1)%2 == 0
}

// DrawPolyline strokes a polyline with a flat, bevel-joined ribbon whose
// width and dash pattern are expressed in screen pixels. It reuses the
// context shader, depth buffer and color commit rules and does not change
// the behavior of DrawLine.
func (dc *Context) DrawPolyline(vertices []Vertex, style StrokeStyle) (RasterizeInfo, error) {
	if len(vertices) == 0 {
		return RasterizeInfo{}, nil
	}
	if err := strokeValidate(style); err != nil {
		return RasterizeInfo{}, err
	}

	// Run the vertex shader once per input vertex and reject anything the
	// measurement tool cannot place in front of the camera. All validation
	// happens before any buffer is touched.
	vs := make([]Vertex, len(vertices))
	for i, v := range vertices {
		ev := dc.Shader.Vertex(v)
		if !finiteVectorW(ev.Output) {
			return RasterizeInfo{}, errors.New("fauxgl: stroke vertex must transform to finite coordinates with w > 0")
		}
		vs[i] = ev
	}

	// Project the whole path and accumulate screen arc length before any
	// clipping, so portions outside the canvas or frustum still consume dash
	// length. Coincident projected points simply add zero length.
	nodes := make([]strokeNode, len(vs))
	total := 0.0
	for i, ev := range vs {
		ndc := ev.Output.DivScalar(ev.Output.W).Vector()
		s := dc.screenMatrix.MulPosition(ndc)
		if i > 0 {
			total += strokeDist2(s, nodes[i-1].s)
		}
		nodes[i] = strokeNode{ev, s, total}
	}
	edgeCount := len(vs) - 1
	if style.Closed {
		edgeCount = len(vs)
		if len(vs) > 1 {
			total += strokeDist2(nodes[0].s, nodes[len(nodes)-1].s)
		}
	}
	if total == 0 {
		// No non-zero edge: a successful call that draws nothing.
		return RasterizeInfo{}, nil
	}

	edges := make([]strokeEdge, edgeCount)
	for e := 0; e < edgeCount; e++ {
		edges[e].a = nodes[e]
		if e == len(vs)-1 {
			b := nodes[0]
			b.length = total
			edges[e].b = b
		} else {
			edges[e].b = nodes[e+1]
		}
	}

	period := 0.0
	for _, d := range style.Dashes {
		period += d
	}

	slots := make([]strokeSlot, dc.Width*dc.Height)
	for i := range slots {
		slots[i].owner = -1
	}
	half := style.Width / 2

	// Visible parameter ranges per edge after frustum clipping; joins are
	// only emitted where both adjacent edges actually reach the shared node.
	visible := make([][2]float64, edgeCount)
	for e := 0; e < edgeCount; e++ {
		tA, tB, ok := dc.strokeClipEdge(edges[e].a.vertex.Output, edges[e].b.vertex.Output)
		if !ok {
			visible[e] = [2]float64{1, 0}
			continue
		}
		visible[e] = [2]float64{tA, tB}
		dc.strokeRibbon(edges[e].a, edges[e].b, e, tA, tB, half, style, period, slots)
	}

	// Bevel wedges at genuine polyline vertices.
	for k := range nodes {
		incoming, outgoing := -1, -1
		if k > 0 {
			incoming = k - 1
		} else if style.Closed && edgeCount > 0 {
			incoming = edgeCount - 1
		}
		if k < edgeCount {
			outgoing = k
		}
		if incoming < 0 || outgoing < 0 {
			continue // open path end: butt cap only
		}
		if visible[incoming][1] < 1-1e-9 || visible[outgoing][0] > 1e-9 {
			continue // vertex itself is clipped away
		}
		arc := nodes[k].length
		if k == 0 && style.Closed {
			arc = total
		}
		if len(style.Dashes) > 0 && !strokeDashOn(style, period, arc) {
			continue // phase decides whether the seam joins
		}
		dc.strokeWedge(nodes, edges, incoming, outgoing, k, half, slots)
	}

	return dc.strokeCommit(nodes, edges, slots), nil
}

// strokeClipEdge clips one edge against the view volume and returns the
// parameter range [tA, tB] that remains visible. A straight edge stays
// straight on screen after perspective division, so only the range matters.
func (dc *Context) strokeClipEdge(w0, w1 VectorW) (float64, float64, bool) {
	tA, tB := 0.0, 1.0
	for _, plane := range clipPlanes {
		f0 := plane.pointInFront(w0)
		f1 := plane.pointInFront(w1)
		if f0 && f1 {
			continue
		}
		u := w1.Sub(w0)
		d := plane.N.Dot(u)
		if d == 0 {
			return 0, 0, false
		}
		alpha := -plane.N.Dot(w0.Sub(plane.P)) / d
		if f0 {
			if alpha > tA {
				tA = alpha
			}
		} else if f1 {
			if alpha < tB {
				tB = alpha
			}
		} else {
			return 0, 0, false
		}
		if tA > tB+1e-12 {
			return 0, 0, false
		}
	}
	return tA, tB, tA <= tB+1e-12
}

// strokeScreenAt evaluates the screen position of an edge at clip-space
// parameter t. Interpolated clip coordinates stay finite with W > 0 because
// both accepted endpoints have positive W.
func (dc *Context) strokeScreenAt(a, b strokeNode, t float64) Vector {
	w := a.vertex.Output.Add(b.vertex.Output.Sub(a.vertex.Output).MulScalar(t))
	ndc := w.DivScalar(w.W).Vector()
	return dc.screenMatrix.MulPosition(ndc)
}

// strokeRibbon rasterizes the flat ribbon of one visible edge portion.
func (dc *Context) strokeRibbon(a, b strokeNode, e int, tA, tB, half float64, style StrokeStyle, period float64, slots []strokeSlot) {
	sA := dc.strokeScreenAt(a, b, tA)
	sB := dc.strokeScreenAt(a, b, tB)
	d := Vector{sB.X - sA.X, sB.Y - sA.Y, 0}
	length := d.Length()
	if length == 0 {
		return
	}
	dir := d.DivScalar(length)
	perp := Vector{-dir.Y, dir.X, 0}
	owner := int32(2*e + 1)

	// Planar geometry of the full, unclipped projected edge.
	fullLen := strokeDist2(a.s, b.s)
	fullDir := strokeDir2(a.s, b.s)

	lo := sA.Min(sB).SubScalar(half + 1)
	hi := sA.Max(sB).AddScalar(half + 1)
	x0, x1, y0, y1 := dc.strokeBounds(lo, hi)
	for y := y0; y <= y1; y++ {
		for x := x0; x <= x1; x++ {
			p := Vector{float64(x) + 0.5, float64(y) + 0.5, 0}
			rel := p.Sub(sA)
			u := rel.Dot(dir)
			if u < 0 || u > length {
				continue // butt cap: nothing extends past the end
			}
			if math.Abs(rel.Dot(perp)) > half+1e-9 {
				continue
			}
			// Foot point on the center line and its fraction of the full
			// projected edge, so clipping never distorts parameters.
			foot := sA.Add(dir.MulScalar(u))
			f := foot.Sub(a.s).Dot(fullDir) / fullLen
			if f < 0 {
				f = 0
			} else if f > 1 {
				f = 1
			}
			arc := a.length + (b.length-a.length)*f
			if len(style.Dashes) > 0 && !strokeDashOn(style, period, arc) {
				continue
			}
			// Depth is linear in screen space.
			z := a.s.Z + (b.s.Z-a.s.Z)*f
			dc.strokeWin(x, y, z, f, owner, slots)
		}
	}
}

// strokeWedge rasterizes the bevel join at a shared vertex: the convex hull
// of the two ribbon end segments, fanned out from the vertex center.
func (dc *Context) strokeWedge(nodes []strokeNode, edges []strokeEdge, in, out, k int, half float64, slots []strokeSlot) {
	s := nodes[k].s
	if strokeDist2(s, edges[in].a.s) == 0 || strokeDist2(edges[out].b.s, s) == 0 {
		return
	}
	ni := strokeDir2(edges[in].a.s, s)
	no := strokeDir2(s, edges[out].b.s)
	pi := Vector{-ni.Y, ni.X, 0}
	po := Vector{-no.Y, no.X, 0}
	corners := []Vector{
		s.Add(pi.MulScalar(half)), s.Sub(pi.MulScalar(half)),
		s.Add(po.MulScalar(half)), s.Sub(po.MulScalar(half)),
	}
	// Distinct corners, ordered by angle around the vertex.
	hull := strokeAngularHull(s, corners)
	if len(hull) < 3 {
		return
	}
	owner := int32(2 * k)
	z := s.Z
	for i := 0; i < len(hull); i++ {
		dc.strokeTriangle(s, hull[i], hull[(i+1)%len(hull)], z, owner, slots)
	}
}

// strokeAngularHull returns the distinct corner points sorted by polar angle
// around center. All corners sit on the same radius circle.
func strokeAngularHull(center Vector, points []Vector) []Vector {
	type item struct {
		p Vector
		a float64
	}
	items := make([]item, 0, len(points))
	for _, p := range points {
		dup := false
		for _, q := range items {
			if p == q.p {
				dup = true
				break
			}
		}
		if !dup {
			r := p.Sub(center)
			items = append(items, item{p, math.Atan2(r.Y, r.X)})
		}
	}
	for i := 1; i < len(items); i++ {
		for j := i; j > 0 && items[j-1].a > items[j].a; j-- {
			items[j-1], items[j] = items[j], items[j-1]
		}
	}
	out := make([]Vector, len(items))
	for i, it := range items {
		out[i] = it.p
	}
	return out
}

// strokeTriangle fills pixel centers inside triangle (a, b, c).
func (dc *Context) strokeTriangle(a, b, c Vector, z float64, owner int32, slots []strokeSlot) {
	area := edge(b, c, a)
	if math.Abs(area) < 1e-12 {
		return
	}
	sign := 1.0
	if area < 0 {
		sign = -1
	}
	lo := a.Min(b.Min(c)).SubScalar(1)
	hi := a.Max(b.Max(c)).AddScalar(1)
	x0, x1, y0, y1 := dc.strokeBounds(lo, hi)
	for y := y0; y <= y1; y++ {
		for x := x0; x <= x1; x++ {
			p := Vector{float64(x) + 0.5, float64(y) + 0.5, 0}
			if sign*edge(b, c, p) < -1e-9 || sign*edge(c, a, p) < -1e-9 || sign*edge(a, b, p) < -1e-9 {
				continue
			}
			dc.strokeWin(x, y, z, 0, owner, slots)
		}
	}
}

// strokeBounds converts a screen bounding box to clamped pixel indices.
func (dc *Context) strokeBounds(lo, hi Vector) (x0, x1, y0, y1 int) {
	x0 = int(math.Floor(lo.X - 0.5))
	y0 = int(math.Floor(lo.Y - 0.5))
	x1 = int(math.Ceil(hi.X - 0.5))
	y1 = int(math.Ceil(hi.Y - 0.5))
	if x0 < 0 {
		x0 = 0
	}
	if y0 < 0 {
		y0 = 0
	}
	if x1 > dc.Width-1 {
		x1 = dc.Width - 1
	}
	if y1 > dc.Height-1 {
		y1 = dc.Height - 1
	}
	return
}

// strokeWin records coverage for a pixel, keeping the smallest window depth
// and, on equal depth, the owner that appears later in the path.
func (dc *Context) strokeWin(x, y int, z, f float64, owner int32, slots []strokeSlot) {
	i := y*dc.Width + x
	if i < 0 || i >= len(slots) {
		return
	}
	s := &slots[i]
	if s.owner < 0 {
		s.z, s.f, s.owner, s.coverage = z, f, owner, 1
		return
	}
	if z < s.z-1e-12 || (math.Abs(z-s.z) <= 1e-12 && owner > s.owner) {
		s.z, s.f, s.owner = z, f, owner
	}
}

// strokeCommit runs the fragment shader and buffer rules once per
// deduplicated coverage candidate.
func (dc *Context) strokeCommit(nodes []strokeNode, edges []strokeEdge, slots []strokeSlot) RasterizeInfo {
	var info RasterizeInfo
	for i := range slots {
		s := &slots[i]
		if s.owner < 0 {
			continue
		}
		info.TotalPixels += uint64(s.coverage)
		x := i % dc.Width
		y := i / dc.Width
		bz := s.z + dc.DepthBias
		if dc.ReadDepth && bz > dc.DepthBuffer[i] {
			continue
		}

		var v Vertex
		if s.owner%2 == 0 {
			// Bevel wedge: attributes and depth of the owning vertex.
			k := int(s.owner / 2)
			v = nodes[k].vertex
		} else {
			e := int((s.owner - 1) / 2)
			v = strokeInterpolate(edges[e].a.vertex, edges[e].b.vertex, s.f)
		}

		color := dc.Shader.Fragment(v)
		if color == Discard {
			continue
		}
		lock := &dc.locks[(x+y)&255]
		lock.Lock()
		if bz <= dc.DepthBuffer[i] || !dc.ReadDepth {
			info.UpdatedPixels++
			if dc.WriteDepth {
				dc.DepthBuffer[i] = s.z
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
		lock.Unlock()
	}
	return info
}

// strokeInterpolate performs perspective-correct vertex interpolation along
// an edge, where f is the screen fraction (0 at v0, 1 at v1). Since 1/w is
// affine on screen, f is exactly the normalized reciprocal-w weight used by
// the triangle rasterizer.
func strokeInterpolate(v0, v1 Vertex, f float64) Vertex {
	r0 := 1 / v0.Output.W
	r1 := 1 / v1.Output.W
	w := 1 / ((1-f)*r0 + f*r1)
	b0 := (1 - f) * r0 * w
	b1 := f * r1 * w
	v := Vertex{}
	v.Position = v0.Position.MulScalar(b0).Add(v1.Position.MulScalar(b1))
	n := v0.Normal.MulScalar(b0).Add(v1.Normal.MulScalar(b1))
	if n.LengthSquared() > 0 {
		v.Normal = n.Normalize()
	}
	v.Texture = v0.Texture.MulScalar(b0).Add(v1.Texture.MulScalar(b1))
	v.Color = v0.Color.MulScalar(b0).Add(v1.Color.MulScalar(b1))
	v.Output = v0.Output.MulScalar(b0).Add(v1.Output.MulScalar(b1))
	return v
}
