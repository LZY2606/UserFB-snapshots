package fauxgl

import (
	"fmt"
	"image"
	"image/color"
	"os"
	"sort"
	"testing"
)

// newIDContext returns a small offscreen context configured for the opaque
// object-ID pass: no alpha blending, depth read/write on, identity transform
// so vertex positions are used directly as normalized device coordinates.
func newIDContext(width, height int) *Context {
	dc := NewContext(width, height)
	dc.Shader = NewSolidColorShader(Identity(), Color{1, 1, 1, 1})
	dc.AlphaBlend = false
	dc.EnableObjectIDs()
	return dc
}

// quadMesh returns a mesh of two triangles covering the NDC rectangle
// [x0,x1] x [y0,y1] at the given depth.
func quadMesh(x0, y0, x1, y1, z float64) *Mesh {
	p := func(x, y float64) Vertex {
		return Vertex{Position: Vector{x, y, z}}
	}
	t1 := NewTriangle(p(x0, y0), p(x1, y0), p(x1, y1))
	t2 := NewTriangle(p(x0, y0), p(x1, y1), p(x0, y1))
	return NewTriangleMesh([]*Triangle{t1, t2})
}

func fullQuadMesh(z float64) *Mesh {
	return quadMesh(-1, -1, 1, 1, z)
}

func mustDrawMeshID(t *testing.T, dc *Context, mesh *Mesh, id uint32) RasterizeInfo {
	t.Helper()
	info, err := dc.DrawMeshID(mesh, id)
	if err != nil {
		t.Fatalf("DrawMeshID(%d): %v", id, err)
	}
	return info
}

func countIDs(dc *Context) map[uint32]uint64 {
	counts := make(map[uint32]uint64)
	for _, id := range dc.ObjectIDBuffer {
		counts[id]++
	}
	return counts
}

func TestObjectIDBufferAllocation(t *testing.T) {
	dc := NewContext(16, 8)
	if dc.ObjectIDBuffer != nil {
		t.Fatal("ObjectIDBuffer should be nil before EnableObjectIDs")
	}
	// untracked drawing still works and allocates nothing
	dc.DrawMesh(fullQuadMesh(0.5))
	if dc.ObjectIDBuffer != nil {
		t.Fatal("ObjectIDBuffer should stay nil without EnableObjectIDs")
	}
	if n := len(dc.VisibleObjectPixels()); n != 0 {
		t.Fatalf("VisibleObjectPixels without tracking: got %d entries", n)
	}
	dc.EnableObjectIDs()
	if len(dc.ObjectIDBuffer) != len(dc.DepthBuffer) {
		t.Fatalf("ObjectIDBuffer size %d, depth buffer size %d",
			len(dc.ObjectIDBuffer), len(dc.DepthBuffer))
	}
	for i, id := range dc.ObjectIDBuffer {
		if id != 0 {
			t.Fatalf("ObjectIDBuffer[%d] = %d, want 0", i, id)
		}
	}
	// enabling again keeps the existing buffer
	dc.ObjectIDBuffer[0] = 42
	dc.EnableObjectIDs()
	if dc.ObjectIDBuffer[0] != 42 {
		t.Fatal("EnableObjectIDs should be a no-op once enabled")
	}
}

func TestObjectIDValidation(t *testing.T) {
	mesh := fullQuadMesh(0.5)

	// tracking not enabled
	dc := NewContext(8, 8)
	dc.AlphaBlend = false
	if _, err := dc.DrawMeshID(mesh, 1); err == nil {
		t.Fatal("expected error without EnableObjectIDs")
	}

	// zero ID
	dc = newIDContext(8, 8)
	if _, err := dc.DrawMeshID(mesh, 0); err == nil {
		t.Fatal("expected error for id 0")
	}

	// bad render state, checked before any shader runs
	type mutation func(*Context)
	cases := []struct {
		name string
		mut  mutation
	}{
		{"ReadDepth=false", func(dc *Context) { dc.ReadDepth = false }},
		{"WriteDepth=false", func(dc *Context) { dc.WriteDepth = false }},
		{"AlphaBlend=true", func(dc *Context) { dc.AlphaBlend = true }},
		{"Wireframe=true", func(dc *Context) { dc.Wireframe = true }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			dc := newIDContext(8, 8)
			shader := &countingShader{}
			dc.Shader = shader
			c.mut(dc)
			if _, err := dc.DrawMeshID(mesh, 1); err == nil {
				t.Fatalf("expected error with %s", c.name)
			}
			if shader.vertexCalls != 0 || shader.fragmentCalls != 0 {
				t.Fatalf("shader invoked before validation failed: %d vertex, %d fragment",
					shader.vertexCalls, shader.fragmentCalls)
			}
		})
	}

	// WriteColor=false is allowed (depth-only pass)
	dc = newIDContext(8, 8)
	dc.WriteColor = false
	if _, err := dc.DrawMeshID(mesh, 1); err != nil {
		t.Fatalf("WriteColor=false should be allowed: %v", err)
	}
}

