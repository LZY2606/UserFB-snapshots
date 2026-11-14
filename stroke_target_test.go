package fauxgl

import (
	"math"
	"testing"

	"github.com/fogleman/fauxgl/task_support/taskcheck"
)

// ---- test shaders -------------------------------------------------------

// strokeCountShader counts every Fragment invocation.
type strokeCountShader struct {
	matrix Matrix
	calls  *int
	color  Color
}

func (s *strokeCountShader) Vertex(v Vertex) Vertex {
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *strokeCountShader) Fragment(v Vertex) Color {
	*s.calls++
	return s.color
}

// strokeAttrShader turns the Texture.X varying into a grayscale color.
type strokeAttrShader struct {
	matrix Matrix
}

func (s *strokeAttrShader) Vertex(v Vertex) Vertex {
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *strokeAttrShader) Fragment(v Vertex) Color {
	return Color{v.Texture.X, v.Texture.X, v.Texture.X, 1}
}

// strokeDiscardShader discards the second half of the attribute range.
type strokeDiscardShader struct {
	matrix Matrix
	calls  *int
}

func (s *strokeDiscardShader) Vertex(v Vertex) Vertex {
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *strokeDiscardShader) Fragment(v Vertex) Color {
	*s.calls++
	if v.Texture.X > 0.5 {
		return Discard
	}
	return Color{v.Texture.X, v.Texture.X, v.Texture.X, 1}
}

// strokeClipShader takes clip coordinates straight from the vertex:
// Position carries (x, y, z) and Texture.Z carries w.
type strokeClipShader struct {
	calls *int
}

func (s *strokeClipShader) Vertex(v Vertex) Vertex {
	v.Output = VectorW{v.Position.X, v.Position.Y, v.Position.Z, v.Texture.Z}
	return v
}

func (s *strokeClipShader) Fragment(v Vertex) Color {
	if s.calls != nil {
		*s.calls++
	}
	return Color{v.Texture.X, v.Texture.X, v.Texture.X, 1}
}

// ---- helpers ------------------------------------------------------------

func strokeNewContext(w, h int) *Context {
	dc := NewContext(w, h)
	dc.Cull = CullNone
	dc.AlphaBlend = false
	dc.Shader = NewSolidColorShader(Identity(), White)
	return dc
}

// strokeScreenVertex builds an identity-shader vertex at screen position
// (x, y), carrying attribute attr in Texture.X.
func strokeScreenVertex(dc *Context, x, y, attr float64) Vertex {
	ndc := Vector{
		2*x/float64(dc.Width) - 1,
		1 - 2*y/float64(dc.Height),
		0,
	}
	return Vertex{Position: ndc, Texture: Vector{attr, 0, 0}}
}

func strokeClipVertex(x, y, z, w, attr float64) Vertex {
	return Vertex{Position: Vector{x, y, z}, Texture: Vector{attr, 0, w}}
}

// strokeScreenClipVertex builds a clip-space vertex landing exactly at screen
// position (sx, sy) in a w-by-h context, with explicit clip w.
func strokeScreenClipVertex(w, h, sx, sy, z, cw, attr float64) Vertex {
	ndcX := 2*sx/float64(w) - 1
	ndcY := 1 - 2*sy/float64(h)
	return strokeClipVertex(ndcX*cw, ndcY*cw, z, cw, attr)
}

func strokeCovered(dc *Context) map[int]bool {
	out := make(map[int]bool)
	for i := 0; i < len(dc.ColorBuffer.Pix); i += 4 {
		if dc.ColorBuffer.Pix[i+3] != 0 {
			out[i/4] = true
		}
	}
	return out
}

func strokeCountCovered(dc *Context) int {
	return len(strokeCovered(dc))
}

func strokeMapsEqual(a, b map[int]bool) bool {
	if len(a) != len(b) {
		return false
	}
	for k := range a {
		if !b[k] {
			return false
		}
	}
	return true
}

func strokeBuffersSnapshot(dc *Context) ([]uint8, []float64) {
	cp := append([]uint8(nil), dc.ColorBuffer.Pix...)
	dp := append([]float64(nil), dc.DepthBuffer...)
	return cp, dp
}

