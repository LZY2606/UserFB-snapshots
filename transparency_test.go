package fauxgl

import (
	"bytes"
	"image/color"
	"math"
	"runtime"
	"sync/atomic"
	"testing"
)

// premul is a premultiplied RGBA color used for independent float64
// compositing in these tests.
type premul [4]float64

func bgPremul(c color.NRGBA) premul {
	a := float64(c.A) / 0xff
	return premul{float64(c.R) / 0xff * a, float64(c.G) / 0xff * a, float64(c.B) / 0xff * a, a}
}

func layerPremul(c Color) premul {
	c = Color{Clamp(c.R, 0, 1), Clamp(c.G, 0, 1), Clamp(c.B, 0, 1), Clamp(c.A, 0, 1)}
	return premul{c.R * c.A, c.G * c.A, c.B * c.A, c.A}
}

func (s premul) over(d premul) premul {
	inv := 1 - s[3]
	return premul{s[0] + inv*d[0], s[1] + inv*d[1], s[2] + inv*d[2], s[3] + inv*d[3]}
}

func (p premul) nrgba() color.NRGBA {
	var r, g, b float64
	if p[3] > 0 {
		r, g, b = p[0]/p[3], p[1]/p[3], p[2]/p[3]
	}
	q := func(x float64) uint8 { return uint8(math.Round(Clamp(x, 0, 1) * 0xff)) }
	return color.NRGBA{q(r), q(g), q(b), q(p[3])}
}

// composite layers far-to-near over a background, all in float64.
func composite(bg color.NRGBA, layers ...Color) color.NRGBA {
	acc := bgPremul(bg)
	for _, l := range layers {
		acc = layerPremul(l).over(acc)
	}
	return acc.nrgba()
}

func assertNRGBA(t *testing.T, what string, got, want color.NRGBA) {
	t.Helper()
	d := func(a, b uint8) int {
		if a > b {
			return int(a) - int(b)
		}
		return int(b) - int(a)
	}
	if d(got.R, want.R) > 1 || d(got.G, want.G) > 1 || d(got.B, want.B) > 1 || d(got.A, want.A) > 1 {
		t.Fatalf("%s: got %v, want %v (±1 per channel)", what, got, want)
	}
}

// transparencyContext returns a context ready for BeginTransparency.
func transparencyContext(w, h int, bg Color) *Context {
	dc := NewContext(w, h)
	dc.Cull = CullNone
	dc.ClearColorBufferWith(bg)
	dc.WriteDepth = false
	return dc
}

func mustBegin(t *testing.T, dc *Context) {
	t.Helper()
	if err := dc.BeginTransparency(); err != nil {
		t.Fatal(err)
	}
}

func mustResolve(t *testing.T, dc *Context) {
	t.Helper()
	if err := dc.ResolveTransparency(); err != nil {
		t.Fatal(err)
	}
}

// vertexColorShader shades with the interpolated vertex color.
type vertexColorShader struct {
	Matrix Matrix
}

func (s vertexColorShader) Vertex(v Vertex) Vertex {
	v.Output = s.Matrix.MulPositionW(v.Position)
	return v
}

func (s vertexColorShader) Fragment(v Vertex) Color {
	return v.Color
}

// pixelCountingShader counts Fragment invocations, in total and per pixel.
// Per-pixel counts are only meaningful for triangles, whose vertex outputs
// correspond to their screen corners; lines reuse vertex outputs across
// corners, so only the total is meaningful there.
type pixelCountingShader struct {
	Matrix Matrix
	Color  Color
	Width  int
	Height int
	Total  int64
	Counts []int64
}

func newPixelCountingShader(w, h int, c Color) *pixelCountingShader {
	return &pixelCountingShader{Matrix: Identity(), Color: c, Width: w, Height: h, Counts: make([]int64, w*h)}
}

func (s *pixelCountingShader) Vertex(v Vertex) Vertex {
	v.Output = s.Matrix.MulPositionW(v.Position)
	return v
}

func (s *pixelCountingShader) Fragment(v Vertex) Color {
	atomic.AddInt64(&s.Total, 1)
	w := v.Output.W
	nx := v.Output.X / w
	ny := v.Output.Y / w
	x := int(math.Floor((nx + 1) * float64(s.Width) / 2))
	y := int(math.Floor((1 - ny) * float64(s.Height) / 2))
	if x >= 0 && x < s.Width && y >= 0 && y < s.Height {
		atomic.AddInt64(&s.Counts[y*s.Width+x], 1)
	}
	return s.Color
}

