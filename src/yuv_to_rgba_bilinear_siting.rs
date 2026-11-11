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
use crate::yuv_error::check_rgba_destination;
use crate::yuv_support::*;
use crate::{
    yuv444_to_bgr, yuv444_to_bgra, yuv444_to_rgb, yuv444_to_rgba, YuvChromaSiting, YuvError,
    YuvPlanarImage,
};

/// Position of a luma pixel on a chroma axis, expressed in quarter chroma samples.
///
/// A chroma sample sits at luma coordinate `2i + s`, so luma pixel `p` maps to chroma
/// grid coordinate `(p - s) / 2`, i.e. quarter units `q = 2p - 2s` where `2s` is `1`
/// for a half-luma-pixel offset and `0` otherwise. `base` is the floor grid index,
/// `fraction` is the sub-grid position in `0..4` (0, 1, 2, 3 = 0, 0.25, 0.5, 0.75).
struct ChromaCoord {
    base: isize,
    fraction: usize,
}

#[inline(always)]
fn chroma_coord(luma: usize, half_luma_offset: u32) -> ChromaCoord {
    let q = 2 * luma as isize - half_luma_offset as isize;
    ChromaCoord {
        base: q.div_euclid(4),
        fraction: q.rem_euclid(4) as usize,
    }
}

#[inline(always)]
fn clamp_index(index: isize, len_minus_one: usize) -> usize {
    index.clamp(0, len_minus_one as isize) as usize
}

/// Builds siting-interpolated 4:4:4 chroma planes.
///
/// U and V keep their full fractional precision until both axes have been combined;
/// each is rounded to nearest exactly once, halves going up. Taps outside the valid
/// chroma extent extend the nearest valid sample, so stride padding is never read.
fn interpolate_chroma_444(
    u444: &mut [u8],
    v444: &mut [u8],
    image: &YuvPlanarImage<u8>,
    sampling: YuvChromaSubsampling,
    siting: YuvChromaSiting,
) {
    let width = image.width as usize;
    let height = image.height as usize;
    let chroma_width = width.div_ceil(2);
    let last_chroma_x = chroma_width - 1;
    let x_offset = siting.x_half_offset();
    let u_stride = image.u_stride as usize;
    let v_stride = image.v_stride as usize;

    if sampling == YuvChromaSubsampling::Yuv422 {
        // 4:2:2 chroma rows match luma rows exactly, only horizontal interpolation.
        for y in 0..height {
            let u_row = &image.u_plane[y * u_stride..y * u_stride + chroma_width];
            let v_row = &image.v_plane[y * v_stride..y * v_stride + chroma_width];
            for x in 0..width {
                let coord = chroma_coord(x, x_offset);
                let ix0 = clamp_index(coord.base, last_chroma_x);
                let ix1 = clamp_index(coord.base + 1, last_chroma_x);
                let wx0 = (4 - coord.fraction) as i32;
                let wx1 = coord.fraction as i32;
                let dst = y * width + x;
                // Two taps, weights sum to 4, single nearest-integer rounding.
                u444[dst] = ((u_row[ix0] as i32 * wx0 + u_row[ix1] as i32 * wx1 + 2) >> 2) as u8;
                v444[dst] = ((v_row[ix0] as i32 * wx0 + v_row[ix1] as i32 * wx1 + 2) >> 2) as u8;
            }
        }
    } else {
        let chroma_height = height.div_ceil(2);
        let last_chroma_y = chroma_height - 1;
        let y_offset = siting.y_half_offset();

        for y in 0..height {
            let y_coord = chroma_coord(y, y_offset);
            let iy0 = clamp_index(y_coord.base, last_chroma_y);
            let iy1 = clamp_index(y_coord.base + 1, last_chroma_y);
            let wy0 = (4 - y_coord.fraction) as i32;
            let wy1 = y_coord.fraction as i32;
            let u_row0 = &image.u_plane[iy0 * u_stride..iy0 * u_stride + chroma_width];
            let u_row1 = &image.u_plane[iy1 * u_stride..iy1 * u_stride + chroma_width];
            let v_row0 = &image.v_plane[iy0 * v_stride..iy0 * v_stride + chroma_width];
            let v_row1 = &image.v_plane[iy1 * v_stride..iy1 * v_stride + chroma_width];

            for x in 0..width {
                let coord = chroma_coord(x, x_offset);
                let ix0 = clamp_index(coord.base, last_chroma_x);
                let ix1 = clamp_index(coord.base + 1, last_chroma_x);
                let wx0 = (4 - coord.fraction) as i32;
                let wx1 = coord.fraction as i32;
                let dst = y * width + x;
                // Four taps, weights sum to 16, single nearest-integer rounding after
                // the full 2D weighting; no per-axis rounding is allowed.
                let u_sum = u_row0[ix0] as i32 * wx0 * wy0
                    + u_row0[ix1] as i32 * wx1 * wy0
                    + u_row1[ix0] as i32 * wx0 * wy1
                    + u_row1[ix1] as i32 * wx1 * wy1;
                let v_sum = v_row0[ix0] as i32 * wx0 * wy0
                    + v_row0[ix1] as i32 * wx1 * wy0
                    + v_row1[ix0] as i32 * wx0 * wy1
                    + v_row1[ix1] as i32 * wx1 * wy1;
                u444[dst] = ((u_sum + 8) >> 4) as u8;
                v444[dst] = ((v_sum + 8) >> 4) as u8;
            }
        }
    }
}

