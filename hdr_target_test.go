package fauxgl

import (
	"image"
	"math"
	"testing"
	"time"

	"github.com/fogleman/fauxgl/task_support/taskcheck"
)

// ---- test shaders -------------------------------------------------------

type hdrTestShader struct {
	matrix  Matrix
	color   Color
	discard bool
}

func (s *hdrTestShader) Vertex(v Vertex) Vertex {
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *hdrTestShader) Fragment(v Vertex) Color {
	if s.discard {
		return Discard
	}
	return s.color
}

func (s *hdrTestShader) FragmentHDR(v Vertex) Color {
	if s.discard {
		return Discard
	}
	return s.color
}

// fragmentOnlyShader deliberately does not implement HDRShader.
type fragmentOnlyShader struct {
	matrix Matrix
	color  Color
}

func (s *fragmentOnlyShader) Vertex(v Vertex) Vertex {
	v.Output = s.matrix.MulPositionW(v.Position)
	return v
}

func (s *fragmentOnlyShader) Fragment(v Vertex) Color {
	return s.color
}

// ---- helpers ------------------------------------------------------------

func approx(a, b float64) bool {
	return math.Abs(a-b) < 1e-9
}

func approxColor(a, b hdrColor) bool {
	return approx(a.R, b.R) && approx(a.G, b.G) && approx(a.B, b.B) && approx(a.A, b.A)
}

// fullScreenTriangle returns a triangle in the z plane whose interior
// contains the screen center; vertices sit outside the clip volume on
// purpose so clipping is exercised as well.
func fullScreenTriangle(z float64) *Triangle {
	return NewTriangleForPoints(
		Vector{-2, -2, z}, Vector{2, -2, z}, Vector{0, 2, z})
}

func hdrPixel(dc *Context, x, y int) hdrColor {
	return dc.hdrBuffer[y*dc.Width+x]
}

func snapshotPixel(snap []Color, w, x, y int) Color {
	return snap[y*w+x]
}

// independent reference downsample: area overlap re-derived from the
// destination footprint fractions.
func refOverlap(lo, hi float64, cell int) float64 {
	c0 := float64(cell)
	c1 := c0 + 1
	l := math.Max(lo, c0)
	r := math.Min(hi, c1)
	if r <= l {
		return 0
	}
	return r - l
}

func refDownsample(src []hdrColor, sw, sh, dw, dh int) []hdrColor {
	dst := make([]hdrColor, dw*dh)
	for dy := 0; dy < dh; dy++ {
		yLo := float64(dy) * float64(sh) / float64(dh)
		yHi := float64(dy+1) * float64(sh) / float64(dh)
		for dx := 0; dx < dw; dx++ {
			xLo := float64(dx) * float64(sw) / float64(dw)
			xHi := float64(dx+1) * float64(sw) / float64(dw)
			var r, g, b, n float64
			for sy := 0; sy < sh; sy++ {
				oy := refOverlap(yLo, yHi, sy)
				if oy == 0 {
					continue
				}
				for sx := 0; sx < sw; sx++ {
					ox := refOverlap(xLo, xHi, sx)
					if ox == 0 {
						continue
					}
					w := ox * oy
					p := src[sy*sw+sx]
					r += p.R * w
					g += p.G * w
					b += p.B * w
					n += w
				}
			}
			dst[dy*dw+dx] = hdrColor{r / n, g / n, b / n, 1}
		}
	}
	return dst
}

func refClamp(x, lo, hi int) int {
	if x < lo {
		return lo
	}
	if x > hi {
		return hi
	}
	return x
}