func (s *pixelCountingShader) sumOfCounts() int64 {
	var n int64
	for _, c := range s.Counts {
		n += c
	}
	return n
}

func (s *pixelCountingShader) max() int64 {
	var n int64
	for _, c := range s.Counts {
		if c > n {
			n = c
		}
	}
	return n
}

// discardRightShader discards fragments with world X > 0.
type discardRightShader struct {
	Color Color
}

func (s discardRightShader) Vertex(v Vertex) Vertex {
	v.Output = Identity().MulPositionW(v.Position)
	return v
}

func (s discardRightShader) Fragment(v Vertex) Color {
	if v.Position.X > 0 {
		return Discard
	}
	return s.Color
}

// uvShader outputs the interpolated U texture coordinate as red.
type uvShader struct {
	Matrix Matrix
}

func (s uvShader) Vertex(v Vertex) Vertex {
	v.Output = s.Matrix.MulPositionW(v.Position)
	return v
}

func (s uvShader) Fragment(v Vertex) Color {
	return Color{v.Texture.X, 0, 0, 1}
}

func crossingTriangles() (red, blue *Triangle) {
	red = NewTriangleForPoints(V(-.8, -.7, -.6), V(.7, -.4, .6), V(-.1, .8, .1))
	blue = NewTriangleForPoints(V(-.8, -.7, .6), V(.7, -.4, -.6), V(-.1, .8, -.1))
	return
}

