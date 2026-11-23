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

A three-dimensional color lookup table can be folded into the same filter
graph with `zimg_filter_graph_build_with_lut`. It accepts the same source
format, destination format, and builder parameters as
`zimg_filter_graph_build`, plus a `zimg_lut3d` descriptor. The lookup is
inserted after spatial resampling and conversion to linear RGB in the target
primaries, and before the target transfer characteristics, YUV matrix
coefficients, chroma subsampling, and bit depth quantization. The table is
therefore evaluated on linear RGB relative to the target white point, and it
runs even when the source and destination formats are identical.

The table holds `size` cubed RGB triplets in R-fastest order (`R` changes
fastest, then `G`, then `B`), and `data_count` is the number of floats. `size`
must be between 2 and 65. Each channel has an independent input domain given
by `domain_min` and `domain_max`; values outside the domain are clamped to the
boundary, and values inside are interpolated with tetrahedral interpolation.
Table values may lie outside the range `[0, 1]`; they are not clipped for
floating-point output. The graph takes a private snapshot of the table, so the
caller may modify or free the data immediately after building the graph.

Both formats must specify color primaries and transfer characteristics, and a
greyscale destination is not supported. Alpha is handled with the usual
premultiplied-alpha rules; the lookup always operates on unpremultiplied color,
and color under fully transparent pixels is forced to zero after the lookup.

In C:

```c
zimg_image_format src, dst;
zimg_image_format_default(&src, ZIMG_API_VERSION);
zimg_image_format_default(&dst, ZIMG_API_VERSION);

/* ... fill in width, height, pixel types, colorspace ... */
src.color_family = ZIMG_COLOR_RGB;
src.matrix_coefficients = ZIMG_MATRIX_RGB;
src.transfer_characteristics = ZIMG_TRANSFER_IEC_61966_2_1;
src.color_primaries = ZIMG_PRIMARIES_BT709;

/* A 2x2x2 identity table over [0, 1]. */
static const float table[2 * 2 * 2 * 3] = {
	/* b=0, g=0: r=0, r=1 */ 0, 0, 0, 1, 0, 0,
	/* b=0, g=1: r=0, r=1 */ 0, 1, 0, 1, 1, 0,
	/* b=1, g=0: r=0, r=1 */ 0, 0, 1, 1, 0, 1,
	/* b=1, g=1: r=0, r=1 */ 0, 1, 1, 1, 1, 1,
};

zimg_lut3d lut = { 0 };
lut.size = 2;
lut.data = table;
lut.data_count = 2 * 2 * 2 * 3;
lut.domain_min[0] = lut.domain_min[1] = lut.domain_min[2] = 0.0f;
lut.domain_max[0] = lut.domain_max[1] = lut.domain_max[2] = 1.0f;

zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(&src, &dst, NULL, &lut);
```

In C++:

```cpp
zimgxx::zimage_format src_format;
zimgxx::zimage_format dst_format;
/* ... fill in dimensions and formats ... */

std::vector<float> data(/* size^3 * 3 */);
zimg_lut3d lut = {};
lut.size = 33;
lut.data = data.data();
lut.data_count = data.size();
for (int c = 0; c < 3; ++c) {
	lut.domain_min[c] = 0.0f;
	lut.domain_max[c] = 1.0f;
}

zimg_filter_graph *graph = zimg_filter_graph_build_with_lut(
	&src_format, &dst_format, nullptr, &lut);
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