fn yuv_to_rgbx_with_siting_impl<const DESTINATION_CHANNELS: u8, const SAMPLING: u8>(
    image: &YuvPlanarImage<u8>,
    rgba: &mut [u8],
    rgba_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    let chroma_subsampling: YuvChromaSubsampling = SAMPLING.into();
    assert!(
        chroma_subsampling == YuvChromaSubsampling::Yuv420
            || chroma_subsampling == YuvChromaSubsampling::Yuv422
    );
    let dst_chans: YuvSourceChannels = DESTINATION_CHANNELS.into();
    let channels = dst_chans.get_channels_count();

    // Reject everything before touching the destination so an invalid frame leaves it intact.
    check_rgba_destination(rgba, rgba_stride, image.width, image.height, channels)?;
    image.check_constraints(chroma_subsampling)?;

    let width = image.width as usize;
    let height = image.height as usize;

    // Interpolate the chroma planes to 4:4:4 first. Luma keeps its original positions and
    // is passed through untouched; the color stage then runs the library's regular 8-bit
    // range/matrix conversion, including the same clipping and SIMD paths as yuv444_*.
    let mut u444 = vec![0u8; width * height];
    let mut v444 = vec![0u8; width * height];
    interpolate_chroma_444(&mut u444, &mut v444, image, chroma_subsampling, siting);

    let image444 = YuvPlanarImage {
        y_plane: image.y_plane,
        y_stride: image.y_stride,
        u_plane: &u444,
        u_stride: image.width,
        v_plane: &v444,
        v_stride: image.width,
        width: image.width,
        height: image.height,
    };

    match dst_chans {
        YuvSourceChannels::Rgb => yuv444_to_rgb(&image444, rgba, rgba_stride, range, matrix),
        YuvSourceChannels::Bgr => yuv444_to_bgr(&image444, rgba, rgba_stride, range, matrix),
        YuvSourceChannels::Rgba => yuv444_to_rgba(&image444, rgba, rgba_stride, range, matrix),
        YuvSourceChannels::Bgra => yuv444_to_bgra(&image444, rgba, rgba_stride, range, matrix),
    }
}

/// Convert YUV 4:2:0 planar 8-bit data to RGB with explicit chroma siting.
///
/// Unlike the plain `bilinear` entry points this honors the chroma sample position
/// ([`YuvChromaSiting`]) signaled by the decoder. Luma pixel centers are integer
/// coordinates `(x, y)`: for 4:2:0 chroma sample `(i, j)` sits at `(2i + sx, 2j + sy)`.
///
/// Each output pixel maps its coordinate back onto the chroma grid and bilinearly
/// interpolates the four neighboring chroma samples. Positions outside the valid chroma
/// extent extend the nearest valid sample, so row padding is never read. The full 2D
/// weighting is accumulated before U and V are each rounded once to the nearest integer,
/// halves going up; the interpolated chroma is then converted with the same 8-bit
/// range/matrix transform and clipping as the plain 4:4:4 conversions. Luma is always
/// sampled at its original position.
///
/// Only progressive (frame) 8-bit planar images are supported; interlaced fields and
/// 10-bit formats are not covered. Stride padding of both input planes and the
/// destination is left untouched.
pub fn yuv420_to_rgb_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    rgb: &mut [u8],
    rgb_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Rgb as u8 },
        { YuvChromaSubsampling::Yuv420 as u8 },
    >(planar_image, rgb, rgb_stride, range, matrix, siting)
}

/// Convert YUV 4:2:0 planar 8-bit data to BGR with explicit chroma siting.
///
/// See [`yuv420_to_rgb_bilinear_with_siting`] for the siting and interpolation semantics.
pub fn yuv420_to_bgr_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    bgr: &mut [u8],
    bgr_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Bgr as u8 },
        { YuvChromaSubsampling::Yuv420 as u8 },
    >(planar_image, bgr, bgr_stride, range, matrix, siting)
}