func TestTransparencyOrder(t *testing.T) {
	redC := Color{1, 0, 0, .5}
	blueC := Color{0, 0, 1, .5}
	bg := color.NRGBA{0, 0, 0, 255}

	// two crossing slanted planes: per-pixel depth decides which is in
	// front, and the front layer differs between the left and right sides
	red, blue := crossingTriangles()
	dc := transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), redC)
	redInfo := dc.DrawTriangle(red)
	dc.Shader = NewSolidColorShader(Identity(), blueC)
	blueInfo := dc.DrawTriangle(blue)
	mustResolve(t, dc)
	if redInfo.UpdatedPixels == 0 || blueInfo.UpdatedPixels == 0 {
		t.Fatalf("no fragments collected: %+v %+v", redInfo, blueInfo)
	}
	left := dc.ColorBuffer.NRGBAAt(9, 20)
	right := dc.ColorBuffer.NRGBAAt(21, 20)
	if left == right {
		t.Fatalf("crossing planes resolved identically on both sides: %v", left)
	}
	assertNRGBA(t, "left side (red in front)", left, composite(bg, blueC, redC))
	assertNRGBA(t, "right side (blue in front)", right, composite(bg, redC, blueC))

	// batch vs individual submission, under different GOMAXPROCS:
	// the resolved image must be identical
	render := func(procs int, batch, redFirst bool) *Context {
		defer runtime.GOMAXPROCS(runtime.GOMAXPROCS(procs))
		dc := transparencyContext(32, 32, Black)
		dc.Shader = vertexColorShader{Identity()}
		r, b := crossingTriangles()
		r.SetColor(redC)
		b.SetColor(blueC)
		triangles := []*Triangle{r, b}
		if !redFirst {
			triangles[0], triangles[1] = b, r
		}
		mustBegin(t, dc)
		if batch {
			dc.DrawTriangles(triangles)
		} else {
			for _, tri := range triangles {
				dc.DrawTriangle(tri)
			}
		}
		mustResolve(t, dc)
		return dc
	}
	ref := render(1, false, true)
	for _, c := range []struct {
		procs int
		batch bool
	}{{1, true}, {2, false}, {4, true}, {8, false}, {8, true}} {
		got := render(c.procs, c.batch, true)
		if !bytes.Equal(ref.ColorBuffer.Pix, got.ColorBuffer.Pix) {
			t.Fatalf("procs=%d batch=%v: image differs from reference", c.procs, c.batch)
		}
	}

	// with strictly different depths, swapping submission order and
	// changing GOMAXPROCS must not change any pixel. Pixels exactly on
	// the crossing line have equal depth and are excluded.
	zAt := func(x, y float64) float64 {
		ax, ay, bx, by, cx, cy := 3.2, 27.2, 27.2, 22.4, 14.4, 3.2
		den := (by-cy)*(ax-cx) + (cx-bx)*(ay-cy)
		u := ((by-cy)*(x-cx) + (cx-bx)*(y-cy)) / den
		v := ((cy-ay)*(x-cx) + (ax-cx)*(y-cy)) / den
		return -.6*u + .6*v + .1*(1-u-v)
	}
	alternate := render(1, false, false)
	strict := 0
	for y := 0; y < 32; y++ {
		for x := 0; x < 32; x++ {
			if math.Abs(zAt(float64(x)+0.5, float64(y)+0.5)) <= 1e-9 {
				continue
			}
			strict++
			j := (y*32 + x) * 4
			if !bytes.Equal(ref.ColorBuffer.Pix[j:j+4], alternate.ColorBuffer.Pix[j:j+4]) {
				t.Fatalf("swapped submission order changed strictly-different-depth pixel (%d,%d)", x, y)
			}
		}
	}
	if strict < 100 {
		t.Fatalf("too few strictly-different-depth pixels: %d", strict)
	}
	for _, procs := range []int{4, 8} {
		got := render(procs, true, false)
		for y := 0; y < 32; y++ {
			for x := 0; x < 32; x++ {
				if math.Abs(zAt(float64(x)+0.5, float64(y)+0.5)) <= 1e-9 {
					continue
				}
				j := (y*32 + x) * 4
				if !bytes.Equal(ref.ColorBuffer.Pix[j:j+4], got.ColorBuffer.Pix[j:j+4]) {
					t.Fatalf("procs=%d with swapped order changed pixel (%d,%d)", procs, x, y)
				}
			}
		}
	}

	// equal depths: the later submission lands on top, deterministically
	equalDepth := func(procs int, batch, redFirst bool) *Context {
		defer runtime.GOMAXPROCS(runtime.GOMAXPROCS(procs))
		dc := transparencyContext(32, 32, Black)
		dc.Shader = vertexColorShader{Identity()}
		r := seedTriangle(0)
		r.SetColor(redC)
		b := seedTriangle(0)
		b.SetColor(blueC)
		triangles := []*Triangle{r, b}
		if !redFirst {
			triangles[0], triangles[1] = b, r
		}
		mustBegin(t, dc)
		if batch {
			dc.DrawTriangles(triangles)
		} else {
			for _, tri := range triangles {
				dc.DrawTriangle(tri)
			}
		}
		mustResolve(t, dc)
		return dc
	}
	redOnTop := composite(bg, blueC, redC)
	blueOnTop := composite(bg, redC, blueC)
	assertNRGBA(t, "equal depth, red submitted last", equalDepth(1, false, false).ColorBuffer.NRGBAAt(16, 16), redOnTop)
	assertNRGBA(t, "equal depth, blue submitted last", equalDepth(1, false, true).ColorBuffer.NRGBAAt(16, 16), blueOnTop)
	for _, procs := range []int{1, 4, 8} {
		for _, batch := range []bool{false, true} {
			got := equalDepth(procs, batch, true).ColorBuffer.NRGBAAt(16, 16)
			assertNRGBA(t, "equal depth determinism", got, blueOnTop)
		}
	}

	// DrawMesh submits triangles before explicit lines; at equal depth the
	// line lands on top. The triangle and the line quad are built from
	// integer screen coordinates with power-of-two doubled areas so the
	// interpolated depth is exactly 0.5 for both.
	tri := NewTriangleForPoints(V(-.5, .5, 0), V(.5, .5, 0), V(-.5, -.5, 0))
	tri.SetColor(redC)
	ln := NewLineForPoints(V(-.5, .25, 0), V(.5, .25, 0))
	ln.V1.Color = blueC
	ln.V2.Color = blueC
	mesh := NewMesh([]*Triangle{tri}, []*Line{ln})
	draw := func(asMesh bool) *Context {
		dc := transparencyContext(32, 32, Black)
		dc.LineWidth = 16
		dc.Shader = vertexColorShader{Identity()}
		mustBegin(t, dc)
		if asMesh {
			dc.DrawMesh(mesh)
		} else {
			dc.DrawTriangles(mesh.Triangles)
			dc.DrawLines(mesh.Lines)
		}
		mustResolve(t, dc)
		return dc
	}
	p := draw(true).ColorBuffer.NRGBAAt(12, 12)
	assertNRGBA(t, "mesh: line over triangle", p, composite(bg, redC, blueC))
	if !bytes.Equal(draw(true).ColorBuffer.Pix, draw(false).ColorBuffer.Pix) {
		t.Fatal("DrawMesh differs from sequential DrawTriangles + DrawLines")
	}
	// control: submitting the line first puts the triangle on top
	dc = transparencyContext(32, 32, Black)
	dc.LineWidth = 16
	dc.Shader = vertexColorShader{Identity()}
	mustBegin(t, dc)
	dc.DrawLine(ln)
	dc.DrawTriangle(tri)
	mustResolve(t, dc)
	assertNRGBA(t, "line submitted first", dc.ColorBuffer.NRGBAAt(12, 12), composite(bg, blueC, redC))
}

