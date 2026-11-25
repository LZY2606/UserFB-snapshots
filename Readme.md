# Understandable RayTracing in 256 lines of bare C++

This repository is a support code for my computer graphics lectures. It is not meant to be the ultimate rendering code or even physically realistic. It is meant to be **simple**. This project is distributed under the [DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE](https://en.wikipedia.org/wiki/WTFPL).

**Check [the wiki](https://github.com/ssloy/tinyraytracer/wiki) that accompanies the source code. The second raytracing chapter is available [in the tinykaboom repository](https://github.com/ssloy/tinykaboom/wiki). If you are looking for a software rasterizer, check the [other part of the lectures](https://github.com/ssloy/tinyrenderer/wiki).**

In my lectures I tend to avoid third party libraries as long as it is reasonable, because it forces me to understand what is happening under the hood. So, the raytracing 256 lines of plain C++ give us this result:
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

On open, the editor will compile & run the program as well as open the resulting image in the editor's preview. Just change the code in the editor and rerun the script (use the terminal's history) to get updated results.

## Isosurface support (`renderer.h`, `isosurface.h/.cpp`)

The shared geometry, materials and rendering declarations live in
`renderer.h`; the original demo in `tinyraytracer.cpp` is now a thin PPM
front-end. An `IsoGrid` renders the `F = isovalue` surface of a trilinearly
interpolated scalar field sampled on a regular vertex grid. It is rendered
directly: there is no triangle mesh and no fixed-step ray marching.

### Grid conventions

* Vertex `(i,j,k)` (`0 <= i < nx`, likewise `j,k`; each count is at least 2)
  is stored at `((k*ny) + j)*nx + i` — **x is the fastest varying axis** — and
  sits at world position `origin + (i*sx, j*sy, k*sz)` with strictly positive
  spacing. The scalar array is deep-copied; after construction the object is
  read-only and queries share no mutable cursor.
* Inside each cell the field is the trilinear interpolation of its eight
  corner values, so along a ray it is a cubic in the ray parameter. All real
  roots are solved analytically (up to three per cell); tangential double
  roots are reported as hits and endpoint sign changes are never relied upon.
* The field exists **only inside the grid box**. Rays never intersect
  anything outside it and the box itself is never capped: a field that does
  not cross the isovalue yields no surface, even on the boundary.
* The construction rejects non-finite scalars/origin/isovalue, non-positive or
  non-finite spacing, vertex counts below 2, and data whose length differs
  from `nx*ny*nz`, by throwing `std::invalid_argument`.

### Ray query conventions

`intersect` / `occluded` take an origin `O`, a **non-zero, finite direction
that is not normalized**, and a finite interval `0 <= tmin < tmax`; the ray is
`O + t*D` and only `t in [tmin, tmax)` is considered (the upper bound is
exclusive). A hit returns the parameter `t`, the world position, the owning
cell's 3D index and the material.

* **Normal:** the unit analytic gradient of the trilinear field, pointing
  toward increasing scalar values (the world-space gradient includes the
  grid spacing).
* **Shared cell faces:** a root lying on an internal grid plane is reported
  exactly once. Its identity and normal come from the cell with the smallest
  x-fastest linear index among all cells containing the hit point (at most
  eight candidates, checked in constant time — the grid is never searched).
* **Outer box boundary:** a hit on a box face belongs to the unique inner
  cell; there is no exterior cell.
* Rays whose direction has zero components, rays starting inside the box,
  reverse rays and rays crossing two or three grid planes simultaneously are
  all handled by the same slab/DDA traversal.

### Acceleration and statistics

Cells are grouped into bricks of at most 4 cells per axis (the last brick may
be partial). Each brick stores the min/max of its grid points; bricks whose
closed range does not contain the isovalue are skipped without visiting their
cells. A query first walks bricks by ray events, then runs a cell DDA only
inside the active bricks — the full grid is never scanned and active cells are
never enumerated up front. `QueryStats` reports, from the production
traversal itself, the number of bricks entered and the number of cells that
actually ran polynomial root solving.

### Scene integration

A `Scene` holds the spheres, the lights and one optional, non-owned, read-only
`IsoGrid`; the checkerboard floor and sky background are unchanged. The
scene-aware `scene_intersect` and `cast_ray` compare the isosurface against
the spheres and floor on one nearest-hit chain, and reflection/refraction
recurse with the same `Scene`. Shadow rays are bounded by the light distance,
so a surface behind a light cannot occlude it. The original
`scene_intersect`, `cast_ray` and `ray_sphere_intersect` (still assuming a
unit direction and the `.001` self-intersection offset) keep their previous
behavior, and the default scene contains no grid.

### Tests and example

```sh
bash task_support/run.sh target   # builds the production units and the tests
```

The registered cases in `tests/isosurface_target.cpp` cover analytic cubic
roots (three roots, same-sign double root, tangency, gradients), traversal
identity and empty-brick skipping with printed work counts, and recursive
framebuffer rendering. `examples/isosurface.cpp` builds a grid purely from
in-memory scalars and writes `isosurface_example.ppm`:

```sh
c++ -std=c++17 -O2 -I. examples/isosurface.cpp renderer.cpp isosurface.cpp -o iso_demo
./iso_demo
```
