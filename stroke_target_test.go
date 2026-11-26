package fauxgl

import (
	"math"
	"testing"

	"github.com/fogleman/fauxgl/task_support/taskcheck"
)

// testProbeShader forwards vertices through a matrix, records every
// Vertex/Fragment invocation, and can discard fragments in a screen rectangle.
type testProbeShader struct {
	matrix    Matrix
	width     int
	height    int
	vertex    int
	fragment  int
	discarded int
	color     Color
	discard   *testRect
}

type testRect struct {
	x0, y0, x1, y1 int
}

func (s *testProbeShader) Vertex(v Vertex) Vertex {
	s.vertex++
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *testProbeShader) Fragment(v Vertex) Color {
	s.fragment++
	if s.discard != nil {
		ndc := v.Output.DivScalar(v.Output.W).Vector()
		x := int(ndc.X*0.5*float64(s.width) + 0.5*float64(s.width))
		y := int(-ndc.Y*0.5*float64(s.height) + 0.5*float64(s.height))
		if x >= s.discard.x0 && x < s.discard.x1 && y >= s.discard.y0 && y < s.discard.y1 {
			s.discarded++
			return Discard
		}
	}
	return s.color
}

const (
	testWidth  = 200
	testHeight = 80
)

// testScene builds a context with an identity transform, so a position in
// [-1,1] maps directly to normalized device coordinates.
func testScene(w, h int) (*Context, *testProbeShader) {
	dc := NewContext(w, h)
	dc.Cull = CullNone
	dc.AlphaBlend = false
	sh := &testProbeShader{matrix: Identity(), width: w, height: h, color: Color{1, 1, 1, 1}}
	dc.Shader = sh
	return dc, sh
}

// testScreenX converts an NDC x coordinate to a screen x for a given width.
func testScreenX(ndcX float64, w int) float64 {
	return ndcX*0.5*float64(w) + 0.5*float64(w)
}

// testNDCX converts a screen x to NDC x for a given width.
func testNDCX(screenX float64, w int) float64 {
	return (screenX - float64(w)/2) / (float64(w) / 2)
}

// testNDCY converts a screen y to NDC y for a given height.
func testNDCY(screenY float64, h int) float64 {
	return -(screenY - float64(h)/2) / (float64(h) / 2)
}

// testRow reports which pixels are painted on one scanline.
func testRow(dc *Context, y int) []bool {
	row := make([]bool, dc.Width)
	for x := 0; x < dc.Width; x++ {
		row[x] = dc.DepthBuffer[y*dc.Width+x] < math.MaxFloat64
	}
	return row
}

// testFirstPainted returns the number of the first and last painted pixel.
func testSpan(row []bool) (int, int) {
	first, last := -1, -1
	for x, on := range row {
		if on {
			if first < 0 {
				first = x
			}
			last = x
		}
	}
	return first, last
}

