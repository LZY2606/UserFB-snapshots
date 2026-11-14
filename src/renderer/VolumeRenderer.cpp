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

#include "VolumeRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "VertexProcessor.h"
#include "VertexShaderBase.h"
#include "PixelShaderBase.h"

namespace swr {

namespace {

// ---------------------------------------------------------------------------
// Small double precision helpers. Matrices are 4x4 column-major.
// ---------------------------------------------------------------------------

/// Invert a 4x4 matrix (column-major) with Gauss-Jordan elimination.
/// Returns false for singular matrices.
bool invert4(const double m[16], double inv[16])
{
	double a[4][8];
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c)
			a[r][c] = m[c * 4 + r];
		for (int c = 0; c < 4; ++c)
			a[r][4 + c] = (c == r) ? 1.0 : 0.0;
	}
	for (int col = 0; col < 4; ++col) {
		int piv = col;
		for (int r = col + 1; r < 4; ++r)
			if (std::fabs(a[r][col]) > std::fabs(a[piv][col]))
				piv = r;
		if (a[piv][col] == 0.0)
			return false;
		if (piv != col)
			for (int c = 0; c < 8; ++c)
				std::swap(a[piv][c], a[col][c]);
		double d = a[col][col];
		for (int c = 0; c < 8; ++c)
			a[col][c] /= d;
		for (int r = 0; r < 4; ++r) {
			if (r == col)
				continue;
			double f = a[r][col];
			if (f == 0.0)
				continue;
			for (int c = 0; c < 8; ++c)
				a[r][c] -= f * a[col][c];
		}
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			inv[c * 4 + r] = a[r][4 + c];
	return true;
}

/// Unproject an NDC position to view space. out = (invP * (ndc,1)) / w.
void unproject(const double invProj[16], double nx, double ny, double nz, double out[3])
{
	double c[4] = { nx, ny, nz, 1.0 };
	double r[4] = { 0.0, 0.0, 0.0, 0.0 };
	for (int i = 0; i < 4; ++i)
		for (int k = 0; k < 4; ++k)
			r[i] += invProj[k * 4 + i] * c[k];
	out[0] = r[0] / r[3];
	out[1] = r[1] / r[3];
	out[2] = r[2] / r[3];
}

double length3(const double v[3])
{
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// ---------------------------------------------------------------------------
// Capture pipeline shaders.
//
// The shaders are static by pipeline design, so the per-capture context is
// passed through a pointer that is only valid while capture() rasterizes.
// capture() is documented as non-reentrant; instances may still be
// interleaved freely at call granularity.
// ---------------------------------------------------------------------------

/// One boundary crossing reported by a rasterized fragment.
struct Event {
	float dist;  ///< View-space Euclidean distance of the crossing from the camera.
	int cell;    ///< Owning cell.
	int delta;   ///< +1 enter, -1 exit.
};

struct CaptureContext {
	float proj[16];                       ///< Column-major projection for the vertex shader.
	int vpX, vpY, vpW, vpH;               ///< Viewport (events are stored viewport-relative).
	std::vector<Event> *events;           ///< w*h buckets.
};

CaptureContext *g_captureContext = nullptr;

struct CaptureContextGuard {
	CaptureContext *previous;
	explicit CaptureContextGuard(CaptureContext *ctx) : previous(g_captureContext)
	{
		g_captureContext = ctx;
	}
	~CaptureContextGuard()
	{
		g_captureContext = previous;
	}
};

struct CaptureVertex {
	float pos[3];  ///< View-space position.
	float nrm[3];  ///< Outward face normal.
	float id;      ///< Cell id.
};

struct VolumeVertexShader : VertexShaderBase<VolumeVertexShader> {
	static const int AttribCount = 3;
	static const int AVarCount = 4; // outward normal + cell id (constant per face)
	static const int PVarCount = 3; // view-space position (perspective correct)

	static void processVertex(VertexShaderInput in, VertexShaderOutput *out)
	{
		const float *pos = static_cast<const float *>(in[0]);
		const float *nrm = static_cast<const float *>(in[1]);
		const float *id = static_cast<const float *>(in[2]);
		const float *m = g_captureContext->proj;

		float x = pos[0], y = pos[1], z = pos[2];
		out->x = m[0] * x + m[4] * y + m[8] * z + m[12];
		out->y = m[1] * x + m[5] * y + m[9] * z + m[13];
		out->z = m[2] * x + m[6] * y + m[10] * z + m[14];
		out->w = m[3] * x + m[7] * y + m[11] * z + m[15];

		out->pvar[0] = x;
		out->pvar[1] = y;
		out->pvar[2] = z;

		out->avar[0] = nrm[0];
		out->avar[1] = nrm[1];
		out->avar[2] = nrm[2];
		out->avar[3] = id[0];
	}
};

struct VolumePixelShader : PixelShaderBase<VolumePixelShader> {
	static const int InterpolateZ = false;
	static const int InterpolateW = false;
	static const int AVarCount = 4;
	static const int PVarCount = 3;

	static void drawPixel(const PixelData &p)
	{
		CaptureContext *ctx = g_captureContext;
		if (!ctx)
			return;
		// The block rasterizer can emit pixels outside the scissor rect;
		// keep only fragments inside the viewport.
		if (p.x < ctx->vpX || p.x >= ctx->vpX + ctx->vpW ||
			p.y < ctx->vpY || p.y >= ctx->vpY + ctx->vpH)
			return;

		double vx = p.pvar[0], vy = p.pvar[1], vz = p.pvar[2];
		// Sign of (outward normal . ray direction) classifies the crossing.
		double dot = vx * p.avar[0] + vy * p.avar[1] + vz * p.avar[2];

		Event e;
		e.dist = (float)std::sqrt(vx * vx + vy * vy + vz * vz);
		e.cell = (int)std::floor((double)p.avar[3] + 0.5);
		e.delta = dot < 0.0 ? 1 : -1;
		ctx->events[(std::size_t)(p.y - ctx->vpY) * (std::size_t)ctx->vpW + (std::size_t)(p.x - ctx->vpX)].push_back(e);
	}
};

// ---------------------------------------------------------------------------
// Geometry helpers.
// ---------------------------------------------------------------------------

const int kFaceVerts[4][3] = { {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3} };

/// Half-space test of point dir*s against the four outward cell planes.
bool cellContains(const volumedetail::CellData &g, const double dir[3], double s)
{
	double p[3] = { dir[0] * s, dir[1] * s, dir[2] * s };
	for (int f = 0; f < 4; ++f) {
		double d = 0.0;
		for (int c = 0; c < 3; ++c)
			d += g.planeN[f][c] * (p[c] - g.planeP[f][c]);
		if (d > 0.0)
			return false;
	}
	return true;
}

void emitInterval(std::vector<volumedetail::Interval> &out, unsigned cell,
                  double a, double b, double nd, double fd)
{
	if (a < nd) a = nd;
	if (b < nd) b = nd;
	if (a > fd) a = fd;
	if (b > fd) b = fd;
	if (b > a) {
		volumedetail::Interval in;
		in.cell = cell;
		in.enter = (float)a;
		in.exit = (float)b;
		out.push_back(in);
	}
}

/// Reduce the boundary crossings of one cell at one pixel to ray intervals.
/// Events are sorted by distance; crossings at exactly equal distance are
/// applied as one group so that shared edges, coplanar endpoints and clip
/// fan splits can neither duplicate nor drop thickness.
void sweepCellEvents(const Event *ev, std::size_t n, unsigned cellId,
                     const volumedetail::CellData &g, const double dir[3],
                     double nd, double fd, std::vector<volumedetail::Interval> &out)
{
	// Crossings at or behind the near cap are replaced by the cap itself.
	std::size_t first = 0;
	while (first < n && ev[first].dist <= nd)
		++first;
	if (first == n)
		return;

	// The medium state is constant between the near cap and the first
	// crossing; sample it with a half-space test (this also covers the
	// camera-inside-cell and near-plane-inside-cell cases).
	double s0 = 0.5 * (nd + (double)ev[first].dist);
	int counter = cellContains(g, dir, s0) ? 1 : 0;
	bool open = counter > 0;
	double start = nd;

	std::size_t k = first;
	while (k < n) {
		std::size_t m = k + 1;
		while (m < n && ev[m].dist == ev[k].dist)
			++m;
		int net = 0;
		for (std::size_t q = k; q < m; ++q)
			net += ev[q].delta;
		double d = ev[k].dist;
		if (d > fd)
			d = fd;
		bool wasOpen = counter > 0;
		counter += net;
		if (counter < 0)
			counter = 0;
		bool nowOpen = counter > 0;
		if (!wasOpen && nowOpen) {
			open = true;
			start = d;
		} else if (wasOpen && !nowOpen && open) {
			emitInterval(out, cellId, start, d, nd, fd);
			open = false;
		}
		k = m;
	}
	// A still-open interval runs into the far cap.
	if (open)
		emitInterval(out, cellId, start, fd, nd, fd);
}

struct ScreenBox {
	int x0, y0, x1, y1; ///< Viewport-relative, [x0,x1) x [y0,y1).
	bool contains(int x, int y) const
	{
		return x >= x0 && x < x1 && y >= y0 && y < y1;
	}
};

} // end anonymous namespace

// ---------------------------------------------------------------------------
// VolumeRenderer
// ---------------------------------------------------------------------------

VolumeRenderer::VolumeRenderer()
{
}

void VolumeRenderer::setRasterMode(RasterMode mode)
{
	m_rasterMode = mode;
}

void VolumeRenderer::capture(const TetCell *cells, std::size_t cellCount,
                             const float proj[16], const Viewport &viewport)
{
	// ---- Validate everything up front. On any failure the previous
	// ---- capture is kept untouched.
	if (viewport.width <= 0 || viewport.height <= 0)
		throw std::invalid_argument("VolumeRenderer: viewport dimensions must be positive");
	if (cellCount > 0 && cells == nullptr)
		throw std::invalid_argument("VolumeRenderer: null cell array");
	if (proj == nullptr)
		throw std::invalid_argument("VolumeRenderer: null projection");

	double projD[16];
	for (int i = 0; i < 16; ++i) {
		if (!std::isfinite(proj[i]))
			throw std::invalid_argument("VolumeRenderer: projection must be finite");
		projD[i] = proj[i];
	}
	// Regular perspective: clip.w = -view.z.
	if (projD[3] != 0.0 || projD[7] != 0.0 || projD[11] != -1.0 || projD[15] != 0.0)
		throw std::invalid_argument("VolumeRenderer: projection must be perspective with clip.w = -view.z");
	double invProj[16];
	if (!invert4(projD, invProj))
		throw std::invalid_argument("VolumeRenderer: projection is singular");
	{
		double pn[3], pf[3];
		unproject(invProj, 0.0, 0.0, -1.0, pn);
		unproject(invProj, 0.0, 0.0, 1.0, pf);
		bool finite = true;
		for (int c = 0; c < 3; ++c)
			finite = finite && std::isfinite(pn[c]) && std::isfinite(pf[c]);
		if (!finite || !(pn[2] < 0.0) || !(pf[2] < 0.0) || !(pn[2] > pf[2]))
			throw std::invalid_argument("VolumeRenderer: projection has invalid near/far planes");
	}

	// Validate cells and build owned geometry (constant coefficients and
	// outward face planes in double precision).
	std::vector<CellData> geoms(cellCount);
	for (std::size_t i = 0; i < cellCount; ++i) {
		const TetCell &tc = cells[i];
		double p[4][3];
		for (int v = 0; v < 4; ++v)
			for (int c = 0; c < 3; ++c) {
				if (!std::isfinite(tc.v[v][c]))
					throw std::invalid_argument("VolumeRenderer: cell vertex must be finite");
				p[v][c] = tc.v[v][c];
			}
		if (!std::isfinite(tc.sigma) || tc.sigma < 0.0f)
			throw std::invalid_argument("VolumeRenderer: cell sigma must be finite and >= 0");
		for (int c = 0; c < 3; ++c)
			if (!std::isfinite(tc.j[c]) || tc.j[c] < 0.0f)
				throw std::invalid_argument("VolumeRenderer: cell emission must be finite and >= 0");

		double scale = 0.0;
		for (int a = 0; a < 4; ++a)
			for (int b = a + 1; b < 4; ++b) {
				double d[3] = { p[a][0] - p[b][0], p[a][1] - p[b][1], p[a][2] - p[b][2] };
				scale = std::max(scale, length3(d));
			}
		double e1[3], e2[3], e3[3];
		for (int c = 0; c < 3; ++c) {
			e1[c] = p[1][c] - p[0][c];
			e2[c] = p[2][c] - p[0][c];
			e3[c] = p[3][c] - p[0][c];
		}
		double det =
			e1[0] * (e2[1] * e3[2] - e2[2] * e3[1]) -
			e1[1] * (e2[0] * e3[2] - e2[2] * e3[0]) +
			e1[2] * (e2[0] * e3[1] - e2[1] * e3[0]);
		if (!(scale > 0.0) || std::fabs(det) <= 1e-12 * scale * scale * scale)
			throw std::invalid_argument("VolumeRenderer: cell vertices are coplanar");

		CellData g;
		g.sigma = tc.sigma;
		for (int c = 0; c < 3; ++c)
			g.j[c] = tc.j[c];
		for (int f = 0; f < 4; ++f) {
			const int *fv = kFaceVerts[f];
			int opposite = 6 - fv[0] - fv[1] - fv[2];
			double u[3], w[3], n[3];
			for (int c = 0; c < 3; ++c) {
				u[c] = p[fv[1]][c] - p[fv[0]][c];
				w[c] = p[fv[2]][c] - p[fv[0]][c];
			}
			n[0] = u[1] * w[2] - u[2] * w[1];
			n[1] = u[2] * w[0] - u[0] * w[2];
			n[2] = u[0] * w[1] - u[1] * w[0];
			double len = length3(n);
			double toward = 0.0;
			for (int c = 0; c < 3; ++c)
				toward += n[c] * (p[opposite][c] - p[fv[0]][c]);
			double s = toward > 0.0 ? -1.0 / len : 1.0 / len;
			for (int c = 0; c < 3; ++c) {
				g.planeN[f][c] = n[c] * s;
				g.planeP[f][c] = p[fv[0]][c];
			}
		}
		geoms[i] = g;
	}

	// ---- Build the new capture locally.
	const int w = viewport.width;
	const int h = viewport.height;
	std::vector<Interval> intervals;
	std::vector<unsigned> intervalOffset((std::size_t)w * (std::size_t)h + 1, 0);

	if (cellCount > 0) {
		// Boundary faces with per-face outward normals and cell identity.
		std::vector<CaptureVertex> verts(cellCount * 12);
		std::vector<int> indices(cellCount * 12);
		for (std::size_t i = 0; i < cellCount; ++i) {
			for (int f = 0; f < 4; ++f) {
				for (int k = 0; k < 3; ++k) {
					CaptureVertex &cv = verts[i * 12 + f * 3 + k];
					const TetCell &tc = cells[i];
					const float *pv = tc.v[kFaceVerts[f][k]];
					cv.pos[0] = pv[0];
					cv.pos[1] = pv[1];
					cv.pos[2] = pv[2];
					for (int c = 0; c < 3; ++c)
						cv.nrm[c] = (float)geoms[i].planeN[f][c];
					cv.id = (float)i;
					indices[i * 12 + f * 3 + k] = (int)(i * 12 + f * 3 + k);
				}
			}
		}

		// Rasterize every boundary face double-sided through the real
		// VertexProcessor/Rasterizer pipeline and collect crossing events.
		std::vector<std::vector<Event> > events((std::size_t)w * (std::size_t)h);
		{
			CaptureContext ctx;
			for (int i = 0; i < 16; ++i)
				ctx.proj[i] = proj[i];
			ctx.vpX = viewport.x;
			ctx.vpY = viewport.y;
			ctx.vpW = w;
			ctx.vpH = h;
			ctx.events = events.data();
			CaptureContextGuard guard(&ctx);

			Rasterizer r;
			r.setRasterMode(m_rasterMode);
			r.setScissorRect(viewport.x, viewport.y, w, h);
			r.setPixelShader<VolumePixelShader>();

			VertexProcessor vp(&r);
			vp.setViewport(viewport.x, viewport.y, w, h);
			vp.setCullMode(CullMode::None);
			vp.setVertexShader<VolumeVertexShader>();
			vp.setVertexAttribPointer(0, sizeof(CaptureVertex), &verts[0].pos[0]);
			vp.setVertexAttribPointer(1, sizeof(CaptureVertex), &verts[0].nrm[0]);
			vp.setVertexAttribPointer(2, sizeof(CaptureVertex), &verts[0].id);
			vp.drawElements(DrawMode::Triangle, indices.size(), indices.data());
		}

		// Conservative projected bounding box per cell (viewport-relative)
		// for the near/far cap inside tests.
		std::vector<ScreenBox> boxes(cellCount);
		for (std::size_t i = 0; i < cellCount; ++i) {
			double cx[4], cy[4], cw[4];
			bool clippedW = false;
			for (int v = 0; v < 4; ++v) {
				double x = cells[i].v[v][0], y = cells[i].v[v][1], z = cells[i].v[v][2];
				cx[v] = projD[0] * x + projD[4] * y + projD[8] * z + projD[12];
				cy[v] = projD[1] * x + projD[5] * y + projD[9] * z + projD[13];
				cw[v] = projD[3] * x + projD[7] * y + projD[11] * z + projD[15];
				if (cw[v] <= 0.0)
					clippedW = true;
			}
			ScreenBox b { 0, 0, w, h };
			if (!clippedW) {
				double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
				for (int v = 0; v < 4; ++v) {
					double sx = viewport.x + (cx[v] / cw[v] + 1.0) * 0.5 * w;
					double sy = viewport.y + (1.0 - cy[v] / cw[v]) * 0.5 * h;
					minX = std::min(minX, sx);
					maxX = std::max(maxX, sx);
					minY = std::min(minY, sy);
					maxY = std::max(maxY, sy);
				}
				b.x0 = std::max(0, (int)std::floor(minX - viewport.x - 0.5) - 1);
				b.y0 = std::max(0, (int)std::floor(minY - viewport.y - 0.5) - 1);
				b.x1 = std::min(w, (int)std::ceil(maxX - viewport.x - 0.5) + 2);
				b.y1 = std::min(h, (int)std::ceil(maxY - viewport.y - 0.5) + 2);
			}
			boxes[i] = b;
		}

		// Reduce events to per-cell ray intervals pixel by pixel.
		std::vector<unsigned> seenCells;
		for (int py = 0; py < h; ++py) {
			for (int px = 0; px < w; ++px) {
				std::size_t pi = (std::size_t)py * (std::size_t)w + (std::size_t)px;
				intervalOffset[pi] = (unsigned)intervals.size();

				// Ray through the pixel center.
				double ndcX = ((px + 0.5) - w * 0.5) / (w * 0.5);
				double ndcY = (h * 0.5 - (py + 0.5)) / (h * 0.5);
				double pn[3], pf[3];
				unproject(invProj, ndcX, ndcY, -1.0, pn);
				unproject(invProj, ndcX, ndcY, 1.0, pf);
				double nd = length3(pn);
				double fd = length3(pf);
				double dir[3] = { pn[0] / nd, pn[1] / nd, pn[2] / nd };

				std::vector<Event> &ev = events[pi];
				seenCells.clear();
				if (!ev.empty()) {
					std::sort(ev.begin(), ev.end(), [](const Event &a, const Event &b) {
						if (a.cell != b.cell)
							return a.cell < b.cell;
						return a.dist < b.dist;
					});
					std::size_t i = 0;
					while (i < ev.size()) {
						std::size_t j = i + 1;
						while (j < ev.size() && ev[j].cell == ev[i].cell)
							++j;
						unsigned cid = (unsigned)ev[i].cell;
						sweepCellEvents(&ev[i], j - i, cid, geoms[cid], dir, nd, fd, intervals);
						seenCells.push_back(cid);
						i = j;
					}
				}

				// Cells whose frustum-clipped boundary produced no event at
				// this pixel can still cover it through both caps (e.g. a
				// cell enclosing the frustum): test the near cap inside the
				// projected bounding box.
				for (std::size_t c = 0; c < cellCount; ++c) {
					if (!boxes[c].contains(px, py))
						continue;
					if (std::find(seenCells.begin(), seenCells.end(), (unsigned)c) != seenCells.end())
						continue;
					if (cellContains(geoms[c], dir, nd))
						emitInterval(intervals, (unsigned)c, nd, fd, nd, fd);
				}
			}
		}
		intervalOffset[(std::size_t)w * (std::size_t)h] = (unsigned)intervals.size();
	}

	// ---- Commit the new capture.
	m_cells = std::move(geoms);
	m_intervals = std::move(intervals);
	m_intervalOffset = std::move(intervalOffset);
	m_vpX = viewport.x;
	m_vpY = viewport.y;
	m_vpW = w;
	m_vpH = h;
	for (int i = 0; i < 16; ++i)
		m_invProj[i] = invProj[i];
	m_hasCapture = true;
}

VolumeRenderer::Image VolumeRenderer::composite(const float *baseRgb, const float *baseDepth) const
{
	if (!m_hasCapture)
		throw std::logic_error("VolumeRenderer: composite() requires a successful capture() first");
	if (baseRgb == nullptr || baseDepth == nullptr)
		throw std::invalid_argument("VolumeRenderer: null base image");

	const int w = m_vpW;
	const int h = m_vpH;
	Image img;
	img.width = w;
	img.height = h;
	img.rgb.resize((std::size_t)w * (std::size_t)h * 3);
	img.transmittance.resize((std::size_t)w * (std::size_t)h);

	struct Endpoint {
		double dist;
		unsigned cell;
		int delta;
	};
	std::vector<Endpoint> endpoints;

	for (int py = 0; py < h; ++py) {
		double ndcY = (h * 0.5 - (py + 0.5)) / (h * 0.5);
		for (int px = 0; px < w; ++px) {
			std::size_t pi = (std::size_t)py * (std::size_t)w + (std::size_t)px;
			double ndcX = ((px + 0.5) - w * 0.5) / (w * 0.5);

			// Truncate against the unprojected opaque base depth first.
			double cutoff = std::numeric_limits<double>::infinity();
			float dv = baseDepth[pi];
			if (std::isnan(dv))
				dv = 1.0f;
			if (dv < 0.0f)
				dv = 0.0f;
			if (dv > 1.0f)
				dv = 1.0f;
			if (dv < 1.0f) {
				double p[3];
				unproject(m_invProj, ndcX, ndcY, 2.0 * (double)dv - 1.0, p);
				cutoff = length3(p);
			}

			endpoints.clear();
			unsigned b = m_intervalOffset[pi];
			unsigned e = m_intervalOffset[pi + 1];
			for (unsigned q = b; q < e; ++q) {
				const Interval &in = m_intervals[q];
				double a = in.enter;
				double z = in.exit;
				if (a > cutoff)
					a = cutoff;
				if (z > cutoff)
					z = cutoff;
				if (z > a) {
					Endpoint ep;
					ep.dist = a;
					ep.cell = in.cell;
					ep.delta = 1;
					endpoints.push_back(ep);
					ep.dist = z;
					ep.delta = -1;
					endpoints.push_back(ep);
				}
			}

			const float *base = baseRgb + pi * 3;
			double C[3] = { 0.0, 0.0, 0.0 };
			double T = 1.0;

			if (!endpoints.empty()) {
				std::sort(endpoints.begin(), endpoints.end(), [](const Endpoint &a, const Endpoint &b) {
					if (a.dist != b.dist)
						return a.dist < b.dist;
					if (a.cell != b.cell)
						return a.cell < b.cell;
					return a.delta < b.delta;
				});

				double sigma = 0.0;
				double j[3] = { 0.0, 0.0, 0.0 };
				double prev = 0.0;
				std::size_t k = 0;
				while (k < endpoints.size()) {
					double d = endpoints[k].dist;
					double L = d - prev;
					if (L > 0.0 && (sigma > 0.0 || j[0] != 0.0 || j[1] != 0.0 || j[2] != 0.0)) {
						if (sigma > 0.0) {
							double t = std::exp(-sigma * L);
							double kappa = -std::expm1(-sigma * L) / sigma;
							for (int c = 0; c < 3; ++c)
								C[c] += T * j[c] * kappa;
							T *= t;
						} else {
							for (int c = 0; c < 3; ++c)
								C[c] += T * j[c] * L;
						}
					}
					// Endpoints at the same distance are applied as one
					// group; the submission order cannot decide the color.
					std::size_t m = k;
					while (m < endpoints.size() && endpoints[m].dist == d) {
						const CellData &g = m_cells[endpoints[m].cell];
						double s = endpoints[m].delta > 0 ? 1.0 : -1.0;
						sigma += s * g.sigma;
						for (int c = 0; c < 3; ++c)
							j[c] += s * g.j[c];
						++m;
					}
					k = m;
					prev = d;
				}
			}

			for (int c = 0; c < 3; ++c)
				img.rgb[pi * 3 + c] = (float)(C[c] + T * (double)base[c]);
			img.transmittance[pi] = (float)T;
		}
	}
	return img;
}

} // end namespace swr