func refUpsample(src []hdrColor, sw, sh, dw, dh int) []hdrColor {
	dst := make([]hdrColor, dw*dh)
	for dy := 0; dy < dh; dy++ {
		fy := (float64(dy)+0.5)*float64(sh)/float64(dh) - 0.5
		y0 := int(math.Floor(fy))
		ty := fy - float64(y0)
		cy0, cy1 := refClamp(y0, 0, sh-1), refClamp(y0+1, 0, sh-1)
		for dx := 0; dx < dw; dx++ {
			fx := (float64(dx)+0.5)*float64(sw)/float64(dw) - 0.5
			x0 := int(math.Floor(fx))
			tx := fx - float64(x0)
			cx0, cx1 := refClamp(x0, 0, sw-1), refClamp(x0+1, 0, sw-1)
			p00, p10 := src[cy0*sw+cx0], src[cy0*sw+cx1]
			p01, p11 := src[cy1*sw+cx0], src[cy1*sw+cx1]
			dst[dy*dw+dx] = hdrColor{
				(p00.R*(1-tx)+p10.R*tx)*(1-ty) + (p01.R*(1-tx)+p11.R*tx)*ty,
				(p00.G*(1-tx)+p10.G*tx)*(1-ty) + (p01.G*(1-tx)+p11.G*tx)*ty,
				(p00.B*(1-tx)+p10.B*tx)*(1-ty) + (p01.B*(1-tx)+p11.B*tx)*ty,
				1,
			}
		}
	}
	return dst
}

func refBright(C []hdrColor, threshold float64) []hdrColor {
	D := make([]hdrColor, len(C))
	for i, p := range C {
		l := 0.2126*p.R + 0.7152*p.G + 0.0722*p.B
		if l > 0 && l > threshold {
			k := (l - threshold) / l
			D[i] = hdrColor{p.R * k, p.G * k, p.B * k, 1}
		}
	}
	return D
}

func refBloom(C []hdrColor, w, h int, threshold float64, levels int) []hdrColor {
	type layer struct {
		buf  []hdrColor
		w, h int
	}
	D0 := refBright(C, threshold)
	layers := []layer{{D0, w, h}}
	for len(layers) < levels {
		prev := layers[len(layers)-1]
		nw, nh := (prev.w+1)/2, (prev.h+1)/2
		if nw == prev.w && nh == prev.h {
			break
		}
		layers = append(layers, layer{refDownsample(prev.buf, prev.w, prev.h, nw, nh), nw, nh})
	}
	U := layers[len(layers)-1].buf
	uw, uh := layers[len(layers)-1].w, layers[len(layers)-1].h
	for k := len(layers) - 2; k >= 0; k-- {
		up := refUpsample(U, uw, uh, layers[k].w, layers[k].h)
		D := layers[k].buf
		U = make([]hdrColor, len(D))
		for i := range U {
			U[i] = hdrColor{(D[i].R + up[i].R) / 2, (D[i].G + up[i].G) / 2, (D[i].B + up[i].B) / 2, 1}
		}
		uw, uh = layers[k].w, layers[k].h
	}
	return U
}

func refSRGBByte(linear float64) uint8 {
	var v float64
	if linear <= 0.0031308 {
		v = 12.92 * linear
	} else {
		v = 1.055*math.Pow(linear, 1/2.4) - 0.055
	}
	n := Round(v * 255)
	if n < 0 {
		n = 0
	}
	if n > 255 {
		n = 255
	}
	return uint8(n)
}

// ---- tests --------------------------------------------------------------

