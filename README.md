# Software Renderer ![](https://github.com/zauonlok/renderer/workflows/Windows/badge.svg) ![](https://github.com/zauonlok/renderer/workflows/macOS/badge.svg) ![](https://github.com/zauonlok/renderer/workflows/Linux/badge.svg)

<img src="assets/helmet/screenshot2.png" width="410"> <img src="assets/helmet/screenshot3.png" width="410">
<img src="assets/drone/screenshot1.gif" width="410"> <img src="assets/drone/screenshot2.gif" width="410">

This is a shader-based software renderer written from scratch in C89 with
minimal dependencies, available for Windows, macOS, and Linux.

## Features

* Cross platform
* Minimal dependencies
* Shader based
* Homogeneous clipping
* Back-face culling
* Perspective correct interpolation
* Depth testing
* Alpha testing
* Alpha blending
* Cubemapped skybox
* Skeletal animation
* Tangent space normal mapping
* Shadow mapping
* Omnidirectional point-light shadows
* ACES tone mapping
* Blinn–Phong reflection model
* Physically based rendering (PBR)
* Metallic-roughness workflow
* Specular-glossiness workflow
* Image-based lighting (IBL)
* Orbital camera controls
* PBR material inspector

## Download

Pre-built binaries for Windows, macOS, and Linux are available for download
from the [Releases page](https://github.com/zauonlok/renderer/releases).

## Build

To build the software renderer from source, a C89 compiler and development
files for your window system are required.

### Windows

Install [Visual Studio](https://visualstudio.microsoft.com/downloads/)
with C++ support and run `build_win32.bat`.

### macOS

Install Command Line Tools for Xcode with the command below and
run `build_macos.sh`.

```
xcode-select --install
```

### Linux

Install GCC and Xlib with the following commands and run `build_linux.sh`.

#### Ubuntu / Debian

```
sudo apt install gcc libx11-dev
```

#### Fedora / RHEL

```
sudo dnf install gcc libX11-devel
```

#### openSUSE / SUSE

```
sudo zypper install gcc libX11-devel
```

### Bonus

A `CMakeLists.txt` file is provided for generating project files using
[CMake](https://cmake.org/) (see examples below).

#### Visual Studio

```
mkdir build
cd build
cmake -G "Visual Studio 16 2019" ..
start Renderer.sln
```

#### Xcode

```
mkdir build
cd build
cmake -G Xcode ..
open Renderer.xcodeproj
```

#### Makefile

```
mkdir build
cd build
cmake -G "Unix Makefiles" -D CMAKE_BUILD_TYPE=Release ..
make
```

## Usage

### Launch

If the software renderer is launched without arguments, one of the available
scenes will be chosen randomly. To display a specific scene (see below),
additional arguments should be supplied. The command line syntax is:

```
Viewer [test_name [scene_name]]
```

### Controls

* Orbit: left mouse button
* Pan: right mouse button
* Zoom: mouse wheel
* Rotate lighting: <kbd>A</kbd> <kbd>D</kbd> <kbd>S</kbd> <kbd>W</kbd>
* Reset everything: <kbd>Space</kbd>

### Inspector

For PBR scenes, a material inspector that is very similar to the layers view
of [Marmoset Viewer](https://marmoset.co/viewer/) is provided. Double click
to bring it up.

### Point-light shadows

Besides the default single directional shadow map, a scene can be given an
optional omnidirectional point light through
`renderer/core/point_shadow.h`. `scene_set_point_light(scene, position,
near, far, size, bias)` creates six same-sized square depth framebuffers,
one per cubemap face; passing `size == 0` disables the mode and releases the
resources. A positive `size` requires `near > 0`, `far > near`, `bias >= 0`
and finite floating point arguments. Invalid configurations return `0` and
leave the previous configuration untouched.

```c
typedef struct {
    vec3_t position;
    float near;
    float far;
    float bias;
    framebuffer_t *faces[6];
} point_shadow_t;

int scene_set_point_light(scene_t *scene, vec3_t position, float near,
                          float far, int size, float bias);
float point_shadow_visibility(const point_shadow_t *point_shadow,
                              vec3_t world_position);
```

When the point light is enabled, every opaque model is rendered six times
from the light position with a 90 degree perspective frustum, clearing each
face depth to `1`. The existing model transforms, skeletal/attached
animation, double-sided rendering and alpha cutoff are all reused, and the
directional light and its 2D shadow are suppressed for that draw. The
`near` and `far` values are positive distances along each face's viewing
axis (not a spherical range); the stored values are the rasterizer's
ordinary `[0, 1]` perspective depths, never quantized into a color buffer.

The face order and axes follow the cubemap convention: `px, nx, py, ny, pz,
nz`. With `s = 2u - 1` and `t = 2v - 1`, the inverse direction of each face
is `(1,-t,-s)`, `(-1,-t,s)`, `(s,1,t)`, `(s,-1,-t)`, `(s,-t,1)` and
`(-s,-t,-1)` respectively, and pixel center `(x, y)` maps to
`u = (x + 0.5) / size`, `v = (y + 0.5) / size`. The capture matrices and
the sampling mapping use this same convention, so no test asset needs to be
rotated to compensate.

`point_shadow_visibility` selects a face from the receiver's
`world_position - position` vector (a receiver on the light itself, or
whose major-axis distance falls outside `[near, far]`, is fully visible).
It takes nine equal-weight taps offset by `{-1, 0, 1} / size` around the
receiver's `u/v`. Out-of-range taps are turned back into a direction on the
original face and re-projected onto the neighboring face; texel indices are
floored and clamped only once the final face is known. A stored depth
`d < 1` is converted back to an axis distance
`near * far / (far - d * (far - near))`, then multiplied by the length of
the actually-read texel center's inverse direction to obtain a radial
occluder distance, which is compared against the receiver's Euclidean
distance plus the world-space `bias`. The fraction of the nine taps that
stay visible is the returned visibility value; a clear value of `1` means
no occlusion.

The Blinn, PBR metalness and PBR specular materials all share this one
visibility term. The point light direction is the per-fragment unit vector
toward the light and the direct intensity is `punctual_intensity / r^2`
(zero at `r = 0`). Ambient/IBL lighting, emissive surfaces and the PBR
layer-view preview are not multiplied by the point-light visibility.

## Screenshots

| Scene                                                                                   | Command                   |
| --------------------------------------------------------------------------------------- | ------------------------- |
| [<img src="assets/azura/screenshot.png" width="600">](assets/azura/README.md)           | `Viewer blinn azura`      |
| [<img src="assets/centaur/screenshot.png" width="600">](assets/centaur/README.md)       | `Viewer blinn centaur`    |
| [<img src="assets/craftsman/screenshot2.gif" width="600">](assets/craftsman/README.md)  | `Viewer blinn craftsman`  |
| [<img src="assets/elfgirl/screenshot2.png" width="600">](assets/elfgirl/README.md)      | `Viewer blinn elfgirl`    |
| [<img src="assets/kgirl/screenshot.gif" width="600">](assets/kgirl/README.md)           | `Viewer blinn kgirl`      |
| [<img src="assets/lighthouse/screenshot.png" width="600">](assets/lighthouse/README.md) | `Viewer blinn lighthouse` |
| [<img src="assets/mccree/screenshot.png" width="600">](assets/mccree/README.md)         | `Viewer blinn mccree`     |
| [<img src="assets/nier2b/screenshot.png" width="600">](assets/nier2b/README.md)         | `Viewer blinn nier2b`     |
| [<img src="assets/phoenix/screenshot.gif" width="600">](assets/phoenix/README.md)       | `Viewer blinn phoenix`    |
| [<img src="assets/vivi/screenshot.png" width="600">](assets/vivi/README.md)             | `Viewer blinn vivi`       |
| [<img src="assets/whip/screenshot.gif" width="600">](assets/whip/README.md)             | `Viewer blinn whip`       |
| [<img src="assets/witch/screenshot.png" width="600">](assets/witch/README.md)           | `Viewer blinn witch`      |
| [<img src="assets/assassin/screenshot.gif" width="600">](assets/assassin/README.md)     | `Viewer pbr assassin`     |
| [<img src="assets/buster/screenshot1.gif" width="600">](assets/buster/README.md)        | `Viewer pbr buster`       |
| [<img src="assets/crab/screenshot.gif" width="600">](assets/crab/README.md)             | `Viewer pbr crab`         |
| [<img src="assets/dieselpunk/screenshot.png" width="600">](assets/dieselpunk/README.md) | `Viewer pbr dieselpunk`   |
| [<img src="assets/drone/screenshot1.gif" width="600">](assets/drone/README.md)          | `Viewer pbr drone`        |
| [<img src="assets/helmet/screenshot1.png" width="600">](assets/helmet/README.md)        | `Viewer pbr helmet`       |
| [<img src="assets/horse/screenshot.gif" width="600">](assets/horse/README.md)           | `Viewer pbr horse`        |
| [<img src="assets/junkrat/screenshot.gif" width="600">](assets/junkrat/README.md)       | `Viewer pbr junkrat`      |
| [<img src="assets/ornitier/screenshot.png" width="600">](assets/ornitier/README.md)     | `Viewer pbr ornitier`     |
| [<img src="assets/ponycar/screenshot2.png" width="600">](assets/ponycar/README.md)      | `Viewer pbr ponycar`      |
| [<img src="assets/robot/screenshot.png" width="600">](assets/robot/README.md)           | `Viewer pbr robot`        |
| [<img src="assets/spruit/screenshot1.png" width="600">](assets/spruit/README.md)        | `Viewer pbr sphere`       |
| [<img src="assets/workshop/screenshot2.png" width="600">](assets/workshop/README.md)    | `Viewer pbr spheres`      |

## References

### Rendering pipeline

* [Tiny Renderer](https://github.com/ssloy/tinyrenderer)
* [OpenGL ES 2.0 Specification](https://www.khronos.org/registry/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf)

### Physically based rendering

* [glTF 2.0 Sample Viewer](https://github.com/KhronosGroup/glTF-Sample-Viewer)
* [SIGGRAPH 2013 Shading Course](https://blog.selfshadow.com/publications/s2013-shading-course/)
* [Physically Based Rendering in Filament](https://google.github.io/filament/Filament.html)

### File formats

* [Truevision TGA](http://www.dca.fee.unicamp.br/~martino/disciplinas/ea978/tgaffs.pdf)
* [Radiance HDR](https://www.graphics.cornell.edu/~bjw/rgbe.html)
* [Wavefront OBJ](http://www.cs.utah.edu/~boulos/cs3505/obj_spec.pdf)
* [Khronos glTF](https://github.com/KhronosGroup/glTF/blob/master/specification/2.0/README.md)

### Miscellaneous

* [Homogeneous clipping](http://fabiensanglard.net/polygon_codec/)
* [Barycentric coordinates](http://blackpawn.com/texts/pointinpoly/)
* [Perspective correct interpolation](https://www.comp.nus.edu.sg/~lowkl/publications/lowk_persp_interp_techrep.pdf)
* [Skeletal animation](https://people.rennes.inria.fr/Ludovic.Hoyet/teaching/IMO/05_IMO2016_Skinning.pdf)
* [Tangent space normal mapping](http://www.terathon.com/code/tangent.html)
* [Ambient occlusion](http://mentalraytips.blogspot.com/2008/11/joy-of-little-ambience.html)
* [ACES tone mapping](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/)
* [Orbital camera controls](https://github.com/mrdoob/three.js/blob/master/examples/js/controls/OrbitControls.js)
* [Xlib programming guide](https://web.archive.org/web/20071018025425/http://users.actcom.co.il/~choo/lupg/tutorials/xlib-programming/xlib-programming.html)
* [Type-safe dynamic array](https://github.com/nothings/stb/blob/master/stretchy_buffer.h)

## License

[MIT License](LICENSE)
