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

`virtual_texture.h/.cpp` add a second rendering path that streams texture data
page by page instead of loading a full image. The legacy `IShader`,
`rasterize` and command line draw are untouched; both paths share the
coverage, barycentric and depth helpers in `our_gl.cpp`.

A texture is described by a complete logical mip chain (`TextureDesc`): a
power-of-two base size halved on each axis until 1x1, plus a fixed
power-of-two page edge. The pool (`VirtualTexturePool`) only stores the pages
that have actually been uploaded; full logical images are never allocated. A
page is identified by `PageKey { texture_id, level, page_x, page_y }` and its
content is row-major linear-RGB doubles covering the page's real extent (tail
levels may be smaller than one page). The 1x1 last level is pinned in memory
and cannot be evicted. Invalid sizes, keys, pixel counts or non-finite values
are rejected before any mutation.

```cpp
vt::VirtualTexturePool pool({{/*id*/0, /*base*/16, 16, /*page*/4}});
pool.upload({vt::PageKey{0, 0, 0, 0}, 4, 4, rgb});   // one 4x4 page
auto snapshot = pool.snapshot();                     // owning, immutable
```

A `VirtualTextureFrame` owns the clip-space triangles, per-corner UVs and
texture ids it is given, and binds one snapshot and one viewport. After all
triangles are submitted, `generate()` rasterizes them with the same area
culling, perspective-correct barycentrics and "larger z/w is closer" depth
rule as the legacy path (depth ties keep the first face, discarded fragments
never write depth). UVs are interpolated as a perspective quotient and their
screen-space derivatives are analytical derivatives of that quotient, giving
`rho` and a clamped fractional LOD.

Sampling uses repeat wrapping with bilinear coordinates `(u*W-0.5, v*H-0.5)`;
zero-weight taps are not read and taps may cross pages and periodic
boundaries. If any page needed by the ideal LOD level is missing, the whole
bilinear footprint is retried on coarser levels (taps recomputed on the
original UV each time) until every tap is resident. The two LOD dwords are
resolved independently and blended by the fractional LOD. Each fragment
exposes its linear color, winning face id, depth, UV, derivatives, LOD, the
ideal and actually used levels, and the frame returns the sorted,
de-duplicated list of missing pages from visible fragments only.

```cpp
vt::VirtualTextureFrame frame(width, height, viewport, snapshot);
frame.submit_triangle(/*texture*/0, /*discard*/false, clip, uv);
frame.generate();
const vt::FragmentInfo &f = frame.fragment(x, y);
for (const vt::PageKey &k : frame.missing_pages()) /* stream this page */;
```

Snapshots and generated frames are immutable and self-contained: later
uploads, replacements, evictions or global viewport changes never alter them,
and two frames can be interleaved freely. An empty frame yields zero color,
face id `-1` and an empty feedback list.

### Virtual texture example and tests

The example procedurally builds two small textures and two occluding
triangles, then writes the blurry pre-streaming image, the sharp
post-streaming image and the missing-page list into the given directory:

```sh
c++ -std=c++20 -O1 -I. examples/virtual_texture.cpp \
    our_gl.cpp tgaimage.cpp virtual_texture.cpp -o virtual_texture
./virtual_texture <output-directory>
# writes before_loading.tga, after_loading.tga, missing_pages.txt
```

The task test suite lives in `tests/virtual_texture_cases.cpp` (groups
`paging`, `footprint`, `visibility`) and runs together with the legacy
baseline through:

```sh
bash task_support/run.sh target
```