// ---- TestStrokePhase ----------------------------------------------------

func TestStrokePhase(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "stroke phase group started")

		t.Run("phase across edges and subdivision invariance", func(t *testing.T) {
			dc1 := strokeNewContext(24, 20)
			dc2 := strokeNewContext(24, 20)
			style := StrokeStyle{Width: 2, Dashes: []float64{4, 4}}
			one := []Vertex{
				strokeScreenVertex(dc1, 2, 10, 0),
				strokeScreenVertex(dc1, 20, 10, 1),
			}
			split := []Vertex{
				strokeScreenVertex(dc2, 2, 10, 0),
				strokeScreenVertex(dc2, 8, 10, 0.33),
				strokeScreenVertex(dc2, 14, 10, 0.66),
				strokeScreenVertex(dc2, 20, 10, 1),
			}
			info1, err := dc1.DrawPolyline(one, style)
			c.Check(err == nil, "single edge draw: %v", err)
			info2, err := dc2.DrawPolyline(split, style)
			c.Check(err == nil, "split edge draw: %v", err)
			c.Check(info1.TotalPixels == info2.TotalPixels && info1.TotalPixels > 0,
				"subdividing an edge must not change coverage: %d vs %d", info1.TotalPixels, info2.TotalPixels)
			c.Check(strokeMapsEqual(strokeCovered(dc1), strokeCovered(dc2)),
				"subdividing an edge must not move the dashes")

			// Independently verify the on/off columns from arc length.
			for x := 2; x < 20; x++ {
				arc := float64(x) + 0.5 - 2
				phase := math.Mod(arc, 8)
				wantOn := phase < 4
				idx := 10*dc1.Width + x
				_, got := strokeCovered(dc1)[idx]
				c.Check(got == wantOn, "column %d arc %.1f on=%v want %v", x, arc, got, wantOn)
			}
		})

		t.Run("dash offset sign and period", func(t *testing.T) {
			render := func(offset float64) map[int]bool {
				dc := strokeNewContext(24, 20)
				style := StrokeStyle{Width: 2, Dashes: []float64{6, 2}, DashOffset: offset}
				vs := []Vertex{
					strokeScreenVertex(dc, 2, 10, 0),
					strokeScreenVertex(dc, 20, 10, 1),
				}
				_, err := dc.DrawPolyline(vs, style)
				c.Check(err == nil, "offset %.1f draw: %v", offset, err)
				return strokeCovered(dc)
			}
			// +2 and -6 (=-period+2) are the same phase.
			c.Check(strokeMapsEqual(render(2), render(-6)), "negative offset must wrap by the period")
			c.Check(strokeMapsEqual(render(10), render(2)), "offset beyond one period must wrap")

			// A positive offset starts earlier inside the period: the first
			// on-run is shorter by the offset amount.
			dc := strokeNewContext(24, 20)
			style := StrokeStyle{Width: 1, Dashes: []float64{8, 8}, DashOffset: 3}
			vs := []Vertex{
				strokeScreenVertex(dc, 2.5, 10, 0),
				strokeScreenVertex(dc, 18.5, 10, 1),
			}
			_, err := dc.DrawPolyline(vs, style)
			c.Check(err == nil, "offset draw: %v", err)
			// Arc at column x center is x-2. With offset 3, the first gap
			// starts at arc 11 (8 on - 3 offset + 8 ... = arc 11).
			_, on4 := strokeCovered(dc)[10*dc.Width+6]    // arc 4, phase 1 -> on
			_, off12 := strokeCovered(dc)[10*dc.Width+14] // arc 12, phase 9 -> gap
			c.Check(on4 && !off12, "positive offset must consume the start of the pattern")
		})

		t.Run("half open pattern boundary", func(t *testing.T) {
			dc := strokeNewContext(24, 20)
			style := StrokeStyle{Width: 1, Dashes: []float64{3, 3}}
			vs := []Vertex{
				strokeScreenVertex(dc, 4.5, 10, 0), // pixel centers land on integer arcs
				strokeScreenVertex(dc, 16.5, 10, 1),
			}
			_, err := dc.DrawPolyline(vs, style)
			c.Check(err == nil, "boundary draw: %v", err)
			_, atGapBoundary := strokeCovered(dc)[10*dc.Width+7] // arc 3: gap starts, excluded
			_, atOnBoundary := strokeCovered(dc)[10*dc.Width+10] // arc 6: on starts, included
			c.Check(!atGapBoundary && atOnBoundary, "boundaries are left-closed, right-open")
		})

		t.Run("closed loop seam keeps phase", func(t *testing.T) {
			dc := strokeNewContext(24, 20)
			style := StrokeStyle{Width: 2, Dashes: []float64{5, 3}, Closed: true}
			vs := []Vertex{
				strokeScreenVertex(dc, 4, 4, 0),
				strokeScreenVertex(dc, 20, 4, 1),
				strokeScreenVertex(dc, 20, 16, 2),
				strokeScreenVertex(dc, 4, 16, 3),
			}
			info, err := dc.DrawPolyline(vs, style)
			c.Check(err == nil, "closed draw: %v", err)
			c.Check(info.TotalPixels > 0, "closed loop should render")
			// The closing edge exists: pixels just left of the first vertex
			// column on the top row must be reachable.
			covered := strokeCovered(dc)
			seamLit := false
			for x := 0; x < 4; x++ {
				if covered[4*dc.Width+x] {
					seamLit = true
				}
			}
			c.Check(seamLit, "closing edge must continue the dash pattern around the seam")

			// Shifting the offset by exactly one period changes nothing,
			// proving the seam does not reset the phase.
			dc2 := strokeNewContext(24, 20)
			style2 := style
			style2.DashOffset = 8
			_, err = dc2.DrawPolyline(vs, style2)
			c.Check(err == nil, "shifted closed draw: %v", err)
			c.Check(strokeMapsEqual(covered, strokeCovered(dc2)), "seam phase must be periodic, not reset per edge")
		})

		t.Run("off canvas and near plane still consume length", func(t *testing.T) {
			// First segment runs far outside the canvas; the visible second
			// segment must start mid-pattern because the hidden part counts.
			dc := strokeNewContext(24, 20)
			style := StrokeStyle{Width: 2, Dashes: []float64{4, 4}}
			vs := []Vertex{
				strokeScreenVertex(dc, -20, 10, 0), // 22 px off-canvas
				strokeScreenVertex(dc, 2, 10, 0.5),
				strokeScreenVertex(dc, 20, 10, 1),
			}
			_, err := dc.DrawPolyline(vs, style)
			c.Check(err == nil, "off-canvas draw: %v", err)
			covered := strokeCovered(dc)
			// Arc at the first visible column center: 22 + 0.5 = 22.5,
			// phase 22.5 mod 8 = 6.5 -> gap.
			_, first := covered[10*dc.Width+2]
			c.Check(!first, "off-canvas length must be consumed (first visible column in gap), got on=%v", first)
			// Arc 25.5 at column 5 -> phase 1.5 -> on.
			_, later := covered[10*dc.Width+5]
			c.Check(later, "pattern must resume after the consumed gap")

			// Near-plane clipping: an endpoint behind the camera still lets
			// the visible remainder render with the full edge length counted.
			dc2 := strokeNewContext(24, 20)
			dc2.Shader = &strokeClipShader{}
			// Edge from behind the near plane to an on-screen point; the clip
			// coordinates are finite with w > 0 at every accepted vertex.
			near := []Vertex{
				strokeClipVertex(-0.9, 0, -1.5, 1, 0), // z < -w: clipped
				strokeClipVertex(0.9, 0, 0, 1, 1),
			}
			info, err := dc2.DrawPolyline(near, StrokeStyle{Width: 2, Dashes: []float64{100, 1}})
			c.Check(err == nil, "near clip draw: %v", err)
			c.Check(strokeCountCovered(dc2) > 0, "near-clipped edge should still draw its visible part")
			c.Check(info.UpdatedPixels > 0, "near-clipped edge should commit pixels")
		})

		t.Run("empty degenerate and invalid input", func(t *testing.T) {
			dc := strokeNewContext(16, 16)
			info, err := dc.DrawPolyline(nil, StrokeStyle{Width: 2})
			c.Check(err == nil && info.TotalPixels == 0 && info.UpdatedPixels == 0,
				"empty input returns zero counts, got %v err %v", info, err)

			dup := []Vertex{
				strokeScreenVertex(dc, 4, 4, 0),
				strokeScreenVertex(dc, 4, 4, 1),
				strokeScreenVertex(dc, 4, 4, 2),
			}
			info, err = dc.DrawPolyline(dup, StrokeStyle{Width: 2})
			c.Check(err == nil && info.TotalPixels == 0, "all-coincident path draws nothing, got %v", info)

			bad := []struct {
				name  string
				style StrokeStyle
			}{
				{"zero width", StrokeStyle{Width: 0}},
				{"negative width", StrokeStyle{Width: -1}},
				{"nan width", StrokeStyle{Width: math.NaN()}},
				{"odd dashes", StrokeStyle{Width: 2, Dashes: []float64{1, 2, 3}}},
				{"zero dash", StrokeStyle{Width: 2, Dashes: []float64{0, 2}}},
				{"nan dash", StrokeStyle{Width: 2, Dashes: []float64{1, math.NaN()}}},
				{"nan offset", StrokeStyle{Width: 2, Dashes: []float64{1, 1}, DashOffset: math.NaN()}},
			}
			vs := []Vertex{
				strokeScreenVertex(dc, 2, 2, 0),
				strokeScreenVertex(dc, 10, 10, 1),
			}
			for _, b := range bad {
				_, err := dc.DrawPolyline(vs, b.style)
				c.Check(err != nil, "%s must be rejected", b.name)
			}

			// Invalid transform results: buffers must stay untouched.
			pix0, dep0 := strokeBuffersSnapshot(dc)
			invalid := []Vertex{
				{Position: Vector{math.NaN(), 0, 0}},
				strokeScreenVertex(dc, 10, 10, 1),
			}
			_, err = dc.DrawPolyline(invalid, StrokeStyle{Width: 2})
			c.Check(err != nil, "non-finite vertex must be rejected")
			pix1, dep1 := strokeBuffersSnapshot(dc)
			c.Check(equalBytes(pix0, pix1) && equalFloats(dep0, dep1),
				"failed draw must not modify the buffers")

			zeroW := []Vertex{
				strokeClipVertex(0, 0, 0, 0, 0),
				strokeClipVertex(0.5, 0.5, 0, 1, 1),
			}
			dc.Shader = &strokeClipShader{}
			_, err = dc.DrawPolyline(zeroW, StrokeStyle{Width: 2})
			c.Check(err != nil, "w == 0 must be rejected")
			negW := []Vertex{
				strokeClipVertex(0, 0, 0, -1, 0),
				strokeClipVertex(0.5, 0.5, 0, 1, 1),
			}
			_, err = dc.DrawPolyline(negW, StrokeStyle{Width: 2})
			c.Check(err != nil, "w < 0 must be rejected")
		})
	})
}

