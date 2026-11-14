# Understandable RayTracing in 256 lines of bare C++

This repository is a support code for my computer graphics lectures. It is not meant to be the ultimate rendering code or even physically realistic. It is meant to be **simple**. This project is distributed under the [DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE](https://en.wikipedia.org/wiki/WTFPL).

**Check [the wiki](https://github.com/ssloy/tinyraytracer/wiki) that accompanies the source code. The second raytracing chapter is available [in the tinykaboom repository](https://github.com/ssloy/tinykaboom/wiki). If you are looking for a software rasterizer, check the [other part of the lectures](https://github.com/ssloy/tinyrenderer/wiki).**

In my lectures I tend to avoid third party libraries as long as it is reasonable, because it forces to understand what is happening under the hood. So, the raytracing 256 lines of plain C++ give us this result:
![](https://raw.githubusercontent.com/ssloy/tinyraytracer/master/out.jpg)

## compilation
```sh
git clone https://github.com/ssloy/tinyraytracer.git
cd tinyraytracer
mkdir build
cd build
cmake ..
make
```

You can open the project in Gitpod, a free online dev evironment for GitHub:

[![Open in Gitpod](https://gitpod.io/button/open-in-gitpod.svg)](https://gitpod.io/#https://github.com/ssloy/tinyraytracer)

On open, the editor will compile & run the program as well as open the resulting image in the editor's preview.
Just change the code in the editor and rerun the script (use the terminal's history) to see updated images.

## Direct isosurface rendering (`renderer.h`, `isosurface.h/.cpp`)

Common geometry, materials and rendering declarations now live in `renderer.h`
(the original `vec3`, `Material`, `Sphere`, lights, reflection/refraction and
the legacy `scene_intersect`/`cast_ray` entry points). `isosurface.h/.cpp`
adds `IsoGrid`, an isosurface inside a regular scalar grid that is intersected
directly: no triangle extraction and no fixed-step ray marching.

### Data model

`IsoGrid` is constructed from a world-space `origin`, strictly positive
per-axis `spacing`, per-axis vertex counts (each at least 2), the scalar
doubles in **x-fastest** order (`i + nx*(j + ny*k)`), an `isovalue` and a
`Material`. The scalar vector is deep-copied, and every scalar, the origin and
the isovalue must be finite; bad dimensions, data length or spacing throw
`std::invalid_argument` during construction. A constructed grid is read-only
and keeps no mutable per-query cursor, so queries are independent.

Inside the grid box each cell uses trilinear interpolation, so along a ray
`O + t*D` the residual `F - isovalue` is a cubic polynomial in `t`. Its real
roots are solved analytically (depressed-cubic / trigonometric form), which
keeps all three roots of one cell, repeated tangent roots and same-sign
slivers; endpoint sign flips and a single Newton step are never relied on.
There is no surface outside the box and the box itself is never capped.

### Query semantics

Two read-only entry points are offered:

* `nearest(O, D, tmin, tmax, hit, stats=nullptr)` returns the earliest
  `F = isovalue` point with `t` in the half-open interval `[tmin, tmax)`.
  `D` must be non-zero and finite and is **never normalized**; `tmin` must be
  finite and `0 <= tmin < tmax`.
* `occluded(O, D, tmin, tmax, stats=nullptr)` reports whether any such point
  exists and stops at the first one.

The hit carries `t`, the world position, a **unit normal pointing toward
increasing scalar** (the normalized trilinear gradient in world coordinates),
the owning cell's 3D index and the material.

Boundary and identity conventions:

* A root on an internal shared face is reported once. The owning cell is the
  one containing the point with the **smallest x-fastest linear id**; only a
  constant number of face neighbours are inspected to decide this.
* A point on the outer box boundary belongs to the unique interior cell.
* Rays whose direction component is zero, rays starting inside the box,
  simultaneous crossings of two or three grid planes and reverse rays all
  terminate correctly.
* Inputs guarantee a non-zero gradient at every hit and guarantee the ray is
  never coincident with a patch of the level set.

### Acceleration

The grid is partitioned into bricks of at most `4 x 4 x 4` cells (the last
brick may be smaller). Each brick stores the min/max of its grid points at
construction time; a brick whose closed scalar range does not contain the
isovalue is skipped outright. Queries walk bricks by ray events (a slab DDA),
and only inside an active brick do they walk cells with a second DDA and run
the cubic solver. The full grid is never scanned and active cells are never
pre-enumerated per query. `QueryStats` reports the bricks actually visited and
the cells actually polynomial-solved by that production traversal.

### Scene integration

An explicit `Scene` holds the spheres, the lights and at most one optional
read-only `IsoGrid*`; the checkerboard and sky background are unchanged. The
scene-aware `scene_intersect(..., scene, ...)` compares the isosurface with the
spheres and floor on one nearest-hit chain, and `cast_ray(..., scene)` carries
the same `Scene` through reflection and refraction. Shadow rays are bounded by
the finite distance to the light, so geometry behind a light cannot shade it.
The legacy `scene_intersect(orig, dir)` / `cast_ray(orig, dir)` calls and the
default-scene output are preserved; no global variable selects a grid. The
self-intersection lower bound stays `.001`, and the legacy sphere formula
still assumes a unit direction; the non-unit precision requirement applies
only to the standalone grid query entry points.

### Tests and example

* `tests/isosurface_target.cpp` (registered through
  `task_support/check.h`, no `main`) covers analytic three-root, same-sign
  double-root and tangency polynomials with independently derived gradients,
  cross-cell identity, finite shadow intervals, empty-space skipping and
  genuinely recursive pixels. Run everything with:

  ```sh
  bash task_support/run.sh target
  ```

* `examples/isosurface.cpp` builds a grid purely from in-memory scalars (a
  sampled squared-distance field) and writes a binary PPM via the recursive
  scene tracer:

  ```sh
  c++ -std=c++17 -O2 -I. examples/isosurface.cpp isosurface.cpp -o iso_demo
  ./iso_demo   # writes isosurface.ppm
  ```

