# FauxGL

3D software rendering in pure Go. No OpenGL, no C extensions, no nothin'.

<br>

![Dragon](http://i.imgur.com/uwehodr.png)

### About

It's like OpenGL, but it's not. It's FauxGL.

It doesn't use your graphics card, only your CPU. So it's slow and unsuitable for realtime rendering. But it's still pretty fast. It works the same way OpenGL works - rasterizing.

### Features

- STL, OBJ, PLY, 3DS file formats
- triangle rasterization
- vertex and fragment "shaders"
- view volume clipping
- face culling
- alpha blending
- textures
- triangle & line meshes
- depth biasing
- wireframe rendering
- built-in shapes (plane, sphere, cube, cylinder, cone)
- anti-aliasing (via supersampling)
- voxel rendering
- parallel processing
- per-pixel order-independent transparency pass

### Performance

FauxGL uses all of your CPU cores. But none of your GPU.

Rendering the Stanford Dragon shown above (871306 triangles) at 1920x1080px takes about 150 milliseconds on my machine. With 4x4=16x supersampling, it takes about 950 milliseconds. This is the time to render a frame and does not include loading the mesh from disk.

### Go Get

    go get -u github.com/fogleman/fauxgl

### Go Run

    cd go/src/github.com/fogleman/fauxgl
    go run examples/hello.go

The transparency example renders crossing translucent planes behind an
opaque box and writes `.cache/transparency.png`:

    go run ./examples/transparency

### Order-Independent Transparency

A single sort of the whole mesh cannot resolve interpenetrating
translucent faces, because different pixels can have a different
front-most layer. The optional transparency pass sorts fragments per
pixel instead:

```go
context.WriteDepth = false          // required before beginning
err := context.BeginTransparency()  // also requires ReadDepth=true,
                                    // WriteColor=true, AlphaBlend=true,
                                    // Wireframe=false
if err != nil {
	panic(err)
}

// Draw transparent geometry through the regular entry points.
context.Shader = shader
context.DrawMesh(mesh)

// Composite the collected fragments per pixel, from far to near.
if err := context.ResolveTransparency(); err != nil {
	panic(err)
}
```

While the pass is active, `DrawTriangle`, `DrawTriangles`, `DrawLine`,
`DrawLines` and `DrawMesh` collect fragments instead of touching the
buffers. The existing opaque depth buffer acts as an occluder and the
depth buffer is never written. Fragments are sorted per pixel by
depth (far to near); equal depths are composited in submission order,
so later primitives cover earlier ones. `DrawMesh` submits its triangles
before its explicit lines.

Colors are composited in premultiplied float RGBA and converted back to
straight-alpha NRGBA8 once, at resolve time. Layers with `Discard` or
zero alpha contribute nothing. `CancelTransparency` discards pending
fragments without changing the image, and all clear-color and clear-depth
entry points cancel an active pass before clearing. `ResolveTransparency`
without an active pass, or `BeginTransparency` with incompatible
settings, returns an error.

Callers must use these entry points serially, and must not change
drawing settings (other than `Shader`) or write the buffers directly
while a pass is active. Parallel drawing inside the library remains
safe, and the submission order is independent of worker scheduling and
`GOMAXPROCS`.

### Go Doc

https://godoc.org/github.com/fogleman/fauxgl

### Complete Example

```go
package main

import (
	. "github.com/fogleman/fauxgl"
	"github.com/nfnt/resize"
)

const (
	scale  = 1    // optional supersampling
	width  = 1920 // output width in pixels
	height = 1080 // output height in pixels
	fovy   = 30   // vertical field of view in degrees
	near   = 1    // near clipping plane
	far    = 10   // far clipping plane
)

var (
	eye    = V(-3, 1, -0.75)               // camera position
	center = V(0, -0.07, 0)                // view center position
	up     = V(0, 1, 0)                    // up vector
	light  = V(-0.75, 1, 0.25).Normalize() // light direction
	color  = HexColor("#468966")           // object color
)

func main() {
	// load a mesh
	mesh, err := LoadOBJ("examples/dragon.obj")
	if err != nil {
		panic(err)
	}

	// fit mesh in a bi-unit cube centered at the origin
	mesh.BiUnitCube()

	// smooth the normals
	mesh.SmoothNormalsThreshold(Radians(30))

	// create a rendering context
	context := NewContext(width*scale, height*scale)
	context.ClearColorBufferWith(HexColor("#FFF8E3"))

	// create transformation matrix and light direction
	aspect := float64(width) / float64(height)
	matrix := LookAt(eye, center, up).Perspective(fovy, aspect, near, far)

	// use builtin phong shader
	shader := NewPhongShader(matrix, light, eye)
	shader.ObjectColor = color
	context.Shader = shader

	// render
	context.DrawMesh(mesh)

	// downsample image for antialiasing
	image := context.Image()
	image = resize.Resize(width, height, image, resize.Bilinear)

	// save image
	SavePNG("out.png", image)
}
```

![Teapot](http://i.imgur.com/DaqbkLR.png)