// TestStrokePhase covers dash phase over the whole projected path: phase
// accumulation across many edges, closed seams, invariance to added vertices,
// off-canvas length accounting, near-plane clipping, and the exact
// left-closed/right-open period boundary.
func TestStrokePhase(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "phase group runs")

		t.Run("AcrossEdges", func(t *testing.T) {
			// one long horizontal line, drawn directly and split into many
			// collinear vertices; dashes must land identically
			direct := testRow(mustDashRow(t, []float64{-0.95, 0.95}, 4, nil), testHeight/2)
			split := testRow(mustDashRow(t, []float64{-0.95, 0.95}, 4, []float64{-0.6, -0.2, 0.1, 0.4, 0.7}), testHeight/2)
			diff := 0
			for x := range direct {
				if direct[x] != split[x] {
					diff++
				}
			}
			c.Check(diff == 0, "dash phase changed when the edge was split (diff=%d)", diff)
			// a dashed line really has gaps and solid runs
			on, off := 0, 0
			for _, v := range direct {
				if v {
					on++
				} else {
					off++
				}
			}
			c.Check(on > 0, "no painted pixels")
			c.Check(off > 0, "dash pattern produced no gaps")
		})

		t.Run("ClosedSeam", func(t *testing.T) {
			// the closing edge must continue the dash phase from the last
			// vertex instead of restarting at zero; compare against an open
			// path whose first and last vertices are duplicated, which has
			// the same geometry but no seam join
			square := []Vertex{
				{Position: V(-0.5, -0.5, 0)},
				{Position: V(0.5, -0.5, 0)},
				{Position: V(0.5, 0.5, 0)},
				{Position: V(-0.5, 0.5, 0)},
			}
			dc, _ := testScene(testWidth, testHeight)
			closed, err := dc.DrawPolyline(square, StrokeStyle{Width: 2, Dashes: []float64{13, 7}, Closed: true})
			c.Check(err == nil, "closed stroke failed: %v", err)
			closedPixels := paintedCount(dc)

			// an open path missing the final segment paints less, so the
			// closed seam must add real painted pixels
			dc2, _ := testScene(testWidth, testHeight)
			_, err = dc2.DrawPolyline(square, StrokeStyle{Width: 2, Dashes: []float64{13, 7}})
			c.Check(err == nil, "open stroke failed: %v", err)
			openPixels := paintedCount(dc2)
			c.Check(closedPixels > openPixels, "closing segment painted nothing (closed=%d open=%d)", closedPixels, openPixels)
			c.Check(closed.TotalPixels > 0, "closed dashes covered nothing")

			// the seam is affected by DashOffset exactly like the rest of the
			// path: shifting by one full period reproduces the same coverage
			period := 20.0
			dc3, _ := testScene(testWidth, testHeight)
			shifted, err := dc3.DrawPolyline(square, StrokeStyle{Width: 2, Dashes: []float64{13, 7}, Closed: true, DashOffset: period})
			c.Check(err == nil, "shifted closed stroke failed: %v", err)
			a := testAllRows(dc, testHeight, testWidth)
			b := testAllRows(dc3, testHeight, testWidth)
			diff := 0
			for i := range a {
				if a[i] != b[i] {
					diff++
				}
			}
			c.Check(diff == 0, "offset by one period changed coverage (diff=%d, shifted=%d)", diff, shifted.TotalPixels)
		})

		t.Run("AddedVerticesPerspective", func(t *testing.T) {
			// three-dimensional polyline seen in perspective; subdividing each
			// edge must not move the dashes
			matrix := LookAt(V(-1, 0.5, 4), V(0, 0, 0), V(0, 1, 0)).Perspective(50, 2, 1, 20)
			base := []Vector{
				V(-0.9, -0.4, -0.5),
				V(0.3, 0.2, 0.1),
				V(-0.2, 0.8, 0.4),
				V(0.9, -0.6, -0.3),
			}
			render := func(sub int) []bool {
				dc := NewContext(200, 80)
				dc.Cull = CullNone
				dc.AlphaBlend = false
				dc.Shader = NewSolidColorShader(matrix, Color{1, 1, 1, 1})
				var vs []Vertex
				for i := 0; i+1 < len(base); i++ {
					for j := 0; j <= sub; j++ {
						if i > 0 && j == 0 {
							continue
						}
						t := float64(j) / float64(sub)
						vs = append(vs, Vertex{Position: base[i].Lerp(base[i+1], t)})
					}
				}
				_, err := dc.DrawPolyline(vs, StrokeStyle{Width: 2, Dashes: []float64{9, 7}})
				c.Check(err == nil, "perspective stroke failed: %v", err)
				return testAllRows(dc, 80, 200)
			}
			one := render(1)
			eight := render(8)
			thirty := render(30)
			diff := 0
			for i := range one {
				if one[i] != eight[i] || one[i] != thirty[i] {
					diff++
				}
			}
			c.Check(diff == 0, "adding collinear vertices moved perspective dashes (diff=%d)", diff)
		})

		t.Run("OffCanvasLength", func(t *testing.T) {
			// a path that starts well outside the canvas must still consume
			// dash length there; the visible pattern must match a path that
			// begins at the canvas edge but whose pattern is advanced by the
			// same amount
			const w = 200
			outStart := V(testNDCX(-200, w), 0, 0)
			inStart := V(testNDCX(20, w), 0, 0)
			end := V(testNDCX(180, w), 0, 0)
			// screen distance from x=-200 to x=20 is 220px, consumed before
			// the visible part starts; a positive DashOffset starts further
			// into the pattern, matching that advance
			offset := 220.0

			dc1, _ := testScene(w, testHeight)
			_, err := dc1.DrawPolyline([]Vertex{{Position: outStart}, {Position: end}}, StrokeStyle{Width: 2, Dashes: []float64{20, 10}})
			c.Check(err == nil, "off-canvas stroke failed: %v", err)
			dc2, _ := testScene(w, testHeight)
			_, err = dc2.DrawPolyline([]Vertex{{Position: inStart}, {Position: end}}, StrokeStyle{Width: 2, Dashes: []float64{20, 10}, DashOffset: offset})
			c.Check(err == nil, "offset stroke failed: %v", err)

			row1 := testRow(dc1, testHeight/2)
			row2 := testRow(dc2, testHeight/2)
			diff := 0
			for x := 20; x < 180; x++ {
				if row1[x] != row2[x] {
					diff++
				}
			}
			c.Check(diff == 0, "off-canvas run did not consume dash length (diff=%d)", diff)
			// and the off-canvas path still painted something in view
			painted := 0
			for _, v := range row1 {
				if v {
					painted++
				}
			}
			c.Check(painted > 0, "off-canvas stroke painted nothing on canvas")
		})

		t.Run("NearPlaneLength", func(t *testing.T) {
			// a segment crossing the near plane: the clipped-away part must
			// still advance the dash phase
			matrix := LookAt(V(0, 0, 0), V(0, 0, -1), V(0, 1, 0)).Perspective(60, 1, 1, 10)
			a := V(0.3, 0, -0.5) // behind the near plane
			b := V(1.2, 0.3, -8)
			dc1 := NewContext(100, 100)
			dc1.Cull = CullNone
			dc1.AlphaBlend = false
			dc1.Shader = NewSolidColorShader(matrix, Color{1, 1, 1, 1})
			info1, err := dc1.DrawPolyline([]Vertex{{Position: a}, {Position: b}}, StrokeStyle{Width: 2, Dashes: []float64{15, 10}})
			c.Check(err == nil, "near-clipped stroke failed: %v", err)

			// inserting an explicit vertex at the near-plane crossing must
			// produce identical coverage
			oa := matrix.MulPositionW(a)
			ob := matrix.MulPositionW(b)
			fa := oa.Z + oa.W
			fb := ob.Z + ob.W
			u := fa / (fa - fb)
			crossing := a.Lerp(b, u)
			dc2 := NewContext(100, 100)
			dc2.Cull = CullNone
			dc2.AlphaBlend = false
			dc2.Shader = NewSolidColorShader(matrix, Color{1, 1, 1, 1})
			info2, err := dc2.DrawPolyline([]Vertex{{Position: a}, {Position: crossing}, {Position: b}}, StrokeStyle{Width: 2, Dashes: []float64{15, 10}})
			c.Check(err == nil, "near-split stroke failed: %v", err)

			diff := 0
			for i := range dc1.DepthBuffer {
				if (dc1.DepthBuffer[i] < math.MaxFloat64) != (dc2.DepthBuffer[i] < math.MaxFloat64) {
					diff++
				}
			}
			c.Check(diff == 0, "near-plane clipping reset the dash phase (diff=%d)", diff)
			c.Check(info1.TotalPixels > 0, "near-clipped stroke covered nothing")
			c.Check(info2.TotalPixels == info1.TotalPixels, "near-split coverage differs (%d vs %d)", info2.TotalPixels, info1.TotalPixels)
		})

		t.Run("BoundaryLeftClosedRightOpen", func(t *testing.T) {
			// start a dash exactly at screen x=10.5 so pixel centers sit on
			// the transitions; a run [0,20) paints x centers < 30.5
			const w = 200
			x0 := 10.5
			x1 := 150.5
			dc, _ := testScene(w, testHeight)
			vs := []Vertex{
				{Position: V((x0-float64(w)/2)/(float64(w)/2), 0, 0)},
				{Position: V((x1-float64(w)/2)/(float64(w)/2), 0, 0)},
			}
			_, err := dc.DrawPolyline(vs, StrokeStyle{Width: 2, Dashes: []float64{20, 10}})
			c.Check(err == nil, "boundary stroke failed: %v", err)
			row := testRow(dc, testHeight/2)
			// dash runs are [10.5,30.5), [40.5,60.5), ...; pixel centers at
			// 11.5..29.5 are painted, 30.5 and beyond are not
			c.Check(row[10], "pixel before the dash start was painted")
			c.Check(row[29], "pixel just inside the first dash was not painted")
			c.Check(!row[30], "pixel exactly at the dash end was painted (must be right-open)")
			c.Check(!row[31], "pixel inside the gap was painted")
			c.Check(row[40], "pixel just after the gap was not painted")
			c.Check(row[59], "pixel just inside the second dash was not painted")
			c.Check(!row[60], "second dash end was not right-open")
		})

		t.Run("CoincidentAndDegenerate", func(t *testing.T) {
			// repeating a projected point consumes no length; dash phase is
			// unaffected
			dc1, _ := testScene(testWidth, testHeight)
			base := []Vertex{{Position: V(-0.8, 0, 0)}, {Position: V(0, 0, 0)}, {Position: V(0.8, 0, 0)}}
			_, err := dc1.DrawPolyline(base, StrokeStyle{Width: 2, Dashes: []float64{9, 6}})
			c.Check(err == nil, "base stroke failed: %v", err)
			dc2, _ := testScene(testWidth, testHeight)
			dup := []Vertex{{Position: V(-0.8, 0, 0)}, {Position: V(-0.8, 0, 0)}, {Position: V(0, 0, 0)}, {Position: V(0, 0, 0)}, {Position: V(0.8, 0, 0)}}
			_, err = dc2.DrawPolyline(dup, StrokeStyle{Width: 2, Dashes: []float64{9, 6}})
			c.Check(err == nil, "duplicate stroke failed: %v", err)
			a := testRow(dc1, testHeight/2)
			b := testRow(dc2, testHeight/2)
			diff := 0
			for x := range a {
				if a[x] != b[x] {
					diff++
				}
			}
			c.Check(diff == 0, "coincident points changed dash phase (diff=%d)", diff)

			// a path whose edges are all zero length succeeds and draws nothing
			dc3, _ := testScene(testWidth, testHeight)
			info, err := dc3.DrawPolyline([]Vertex{
				{Position: V(0.2, 0.2, 0)},
				{Position: V(0.2, 0.2, 0)},
				{Position: V(0.2, 0.2, 0)},
			}, StrokeStyle{Width: 4})
			c.Check(err == nil, "degenerate path returned an error: %v", err)
			c.Check(info.TotalPixels == 0 && info.UpdatedPixels == 0, "degenerate path drew pixels: %+v", info)
			c.Check(paintedCount(dc3) == 0, "degenerate path painted the canvas")
		})
	})
}

