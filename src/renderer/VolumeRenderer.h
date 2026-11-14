/*
MIT License

Copyright (c) 2017-2020 Markus Trenkwalder

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

/** @file */

#include <cstddef>
#include <vector>

#include "Rasterizer.h"

namespace swr {

namespace volumedetail {

/// Per-cell constant data owned by a capture.
struct CellData {
	double sigma;
	double j[3];
	double planeN[4][3]; ///< Outward unit face normals.
	double planeP[4][3]; ///< One vertex of each face.
};

/// One ray interval of a cell inside the frustum, distances in view-space
/// Euclidean units from the camera.
struct Interval {
	unsigned cell;
	float enter;
	float exit;
};

} // end namespace volumedetail

/// Off-screen preview renderer for emissive, absorbing volumes made of
/// tetrahedral cells, built on top of the regular software pipeline
/// (VertexProcessor homogeneous clipping + Rasterizer triangle path).
///
/// The renderer works in two steps:
///
///   1. capture():  the boundary faces of every tetrahedral cell are drawn
///      double-sided through the real VertexProcessor/Rasterizer pipeline.
///      Per pixel and per cell the enter/exit crossings along the pixel
///      center ray are recovered from the rasterized fragments and reduced
///      to a set of ray intervals (in view-space Euclidean distance from the
///      camera). The capture owns all data needed later on; the caller may
///      modify or free the cell array afterwards.
///
///   2. composite(): read-only with respect to the capture. The captured
///      intervals are truncated against the unprojected opaque depth of a
///      base image, all interval endpoints are sorted (endpoints at equal
///      distance are applied as one group, so the cell submission order can
///      not influence the result), and the medium is integrated
///      analytically segment by segment from near to far:
///
///          C += T * j * (1 - exp(-sigma * L)) / sigma
///          T *= exp(-sigma * L)              (sigma > 0)
///          C += T * j * L                    (sigma == 0)
///
///      where sigma and j are the sums of the extinction/emission
///      coefficients of all cells active on the segment of length L.
///      Finally C += T * baseRgb is added. No gamma or byte quantization is
///      applied. An empty volume reproduces the base image exactly with
///      transmittance 1.
///
/// Coordinate system and matrices:
///   * View space: camera at the origin looking down -Z, x to the right,
///     y down in image space (the library viewport convention).
///   * The projection is a finite, regular perspective matrix stored
///     column-major (proj[4*col + row]) with clip = P * view and
///     clip.w = -view.z, mapping NDC z to [-1, 1]. Asymmetric frusta are
///     supported.
///   * The viewport origin may be non-zero; pixel (0,0) of the base image
///     and of the result corresponds to the top-left viewport corner
///     (x right, y down).
///   * The base image is viewport-sized linear RGB (3 floats per pixel)
///     plus an opaque depth value in [0, 1] per pixel (0 = near plane,
///     1 = far plane/background; exactly 1 means no opaque surface).
///
/// All interval lengths are view-space Euclidean distances along the pixel
/// center ray, never screen-space depth or z differences.
///
/// Error handling: capture() validates cells, projection and viewport up
/// front and throws std::invalid_argument on illegal input; in that case
/// the previous valid capture is kept untouched. composite() throws
/// std::invalid_argument for null base image pointers and std::logic_error
/// when no successful capture exists yet.
///
/// The class is not thread-safe and capture() is not reentrant; separate
/// instances may be interleaved freely at call granularity.
class VolumeRenderer {
public:
	/// A single tetrahedral cell with constant interior coefficients.
	struct TetCell {
		float v[4][3]; ///< View-space vertex positions. Must be finite and non-coplanar.
		float sigma;   ///< Extinction coefficient, must be finite and >= 0.
		float j[3];    ///< Emission coefficient (linear RGB), finite and >= 0.
	};

	/// Viewport rectangle in framebuffer pixels. The origin may be non-zero.
	struct Viewport {
		int x = 0;      ///< Left edge in pixels (x grows right).
		int y = 0;      ///< Top edge in pixels (y grows down).
		int width = 0;  ///< Width in pixels, must be > 0.
		int height = 0; ///< Height in pixels, must be > 0.
	};

	/// Owned compositing result for the captured viewport.
	struct Image {
		int width = 0;  ///< Captured viewport width.
		int height = 0; ///< Captured viewport height.
		/// Linear RGB, width*height*3 floats, row-major, top row first.
		std::vector<float> rgb;
		/// Remaining transmittance, width*height floats.
		std::vector<float> transmittance;
	};

	VolumeRenderer();

	/// Select the rasterizer mode used by capture(). Default is RasterMode::Span.
	void setRasterMode(RasterMode mode);

	/// The rasterizer mode used by capture().
	RasterMode rasterMode() const { return m_rasterMode; }

	/// Capture a set of tetrahedral cells.
	/**
	  * \param cells     Array of cells; may be null when cellCount == 0. The
	  *                  data is copied; the caller keeps ownership.
	  * \param cellCount Number of cells. 0 captures an empty volume.
	  * \param proj      Column-major 4x4 perspective projection, clip = P * view,
	  *                  clip.w = -view.z, NDC z in [-1, 1].
	  * \param viewport  Viewport rectangle; width/height must be positive.
	  *
	  * Cells may overlap and share faces. Every cell boundary face is drawn
	  * double-sided through the real VertexProcessor/Rasterizer pipeline and
	  * the enter/exit events are recovered per pixel with the cell identity
	  * preserved.
	  *
	  * Throws std::invalid_argument on any invalid input (non-finite or
	  * coplanar cell vertices, negative/non-finite coefficients, null array
	  * with non-zero count, non-perspective or singular projection,
	  * non-positive viewport size). On failure the previous valid capture is
	  * preserved. On success the previous capture is replaced.
	  */
	void capture(const TetCell *cells, std::size_t cellCount, const float proj[16], const Viewport &viewport);

	/// Composite the captured volume over a base image.
	/**
	  * \param baseRgb   Viewport-sized linear RGB, 3 floats per pixel.
	  * \param baseDepth Viewport-sized opaque depth in [0, 1]; 1 means
	  *                  background (no opaque surface). Depth is unprojected
	  *                  with the captured projection and truncates the volume
	  *                  intervals before integration.
	  *
	  * May be called repeatedly on the same capture with different base
	  * images. Does not modify the capture. Returns an independently owned
	  * Image. With an empty capture the base image is preserved exactly and
	  * the transmittance is exactly 1.
	  */
	Image composite(const float *baseRgb, const float *baseDepth) const;

	/// True after the first successful capture().
	bool hasCapture() const { return m_hasCapture; }

	/// Number of cells in the current capture.
	std::size_t capturedCellCount() const { return m_cells.size(); }

private:
	using CellData = volumedetail::CellData;
	using Interval = volumedetail::Interval;

	RasterMode m_rasterMode = RasterMode::Span;

	// Capture-owned data.
	bool m_hasCapture = false;
	std::vector<CellData> m_cells;
	std::vector<Interval> m_intervals;      ///< Flat per-pixel interval storage.
	std::vector<unsigned> m_intervalOffset; ///< size w*h+1 offsets into m_intervals.
	int m_vpX = 0, m_vpY = 0, m_vpW = 0, m_vpH = 0; ///< Captured viewport.
	double m_invProj[16] = {}; ///< Inverse of the captured projection (column-major).
};

} // end namespace swr