// ---- TestStrokeJoins ----------------------------------------------------

func TestStrokeJoins(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "stroke joins group started")

		t.Run("butt caps are flat", func(t *testing.T) {
			dc := strokeNewContext(24, 20)
			vs := []Vertex{
				strokeScreenVertex(dc, 6, 10, 0),
				strokeScreenVertex(dc, 18, 10, 1),
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 3})
			c.Check(err == nil, "solid draw: %v", err)
			covered := strokeCovered(dc)
			c.Check(info.TotalPixels == uint64(len(covered)), "candidate count matches covered pixels")
			// No pixel may extend past either endpoint column.
			for idx := range covered {
				x := idx % dc.Width
				c.Check(x >= 6 && x <= 17, "butt cap leaked past endpoint at column %d", x)
			}
			// Exact footprint: 12 columns x 4 rows (half width 1.5).
			c.Check(len(covered) == 12*4, "flat ribbon footprint is %d pixels", len(covered))
		})

		t.Run("bevel wedge fills only the join", func(t *testing.T) {
			// Shallow 45-degree turn where the bevel triangle has pixels that
			// neither rectangular ribbon covers on its own.
			turn := func(dc *Context) []Vertex {
				return []Vertex{
					strokeScreenVertex(dc, 4, 10, 0),
					strokeScreenVertex(dc, 12, 10, 1),
					strokeScreenVertex(dc, 17.657, 4.343, 2),
				}
			}
			style := StrokeStyle{Width: 4}

			joined := strokeNewContext(24, 20)
			_, err := joined.DrawPolyline(turn(joined), style)
			c.Check(err == nil, "joined draw: %v", err)

			edge0 := strokeNewContext(24, 20)
			_, err = edge0.DrawPolyline(turn(edge0)[:2], style)
			c.Check(err == nil, "edge0 draw: %v", err)
			edge1 := strokeNewContext(24, 20)
			_, err = edge1.DrawPolyline(turn(edge1)[1:], style)
			c.Check(err == nil, "edge1 draw: %v", err)

			jc, c0, c1 := strokeCovered(joined), strokeCovered(edge0), strokeCovered(edge1)
			wedgeOnly := 0
			for idx := range jc {
				if !c0[idx] && !c1[idx] {
					wedgeOnly++
				}
			}
			c.Check(wedgeOnly > 0, "bevel wedge contributes %d join-only pixels", wedgeOnly)

			// No miter spike: well outside the wedge radius stays empty.
			_, spike := jc[2*joined.Width+13]
			c.Check(!spike, "bevel join must not extend a miter spike")

			// The join region is still a subset of the bevel hull: every
			// covered pixel sits within half-width+1 of a center line.
			for idx := range jc {
				x, y := float64(idx%joined.Width)+0.5, float64(idx/joined.Width)+0.5
				d0 := SegmentDistanceXY(x, y, 4, 10, 12, 10)
				d1 := SegmentDistanceXY(x, y, 12, 10, 17.657, 4.343)
				c.Check(math.Min(d0, d1) <= 2+1e-9, "pixel (%v,%v) outside bevel hull, d=%v", x, y, math.Min(d0, d1))
			}
		})

		t.Run("overlapping reverse edges submit once", func(t *testing.T) {
			calls := 0
			dc := strokeNewContext(24, 20)
			dc.Shader = &strokeCountShader{matrix: Identity(), calls: &calls, color: White}
			vs := []Vertex{
				strokeScreenVertex(dc, 4, 10, 0),
				strokeScreenVertex(dc, 18, 10, 1),
				strokeScreenVertex(dc, 18, 10, 2),
				strokeScreenVertex(dc, 4, 10, 3),
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 3})
			c.Check(err == nil, "overlap draw: %v", err)
			covered := strokeCountCovered(dc)
			c.Check(int(info.TotalPixels) == covered, "overlap candidates are deduplicated")
			c.Check(calls == covered, "fragment runs once per covered pixel: calls=%d covered=%d", calls, covered)

			single := strokeNewContext(24, 20)
			info2, err := single.DrawPolyline(vs[:2], StrokeStyle{Width: 3})
			c.Check(err == nil, "single draw: %v", err)
			c.Check(info.TotalPixels == info2.TotalPixels, "back-and-forth edges cover exactly the single line")
		})

		t.Run("seam joins only when the phase is on", func(t *testing.T) {
			rect := func(dc *Context) []Vertex {
				return []Vertex{
					strokeScreenVertex(dc, 6, 6, 0),
					strokeScreenVertex(dc, 18, 6, 1),
					strokeScreenVertex(dc, 18, 14, 2),
					strokeScreenVertex(dc, 6, 14, 3),
				}
			}
			// Perimeter 48; pick a pattern whose seam lands in a gap.
			onStyle := StrokeStyle{Width: 3, Dashes: []float64{48, 48}, Closed: true}
			offStyle := StrokeStyle{Width: 3, Dashes: []float64{48, 48}, DashOffset: 48, Closed: true}

			dcOn := strokeNewContext(24, 20)
			_, err := dcOn.DrawPolyline(rect(dcOn), onStyle)
			c.Check(err == nil, "on-seam draw: %v", err)
			dcOff := strokeNewContext(24, 20)
			_, err = dcOff.DrawPolyline(rect(dcOff), offStyle)
			c.Check(err == nil, "off-seam draw: %v", err)

			on, off := strokeCovered(dcOn), strokeCovered(dcOff)
			c.Check(len(on) > len(off), "a gap-phase seam renders fewer pixels: %d vs %d", len(on), len(off))
			// With the seam in a gap, nothing around the first vertex may be
			// covered by the closing edge.
			litNearSeam := false
			for y := 4; y <= 8; y++ {
				for x := 4; x < 6; x++ {
					if off[y*dcOff.Width+x] {
						litNearSeam = true
					}
				}
			}
			c.Check(!litNearSeam, "gap-phase seam must not connect")

			// Translucent closed loop: the seam must not darken twice.
			calls := 0
			dcA := NewContext(24, 20)
			dcA.Cull = CullNone
			dcA.ClearColorBuffer()
			dcA.Shader = &strokeCountShader{matrix: Identity(), calls: &calls, color: Color{1, 1, 1, 0.5}}
			info, err := dcA.DrawPolyline(rect(dcA), StrokeStyle{Width: 3, Closed: true})
			c.Check(err == nil, "alpha seam draw: %v", err)
			c.Check(calls == int(info.TotalPixels), "seam pixels get one fragment each: %d vs %d", calls, info.TotalPixels)
			for idx := range strokeCovered(dcA) {
				j := idx * 4
				c.Check(dcA.ColorBuffer.Pix[j+3] == 127, "single half-transparent blend at seam pixel, got %d", dcA.ColorBuffer.Pix[j+3])
			}
		})
	})
}