// TestStrokeJoins covers butt caps, bevel joins, one fragment per pixel, and
// the deduplicated pixel accounting.
func TestStrokeJoins(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "join group runs")

		t.Run("ButtCaps", func(t *testing.T) {
			// a horizontal stroke must end exactly at its endpoints with flat
			// caps, never extending beyond them
			const w = 200
			dc, _ := testScene(w, testHeight)
			x0 := 60.0
			x1 := 140.0
			vs := []Vertex{
				{Position: V((x0-float64(w)/2)/(float64(w)/2), 0, 0)},
				{Position: V((x1-float64(w)/2)/(float64(w)/2), 0, 0)},
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 10})
			c.Check(err == nil, "cap stroke failed: %v", err)
			first, last := testSpan(testRow(dc, testHeight/2))
			c.Check(first == 60, "left cap extended past its endpoint (first=%d)", first)
			c.Check(last == 139, "right cap extended past its endpoint (last=%d)", last)
			// five rows above and below the center line for a 10px stroke
			for _, dy := range []int{-5, 4} {
				row := testRow(dc, testHeight/2+dy)
				f, l := testSpan(row)
				c.Check(f == 60 && l == 139, "cap row %+d has wrong span (%d..%d)", dy, f, l)
			}
			row := testRow(dc, testHeight/2-6)
			f, l := testSpan(row)
			c.Check(f < 0 && l < 0, "stroke is wider than requested (span %d..%d)", f, l)
			c.Check(info.TotalPixels > 0, "cap stroke covered nothing")
		})

		t.Run("BevelCorner", func(t *testing.T) {
			// a right-angle bend: the outer corner of the bevel must be filled
			// and must not extend as a full square cap of either edge
			const w = 200
			const h = 120
			dc, sh := testScene(w, h)
			// screen-space path: (60,60) -> (140,60) -> (140,20)
			vs := []Vertex{
				{Position: V(testNDCX(60, w), testNDCY(60, h), 0)},
				{Position: V(testNDCX(140, w), testNDCY(60, h), 0)},
				{Position: V(testNDCX(140, w), testNDCY(20, h), 0)},
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 10})
			c.Check(err == nil, "corner stroke failed: %v", err)
			// horizontal quad: y in [55,65], x in [60,140]
			// vertical quad:   x in [135,145], y in [20,60]
			// outside bevel fills the triangle (140,60),(145,60),(140,65)
			c.Check(dc.DepthBuffer[61*w+142] < math.MaxFloat64, "outer bevel corner is not filled")
			c.Check(dc.DepthBuffer[62*w+140] < math.MaxFloat64, "bevel wedge is missing a pixel")
			// beyond the bevel triangle nothing is painted
			c.Check(dc.DepthBuffer[64*w+144] >= math.MaxFloat64, "bevel extends past the stroke width")
			c.Check(dc.DepthBuffer[66*w+142] >= math.MaxFloat64, "bevel extends too far from the bend")
			// the two legs are fully solid
			c.Check(dc.DepthBuffer[60*w+100] < math.MaxFloat64, "horizontal leg is missing")
			c.Check(dc.DepthBuffer[40*w+140] < math.MaxFloat64, "vertical leg is missing")
			// outside the stroke's half width in both axes, nothing is painted
			c.Check(dc.DepthBuffer[54*w+100] >= math.MaxFloat64, "horizontal leg is too thick")
			c.Check(dc.DepthBuffer[70*w+140] >= math.MaxFloat64, "vertical leg is too thick")
			c.Check(sh.vertex == 3, "vertex shader called %d times, want 3", sh.vertex)
			c.Check(sh.fragment == int(info.TotalPixels), "fragment calls=%d total=%d, want one per covered pixel", sh.fragment, info.TotalPixels)
			c.Check(info.TotalPixels == info.UpdatedPixels, "solid stroke should commit every covered pixel")
		})

		t.Run("InnerCornerNoOverlap", func(t *testing.T) {
			// the inner corner of a bend is covered by both legs; it must be
			// counted and shaded exactly once
			dc, sh := testScene(120, 120)
			vs := []Vertex{
				{Position: V(testNDCX(30, 120), testNDCY(42, 120), 0)},
				{Position: V(testNDCX(72, 120), testNDCY(42, 120), 0)},
				{Position: V(testNDCX(72, 120), testNDCY(90, 120), 0)},
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 12})
			c.Check(err == nil, "inner corner stroke failed: %v", err)
			// pixel (70,44) lies in the overlap of both legs
			c.Check(dc.DepthBuffer[44*120+70] < math.MaxFloat64, "inner corner is not painted")
			c.Check(sh.fragment == int(info.TotalPixels), "inner corner fragment=%d total=%d, want one per pixel", sh.fragment, info.TotalPixels)
			c.Check(info.TotalPixels == info.UpdatedPixels, "overlap was committed more than once")
		})

		t.Run("FragmentCallsOncePerPixel", func(t *testing.T) {
			// a self-crossing polyline: overlap must be submitted once
			dc, sh := testScene(120, 120)
			vs := []Vertex{
				{Position: V(-0.8, -0.8, 0)},
				{Position: V(0.8, 0.8, 0)},
				{Position: V(-0.8, 0.8, 0)},
				{Position: V(0.8, -0.8, 0)},
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 6})
			c.Check(err == nil, "crossing stroke failed: %v", err)
			c.Check(sh.fragment == int(info.TotalPixels), "overlap shaded %d times for %d pixels", sh.fragment, info.TotalPixels)
			c.Check(info.TotalPixels == info.UpdatedPixels, "crossing stroke should commit each pixel once")
		})

		t.Run("DiscardNotCommitted", func(t *testing.T) {
			dc, sh := testScene(120, 120)
			sh.discard = &testRect{x0: 50, y0: 0, x1: 70, y1: 120}
			info, err := dc.DrawPolyline([]Vertex{
				{Position: V(-0.9, 0, 0)},
				{Position: V(0.9, 0, 0)},
			}, StrokeStyle{Width: 8})
			c.Check(err == nil, "discard stroke failed: %v", err)
			// discarded pixels are candidates but are never committed
			c.Check(info.TotalPixels > info.UpdatedPixels, "discard did not reduce committed pixels (total=%d updated=%d)", info.TotalPixels, info.UpdatedPixels)
			c.Check(info.UpdatedPixels > 0, "discard removed the entire stroke")
			c.Check(sh.discarded > 0, "no fragment was discarded, test band missed the stroke")
			c.Check(info.TotalPixels == uint64(sh.fragment), "every candidate should reach the fragment shader exactly once (candidates=%d fragments=%d)", info.TotalPixels, sh.fragment)
			// nothing was written into the discarded band
			committed := 0
			for y := 0; y < 120; y++ {
				for x := 50; x < 70; x++ {
					if dc.DepthBuffer[y*120+x] != math.MaxFloat64 {
						committed++
					}
				}
			}
			c.Check(committed == 0, "%d discarded pixels were committed", committed)
		})
	})
}

