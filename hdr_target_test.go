package fauxgl

import (
	"image"
	"math"
	"testing"

	"github.com/fogleman/fauxgl/task_support/taskcheck"
)

// -----------------------------------------------------------------------------
// test shaders
// -----------------------------------------------------------------------------

// hdrFixedShader returns a constant color through both fragment entry points.
type hdrFixedShader struct {
	color Color
}

func (s *hdrFixedShader) Vertex(v Vertex) Vertex {
	v.Output = v.Position.VectorW()
	return v
}
func (s *hdrFixedShader) Fragment(v Vertex) Color    { return s.color }
func (s *hdrFixedShader) FragmentHDR(v Vertex) Color { return s.color }

// hdrLegacyShader implements only the original Shader interface. In HDR mode
// its Fragment return value must be used as-is.
type hdrLegacyShader struct {
	color Color
}

func (s *hdrLegacyShader) Vertex(v Vertex) Vertex {
	v.Output = v.Position.VectorW()
	return v
}
func (s *hdrLegacyShader) Fragment(v Vertex) Color { return s.color }

// hdrDiscardShader discards fragments whose object-space X is positive.
type hdrDiscardShader struct {
	color Color
}

func (s *hdrDiscardShader) Vertex(v Vertex) Vertex {
	v.Output = v.Position.VectorW()
	return v
}
func (s *hdrDiscardShader) Fragment(v Vertex) Color {
	if v.Position.X > 0 {
		return Discard
	}
	return s.color
}
func (s *hdrDiscardShader) FragmentHDR(v Vertex) Color { return s.Fragment(v) }

// hdrVertexColorShader emits the interpolated vertex color so individual
// triangles can carry their own value.
type hdrVertexColorShader struct{}

func (s *hdrVertexColorShader) Vertex(v Vertex) Vertex {
	v.Output = v.Position.VectorW()
	return v
}
func (s *hdrVertexColorShader) Fragment(v Vertex) Color    { return v.Color }
func (s *hdrVertexColorShader) FragmentHDR(v Vertex) Color { return v.Color }

// hdrPixelShader maps object-space pixel coordinates in [0,w]x[0,h] onto the
// full viewport, with y increasing downward to match image rows.
type hdrPixelShader struct {
	w, h float64
}

func (s *hdrPixelShader) Vertex(v Vertex) Vertex {
	v.Output = VectorW{
		2*v.Position.X/s.w - 1,
		1 - 2*v.Position.Y/s.h,
		0,
		1,
	}
	return v
}
func (s *hdrPixelShader) Fragment(v Vertex) Color    { return v.Color }
func (s *hdrPixelShader) FragmentHDR(v Vertex) Color { return v.Color }

// hdrPassThroughShader preserves the clip-space Output computed by the caller.
type hdrPassThroughShader struct {
	color Color
}

func (s *hdrPassThroughShader) Vertex(v Vertex) Vertex     { return v }
func (s *hdrPassThroughShader) Fragment(v Vertex) Color    { return s.color }
func (s *hdrPassThroughShader) FragmentHDR(v Vertex) Color { return s.color }

// -----------------------------------------------------------------------------
// helpers
// -----------------------------------------------------------------------------

func hdrTestContext(w, h int, clear Color) *Context {
	dc := NewContext(w, h)
	dc.Cull = CullNone
	dc.ClearColor = clear
	dc.EnableHDR()
	return dc
}

func hdrQuad(minX, minY, maxX, maxY float64) *Triangle {
	v1 := Vertex{Position: Vector{minX, minY, 0}}
	v2 := Vertex{Position: Vector{maxX, minY, 0}}
	v3 := Vertex{Position: Vector{maxX, maxY, 0}}
	return NewTriangle(v1, v2, v3)
}

// hdrFillQuads covers the whole viewport with the two halves of a quad.
func hdrFillQuads(dc *Context) RasterizeInfo {
	ll := Vertex{Position: Vector{-1, -1, 0}}
	lr := Vertex{Position: Vector{1, -1, 0}}
	ur := Vertex{Position: Vector{1, 1, 0}}
	ul := Vertex{Position: Vector{-1, 1, 0}}
	info := dc.DrawTriangle(NewTriangle(ll, lr, ur))
	return info.Add(dc.DrawTriangle(NewTriangle(ll, ur, ul)))
}

func hdrSnapAt(snapshot []Color, w, x, y int) Color {
	return snapshot[y*w+x]
}

func closeTo(a, b, eps float64) bool {
	return math.Abs(a-b) <= eps
}

func hdrTestSettings(bg Color) HDRSettings {
	return HDRSettings{
		Background:     bg,
		Exposure:       1,
		BloomThreshold: 0,
		BloomStrength:  0,
		BloomLevels:    1,
	}
}