// ---- TestStrokeDepth ----------------------------------------------------

func TestStrokeDepth(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "stroke depth group started")

		t.Run("perspective correct attributes with differing w", func(t *testing.T) {
			dc := NewContext(21, 21)
			dc.Cull = CullNone
			dc.AlphaBlend = false
			dc.Shader = &strokeClipShader{}
			// Endpoints at screen x 1.5 and 19.5, so screen midpoint 10.5 is
			// the center of column 10. w goes 1 -> 3, attribute 0 -> 1.
			vs := []Vertex{
				strokeScreenClipVertex(21, 21, 1.5, 10.5, 0, 1, 0),
				strokeScreenClipVertex(21, 21, 19.5, 10.5, 0, 3, 1),
			}
			_, err := dc.DrawPolyline(vs, StrokeStyle{Width: 1})
			c.Check(err == nil, "perspective draw: %v", err)
			// At screen fraction 0.5 the perspective-correct attribute is
			// (0.5/3) / (0.5 + 0.5/3) = 0.25.
			j := dc.ColorBuffer.PixOffset(10, 10)
			got := dc.ColorBuffer.Pix[j]
			c.Check(got == 63, "perspective attribute at midpoint = %d, want 63", got)
		})

		t.Run("depth is linear on screen", func(t *testing.T) {
			dc := NewContext(21, 21)
			dc.Cull = CullNone
			dc.AlphaBlend = false
			dc.Shader = &strokeClipShader{}
			// Equal w: the screen midpoint column is exactly the midpoint.
			vs := []Vertex{
				strokeScreenClipVertex(21, 21, 1.5, 10.5, -0.6, 1, 0),
				strokeScreenClipVertex(21, 21, 19.5, 10.5, 0.8, 1, 1),
			}
			_, err := dc.DrawPolyline(vs, StrokeStyle{Width: 1})
			c.Check(err == nil, "depth draw: %v", err)
			z0 := 0.5*(-0.6) + 0.5
			z1 := 0.5*(0.8) + 0.5
			want := (z0 + z1) / 2
			got := dc.DepthBuffer[10*dc.Width+10]
			c.Check(math.Abs(got-want) < 1e-12, "screen-linear depth = %.6f want %.6f", got, want)
		})

		t.Run("crossing edges resolve by depth then path order", func(t *testing.T) {
			// Two diagonals crossing at the center pixel (10.5, 10.5) of a
			// 21x21 context. Edge 0 is farther; edge 1 is nearer.
			ndc := func(sx, sy float64) (float64, float64) {
				return 2*sx/21 - 1, 1 - 2*sy/21
			}
			build := func(z0, z1 float64) *Context {
				dc := NewContext(21, 21)
				dc.Cull = CullNone
				dc.AlphaBlend = false
				dc.Shader = &strokeClipShader{}
				x0, y0 := ndc(2.5, 2.5)
				x1, y1 := ndc(18.5, 18.5)
				x2, y2 := ndc(2.5, 18.5)
				x3, y3 := ndc(18.5, 2.5)
				vs := []Vertex{
					strokeClipVertex(x0, y0, z0, 1, 0.2),
					strokeClipVertex(x1, y1, z0, 1, 0.2),
					strokeClipVertex(x2, y2, z1, 1, 0.8),
					strokeClipVertex(x3, y3, z1, 1, 0.8),
				}
				_, err := dc.DrawPolyline(vs, StrokeStyle{Width: 1})
				c.Check(err == nil, "crossing draw: %v", err)
				return dc
			}
			dc := build(0.4, -0.4) // edge 1 nearer
			j := dc.ColorBuffer.PixOffset(10, 10)
			c.Check(dc.ColorBuffer.Pix[j] == uint8(0.8*255), "nearer edge wins the crossing, got %d", dc.ColorBuffer.Pix[j])

			// Equal depth: the later edge in the path wins.
			dc2 := build(0, 0)
			j2 := dc2.ColorBuffer.PixOffset(10, 10)
			c.Check(dc2.ColorBuffer.Pix[j2] == uint8(0.8*255), "equal depth picks the later edge, got %d", dc2.ColorBuffer.Pix[j2])
			c.Check(math.Abs(dc2.DepthBuffer[10*21+10]-0.5) < 1e-12, "tie keeps the shared depth")
		})

		t.Run("discard and opaque occlusion go through the commit rules", func(t *testing.T) {
			// An opaque triangle sits in front of the whole stroke.
			dc := NewContext(21, 21)
			dc.Cull = CullNone
			dc.AlphaBlend = false
			dc.Shader = NewSolidColorShader(Identity(), Black)
			tri := NewTriangleForPoints(
				Vector{-4, -2, -0.8}, Vector{4, -2, -0.8}, Vector{0, 2, -0.8})
			dc.DrawTriangle(tri)

			calls := 0
			dc.Shader = &strokeDiscardShader{matrix: Identity(), calls: &calls}
			vs := []Vertex{
				{Position: Vector{-0.9, 0, 0.2}, Texture: Vector{0, 0, 0}},
				{Position: Vector{0.9, 0, 0.2}, Texture: Vector{1, 0, 0}},
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 3})
			c.Check(err == nil, "occluded draw: %v", err)
			c.Check(info.TotalPixels > 0, "occluded stroke still reports coverage candidates")
			c.Check(info.UpdatedPixels == 0, "occluded pixels are never committed")
			c.Check(calls == 0, "depth-failed pixels never reach the fragment shader")

			// Without the occluder, discard trims commits but still invokes
			// the fragment shader for every candidate.
			dc2 := NewContext(21, 21)
			dc2.Cull = CullNone
			dc2.AlphaBlend = false
			calls2 := 0
			dc2.Shader = &strokeDiscardShader{matrix: Identity(), calls: &calls2}
			info2, err := dc2.DrawPolyline(vs, StrokeStyle{Width: 3})
			c.Check(err == nil, "discard draw: %v", err)
			c.Check(calls2 == int(info2.TotalPixels), "every candidate reaches the fragment once: %d calls vs %d", calls2, info2.TotalPixels)
			c.Check(info2.UpdatedPixels > 0 && info2.UpdatedPixels < info2.TotalPixels,
				"discard trims updates: %d of %d", info2.UpdatedPixels, info2.TotalPixels)
		})
	})
}

// ---- small generic helpers ----------------------------------------------

func equalBytes(a, b []uint8) bool {
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

func equalFloats(a, b []float64) bool {
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

// SegmentDistanceXY is the 2D point-to-segment distance in screen space.
func SegmentDistanceXY(px, py, ax, ay, bx, by float64) float64 {
	dx, dy := bx-ax, by-ay
	l2 := dx*dx + dy*dy
	if l2 == 0 {
		return math.Hypot(px-ax, py-ay)
	}
	t := ((px-ax)*dx + (py-ay)*dy) / l2
	if t < 0 {
		t = 0
	} else if t > 1 {
		t = 1
	}
	return math.Hypot(px-(ax+t*dx), py-(ay+t*dy))
}
