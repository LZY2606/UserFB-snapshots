z.lib
======
The "z" library implements the commonly required image processing basics of
scaling, colorspace conversion, and depth conversion. A simple API enables
conversion between any supported formats to operate with minimal knowledge
from the programmer. All library routines were designed from the ground-up
with correctness, flexibility, and thread-safety as first priorities.
Allocation, buffering, and I/O are cleanly separated from processing, allowing
the programmer to adapt "z" to many scenarios.

Requirements
-----
- Byte-addressable architecture
- Two's complement integer encoding
- 32-bit or greater machine word
- C++17 compiler
- Platforms: Microsoft Windows, POSIX

Building
-----
The officially supported build system is GNU autotools. Use the provided
"autogen.sh" script to instantiate the familiar "configure" and "make" build
system. Visual Studio project files are not stable and are subject to change.
Download all submodules before compiling by `git submodule update --init
--recursive`.

Capabilities
-----
### Colorspace

Colorspaces: SMPTE-C (NTSC), Rec.709, Rec.2020

The colorspace module provides for conversion between any combination of
colorspaces, as defined by the commonly used triplet of matrix coefficients,
transfer characteristics, and color primaries. Conversions are implemented
with intelligent logic that minimizes the number of intermediate
representations required for common scenarios, such as conversion between
YCbCr and RGB. Support is also provided for the non-traditional YCbCr system
of ITU-R BT.2020 constant luminance (CL), which retains higher fidelity with
chroma subsampling. Note that "z" is not a color management system and should
not be used to perform drastic contrast or gamut reduction, such as BT.2020
to BT.709.

### Depth

Formats: BYTE, WORD, HALF, FLOAT

The depth module provides for conversion between any pixel (number) format,
including one and two-byte integer formats as well as IEEE-754 binary16
(OpenEXR) and binary32 formats. Limited range (16-235) and full swing (0-255)
integer formats are supported, including conversion between such formats.
Multiple dithering methods are available when converting to integer formats,
from basic rounding to high quality error diffusion.

### Resize

The resize module provides high fidelity linear resamplers, including the
popular Bicubic and Lanczos filters. Resampling ratios of up to 100x are
supported for upsampling and downsampling. Full support is provided for
various coordinate systems, including the various chroma siting conventions
(e.g. JPEG and MPEG2) as well as interlaced images.

### 3D LUT

A tetrahedrally interpolated 3D lookup table can be inserted into the filter
graph with `zimg_filter_graph_build_with_lut`. The LUT is applied after
resampling and linear conversion to the target primaries, but before transfer
encoding, YUV conversion, chroma subsampling, and depth quantization. Both
formats must explicitly define primaries and transfer; greyscale output is not
supported. The graph copies the table during construction, so the caller may
release it immediately afterwards.

C example:

```c
#include "zimg.h"
#include <stdlib.h>

/* size^3 RGB triplets, R fastest, then G, then B. */
static zimg_filter_graph *build_lut_graph(unsigned w, unsigned h,
                                          const float *table, unsigned size)
{
    zimg_image_format src, dst;
    zimg_image_format_default(&src, ZIMG_API_VERSION);
    src.width = w;
    src.height = h;
    src.pixel_type = ZIMG_PIXEL_FLOAT;
    src.color_family = ZIMG_COLOR_RGB;
    src.matrix_coefficients = ZIMG_MATRIX_RGB;
    src.transfer_characteristics = ZIMG_TRANSFER_LINEAR;
    src.color_primaries = ZIMG_PRIMARIES_BT709;
    dst = src; /* Any destination format works; YUV/integer outputs too. */

    zimg_lut3d lut;
    lut.size = size;
    lut.data = table;
    lut.data_count = (size_t)3 * size * size * size;
    for (int c = 0; c < 3; ++c) {
        lut.domain_min[c] = 0.0f;
        lut.domain_max[c] = 1.0f;
    }

    return zimg_filter_graph_build_with_lut(&src, &dst, NULL, &lut);
}
```

Processing uses the same planar `zimg_image_buffer` convention as
`zimg_filter_graph_process` (planes R, G, B, optional A):

```c
size_t tmp_size;
zimg_filter_graph_get_tmp_size(graph, &tmp_size);
void *tmp = NULL;
posix_memalign(&tmp, 64, tmp_size ? tmp_size : 64);

zimg_image_buffer_const in = { ZIMG_API_VERSION };
zimg_image_buffer out = { ZIMG_API_VERSION };
for (int p = 0; p < 3; ++p) {
    in.plane[p].data = src_plane[p];
    in.plane[p].stride = src_stride;
    in.plane[p].mask = ZIMG_BUFFER_MAX;
    out.plane[p].data = dst_plane[p];
    out.plane[p].stride = dst_stride;
    out.plane[p].mask = ZIMG_BUFFER_MAX;
}
zimg_filter_graph_process(graph, &in, &out, tmp, NULL, NULL, NULL, NULL);
```

C++ example using the header-only wrapper:

```cpp
#include "zimg++.hpp"

zimgxx::zimage_format src, dst;
src.width = dst.width = width;
src.height = dst.height = height;
src.pixel_type = dst.pixel_type = ZIMG_PIXEL_FLOAT;
src.color_family = dst.color_family = ZIMG_COLOR_RGB;
src.matrix_coefficients = dst.matrix_coefficients = ZIMG_MATRIX_RGB;
src.transfer_characteristics = dst.transfer_characteristics = ZIMG_TRANSFER_LINEAR;
src.color_primaries = dst.color_primaries = ZIMG_PRIMARIES_BT709;

zimg_lut3d lut{ size, table, 3ULL * size * size * size,
                { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f } };

zimgxx::FilterGraph graph =
    zimgxx::FilterGraph::build_with_lut(src, dst, &lut);
size_t tmp_size = graph.get_tmp_size();
/* ...allocate aligned tmp and zimgxx::zimage_buffer planes, then... */
graph.process(in_buf, out_buf, tmp.get());
```

Performance
-----
"z" is optimized for Intel(R) Architecture and features faster processing times
than industry standard swscale software.

Time (ms) to resize FHD image to UHD with Lanczos filter.

|                                | z.lib 2.8 | swscale 4.0.2* |
|--------------------------------|-----------|----------------|
| Intel(R) Core(TM) i7-8565U     |       7.7 |           15.2 |
| Intel(R) Xeon(R) Platinum 8176 |      10.8 |           22.2 |

Time (ms) to convert FHD BT.709 (YUV) to FHD BT.2020.

|                                | z.lib 2.8 | swscale 4.0.2** |
|--------------------------------|-----------|-----------------|
| Intel(R) Core(TM) i7-8565U     |       8.3 |            17.5 |
| Intel(R) Xeon(R) Platinum 8176 |      11.5 |            25.6 |

\* `scale=3840:2160:sws_flags=lanczos+accurate_rnd:sws_dither=none`

\** `colorspace=all=bt2020:iall=bt709:format=yuv420p10`