func TestTransparencyAlpha(t *testing.T) {
	// a single half-alpha red layer on a transparent background keeps the
	// full red channel when converted back to straight alpha
	dc := transparencyContext(32, 32, Transparent)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc.DrawTriangle(seedTriangle(0))
	mustResolve(t, dc)
	assertNRGBA(t, "red on transparent", dc.ColorBuffer.NRGBAAt(16, 16),
		composite(color.NRGBA{}, Color{1, 0, 0, .5}))
	if got := dc.ColorBuffer.NRGBAAt(16, 16); got.R < 254 {
		t.Fatalf("red channel not preserved: %v", got)
	}

	// multiple layers with asymmetric colors over a semi-transparent
	// background, composited independently in float64
	dc = NewContext(32, 32)
	dc.Cull = CullNone
	dc.ClearColorBufferWith(Color{0.2, 0.6, 0.9, 0.5})
	dc.WriteDepth = false
	bgN := dc.ColorBuffer.NRGBAAt(16, 16)
	layers := []struct {
		z float64
		c Color
	}{
		{.4, Color{1, 0, 0, .5}},
		{0, Color{0, 1, 0, .25}},
		{-.4, Color{0, 0, 1, .75}},
	}
	mustBegin(t, dc)
	for _, l := range layers {
		dc.Shader = NewSolidColorShader(Identity(), l.c)
		dc.DrawTriangle(seedTriangle(l.z))
	}
	mustResolve(t, dc)
	assertNRGBA(t, "three layers over semi-transparent background",
		dc.ColorBuffer.NRGBAAt(16, 16),
		composite(bgN, layers[0].c, layers[1].c, layers[2].c))

	// alpha=0 layers contribute neither color nor fragments
	draw := func(withZero bool) (*Context, RasterizeInfo) {
		dc := transparencyContext(32, 32, Black)
		mustBegin(t, dc)
		dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
		dc.DrawTriangle(seedTriangle(.2))
		var info RasterizeInfo
		if withZero {
			dc.Shader = NewSolidColorShader(Identity(), Color{1, 1, 1, 0})
			info = dc.DrawTriangle(seedTriangle(0))
		}
		dc.Shader = NewSolidColorShader(Identity(), Color{0, 0, 1, .5})
		dc.DrawTriangle(seedTriangle(-.2))
		mustResolve(t, dc)
		return dc, info
	}
	withZero, zeroInfo := draw(true)
	withoutZero, _ := draw(false)
	if zeroInfo.TotalPixels == 0 || zeroInfo.UpdatedPixels != 0 {
		t.Fatalf("alpha=0 layer: %+v", zeroInfo)
	}
	if !bytes.Equal(withZero.ColorBuffer.Pix, withoutZero.ColorBuffer.Pix) {
		t.Fatal("alpha=0 layer changed the image")
	}

	// discarded fragments contribute nothing
	dc = transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	full := dc.DrawTriangle(seedTriangle(0))
	mustResolve(t, dc)
	inside := dc.ColorBuffer.NRGBAAt(20, 16)
	if inside == (color.NRGBA{0, 0, 0, 255}) {
		t.Fatal("probe pixel not covered by the triangle")
	}
	dc = transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = discardRightShader{Color{1, 0, 0, .5}}
	part := dc.DrawTriangle(seedTriangle(0))
	mustResolve(t, dc)
	if part.UpdatedPixels == 0 || part.UpdatedPixels >= part.TotalPixels || part.TotalPixels != full.TotalPixels {
		t.Fatalf("discard counts: %+v vs %+v", part, full)
	}
	if got := dc.ColorBuffer.NRGBAAt(20, 16); got != (color.NRGBA{0, 0, 0, 255}) {
		t.Fatalf("discarded fragment contributed: %v", got)
	}
	assertNRGBA(t, "kept fragment", dc.ColorBuffer.NRGBAAt(12, 16),
		composite(color.NRGBA{0, 0, 0, 255}, Color{1, 0, 0, .5}))
}

