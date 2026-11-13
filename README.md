# Software rendering in 500 lines of bare C++

The code itself is of little interest. Check the course notes:
1. [Introduction](https://haqr.eu/tinyrenderer/)
2. [Bresenham’s line drawing algorithm](https://haqr.eu/tinyrenderer/bresenham/)
3. [Triangle rasterization](https://haqr.eu/tinyrenderer/rasterization/)
4. [Primer on barycentric coordinates](https://haqr.eu/tinyrenderer/barycentric/)
5. [Hidden faces removal](https://haqr.eu/tinyrenderer/z-buffer/)
6. [A crude (but simple) approach to camera handling](https://haqr.eu/tinyrenderer/camera-naive/)
7. [Better camera handling](https://haqr.eu/tinyrenderer/camera/)
8. [Shading](https://haqr.eu/tinyrenderer/shading/)
9. [More data!](https://haqr.eu/tinyrenderer/textures/)
10. [Tangent space normal mapping](https://haqr.eu/tinyrenderer/tangent/)
11. [Shadow mapping](https://haqr.eu/tinyrenderer/shadow/)
12. [Indirect lighting](https://haqr.eu/tinyrenderer/ssao/)
13. [Bonus: toon shading](https://haqr.eu/tinyrenderer/toon/)
14. [Afterword](https://haqr.eu/tinyrenderer/afterword/)

In this series of articles, I aim to demonstrate how OpenGL, Vulkan, Metal, and DirectX work by writing a simplified clone from scratch.
Surprisingly, many people struggle with the initial hurdle of learning a 3D graphics API.
To help with this, I have prepared a short series of lectures, after which my students are able to produce quite capable renderers.

The task is as follows: using no third-party libraries (especially graphics-related ones), we will generate an image like this:

![](https://haqr.eu/tinyrenderer/home/africanhead.png)

_Warning: This is a training material that loosely follows the structure of modern 3D graphics libraries.
It is a **software renderer**.
**I do not intend to show how to write GPU applications — I want to show how they work.**
I firmly believe that understanding this is essential for writing efficient applications using 3D libraries._

## The starting point

The final code consists of about 500 lines.
My students typically require 10 to 20 hours of programming to start producing such renderers.
The input is a 3D model composed of a triangulated mesh and textures.
The output is a rendering.
There is no graphical interface, the program simply generates an image.

To minimize external dependencies, I provide my students with a single class for handling [TGA](http://en.wikipedia.org/wiki/Truevision_TGA) files —
one of the simplest formats supporting RGB, RGBA, and grayscale images.
This serves as our foundation for image manipulation.
At the beginning, the only available functionality (besides loading and saving images) is the ability to set the color of a single pixel.

There are no built-in functions for drawing line segments or triangles — we will implement all of this manually.
While I provide my own source code, written alongside my students, I do not recommend using it directly, as doing the work yourself is essential to understanding the concepts.
The complete code is available on [github](https://github.com/ssloy/tinyrenderer), and you can find the initial source code I provide to my students [here](https://github.com/ssloy/tinyrenderer/tree/706b2dfecff65daeb93de568ee2c2bd87f277860).
Behold, here is the starting point:

```cpp
#include "tgaimage.h"

constexpr TGAColor white   = {255, 255, 255, 255}; // attention, BGRA order
constexpr TGAColor green   = {  0, 255,   0, 255};
constexpr TGAColor red     = {  0,   0, 255, 255};
constexpr TGAColor blue    = {255, 128,  64, 255};
constexpr TGAColor yellow  = {  0, 200, 255, 255};

int main(int argc, char** argv) {
    constexpr int width  = 64; 
    constexpr int height = 64; 
    TGAImage framebuffer(width, height, TGAImage::RGB);

    int ax =  7, ay =  3;  
    int bx = 12, by = 37; 
    int cx = 62, cy = 53; 

    framebuffer.set(ax, ay, white);
    framebuffer.set(bx, by, white);
    framebuffer.set(cx, cy, white);

    framebuffer.write_tga_file("framebuffer.tga");
    return 0;
}
```

It produces the 64x64 image `framebuffer.tga`, here I scaled it for better readability:

![](https://haqr.eu/tinyrenderer/bresenham/bresenham0.png)


## Teaser: few examples made with the renderer

![](https://haqr.eu/tinyrenderer/home/demon.png)

![](https://haqr.eu/tinyrenderer/home/diablo-glow.png)

![](https://haqr.eu/tinyrenderer/home/boggie.png)

![](https://haqr.eu/tinyrenderer/home/diablo-ssao.png)

## Compilation

```sh
git clone https://github.com/ssloy/tinyrenderer.git &&
cd tinyrenderer &&
cmake -Bbuild &&
cmake --build build -j &&
build/tinyrenderer obj/diablo3_pose/diablo3_pose.obj obj/floor.obj
```
The rendered image is saved to `framebuffer.tga`.

## Hard shadows with closed shadow volumes

The original `rasterize()` path and the default command-line output are kept
unchanged. A second, reusable path renders hard directional-light shadows with
closed shadow volumes (the z-fail algorithm), avoiding shadow-map resolution
and bias tuning. It lives in `shadow_volume.{h,cpp}` and the `ShadowRenderer`
class in `our_gl.{h,cpp}`; the Phong math is shared through `phong_shader.h`.
See `examples/shadow_volume.cpp` for a complete, self-contained program and
`task_tests/target.cpp` for the semantics.

### Building a volume

```cpp
ShadowVolume vol(positions,          // finite world-space positions
                 face_indices,       // triples of *position* indices
                 light_dir,          // non-zero, points TOWARD the light
                 extrusion);         // positive finite distance D
```

* Adjacency is built from position indices only, so UV/normal seams (one
  position referenced by several face corners) are not seen as cracks, while
  positions that merely share coordinates but have different indices are never
  welded.
* A face is lit when `dot(outward_normal, light_dir) > 0`; an exactly zero dot
  product faces away. Lit faces form the near cap, their reversed extrusions
  form the far cap, and the silhouette edges (a lit face next to a dark one)
  are joined by outward-oriented side walls. Coplanar triangulation edges are
  interior and never produce walls.
* Extrusion runs along `-normalize(light_dir) * D`. The caller chooses `D` and
  must guarantee that every receiver-to-occluder intersection along
  `normalize(light_dir)` is closer than `D`, so the far cap lies behind every
  receiver. Infinite-far capping is intentionally not supported.
* Out-of-range indices, zero-area faces, open edges, same-winding shared edges,
  or an edge incident to more than two faces throw `ShadowVolumeError` during
  construction; no half-built drawable object remains. The generated vertices,
  triangles and silhouette edges are exposed read-only for closure checks.

### Clip convention and supported range

The new path takes a caller-supplied world-to-clip matrix and viewport; both
orthographic and perspective projections work. A clip point `(x,y,z,w)` is
inside the visible volume when `w > 0` and `-w <= x,y,z <= w`: the near plane
is `z = +w`, the far plane `z = -w`, and a larger `z/w` is closer. Depth is
`z/w`, interpolated linearly in screen space.

Volumes and receivers may cross the near, side and eye planes. The supported
range requires **every shadow-volume vertex to satisfy `z >= -w`**, so the far
cap is never clipped and no far-plane cap reconstruction is performed — a more
distant arbitrary cap must not be substituted for a genuinely closed volume.
Receivers are clipped against all six planes.

### Rasterization rules

All new sampling uses half-pixel centers with y pointing up. Projected
triangles are oriented to positive area; downward edges and leftward
horizontal edges own their boundary, and clipping-fan inner edges cover each
sample exactly once. After clipping, UV/normal interpolation still uses the
perspective-correct weights of the original three vertices. The legacy rule
that drops triangles under one pixel of area is not applied on this path.

### Drawing order

```cpp
ShadowRenderer renderer(width, height, viewport);
renderer.clear();                       // depth, valid, lighting, signed count

renderer.draw_surface(nfaces, shader);  // opaque receivers and occluders;
                                        // discard writes no depth or material
renderer.draw_volume(volume, clip);     // two-sided, frozen-depth z-fail;
                                        // no color or depth writes
renderer.compose(framebuffer, background); // clamp + quantize only now
```

The stencil is a signed integer (never saturated to 8 bits and never treated as
parity/XOR). Only fragments failing the frozen depth test are counted —
back faces add one, front faces subtract one — including fragments at equal
depth, so a lit near cap resting on the visible surface cancels its far cap
and the occluder never shadows itself. Background pixels with no visible
surface are not counted, and a non-zero total across any number of overlapping
volumes means "in shadow". Because it is z-fail, the count stays correct when
the camera is inside a volume or the near plane cuts a cap; it never assumes a
zero count at the camera (z-pass).

Lighting keeps unquantized `ambient` and `diffuse+specular` on the visible
surface and composes `ambient + visibility*(diffuse+specular)` at the end, so
an already byte-quantized image is never darkened after the fact. A new frame
must call `clear()` (or `clear_stencil()` between lights); re-running a shadow
pass without clearing would otherwise accumulate. `compose()` rejects a
framebuffer whose dimensions do not match the renderer.
