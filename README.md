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

`zimg_filter_graph_build_with_lut` inserts a tetrahedrally interpolated 3D LUT
into the normal row-buffered graph. The table is applied after resampling and
conversion to linear RGB in the destination primaries, and before transfer
encoding, YUV conversion, chroma subsampling, and depth quantization. Both
image formats must explicitly specify primaries and transfer characteristics.

The table uses `size³` RGB triplets. R changes fastest, then G, then B;
`data_count` counts floats (`3 * size³`). The graph copies the table during
construction, so the caller's array can be released or reused immediately.

```c
#include <zimg.h>

unsigned n = 17;
float *table = ...; /* n*n*n RGB triplets, R fastest */

zimg_lut3d lut;
lut.size = n;
lut.data = table;
lut.data_count = 3ULL * n * n * n;
for (int c = 0; c < 3; ++c) {
    lut.domain_min[c] = 0.0f;
    lut.domain_max[c] = 1.0f;
}

zimg_image_format src, dst;
zimg_image_format_default(&src, ZIMG_API_VERSION);
zimg_image_format_default(&dst, ZIMG_API_VERSION);
/* Set dimensions, pixel types, RGB/YUV family, and explicit
   color_primaries and transfer_characteristics on both formats. */

zimg_graph_builder_params params;
zimg_graph_builder_params_default(&params, ZIMG_API_VERSION);

zimg_filter_graph *graph =
    zimg_filter_graph_build_with_lut(&src, &dst, &params, &lut);
if (!graph) {
    char message[256];
    zimg_get_last_error(message, sizeof(message));
    /* Handle error. */
}

/* The graph has its own snapshot: free table here if desired. */

zimg_image_buffer_const src_buf = { ZIMG_API_VERSION };
zimg_image_buffer dst_buf = { ZIMG_API_VERSION };
/* Initialize planar buffer pointers, strides, and masks as usual. */

size_t tmp_size = 0;
zimg_filter_graph_get_tmp_size(graph, &tmp_size);
void *tmp = 0;
posix_memalign(&tmp, 64, tmp_size ? tmp_size : 64);

zimg_filter_graph_process(graph, &src_buf, &dst_buf, tmp,
                          0, 0, 0, 0);
free(tmp);
zimg_filter_graph_free(graph);
```

The C++ wrapper also exposes the new entry point:

```cpp
#include "api/zimg++.hpp"

try {
    zimgxx::FilterGraph graph =
        zimgxx::FilterGraph::build_with_lut(src, dst, &params, &lut);

    size_t tmp_size = graph.get_tmp_size();
    void *tmp = nullptr;
    posix_memalign(&tmp, 64, tmp_size ? tmp_size : 64);

    graph.process(src_buf, dst_buf, tmp);
    free(tmp);
} catch (const zimgxx::zerror &) {
    /* zimg_get_last_error() describes the failure. */
}
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

