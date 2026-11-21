# Software Renderer

<p align="center">
  <img src="https://user-images.githubusercontent.com/8971799/189614240-9449b3fe-372d-4796-8f32-3b13309ca629.png" />
  <img src="https://user-images.githubusercontent.com/8971799/202896058-9cb22703-8c2a-4758-8834-32e43173c337.png" />
</p>

### About

NOTE: I am in the long process of changing the structure of this project, so all commits after 09a6c185963e0be8e02ccbb1eed5118c28ec5136 (September 12, 2022) are not guaranteed to build.  Please revert to that commit to experiment with the features.

SR is a lightweight software rendering library written in C.  It exposes the main algorithms, data structures, and math behind the standard real-time rendering model used in popular graphics api's like OpenGL and DirectX3D.  This project is entirely educational, and is free for anyone to use / adapt.  SR is tested with the [Unity Framework](http://www.throwtheswitch.org/unity) and the examples use SDL2 in order to display its output buffer.

### Features

<p align="right">
  <img align="right" src="https://user-images.githubusercontent.com/8971799/189619522-cc23a50b-4dfd-4391-95c2-3a930e215c3d.png" />
</p>
<p align="right">
  <img align="right" src="https://user-images.githubusercontent.com/8971799/189634455-dd1d293c-930b-4c03-9341-2c6949d07b8e.png" />
</p>

* perspective-correct interpolation
* texture mapping
* spot, point, & directional lighting
* clipping
* depth buffer
* color blending
* programmable shading stages
* custom vertex attributes
* obj loading
* tga image loading

### Design Overview
The core of the library is written in `sr_pipe.c`, where the rendering pipeline is implemented.  Its functionality depends on data organized into a struct called `sr_pipeline`:
```c
/* sr.h */
struct sr_pipeline {
    
    struct sr_framebuffer* fbuf;
    
    void* uniform;        
    vs_f vs;
    fs_f fs;
    float* pts_in;
    int n_pts;
    int n_attr_in;
    int n_attr_out;
    int winding;
};
```
The framebuffer structure `fbuf` serves primarily as the product of an `sr_render` call.  Contained within it is an image buffer that the render function fills.  

The next three fields, `uniform`, `vs`, and `fs` are the programmable parts of the rendering pipeline.  `vs` and `fs` are the vertex shader and fragment shader function pointers, respectivley.  Each one takes a reference to the `uniform`, which stores all data used by the shader that does not vary per vertex while rendering a mesh like textures, materials, bump maps, etc.

The next four fields are data specific to the model.
SR uses one float buffer called `pts` to store the vertex attribute data for rendering.  Each vertex is laid out contiguously in the buffer, where the stride of each vertex is given by `n_attr` (number of attributes).  For example, for a vertex type with a position and color attribute, the pts buffer might look like this:
```c
float pts[2 * 6] = {
    /* x, y, z */       /* r, g, b */
    0, 1, 2,            0.3, 0.1, 0.0,
    1, -1, 0,           0.9, 0.3, 0.4
};
                        
```
This buffer has two points, each with six attributes.  As such, `n_pts = 2` and `n_attr = 6`.  This way most every kind of vertex attribute layout desired by the user can be used.  In the pipeline, `pts_in` holds all the vertices of an object in model space.  And `n_attr_out` specifies how the vertex shader changes the attribute layout so that space can be pre-allocated for it.  The user will be responsible for how the access the memory in the input and output vertex buffers from the vertex shader-- the only garuntee is that they will have the memory available.

Finally, `winding` specefies a winding order of the input vertices: 1 for counter-clock-wise and -1 for clock-wise.

The only assumptions SR will make about the user defined vertex shader is that the clip space coordinates of the vertex (x, y, z, w) appear at the front of the buffer.

<p align="center">
  <img src="https://user-images.githubusercontent.com/8971799/189635638-c78c674f-316b-4337-b5c3-75948d0e468e.png" />
  <img src="https://user-images.githubusercontent.com/8971799/189635641-df877cd6-5575-4f24-8a3d-e2ae5e21b405.png" />
</p>

On top of this core, there exists a fixed function pipeline with a more accessable api supplied in `sr_lib.c`.  The library keeps a global pipeline context and custom uniform type `sr_uniform`.  Functions in the library allow for interaction with these global state variables easily.  

To give control over the model view projection transform, the user can switch between matrix 'modes' using `sr_matrix_mode`.  The modes correspond to either the model, view, or projection matrix.  Then the user can make transformations using `sr_translate`, `sr_scale`, etc.  Or make a view matrix with `sr_look_at`.  This approach is drawn from early implementations of OpenGL.

The library also supplies custom lighting for up to eight lights.  Within the uniform is an array of lights whose fields can be set by the `sr_light` function.

### Build


#### For Linux:
Should work out of the box, just run make and see the rules below for details

#### For Windows (MinGW)
Go into the Makefile and change line 70 (the example rules) and add ```$(MINGW_FLAGS)``` like so:
```Make
    $(CC) $(CFLAGS) $(MINGW_FLAGS) $< $(SR_SRC) -o $@ $(SDL2_FLAGS) -Isrc -lm
```
Make sure that the SDL2 .a files, headers, and binaries are in the ```C:\MinGW``` lib, include, and bin folders respectively, or change the ```MINGW_FLAGS``` variable to reflect where those folders are on your system.

### Makefile rules
to make the examples go into the repository and type 
```
$ make examples
```
then enter the examples directory
```
$ cd examples
```
and then run the example you desire.

To make the tests go into the repository and type
```
$ make tests
```
then enter the tests directory
```
$ cd tests
```
and then run the test you deire.

To clean tests executables run
```
$ make clean-tests
```
And likewise to clean the example executables run 
```
$ make clean-examples
```