// -----------------------------------------------------------------------------
// TestHDRCommit: the draw path must store full precision linear values
// -----------------------------------------------------------------------------

func TestHDRCommit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Phong HDR keeps specular values above 1 while Fragment still clamps.
		dc := NewContext(24, 20)
		dc.Cull = CullNone
		dc.EnableHDR()
		shader := NewPhongShader(Identity(), Vector{0, 0, 1}, Vector{0, 0, 2})
		shader.ObjectColor = Color{4, 2, 0.5, 1}
		shader.AmbientColor = White
		shader.DiffuseColor = Black
		shader.SpecularPower = 0
		dc.Shader = shader
		info := dc.DrawTriangle(seedTriangle())
		c.Check(info.UpdatedPixels > 0, "phong hdr: no pixels updated: %+v", info)

		snapshot := dc.HDRSnapshot()
		c.Check(len(snapshot) == 24*20, "phong hdr: snapshot length %d", len(snapshot))
		if len(snapshot) != 24*20 {
			return
		}
		// Premultiplied with alpha 1, so linear RGB equals the shader output.
		p := hdrSnapAt(snapshot, 24, 12, 10)
		c.Check(closeTo(p.R, 4, 1e-9) && closeTo(p.G, 2, 1e-9) && closeTo(p.B, 0.5, 1e-9),
			"phong hdr: stored linear rgb = %v", p)
		c.Check(p.A == 1, "phong hdr: stored alpha = %v", p.A)
		raw := shader.FragmentHDR(Vertex{Position: Vector{}, Normal: Vector{0, 0, 1}})
		c.Check(closeTo(raw.R, 4, 1e-9) && closeTo(raw.G, 2, 1e-9),
			"phong FragmentHDR rgb = %v", raw)
		legacy := shader.Fragment(Vertex{Position: Vector{}, Normal: Vector{0, 0, 1}})
		c.Check(closeTo(legacy.R, 1, 1e-9) && closeTo(legacy.G, 1, 1e-9),
			"phong Fragment must stay clamped: %v", legacy)
		// Byte buffer untouched: HDR target is independent.
		c.Check(dc.ColorBuffer.NRGBAAt(12, 10).A == 0,
			"hdr phong: byte buffer was written")

		// A shader without FragmentHDR falls back to Fragment, byte-unclamped.
		dc2 := hdrTestContext(4, 4, Black)
		dc2.Shader = &hdrLegacyShader{Color{2, 3, 0.25, 1}}
		dc2.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		legacySnap := dc2.HDRSnapshot()
		lp := hdrSnapAt(legacySnap, 4, 2, 2)
		c.Check(closeTo(lp.R, 2, 1e-9) && closeTo(lp.G, 3, 1e-9) && closeTo(lp.B, 0.25, 1e-9),
			"legacy shader in hdr mode: %v", lp)

		// Multi-layer transparency with premultiplied alpha blend. The clear
		// is transparent so destination alpha accumulates from zero.
		dc3 := hdrTestContext(6, 6, Transparent)
		dc3.WriteDepth = false // layers at the same z
		dc3.Shader = &hdrFixedShader{Color{0.5, 1.5, 3, 0.5}}
		dc3.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		dc3.Shader = &hdrFixedShader{Color{2, 0.25, 1, 0.5}}
		dc3.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		third := hdrSnapAt(dc3.HDRSnapshot(), 6, 3, 3)
		// layer1: (0.25,0.75,1.5,0.5); layer2 over: 2*0.5 + .25*0.5=1.125, .25*.5+.75*.5=.5, 1*.5+1.5*.5=1.25, .5+.5*.5=.75
		c.Check(closeTo(third.R, 1.125, 1e-9) && closeTo(third.G, 0.5, 1e-9) &&
			closeTo(third.B, 1.25, 1e-9) && closeTo(third.A, 0.75, 1e-9),
			"hdr alpha blend: got %v", third)

		// AlphaBlend off replaces instead of blending.
		dc4 := hdrTestContext(6, 6, Black)
		dc4.AlphaBlend = false
		dc4.WriteDepth = false
		dc4.Shader = &hdrFixedShader{Color{0.5, 1.5, 3, 0.5}}
		dc4.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		dc4.Shader = &hdrFixedShader{Color{2, 0.25, 1, 0.25}}
		dc4.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		four := hdrSnapAt(dc4.HDRSnapshot(), 6, 3, 3)
		c.Check(closeTo(four.R, 0.5, 1e-9) && closeTo(four.G, 0.0625, 1e-9) &&
			closeTo(four.B, 0.25, 1e-9) && closeTo(four.A, 0.25, 1e-9),
			"hdr replace: got %v", four)

		// Discard commits nothing; partial discard splits the quad. Two
		// triangles cover the whole viewport so both halves are actually
		// rasterized candidates.
		dc5 := hdrTestContext(8, 8, Transparent)
		dc5.Shader = &hdrDiscardShader{Color{1, 2, 3, 1}}
		hdrFillQuads(dc5)
		snap5 := dc5.HDRSnapshot()
		left := hdrSnapAt(snap5, 8, 1, 4)
		right := hdrSnapAt(snap5, 8, 7, 4)
		c.Check(closeTo(left.R, 1, 1e-9) && left.A == 1,
			"discard kept left half: %v", left)
		c.Check(right == (Color{}), "discard wrote right half: %v", right)
		// Byte buffer must stay untouched by the discard too.
		c.Check(dc5.ColorBuffer.NRGBAAt(6, 4).A == 0,
			"discard wrote byte buffer")

		// Depth mask / read mask behaviour in HDR mode.
		dc6 := hdrTestContext(4, 4, Black)
		dc6.Shader = &hdrFixedShader{Color{3, 3, 3, 1}}
		dc6.WriteDepth = false
		dc6.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		c.Check(dc6.DepthBuffer[2*4+2] == math.MaxFloat64,
			"WriteDepth=false wrote depth %v", dc6.DepthBuffer[2*4+2])
		snap6 := dc6.HDRSnapshot()
		c.Check(hdrSnapAt(snap6, 4, 2, 2).R == 3, "WriteDepth=false blocked color")
		dc6.WriteColor = false
		dc6.WriteDepth = true
		dc6.ReadDepth = true
		dc6.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		snap6b := dc6.HDRSnapshot()
		c.Check(hdrSnapAt(snap6b, 4, 2, 2).R == 3,
			"WriteColor=false changed color %v", hdrSnapAt(snap6b, 4, 2, 2))
		c.Check(dc6.DepthBuffer[2*4+2] != math.MaxFloat64,
			"WriteColor=false blocked depth write")

		// Color mask off means later closer geometry cannot repaint.
		dc7 := hdrTestContext(4, 4, Black)
		dc7.Shader = &hdrFixedShader{Color{1, 1, 1, 1}}
		dc7.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		dc7.WriteColor = false
		dc7.Shader = &hdrFixedShader{Color{5, 5, 5, 1}}
		dc7.DrawTriangle(hdrQuad(-0.5, -0.5, 0.5, 0.5))
		snap7 := dc7.HDRSnapshot()
		c.Check(hdrSnapAt(snap7, 4, 2, 2).R == 1,
			"WriteColor=false let a later draw repaint: %v", hdrSnapAt(snap7, 4, 2, 2))

		// DepthBias still applies in HDR mode.
		dc8 := hdrTestContext(4, 4, Black)
		dc8.Shader = &hdrFixedShader{Color{1, 1, 1, 1}}
		dc8.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		dc8.Shader = &hdrFixedShader{Color{2, 2, 2, 1}}
		dc8.DepthBias = 0.1 // push the second quad behind the first
		dc8.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		c.Check(hdrSnapAt(dc8.HDRSnapshot(), 4, 2, 2).R == 1,
			"DepthBias was ignored in hdr mode")

		// Thick lines go through the same HDR target.
		dc9 := hdrTestContext(16, 16, Transparent)
		dc9.Shader = &hdrFixedShader{Color{2, 1, 0.5, 1}}
		dc9.LineWidth = 6
		dc9.DrawLine(NewLineForPoints(Vector{-1, 0, 0}, Vector{1, 0, 0}))
		snap9 := dc9.HDRSnapshot()
		c.Check(hdrSnapAt(snap9, 16, 8, 8).R == 2, "thick line hdr: %v", hdrSnapAt(snap9, 16, 8, 8))
		c.Check(hdrSnapAt(snap9, 16, 8, 0) == (Color{}), "thick line leaked")
		c.Check(dc9.ColorBuffer.NRGBAAt(8, 8).A == 0, "thick line wrote byte buffer")

		// A triangle crossing the clip plane must still reach the HDR target
		// through the real clipping chain. Clip-space positions are computed
		// up front and the pass-through shader leaves them untouched, so
		// DrawTriangle must run ClipTriangle.
		dc10 := hdrTestContext(32, 32, Black)
		dc10.Shader = &hdrPassThroughShader{Color{1, 1.5, 2, 1}}
		matrix := LookAt(Vector{0, 0, -3}, Vector{0, 0, 0}, Vector{0, 1, 0}).
			Perspective(45, 1, 0.5, 10)
		world := []Vector{
			{-0.5, -0.5, -2.7}, // behind the near plane
			{0.5, -0.5, -0.5},
			{0, 0.5, -0.5},
		}
		var verts [3]Vertex
		outside := false
		for i, p := range world {
			w := matrix.MulPositionW(p)
			verts[i] = Vertex{Position: w.Vector(), Normal: Vector{0, 0, 1}, Output: w}
			outside = outside || w.Outside()
		}
		c.Check(outside, "clip test geometry is not outside the view volume")
		tri := NewTriangle(verts[0], verts[1], verts[2])
		c.Check(tri.V1.Outside() || tri.V2.Outside() || tri.V3.Outside(),
			"clip test geometry lost its clip-space output")
		info10 := dc10.DrawTriangle(tri)
		c.Check(info10.UpdatedPixels > 0, "clipped triangle drew nothing: %+v", info10)
		c.Check(info10.UpdatedPixels > 0, "clipped triangle drew nothing: %+v", info10)
		snap10 := dc10.HDRSnapshot()
		found := false
		for _, p := range snap10 {
			if closeTo(p.R, 1, 1e-9) && closeTo(p.G, 1.5, 1e-9) && closeTo(p.B, 2, 1e-9) {
				found = true
				break
			}
		}
		c.Check(found, "clipped triangle stored wrong values")

		// ClearColorBufferWith writes the HDR target in linear space (no clamp)
		// and leaves the byte buffer alone. Stored values are premultiplied.
		dc11 := hdrTestContext(3, 2, Black)
		dc11.ClearColorBufferWith(Color{4, 0.5, 2, 0.5})
		snap11 := dc11.HDRSnapshot()
		for i, p := range snap11 {
			c.Check(closeTo(p.R, 2, 1e-12) && closeTo(p.G, 0.25, 1e-12) &&
				closeTo(p.B, 1, 1e-12) && closeTo(p.A, 0.5, 1e-12),
				"hdr clear pixel %d = %v", i, p)
		}
		c.Check(dc11.ColorBuffer.NRGBAAt(0, 0).A == 0, "hdr clear wrote byte buffer")

		// A fragment with fractional alpha is stored premultiplied and keeps
		// its >1 rgb.
		dc11b := hdrTestContext(3, 2, Transparent)
		dc11b.Shader = &hdrFixedShader{Color{2, 4, 0.5, 0.5}}
		dc11b.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		p11b := hdrSnapAt(dc11b.HDRSnapshot(), 3, 1, 1)
		c.Check(closeTo(p11b.R, 1, 1e-12) && closeTo(p11b.G, 2, 1e-12) &&
			closeTo(p11b.B, 0.25, 1e-12) && closeTo(p11b.A, 0.5, 1e-12),
			"fractional alpha not premultiplied: %v", p11b)

		// ClearDepthBuffer only touches depth.
		dc12 := hdrTestContext(3, 2, Color{1, 1, 1, 1})
		dc12.ClearDepthBufferWith(5)
		c.Check(dc12.DepthBuffer[0] == 5, "depth clear value %v", dc12.DepthBuffer[0])
		c.Check(hdrSnapAt(dc12.HDRSnapshot(), 3, 0, 0).R == 1, "depth clear touched hdr")

		// Input colors are used as linear values; no implicit conversion.
		texSh := NewTextureShader(Identity(), &hdrFixedTexture{})
		dc13 := hdrTestContext(4, 4, Black)
		dc13.Shader = texSh
		dc13.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		c.Check(hdrSnapAt(dc13.HDRSnapshot(), 4, 2, 2).R == 0.75,
			"texture sample treated as non-linear: %v", hdrSnapAt(dc13.HDRSnapshot(), 4, 2, 2).R)

		// Parallel batch drawing at non-uniform depths must stay race-free
		// and depth-correct. Every triangle overlaps the same pixels at a
		// distinct depth, so workers contend while depth decides the winner.
		dc14 := NewContext(32, 32)
		dc14.Cull = CullNone
		dc14.EnableHDR()
		dc14.ClearColorBufferWith(Transparent)
		mesh := NewEmptyMesh()
		for i := 0; i < 16; i++ {
			z := -0.5 + float64(i)*0.0625
			color := Color{float64(i) + 1, 0.5, 1, 1}
			mesh.Add(NewTriangleMesh([]*Triangle{
				NewTriangle(
					Vertex{Position: Vector{-0.95, -0.95, z}, Color: color},
					Vertex{Position: Vector{0.95, -0.95, z}, Color: color},
					Vertex{Position: Vector{0.95, 0.95, z}, Color: color}),
				NewTriangle(
					Vertex{Position: Vector{-0.95, -0.95, z}, Color: color},
					Vertex{Position: Vector{0.95, 0.95, z}, Color: color},
					Vertex{Position: Vector{-0.95, 0.95, z}, Color: color}),
			}))
		}
		dc14.Shader = &hdrVertexColorShader{}
		info14 := dc14.DrawMesh(mesh)
		c.Check(info14.TotalPixels > 0, "parallel hdr draw did nothing: %+v", info14)
		pix := hdrSnapAt(dc14.HDRSnapshot(), 32, 16, 16)
		// z = -0.5 is the front-most depth, owned by triangle 0 (R = 1).
		c.Check(closeTo(pix.R, 1, 1e-9) && closeTo(pix.G, 0.5, 1e-9) && closeTo(pix.B, 1, 1e-9),
			"parallel hdr depth result %v", pix)
	})
}