func TestHDRCommit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Custom HDR shader stores channels greater than one.
		dc := NewContext(8, 6)
		dc.Cull = CullNone
		dc.EnableHDR()
		dc.Shader = &hdrTestShader{Identity(), Color{3, 2, 1.5, 1}, false}
		dc.DrawTriangle(fullScreenTriangle(0))
		snap := dc.HDRSnapshot()
		c.Check(len(snap) == 8*6, "snapshot size %d", len(snap))
		center := snapshotPixel(snap, 8, 4, 3)
		c.Check(approxColor(hdrColor(center), hdrColor{3, 2, 1.5, 1}), "hdr stores >1 at center: %+v", center)
		c.Check(hdrPixel(dc, 1, 1).R > 1, "interior pixel holds linear HDR value")

		// Snapshot is an independent, row-major, premultiplied copy.
		snap[0].R = 999
		c.Check(hdrPixel(dc, 0, 0).R != 999, "snapshot must be a copy")
		dc.ClearColorBufferWith(Color{0.8, 0.6, 0.4, 0.5})
		snap = dc.HDRSnapshot()
		p0 := snapshotPixel(snap, 8, 0, 0)
		c.Check(approxColor(hdrColor(p0), hdrColor{0.4, 0.3, 0.2, 0.5}), "snapshot is premultiplied: %+v", p0)
		c.Check(snapshotPixel(snap, 8, 7, 5) == snap[5*8+7], "snapshot is row major")

		// Snapshot unavailable outside HDR mode; byte buffer stays untouched.
		byteBefore := dc.ColorBuffer.Pix[0]
		dc.DisableHDR()
		c.Check(dc.HDRSnapshot() == nil, "snapshot nil when disabled")
		c.Check(dc.ColorBuffer.Pix[0] == byteBefore && byteBefore == 0, "HDR draws never touch ColorBuffer")

		// Re-enabling reinitializes from the current ClearColor.
		dc.ClearColor = Color{0.1, 0.2, 0.3, 1}
		dc.EnableHDR()
		c.Check(approxColor(hdrPixel(dc, 3, 2), hdrColor{0.1, 0.2, 0.3, 1}), "re-enable reinitializes from ClearColor")
		dc.DrawTriangle(fullScreenTriangle(0))
		first := hdrPixel(dc, 4, 3)
		dc.EnableHDR() // repeated enable keeps contents
		c.Check(hdrPixel(dc, 4, 3) == first, "repeated EnableHDR preserves contents")

		// Shader without HDRShader falls back to Fragment and still keeps
		// its linear (unclamped) return value in the float target.
		dc2 := NewContext(8, 6)
		dc2.Cull = CullNone
		dc2.EnableHDR()
		dc2.Shader = &fragmentOnlyShader{Identity(), Color{1.5, 0.5, 0.25, 1}}
		dc2.DrawTriangle(fullScreenTriangle(0))
		c.Check(approx(hdrPixel(dc2, 4, 3).R, 1.5), "plain Fragment feeds HDR target unclamped")

		// Phong: FragmentHDR keeps the highlight, Fragment stays clamped.
		phong := NewPhongShader(Identity(), Vector{0, 0, 1}, Vector{0, 0, 2})
		phong.ObjectColor = Color{2, 2, 2, 1}
		phong.AmbientColor = Black
		phong.DiffuseColor = Black
		phong.SpecularColor = White
		phong.SpecularPower = 32
		v := Vertex{Position: Vector{}, Normal: Vector{0, 0, 1}}
		hdrFragment := phong.FragmentHDR(v)
		c.Check(approxColor(hdrColor(hdrFragment), hdrColor{2, 2, 2, 1}), "Phong HDR fragment unclamped: %+v", hdrFragment)
		c.Check(phong.Fragment(v) == White, "Phong Fragment still clamps to white")
		// Ambient-only Phong is position independent and stays above one.
		ambient := NewPhongShader(Identity(), Vector{0, 0, 1}, Vector{0, 0, 2})
		ambient.ObjectColor = Color{2, 2, 2, 1}
		ambient.AmbientColor = White
		ambient.DiffuseColor = Black
		ambient.SpecularPower = 0
		dc2.Shader = ambient
		dc2.DrawTriangle(fullScreenTriangle(0))
		c.Check(approx(hdrPixel(dc2, 4, 3).R, 2), "real Phong color above one survives in HDR target")
		// Enabling a real specular term pushes the stored value even higher.
		specular := NewPhongShader(Identity(), Vector{0, 0, 1}, Vector{0, 0, 2})
		specular.ObjectColor = Color{2, 2, 2, 1}
		specular.AmbientColor = White
		specular.DiffuseColor = Black
		specular.SpecularColor = White
		specular.SpecularPower = 4
		dc2.Shader = specular
		dc2.ReadDepth = false
		dc2.DrawTriangle(fullScreenTriangle(0))
		c.Check(hdrPixel(dc2, 4, 3).R > 2, "real Phong specular highlight adds unclamped energy")
		dc2.ReadDepth = true

		// Discard commits nothing: neither color nor depth changes.
		dc3 := NewContext(8, 6)
		dc3.Cull = CullNone
		dc3.EnableHDR()
		dc3.Shader = &hdrTestShader{Identity(), Color{1, 0, 0, 1}, false}
		dc3.DrawTriangle(fullScreenTriangle(-0.5))
		red := hdrPixel(dc3, 4, 3)
		depthBefore := dc3.DepthBuffer[3*8+4]
		dc3.Shader = &hdrTestShader{Identity(), Color{0, 1, 0, 1}, true}
		info := dc3.DrawTriangle(fullScreenTriangle(0))
		c.Check(info.UpdatedPixels == 0 && info.TotalPixels > 0, "discard updates no pixels")
		c.Check(hdrPixel(dc3, 4, 3) == red, "discard leaves color untouched")
		c.Check(dc3.DepthBuffer[3*8+4] == depthBefore, "discard leaves depth untouched")

		// Depth re-check rejects the farther fragment.
		dc3.Shader = &hdrTestShader{Identity(), Color{0, 0, 1, 1}, false}
		info = dc3.DrawTriangle(fullScreenTriangle(0.5))
		c.Check(info.UpdatedPixels == 0, "far fragment fails depth re-check")
		c.Check(hdrPixel(dc3, 4, 3) == red, "far fragment does not overwrite")
		// With ReadDepth off it goes through.
		dc3.ReadDepth = false
		info = dc3.DrawTriangle(fullScreenTriangle(0.5))
		c.Check(info.UpdatedPixels > 0 && approx(hdrPixel(dc3, 4, 3).B, 1), "ReadDepth=false allows overwrite")
		dc3.ReadDepth = true

		// WriteDepth false keeps depth; WriteColor false keeps color but
		// depth still lands; both share the same critical section.
		dc4 := NewContext(8, 6)
		dc4.Cull = CullNone
		dc4.EnableHDR()
		dc4.Shader = &hdrTestShader{Identity(), Color{1, 1, 1, 1}, false}
		dc4.WriteColor = false
		dc4.DrawTriangle(fullScreenTriangle(0))
		c.Check(hdrPixel(dc4, 4, 3).A == 0, "WriteColor=false blocks color write")
		c.Check(dc4.DepthBuffer[3*8+4] == 0.5, "depth still written with WriteColor=false")
		dc4.WriteColor = true
		dc4.ClearDepthBuffer()
		dc4.WriteDepth = false
		dc4.DrawTriangle(fullScreenTriangle(0.2))
		c.Check(dc4.DepthBuffer[3*8+4] == math.MaxFloat64, "WriteDepth=false blocks depth write")
		c.Check(approx(hdrPixel(dc4, 4, 3).R, 1), "color still written with WriteDepth=false")
		dc4.WriteDepth = true

		// DepthBias participates in the depth comparison.
		dc5 := NewContext(8, 6)
		dc5.Cull = CullNone
		dc5.EnableHDR()
		dc5.Shader = &hdrTestShader{Identity(), Color{1, 0, 0, 1}, false}
		dc5.DrawTriangle(fullScreenTriangle(0))
		dc5.DepthBias = 10
		info = dc5.DrawTriangle(fullScreenTriangle(0))
		c.Check(info.UpdatedPixels == 0, "positive DepthBias rejects fragment")
		dc5.DepthBias = 0

		// Multi-layer HDR alpha blending, including >1 source colors.
		dc6 := NewContext(8, 6)
		dc6.Cull = CullNone
		dc6.EnableHDR()
		dc6.ReadDepth = false
		dc6.Shader = &hdrTestShader{Identity(), Color{1, 0, 0, 0.5}, false}
		dc6.DrawTriangle(fullScreenTriangle(0))
		dc6.DrawTriangle(fullScreenTriangle(0))
		c.Check(approxColor(hdrPixel(dc6, 4, 3), hdrColor{0.75, 0, 0, 0.75}), "two blended layers: %+v", hdrPixel(dc6, 4, 3))
		dc6.Shader = &hdrTestShader{Identity(), Color{2, 1, 0, 0.25}, false}
		dc6.DrawTriangle(fullScreenTriangle(0))
		c.Check(approxColor(hdrPixel(dc6, 4, 3), hdrColor{1.0625, 0.25, 0, 0.8125}), "HDR blend layer: %+v", hdrPixel(dc6, 4, 3))

		// Blending disabled: straight replacement by premultiplied source.
		dc7 := NewContext(8, 6)
		dc7.Cull = CullNone
		dc7.EnableHDR()
		dc7.AlphaBlend = false
		dc7.Shader = &hdrTestShader{Identity(), Color{2, 1, 0.5, 0.4}, false}
		dc7.DrawTriangle(fullScreenTriangle(0))
		c.Check(approxColor(hdrPixel(dc7, 4, 3), hdrColor{0.8, 0.4, 0.2, 0.4}), "no-blend replace: %+v", hdrPixel(dc7, 4, 3))

		// Thick lines traverse the HDR target.
		dc8 := NewContext(16, 12)
		dc8.Cull = CullNone
		dc8.LineWidth = 4
		dc8.EnableHDR()
		dc8.Shader = &hdrTestShader{Identity(), Color{2.5, 1.5, 0.5, 1}, false}
		dc8.DrawLine(NewLineForPoints(Vector{-0.8, -0.8, 0}, Vector{0.8, 0.8, 0}))
		hits := 0
		for i := range dc8.hdrBuffer {
			if approx(dc8.hdrBuffer[i].R, 2.5) {
				hits++
			}
		}
		c.Check(hits > 4, "thick line painted %d HDR pixels", hits)

		// A triangle extending past the clip volume still lands in HDR.
		dc9 := NewContext(16, 12)
		dc9.Cull = CullNone
		dc9.EnableHDR()
		dc9.Shader = &hdrTestShader{Identity(), Color{1.25, 2.25, 3.25, 1}, false}
		clipped := NewTriangleForPoints(Vector{-1, -1, 0}, Vector{2, -1, 0}, Vector{2, 1, 0})
		dc9.DrawTriangle(clipped)
		got := hdrPixel(dc9, 14, 6)
		c.Check(approxColor(got, hdrColor{1.25, 2.25, 3.25, 1}), "clipped triangle reaches HDR target: %+v", got)

		// Clipping against the perspective frustum produces triangles with
		// horizontal screen-space edges; these must rasterize in bounded
		// time instead of scanning with infinite row offsets.
		dcClip := NewContext(64, 48)
		dcClip.Cull = CullNone
		dcClip.EnableHDR()
		eye := V(2.2, -3.2, 1.8)
		perspective := LookAt(eye, V(0, 0, 0), V(0, 0, 1)).Perspective(40, 64.0/48.0, 1, 20)
		dcClip.Shader = &hdrTestShader{perspective, Color{1.5, 1.5, 1.5, 1}, false}
		clipMesh := NewSphere(2)
		done := make(chan RasterizeInfo, 1)
		go func() { done <- dcClip.DrawMesh(clipMesh) }()
		select {
		case clipInfo := <-done:
			c.Check(clipInfo.UpdatedPixels > 0, "perspective-clipped sphere painted pixels")
		case <-time.After(5 * time.Second):
			c.Check(false, "clipped horizontal-edge triangles must not hang the rasterizer")
		}
		c.Check(hdrPixel(dcClip, 32, 24).R > 0, "clipped sphere visible in HDR target")

		// Clearing depth never touches color; clearing color in HDR never
		// touches the byte image.
		dc10 := NewContext(8, 6)
		dc10.EnableHDR()
		dc10.ClearColorBufferWith(Color{0.7, 0.7, 0.7, 1})
		dc10.ClearDepthBufferWith(0.25)
		c.Check(approx(hdrPixel(dc10, 0, 0).R, 0.7), "color survives ClearDepthBuffer")
		c.Check(dc10.DepthBuffer[0] == 0.25, "ClearDepthBuffer only clears depth")
		c.Check(dc10.ColorBuffer.Pix[0] == 0, "byte buffer remains its own image")
		c.Check(dc10.Image() == dc10.ColorBuffer, "Image() still returns the byte buffer")

		// Parallel, non-equal-depth draws match a serial render and stay
		// race free (the suite runs under -race).
		parallel := NewContext(16, 12)
		serial := NewContext(16, 12)
		parallel.Cull, serial.Cull = CullNone, CullNone
		parallel.EnableHDR()
		serial.EnableHDR()
		const n = 64
		tris := make([]*Triangle, n)
		seed := uint64(1234567)
		for i := 0; i < n; i++ {
			seed = seed*6364136223846793005 + 1442695040888963407
			z := -0.9 + float64(seed%1000)/1000*1.8
			tris[i] = fullScreenTriangle(z)
		}
		parallel.Shader = &hdrTestShader{Identity(), Color{1, 1, 1, 1}, false}
		serial.Shader = &hdrTestShader{Identity(), Color{1, 1, 1, 1}, false}
		parallel.DrawTriangles(tris)
		for _, tri := range tris {
			serial.DrawTriangle(tri)
		}
		same := true
		for i := range parallel.hdrBuffer {
			if !approxColor(parallel.hdrBuffer[i], serial.hdrBuffer[i]) ||
				!approx(parallel.DepthBuffer[i], serial.DepthBuffer[i]) {
				same = false
				break
			}
		}
		c.Check(same, "parallel and serial HDR renders differ")

		// Default byte path still works after HDR is disabled.
		base := NewContext(24, 20)
		base.Cull = CullNone
		base.EnableHDR()
		base.DisableHDR()
		base.Shader = NewSolidColorShader(Identity(), Color{1, .25, .5, 1})
		base.DrawTriangle(seedTriangle())
		bc := base.ColorBuffer.NRGBAAt(12, 10)
		c.Check(bc.R == 255 && bc.G == 63 && bc.B == 127 && bc.A == 255, "byte path intact after HDR cycle: %+v", bc)
	})
}