type countingShader struct {
	vertexCalls   int
	fragmentCalls int
}

func (s *countingShader) Vertex(v Vertex) Vertex {
	s.vertexCalls++
	v.Output = v.Position.VectorW()
	return v
}

func (s *countingShader) Fragment(v Vertex) Color {
	s.fragmentCalls++
	return Color{1, 1, 1, 1}
}

func TestObjectIDFullOcclusion(t *testing.T) {
	const w, h = 32, 32
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 1)
	if n := dc.VisibleObjectPixels()[1]; n != w*h {
		t.Fatalf("id 1 owns %d pixels, want %d", n, w*h)
	}
	// nearer quad completely hides the first one
	mustDrawMeshID(t, dc, fullQuadMesh(0.2), 2)
	counts := dc.VisibleObjectPixels()
	if counts[1] != 0 {
		t.Fatalf("occluded id 1 still owns %d pixels", counts[1])
	}
	if counts[2] != w*h {
		t.Fatalf("id 2 owns %d pixels, want %d", counts[2], w*h)
	}
}

func TestObjectIDPartialOverlap(t *testing.T) {
	const w, h = 64, 64
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 1)
	// nearer quad covers the right half of the screen
	mustDrawMeshID(t, dc, quadMesh(0, -1, 1, 1, 0.2), 2)
	counts := dc.VisibleObjectPixels()
	if counts[1] != w*h/2 {
		t.Fatalf("id 1 owns %d pixels, want %d", counts[1], w*h/2)
	}
	if counts[2] != w*h/2 {
		t.Fatalf("id 2 owns %d pixels, want %d", counts[2], w*h/2)
	}
}

// holeShader discards fragments on the left half of the screen.
type holeShader struct{}

func (s *holeShader) Vertex(v Vertex) Vertex {
	v.Output = v.Position.VectorW()
	return v
}

func (s *holeShader) Fragment(v Vertex) Color {
	if v.Position.X < 0 {
		return Discard
	}
	return Color{1, 1, 1, 1}
}

func TestObjectIDDiscardHoles(t *testing.T) {
	const w, h = 64, 64
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 1)
	// nearer quad with discard holes: holes must keep the old owner
	dc.Shader = &holeShader{}
	mustDrawMeshID(t, dc, fullQuadMesh(0.2), 2)
	counts := dc.VisibleObjectPixels()
	if counts[1] != w*h/2 {
		t.Fatalf("id 1 owns %d pixels, want %d (discard holes)", counts[1], w*h/2)
	}
	if counts[2] != w*h/2 {
		t.Fatalf("id 2 owns %d pixels, want %d", counts[2], w*h/2)
	}
}

func TestObjectIDViewportClipping(t *testing.T) {
	const w, h = 32, 32
	dc := newIDContext(w, h)
	// quad extends well past the view volume on every side
	mustDrawMeshID(t, dc, quadMesh(-3, -3, 3, 3, 0.5), 7)
	if n := dc.VisibleObjectPixels()[7]; n != w*h {
		t.Fatalf("clipped id 7 owns %d pixels, want %d", n, w*h)
	}
}

