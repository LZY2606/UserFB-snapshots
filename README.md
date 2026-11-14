# Andrew Lim's C++ 3D Software Rasterizer

Personal 3D playground program for me to test and learn 3D rendering. Source code made public in case somebody finds it helpful.
Though it is a still a very much work in progress.

You will need a C++ 11 compiler. Currently I'm developing with [Embarcadero Dev-C++](https://github.com/Embarcadero/Dev-Cpp)

All rendering is done first on a custom [ImageData](src/ImageData.h) object which holds a one-dimensional array of bytes (of type uint8_t).
Then the bytes are transferred/blitted to the native screen format. In this case, I'm testing with Win32 GDI where the bytes are
displayed with [SetDIBitsToDevice](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setdibitstodevice).

[Download Latest Release Here](https://github.com/andrew-lim/sw3dcpp/releases/).

## Screenshots
<table style="padding:10px">
  <tr>
    <td width="50%"><img src="screenshots/screenshot01.png"/></td>
    <td width="50%"><img src="screenshots/screenshot02.png"/></td>
  </tr>
  <tr>
    <td width="50%"><img src="screenshots/screenshot03.png"/></td>
    <td width="50%"><img src="screenshots/screenshot04.png"/></td>
  </tr>
</table>

## Videos

[YouTube Playlist](https://www.youtube.com/watch?v=YPubnjG4eH8&list=PL4Pn9mFG1hxREHDD_Zxq7BTCi5jRkjrjr&ab_channel=AndrewLim)

## Features

- 4 triangle rendering algorithms: standard scanline, barycentric, and 2 optimized barycentric variants
- Displays a simple textured cube at the start
- Loading .OBJ and .MTL files with BMP/PNG support. Sample models can be loaded from the Debug menu
- Solid, Affine and Nearest Neighbour Perspective-Correct Texture Mapping
- Clipping by near plane only, or clipping by all 6 planes
- Z Buffer
- Flat and Smooth/Gourand Lighting

## Libraries Used

- [lodepng](https://github.com/lvandeve/lodepng) for PNG loading
- [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) for .OBJ parsing
- [glm](https://github.com/g-truc/glm) was previously used for matrix math but has been replaced by custom Matrix and Vector classes. However it is still distributed together with the source code.

## Retained Rendering (partial redraw)

[RetainedRenderer](src/RetainedRenderer.h) keeps a scene of objects and only
repaints the 8x8 tiles an object touched when it is added, replaced or removed.
Pixel filling is still delegated to [TriangleDrawer](src/TriangleDrawer.h)
through its optional half-open scissor rectangle, so each algorithm keeps its
original sampling and interpolation behaviour.

```cpp
#include "RetainedRenderer.h"
using namespace al::graphics;

// A retained scene with a fixed canvas size and one fill algorithm.
RetainedRenderer scene(320, 200, TriangleDrawer::BARYCENTRIC_O1,
                       ImageData::makePixel(0, 0, 0, 255));

// Per-object material. Triangles and textures are copied by set().
RetainedRenderer::Material material(
  TriangleDrawer::DRAW_PERSPECTIVE,   // or DRAW_SOLID / DRAW_AFFINE / DRAW_RGB_SHADED
  TriangleDrawer::LIGHTS_STYLE_SMOOTH,
  G3D::normalize(Vector3f(0.3f, 0.4f, 0.6f)));
material.texture = myImageData;     // optional; omit for untextured draws

std::vector<Triangle> triangles = loadSomeTriangles();
scene.set(/* id */ 42, triangles, material);
scene.flush();                      // first flush initializes the whole canvas

// Later: move the object and only its old/new tiles are repainted.
triangles = translate(std::move(triangles), 4.0f, 0.0f);
scene.set(42, triangles, material);
scene.flush();

// The latest result, depth buffer and per-flush work counters.
const ImageData& image = scene.image();
const ZBuffer& depth = scene.depth();
const RetainedRenderer::Stats& stats = scene.stats();
// stats.dirty_tiles, stats.candidate_visits, stats.raster_calls

// Removing an object repaints just the tiles it used to cover.
scene.erase(42);
scene.flush();
```

`TriangleDrawer` itself can also be scissored directly; the rectangle is
`[x0, x1) x [y0, y1)` and is disabled by default:

```cpp
TriangleDrawer drawer(&depthBuffer);
drawer.setScissor(16, 16, 32, 32);
drawer.triangle(image, triangle);   // only pixels inside the rect change
drawer.clearScissor();
```

MIT Licensed