func TestTransparencyCoverage(t *testing.T) {
	bg := color.NRGBA{0, 0, 0, 255}
	green := Color{0, 1, 0, 1}
	greenN := color.NRGBA{0, 255, 0, 255}

	// existing opaque geometry occludes through the collected fragments
	dc := NewContext(32, 32)
	dc.Cull = CullNone
	dc.ClearColorBufferWith(Black)
	dc.Shader = NewSolidColorShader(Identity(), green)
	dc.DrawTriangle(seedTriangle(0))
	dc.WriteDepth = false
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	behind := dc.DrawTriangle(seedTriangle(.4))
	dc.Shader = NewSolidColorShader(Identity(), Color{0, 0, 1, .5})
	front := dc.DrawTriangle(seedTriangle(-.4))
	mustResolve(t, dc)
	if behind.UpdatedPixels != 0 || front.UpdatedPixels == 0 {
		t.Fatalf("occlusion: behind=%+v front=%+v", behind, front)
	}
	assertNRGBA(t, "blue over opaque green", dc.ColorBuffer.NRGBAAt(16, 16),
		composite(greenN, Color{0, 0, 1, .5}))

	// the occlusion test uses z+DepthBias while sorting uses unbiased z
	occluded := func(bias float64) (*Context, RasterizeInfo) {
		dc := NewContext(32, 32)
		dc.Cull = CullNone
		dc.ClearColorBufferWith(Black)
		dc.Shader = NewSolidColorShader(Identity(), green)
		dc.DrawTriangle(seedTriangle(0))
		dc.WriteDepth = false
		dc.DepthBias = bias
		mustBegin(t, dc)
		dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
		info := dc.DrawTriangle(seedTriangle(.1))
		mustResolve(t, dc)
		return dc, info
	}
	dcb, infoB := occluded(.1)
	if infoB.UpdatedPixels != 0 || dcb.ColorBuffer.NRGBAAt(16, 16) != greenN {
		t.Fatalf("positive DepthBias should occlude: %+v", infoB)
	}
	dcf, infoF := occluded(-.1)
	if infoF.UpdatedPixels == 0 {
		t.Fatalf("negative DepthBias should pass: %+v", infoF)
	}
	assertNRGBA(t, "biased fragment", dcf.ColorBuffer.NRGBAAt(16, 16),
		composite(greenN, Color{1, 0, 0, .5}))

	// a triangle crossing the view volume is clipped into a fan; the
	// internal fan edges must be covered exactly once
	dc = transparencyContext(32, 32, Black)
	shader := newPixelCountingShader(32, 32, Color{1, 0, 0, .5})
	dc.Shader = shader
	mustBegin(t, dc)
	info := dc.DrawTriangle(NewTriangleForPoints(V(-3, -1, 0), V(3, -1, 0), V(0, 3, 0)))
	mustResolve(t, dc)
	if info.UpdatedPixels == 0 {
		t.Fatal("clipped triangle produced no fragments")
	}
	if m := shader.max(); m != 1 {
		t.Fatalf("pixel covered %d times by clipped fan", m)
	}
	if shader.Total != int64(info.UpdatedPixels) || shader.sumOfCounts() != shader.Total {
		t.Fatalf("fragment count %d != UpdatedPixels %d", shader.Total, info.UpdatedPixels)
	}
	want := composite(bg, Color{1, 0, 0, .5})
	for y := 0; y < 32; y++ {
		for x := 0; x < 32; x++ {
			if shader.Counts[y*32+x] == 1 {
				assertNRGBA(t, "uniform clipped color", dc.ColorBuffer.NRGBAAt(x, y), want)
			}
		}
	}

	// perspective-correct UV interpolation with varying w, on a triangle
	// that is partially outside the view volume (clipped in x). Clipping
	// contributes new vertices but must preserve the original
	// perspective-correct interpolation.
	matrix := Identity().Perspective(60, 1, 1, 10)
	tri := NewTriangleForPoints(V(-1, -1, -2), V(6, -1, -8), V(0, 2, -4))
	tri.V1.Texture = V(0, 0, 0)
	tri.V2.Texture = V(1, 0, 0)
	tri.V3.Texture = V(.5, 0, 0)
	dc = transparencyContext(64, 64, Black)
	dc.Shader = uvShader{matrix}
	mustBegin(t, dc)
	uvInfo := dc.DrawTriangle(tri)
	mustResolve(t, dc)
	if uvInfo.UpdatedPixels == 0 {
		t.Fatal("uv triangle produced no fragments")
	}
	var sx, sy, cw [3]float64
	for i, v := range []Vertex{tri.V1, tri.V2, tri.V3} {
		out := matrix.MulPositionW(v.Position)
		sx[i] = (out.X/out.W + 1) * 32
		sy[i] = (1 - out.Y/out.W) * 32
		cw[i] = out.W
	}
	// probe near an interior point of the original triangle that is also
	// inside the visible (clipped) region
	ox := 0.3*sx[0] + 0.3*sx[1] + 0.4*sx[2]
	oy := 0.3*sy[0] + 0.3*sy[1] + 0.4*sy[2]
	px := float64(int(math.Floor(ox))) + 0.5
	py := float64(int(math.Floor(oy))) + 0.5
	if px < 1 || px >= 63 || py < 1 || py >= 63 {
		t.Fatalf("probe pixel (%g,%g) outside the visible region", px, py)
	}
	d := (sy[1]-sy[2])*(sx[0]-sx[2]) + (sx[2]-sx[1])*(sy[0]-sy[2])
	b0 := ((sy[1]-sy[2])*(px-sx[2]) + (sx[2]-sx[1])*(py-sy[2])) / d
	b1 := ((sy[2]-sy[0])*(px-sx[2]) + (sx[0]-sx[2])*(py-sy[2])) / d
	b2 := 1 - b0 - b1
	if b0 <= 1e-6 || b1 <= 1e-6 || b2 <= 1e-6 {
		t.Fatalf("probe pixel outside triangle: %g %g %g", b0, b1, b2)
	}
	us := [3]float64{0, 1, .5}
	uPersp := (b0*us[0]/cw[0] + b1*us[1]/cw[1] + b2*us[2]/cw[2]) / (b0/cw[0] + b1/cw[1] + b2/cw[2])
	uAffine := b0*us[0] + b1*us[1] + b2*us[2]
	if math.Abs(uPersp-uAffine) < 0.05 {
		t.Fatalf("probe cannot distinguish perspective correction: %g vs %g", uPersp, uAffine)
	}
	got := dc.ColorBuffer.NRGBAAt(int(px-0.5), int(py-0.5))
	wantRed := uint8(math.Round(Clamp(uPersp, 0, 1) * 0xff))
	if diff := int(got.R) - int(wantRed); diff < -1 || diff > 1 {
		t.Fatalf("perspective-correct u after clipping: got red %d, want %d (affine would be %d)",
			got.R, wantRed, uint8(math.Round(Clamp(uAffine, 0, 1)*0xff)))
	}
	if got.A != 255 {
		t.Fatalf("opaque layer alpha: %v", got)
	}

	// two triangles sharing an edge: the shared edge is covered once
	dc = transparencyContext(32, 32, Black)
	shader = newPixelCountingShader(32, 32, Color{1, 0, 0, .5})
	dc.Shader = shader
	mustBegin(t, dc)
	q1 := NewTriangleForPoints(V(-.5, -.5, 0), V(.5, -.5, 0), V(.5, .5, 0))
	q2 := NewTriangleForPoints(V(-.5, -.5, 0), V(.5, .5, 0), V(-.5, .5, 0))
	quadInfo := dc.DrawTriangles([]*Triangle{q1, q2})
	mustResolve(t, dc)
	if m := shader.max(); m != 1 {
		t.Fatalf("shared edge covered %d times", m)
	}
	if shader.Total != int64(quadInfo.UpdatedPixels) || shader.sumOfCounts() != shader.Total {
		t.Fatalf("fragment count %d != UpdatedPixels %d", shader.Total, quadInfo.UpdatedPixels)
	}
	// pixel centers exactly on the shared diagonal (x+y == 31)
	onDiagonal := 0
	for x := 8; x < 24; x++ {
		y := 31 - x
		if shader.Counts[y*32+x] != 1 {
			t.Fatalf("diagonal pixel (%d,%d) covered %d times", x, y, shader.Counts[y*32+x])
		}
		assertNRGBA(t, "diagonal color", dc.ColorBuffer.NRGBAAt(x, y), want)
		onDiagonal++
	}
	if onDiagonal == 0 {
		t.Fatal("no pixel centers on the shared diagonal")
	}

	// a thick line's internal diagonal is covered exactly once: every
	// covered pixel holds a single layer, and the number of Fragment
	// calls equals the number of covered pixels
	dc = transparencyContext(32, 32, Black)
	dc.LineWidth = 4
	shader = newPixelCountingShader(32, 32, Color{0, 0, 1, .5})
	dc.Shader = shader
	mustBegin(t, dc)
	lineInfo := dc.DrawLine(NewLineForPoints(V(-.5, 0, 0), V(.5, 0, 0)))
	mustResolve(t, dc)
	if lineInfo.UpdatedPixels == 0 {
		t.Fatal("thick line produced no fragments")
	}
	wantLine := composite(bg, Color{0, 0, 1, .5})
	covered := 0
	for y := 0; y < 32; y++ {
		for x := 0; x < 32; x++ {
			p := dc.ColorBuffer.NRGBAAt(x, y)
			if p == (color.NRGBA{0, 0, 0, 255}) {
				continue
			}
			covered++
			assertNRGBA(t, "line pixel", p, wantLine)
		}
	}
	if int64(covered) != shader.Total || int64(covered) != int64(lineInfo.UpdatedPixels) {
		t.Fatalf("line covered %d pixels but made %d Fragment calls (%+v)",
			covered, shader.Total, lineInfo)
	}
	// pixel (13,16) center lies exactly on the internal diagonal
	assertNRGBA(t, "internal diagonal pixel", dc.ColorBuffer.NRGBAAt(13, 16), wantLine)

	// horizontal edges and zero-area geometry terminate normally
	dc = transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	flat := dc.DrawTriangle(NewTriangleForPoints(V(-.5, .5, 0), V(.5, .5, 0), V(0, -.5, 0)))
	zero := dc.DrawTriangle(NewTriangleForPoints(V(-.5, -.5, 0), V(0, 0, 0), V(.5, .5, 0)))
	mustResolve(t, dc)
	if flat.UpdatedPixels == 0 {
		t.Fatal("horizontal-edge triangle produced no fragments")
	}
	if zero.TotalPixels != 0 || zero.UpdatedPixels != 0 {
		t.Fatalf("zero-area triangle produced fragments: %+v", zero)
	}
}