// TestStrokeDepth covers perspective-correct attributes, screen-linear depth,
// true occlusion, and depth bias.
func TestStrokeDepth(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "depth group runs")

		t.Run("DepthInterpolation", func(t *testing.T) {
			// depth is the window coordinate of the clipped projected vertex,
			// interpolated linearly across the screen
			matrix := LookAt(V(0, 0, 5), V(0, 0, 0), V(0, 1, 0)).Perspective(60, 1, 1, 10)
			const w = 64
			dc := NewContext(w, w)
			dc.Cull = CullNone
			dc.AlphaBlend = false
			dc.Shader = NewSolidColorShader(matrix, Color{1, 1, 1, 1})
			a := V(-0.5, 0, -1)
			b := V(0.5, 0, 1)
			_, err := dc.DrawPolyline([]Vertex{{Position: a}, {Position: b}}, StrokeStyle{Width: 3})
			c.Check(err == nil, "depth stroke failed: %v", err)
			oa := matrix.MulPositionW(a)
			ob := matrix.MulPositionW(b)
			sa := dc.screenMatrix.MulPosition(oa.DivScalar(oa.W).Vector())
			sb := dc.screenMatrix.MulPosition(ob.DivScalar(ob.W).Vector())
			checked := 0
			for x := 28; x < 36; x++ {
				i := 32*w + x
				if dc.DepthBuffer[i] >= math.MaxFloat64 {
					c.Check(false, "expected painted pixel at %d", x)
					continue
				}
				t0 := (float64(x) + 0.5 - sa.X) / (sb.X - sa.X)
				want := sa.Z + t0*(sb.Z-sa.Z)
				got := dc.DepthBuffer[i]
				c.Check(math.Abs(got-want) < 1e-9, "depth at x=%d got %.9f want %.9f", x, got, want)
				checked++
			}
			c.Check(checked >= 4, "too few depth samples checked (%d)", checked)
		})

		t.Run("PerspectiveCorrectAttributes", func(t *testing.T) {
			// a texture coordinate interpolated along a receding edge must be
			// perspective-correct, not linear in screen space
			matrix := LookAt(V(0, 0, 5), V(0, 0, 0), V(0, 1, 0)).Perspective(60, 1, 1, 10)
			const w = 64
			dc := NewContext(w, w)
			dc.Cull = CullNone
			dc.AlphaBlend = false
			dc.WriteDepth = false
			dc.Shader = &testTextureShader{matrix: matrix}
			v1 := Vertex{Position: V(-0.5, 0, -1), Texture: V(0, 0, 0)}
			v2 := Vertex{Position: V(0.5, 0, 1), Texture: V(1, 0, 0)}
			_, err := dc.DrawPolyline([]Vertex{v1, v2}, StrokeStyle{Width: 3})
			c.Check(err == nil, "attribute stroke failed: %v", err)
			oa := matrix.MulPositionW(v1.Position)
			ob := matrix.MulPositionW(v2.Position)
			sa := dc.screenMatrix.MulPosition(oa.DivScalar(oa.W).Vector())
			sb := dc.screenMatrix.MulPosition(ob.DivScalar(ob.W).Vector())
			checked := 0
			for x := 26; x < 39; x++ {
				got := dc.ColorBuffer.NRGBAAt(x, w/2).R
				t0 := Clamp((float64(x)+0.5-sa.X)/(sb.X-sa.X), 0, 1)
				la := (1 - t0) / oa.W
				lb := t0 / ob.W
				want := lb / (la + lb)
				c.Check(math.Abs(float64(got)/255-want) < 0.02, "texture at x=%d got %.3f want %.3f", x, float64(got)/255, want)
				checked++
			}
			c.Check(checked >= 6, "too few attribute samples checked (%d)", checked)
		})

		t.Run("TrueOcclusion", func(t *testing.T) {
			// an opaque nearer stroke blocks a farther one; the farther stroke
			// still reports candidate pixels but commits none where occluded
			dc, _ := testScene(testWidth, testHeight)
			near := []Vertex{{Position: V(-0.9, 0, 0.2)}, {Position: V(0.9, 0, 0.2)}}
			_, err := dc.DrawPolyline(near, StrokeStyle{Width: 16})
			c.Check(err == nil, "near stroke failed: %v", err)
			before := make([]float64, len(dc.DepthBuffer))
			copy(before, dc.DepthBuffer)
			far := []Vertex{{Position: V(-0.9, 0.4, 0.6)}, {Position: V(0.9, -0.4, 0.6)}}
			info, err := dc.DrawPolyline(far, StrokeStyle{Width: 16})
			c.Check(err == nil, "far stroke failed: %v", err)
			c.Check(info.TotalPixels > info.UpdatedPixels, "occlusion was not detected (total=%d updated=%d)", info.TotalPixels, info.UpdatedPixels)
			// center of the near stroke is untouched
			center := (testHeight / 2) * testWidth
			c.Check(dc.DepthBuffer[center+10] == before[center+10], "near stroke depth was overwritten")
			// a far-only pixel was painted; at x=20 the near band occupies
			// rows around the center, while the slanted far stroke reaches
			// up to row 25
			c.Check(dc.DepthBuffer[25*testWidth+20] < math.MaxFloat64, "far stroke painted nothing where visible")
		})

		t.Run("SelfCrossTieBreak", func(t *testing.T) {
			// a planar self-crossing polyline: all edges share one depth, so
			// the later edge along the path must win the overlap
			dc := NewContext(100, 100)
			dc.Cull = CullNone
			dc.AlphaBlend = false
			dc.Shader = &testTagShader{matrix: Identity()}
			vs := []Vertex{
				{Position: V(-0.8, 0, 0), Color: Color{0.25, 0, 0, 1}},
				{Position: V(0.8, 0, 0), Color: Color{0.25, 0, 0, 1}},
				{Position: V(0, -0.8, 0), Color: Color{0.75, 0, 0, 1}},
				{Position: V(0, 0.8, 0), Color: Color{0.75, 0, 0, 1}},
			}
			info, err := dc.DrawPolyline(vs, StrokeStyle{Width: 8})
			c.Check(err == nil, "crossing stroke failed: %v", err)
			center := dc.ColorBuffer.NRGBAAt(50, 50)
			c.Check(center.R > 160, "equal-depth crossing did not pick the later edge (R=%d)", center.R)
			// away from the crossing each edge keeps its own attribute
			c.Check(dc.ColorBuffer.NRGBAAt(20, 50).R < 100, "earlier edge attribute was lost")
			c.Check(dc.ColorBuffer.NRGBAAt(50, 20).R > 160, "later edge attribute was lost")
			c.Check(info.TotalPixels == info.UpdatedPixels, "crossing submitted a pixel twice")

			// when the edges differ in depth the nearer edge wins even if it
			// appears earlier along the path
			dc2 := NewContext(100, 100)
			dc2.Cull = CullNone
			dc2.AlphaBlend = false
			dc2.Shader = &testTagShader{matrix: Identity()}
			vs2 := []Vertex{
				{Position: V(-0.8, 0, -0.3), Color: Color{0.25, 0, 0, 1}},
				{Position: V(0.8, 0, -0.3), Color: Color{0.25, 0, 0, 1}},
				{Position: V(0, -0.8, 0.3), Color: Color{0.75, 0, 0, 1}},
				{Position: V(0, 0.8, 0.3), Color: Color{0.75, 0, 0, 1}},
			}
			_, err = dc2.DrawPolyline(vs2, StrokeStyle{Width: 8})
			c.Check(err == nil, "depth-crossing stroke failed: %v", err)
			c.Check(dc2.ColorBuffer.NRGBAAt(50, 50).R < 100, "nearer earlier edge did not win the crossing")
		})

		t.Run("DepthBias", func(t *testing.T) {
			dc, _ := testScene(testWidth, testHeight)
			line := []Vertex{{Position: V(-0.5, 0, 0.5)}, {Position: V(0.5, 0, 0.5)}}
			_, err := dc.DrawPolyline(line, StrokeStyle{Width: 8})
			c.Check(err == nil, "first stroke failed: %v", err)
			i := (testHeight / 2) * testWidth
			want := dc.DepthBuffer[i]
			// the same depth without bias wins the depth test the same way
			// the existing triangle/line path does: bz <= depth commits
			infoSame, err := dc.DrawPolyline(line, StrokeStyle{Width: 8})
			c.Check(err == nil, "second stroke failed: %v", err)
			c.Check(infoSame.UpdatedPixels > 0, "equal depth was not committed")
			// a farther stroke with the same geometry must be rejected
			dc.WriteColor = false
			farther := []Vertex{{Position: V(-0.5, 0, 0.6)}, {Position: V(0.5, 0, 0.6)}}
			infoFar, err := dc.DrawPolyline(farther, StrokeStyle{Width: 8})
			c.Check(err == nil, "far stroke failed: %v", err)
			c.Check(infoFar.UpdatedPixels == 0, "farther stroke passed the depth test (updated=%d)", infoFar.UpdatedPixels)
			// a negative bias lets the same geometry win
			dc.WriteColor = true
			dc.DepthBias = -1e-3
			infoBias, err := dc.DrawPolyline(line, StrokeStyle{Width: 8})
			c.Check(err == nil, "biased stroke failed: %v", err)
			c.Check(infoBias.UpdatedPixels > 0, "negative bias did not commit")
			c.Check(dc.DepthBuffer[i] == want, "stored depth should be unbiased (got %v want %v)", dc.DepthBuffer[i], want)
		})

		t.Run("FlatEndsAndWidth", func(t *testing.T) {
			// width is in screen pixels: measure the painted thickness of a
			// horizontal stroke at its midpoint
			const w = 200
			h := 40
			thickness := func(width float64) int {
				dc, _ := testScene(w, h)
				vs := []Vertex{{Position: V(-0.5, 0, 0)}, {Position: V(0.5, 0, 0)}}
				_, err := dc.DrawPolyline(vs, StrokeStyle{Width: width})
				c.Check(err == nil, "width stroke failed: %v", err)
				n := 0
				x := w / 2
				for y := 0; y < h; y++ {
					if dc.DepthBuffer[y*w+x] < math.MaxFloat64 {
						n++
					}
				}
				return n
			}
			thin := thickness(4)
			thick := thickness(12)
			c.Check(thin == 4, "4px stroke is %d px thick", thin)
			c.Check(thick == 12, "12px stroke is %d px thick", thick)
			// the length is unchanged by width for a horizontal line
			dc, _ := testScene(w, h)
			_, err := dc.DrawPolyline([]Vertex{{Position: V(-0.5, 0, 0)}, {Position: V(0.5, 0, 0)}}, StrokeStyle{Width: 12})
			c.Check(err == nil, "width stroke failed: %v", err)
			length := 0
			for x := 0; x < w; x++ {
				if dc.DepthBuffer[(h/2)*w+x] < math.MaxFloat64 {
					length++
				}
			}
			c.Check(length == 100, "half-width stroke spans %d pixels, want 100", length)
		})

		t.Run("VertexShaderOncePerInput", func(t *testing.T) {
			dc, sh := testScene(testWidth, testHeight)
			vs := []Vertex{
				{Position: V(-0.5, -0.5, 0)},
				{Position: V(0.5, -0.5, 0)},
				{Position: V(0.5, 0.5, 0)},
				{Position: V(-0.5, 0.5, 0)},
			}
			_, err := dc.DrawPolyline(vs, StrokeStyle{Width: 3, Closed: true, Dashes: []float64{9, 5}})
			c.Check(err == nil, "vertex-count stroke failed: %v", err)
			c.Check(sh.vertex == len(vs), "vertex shader called %d times for %d input vertices", sh.vertex, len(vs))
		})
	})
}