func TestBloomPyramid(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const w, h = 3, 5
		C := make([]hdrColor, w*h)
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				v := float64(y*w + x + 1)
				C[y*w+x] = hdrColor{v, v + 0.5, v * 2, 1}
			}
		}

		// Bright pass with zero threshold is identity; an L=0 pixel and a
		// sub-threshold pixel produce zero glow.
		D := refBright(C, 0)
		c.Check(len(D) == len(C), "bright pass size")
		c.Check(approxColor(D[7], C[7]), "zero threshold keeps bright color")
		zero := []hdrColor{{0, 0, 0, 1}, {0.01, 0.01, 0.01, 1}, {0, 0, 0, 1}}
		d0 := refBright(zero, 0.05)
		c.Check(d0[0].R == 0 && d0[2].R == 0, "L=0 and below-threshold pixels give zero")
		c.Check(d0[1].R == 0, "below-threshold luminance is removed")

		// First downsample 3x5 -> 2x3, checked against an independent
		// reference and against one fully hand-derived cell.
		got := downsampleHDR(C, w, h, 2, 3)
		want := refDownsample(C, w, h, 2, 3)
		for i := range got {
			c.Check(approxColor(got[i], want[i]), "downsample cell %d: got %+v want %+v", i, got[i], want[i])
		}
		// Hand weights for destination (dx=0, dy=0): source cells and
		// overlap areas (1), (0.5), (2/3), (1/3), total area 2.5.
		var hr, hg, hb float64
		hand := []struct {
			sx, sy int
			weight float64
		}{{0, 0, 1}, {1, 0, 0.5}, {0, 1, 2.0 / 3.0}, {1, 1, 1.0 / 3.0}}
		for _, e := range hand {
			p := C[e.sy*w+e.sx]
			hr += p.R * e.weight
			hg += p.G * e.weight
			hb += p.B * e.weight
		}
		c.Check(approxColor(got[0], hdrColor{hr / 2.5, hg / 2.5, hb / 2.5, 1}), "hand-derived downsample cell: %+v", got[0])

		// The 1x1 level must equal the arithmetic mean of the source
		// because every source pixel has the same unit area.
		full := downsampleHDR(C, w, h, 1, 1)
		var mr, mg, mb float64
		for _, p := range C {
			mr, mg, mb = mr+p.R, mg+p.G, mb+p.B
		}
		n := float64(len(C))
		c.Check(approxColor(full[0], hdrColor{mr / n, mg / n, mb / n, 1}), "1x1 level is global mean: %+v", full[0])

		// Constant image stays constant through every level.
		constImg := make([]hdrColor, w*h)
		for i := range constImg {
			constImg[i] = hdrColor{0.9, 0.8, 0.7, 1}
		}
		d1 := downsampleHDR(constImg, w, h, 2, 3)
		d2 := downsampleHDR(d1, 2, 3, 1, 2)
		d3 := downsampleHDR(d2, 1, 2, 1, 1)
		c.Check(approxColor(d1[3], constImg[0]) && approxColor(d2[1], constImg[0]) && approxColor(d3[0], constImg[0]),
			"constant image survives odd-sized downsamples")

		// Upsample: edge clamp at the corner and interior bilinear weights.
		src := []hdrColor{
			{1, 1, 1, 1}, {2, 2, 2, 1},
			{3, 3, 3, 1}, {4, 4, 4, 1},
			{5, 5, 5, 1}, {6, 6, 6, 1},
		}
		up := upsampleHDR(src, 2, 3, 3, 5)
		refUp := refUpsample(src, 2, 3, 3, 5)
		for i := range up {
			c.Check(approxColor(up[i], refUp[i]), "upsample cell %d: %+v vs %+v", i, up[i], refUp[i])
		}
		// Corner sample center maps to (-1/6, -1/6): both axes clamp to 0.
		c.Check(approx(up[0].R, 1), "upsample clamps at corner: %+v", up[0])

		// Full reconstruction against the independent reference on 3x5.
		U := resolveBloom(C, w, h, 0, 4)
		refU := refBloom(C, w, h, 0, 4)
		c.Check(len(U) == w*h, "reconstructed bloom has full resolution")
		for i := range U {
			c.Check(approxColor(U[i], refU[i]), "reconstruct cell %d: got %+v want %+v", i, U[i], refU[i])
		}

		// A non-constant image actually spreads glow to a neighboring cell
		// (guards against accidentally returning D0 unchanged).
		spike := make([]hdrColor, w*h)
		for i := range spike {
			spike[i] = hdrColor{0.01, 0.01, 0.01, 1}
		}
		spike[0] = hdrColor{4, 4, 4, 1}
		spread := resolveBloom(spike, w, h, 0.5, 4)
		c.Check(spread[1].R > spike[1].R && spread[w].R > spike[w].R, "bloom spreads beyond the bright pixel: %+v", spread)
	})
}

