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

`transparency.h` adds an order-independent transparency path on top of the CPU
rasterizer. Opaque geometry still goes through `rasterize()`/`IShader`; colored
semi-transparent thin surfaces are submitted to a per-frame
`TransparencyBuffer`, then composited over the finished opaque frame.

Per pixel the buffer accumulates (all values in floating point, no sorting and
no per-fragment quantization):

```text
A.rgb += RGB * alpha * w
A.a   += alpha * w
R     *= 1 - alpha
```

with `A.rgb = A.a = 0`, `R = 1` initially and

```text
w = clamp(0.01 + 8*alpha, 0.01, 8) * clamp(1 + z/w, 0.1, 2)
```

where `z/w` is the fragment's ndc depth. The average color is `A.rgb/A.a`, the
coverage is `1-R`, and `composite()` blends that with the frozen opaque
background. Fragments are never depth-sorted against each other, so drawing
order does not matter and intersecting sheets blend correctly.

### Calling order

```cpp
// 1. Draw all opaque geometry exactly as before.
rasterize(clip, opaque_shader, framebuffer);   // writes framebuffer + zbuffer

// 2. Create the per-frame transparent buffer, sized like color and depth.
TransparencyBuffer transparency(width, height);
transparency.clear();                          // explicit per-frame clear

// 3. Freeze the opaque depth of this frame and submit transparent triangles.
transparency.set_opaque_depth(zbuffer);        // or submit() after drawing more
transparency.submit();
transparency.triangle(transparent_clip, transparent_shader);

// 4. Composite once onto the opaque RGB/RGBA(alpha=255) frame.
transparency.composite(framebuffer);
```

`ITransparentShader::fragment` returns non-premultiplied linear RGB and alpha in
`[0,1]` plus a `discard` flag. Transparent fragments are tested only against the
frozen opaque depth (`depth <= opaque depth` counts as occluded), never write
the opaque depth, and are never occluded by one another. `composite()` captures
the background on first use, so calling it twice yields the same image; call
`clear()` before starting the next frame. Accumulated values and `R` are
readable via `accumulated_rgb()`, `accumulated_alpha()` and `transmittance()`.

Run the self-contained demo with:

```sh
sh task_support/run.sh target          # tests + example
# or directly:
c++ -std=c++20 -O1 -I. examples/transparency.cpp transparency.cpp our_gl.cpp tgaimage.cpp -o example
./example output.tga
```

### Scope of the approximation

Weighted-blended transparency is an approximation, not a physically correct
alpha compositor:

- It is intended for thin, low-to-medium opacity surfaces (glass, smoke,
  colored sheets). With many overlapping layers, or with strongly opaque
  surfaces, the result may look washed out or lose ordering cues: the single
  average color cannot represent a different layer order per pixel.
- Because `R` multiplies `(1-alpha)`, many nearly opaque transparent layers
  fade out coverage but still contribute to the average color; the blend is
  symmetric in drawing order, so front-to-back cues are lost.
- Depth is evaluated per fragment against the opaque depth only; transparent
  geometry hidden by opaque geometry is skipped, and a transparent surface
  exactly at the opaque depth is treated as occluded.
- No view-frustum clipping is added: geometry must already be inside the
  renderer's existing clip assumptions (`rasterize()` clips the bounding box to
  the screen only).