### World-space irradiance probes

`probe.h` / `probe.c` add a grid of world-space rule irradiance probes on
top of the existing raster pipeline.  Each probe captures the scene into
the six faces of a cube with `gl_render`, keeps a floating point HDR
radiance and directional distance-moment image per texel, projects the
radiance onto real spherical harmonics and stores Lambert-convolved
irradiance.  Changing the camera never re-bakes a probe, because probes
are independent of any view.

```c
struct probe_config cfg = {
    .origin = {-1.5f, -1.5f, 0.0f},   /* world position of probe (0,0,0) */
    .spacing = 1.0f,                  /* positive distance between probes */
    .nx = 4, .ny = 4, .nz = 8,        /* 2..16 per axis */
    .face_size = 16,                  /* 8..64 per face */
    .near_plane = 0.05f,              /* positive */
    .far_plane = 30.0f,               /* positive, > near */
};

struct probe_scene scene = {
    .positions = verts,               /* 3 floats per vertex */
    .vertex_count = nverts,
    .triangles = tris,                /* indices + linear RGB radiance */
    .triangle_count = ntris,
};

struct probe_set *set = probe_create(&cfg, &scene);  /* copies both */
probe_begin_update(set, environment);                 /* freeze an update */
while (probe_step(set, 16) == 0) { }                  /* 16 faces per call */
```

Triangle radiance is the light leaving a surface along the ray that hit
it, so black surfaces block light and lit surfaces illuminate their
surroundings.  Geometry is two sided.

A probe set is double buffered.  `probe_begin_update` freezes a private
copy of the scene plus the constant environment and starts a new pending
generation, replacing any unfinished update.  `probe_step(set, budget)`
completes at most `budget` whole cube faces and returns 1 on the step that
publishes the new generation; a budget of zero does no work.  Only when
every probe of the update owns all six faces and its coefficients does the
new generation become visible.  Until then `probe_sample` keeps reading
the last complete generation, and returns 0 when none exists.
`probe_abort` drops unfinished work; both it and a replacement update keep
the old generation readable.  The scene and environment are copied, so the
caller may mutate or free its arrays immediately after
`probe_create`/`probe_begin_update`.

`probe_sample(set, p, n, bias, irradiance)` offsets `p` along the unit
normal by `bias`, trilinearly selects the eight surrounding probes
(clamping at the grid edge), and for each probe computes
`r = |p - q|`.  The distance moments `mu` and `m2` are bilinearly read
along `normalize(p - q)`; a tap outside its face is re-projected by its
own direction onto the neighbouring face rather than clamped.  Visibility
is 1 when `r <= mu`, otherwise
`variance / (variance + (r - mu)^2)` with
`variance = max(m2 - mu^2, 1e-4 * spacing^2)`.  Each probe contributes
`v^3 * max(0.05, dot(n, normalize(q - p)))^2` of its SH irradiance
evaluated for `n`; the weights are normalised, the mixed result is
clamped to non-negative, and a zero total weight returns 0 instead of
picking a nearest probe.

`gl_bind_probe_volume(probe_gl_volume(set, bias))` binds the field to the
standard phong shader, which adds `diffuse material * kd * E / pi` in the
float domain while leaving ambient, explicit lights and specular intact.
Binding `NULL` (or never binding) restores the classic output.  Capture
uses its own framebuffer and uniform and does not disturb the caller's
matrices, lights, material or framebuffer bindings.

#### Spherical-harmonic ordering

The nine coefficients per channel use the real SH basis

| index | basis function |
|-------|----------------|
| 0 | `0.282095` |
| 1 | `0.488603 y` |
| 2 | `0.488603 z` |
| 3 | `0.488603 x` |
| 4 | `1.092548 x y` |
| 5 | `1.092548 y z` |
| 6 | `0.315392 (2z^2 - x^2 - y^2)` |
| 7 | `1.092548 x z` |
| 8 | `0.546274 (x^2 - y^2)` |

Radiance is projected with the per-texel solid angle, then convolved for
Lambert irradiance by `pi` (band 0), `2pi/3` (band 1) and `pi/4` (band 2).
Each texel's solid angle is integrated from the four corners of the pixel
on the unit cube, and all six faces sum to `4pi`.  `probe_read_capture`,
`probe_read_coefficients` and `probe_read_distance_moment` expose the
committed capture and coefficients.

#### Running the example and tests

The probe example writes three images into the directory given on the
command line:

```
$ cc -std=c99 -O1 -Iinclude -I. examples/probe.c probe.c \
     clip.c gl.c map.c mat.c obj.c pipe.c rast.c tga.c -lm -o probe_example
$ ./probe_example out
```

`unbound.ppm` is the classic phong output, `partial.ppm` is the same image
with a probe volume bound before any generation exists (no data, so it
matches `unbound.ppm`), and `complete.ppm` receives indirect light from
the glowing panel, including a soft shadow cast by the black wall.

The probe tests live in `task_tests/probe.c` and are registered through
`task_cases`/`task_case_count` in the `capture`, `visibility` and
`publication` groups.  `bash task_support/run.sh target` builds the new
header, implementation, tests and example, runs the example and checks
the produced images, then runs the original off-screen baseline.  The
probe tests cover the cube solid angle, constant-environment SH, radial
distance, occlusion, HDR, seam interpolation, thin-wall visibility,
whole-face vs per-face stepping, source-array lifetime, replacement/abort
and two interleaved sets.

### Credits

Happy Mask Salesman model & texture do not belong to me

Link model & texture created by [ps1guy](https://sketchfab.com/ps1guy) on sketchfab

Thanks to the [Graphics Programming Discord](https://discord.com/invite/6mgNGk7) for helping me along the way!


