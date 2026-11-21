# rpt

[![Latest Version](https://img.shields.io/crates/v/rpt.svg)](https://crates.io/crates/rpt)
[![API Documentation](https://docs.rs/rpt/badge.svg)](https://docs.rs/rpt)

This is a physically based, CPU-only rendering engine written in Rust. It uses path tracing to generate realistic images of 3D scenes.

![Demo renders](https://i.imgur.com/3J6B3cx.jpg)
![Demo video](https://i.imgur.com/UTS3Yo8.gif)

## Features

- Simple declarative API, 100% Safe Rust
- Supports .OBJ, .MTL, and .STL file formats
- Uses unbiased path tracing for physically-based light transport
- Uses a microfacet BSDF model with multiple importance sampling
- Uses kd-trees to accelerate ray intersections
- Supports direct light sampling and emissive materials
- Supports HDRI environment maps
- Supports depth of field
- Supports iterative rendering, variance estimation, and firefly reduction
- Supports physics simulation with numerical integrators and particle systems
- Uses all CPU cores concurrently, scaling linearly up to 96 cores

## Quickstart

First, clone the repository. The library containing path tracing code is located inside `src/`. Example code and scenes are located in `examples/`. To compile and run `examples/basic.rs`, use the command:

```bash
cargo run --example basic
```

To run tests, use:

```bash
cargo test
```

### Library Usage

To use `rpt` as a library, add the following to your `Cargo.toml`:

```toml
[dependencies]
rpt = "0.2"
```

Here's a simple scene that demonstrates the basics of the API.

```rust
use rpt::*;

fn main() {
    let mut scene = Scene::new();

    scene.add(Object::new(sphere())); // default red material
    scene.add(
        Object::new(plane(glm::vec3(0.0, 1.0, 0.0), -1.0))
            .material(Material::diffuse(hex_color(0xAAAAAA))),
    );
    scene.add(Light::Object(
        Object::new(
            sphere()
                .scale(&glm::vec3(2.0, 2.0, 2.0))
                .translate(&glm::vec3(0.0, 12.0, 0.0)),
        )
        .material(Material::light(hex_color(0xFFFFFF), 40.0)),
    ));

    let camera = Camera::look_at(
        glm::vec3(-2.5, 4.0, 6.5),
        glm::vec3(0.0, -0.25, 0.0),
        glm::vec3(0.0, 1.0, 0.0),
        std::f64::consts::FRAC_PI_4,
    );

    Renderer::new(&scene, camera)
        .width(960)
        .height(540)
        .max_bounces(2)
        .num_samples(100)
        .render()
        .save("output.png")
        .unwrap();
}
```

![Example output](https://i.imgur.com/RioQyXf.png)

This code can also be found in `examples/sphere.rs`. Note that the shadow is correctly tinted red due to [global illumination](https://en.wikipedia.org/wiki/Global_illumination). See the detailed [API documentation](https://docs.rs/rpt) for information about all of the features, and feel free to learn from the other examples!

## Finite planar mirrors

Rough materials can only discover specular paths by randomly sampling directions,
which essentially never finds a discrete path that folds through a small set of
finite mirrors. The `rpt::mirrors` module instead adds up to 8 finite, double-sided,
ideal planar mirrors (`MirrorPanel`) and solves every valid point-light connection
deterministically by unfolding the polyline into virtual mirror images. No copy of
the panels is placed in `Scene.objects`; once attached to a `Renderer` with the
`mirror_transport` builder, the panels are real geometry for eye paths, BSDF
continuations and occlusion, without modifying the scene or any material.

```rust
use rpt::*;

let panel = MirrorPanel {
    center: glm::vec3(0.0, 0.0, 1.25),
    edge_u: glm::vec3(0.0, 1.5, 0.0),   // orthogonal, nonzero half-edge
    edge_v: glm::vec3(0.0, 0.0, 1.25),
    reflectance: glm::vec3(0.9, 0.7, 0.45), // each channel in [0, 1]
};
let transport = MirrorTransport::new(vec![panel], 2 /* max reflections, 0..=4 */)?;
let image = Renderer::new(&scene, camera)
    .mirror_transport(&transport)
    .max_bounces(3)
    .render();
```

For every ordinary opaque receiver and each `Light::Point`, the direct path is kept
and every ordered panel sequence up to the remaining bounce budget is enumerated
(consecutive repeats of one panel collapse and are skipped, but `A -> B -> A` is
kept). `MirrorTransport::connect` returns the solved `MirrorPath`s — panel indices,
world-space fold vertices, total polyline length and reflectance product — and the
renderer accumulates those exact paths: point-light intensity divided by the square
of the *total* path length, times the product of RGB reflectances, evaluated with
the receiver's existing BSDF and forward cosine. There is no separate diagnostic
solver and no per-segment attenuation.

Degenerate configurations produce no contribution rather than errors: parallel or
coplanar unfolded lines, zero-length segments, folds outside a finite aperture, and
segments blocked by an ordinary object or by a panel not at that segment's endpoint.
Only endpoint self-intersections are ignored; an object behind the point light never
blocks. The finite-segment predicates use fixed tolerances, documented on the
`MIRROR_EPSILON`, `MIRROR_PARALLEL_EPSILON` and `MIRROR_TIME_EPSILON` constants.
`Renderer::trace_ray_seeded` traces one ray through the same code path and returns
the un-tonemapped linear color.


## References

- [Physically Based Rendering, 3rd ed.](http://www.pbr-book.org/)
- [CS 348B: Image Synthesis Techniques](https://graphics.stanford.edu/courses/cs348b/)
- [Microfacet Models for Refraction through Rough Surfaces](https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf)
- [Scratchapixel: Global Illumination and Path Tracing](https://www.scratchapixel.com/lessons/3d-basic-rendering/global-illumination-path-tracing)
- [SIGGRAPH '10 Notes on Physically-Based Shading](https://renderwonk.com/publications/s2010-shading-course/hoffman/s2010_physically_based_shading_hoffman_a_notes.pdf)
- Inspired by: [fogleman/pt](https://github.com/fogleman/pt), [hunterloftis/pbr](https://github.com/hunterloftis/pbr)

## Samples

![Dragon](https://i.imgur.com/UEWtPDi.png)
![Cornell box](https://i.imgur.com/K7H8rz4.png)
![Pegasus](https://i.imgur.com/sBKAboG.png)
![Lego plane](https://i.imgur.com/BMVCnZ7.png)
![Fractal spheres](https://i.imgur.com/4aO9A2o.png)
![Rustacean](https://i.imgur.com/zZgl7jE.png)
![Wine glass](https://i.imgur.com/8EAmwuq.png)
![Spheres](https://i.imgur.com/jyNpLN5.png)

## Acknowledgements

This project was built by [Eric Zhang](https://github.com/ekzhang) and [Alexander Morozov](https://github.com/scanhex). We'd like to thank [Justin Solomon](https://people.csail.mit.edu/jsolomon/), Yuanming Hu, Lingxiao Li, and Dmitriy Smirnov for teaching an excellent computer graphics class at MIT.

Some of the examples use free 3D models and image assets available on the Internet. Links are provided in comments in the source code, where used.
