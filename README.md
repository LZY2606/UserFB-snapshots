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

### Performance

FauxGL uses all of your CPU cores. But none of your GPU.

Rendering the Stanford Dragon shown above (871306 triangles) at 1920x1080px takes about 150 milliseconds on my machine. With 4x4=16x supersampling, it takes about 950 milliseconds. This is the time to render a frame and does not include loading the mesh from disk.

### Go Get

    go get -u github.com/fogleman/fauxgl

### Go Run

    cd go/src/github.com/fogleman/fauxgl
    go run examples/hello.go

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

### Object-ID Tracking

For offline assembly previews you often need to know which parts actually
own depth samples on screen after a frame is drawn. `RasterizeInfo.UpdatedPixels`
can't tell you - it only counts writes during drawing, so a part that is
later completely occluded still looks "visible". Object-ID tracking records
the final owner of each pixel along the real rasterization path instead.

```go
context := NewContext(width, height)
context.EnableObjectIDs() // allocates context.ObjectIDBuffer ([]uint32)

// opaque pass: ReadDepth/WriteDepth on, AlphaBlend/Wireframe off
context.AlphaBlend = false

// draw each part with its own non-zero ID
if _, err := context.DrawMeshID(chassis, 1); err != nil {
	panic(err)
}
if _, err := context.DrawMeshID(wheels, 2); err != nil {
	panic(err)
}

// how many pixels does each part own in the final frame?
for id, pixels := range context.VisibleObjectPixels() {
	fmt.Printf("part %d: %d pixels\n", id, pixels)
}
```

`ObjectIDBuffer` has one `uint32` per pixel, matching the depth buffer.
ID 0 means "no tag": regular `DrawMesh` / `DrawTriangle` / `DrawLine` calls
still work while tracking is enabled and claim pixels as 0. `DrawMeshID`
requires an opaque pass (`ReadDepth` and `WriteDepth` true, `AlphaBlend`
and `Wireframe` false) and rejects anything else before any shader runs;
`WriteColor` may be false for a depth-only pass. The ownership of a pixel
is updated in the same critical section as the depth write that wins the
final depth test, after the fragment shader returns (discarded fragments
attribute nothing). `VisibleObjectPixels` returns a caller-owned snapshot
of the current counts, excluding ID 0. Clearing the color buffer keeps the
attribution; clearing the depth buffer resets it to zero.

