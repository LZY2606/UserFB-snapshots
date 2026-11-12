# pt: a golang path tracer

[![Build Status](https://travis-ci.org/fogleman/pt.png?branch=master)](https://travis-ci.org/fogleman/pt) [![GoDoc](https://godoc.org/github.com/fogleman/pt/pt?status.svg)](https://godoc.org/github.com/fogleman/pt/pt)

This is a CPU-only, unidirectional [path tracing](http://en.wikipedia.org/wiki/Path_tracing) engine written in Go. It has lots of features and a simple API.

![Examples](http://i.imgur.com/BTaDLC3.png)

---

### Features

* Supports OBJ and STL
* Supports textures, bump maps and normal maps
* Supports raymarching of signed distance fields
* Supports volume rendering from image slices
* Supports various material properties
* Supports configurable depth of field
* Supports iterative rendering
* Supports adaptive sampling and firefly reduction
* Uses k-d trees to accelerate ray intersection tests
* Uses all CPU cores in parallel
* 100% pure Go with no dependencies besides the standard library

### Installation

    go get -u github.com/fogleman/pt/pt

### Examples

The are [lots of examples](https://github.com/fogleman/pt/tree/master/examples) to learn from! To try them, just run, e.g.

    cd go/src/github.com/fogleman/pt
    go run examples/gopher.go

### Optional Embree Acceleration

You can optionally utilize Intel's Embree ray tracing kernels to accelerate triangle mesh intersections. First, install embree on your system: http://embree.github.io/ Then get the `go-embree` wrapper and checkout the `embree` branch of `pt`.

    git checkout embree
    go get -u github.com/fogleman/go-embree

### Hello World

The following code demonstrates the basics of the API.

```go
package main

import . "github.com/fogleman/pt/pt"

func main() {
	// create a scene
	scene := Scene{}

	// create a material
	material := DiffuseMaterial(White)

	// add the floor (a plane)
	plane := NewPlane(V(0, 0, 0), V(0, 0, 1), material)
	scene.Add(plane)

	// add the ball (a sphere)
	sphere := NewSphere(V(0, 0, 1), 1, material)
	scene.Add(sphere)

	// add a spherical light source
	light := NewSphere(V(0, 0, 5), 1, LightMaterial(White, 8))
	scene.Add(light)

	// position the camera
	camera := LookAt(V(3, 3, 3), V(0, 0, 0.5), V(0, 0, 1), 50)

	// render the scene with progressive refinement
	sampler := NewSampler(4, 4)
	renderer := NewRenderer(&scene, &camera, sampler, 960, 540)
	renderer.AdaptiveSamples = 128
	renderer.IterativeRender("out%03d.png", 1000)
}
```

![Hello World](http://i.imgur.com/FyAmeMx.png)

### Adaptive Sampling

There are several sampling options that can reduce the amount of time needed to converge to an acceptable noise level. Both images below were rendered in 60 seconds. On the left, no advanced features were enabled. On the right, the features listed below were used.

![Gopher](http://i.imgur.com/nidccRU.png)

Here are some of the options that are utilized to achieve this improvement:

- **stratified sampling** - on first intersection, spawn NxN rays in a stratified pattern to ensure well-distributed coverage
- **sample all lights** - at each intersection, sample all lights for direct lighting instead of one random light
- **forced specular reflections** - at each intersection, force both a diffuse and specular bounce and weigh them appropriately
- **adaptive sampling** - spend more time sampling pixels with high variance
- **firefly reduction** - very heavily sample pixels with very high variance

Combining these features results in cleaner, faster renders. The specific parameters that should be used depend greatly on the scene being rendered.

### Environment Map Importance Sampling

Scenes lit by a small bright region of the sky (e.g. the sun in an HDRI) converge slowly when diffuse bounces have to randomly hit that region. An `EnvironmentMap` provides importance sampling of an equirectangular sky radiance map, and `DefaultSampler.EnvironmentMIS` combines environment direction sampling with the usual cosine-weighted bounces using multiple importance sampling (power heuristic).

```go
// pixels: linear RGB radiance, row-major, first row at the north pole;
// each texel is treated as constant (no interpolation)
env, err := pt.NewEnvironmentMap(width, height, pixels)
if err != nil {
    // non-positive/overflowing dimensions, wrong pixel count,
    // or negative/non-finite components
}

scene := pt.Scene{}
scene.Environment = env        // nil falls back to Scene.Texture / Scene.Color
scene.TextureAngle = angle     // optional rotation about the +Y axis (radians)

sampler := pt.NewSampler(4, 4)
sampler.EnvironmentMIS = true  // off by default; requires DirectLighting=true
```

For a unit direction `d`, texel coordinates are

```
u = fract((atan2(d.Z, d.X) + angle + pi) / (2*pi))
v = acos(clamp(d.Y, -1, 1)) / pi
```

so `+Y` is the north pole (`v = 0`, first pixel row). The map provides:

- `Evaluate(direction, angle) Color` - radiance of the texel containing the direction
- `PDF(direction, angle) float64` - solid-angle probability density of the direction
- `Sample(rnd, angle) (direction, radiance, pdf)` - draw a direction proportional to texel luminance (`.2126R+.7152G+.0722B`) times true spherical area, uniform in solid angle within the texel

All PDFs are per unit solid angle, and `PDF` always agrees with the value returned by `Sample`. An all-black map falls back to a uniform spherical distribution. A single map is immutable after construction and can be shared by concurrent scenes using different rotation angles.

When `EnvironmentMIS` is enabled (and `DirectLighting` is true), every executed non-transparent diffuse branch additionally samples one environment direction with a real visibility test, and the two strategies (environment direction and cosine bounce) are combined with the power heuristic. Finite lights still use the existing direct-lighting path. With `EnvironmentMIS` off or no `Environment` set, rendering is bit-identical to before.

See `examples/environment/main.go` for a complete example that renders to `.cache/environment.ppm`.

### Links

Here are some resources that I have found useful.

* [WebGL Path Tracing - Evan Wallace](http://madebyevan.com/webgl-path-tracing/)
* [Global Illumination in a Nutshell](http://www.thepolygoners.com/tutorials/GIIntro/GIIntro.htm)
* [Simple Path Tracing - Iñigo Quilez](http://www.iquilezles.org/www/articles/simplepathtracing/simplepathtracing.htm)
* [Realistic Raytracing - Zack Waters](http://web.cs.wpi.edu/~emmanuel/courses/cs563/write_ups/zackw/realistic_raytracing.html)
* [Reflections and Refractions in Ray Tracing - Bram de Greve](http://graphics.stanford.edu/courses/cs148-10-summer/docs/2006--degreve--reflection_refraction.pdf)
* [Better Sampling - Rory Driscoll](http://www.rorydriscoll.com/2009/01/07/better-sampling/)
* [Ray Tracing for Global Illumination - Nelson Max at UC Davis](https://www.youtube.com/playlist?list=PLslgisHe5tBPckSYyKoU3jEA4bqiFmNBJ)
* [Physically Based Rendering - Matt Pharr, Greg Humphreys](http://www.amazon.com/Physically-Based-Rendering-Second-Edition/dp/0123750792)

### Samples

![Go](http://i.imgur.com/LMNUoaM.jpg)

![Dragon](https://www.michaelfogleman.com/static/gallery/out1000c.png)

![Cornell](https://www.michaelfogleman.com/static/gallery/853.png)

![Lucy](https://www.michaelfogleman.com/static/gallery/756b.png)

![SDF](https://www.michaelfogleman.com/static/gallery/470d.png)

![Spheres](https://www.michaelfogleman.com/static/gallery/dof.png)

![Suzanne](http://i.imgur.com/iw32US1.png)

![Molecule](https://www.michaelfogleman.com/static/gallery/600d.png)