/// Convert YUV 4:2:0 planar 8-bit data to RGBA with explicit chroma siting.
///
/// See [`yuv420_to_rgb_bilinear_with_siting`] for the siting and interpolation semantics.
/// The alpha channel is written as `255`.
///
/// # Examples
///
/// A decoded video frame entering the preview pipeline carries its chroma siting
/// alongside the plane pointers:
///
/// ```no_run
/// use yuv::{
///     yuv420_to_rgba_bilinear_with_siting, YuvChromaSiting, YuvPlanarImage, YuvRange,
///     YuvStandardMatrix,
/// };
///
/// // Plane pointers and strides produced by the video decoder, together with the
/// // chroma siting advertised for this frame.
/// let (y_plane, u_plane, v_plane): (Vec<u8>, Vec<u8>, Vec<u8>) =
///     (vec![16u8; 64], vec![128u8; 16], vec![128u8; 16]);
/// let (width, height) = (8u32, 8u32);
/// let siting = YuvChromaSiting::Left;
///
/// let frame = YuvPlanarImage {
///     y_plane: &y_plane,
///     y_stride: width,
///     u_plane: &u_plane,
///     u_stride: width / 2,
///     v_plane: &v_plane,
///     v_stride: width / 2,
///     width,
///     height,
/// };
/// let mut rgba = vec![0u8; width as usize * height as usize * 4];
/// yuv420_to_rgba_bilinear_with_siting(
///     &frame,
///     &mut rgba,
///     width * 4,
///     YuvRange::Limited,
///     YuvStandardMatrix::Bt709,
///     siting,
/// )
/// .unwrap();
/// ```
pub fn yuv420_to_rgba_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    rgba: &mut [u8],
    rgba_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Rgba as u8 },
        { YuvChromaSubsampling::Yuv420 as u8 },
    >(planar_image, rgba, rgba_stride, range, matrix, siting)
}

/// Convert YUV 4:2:0 planar 8-bit data to BGRA with explicit chroma siting.
///
/// See [`yuv420_to_rgb_bilinear_with_siting`] for the siting and interpolation semantics.
/// The alpha channel is written as `255`.
pub fn yuv420_to_bgra_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    bgra: &mut [u8],
    bgra_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Bgra as u8 },
        { YuvChromaSubsampling::Yuv420 as u8 },
    >(planar_image, bgra, bgra_stride, range, matrix, siting)
}

/// Convert YUV 4:2:2 planar 8-bit data to RGB with explicit chroma siting.
///
/// Unlike the plain `bilinear` entry points this honors the chroma sample position
/// ([`YuvChromaSiting`]) signaled by the decoder. Luma pixel centers are integer
/// coordinates `(x, y)`: for 4:2:2 chroma sample `(i, j)` sits at `(2i + sx, j)`, so only
/// horizontal interpolation is performed.
///
/// Each output pixel maps its coordinate back onto the chroma grid and bilinearly
/// interpolates the two neighboring chroma samples of its own chroma row. Positions
/// outside the valid chroma extent extend the nearest valid sample, so row padding is
/// never read. The weighting is accumulated before U and V are each rounded once to the
/// nearest integer, halves going up; the interpolated chroma is then converted with the
/// same 8-bit range/matrix transform and clipping as the plain 4:4:4 conversions. Luma
/// is always sampled at its original position.
///
/// Only progressive (frame) 8-bit planar images are supported; interlaced fields and
/// 10-bit formats are not covered. Stride padding of both input planes and the
/// destination is left untouched.
pub fn yuv422_to_rgb_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    rgb: &mut [u8],
    rgb_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Rgb as u8 },
        { YuvChromaSubsampling::Yuv422 as u8 },
    >(planar_image, rgb, rgb_stride, range, matrix, siting)
}

/// Convert YUV 4:2:2 planar 8-bit data to BGR with explicit chroma siting.
///
/// See [`yuv422_to_rgb_bilinear_with_siting`] for the siting and interpolation semantics.
pub fn yuv422_to_bgr_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    bgr: &mut [u8],
    bgr_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Bgr as u8 },
        { YuvChromaSubsampling::Yuv422 as u8 },
    >(planar_image, bgr, bgr_stride, range, matrix, siting)
}

/// Convert YUV 4:2:2 planar 8-bit data to RGBA with explicit chroma siting.
///
/// See [`yuv422_to_rgb_bilinear_with_siting`] for the siting and interpolation semantics.
/// The alpha channel is written as `255`.
pub fn yuv422_to_rgba_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    rgba: &mut [u8],
    rgba_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Rgba as u8 },
        { YuvChromaSubsampling::Yuv422 as u8 },
    >(planar_image, rgba, rgba_stride, range, matrix, siting)
}

/// Convert YUV 4:2:2 planar 8-bit data to BGRA with explicit chroma siting.
///
/// See [`yuv422_to_rgb_bilinear_with_siting`] for the siting and interpolation semantics.
/// The alpha channel is written as `255`.
pub fn yuv422_to_bgra_bilinear_with_siting(
    planar_image: &YuvPlanarImage<u8>,
    bgra: &mut [u8],
    bgra_stride: u32,
    range: YuvRange,
    matrix: YuvStandardMatrix,
    siting: YuvChromaSiting,
) -> Result<(), YuvError> {
    yuv_to_rgbx_with_siting_impl::<
        { YuvSourceChannels::Bgra as u8 },
        { YuvChromaSubsampling::Yuv422 as u8 },
    >(planar_image, bgra, bgra_stride, range, matrix, siting)
}
