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

The original scalar path still provides `Material`, `cast_ray`, and its normal
rendering `main`. Shared vector/sphere/intersection definitions were extracted
to `geometry.h`; both the old path and the polarization path call the same
`ray_sphere_intersect`.

## Polarized reflection path

`polarization.h/.cpp` contains `double` Stokes vectors, Mueller matrices,
complex Fresnel amplitudes, same-direction basis rebasing, reflection Mueller
matrices, and the ideal linear analyzer. RGB channels are independent:

```cpp
pol::StokesRGB state; // state[0..2] are R, G, B, each {I,Q,U,V}
```

The Stokes/Jones convention is

```text
Q = |E1|^2 - |E2|^2
U = 2 Re(E1 conj(E2))
V = 2 Im(E1 conj(E2))
```

Every `pol::PolarizationState` carries the propagation direction `k` and a
unit reference axis `e1` perpendicular to `k`. The second axis is

```text
e2 = cross(k, e1)
```

which makes `(e1, e2, k)` right handed because `cross(e1, e2) = k`. If `e1`
is turned towards `e2` by `psi`, then

```text
Q' =  cos(2 psi) Q + sin(2 psi) U
U' = -sin(2 psi) Q + cos(2 psi) U
I' = I
V' = V
```

`pol::change_basis` applies this same physical coordinate conversion to an
arbitrary new perpendicular reference axis.

The deterministic world basis used for background and internal return frames
is the projection of world `+Y` onto the plane perpendicular to `k`; when
`abs(dot(k, +Y)) > 0.9`, world `+X` is projected instead. The helper is
`pol::world_reference_axis(k)`. The same rule is the normal-incidence `s`
fallback, so `cross(N, k_out) = 0` never causes division by zero.

For the renderer, a camera ray direction `D` points into the scene, while the
returned light propagates along `k = -D`. At a hit the renderer uses

```text
s     = normalize(cross(N, k_out))
p_in  = cross(k_in,  s)
p_out = cross(k_out, s)
```

and rebases the recursive Stokes state to `(s, p_in)` before applying the
reflection Mueller matrix. The output state uses `(s, p_out)`; it is then
rebased to the caller's reference basis. Each `PolarizedSphere` stores
`n_out` and `n_in`; the sign of `dot(N, k_out)` selects whether the current
reflection side is outside or inside. Reflection stays in that same medium.

Fresnel amplitudes use the requested positive real indices and

```text
ci = |dot(N, k_out)|
ct = sqrt(1 - (n1/n2)^2 (1-ci^2))
rs = (n1 ci - n2 ct)/(n1 ci + n2 ct)
rp = (n2 ci - n1 ct)/(n2 ci + n1 ct)
```

with the complex square-root branch having non-negative imaginary part.
`diag(rs, rp)` preserves the total-internal-reflection phase difference; the
Mueller `U/V` block couples those components instead of discarding phase.

An ideal analyzer at angle `theta` from the current `e1` measures

```text
I_out = 1/2 (I + Q cos(2 theta) + U sin(2 theta))
```

and the complete Stokes state remains available before this measurement.

### Scope of the energy transport

This is an additional reflection-only path, not a replacement for the original
renderer. It follows only the smooth dielectric interface specular reflection
channel between spheres. The Fresnel Mueller matrix retains reflected power
(`|rs|^2`, `|rp|^2`); the non-reflected transmitted/refracted energy leaves
this path. There is no transmission ray, Beer/volume absorption, diffuse
lighting, shadow, checkerboard plane, fluorescence, or scattering. A miss
returns the supplied background Stokes state. A sphere hit after the maximum
number of actual surface reflection events has been used returns zero Stokes.
With `max_reflections = N`, up to `N` processed surface events are allowed;
the recursive query after the `N`-th event returns zero if it hits another
surface.

### Polarization example and tests

```sh
sh task_support/run.sh target    # optics/frames/transport tests + example
sh task_support/run.sh baseline  # original scalar TinyRayTracer path
```

The target script writes a three-panel PPM (analyzer angles 0, 60, and 120
degrees side by side) to the path passed as the example's first argument:

```sh
c++ -std=c++17 -I. examples/polarized_reflections.cpp \
    task_support/library.cpp polarization.cpp polarized_renderer.cpp \
    -o polarized_reflections
./polarized_reflections demo.ppm
```

`task_tests/target.cpp` derives expected Stokes values from its own complex
Jones fields, including Brewster angle, total-internal-reflection `U/V` phase,
basis rotations, normal incidence, and two non-coplanar recursive reflections.

You can open the project in Gitpod, a free online dev evironment for GitHub:

[![Open in Gitpod](https://gitpod.io/button/open-in-gitpod.svg)](https://gitpod.io/#https://github.com/ssloy/tinyraytracer)

On open, the editor will compile & run the program as well as open the resulting image in the editor's preview.
Just change the code in the editor and rerun the script (use the terminal's history) to see updated images.
