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

## Sparse virtual texturing

Alongside the legacy shader path, the repository contains a sparse virtual
texture (SVT) renderer in `virtual_texture.h` / `virtual_texture.cpp`. It keeps
the old `IShader` / `rasterize` / command-line drawing bit-identical; both
paths share the projection, coverage (area culling and backface culling),
barycentric and depth math in `our_gl.cpp`, so there is no second rasterizer.

### Interfaces (namespace `vtex`)

- `TextureDesc` describes one logical texture: power-of-two base width/height
  and a power-of-two fixed page edge. The mip chain halves every axis
  independently until a 1x1 level; tail levels may be smaller than one page.
- `SparseTexturePool` stores only the pages that have actually been uploaded —
  a full logical image is never allocated. `declare_texture`, `upload_page`,
  `replace_page` and `evict_page` validate sizes, page keys, pixel counts and
  finiteness **before** mutating anything and throw `std::invalid_argument` on
  rejection. The 1x1 last level is pinned and cannot be evicted.
- `PageKey{texture, level, page_x, page_y}` identifies a page; `PageData` holds
  its row-major linear-space double RGB pixels over the page's real extent.
- `snapshot()` returns an immutable, owning `ResidencySnapshot`. Frames bind
  one snapshot; later uploads, replacements or evictions never reach existing
  snapshots or already produced frames.
- `VirtualTextureFrame(width, height, snapshot, viewport)` owns every triangle
  submitted to it (`submit` takes clip coordinates, per-corner uv, a texture id
  and an optional discard flag; input must be finite, have positive w and lie in
  the clip frustum). `resolve()` rasterizes the frame and `fragment(x, y)` /
  `missing_pages()` report the results; resolving twice is idempotent.

### Sampling, LOD and paging

Sampling uses repeat wrapping with bilinear coordinates
`(u*W-0.5, v*H-0.5)` and periodic texel modulo; zero-weight taps are never read
and taps may cross pages and period boundaries without clamping. For each LOD
band, all non-zero taps of the ideal level are checked first: if any page is
missing, the whole footprint retreats one coarser level, recomputing its taps
on the original uv, until every tap is resident. The missing-page list reports
**only** the ideal level of each band (already-resident pages are not
reported), sorted by texture, level, page y, page x and deduplicated.

The fragment uv is the perspective-interpolated quotient, and the screen
`du/dx`, `du/dy` are its analytic derivatives (no per-triangle constant, no
affine approximation and no extra fragment invocations). With
`rho = max(length((W0*du/dx, H0*dv/dx)), length((W0*du/dy, H0*dv/dy)))`,
`LOD = clamp(log2(max(1, rho)), 0, last)`. The two bands are `floor(LOD)` and
`min(floor(LOD)+1, last)`, blended by the fractional part; a zero-weight band
is neither sampled nor reported. Each `FragmentResult` exposes the linear RGB,
winning face id, depth, uv, derivatives, floating-point LOD and the level each
band actually used. Only final winning fragments issue requests: occluded,
discarded and background pixels produce no feedback, and an empty frame
returns zero color, face `-1` and an empty list.

### Tests and example

The task cases live in `tests/virtual_texture_cases.cpp` and register the
`paging`, `footprint` and `visibility` groups through
`task_support/check.h` (no `main` of their own). They build and run together
with the unchanged legacy baseline via:

```sh
bash task_support/run.sh target
```

`examples/virtual_texture.cpp` generates two procedural page-backed textures
and two mutually occluding quads, then writes a fallback rendering and its
missing-page list, uploads the requested pages and writes the high-resolution
rendering. It takes exactly one argument, the output directory:

```sh
c++ -std=c++20 -O1 -I. examples/virtual_texture.cpp virtual_texture.cpp our_gl.cpp tgaimage.cpp -o virtual_texture
./virtual_texture <output-directory>
# writes virtual_texture_before.tga, virtual_texture_after.tga and missing_pages.txt
```

Only the C++ standard library and the in-repository sources are used.
