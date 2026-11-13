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

## Weighted-blended transparency

The default pipeline treats every fragment as an opaque surface, so
overlapping thin surfaces hide each other through the z-buffer.
`transparency.h/.cpp` adds an order-independent path based on weighted
blended order-independent transparency (WBOIT): transparent triangles can be
submitted in any order, intersecting surfaces need no sorting, and a single
composite pass produces the final pixels.

Per-frame call order:

1. Run the opaque pass exactly as before (`rasterize` + `IShader`); it keeps
   writing the ordinary z-buffer and the color framebuffer.
2. Construct a `TransparencyBuffer` with the same width and height, passing
   the opaque z-buffer. It only reads that buffer and never writes it.
3. `submit()` every transparent triangle through an `ITransparencyShader`.
   The shader returns a discard flag plus non-premultiplied linear RGB and an
   alpha in `[0,1]`. Rasterization reuses the same traversal as `rasterize`
   (perspective-correct barycentrics, coverage and back-face culling; no
   frustum clipping is added). A fragment survives only when its depth is
   strictly greater than the frozen opaque depth — equal depth counts as
   occluded — and earlier transparent surfaces never block later ones.
4. Call `composite()` once onto the opaque `TGAImage` (RGB, or RGBA whose
   alpha bytes are all 255). Pixels without any contribution keep their
   background bytes; output alpha is 255. Compositing again is a no-op, so
   the previous result is never blended a second time.
5. For the next frame, call `clear()` (or construct a fresh buffer) after the
   new opaque pass; the accumulators hold floating-point state for the whole
   frame and are never quantized per triangle.

A standalone scene with two intersecting colored sheets, an opaque occluder
and a checkerboard background lives in `examples/transparency.cpp`:

```sh
sh task_support/run.sh target     # builds the example, writes a small TGA, runs the numeric tests
```

Per pixel the buffer keeps `A.rgb += rgb*alpha*w`, `A.a += alpha*w` and
`R *= 1-alpha` (initialized to zero, zero and one), where
`w = clamp(0.01+8*alpha, 0.01, 8) * clamp(1+z/w_clip, 0.1, 2)`. The average
transparent color is `A.rgb/A.a`, the coverage is `1-R`, and those are blended
once over the frozen opaque background. The running values can be inspected
through `accumulated_rgb()`, `accumulated_alpha()` and `revealage()`.

### Applicability of the approximation

Weighted blending is an approximation, not exact compositing. It fits thin,
low-to-moderate-alpha surfaces (glass, colored sheets, smoke, hair wisps) and
intersecting geometry where sorting triangles is impractical. The weighted
average cannot reproduce a strict back-to-front color order, so strongly
absorbing or nearly opaque layers, high-contrast overlapping colors, and
correct refraction or transmission of the background are not modeled; for
those, sorted alpha blending or a per-pixel fragment list is needed. It also
assumes an opaque background: the framebuffer passed to `composite` must be
RGB or fully opaque RGBA.