func TestObjectIDLines(t *testing.T) {
	const w, h = 64, 64
	dc := newIDContext(w, h)
	v := func(x, y, z float64) Vertex {
		return Vertex{Position: Vector{x, y, z}}
	}
	// a horizontal line across the middle, plus one that crosses the
	// viewport boundary and gets clipped
	lines := []*Line{
		NewLine(v(-0.5, 0, 0.5), v(0.5, 0, 0.5)),
		NewLine(v(-3, 0.5, 0.5), v(3, 0.5, 0.5)),
	}
	mustDrawMeshID(t, dc, NewLineMesh(lines), 5)
	counts := dc.VisibleObjectPixels()
	if counts[5] == 0 {
		t.Fatal("lines owned no pixels")
	}
	if counts[5] >= w*h {
		t.Fatalf("lines own %d pixels, want less than %d", counts[5], w*h)
	}
	// every attributed pixel must lie near one of the two line rows
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			if dc.ObjectIDBuffer[y*w+x] != 5 {
				continue
			}
			// NDC y=0 -> row h/2, NDC y=0.5 -> row h/4 (y axis flipped)
			if d := y - h/2; d < -3 || d > 3 {
				if d := y - h/4; d < -3 || d > 3 {
					t.Fatalf("id 5 at unexpected pixel (%d, %d)", x, y)
				}
			}
		}
	}
}

func TestObjectIDDepthOnlyPass(t *testing.T) {
	const w, h = 32, 32
	dc := newIDContext(w, h)
	dc.WriteColor = false
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 9)
	if n := dc.VisibleObjectPixels()[9]; n != w*h {
		t.Fatalf("id 9 owns %d pixels, want %d", n, w*h)
	}
	// color buffer untouched
	for _, c := range dc.ColorBuffer.Pix {
		if c != 0 {
			t.Fatal("depth-only pass wrote to the color buffer")
		}
	}
}

func TestObjectIDSameIDTwice(t *testing.T) {
	const w, h = 64, 64
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, quadMesh(-1, -1, 0, 1, 0.5), 3)
	mustDrawMeshID(t, dc, quadMesh(0, -1, 1, 1, 0.5), 3)
	counts := dc.VisibleObjectPixels()
	if counts[3] != w*h {
		t.Fatalf("id 3 owns %d pixels after two submissions, want %d", counts[3], w*h)
	}
	if len(counts) != 1 {
		t.Fatalf("expected a single id, got %v", counts)
	}
}

func TestObjectIDEqualDepth(t *testing.T) {
	const w, h = 32, 32
	// later submission wins at equal depth
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 1)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 2)
	if counts := dc.VisibleObjectPixels(); counts[2] != w*h || counts[1] != 0 {
		t.Fatalf("equal depth: got %v, want id 2 to own everything", counts)
	}
	// swapping the submission order swaps the owner
	dc = newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 2)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 1)
	if counts := dc.VisibleObjectPixels(); counts[1] != w*h || counts[2] != 0 {
		t.Fatalf("equal depth reversed: got %v, want id 1 to own everything", counts)
	}
}

func TestObjectIDDepthBias(t *testing.T) {
	const w, h = 32, 32
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.4), 1)
	// geometrically farther, but the bias pulls it in front
	dc.DepthBias = -0.2
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 2)
	if counts := dc.VisibleObjectPixels(); counts[2] != w*h || counts[1] != 0 {
		t.Fatalf("depth bias: got %v, want id 2 to own everything", counts)
	}
	// depth buffer keeps the unbiased value
	if d := dc.DepthBuffer[0]; d != 0.75 { // NDC 0.5 -> screen 0.75
		t.Fatalf("depth buffer = %v, want unbiased 0.75", d)
	}
}

func TestObjectIDUntaggedOcclusion(t *testing.T) {
	const w, h = 32, 32
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 4)
	// an untagged draw hides the tagged one and claims the pixels as 0
	dc.DrawMesh(fullQuadMesh(0.2))
	if counts := dc.VisibleObjectPixels(); len(counts) != 0 {
		t.Fatalf("untagged occlusion: got %v, want no visible ids", counts)
	}
	for i, id := range dc.ObjectIDBuffer {
		if id != 0 {
			t.Fatalf("ObjectIDBuffer[%d] = %d, want 0", i, id)
		}
	}
}