// testTextureShader renders the interpolated texture X coordinate as red.
type testTextureShader struct {
	matrix Matrix
}

func (s *testTextureShader) Vertex(v Vertex) Vertex {
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *testTextureShader) Fragment(v Vertex) Color {
	return Color{v.Texture.X, 0, 0, 1}
}

// testTagShader renders the vertex color's red channel directly, so each edge
// can be tagged with a constant attribute value.
type testTagShader struct {
	matrix Matrix
}

func (s *testTagShader) Vertex(v Vertex) Vertex {
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *testTagShader) Fragment(v Vertex) Color {
	return Color{v.Color.R, 0, 0, 1}
}

// mustDashRow draws a horizontal dashed line between two NDC x coordinates
// with an optional list of extra collinear x coordinates.
func mustDashRow(t *testing.T, ends []float64, width float64, extra []float64) *Context {
	t.Helper()
	dc, _ := testScene(testWidth, testHeight)
	xs := []float64{ends[0]}
	xs = append(xs, extra...)
	xs = append(xs, ends[1])
	vs := make([]Vertex, len(xs))
	for i, x := range xs {
		vs[i] = Vertex{Position: V(x, 0, 0)}
	}
	_, err := dc.DrawPolyline(vs, StrokeStyle{Width: width, Dashes: []float64{20, 10}})
	if err != nil {
		t.Fatalf("dash row failed: %v", err)
	}
	return dc
}

func paintedCount(dc *Context) int {
	n := 0
	for _, d := range dc.DepthBuffer {
		if d < math.MaxFloat64 {
			n++
		}
	}
	return n
}

func testAllRows(dc *Context, h, w int) []bool {
	out := make([]bool, h*w)
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			out[y*w+x] = dc.DepthBuffer[y*w+x] < math.MaxFloat64
		}
	}
	return out
}