func TestHDRResolve(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dc := NewContext(4, 3)
		dc.EnableHDR()

		// Resolve requires HDR and valid settings.
		im, err := dc.ResolveHDR(HDRSettings{Black, 1, 0, 0, 1})
		c.Check(err == nil && im != nil, "valid resolve failed: %v", err)
		disabled := NewContext(4, 3)
		_, err = disabled.ResolveHDR(HDRSettings{Black, 1, 0, 0, 1})
		c.Check(err != nil, "resolve without HDR must error")
		bad := []HDRSettings{
			{Color{0, 0, 0, 0.5}, 1, 0, 0, 1},
			{Color{math.NaN(), 0, 0, 1}, 1, 0, 0, 1},
			{Color{math.Inf(1), 0, 0, 1}, 1, 0, 0, 1},
			{Black, 0, 0, 0, 1},
			{Black, -1, 0, 0, 1},
			{Black, math.Inf(1), 0, 0, 1},
			{Black, 1, -1, 0, 1},
			{Black, 1, 0, -1, 1},
			{Black, 1, 0, 0, 0},
		}
		for i, s := range bad {
			im, err := dc.ResolveHDR(s)
			c.Check(err != nil && im == nil, "invalid settings case %d accepted", i)
		}

		// Output geometry and opaque alpha.
		c.Check(im.Bounds() == image.Rect(0, 0, 4, 3), "resolve image size")
		allOpaque := true
		for y := 0; y < 3; y++ {
			for x := 0; x < 4; x++ {
				if im.NRGBAAt(x, y).A != 255 {
					allOpaque = false
				}
			}
		}
		c.Check(allOpaque, "resolve alpha must be 255 everywhere")

		// Background compositing through a transparent pixel, including a
		// background brighter than one.
		for i := range dc.hdrBuffer {
			dc.hdrBuffer[i] = hdrColor{0, 0, 0, 0}
		}
		bg := Color{0.4, 0.5, 0.6, 1}
		im, err = dc.ResolveHDR(HDRSettings{bg, 1, 100, 0, 2})
		c.Check(err == nil, "background resolve errored: %v", err)
		px := im.NRGBAAt(0, 0)
		c.Check(px.R == refSRGBByte(1-math.Exp(-0.4)), "background red tone map: %d", px.R)
		c.Check(px.G == refSRGBByte(1-math.Exp(-0.5)), "background green tone map")
		c.Check(px.B == refSRGBByte(1-math.Exp(-0.6)), "background blue tone map")

		// sRGB transfer: engineered linear value inside the linear segment.
		c.Check(sRGBByte(0) == 0, "sRGB zero")
		c.Check(sRGBByte(1) == 255, "sRGB one")
		m := 0.002
		c.Check(sRGBByte(m) == uint8(Round(12.92*m*255)), "sRGB linear segment byte")

		// Threshold is applied before exposure: sub-threshold pixels never
		// bloom regardless of strength or exposure.
		for i := range dc.hdrBuffer {
			dc.hdrBuffer[i] = hdrColor{0.5, 0.5, 0.5, 1}
		}
		noBloom, err := dc.ResolveHDR(HDRSettings{Black, 1, 0.8, 0, 3})
		c.Check(err == nil, "resolve errored: %v", err)
		strongBloom, err := dc.ResolveHDR(HDRSettings{Black, 1, 0.8, 10, 3})
		c.Check(err == nil, "resolve errored: %v", err)
		c.Check(string(noBloom.Pix) == string(strongBloom.Pix), "sub-threshold scene must not bloom")
		dc.hdrBuffer[0] = hdrColor{2, 2, 2, 1}
		brightBloom, err := dc.ResolveHDR(HDRSettings{Black, 1, 0.8, 5, 3})
		c.Check(err == nil, "resolve errored: %v", err)
		c.Check(brightBloom.Pix[1] != noBloom.Pix[1] || brightBloom.Pix[4] != noBloom.Pix[4],
			"super-threshold pixel must bloom into neighbors")

		// Repeated resolves never accumulate: exposure is a pure function
		// of the snapshot, and the target and depth are untouched.
		before := dc.HDRSnapshot()
		depthBefore := append([]float64(nil), dc.DepthBuffer...)
		a1, err := dc.ResolveHDR(HDRSettings{Black, 0.7, 0.5, 1.5, 3})
		c.Check(err == nil, "resolve A1 errored: %v", err)
		b1, err := dc.ResolveHDR(HDRSettings{Black, 1.3, 0.5, 1.5, 3})
		c.Check(err == nil, "resolve B1 errored: %v", err)
		a2, err := dc.ResolveHDR(HDRSettings{Black, 0.7, 0.5, 1.5, 3})
		c.Check(err == nil, "resolve A2 errored: %v", err)
		c.Check(string(a1.Pix) == string(a2.Pix), "same exposure must reproduce identical output")
		c.Check(string(a1.Pix) != string(b1.Pix), "different exposures must differ")
		after := dc.HDRSnapshot()
		same := len(before) == len(after)
		for i := range before {
			if before[i] != after[i] {
				same = false
			}
		}
		c.Check(same, "resolve must not modify the HDR target")
		depthSame := len(depthBefore) == len(dc.DepthBuffer)
		for i := range depthBefore {
			if depthBefore[i] != dc.DepthBuffer[i] {
				depthSame = false
			}
		}
		c.Check(depthSame, "resolve must not modify the depth buffer")

		// Full pipeline on an opaque >1 pixel matches the reference math.
		dc.hdrBuffer[0] = hdrColor{3, 0.25, 0, 1}
		settings := HDRSettings{Color{0.1, 0.1, 0.1, 1}, 0.9, 0.4, 1.25, 4}
		im, err = dc.ResolveHDR(settings)
		c.Check(err == nil, "pipeline resolve errored: %v", err)
		snap := dc.HDRSnapshot()
		C := make([]hdrColor, len(snap))
		for i, p := range snap {
			C[i] = hdrColor{p.R + settings.Background.R*(1-p.A),
				p.G + settings.Background.G*(1-p.A),
				p.B + settings.Background.B*(1-p.A), 1}
		}
		U := refBloom(C, 4, 3, settings.BloomThreshold, settings.BloomLevels)
		ok := true
		for y := 0; y < 3; y++ {
			for x := 0; x < 4; x++ {
				i := y*4 + x
				hr := C[i].R + settings.BloomStrength*U[i].R
				hg := C[i].G + settings.BloomStrength*U[i].G
				hb := C[i].B + settings.BloomStrength*U[i].B
				want := [3]uint8{
					refSRGBByte(1 - math.Exp(-settings.Exposure*hr)),
					refSRGBByte(1 - math.Exp(-settings.Exposure*hg)),
					refSRGBByte(1 - math.Exp(-settings.Exposure*hb)),
				}
				got := im.NRGBAAt(x, y)
				if got.R != want[0] || got.G != want[1] || got.B != want[2] {
					ok = false
				}
			}
		}
		c.Check(ok, "full pipeline bytes differ from reference")
	})
}
