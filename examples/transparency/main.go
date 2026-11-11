// Command transparency renders a .cache/transparency.png demo showing
// two crossing transparent planes composited per pixel, with an opaque
// box in front occluding part of the scene (and part of the planes behind
// it). It uses the regular drawing entry points and the optional
// per-pixel order-independent transparency pass.
package main

import (
	"os"

	. "github.com/fogleman/fauxgl"
)

const (
	width  = 800
	height = 600
	fovy   = 30
	near   = 1
	far    = 20
)

var (
	eye    = V(0, 0.45, 4)
	center = V(0, 0, 0)
	up     = V(0, 1, 0)
	light  = V(-1.5, 2, 3).Normalize()
)

// quadMesh builds a mesh made of two triangles spanning four corners.
func quadMesh(a, b, c, d Vector) *Mesh {
	return NewMesh([]*Triangle{
		NewTriangleForPoints(a, b, c),
		NewTriangleForPoints(a, c, d),
	}, nil)
}

func main() {
	dc := NewContext(width, height)
	dc.Cull = CullNone
	dc.ClearColor = HexColor("#1d232b")
	dc.ClearColorBuffer()

	aspect := float64(width) / float64(height)
	matrix := LookAt(eye, center, up).Perspective(fovy, aspect, near, far)

	// An opaque box in front of the planes. It writes depth normally, so
	// it occludes the transparent geometry drawn in the later pass.
	box := NewCube()
	box.Transform(Translate(V(-0.6, -0.3, 0.9)).Scale(V(0.5, 0.5, 0.5)))
	shader := NewPhongShader(matrix, light, eye)
	shader.ObjectColor = HexColor("#e8a33d")
	dc.Shader = shader
	dc.DrawMesh(box)

	// Transparent pass. Depth writes must be off; the existing depth
	// buffer remains as the opaque occluder.
	dc.WriteDepth = false
	if err := dc.BeginTransparency(); err != nil {
		panic(err)
	}

	// Two planes crossing along the vertical center line:
	// plane 1 has z = 0.4x, plane 2 has z = -0.4x (here at x = +/-1.2,
	// y = +/-0.9). They swap depth order across the image, which a single
	// whole-mesh sort cannot resolve.
	plane1 := quadMesh(
		V(-1.2, -0.9, -0.48),
		V(1.2, -0.9, 0.48),
		V(1.2, 0.9, 0.48),
		V(-1.2, 0.9, -0.48),
	)
	plane2 := quadMesh(
		V(-1.2, -0.9, 0.48),
		V(1.2, -0.9, -0.48),
		V(1.2, 0.9, -0.48),
		V(-1.2, 0.9, 0.48),
	)
	// A third, farther plane adds a layer behind both.
	back := quadMesh(
		V(-1.5, -1.1, -1.1),
		V(1.5, -1.1, -1.1),
		V(1.5, 1.1, -1.1),
		V(-1.5, 1.1, -1.1),
	)

	dc.Shader = NewSolidColorShader(matrix, Color{0.2, 0.5, 0.85, 0.45})
	dc.DrawMesh(back)
	dc.Shader = NewSolidColorShader(matrix, Color{0.9, 0.25, 0.2, 0.6})
	dc.DrawMesh(plane1)
	dc.Shader = NewSolidColorShader(matrix, Color{0.25, 0.8, 0.35, 0.6})
	dc.DrawMesh(plane2)

	if err := dc.ResolveTransparency(); err != nil {
		panic(err)
	}

	if err := os.MkdirAll(".cache", 0755); err != nil {
		panic(err)
	}
	if err := SavePNG(".cache/transparency.png", dc.Image()); err != nil {
		panic(err)
	}
}