func TestObjectIDClearing(t *testing.T) {
	const w, h = 32, 32
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 6)
	before := dc.VisibleObjectPixels()
	if before[6] != w*h {
		t.Fatalf("id 6 owns %d pixels, want %d", before[6], w*h)
	}
	// clearing color alone keeps the attribution
	dc.ClearColorBuffer()
	if counts := dc.VisibleObjectPixels(); counts[6] != w*h {
		t.Fatalf("after ClearColorBuffer: got %v", counts)
	}
	// clearing depth resets the attribution
	dc.ClearDepthBuffer()
	if counts := dc.VisibleObjectPixels(); len(counts) != 0 {
		t.Fatalf("after ClearDepthBuffer: got %v", counts)
	}
	// ... through every depth-clearing entry point
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 6)
	dc.ClearDepthBufferWith(12345)
	if counts := dc.VisibleObjectPixels(); len(counts) != 0 {
		t.Fatalf("after ClearDepthBufferWith: got %v", counts)
	}
	for i, id := range dc.ObjectIDBuffer {
		if id != 0 {
			t.Fatalf("ObjectIDBuffer[%d] = %d, want 0", i, id)
		}
	}
}

func TestObjectIDSnapshotIsolation(t *testing.T) {
	const w, h = 32, 32
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.5), 1)
	snapshot := dc.VisibleObjectPixels()
	// later drawing must not mutate a previously returned map
	mustDrawMeshID(t, dc, fullQuadMesh(0.2), 2)
	dc.ClearDepthBuffer()
	if snapshot[1] != w*h || len(snapshot) != 1 {
		t.Fatalf("snapshot mutated by later drawing: %v", snapshot)
	}
	snapshot[1] = 0 // caller owns the map; the context must not notice
	if counts := dc.VisibleObjectPixels(); len(counts) != 0 {
		t.Fatalf("context observed caller's map: %v", counts)
	}
}

// jitterMesh builds a mesh of many small quads scattered over the screen,
// all at the same depth, so that several parallel workers get involved.
func jitterMesh(seed, n int, z float64) *Mesh {
	var triangles []*Triangle
	for i := 0; i < n; i++ {
		// deterministic pseudo-random placement
		a := float64((seed*31+i*17)%29) / 29
		b := float64((seed*13+i*41)%31) / 31
		x := a*1.6 - 0.8
		y := b*1.6 - 0.8
		s := 0.1 + float64((seed+i)%7)*0.03
		for _, t := range quadMesh(x, y, x+s, y+s, z).Triangles {
			triangles = append(triangles, t)
		}
	}
	return NewTriangleMesh(triangles)
}

func TestObjectIDDeterministicAttribution(t *testing.T) {
	const w, h = 64, 64
	// two overlapping layers at strictly different depths
	render := func(farFirst bool) []uint32 {
		dc := newIDContext(w, h)
		near := jitterMesh(1, 40, 0.3)
		far := jitterMesh(2, 40, 0.6)
		if farFirst {
			mustDrawMeshID(t, dc, far, 21)
			mustDrawMeshID(t, dc, near, 20)
		} else {
			mustDrawMeshID(t, dc, near, 20)
			mustDrawMeshID(t, dc, far, 21)
		}
		buf := make([]uint32, len(dc.ObjectIDBuffer))
		copy(buf, dc.ObjectIDBuffer)
		return buf
	}
	reference := render(true)
	for trial := 0; trial < 5; trial++ {
		for _, farFirst := range []bool{true, false} {
			got := render(farFirst)
			for i := range got {
				if got[i] != reference[i] {
					t.Fatalf("trial %d farFirst=%v: pixel %d owned by %d, want %d",
						trial, farFirst, i, got[i], reference[i])
				}
			}
		}
	}
}

