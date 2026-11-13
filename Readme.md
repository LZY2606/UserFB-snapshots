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

## Polarized specular reflection path

Alongside the original scalar renderer (`tinyraytracer.cpp`, unchanged
behavior and default output), the project contains a separately callable
polarization path that tracks only the mirror reflection channel of smooth
dielectric interfaces:

* `geometry.h` — shared `vec3`/`dvec3`, `Material`, `Sphere` and the single
  `ray_sphere_intersect()` used by both renderers. The polarization code does
  not reimplement sphere intersection.
* `polarization.h/.cpp` — double-precision Stokes four-vectors and Mueller
  matrices, one `(I, Q, U, V)` state per RGB channel.
* `polarized_renderer.h/.cpp` — a sphere scene, camera rays, a background
  Stokes state, a reflection budget and a trace entry returning full Stokes.
* `examples/polarized_reflections.cpp` — PPM with three analyzer angles
  side by side.
* `task_tests/target.cpp` — optics, frame and transport checks whose
  expectations are generated from independent complex electric-field math.

### Direction and handedness conventions

* A camera ray direction `D` points *into* the scene. The returned light
  physically propagates toward the camera, so every `PolarState` carries
  `k = -D`. Never treat `D` itself as the propagation direction.
* Each state also carries a unit reference axis `e1 ⟂ k`; the second
  transverse axis is fixed by the right-handed rule
  `e2 = cross(k, e1)`.
* Stokes signs: `Q = |E1|² − |E2|²`, `U = 2 Re(E1·conj(E2))`,
  `V = 2 Im(E1·conj(E2))`. With the reference axes rotated toward `e2` by
  `psi`, `I` and `V` are unchanged and
  `Q' = cos(2ψ)Q + sin(2ψ)U`, `U' = −sin(2ψ)Q + cos(2ψ)U`.
* At an interface the shared s-axis is `s = normalize(cross(N, k_out))`, with
  a deterministic world-axis fallback at normal incidence (no division by
  zero); the p axes are `p_out = cross(k_out, s)` and
  `p_in = cross(k_in, s)`. The incoming light is rotated into the interface
  s/p frame, the reflection Mueller matrix is applied, and the result is
  rotated back into the caller's outgoing frame.
* The deterministic frame used for the background and for top-level rays is
  documented at `PolarScene::reference_axis()`: the world axis least aligned
  with `k` is projected onto the plane perpendicular to `k` and normalized.
  Frames are rebuilt per trace, so repeated traces and camera rolls never
  reuse another ray's basis.
* An analyzer at angle `theta` measures
  `I_out = 0.5·(I + Q·cos(2θ) + U·sin(2θ))`; the full Stokes state is kept up
  to the analyzer.

### Scope of the path: where energy leaves

The path contains **specular reflection only**. At every hit the Fresnel
Mueller matrix is applied, but any energy that is not reflected — the
transmitted/refracted part, and anything an analyzer rejects — leaves this
path immediately. There is no refraction ray, no volume transport and no
diffuse/specular lighting term. Consequently:

* a ray that misses every sphere returns the background Stokes state;
* a ray that hits a surface after the reflection budget (counted in actual
  surface events) is exhausted returns zero;
* a reflection-only ray trapped inside a closed convex object can never reach
  the background and ends in zero once the budget is spent;
* traced intensity can never exceed the background intensity it ultimately
  samples.

The original `Material`, `cast_ray()` and PPM output are untouched; the
polarization code is opt-in through `polarized_renderer.h`.

### Task build

```sh
sh task_support/run.sh target   # polarized example + optics/frame/transport tests
sh task_support/run.sh baseline # original scalar renderer smoke test
```
Just change the code in the editor and rerun the script (use the terminal's history) to see updated images.