// hdrFixedTexture samples a constant linear color.
type hdrFixedTexture struct{}

func (t *hdrFixedTexture) Sample(u, v float64) Color         { return Color{0.75, 0.25, 0.5, 1} }
func (t *hdrFixedTexture) BilinearSample(u, v float64) Color { return Color{0.75, 0.25, 0.5, 1} }

// -----------------------------------------------------------------------------
// TestBloomPyramid
// -----------------------------------------------------------------------------

func TestBloomPyramid(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A 3x5 pattern whose values differ per texel. The references below
		// came from an independent implementation of the same area/center
		// alignment rules. For example L1[2][1] mixes source rows 3 and 4
		// (weights 2/3 and 1) and columns 1 and 2 (weights 1/2 and 1).
		pattern := []float64{1, 2, 4, 8, 16, 32, 3, 5, 7, 11, 13, 17, 19, 23, 29}
		level := func(p []float64) []Color {
			out := make([]Color, len(p))
			for i, v := range p {
				out[i] = Color{v, v, v, 1}
			}
			return out
		}

		levels := hdrBuildPyramid(level(pattern), 3, 5, 4)
		c.Check(len(levels) == 4, "pyramid level count %d", len(levels))
		if len(levels) != 4 {
			return
		}
		c.Check(levels[1].width == 2 && levels[1].height == 3,
			"level1 size %dx%d", levels[1].width, levels[1].height)
		c.Check(levels[2].width == 1 && levels[2].height == 2,
			"level2 size %dx%d", levels[2].width, levels[2].height)
		c.Check(levels[3].width == 1 && levels[3].height == 1,
			"level3 size %dx%d", levels[3].width, levels[3].height)

		l1 := levels[1].pixels
		areaAvg := func(y, x int) float64 { return l1[y*2+x].R }
		// Row 0: src rows 0,1 each contribute 2px, src cols 0,1 -> (1+2+8+16)/4
		c.Check(closeTo(areaAvg(0, 0), 5.066666666666666, 1e-9),
			"L1[0][0]=%v want 5.066666", areaAvg(0, 0))
		c.Check(closeTo(areaAvg(0, 1), 12.666666666666666, 1e-9),
			"L1[0][1]=%v want 12.666666", areaAvg(0, 1))
		// Odd height row: src row 2 full + src row 3 half of 2/4? row 3 weight 2/3...
		c.Check(closeTo(areaAvg(2, 0), 16.866666666666667, 1e-9),
			"L1[2][0]=%v want 16.866666", areaAvg(2, 0))
		c.Check(closeTo(areaAvg(2, 1), 22.466666666666665, 1e-9),
			"L1[2][1]=%v want 22.466666", areaAvg(2, 1))
		// Last row must not be padded or duplicated: those would give 11.5
		// and 23.0.
		c.Check(!closeTo(areaAvg(2, 1), 11.5, 1e-9) && !closeTo(areaAvg(2, 1), 23.0, 1e-9),
			"odd-size area weighting degenerated to pad/duplicate")
		// Odd width column: src col 2 weighted half in each output column.
		c.Check(closeTo(areaAvg(1, 1), 12.266666666666667, 1e-9),
			"L1[1][1]=%v want 12.266666", areaAvg(1, 1))
		c.Check(closeTo(levels[2].pixels[0].R, 9.066666666666666, 1e-9),
			"L2[0]=%v want 9.066666", levels[2].pixels[0].R)
		c.Check(closeTo(levels[2].pixels[1].R, 16.266666666666666, 1e-9),
			"L2[1]=%v want 16.266666", levels[2].pixels[1].R)

		// Reconstruction: Ulast = Dlast, Ui = (Di + bilinear(Ui+1)) / 2.
		u := hdrReconstructBloom(levels)
		ref := []float64{
			4.483333333333333, 5.933333333333333, 7.883333333333333,
			8.323333333333332, 13.173333333333333, 22.023333333333333,
			6.333333333333333, 8.033333333333333, 9.733333333333333,
			12.133333333333333, 13.833333333333333, 16.533333333333333,
			17.333333333333333, 20.033333333333333, 23.733333333333333,
		}
		for i, want := range ref {
			c.Check(closeTo(u[i].R, want, 1e-9), "U0[%d]=%v want %v", i, u[i].R, want)
		}

		// Bilinear upsampling is normalized texel-center aligned and clamped.
		up := hdrBilinearUpsample(levels[2].pixels, 1, 2, 3, 5)
		c.Check(closeTo(up[0].R, 9.066666666666666, 1e-9),
			"edge clamp upsample top=%v", up[0].R)
		c.Check(closeTo(up[14].R, 16.266666666666666, 1e-9),
			"edge clamp upsample bottom=%v", up[14].R)

		// Zero threshold: a bright texel keeps its own color since the
		// luminance ratio is 1; L=0 yields zero.
		high := hdrHighlight(Color{0, 0, 4, 1}, 0)
		c.Check(closeTo(high.B, 4, 1e-12) && high.R == 0, "highlight ratio %v", high)
		mid := hdrHighlight(Color{0, 0, 0.5, 1}, 0)
		c.Check(closeTo(mid.B, 0.5, 1e-12), "unchanged highlight %v", mid)
		zeroL := hdrHighlight(Color{0, 0, 0, 1}, 0)
		c.Check(zeroL == (Color{0, 0, 0, 1}), "L=0 highlight %v", zeroL)
		// Threshold above luminance produces nothing.
		above := hdrHighlight(Color{1, 0, 0, 1}, 2)
		c.Check(above == (Color{0, 0, 0, 1}), "threshold above luminance %v", above)
	})
}