func TestTransparencyLifecycle(t *testing.T) {
	// invalid configurations fail without touching buffers or state
	breaks := []func(*Context){
		func(dc *Context) { dc.ReadDepth = false },
		func(dc *Context) { dc.WriteDepth = true },
		func(dc *Context) { dc.WriteColor = false },
		func(dc *Context) { dc.AlphaBlend = false },
		func(dc *Context) { dc.Wireframe = true },
	}
	for i, brk := range breaks {
		dc := NewContext(8, 8)
		dc.WriteDepth = false
		brk(dc)
		before := append([]byte(nil), dc.ColorBuffer.Pix...)
		if err := dc.BeginTransparency(); err == nil {
			t.Fatalf("case %d: expected BeginTransparency error", i)
		}
		if !bytes.Equal(before, dc.ColorBuffer.Pix) {
			t.Fatalf("case %d: failed begin modified the color buffer", i)
		}
		if dc.transparency != nil {
			t.Fatalf("case %d: failed begin left an active pass", i)
		}
	}

	// a second begin fails and preserves the pending pass
	dc := transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc.DrawTriangle(seedTriangle(0))
	if err := dc.BeginTransparency(); err == nil {
		t.Fatal("expected error from nested begin")
	}
	mustResolve(t, dc)
	assertNRGBA(t, "pending fragments survive failed begin", dc.ColorBuffer.NRGBAAt(16, 16),
		composite(color.NRGBA{0, 0, 0, 255}, Color{1, 0, 0, .5}))

	// resolve without an active pass fails
	if err := NewContext(8, 8).ResolveTransparency(); err == nil {
		t.Fatal("expected ResolveTransparency error without a pass")
	}
	if err := dc.ResolveTransparency(); err == nil {
		t.Fatal("expected ResolveTransparency error after resolve")
	}

	// cancel discards pending fragments without changing the image
	dc = transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc.DrawTriangle(seedTriangle(0))
	dc.CancelTransparency()
	for i, p := range dc.ColorBuffer.Pix {
		if p != []byte{0, 0, 0, 255}[i%4] {
			t.Fatal("cancel modified the image")
		}
	}
	if err := dc.ResolveTransparency(); err == nil {
		t.Fatal("expected ResolveTransparency error after cancel")
	}

	// clearing the color buffer cancels the pass, then clears
	dc = transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc.DrawTriangle(seedTriangle(0))
	dc.ClearColorBufferWith(White)
	if dc.transparency != nil {
		t.Fatal("clear did not cancel the pass")
	}
	if dc.ColorBuffer.NRGBAAt(16, 16) != (color.NRGBA{255, 255, 255, 255}) {
		t.Fatal("clear did not run after canceling")
	}
	if err := dc.ResolveTransparency(); err == nil {
		t.Fatal("expected ResolveTransparency error after clear")
	}

	// clearing the depth buffer also cancels the pass
	dc = transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc.DrawTriangle(seedTriangle(0))
	dc.ClearDepthBuffer()
	if dc.transparency != nil {
		t.Fatal("depth clear did not cancel the pass")
	}
	if dc.ColorBuffer.NRGBAAt(16, 16) != (color.NRGBA{0, 0, 0, 255}) {
		t.Fatal("canceled pass leaked fragments into the image")
	}

	// two sequential frames: no fragments leak across frames
	dc = transparencyContext(32, 32, Black)
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc.DrawTriangle(seedTriangle(-.4))
	mustResolve(t, dc)
	assertNRGBA(t, "frame 1", dc.ColorBuffer.NRGBAAt(16, 16),
		composite(color.NRGBA{0, 0, 0, 255}, Color{1, 0, 0, .5}))
	dc.ClearColorBufferWith(Black)
	dc.ClearDepthBuffer()
	mustBegin(t, dc)
	dc.Shader = NewSolidColorShader(Identity(), Color{0, 0, 1, .5})
	dc.DrawTriangle(seedTriangle(-.4))
	mustResolve(t, dc)
	assertNRGBA(t, "frame 2 has no frame 1 fragments", dc.ColorBuffer.NRGBAAt(16, 16),
		composite(color.NRGBA{0, 0, 0, 255}, Color{0, 0, 1, .5}))

	// two contexts are fully isolated
	dc1 := transparencyContext(32, 32, Black)
	dc2 := transparencyContext(32, 32, Black)
	mustBegin(t, dc1)
	mustBegin(t, dc2)
	dc1.Shader = NewSolidColorShader(Identity(), Color{1, 0, 0, .5})
	dc1.DrawTriangle(seedTriangle(-.4))
	dc2.Shader = NewSolidColorShader(Identity(), Color{0, 0, 1, .5})
	dc2.DrawTriangle(seedTriangle(-.4))
	mustResolve(t, dc1)
	mustResolve(t, dc2)
	assertNRGBA(t, "context 1", dc1.ColorBuffer.NRGBAAt(16, 16),
		composite(color.NRGBA{0, 0, 0, 255}, Color{1, 0, 0, .5}))
	assertNRGBA(t, "context 2", dc2.ColorBuffer.NRGBAAt(16, 16),
		composite(color.NRGBA{0, 0, 0, 255}, Color{0, 0, 1, .5}))
}
