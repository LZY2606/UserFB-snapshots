/*
 * Copyright (c) Radzivon Bartoshyk, 11/2026. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3.  Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
//!
//! Tests for the explicit chroma-siting bilinear entry points.
//!
//! Expected values are never produced by the functions under test: every expectation is
//! derived independently from hand-written bilinear tap weights and then fed through the
//! existing 4:4:4 8-bit conversion, which applies the library's usual range/matrix path.
#![allow(clippy::too_many_arguments)]

use yuv::{
    yuv420_to_bgr_bilinear, yuv420_to_bgr_bilinear_with_siting, yuv420_to_bgra_bilinear,
    yuv420_to_bgra_bilinear_with_siting, yuv420_to_rgb_bilinear,
    yuv420_to_rgb_bilinear_with_siting, yuv420_to_rgba_bilinear,
    yuv420_to_rgba_bilinear_with_siting, yuv422_to_bgr_bilinear,
    yuv422_to_bgr_bilinear_with_siting, yuv422_to_bgra_bilinear,
    yuv422_to_bgra_bilinear_with_siting, yuv422_to_rgb_bilinear,
    yuv422_to_rgb_bilinear_with_siting, yuv422_to_rgba_bilinear,
    yuv422_to_rgba_bilinear_with_siting, yuv444_to_bgr, yuv444_to_bgra, yuv444_to_rgb,
    yuv444_to_rgba, YuvChromaSiting, YuvChromaSubsampling, YuvError, YuvPlanarImage, YuvRange,
    YuvStandardMatrix,
};

type SitingFn = fn(
    &YuvPlanarImage<'_, u8>,
    &mut [u8],
    u32,
    YuvRange,
    YuvStandardMatrix,
    YuvChromaSiting,
) -> Result<(), YuvError>;

const SITINGS: [YuvChromaSiting; 3] = [
    YuvChromaSiting::Center,
    YuvChromaSiting::Left,
    YuvChromaSiting::TopLeft,
];

#[derive(Copy, Clone, Debug)]
enum Layout {
    Rgb,
    Bgr,
    Rgba,
    Bgra,
}

impl Layout {
    fn channels(self) -> usize {
        match self {
            Layout::Rgb | Layout::Bgr => 3,
            Layout::Rgba | Layout::Bgra => 4,
        }
    }
}

const LAYOUTS: [Layout; 4] = [Layout::Rgb, Layout::Bgr, Layout::Rgba, Layout::Bgra];

/// Deterministic LCG so the test needs no RNG and behaves identically feature-gated or not.
struct Lcg(u32);

impl Lcg {
    fn next_u8(&mut self) -> u8 {
        // Numerical Recipes constants, keep only the low byte.
        self.0 = self.0.wrapping_mul(1664525).wrapping_add(1013904223);
        (self.0 >> 24) as u8
    }
}

fn make_plane(width: usize, height: usize, stride: usize, rng: &mut Lcg, sentinel: u8) -> Vec<u8> {
    let mut plane = vec![sentinel; stride * (height - 1) + width];
    for row in 0..height {
        for x in 0..width {
            plane[row * stride + x] = rng.next_u8();
        }
    }
    plane
}

fn half_offsets(sampling: YuvChromaSubsampling, siting: YuvChromaSiting) -> (u32, u32) {
    let sx = match siting {
        YuvChromaSiting::Center => 1,
        YuvChromaSiting::Left | YuvChromaSiting::TopLeft => 0,
    };
    let sy = match sampling {
        YuvChromaSubsampling::Yuv420 => match siting {
            YuvChromaSiting::Center | YuvChromaSiting::Left => 1,
            YuvChromaSiting::TopLeft => 0,
        },
        YuvChromaSubsampling::Yuv422 => 0,
        _ => unreachable!(),
    };
    (sx, sy)
}

/// Hand-written bilinear chroma interpolation, in quarter-chroma-sample units.
///
/// Luma pixel `p` maps to chroma grid coordinate `(2p - s) / 2`, i.e. quarter units
/// `q = 2p - s` (`s` is 0 or 1 half-luma-pixel). Out-of-range taps extend the nearest
/// valid chroma sample instead of reading stride padding.
fn interpolate_chroma(
    plane: &[u8],
    stride: usize,
    chroma_width: usize,
    chroma_height: usize,
    vertical: bool,
    siting: YuvChromaSiting,
    sampling: YuvChromaSubsampling,
    x: usize,
    y: usize,
) -> u8 {
    let (sx, sy) = half_offsets(sampling, siting);
    let qx = 2 * x as isize - sx as isize;
    let bx = qx.div_euclid(4);
    let fx = qx.rem_euclid(4) as usize;
    let ix0 = bx.clamp(0, chroma_width as isize - 1) as usize;
    let ix1 = (bx + 1).clamp(0, chroma_width as isize - 1) as usize;
    let wx0 = (4 - fx) as i32;
    let wx1 = fx as i32;

    if !vertical {
        // 4:2:2: the chroma row is the luma row, only two horizontal taps.
        let row = &plane[y * stride..y * stride + chroma_width];
        return ((row[ix0] as i32 * wx0 + row[ix1] as i32 * wx1 + 2) >> 2) as u8;
    }

    let qy = 2 * y as isize - sy as isize;
    let by = qy.div_euclid(4);
    let fy = qy.rem_euclid(4) as usize;
    let iy0 = by.clamp(0, chroma_height as isize - 1) as usize;
    let iy1 = (by + 1).clamp(0, chroma_height as isize - 1) as usize;
    let wy0 = (4 - fy) as i32;
    let wy1 = fy as i32;

    let row0 = &plane[iy0 * stride..iy0 * stride + chroma_width];
    let row1 = &plane[iy1 * stride..iy1 * stride + chroma_width];
    // Four taps, weights sum to 16; single nearest-integer rounding, halves going up.
    let sum = row0[ix0] as i32 * wx0 * wy0
        + row0[ix1] as i32 * wx1 * wy0
        + row1[ix0] as i32 * wx0 * wy1
        + row1[ix1] as i32 * wx1 * wy1;
    ((sum + 8) >> 4) as u8
}

/// Builds the independently interpolated 4:4:4 planes and runs the existing 4:4:4 path.
fn reference_via_444(
    y: &[u8],
    y_stride: usize,
    u: &[u8],
    u_stride: usize,
    v: &[u8],
    v_stride: usize,
    width: usize,
    height: usize,
    sampling: YuvChromaSubsampling,
    siting: YuvChromaSiting,
    layout: Layout,
    range: YuvRange,
    matrix: YuvStandardMatrix,
) -> Vec<u8> {
    let vertical = sampling == YuvChromaSubsampling::Yuv420;
    let chroma_width = width.div_ceil(2);
    let chroma_height = if vertical { height.div_ceil(2) } else { height };

    let mut u444 = vec![0u8; width * height];
    let mut v444 = vec![0u8; width * height];
    let mut y444 = vec![0u8; width * height];
    for row in 0..height {
        for x in 0..width {
            y444[row * width + x] = y[row * y_stride + x];
            u444[row * width + x] = interpolate_chroma(
                u,
                u_stride,
                chroma_width,
                chroma_height,
                vertical,
                siting,
                sampling,
                x,
                row,
            );
            v444[row * width + x] = interpolate_chroma(
                v,
                v_stride,
                chroma_width,
                chroma_height,
                vertical,
                siting,
                sampling,
                x,
                row,
            );
        }
    }

    let image444 = YuvPlanarImage {
        y_plane: &y444,
        y_stride: width as u32,
        u_plane: &u444,
        u_stride: width as u32,
        v_plane: &v444,
        v_stride: width as u32,
        width: width as u32,
        height: height as u32,
    };
    let channels = layout.channels();
    let mut dst = vec![0u8; width * height * channels];
    let stride = (width * channels) as u32;
    match layout {
        Layout::Rgb => yuv444_to_rgb(&image444, &mut dst, stride, range, matrix).unwrap(),
        Layout::Bgr => yuv444_to_bgr(&image444, &mut dst, stride, range, matrix).unwrap(),
        Layout::Rgba => yuv444_to_rgba(&image444, &mut dst, stride, range, matrix).unwrap(),
        Layout::Bgra => yuv444_to_bgra(&image444, &mut dst, stride, range, matrix).unwrap(),
    }
    dst
}

fn siting_fn(sampling: YuvChromaSubsampling, layout: Layout) -> SitingFn {
    match (sampling, layout) {
        (YuvChromaSubsampling::Yuv420, Layout::Rgb) => yuv420_to_rgb_bilinear_with_siting,
        (YuvChromaSubsampling::Yuv420, Layout::Bgr) => yuv420_to_bgr_bilinear_with_siting,
        (YuvChromaSubsampling::Yuv420, Layout::Rgba) => yuv420_to_rgba_bilinear_with_siting,
        (YuvChromaSubsampling::Yuv420, Layout::Bgra) => yuv420_to_bgra_bilinear_with_siting,
        (YuvChromaSubsampling::Yuv422, Layout::Rgb) => yuv422_to_rgb_bilinear_with_siting,
        (YuvChromaSubsampling::Yuv422, Layout::Bgr) => yuv422_to_bgr_bilinear_with_siting,
        (YuvChromaSubsampling::Yuv422, Layout::Rgba) => yuv422_to_rgba_bilinear_with_siting,
        (YuvChromaSubsampling::Yuv422, Layout::Bgra) => yuv422_to_bgra_bilinear_with_siting,
        _ => unreachable!(),
    }
}

struct PaddedFrame {
    y: Vec<u8>,
    u: Vec<u8>,
    v: Vec<u8>,
    y_stride: usize,
    u_stride: usize,
    v_stride: usize,
    width: usize,
    height: usize,
}

impl PaddedFrame {
    fn image(&self) -> YuvPlanarImage<'_, u8> {
        YuvPlanarImage {
            y_plane: &self.y,
            y_stride: self.y_stride as u32,
            u_plane: &self.u,
            u_stride: self.u_stride as u32,
            v_plane: &self.v,
            v_stride: self.v_stride as u32,
            width: self.width as u32,
            height: self.height as u32,
        }
    }
}

fn build_frame(
    width: usize,
    height: usize,
    sampling: YuvChromaSubsampling,
    seed: u32,
    pad: bool,
) -> PaddedFrame {
    let mut rng = Lcg(seed);
    let chroma_width = width.div_ceil(2);
    let chroma_height = match sampling {
        YuvChromaSubsampling::Yuv420 => height.div_ceil(2),
        _ => height,
    };
    let (y_stride, u_stride, v_stride) = if pad {
        (width + 3, chroma_width + 2, chroma_width + 5)
    } else {
        (width, chroma_width, chroma_width)
    };
    PaddedFrame {
        y: make_plane(width, height, y_stride, &mut rng, 0xCD),
        u: make_plane(chroma_width, chroma_height, u_stride, &mut rng, 0xAB),
        v: make_plane(chroma_width, chroma_height, v_stride, &mut rng, 0xEF),
        y_stride,
        u_stride,
        v_stride,
        width,
        height,
    }
}

/// Full cross-product: both samplings, three sitings, four channel orders, both matrices,
/// both ranges, including odd and single-pixel dimensions and non-compact planes.
#[test]
fn siting_conversion_matches_independent_444_reference() {
    let dimensions = [
        (1usize, 1usize),
        (3, 1),
        (1, 3),
        (5, 5),
        (7, 4),
        (4, 7),
        (9, 9),
    ];
    let matrices = [YuvStandardMatrix::Bt601, YuvStandardMatrix::Bt709];
    let ranges = [YuvRange::Full, YuvRange::Limited];

    for sampling in [YuvChromaSubsampling::Yuv420, YuvChromaSubsampling::Yuv422] {
        for (idx, &(width, height)) in dimensions.iter().enumerate() {
            for (pad_idx, pad) in [false, true].iter().enumerate() {
                let frame = build_frame(
                    width,
                    height,
                    sampling,
                    0x9E3779B9 ^ ((idx as u32) << 8) ^ ((pad_idx as u32) << 16),
                    *pad,
                );
                for siting in SITINGS {
                    for layout in LAYOUTS {
                        for matrix in matrices {
                            for range in ranges {
                                let channels = layout.channels();
                                let dst_stride = width * channels + if *pad { 2 } else { 0 };
                                let mut dst =
                                    vec![0x77u8; dst_stride * (height - 1) + width * channels];
                                siting_fn(sampling, layout)(
                                    &frame.image(),
                                    &mut dst,
                                    dst_stride as u32,
                                    range,
                                    matrix,
                                    siting,
                                )
                                .unwrap();
                                let expected = reference_via_444(
                                    &frame.y,
                                    frame.y_stride,
                                    &frame.u,
                                    frame.u_stride,
                                    &frame.v,
                                    frame.v_stride,
                                    width,
                                    height,
                                    sampling,
                                    siting,
                                    layout,
                                    range,
                                    matrix,
                                );
                                for row in 0..height {
                                    let got =
                                        &dst[row * dst_stride..row * dst_stride + width * channels];
                                    let want = &expected
                                        [row * width * channels..(row + 1) * width * channels];
                                    assert_eq!(
                                        got, want,
                                        "sampling {sampling:?} siting {siting:?} layout {layout:?} \
                                         matrix {matrix:?} range {range:?} size {width}x{height} pad {pad} row {row}"
                                    );
                                }
                                if matches!(layout, Layout::Rgba | Layout::Bgra) {
                                    for row in 0..height {
                                        for x in 0..width {
                                            assert_eq!(
                                                dst[row * dst_stride + x * channels + 3],
                                                255,
                                                "alpha must be 255"
                                            );
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/// The exact numbers from the design: 4x4 luma 128, V 128, U grid [[80,96],[112,128]].
#[test]
fn given_4x4_frame_produces_92_96_104_at_pixel_1_1() {
    let width = 4usize;
    let height = 4usize;
    let chroma = 2usize;
    let y = vec![128u8; width * height];
    let v = vec![128u8; chroma * chroma];
    let u_grid = [80u8, 96, 112, 128];

    let expected_u = [
        (YuvChromaSiting::Center, 92u8),
        (YuvChromaSiting::Left, 96),
        (YuvChromaSiting::TopLeft, 104),
    ];

    for (siting, u_value) in expected_u {
        // First, the independent hand-written tap weights must produce the specified U
        // value at luma pixel (1, 1) before any color conversion happens.
        assert_eq!(
            interpolate_chroma(
                &u_grid,
                chroma,
                chroma,
                chroma,
                true,
                siting,
                YuvChromaSubsampling::Yuv420,
                1,
                1
            ),
            u_value,
            "siting {siting:?} must first interpolate U at (1,1) to {u_value}"
        );

        let frame = YuvPlanarImage {
            y_plane: &y,
            y_stride: width as u32,
            u_plane: &u_grid,
            u_stride: chroma as u32,
            v_plane: &v,
            v_stride: chroma as u32,
            width: width as u32,
            height: height as u32,
        };
        let mut dst = vec![0u8; width * height * 3];
        yuv420_to_rgb_bilinear_with_siting(
            &frame,
            &mut dst,
            (width * 3) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt601,
            siting,
        )
        .unwrap();

        // Independent expectation: interpolate to the single hand-computed U value and
        // convert as 4:4:4 through the existing path.
        let u444 = vec![u_value; width * height];
        let v444 = vec![128u8; width * height];
        let image444 = YuvPlanarImage {
            y_plane: &y,
            y_stride: width as u32,
            u_plane: &u444,
            u_stride: width as u32,
            v_plane: &v444,
            v_stride: width as u32,
            width: width as u32,
            height: height as u32,
        };
        let mut expected = vec![0u8; width * height * 3];
        yuv444_to_rgb(
            &image444,
            &mut expected,
            (width * 3) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt601,
        )
        .unwrap();

        let pixel = width + 1; // luma pixel (1, 1)
        assert_eq!(
            &dst[pixel * 3..pixel * 3 + 3],
            &expected[pixel * 3..pixel * 3 + 3],
            "siting {siting:?} must first interpolate U to {u_value}"
        );
    }
}

/// Constructs a Center-siting pixel where the full 2D weighting and per-axis rounding
/// disagree by one level. The implementation must follow the single 2D rounding.
#[test]
fn rounding_happens_once_after_full_2d_weighting() {
    // 2x2 chroma grid on a 4x4 frame, inspecting luma pixel (1,1).
    let width = 4usize;
    let height = 4usize;
    let chroma = 2usize;

    let y = vec![128u8; width * height];
    let v = vec![128u8; chroma * chroma];
    // Center weights at luma (1,1) are 9/16, 3/16, 3/16, 1/16 over (00,10,01,11).
    // Taps (128,128,128,134):
    //   2D      -> (128*15 + 134 + 8) >> 4 = 128
    //   per-axis-> row means 128 and 130, then (128*3 + 130 + 2) >> 2 = 129
    let u = vec![128u8, 128, 128, 134];

    let frame = YuvPlanarImage {
        y_plane: &y,
        y_stride: width as u32,
        u_plane: &u,
        u_stride: chroma as u32,
        v_plane: &v,
        v_stride: chroma as u32,
        width: width as u32,
        height: height as u32,
    };
    let mut dst = vec![0u8; width * height * 3];
    yuv420_to_rgb_bilinear_with_siting(
        &frame,
        &mut dst,
        (width * 3) as u32,
        YuvRange::Full,
        YuvStandardMatrix::Bt601,
        YuvChromaSiting::Center,
    )
    .unwrap();

    let u_2d = 128u8;
    let u_per_axis = 129u8;

    let convert = |u_value: u8| -> [u8; 3] {
        let u444 = vec![u_value; width * height];
        let v444 = vec![128u8; width * height];
        let image444 = YuvPlanarImage {
            y_plane: &y,
            y_stride: width as u32,
            u_plane: &u444,
            u_stride: width as u32,
            v_plane: &v444,
            v_stride: width as u32,
            width: width as u32,
            height: height as u32,
        };
        let mut expected = vec![0u8; width * height * 3];
        yuv444_to_rgb(
            &image444,
            &mut expected,
            (width * 3) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt601,
        )
        .unwrap();
        let pixel = width + 1; // luma pixel (1, 1)
        [
            expected[pixel * 3],
            expected[pixel * 3 + 1],
            expected[pixel * 3 + 2],
        ]
    };

    assert_ne!(
        convert(u_2d),
        convert(u_per_axis),
        "fixture must distinguish the rounding rules"
    );
    let pixel = width + 1; // luma pixel (1, 1)
    let got = [dst[pixel * 3], dst[pixel * 3 + 1], dst[pixel * 3 + 2]];
    assert_eq!(got, convert(u_2d), "must use the single 2D rounding result");
    assert_ne!(got, convert(u_per_axis), "must not round per axis");
}

/// Corners and edge pixels exercise edge extension, which a whole-frame average
/// comparison would hide. Corner luma pixels map onto the corner chroma sample under
/// every siting, while neighboring edge pixels expose the half-pixel shift.
#[test]
fn four_corners_follow_hand_computed_taps_and_distinguish_sitings() {
    let width = 4usize;
    let height = 4usize;
    let chroma = 2usize;
    let y = vec![128u8; width * height];
    let v = vec![128u8; chroma * chroma];
    let u_grid = [80u8, 96, 112, 128];

    let frame = YuvPlanarImage {
        y_plane: &y,
        y_stride: width as u32,
        u_plane: &u_grid,
        u_stride: chroma as u32,
        v_plane: &v,
        v_stride: chroma as u32,
        width: width as u32,
        height: height as u32,
    };

    // Hand-computed corner U values: edge extension makes every siting read the single
    // nearest corner sample, so all three sitings agree on [80, 96, 112, 128].
    let corners = [
        ((0usize, 0usize), 80u8),
        ((3, 0), 96),
        ((0, 3), 112),
        ((3, 3), 128),
    ];
    for siting in SITINGS {
        let mut dst = vec![0u8; width * height * 3];
        yuv420_to_rgb_bilinear_with_siting(
            &frame,
            &mut dst,
            (width * 3) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt601,
            siting,
        )
        .unwrap();
        for ((x, y_row), u_value) in corners {
            assert_eq!(
                interpolate_chroma(
                    &u_grid,
                    chroma,
                    chroma,
                    chroma,
                    true,
                    siting,
                    YuvChromaSubsampling::Yuv420,
                    x,
                    y_row
                ),
                u_value,
                "corner ({x},{y_row}) siting {siting:?}"
            );
            let u444 = vec![u_value; width * height];
            let v444 = vec![128u8; width * height];
            let image444 = YuvPlanarImage {
                y_plane: &y,
                y_stride: width as u32,
                u_plane: &u444,
                u_stride: width as u32,
                v_plane: &v444,
                v_stride: width as u32,
                width: width as u32,
                height: height as u32,
            };
            let mut expected = vec![0u8; width * height * 3];
            yuv444_to_rgb(
                &image444,
                &mut expected,
                (width * 3) as u32,
                YuvRange::Full,
                YuvStandardMatrix::Bt601,
            )
            .unwrap();
            let pixel = y_row * width + x;
            assert_eq!(
                &dst[pixel * 3..pixel * 3 + 3],
                &expected[pixel * 3..pixel * 3 + 3],
                "corner ({x},{y_row}) siting {siting:?}"
            );
        }
    }

    // On the top edge one step inside, horizontal siting moves the value:
    // Center (weights 3/4, 1/4) -> (80*3 + 96 + 2) >> 2 = 84,
    // Left/TopLeft (weights 1/2, 1/2) -> (80 + 96 + 1) >> 1 = 88.
    let top_edge_center = interpolate_chroma(
        &u_grid,
        chroma,
        chroma,
        chroma,
        true,
        YuvChromaSiting::Center,
        YuvChromaSubsampling::Yuv420,
        1,
        0,
    );
    let top_edge_left = interpolate_chroma(
        &u_grid,
        chroma,
        chroma,
        chroma,
        true,
        YuvChromaSiting::Left,
        YuvChromaSubsampling::Yuv420,
        1,
        0,
    );
    assert_eq!((top_edge_center, top_edge_left), (84, 88));

    // On the left edge one row down, horizontal weights collapse onto sample column 0;
    // only vertical siting moves the value:
    // Left (vertical weights 3/4, 1/4) -> (80*3 + 112 + 2) >> 2 = 88,
    // TopLeft (weights 1/2, 1/2) -> (80 + 112 + 1) >> 1 = 96.
    let left_edge_left = interpolate_chroma(
        &u_grid,
        chroma,
        chroma,
        chroma,
        true,
        YuvChromaSiting::Left,
        YuvChromaSubsampling::Yuv420,
        0,
        1,
    );
    let left_edge_top_left = interpolate_chroma(
        &u_grid,
        chroma,
        chroma,
        chroma,
        true,
        YuvChromaSiting::TopLeft,
        YuvChromaSubsampling::Yuv420,
        0,
        1,
    );
    assert_eq!((left_edge_left, left_edge_top_left), (88, 96));
}

/// Width 35 crosses common 16/32-byte SIMD block boundaries; the scalar tail and the
/// vectorized body must honor the same siting convention.
#[test]
fn simd_block_boundary_width_uses_one_siting_convention() {
    for sampling in [YuvChromaSubsampling::Yuv420, YuvChromaSubsampling::Yuv422] {
        let width = 35usize;
        let height = 6usize;
        let frame = build_frame(width, height, sampling, 0xDEAD_BEEF, true);
        for siting in SITINGS {
            for layout in LAYOUTS {
                let channels = layout.channels();
                let dst_stride = width * channels + 4;
                let mut dst = vec![0x33u8; dst_stride * (height - 1) + width * channels];
                siting_fn(sampling, layout)(
                    &frame.image(),
                    &mut dst,
                    dst_stride as u32,
                    YuvRange::Limited,
                    YuvStandardMatrix::Bt709,
                    siting,
                )
                .unwrap();
                let expected = reference_via_444(
                    &frame.y,
                    frame.y_stride,
                    &frame.u,
                    frame.u_stride,
                    &frame.v,
                    frame.v_stride,
                    width,
                    height,
                    sampling,
                    siting,
                    layout,
                    YuvRange::Limited,
                    YuvStandardMatrix::Bt709,
                );
                for row in 0..height {
                    let got = &dst[row * dst_stride..row * dst_stride + width * channels];
                    let want = &expected[row * width * channels..(row + 1) * width * channels];
                    assert_eq!(
                        got, want,
                        "boundary frame sampling {sampling:?} siting {siting:?} layout {layout:?} row {row}"
                    );
                    // Explicitly re-check the tail columns against independent tap weights.
                    for x in (width - 3)..width {
                        let pixel = &got[x * channels..(x + 1) * channels];
                        let want_pixel = &want[x * channels..(x + 1) * channels];
                        assert_eq!(pixel, want_pixel, "tail pixel {x} row {row}");
                    }
                }
            }
        }
    }
}

/// Invalid planes or strides must be rejected before the destination is touched.
#[test]
fn invalid_arguments_leave_destination_untouched() {
    let width = 4usize;
    let height = 4usize;
    let chroma = 2usize;
    let y = vec![128u8; width * height];
    let u = vec![128u8; chroma * chroma];
    let v = vec![128u8; chroma * chroma];
    let frame = YuvPlanarImage {
        y_plane: &y,
        y_stride: width as u32,
        u_plane: &u,
        u_stride: chroma as u32,
        v_plane: &v,
        v_stride: chroma as u32,
        width: width as u32,
        height: height as u32,
    };

    let fns_420: [SitingFn; 4] = [
        yuv420_to_rgb_bilinear_with_siting,
        yuv420_to_bgr_bilinear_with_siting,
        yuv420_to_rgba_bilinear_with_siting,
        yuv420_to_bgra_bilinear_with_siting,
    ];
    let fns_422: [SitingFn; 4] = [
        yuv422_to_rgb_bilinear_with_siting,
        yuv422_to_bgr_bilinear_with_siting,
        yuv422_to_rgba_bilinear_with_siting,
        yuv422_to_bgra_bilinear_with_siting,
    ];

    for (convert, channels) in fns_420
        .iter()
        .chain(fns_422.iter())
        .zip([3usize, 3, 4, 4, 3, 3, 4, 4])
    {
        let sentinel = 0xA5u8;

        // Destination buffer shorter than the frame requires (a one-byte buffer fails
        // every layout's minimum size).
        let mut dst = vec![sentinel; 1];
        let err = convert(
            &frame,
            &mut dst,
            (width * channels) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt601,
            YuvChromaSiting::Center,
        );
        assert!(matches!(err, Err(YuvError::DestinationSizeMismatch(_))));
        assert!(dst.iter().all(|&b| b == sentinel), "dst modified on error");

        // Stride below the minimum row width.
        let mut dst = vec![sentinel; width * height * channels];
        let err = convert(
            &frame,
            &mut dst,
            (width * channels - 1) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt601,
            YuvChromaSiting::Center,
        );
        assert!(matches!(err, Err(YuvError::MinimumStrideSizeMismatch(_))));
        assert!(dst.iter().all(|&b| b == sentinel), "dst modified on error");
    }

    // Chroma plane shorter than the frame requires.
    let short_u = vec![128u8; chroma * chroma - 1];
    let bad_frame = YuvPlanarImage {
        y_plane: &y,
        y_stride: width as u32,
        u_plane: &short_u,
        u_stride: chroma as u32,
        v_plane: &v,
        v_stride: chroma as u32,
        width: width as u32,
        height: height as u32,
    };
    let mut dst = vec![0xA5u8; width * height * 4];
    let err = yuv420_to_rgba_bilinear_with_siting(
        &bad_frame,
        &mut dst,
        (width * 4) as u32,
        YuvRange::Full,
        YuvStandardMatrix::Bt601,
        YuvChromaSiting::Center,
    );
    assert!(err.is_err());
    assert!(dst.iter().all(|&b| b == 0xA5), "dst modified on error");
}

/// Input and output row padding must survive the conversion untouched.
#[test]
fn row_padding_is_preserved() {
    let width = 7usize;
    let height = 5usize;
    let frame = build_frame(
        width,
        height,
        YuvChromaSubsampling::Yuv420,
        0x1234_5678,
        true,
    );
    let channels = 4usize;
    let dst_stride = width * channels + 6;
    let mut dst = vec![0x91u8; dst_stride * (height - 1) + width * channels];

    yuv420_to_bgra_bilinear_with_siting(
        &frame.image(),
        &mut dst,
        dst_stride as u32,
        YuvRange::Limited,
        YuvStandardMatrix::Bt709,
        YuvChromaSiting::Left,
    )
    .unwrap();

    for row in 0..height - 1 {
        assert!(
            dst[row * dst_stride + width * channels..(row + 1) * dst_stride]
                .iter()
                .all(|&b| b == 0x91)
        );
    }
    // Input planes keep their own padding sentinels. The last row of a minimum-size
    // plane has no trailing bytes, so only rows followed by another row are checked.
    let cw = width.div_ceil(2);
    let ch = height.div_ceil(2);
    for row in 0..ch - 1 {
        assert!(
            frame.u[row * frame.u_stride + cw..(row + 1) * frame.u_stride]
                .iter()
                .all(|&b| b == 0xAB)
        );
        assert!(
            frame.v[row * frame.v_stride + cw..(row + 1) * frame.v_stride]
                .iter()
                .all(|&b| b == 0xEF)
        );
    }
    for row in 0..height - 1 {
        assert!(
            frame.y[row * frame.y_stride + width..(row + 1) * frame.y_stride]
                .iter()
                .all(|&b| b == 0xCD)
        );
    }
}

/// Fixed byte snapshots pin the legacy entry points: their signatures and numerical
/// behavior must not change when the siting-aware entry points are added.
#[test]
fn legacy_bilinear_entry_points_keep_fixed_bytes_420() {
    let w = 6usize;
    let h = 4usize;
    let cw = w.div_ceil(2);
    let ch = h.div_ceil(2);
    let y: Vec<u8> = (0..w * h)
        .map(|i| 40u8.wrapping_add(((i % w) * 7 + (i / w) * 13) as u8))
        .collect();
    let u: Vec<u8> = (0..cw * ch)
        .map(|i| 90u8.wrapping_add(((i % cw) * 11 + (i / cw) * 17) as u8))
        .collect();
    let v: Vec<u8> = (0..cw * ch)
        .map(|i| 160u8.wrapping_sub(((i % cw) * 5 + (i / cw) * 13) as u8))
        .collect();
    let image = YuvPlanarImage {
        y_plane: &y,
        y_stride: w as u32,
        u_plane: &u,
        u_stride: cw as u32,
        v_plane: &v,
        v_stride: cw as u32,
        width: w as u32,
        height: h as u32,
    };
    let mut rgba = vec![0u8; w * h * 4];
    yuv420_to_rgba_bilinear(
        &image,
        &mut rgba,
        (w * 4) as u32,
        YuvRange::Full,
        YuvStandardMatrix::Bt601,
    )
    .unwrap();
    // Generated once from the legacy path and recorded verbatim (identical under the
    // default and no-default-feature builds).
    const SNAPSHOT_420: [u8; 96] = [
        79, 31, 0, 255, 82, 38, 3, 255, 86, 44, 19, 255, 89, 52, 36, 255, 95, 59, 47, 255, 102, 66,
        54, 255, 82, 46, 14, 255, 87, 52, 30, 255, 89, 59, 48, 255, 94, 66, 63, 255, 98, 73, 76,
        255, 105, 80, 83, 255, 91, 59, 34, 255, 94, 67, 50, 255, 98, 73, 68, 255, 101, 81, 83, 255,
        107, 87, 96, 255, 114, 94, 103, 255, 104, 72, 47, 255, 107, 80, 63, 255, 111, 86, 81, 255,
        114, 94, 96, 255, 120, 100, 109, 255, 127, 107, 116, 255,
    ];
    assert_eq!(rgba, SNAPSHOT_420);
}

#[test]
fn legacy_bilinear_entry_points_keep_fixed_bytes_422() {
    let w = 6usize;
    let h = 4usize;
    let cw = w.div_ceil(2);
    let y: Vec<u8> = (0..w * h)
        .map(|i| 40u8.wrapping_add(((i % w) * 7 + (i / w) * 13) as u8))
        .collect();
    let u: Vec<u8> = (0..cw * h)
        .map(|i| 90u8.wrapping_add(((i % cw) * 11 + (i / cw) * 17) as u8))
        .collect();
    let v: Vec<u8> = (0..cw * h)
        .map(|i| 160u8.wrapping_sub(((i % cw) * 5 + (i / cw) * 13) as u8))
        .collect();
    let image = YuvPlanarImage {
        y_plane: &y,
        y_stride: w as u32,
        u_plane: &u,
        u_stride: cw as u32,
        v_plane: &v,
        v_stride: cw as u32,
        width: w as u32,
        height: h as u32,
    };
    let mut rgba = vec![0u8; w * h * 4];
    yuv422_to_rgba_bilinear(
        &image,
        &mut rgba,
        (w * 4) as u32,
        YuvRange::Full,
        YuvStandardMatrix::Bt601,
    )
    .unwrap();
    const SNAPSHOT_422: [u8; 96] = [
        83, 30, 0, 255, 86, 37, 0, 255, 90, 44, 11, 255, 93, 51, 27, 255, 99, 58, 40, 255, 106, 65,
        47, 255, 78, 46, 21, 255, 81, 54, 37, 255, 85, 60, 55, 255, 88, 68, 70, 255, 94, 74, 83,
        255, 101, 81, 90, 255, 73, 63, 64, 255, 76, 70, 80, 255, 80, 77, 98, 255, 83, 84, 114, 255,
        88, 91, 126, 255, 95, 98, 133, 255, 68, 79, 107, 255, 71, 87, 123, 255, 75, 93, 141, 255,
        78, 100, 157, 255, 83, 107, 169, 255, 90, 114, 176, 255,
    ];
    assert_eq!(rgba, SNAPSHOT_422);
}

/// The siting parameter must actually move chroma: the three interpretations of one frame
/// cannot all produce the same bytes.
#[test]
fn different_sitings_produce_different_outputs() {
    let width = 8usize;
    let height = 8usize;
    let frame = build_frame(width, height, YuvChromaSubsampling::Yuv420, 0xABCDEF, false);
    let mut outputs = Vec::new();
    for siting in SITINGS {
        let mut dst = vec![0u8; width * height * 3];
        yuv420_to_rgb_bilinear_with_siting(
            &frame.image(),
            &mut dst,
            (width * 3) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt709,
            siting,
        )
        .unwrap();
        outputs.push(dst);
    }
    assert_ne!(outputs[0], outputs[1], "Center and Left must differ");
    assert_ne!(outputs[1], outputs[2], "Left and TopLeft must differ");
    assert_ne!(outputs[0], outputs[2], "Center and TopLeft must differ");

    // 4:2:2 only carries a horizontal offset, so Left and TopLeft (both sx = 0) must be
    // byte-identical while Center (sx = 0.5) has to move the chroma relative to both.
    let frame422 = build_frame(width, height, YuvChromaSubsampling::Yuv422, 0xABCDEF, false);
    let mut outputs422 = Vec::new();
    for siting in SITINGS {
        let mut dst = vec![0u8; width * height * 3];
        yuv422_to_rgb_bilinear_with_siting(
            &frame422.image(),
            &mut dst,
            (width * 3) as u32,
            YuvRange::Full,
            YuvStandardMatrix::Bt709,
            siting,
        )
        .unwrap();
        outputs422.push(dst);
    }
    assert_eq!(
        outputs422[1], outputs422[2],
        "4:2:2 Left and TopLeft share sx = 0 and must agree"
    );
    assert_ne!(
        outputs422[0], outputs422[1],
        "4:2:2 Center must differ from Left"
    );
}

/// Compile-time visibility check that the public surface is exported from the crate root.
#[test]
fn public_surface_is_exported() {
    let _ = yuv420_to_rgb_bilinear_with_siting as SitingFn;
    let _ = yuv420_to_bgr_bilinear_with_siting as SitingFn;
    let _ = yuv420_to_rgba_bilinear_with_siting as SitingFn;
    let _ = yuv420_to_bgra_bilinear_with_siting as SitingFn;
    let _ = yuv422_to_rgb_bilinear_with_siting as SitingFn;
    let _ = yuv422_to_bgr_bilinear_with_siting as SitingFn;
    let _ = yuv422_to_rgba_bilinear_with_siting as SitingFn;
    let _ = yuv422_to_bgra_bilinear_with_siting as SitingFn;
    let _ = YuvChromaSiting::Center;
    let _ = YuvChromaSiting::Left;
    let _ = YuvChromaSiting::TopLeft;
    // Legacy entry points remain present alongside the new ones.
    let _ = yuv420_to_rgb_bilinear;
    let _ = yuv420_to_bgr_bilinear;
    let _ = yuv420_to_rgba_bilinear;
    let _ = yuv420_to_bgra_bilinear;
    let _ = yuv422_to_rgb_bilinear;
    let _ = yuv422_to_bgr_bilinear;
    let _ = yuv422_to_rgba_bilinear;
    let _ = yuv422_to_bgra_bilinear;
}