// -----------------------------------------------------------------------------
// TestHDRResolve
// -----------------------------------------------------------------------------

func TestHDRResolve(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Resolve before enabling must fail.
		dc := NewContext(4, 4)
		_, err := dc.ResolveHDR(hdrTestSettings(Black))
		c.Check(err != nil, "resolve without hdr did not fail")

		// Invalid parameters must fail.
		dc.EnableHDR()
		bad := map[string]HDRSettings{
			"background alpha":    {Color{0, 0, 0, 0.5}, 1, 0, 0, 1},
			"background negative": {Color{-1, 0, 0, 1}, 1, 0, 0, 1},
			"background nan":      {Color{math.NaN(), 0, 0, 1}, 1, 0, 0, 1},
			"background inf":      {Color{0, math.Inf(1), 0, 1}, 1, 0, 0, 1},
			"exposure zero":       {Black, 0, 0, 0, 1},
			"exposure negative":   {Black, -1, 0, 0, 1},
			"exposure inf":        {Black, math.Inf(1), 0, 0, 1},
			"threshold negative":  {Black, 1, -1, 0, 1},
			"threshold nan":       {Black, 1, math.NaN(), 0, 1},
			"strength negative":   {Black, 1, 0, -1, 1},
			"strength inf":        {Black, 1, 0, math.Inf(1), 1},
			"levels zero":         {Black, 1, 0, 0, 0},
			"levels negative":     {Black, 1, 0, 0, -2},
		}
		for name, settings := range bad {
			_, err := dc.ResolveHDR(settings)
			c.Check(err != nil, "invalid settings %q accepted", name)
		}

		// Resolve must not mutate the float target or depth buffer, and
		// repeated resolves at different exposures must be independent.
		dc2 := hdrTestContext(8, 6, Color{0.5, 0.25, 0.125, 1})
		dc2.DepthBuffer[0] = 0.75
		before := dc2.HDRSnapshot()
		settings := HDRSettings{Color{0.1, 0.2, 0.3, 1}, 1, 0.25, 0.5, 3}
		im1, err := dc2.ResolveHDR(settings)
		c.Check(err == nil, "valid resolve failed: %v", err)
		after := dc2.HDRSnapshot()
		c.Check(len(after) == len(before), "resolve changed snapshot length")
		if len(after) == len(before) {
			for i := range before {
				if before[i] != after[i] {
					c.Check(false, "resolve mutated hdr pixel %d: %v -> %v", i, before[i], after[i])
					break
				}
			}
		}
		c.Check(dc2.DepthBuffer[0] == 0.75, "resolve mutated depth buffer")

		dc2.DepthBuffer[0] = 0.75
		settings2 := settings
		settings2.Exposure = 4
		im2, err := dc2.ResolveHDR(settings2)
		c.Check(err == nil, "second resolve failed: %v", err)
		// Equal exposure must reproduce the first image exactly.
		im3, _ := dc2.ResolveHDR(settings)
		c.Check(bytesEqual(im1, im3), "repeated resolves differ")

		// Exposure order: a higher exposure must be brighter.
		c.Check(brighter(im2, im1), "exposure ordering wrong")

		// Resolve results own their storage: mutating one must not affect a
		// subsequent resolve.
		saved := *im2
		saved.Pix = make([]uint8, len(im2.Pix))
		copy(saved.Pix, im2.Pix)
		im2.Pix[0] ^= 0xff
		im4, _ := dc2.ResolveHDR(settings2)
		c.Check(bytesEqual(im4, &saved), "resolve results share storage")

		// Clearing the HDR target, and disable/re-enable initialisation.
		dc3 := hdrTestContext(5, 4, Color{1.5, 2, 0.5, 1})
		snap := dc3.HDRSnapshot()
		c.Check(hdrSnapAt(snap, 5, 0, 0).R == 1.5,
			"initial clear did not use ClearColor: %v", hdrSnapAt(snap, 5, 0, 0))
		// Enabling again preserves contents.
		dc3.EnableHDR()
		snap2 := dc3.HDRSnapshot()
		c.Check(hdrSnapAt(snap2, 5, 0, 0).R == 1.5, "second enable reset contents")
		dc3.Shader = &hdrFixedShader{Color{3, 0, 1, 1}}
		dc3.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		c.Check(hdrSnapAt(dc3.HDRSnapshot(), 5, 2, 2).R == 3, "hdr draw failed")
		// Disable keeps HDR contents inaccessible; enabling again re-clears from
		// ClearColor.
		dc3.DisableHDR()
		c.Check(dc3.HDRSnapshot() == nil, "snapshot after disable is not nil")
		_, err = dc3.ResolveHDR(settings)
		c.Check(err != nil, "resolve after disable did not fail")
		dc3.EnableHDR()
		reEnabled := dc3.HDRSnapshot()
		c.Check(hdrSnapAt(reEnabled, 5, 2, 2).R == 1.5,
			"re-enable did not clear to ClearColor: %v", hdrSnapAt(reEnabled, 5, 2, 2))

		// Snapshot ownership: mutating the returned copy must not alter the
		// target, and a later draw must not change a previously returned copy.
		dc4 := hdrTestContext(3, 3, Transparent)
		first := dc4.HDRSnapshot()
		first[0] = Color{9, 9, 9, 1}
		c.Check(hdrSnapAt(dc4.HDRSnapshot(), 3, 0, 0) == (Color{}),
			"snapshot aliases the target")
		second := dc4.HDRSnapshot()
		dc4.Shader = &hdrFixedShader{Color{1, 1, 1, 1}}
		dc4.DrawTriangle(hdrQuad(-1, -1, 1, 1))
		c.Check(second[0] == (Color{}), "later draw mutated old snapshot")
		c.Check(first[0] == (Color{9, 9, 9, 1}), "snapshot copy was clobbered")

		// Byte buffer and Image() remain byte-based even with HDR enabled.
		dc5 := NewContext(12, 12)
		dc5.Cull = CullNone
		dc5.EnableHDR()
		dc5.Shader = NewSolidColorShader(Identity(), Color{1, 0.25, 0.5, 1})
		dc5.DrawTriangle(seedTriangle())
		c.Check(dc5.ColorBuffer.NRGBAAt(6, 6).A == 0, "hdr leaked into byte buffer")
		if img, ok := dc5.Image().(*image.NRGBA); ok {
			c.Check(img == dc5.ColorBuffer, "Image() changed")
		} else {
			c.Check(false, "Image() type changed: %T", dc5.Image())
		}
		// Default (non-HDR) rendering still writes bytes.
		dc6 := NewContext(12, 12)
		dc6.Cull = CullNone
		dc6.Shader = NewSolidColorShader(Identity(), Color{1, 0.25, 0.5, 1})
		dc6.DrawTriangle(seedTriangle())
		c.Check(dc6.ColorBuffer.NRGBAAt(6, 6).R == 255, "default draw broken")

		// The 3x5 resolve test makes the exposure order, background compositing
		// and gamma encoding concrete with non-constant values. Each pixel is
		// one object-space unit square mapped onto exactly one destination
		// pixel by hdrPixelShader.
		bloomContext := hdrTestContext(3, 5, Black)
		bloomContext.AlphaBlend = false
		bloomContext.WriteDepth = false
		bloomContext.Shader = &hdrPixelShader{3, 5}
		for y := 0; y < 5; y++ {
			for x := 0; x < 3; x++ {
				red := float64((x+1))*0.2 + float64(y)*0.15
				tri := NewTriangle(
					Vertex{Position: Vector{float64(x), float64(y), 0}, Color: Color{red, 0.5, 0.25, 1}},
					Vertex{Position: Vector{float64(x), float64(y + 1), 0}, Color: Color{red, 0.5, 0.25, 1}},
					Vertex{Position: Vector{float64(x + 1), float64(y), 0}, Color: Color{red, 0.5, 0.25, 1}},
				)
				bloomContext.DrawTriangle(tri)
				tri = NewTriangle(
					Vertex{Position: Vector{float64(x + 1), float64(y), 0}, Color: Color{red, 0.5, 0.25, 1}},
					Vertex{Position: Vector{float64(x), float64(y + 1), 0}, Color: Color{red, 0.5, 0.25, 1}},
					Vertex{Position: Vector{float64(x + 1), float64(y + 1), 0}, Color: Color{red, 0.5, 0.25, 1}},
				)
				bloomContext.DrawTriangle(tri)
			}
		}
		// Sanity: the grid rendered one distinct color per pixel.
		grid := bloomContext.HDRSnapshot()
		c.Check(closeTo(hdrSnapAt(grid, 3, 0, 0).R, 0.2, 1e-9) &&
			closeTo(hdrSnapAt(grid, 3, 2, 4).R, 1.2, 1e-9),
			"grid shader did not fill the target: %v %v",
			hdrSnapAt(grid, 3, 0, 0), hdrSnapAt(grid, 3, 2, 4))
		solution := HDRSettings{
			Background:     Black,
			Exposure:       1,
			BloomThreshold: 0.1,
			BloomStrength:  0.5,
			BloomLevels:    3,
		}
		out, err := bloomContext.ResolveHDR(solution)
		c.Check(err == nil, "bloom resolve failed: %v", err)
		want := [][3]uint8{
			{143, 188, 147},
			{177, 188, 148},
			{198, 188, 148},
			{171, 188, 148},
			{194, 188, 148},
			{210, 188, 148},
			{190, 188, 148},
			{207, 188, 148},
			{219, 188, 148},
			{205, 188, 148},
			{217, 188, 148},
			{227, 189, 148},
			{215, 188, 148},
			{225, 189, 148},
			{232, 189, 148},
		}
		for y := 0; y < 5; y++ {
			for x := 0; x < 3; x++ {
				got := out.NRGBAAt(x, y)
				ref := want[y*3+x]
				c.Check(got.R == ref[0] && got.G == ref[1] && got.B == ref[2] && got.A == 255,
					"bloom byte (%d,%d) = %v want %v", x, y, got, ref)
			}
		}
		// Gamma-only resolve (strength 0, black bg) should be dimmer than
		// bloom resolve.
		plain := solution
		plain.BloomStrength = 0
		noBloom, _ := bloomContext.ResolveHDR(plain)
		c.Check(byteSum(noBloom) < byteSum(out), "bloom resolve is not brighter than plain")

		// Background alpha validation already covered; a brighter background
		// raises the encoded result where the target is not opaque.
		bgSettings := solution
		bgSettings.Background = Color{0.5, 0.5, 0.5, 1}
		// A transparent target lets the background show through; an opaque
		// black target hides it.
		blank := hdrTestContext(3, 5, Transparent)
		black, _ := blank.ResolveHDR(solution)
		gray, _ := blank.ResolveHDR(bgSettings)
		c.Check(brighter(gray, black), "background does not brighten the resolve")
		opaque := hdrTestContext(3, 5, Black)
		opaqueBlack, _ := opaque.ResolveHDR(solution)
		opaqueGray, _ := opaque.ResolveHDR(bgSettings)
		c.Check(bytesEqual(opaqueBlack, opaqueGray),
			"background leaked through an opaque target")

		// sRGB transfer function sanity: linear 0.5 encodes above 0.5,
		// 0.0031308 boundary is continuous.
		c.Check(closeTo(linearToSRGB(0.5), 0.7353569830524495, 1e-12),
			"sRGB(0.5)=%v", linearToSRGB(0.5))
		c.Check(closeTo(linearToSRGB(0.0031308), 12.92*0.0031308, 1e-12),
			"sRGB boundary=%v", linearToSRGB(0.0031308))

		// Byte encoding rounds to nearest, not truncation. Pick values from the
		// encoded domain and confirm Round matches floor/ceil as appropriate.
		checkedUp, checkedDown := false, false
		for _, raw := range []float64{0.2, 0.4, 0.6, 0.9, 1.0, 0.35, 3.0, 255.4, 255.6, 10.6} {
			b := Round(raw)
			if raw-math.Floor(raw) > 0.55 && b == int(math.Ceil(raw)) {
				checkedUp = true
			}
			if raw-math.Floor(raw) < 0.45 && b == int(math.Floor(raw)) {
				checkedDown = true
			}
		}
		c.Check(checkedUp && checkedDown, "sRGB byte encoding does not round to nearest")
		// The resolve's encoding path uses that same Round.
		c.Check(Round(linearToSRGB(0.5)*255) == 188,
			"encoded mid gray %d", Round(linearToSRGB(0.5)*255))
	})
}

func bytesEqual(a, b *image.NRGBA) bool {
	if a.Bounds() != b.Bounds() {
		return false
	}
	for i := range a.Pix {
		if a.Pix[i] != b.Pix[i] {
			return false
		}
	}
	return true
}

func brighter(a, b *image.NRGBA) bool {
	return byteSum(a) > byteSum(b)
}

func byteSum(im *image.NRGBA) uint64 {
	var s uint64
	for i := 0; i < len(im.Pix); i += 4 {
		s += uint64(im.Pix[i]) + uint64(im.Pix[i+1]) + uint64(im.Pix[i+2])
	}
	return s
}
