# C++ Software Renderer/Rasterizer

This project implements a C++ software renderer/rasterizer with vertex- and
pixel shader support.

## Features
* Generic vertex arrays for arbitrary data in the vertex processing stage.
* Internal vertex cache for better vertex processing.
* Affine and perspective correct per vertex parameter interpolation.
* Vertex and pixel shaders written in C++ using some C++ template magic.

## Resources

* [Triangle Rasterization](http://www.cs.unc.edu/~blloyd/comp770/Lecture08.pdf)
* [Accelerated Half-Space Triangle Rasterization](https://www.researchgate.net/publication/286441992_Accelerated_Half-Space_Triangle_Rasterization)

## Example

```c++

#include "Renderer.h"

// Define a pixel shader
class PixelShader : public PixelShaderBase<PixelShader> {
public:
  static const bool InterpolateZ = true;
  static const bool InterpolateW = true;

  // Number of affine/linear variables used.
  static const int AVarCount = 3;

  // Number of perspective correct variables used.
  static const int PVarCount = 2;

  static void drawPixel(const PixelData &p)
  {
      ...
  }
};

// Define a vertex shader
class VertexShader : public VertexShaderBase<VertexShader> {
public:
  // Number of input vertex attributes used.
  static const int AttribCount = 1;

  static void processVertex(VertexShaderInput in, VertexShaderOutput *out)
  {
    // Write result to out.
    ...
  }
};

// Use the renderer
Rasterizer r;
VertexProcessor v(&r);

r.setRasterMode(RasterMode::Span);
r.setScissorRect(0, 0, 640, 480);
r.setPixelShader<PixelShader>();

v.setViewport(0, 0, 640, 480);
v.setCullMode(CullMode::CW);
v.setVertexShader<VertexShader>();

// Draw
v.setVertexAttribPointer(0, sizeof(VertexData), vertexData);
v.drawElements(DrawMode::Triangle, indexData.size(), indexData);

```

## Volumetric preview (VolumeRenderer)

`src/renderer/VolumeRenderer.h` adds an off-screen preview renderer for
glowing, translucent volumes built from tetrahedral cells. It reuses the
regular pipeline: every cell boundary face is drawn double-sided through
the real `VertexProcessor` (homogeneous clipping) and `Rasterizer`
(Span/Block/Adaptive) triangle path, and per-pixel enter/exit events are
recovered from the fragments with the cell identity attached.

Usage:

```c++
#include "VolumeRenderer.h"

swr::VolumeRenderer vr;
vr.setRasterMode(swr::RasterMode::Span); // Span, Block or Adaptive

// Tetrahedral cells in view space (camera at origin looking down -Z).
// Constant non-negative extinction sigma and non-negative RGB emission j
// per cell; cells may overlap and share faces.
std::vector<swr::VolumeRenderer::TetCell> cells = ...;

// Column-major 4x4 perspective matrix (clip = P * view, clip.w = -view.z,
// NDC z in [-1, 1]; asymmetric frusta supported) and the viewport
// (origin may be non-zero, x right, y down).
float proj[16] = ...;
swr::VolumeRenderer::Viewport vp{0, 0, 640, 480};

// Validates cells/projection/viewport (std::invalid_argument on illegal
// input, keeping the previous capture) and captures the volume. The
// capture owns all data; the caller may free or reuse the cell array.
vr.capture(cells.data(), cells.size(), proj, vp);

// Read-only compositing over a viewport-sized base image: linear RGB
// (3 floats/pixel) plus opaque depth in [0, 1] per pixel (1 = background).
// May be repeated on the same capture with different base images.
swr::VolumeRenderer::Image img = vr.composite(baseRgb.data(), baseDepth.data());
// img.rgb: owned linear RGB (w*h*3), img.transmittance: owned T (w*h).
```

Interval lengths are view-space Euclidean distances along the pixel-center
ray. Captured intervals are first truncated against the unprojected opaque
depth, then all cell endpoints are swept in distance order (equal distances
are applied as one group, so cell submission order cannot change the
result). Each constant-coefficient segment of length L integrates
`C += T*j*(1-exp(-sigma*L))/sigma`, `T *= exp(-sigma*L)` (or `C += T*j*L`
when `sigma == 0`), and finally `C += T * baseRgb` is added without gamma
or byte quantization. An empty volume preserves the base image exactly
with `T == 1`.

## License

This code is licensed under the MIT License (see [LICENSE](LICENSE)).