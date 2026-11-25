# mini3d

3D software rendering tutorial, without any performance optimization, mainly explains how to write a fixed pipeline software renderer. Although the main code is only 700 lines, it is small but complete.

## Features

* Single File: The source code consists of only one file, mini3d.c, which implements all the functionality. It is easy to read and understand.
* Independent Compilation: There is no third-party library dependency and no complex project directory.
* Model Standard: Uses the standard D3D coordinate model, left-handed system with WORLD/VIEW/PROJECTION matrices.
* Clipping Implementation: Implements simple CVV (Canonical View Volume) clipping.
* Texture Support: Supports textures up to a maximum of 1024 x 1024.
* Depth Buffering: Uses a depth buffer to determine the order of image rendering.
* Perspective Texture Mapping: Implements perspective texture mapping and perspective color filling.
* Edge Calculation: Accurately calculates polygon edge coverage.
* Simplified Implementation: The rendering engine consists of only 700 lines of code, with clear modules and a prominent main structure.
* Detailed Comments: The primary code is well-documented with detailed comments.

## Compile

* mingw: 
		gcc -O3 mini3d.c -o mini3d.exe -lgdi32
* msvc:
		cl -O2 -nologo mini3d.c 
* Compiled version:
[https://github.com/skywind3000/mini3d/releases](https://github.com/skywind3000/mini3d/releases)

## Demonstration

Texture Mapping：RENDER_STATE_TEXTURE
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_1.png)

Color Filling：RENDER_STATE_COLOR
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_0.png)

Wireframe Rendering：RENDER_STATE_WIREFRAME
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_2.png)

Added Lighting and Bicubic Interpolation (screenshot of lighting effect added by a friend to Mini3D)
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_3.png)

## Progressive Radiosity

An offline diffuse indirect-lighting module sits on top of the original fixed pipeline:

* `mini3d.h` exposes the core types (`matrix_t`/`vertex_t`/`device_t`, ...) and functions implemented in `mini3d.c`. `mini3d.c` includes it too, so declarations cannot drift from the definitions; the header is wrapped in `extern "C"` and can be consumed directly from C++.
* `radiosity.h` / `radiosity.c` is a C99 interface implementing hemicube-visibility-driven progressive radiosity. Each patch (`radiosity_patch`) is a world-space triangle whose winding defines the outward normal. `radiosity_create` deep-copies the geometry, places a hemicube at the area centroid of every patch offset by `offset`, and walks the five 90-degree views, reusing the original matrix, trapezoid-split and scanline chain to integrate visibility into a transfer matrix `F`. It then repeatedly shoots the energy of the patch with the largest `Aᵢ₍Uᵣ+U_g+U_b₎` into the others until no energy remains.

Core API:

```c
radiosity_scene *radiosity_create(const radiosity_patch *patches,
                                  size_t count, int resolution, float offset);
void   radiosity_destroy(radiosity_scene *scene);
float  radiosity_factor(const radiosity_scene *scene, size_t i, size_t j);
size_t radiosity_step(radiosity_scene *scene, size_t shots);
int    radiosity_get(const radiosity_scene *scene, size_t i,
                     color_t *B, color_t *U, float *area);
size_t radiosity_last_shooter(const radiosity_scene *scene);
void   radiosity_reset(radiosity_scene *scene);
void   radiosity_draw(const radiosity_scene *scene, device_t *device,
                      float exposure);
```

`radiosity_draw` feeds the current accumulated radiosity `B × exposure` as a constant vertex colour per patch through the original triangle pipeline, clamping only at the final byte colour. It uses the scene's own world coordinates while drawing, restores the caller's `world`/`render_state` and other transform state afterwards, and never clears the caller's existing colour or depth buffers.

### A fully C-constructed offline example

`examples/radiosity_demo.c` needs no window library and no external assets: an emissive top panel sends energy to a floor and a back wall (two receivers) while an opaque plate hangs in between and casts a shadow. It runs one shot and many shots, writing a PPM for each:

```bash
cc -std=c99 -O2 -DMINI3D_HEADLESS -I. \
   examples/radiosity_demo.c radiosity.c mini3d.c -lm -o radiosity_demo
./radiosity_demo       # writes radiosity_1bounce.ppm and radiosity_nbounce.ppm
```

With a single shot only direct light and the first bounce are present, so the room stays dark and the shadow is hard; with many shots the receivers themselves emit, indirect light fills the room, the coloured surfaces bleed into each other and the shadow softens.

The module's tests live in `tests/radiosity_target.c` and run through `bash task_support/run.sh target` (it builds the new module and tests, runs the visibility / transport / integration groups, then the original fixed-pipeline comparison).

## Basic TO-DO:

* Add backface culling
* Implement simple lighting
* Provide more rendering modes
* Implement texture sampling with bicubic interpolation

## Advanced TO-DO:

* Derive and prove all geometric knowledge used in the program
* Optimize vertex computation performance
* Optimize draw_scanline performance
* Load textures from BMP/TGA files
* Load BSP scenes and implement roaming.

## Explanation of principles

- [Wei Yixiao: How do OpenGL and DirectX determine the position of pixels only based on vertices?](https://skywind.me/blog/archives/2594)
- [Wei Yixiao: How does the computer access the graphics card at a low level?](https://skywind.me/blog/archives/1774)

## Related Projects

- [RenderHelp](https://github.com/skywind3000/RenderHelp)：Another implementation of a streamlined software renderer with support for programmable rendering pipelines, for more details see https://skywind.me/blog/archives/2589

## Welcome to donate:

![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/donation.png)

Your donation is the greatest recognition for this tutorial. You are welcome to use Alipay to scan the QR code above to make a donation. The donated funds will be used to improve the tutorial documentation and illustrations, as well as to help me write more interesting tutorials.



## Welcome to follow us:

blog: https://skywind.me/blog

zhihu: https://www.zhihu.com/people/skywind3000 