func TestObjectIDHorizontalAndDegenerate(t *testing.T) {
	const w, h = 17, 13
	dc := newIDContext(w, h)
	// triangle with a horizontal edge; used to hang the rasterizer
	horiz := NewTriangle(
		Vertex{Position: Vector{-0.8, -0.8, 0}},
		Vertex{Position: Vector{0.8, -0.8, 0}},
		Vertex{Position: Vector{0, 0.8, 0}},
	)
	info := mustDrawMeshID(t, dc, NewTriangleMesh([]*Triangle{horiz}), 8)
	if info.UpdatedPixels == 0 {
		t.Fatal("horizontal-edge triangle owned no pixels")
	}
	if dc.VisibleObjectPixels()[8] != info.UpdatedPixels {
		t.Fatalf("id 8 owns %d pixels, UpdatedPixels says %d",
			dc.VisibleObjectPixels()[8], info.UpdatedPixels)
	}
	// zero-area (collinear) triangle: completes, covers nothing
	degenerate := NewTriangle(
		Vertex{Position: Vector{-0.5, -0.5, 0}},
		Vertex{Position: Vector{0, 0, 0}},
		Vertex{Position: Vector{0.5, 0.5, 0}},
	)
	info = mustDrawMeshID(t, dc, NewTriangleMesh([]*Triangle{degenerate}), 9)
	if info.UpdatedPixels != 0 {
		t.Fatalf("degenerate triangle updated %d pixels", info.UpdatedPixels)
	}
	if counts := dc.VisibleObjectPixels(); counts[9] != 0 {
		t.Fatalf("degenerate triangle owns pixels: %v", counts)
	}
	// zero-length line inside a tagged mesh must also terminate
	v := Vertex{Position: Vector{0.25, 0.25, 0.5}}
	info = mustDrawMeshID(t, dc, NewLineMesh([]*Line{NewLine(v, v)}), 10)
	if counts := dc.VisibleObjectPixels(); counts[10] != 0 {
		t.Fatalf("zero-length line owns pixels: %v", counts)
	}
	_ = info
}

// TestObjectIDVisualization renders a small tagged scene and, when the
// FAUXGL_OBJECT_ID_PNG environment variable is set, writes a color-coded
// PNG of the ownership map and prints the final pixel count per ID.
// Colors in the PNG are for display only; assertions elsewhere always
// consult the uint32 buffer.
func TestObjectIDVisualization(t *testing.T) {
	path := os.Getenv("FAUXGL_OBJECT_ID_PNG")
	if path == "" {
		t.Skip("FAUXGL_OBJECT_ID_PNG not set")
	}
	const w, h = 128, 96
	dc := newIDContext(w, h)
	mustDrawMeshID(t, dc, fullQuadMesh(0.8), 1)                   // background plate
	mustDrawMeshID(t, dc, quadMesh(-0.9, -0.9, 0.1, 0.3, 0.5), 2) // left block
	mustDrawMeshID(t, dc, quadMesh(-0.2, -0.4, 0.9, 0.9, 0.6), 3) // right block (behind 2 where they cross)
	mustDrawMeshID(t, dc, quadMesh(-0.6, 0.4, 0.4, 0.95, 0.4), 4) // front strip
	v := func(x, y, z float64) Vertex { return Vertex{Position: Vector{x, y, z}} }
	mustDrawMeshID(t, dc, NewLineMesh([]*Line{
		NewLine(v(-1, -0.6, 0.3), v(1, -0.6, 0.3)),
	}), 5) // tagged line
	dc.DrawMesh(quadMesh(0.5, -1, 1, -0.2, 0.2)) // untagged occluder -> id 0

	counts := dc.VisibleObjectPixels()
	ids := make([]int, 0, len(counts))
	for id := range counts {
		ids = append(ids, int(id))
	}
	sort.Ints(ids)
	for _, id := range ids {
		fmt.Printf("object id %d: %d pixels\n", id, counts[uint32(id)])
	}

	palette := []color.NRGBA{
		{0x20, 0x20, 0x20, 0xff}, // 0: untagged
		{0x4c, 0x78, 0xa8, 0xff},
		{0xf5, 0x85, 0x18, 0xff},
		{0x54, 0xa2, 0x4b, 0xff},
		{0xe4, 0x57, 0x56, 0xff},
		{0x72, 0xb7, 0xb2, 0xff},
	}
	im := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			id := dc.ObjectIDBuffer[y*w+x]
			im.SetNRGBA(x, y, palette[int(id)%len(palette)])
		}
	}
	if err := SavePNG(path, im); err != nil {
		t.Fatal(err)
	}
	fmt.Printf("wrote %s\n", path)
}
