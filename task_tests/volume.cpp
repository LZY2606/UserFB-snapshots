// Regression tests for swr::VolumeRenderer.
//
// The reference implementation below is intentionally independent from the
// production code: rays through pixel centers are intersected with the
// tetrahedral cells analytically via half-space (slab) tests and the medium
// is integrated with the closed-form constant-coefficient segment solution.
// Production results are compared against this reference on the interior
// pixels commonly covered by all three raster modes (Span/Block/Adaptive).

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "Renderer.h"
#include "VolumeRenderer.h"

namespace {

using swr::VolumeRenderer;

const double kTol = 1e-4;
const swr::RasterMode kModes[3] = { swr::RasterMode::Span, swr::RasterMode::Block, swr::RasterMode::Adaptive };

// ---------------------------------------------------------------------------
// Small independent double-precision math.
// ---------------------------------------------------------------------------

struct D3 {
	double x, y, z;
};

D3 sub(D3 a, D3 b) { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 crossp(D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dotp(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double lenp(D3 a) { return std::sqrt(dotp(a, a)); }

void makePerspective(double m[16], double l, double r, double b, double t, double n, double f)
{
	for (int i = 0; i < 16; ++i)
		m[i] = 0.0;
	m[0] = 2.0 * n / (r - l);
	m[5] = 2.0 * n / (t - b);
	m[8] = (r + l) / (r - l);
	m[9] = (t + b) / (t - b);
	m[10] = -(f + n) / (f - n);
	m[11] = -1.0;
	m[14] = -2.0 * f * n / (f - n);
}

void toFloat16(const double m[16], float out[16])
{
	for (int i = 0; i < 16; ++i)
		out[i] = (float)m[i];
}

// Test-side 4x4 inverse (column-major), Gauss-Jordan.
bool inv4(const double m[16], double inv[16])
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
		for (int c = 0; c < 8; ++c)
			std::swap(a[piv][c], a[col][c]);
		double d = a[col][col];
		for (int c = 0; c < 8; ++c)
			a[col][c] /= d;
		for (int r = 0; r < 4; ++r) {
			if (r == col)
				continue;
			double f = a[r][col];
			for (int c = 0; c < 8; ++c)
				a[r][c] -= f * a[col][c];
		}
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			inv[c * 4 + r] = a[r][4 + c];
	return true;
}

D3 unprojectT(const double inv[16], double nx, double ny, double nz)
{
	double c[4] = {nx, ny, nz, 1.0};
	double r[4] = {0.0, 0.0, 0.0, 0.0};
	for (int i = 0; i < 4; ++i)
		for (int k = 0; k < 4; ++k)
			r[i] += inv[k * 4 + i] * c[k];
	return D3{r[0] / r[3], r[1] / r[3], r[2] / r[3]};
}

// ---------------------------------------------------------------------------
// Independent reference: ray/tet half-space intersection + analytic segments.
// ---------------------------------------------------------------------------

const int kFaceVerts[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};

struct RefCell {
	D3 v[4];
	double sigma;
	double j[3];
	D3 n[4]; // outward unit normals
	D3 a[4]; // face points
};

RefCell makeRefCell(const VolumeRenderer::TetCell &tc)
{
	RefCell c;
	for (int i = 0; i < 4; ++i)
		c.v[i] = D3{tc.v[i][0], tc.v[i][1], tc.v[i][2]}; // same float values the API sees
	c.sigma = tc.sigma;
	for (int k = 0; k < 3; ++k)
		c.j[k] = tc.j[k];
	for (int f = 0; f < 4; ++f) {
		D3 a = c.v[kFaceVerts[f][0]];
		D3 b = c.v[kFaceVerts[f][1]];
		D3 d = c.v[kFaceVerts[f][2]];
		D3 opp = c.v[6 - kFaceVerts[f][0] - kFaceVerts[f][1] - kFaceVerts[f][2]];
		D3 n = crossp(sub(b, a), sub(d, a));
		double l = lenp(n);
		n = D3{n.x / l, n.y / l, n.z / l};
		if (dotp(n, sub(opp, a)) > 0.0)
			n = D3{-n.x, -n.y, -n.z};
		c.n[f] = n;
		c.a[f] = a;
	}
	return c;
}

// Ray is p = t * dir with origin at the camera. Returns the inside interval.
bool rayTet(const RefCell &c, const D3 &dir, double &t0, double &t1)
{
	t0 = -std::numeric_limits<double>::infinity();
	t1 = std::numeric_limits<double>::infinity();
	for (int f = 0; f < 4; ++f) {
		double dn = dotp(c.n[f], dir);
		double da = dotp(c.n[f], c.a[f]);
		if (dn == 0.0) {
			if (da < 0.0)
				return false; // camera side of the plane is outside
			continue;
		}
		double t = da / dn;
		if (dn < 0.0)
			t0 = std::max(t0, t);
		else
			t1 = std::min(t1, t);
	}
	return t0 < t1;
}

struct Scene {
	std::vector<VolumeRenderer::TetCell> cells;
	double proj[16];
	int vpx, vpy, vpw, vph;
};

struct RefImage {
	int w = 0, h = 0;
	std::vector<double> rgb;    // w*h*3
	std::vector<double> trans;  // w*h
	std::vector<char> covered;  // analytic coverage inside the frustum (ignoring opaque depth)
};

void pixelRay(const double inv[16], int w, int h, int px, int py, D3 &dir, double &nd, double &fd)
{
	double ndcX = ((px + 0.5) - w * 0.5) / (w * 0.5);
	double ndcY = (h * 0.5 - (py + 0.5)) / (h * 0.5);
	D3 pn = unprojectT(inv, ndcX, ndcY, -1.0);
	D3 pf = unprojectT(inv, ndcX, ndcY, 1.0);
	nd = lenp(pn);
	fd = lenp(pf);
	dir = D3{pn.x / nd, pn.y / nd, pn.z / nd};
}

RefImage renderReference(const Scene &sc, const std::vector<float> &baseRgb, const std::vector<float> &baseDepth)
{
	const int w = sc.vpw, h = sc.vph;
	RefImage out;
	out.w = w;
	out.h = h;
	out.rgb.assign((size_t)w * h * 3, 0.0);
	out.trans.assign((size_t)w * h, 1.0);
	out.covered.assign((size_t)w * h, 0);

	double inv[16];
	inv4(sc.proj, inv);

	std::vector<RefCell> cells;
	for (size_t i = 0; i < sc.cells.size(); ++i)
		cells.push_back(makeRefCell(sc.cells[i]));

	struct Ep {
		double t;
		int cell;
		int delta;
	};
	std::vector<Ep> eps;

	for (int py = 0; py < h; ++py) {
		for (int px = 0; px < w; ++px) {
			size_t pi = (size_t)py * w + px;
			D3 dir;
			double nd, fd;
			pixelRay(inv, w, h, px, py, dir, nd, fd);

			double cutoff = std::numeric_limits<double>::infinity();
			float dv = baseDepth[pi];
			if (dv < 1.0f) {
				double ndcX = ((px + 0.5) - w * 0.5) / (w * 0.5);
				double ndcY = (h * 0.5 - (py + 0.5)) / (h * 0.5);
				D3 p = unprojectT(inv, ndcX, ndcY, 2.0 * (double)dv - 1.0);
				cutoff = lenp(p);
			}

			eps.clear();
			bool cov = false;
			for (size_t c = 0; c < cells.size(); ++c) {
				double t0, t1;
				if (!rayTet(cells[c], dir, t0, t1))
					continue;
				double a = std::max(t0, nd);
				double b = std::min(t1, fd);
				if (!(b > a))
					continue;
				cov = true;
				double a2 = std::min(a, cutoff);
				double b2 = std::min(b, cutoff);
				if (b2 > a2) {
					eps.push_back(Ep{a2, (int)c, 1});
					eps.push_back(Ep{b2, (int)c, -1});
				}
			}
			out.covered[pi] = cov ? 1 : 0;

			double C[3] = {0.0, 0.0, 0.0};
			double T = 1.0;
			if (!eps.empty()) {
				std::sort(eps.begin(), eps.end(), [](const Ep &a, const Ep &b) {
					if (a.t != b.t)
						return a.t < b.t;
					if (a.cell != b.cell)
						return a.cell < b.cell;
					return a.delta < b.delta;
				});
				double sigma = 0.0, j[3] = {0.0, 0.0, 0.0}, prev = 0.0;
				size_t k = 0;
				while (k < eps.size()) {
					double d = eps[k].t;
					double L = d - prev;
					if (L > 0.0 && (sigma > 0.0 || j[0] != 0.0 || j[1] != 0.0 || j[2] != 0.0)) {
						if (sigma > 0.0) {
							double t = std::exp(-sigma * L);
							double kk = -std::expm1(-sigma * L) / sigma;
							for (int c = 0; c < 3; ++c)
								C[c] += T * j[c] * kk;
							T *= t;
						} else {
							for (int c = 0; c < 3; ++c)
								C[c] += T * j[c] * L;
						}
					}
					size_t m = k;
					while (m < eps.size() && eps[m].t == d) {
						const RefCell &g = cells[eps[m].cell];
						double s = eps[m].delta > 0 ? 1.0 : -1.0;
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
				out.rgb[pi * 3 + c] = C[c] + T * (double)baseRgb[pi * 3 + c];
			out.trans[pi] = T;
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// Pipeline driver + comparison helpers.
// ---------------------------------------------------------------------------

VolumeRenderer::Image runPipeline(const Scene &sc, swr::RasterMode mode,
                                  const std::vector<float> &baseRgb, const std::vector<float> &baseDepth)
{
	VolumeRenderer vr;
	vr.setRasterMode(mode);
	float pf[16];
	toFloat16(sc.proj, pf);
	VolumeRenderer::Viewport vp;
	vp.x = sc.vpx;
	vp.y = sc.vpy;
	vp.width = sc.vpw;
	vp.height = sc.vph;
	vr.capture(sc.cells.data(), sc.cells.size(), pf, vp);
	return vr.composite(baseRgb.data(), baseDepth.data());
}

struct MaxErr {
	double rgb = 0.0;
	double trans = 0.0;
	int compared = 0;
};

// Compare on interior pixels: analytically covered with all 8 neighbours
// covered as well, so fill-convention differences at silhouettes cannot
// interfere. Those pixels are covered in all three raster modes.
MaxErr compareInterior(const VolumeRenderer::Image &img, const RefImage &ref)
{
	MaxErr r;
	int w = ref.w, h = ref.h;
	for (int y = 1; y + 1 < h; ++y) {
		for (int x = 1; x + 1 < w; ++x) {
			bool interior = true;
			for (int dy = -1; dy <= 1 && interior; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					if (!ref.covered[(size_t)(y + dy) * w + (x + dx)]) {
						interior = false;
						break;
					}
			if (!interior)
				continue;
			++r.compared;
			size_t pi = (size_t)y * w + x;
			for (int c = 0; c < 3; ++c)
				r.rgb = std::max(r.rgb, std::fabs((double)img.rgb[pi * 3 + c] - ref.rgb[pi * 3 + c]));
			r.trans = std::max(r.trans, std::fabs((double)img.transmittance[pi] - ref.trans[pi]));
		}
	}
	return r;
}

// Run all three raster modes and check each against the reference.
int checkAllModes(const Scene &sc, const std::vector<float> &baseRgb, const std::vector<float> &baseDepth,
                  double tol, int minInterior)
{
	RefImage ref = renderReference(sc, baseRgb, baseDepth);
	for (int m = 0; m < 3; ++m) {
		VolumeRenderer::Image img = runPipeline(sc, kModes[m], baseRgb, baseDepth);
		TASK_CHECK(img.width == sc.vpw && img.height == sc.vph);
		TASK_CHECK(img.rgb.size() == (size_t)sc.vpw * sc.vph * 3);
		TASK_CHECK(img.transmittance.size() == (size_t)sc.vpw * sc.vph);
		MaxErr e = compareInterior(img, ref);
		TASK_CHECK(e.compared > minInterior);
		TASK_CHECK(e.rgb < tol);
		TASK_CHECK(e.trans < tol);
	}
	return 0;
}

std::vector<float> makeBaseRgb(int w, int h)
{
	std::vector<float> b((size_t)w * h * 3);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			size_t i = (size_t)y * w + x;
			b[i * 3 + 0] = 0.10f + 0.40f * (float)x / (float)w;
			b[i * 3 + 1] = 0.20f + 0.30f * (float)y / (float)h;
			b[i * 3 + 2] = 0.35f;
		}
	return b;
}

std::vector<float> makeConstDepth(int w, int h, float d)
{
	return std::vector<float>((size_t)w * h, d);
}

VolumeRenderer::TetCell makeCell(float v[4][3], float sigma, float j0, float j1, float j2)
{
	VolumeRenderer::TetCell c;
	for (int i = 0; i < 4; ++i)
		for (int k = 0; k < 3; ++k)
			c.v[i][k] = v[i][k];
	c.sigma = sigma;
	c.j[0] = j0;
	c.j[1] = j1;
	c.j[2] = j2;
	return c;
}

Scene makeScene(std::vector<VolumeRenderer::TetCell> cells)
{
	Scene sc;
	sc.cells = std::move(cells);
	makePerspective(sc.proj, -0.9, 0.5, -0.6, 0.8, 1.0, 14.0); // asymmetric frustum
	sc.vpx = 3; // non-zero viewport origin
	sc.vpy = 2;
	sc.vpw = 44;
	sc.vph = 32;
	return sc;
}

Scene sceneSkewedCell()
{
	float v[4][3] = {{-0.9f, -0.6f, -2.0f}, {0.8f, -0.5f, -2.6f}, {0.1f, 0.9f, -3.2f}, {-0.2f, 0.0f, -5.5f}};
	return makeScene({makeCell(v, 0.6f, 0.9f, 0.35f, 0.15f)});
}

Scene sceneNearFarClip()
{
	float a[4][3] = {{-0.5f, -0.5f, -0.4f}, {0.6f, -0.4f, -1.5f}, {0.0f, 0.7f, -2.0f}, {0.0f, 0.0f, -4.0f}};
	float b[4][3] = {{-0.4f, -0.4f, -8.0f}, {0.5f, -0.3f, -10.0f}, {0.1f, 0.5f, -12.0f}, {0.0f, 0.0f, -18.0f}};
	return makeScene({makeCell(a, 0.5f, 0.2f, 0.8f, 0.3f), makeCell(b, 0.3f, 0.7f, 0.2f, 0.9f)});
}

Scene sceneCameraInside()
{
	float v[4][3] = {{0.1f, 0.2f, 1.0f}, {-3.0f, -2.5f, -5.0f}, {3.2f, -2.2f, -5.0f}, {0.5f, 3.0f, -5.0f}};
	return makeScene({makeCell(v, 0.45f, 0.6f, 0.3f, 0.8f)});
}

Scene sceneCoplanarNeighbors()
{
	// Two cells sharing exactly one face (same three vertices).
	float s0[3] = {-0.6f, -0.5f, -2.5f};
	float s1[3] = {0.7f, -0.4f, -2.8f};
	float s2[3] = {0.0f, 0.8f, -3.0f};
	float a[4][3] = {{s0[0], s0[1], s0[2]}, {s1[0], s1[1], s1[2]}, {s2[0], s2[1], s2[2]}, {0.0f, 0.0f, -1.6f}};
	float b[4][3] = {{s0[0], s0[1], s0[2]}, {s1[0], s1[1], s1[2]}, {s2[0], s2[1], s2[2]}, {0.1f, 0.05f, -4.6f}};
	return makeScene({makeCell(a, 0.7f, 0.9f, 0.2f, 0.2f), makeCell(b, 0.4f, 0.2f, 0.4f, 0.9f)});
}

Scene sceneClipFan()
{
	// Two vertices between camera and near plane: clipped faces become
	// polygons that the pipeline triangulates as fans.
	float v[4][3] = {{-0.8f, -0.6f, -0.3f}, {0.9f, -0.5f, -0.4f}, {0.0f, 0.8f, -3.0f}, {-0.1f, -0.2f, -4.5f}};
	return makeScene({makeCell(v, 0.55f, 0.9f, 0.5f, 0.2f)});
}

Scene sceneInterleaved()
{
	float a[4][3] = {{-0.7f, -0.6f, -2.0f}, {0.7f, -0.5f, -2.2f}, {0.0f, 0.7f, -2.4f}, {0.0f, 0.0f, -4.0f}};
	float b[4][3] = {{-0.6f, -0.5f, -3.0f}, {0.6f, -0.6f, -3.1f}, {0.1f, 0.6f, -3.3f}, {0.0f, 0.1f, -5.2f}};
	float c[4][3] = {{-0.5f, -0.4f, -4.5f}, {0.5f, -0.5f, -4.6f}, {0.0f, 0.5f, -4.8f}, {0.0f, 0.0f, -6.5f}};
	return makeScene({makeCell(a, 0.5f, 0.9f, 0.1f, 0.1f),
	                  makeCell(b, 0.4f, 0.1f, 0.9f, 0.2f),
	                  makeCell(c, 0.6f, 0.2f, 0.3f, 0.9f)});
}

Scene sceneZeroExtinction()
{
	float a[4][3] = {{-0.7f, -0.6f, -2.0f}, {0.7f, -0.5f, -2.3f}, {0.0f, 0.7f, -2.6f}, {0.0f, 0.0f, -5.0f}};
	float b[4][3] = {{-0.5f, -0.4f, -3.0f}, {0.5f, -0.5f, -3.2f}, {0.1f, 0.5f, -3.4f}, {0.0f, 0.0f, -4.5f}};
	return makeScene({makeCell(a, 0.0f, 0.3f, 0.6f, 0.9f), makeCell(b, 0.9f, 0.4f, 0.1f, 0.1f)});
}

Scene sceneThinInterval()
{
	// Slab ~2e-3 thick along the view direction.
	float v[4][3] = {{-0.8f, -0.7f, -3.0f}, {0.9f, -0.6f, -3.0f}, {0.0f, 0.8f, -3.0f}, {0.05f, 0.0f, -3.002f}};
	return makeScene({makeCell(v, 30.0f, 1.5f, 1.0f, 0.5f)});
}

// ---------------------------------------------------------------------------
// Opaque occluder rendered through the real pipeline.
// ---------------------------------------------------------------------------

float g_occProj[16];
float *g_occColor = nullptr;
float *g_occDepth = nullptr;
int g_occVpX = 0, g_occVpY = 0, g_occW = 0, g_occH = 0;
float g_occR = 0, g_occG = 0, g_occB = 0;

struct OccVS : swr::VertexShaderBase<OccVS> {
	static const int AttribCount = 1;
	static void processVertex(swr::VertexShaderInput in, swr::VertexShaderOutput *out)
	{
		const float *v = static_cast<const float *>(in[0]);
		out->x = g_occProj[0] * v[0] + g_occProj[4] * v[1] + g_occProj[8] * v[2] + g_occProj[12];
		out->y = g_occProj[1] * v[0] + g_occProj[5] * v[1] + g_occProj[9] * v[2] + g_occProj[13];
		out->z = g_occProj[2] * v[0] + g_occProj[6] * v[1] + g_occProj[10] * v[2] + g_occProj[14];
		out->w = g_occProj[3] * v[0] + g_occProj[7] * v[1] + g_occProj[11] * v[2] + g_occProj[15];
	}
};

struct OccPS : swr::PixelShaderBase<OccPS> {
	static const int InterpolateZ = true;
	static void drawPixel(const swr::PixelData &p)
	{
		int lx = p.x - g_occVpX;
		int ly = p.y - g_occVpY;
		if (lx < 0 || ly < 0 || lx >= g_occW || ly >= g_occH)
			return;
		size_t i = (size_t)ly * g_occW + lx;
		g_occDepth[i] = p.z;
		g_occColor[i * 3 + 0] = g_occR;
		g_occColor[i * 3 + 1] = g_occG;
		g_occColor[i * 3 + 2] = g_occB;
	}
};

// Draw one opaque triangle with the real VertexProcessor/Rasterizer path,
// producing a linear-RGB color buffer and a [0,1] depth buffer.
void renderOccluder(const Scene &sc, const float tri[9], float r, float g, float b, swr::RasterMode mode,
                    std::vector<float> &color, std::vector<float> &depth)
{
	color = makeBaseRgb(sc.vpw, sc.vph);
	depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);

	toFloat16(sc.proj, g_occProj);
	g_occColor = color.data();
	g_occDepth = depth.data();
	g_occVpX = sc.vpx;
	g_occVpY = sc.vpy;
	g_occW = sc.vpw;
	g_occH = sc.vph;
	g_occR = r;
	g_occG = g;
	g_occB = b;

	swr::Rasterizer rst;
	rst.setRasterMode(mode);
	rst.setScissorRect(sc.vpx, sc.vpy, sc.vpw, sc.vph);
	rst.setPixelShader<OccPS>();

	swr::VertexProcessor vp(&rst);
	vp.setViewport(sc.vpx, sc.vpy, sc.vpw, sc.vph);
	vp.setCullMode(swr::CullMode::None);
	vp.setVertexShader<OccVS>();
	vp.setVertexAttribPointer(0, sizeof(float) * 3, tri);
	int idx[3] = {0, 1, 2};
	vp.drawElements(swr::DrawMode::Triangle, 3, idx);

	g_occColor = nullptr;
	g_occDepth = nullptr;
}

// ---------------------------------------------------------------------------
// capture group
// ---------------------------------------------------------------------------

int case_capture_skewed_perspective_cell()
{
	Scene sc = sceneSkewedCell();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;

	// Uncovered corner pixels keep the base image exactly with T == 1.
	RefImage ref = renderReference(sc, base, depth);
	VolumeRenderer::Image img = runPipeline(sc, swr::RasterMode::Span, base, depth);
	int uncovered = 0;
	for (size_t i = 0; i < ref.covered.size(); ++i) {
		if (ref.covered[i])
			continue;
		++uncovered;
		for (int c = 0; c < 3; ++c)
			TASK_CHECK(img.rgb[i * 3 + c] == base[i * 3 + c]);
		TASK_CHECK(img.transmittance[i] == 1.0f);
	}
	TASK_CHECK(uncovered > 0);
	return 0;
}

int case_capture_near_far_clipping()
{
	Scene sc = sceneNearFarClip();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;

	// Both caps must produce visible medium: the transmittance drops below 1
	// on interior pixels of both the near-clipped and the far-clipped cell.
	RefImage ref = renderReference(sc, base, depth);
	double minT = 1.0;
	for (size_t i = 0; i < ref.trans.size(); ++i)
		if (ref.covered[i])
			minT = std::min(minT, ref.trans[i]);
	TASK_CHECK(minT < 0.95);
	return 0;
}

int case_capture_camera_inside()
{
	Scene sc = sceneCameraInside();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;

	// The camera sits inside the cell: every ray starts in the medium, so
	// every covered pixel absorbs/emits from the near plane on.
	RefImage ref = renderReference(sc, base, depth);
	size_t center = (size_t)(sc.vph / 2) * sc.vpw + (size_t)(sc.vpw / 2);
	TASK_CHECK(ref.covered[center] == 1);
	TASK_CHECK(ref.trans[center] < 0.9);
	return 0;
}

int case_capture_coplanar_neighbors()
{
	Scene sc = sceneCoplanarNeighbors();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	// Coplanar shared faces must not create duplicated or missing thickness;
	// the analytic reference has exactly one boundary at the shared plane.
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;
	return 0;
}

int case_capture_clip_fan()
{
	Scene sc = sceneClipFan();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	// Clip-fan splits of the same face must not duplicate thickness.
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;
	return 0;
}

// ---------------------------------------------------------------------------
// overlap group
// ---------------------------------------------------------------------------

int case_overlap_interleaved_media()
{
	Scene sc = sceneInterleaved();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;
	return 0;
}

int case_overlap_zero_extinction_emission()
{
	Scene sc = sceneZeroExtinction();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;

	// The sigma == 0 cell emits without attenuating: some covered pixel must
	// be brighter than the base while its transmittance only comes from the
	// absorbing cell.
	RefImage ref = renderReference(sc, base, depth);
	VolumeRenderer::Image img = runPipeline(sc, swr::RasterMode::Block, base, depth);
	bool brighter = false;
	for (size_t i = 0; i < ref.covered.size(); ++i) {
		if (!ref.covered[i])
			continue;
		if (img.rgb[i * 3 + 2] > base[i * 3 + 2] + 0.01f)
			brighter = true;
	}
	TASK_CHECK(brighter);
	return 0;
}

int case_overlap_thin_interval()
{
	Scene sc = sceneThinInterval();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	if (checkAllModes(sc, base, depth, kTol, 30))
		return 1;

	// The ~2e-3 interval is genuinely non-zero and must not be skipped:
	// interior pixels clearly deviate from the base image.
	RefImage ref = renderReference(sc, base, depth);
	VolumeRenderer::Image img = runPipeline(sc, swr::RasterMode::Span, base, depth);
	double maxDev = 0.0;
	for (int y = 1; y + 1 < sc.vph; ++y)
		for (int x = 1; x + 1 < sc.vpw; ++x) {
			size_t i = (size_t)y * sc.vpw + x;
			if (!ref.covered[i])
				continue;
			for (int c = 0; c < 3; ++c)
				maxDev = std::max(maxDev, std::fabs((double)img.rgb[i * 3 + c] - (double)base[i * 3 + c]));
		}
	TASK_CHECK(maxDev > 1e-3);
	return 0;
}

int case_overlap_swapped_submission()
{
	Scene sc = sceneInterleaved();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);

	Scene rev = sc;
	std::reverse(rev.cells.begin(), rev.cells.end());

	for (int m = 0; m < 3; ++m) {
		VolumeRenderer::Image forward = runPipeline(sc, kModes[m], base, depth);
		VolumeRenderer::Image backward = runPipeline(rev, kModes[m], base, depth);
		double maxDiff = 0.0;
		for (size_t i = 0; i < forward.rgb.size(); ++i)
			maxDiff = std::max(maxDiff, std::fabs((double)forward.rgb[i] - (double)backward.rgb[i]));
		for (size_t i = 0; i < forward.transmittance.size(); ++i)
			maxDiff = std::max(maxDiff, std::fabs((double)forward.transmittance[i] - (double)backward.transmittance[i]));
		// The submission order must not decide the color.
		TASK_CHECK(maxDiff < 1e-5);
	}

	// And both orders still match the analytic reference.
	if (checkAllModes(rev, base, depth, kTol, 30))
		return 1;
	return 0;
}

int case_overlap_opaque_occluder()
{
	Scene sc = sceneInterleaved();
	// Opaque triangle halfway through the volume, drawn for real.
	const float tri[9] = {-2.0f, -1.5f, -3.5f, 2.0f, -1.0f, -3.5f, 0.0f, 2.0f, -3.5f};

	for (int m = 0; m < 3; ++m) {
		std::vector<float> color, depth;
		renderOccluder(sc, tri, 0.85f, 0.6f, 0.25f, kModes[m], color, depth);

		// Sanity: the occluder really covers part of the view.
		int occ = 0;
		for (size_t i = 0; i < depth.size(); ++i)
			if (depth[i] < 1.0f)
				++occ;
		TASK_CHECK(occ > 50);

		VolumeRenderer::Image img = runPipeline(sc, kModes[m], color, depth);
		RefImage ref = renderReference(sc, color, depth);
		MaxErr e = compareInterior(img, ref);
		TASK_CHECK(e.compared > 30);
		TASK_CHECK(e.rgb < kTol);
		TASK_CHECK(e.trans < kTol);

		// Behind the occluder the volume must be cut off: pixels deep inside
		// the occluder show (attenuated) occluder color, not raw volume.
		bool found = false;
		for (int y = 2; y + 2 < sc.vph && !found; ++y)
			for (int x = 2; x + 2 < sc.vpw; ++x) {
				size_t i = (size_t)y * sc.vpw + x;
				if (depth[i] < 1.0f && ref.covered[i]) {
					found = true;
					TASK_CHECK(std::fabs((double)img.rgb[i * 3 + 0] - (double)ref.rgb[i * 3 + 0]) < kTol);
					break;
				}
			}
		TASK_CHECK(found);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// lifecycle group
// ---------------------------------------------------------------------------

int case_lifecycle_recapture_replaces()
{
	Scene a = sceneSkewedCell();
	Scene b = sceneInterleaved();
	std::vector<float> base = makeBaseRgb(a.vpw, a.vph);
	std::vector<float> depth = makeConstDepth(a.vpw, a.vph, 1.0f);
	float pf[16];

	VolumeRenderer vr;
	vr.setRasterMode(swr::RasterMode::Adaptive);

	toFloat16(a.proj, pf);
	VolumeRenderer::Viewport vp{a.vpx, a.vpy, a.vpw, a.vph};
	vr.capture(a.cells.data(), a.cells.size(), pf, vp);
	VolumeRenderer::Image imgA = vr.composite(base.data(), depth.data());
	RefImage refA = renderReference(a, base, depth);
	MaxErr eA = compareInterior(imgA, refA);
	TASK_CHECK(eA.compared > 30);
	TASK_CHECK(eA.rgb < kTol);

	// Replacing the capture (next frame) must not keep any state around.
	toFloat16(b.proj, pf);
	vr.capture(b.cells.data(), b.cells.size(), pf, vp);
	VolumeRenderer::Image imgB = vr.composite(base.data(), depth.data());
	RefImage refB = renderReference(b, base, depth);
	MaxErr eB = compareInterior(imgB, refB);
	TASK_CHECK(eB.compared > 30);
	TASK_CHECK(eB.rgb < kTol);
	TASK_CHECK(eB.trans < kTol);

	double maxDiff = 0.0;
	for (size_t i = 0; i < imgA.rgb.size(); ++i)
		maxDiff = std::max(maxDiff, std::fabs((double)imgA.rgb[i] - (double)imgB.rgb[i]));
	TASK_CHECK(maxDiff > 0.01); // the two scenes really differ
	return 0;
}

int case_lifecycle_repeated_composite_and_base_swap()
{
	Scene sc = sceneInterleaved();
	std::vector<float> base1 = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth1 = makeConstDepth(sc.vpw, sc.vph, 1.0f);

	VolumeRenderer vr;
	vr.setRasterMode(swr::RasterMode::Span);
	float pf[16];
	toFloat16(sc.proj, pf);
	VolumeRenderer::Viewport vp{sc.vpx, sc.vpy, sc.vpw, sc.vph};
	vr.capture(sc.cells.data(), sc.cells.size(), pf, vp);

	VolumeRenderer::Image img1 = vr.composite(base1.data(), depth1.data());
	VolumeRenderer::Image img2 = vr.composite(base1.data(), depth1.data());
	// Repeated compositing of the same capture is bit-exact.
	TASK_CHECK(img1.rgb == img2.rgb);
	TASK_CHECK(img1.transmittance == img2.transmittance);

	// The same capture works with a different base image (color + depth).
	std::vector<float> base2((size_t)sc.vpw * sc.vph * 3, 0.7f);
	std::vector<float> depth2((size_t)sc.vpw * sc.vph, 1.0f);
	for (int y = 0; y < sc.vph; ++y)
		for (int x = 0; x < sc.vpw / 2; ++x)
			depth2[(size_t)y * sc.vpw + x] = 0.45f; // opaque mid-frustum on the left half
	VolumeRenderer::Image img3 = vr.composite(base2.data(), depth2.data());
	RefImage ref3 = renderReference(sc, base2, depth2);
	MaxErr e3 = compareInterior(img3, ref3);
	TASK_CHECK(e3.compared > 30);
	TASK_CHECK(e3.rgb < kTol);
	TASK_CHECK(e3.trans < kTol);

	// And the first base image still composes correctly afterwards.
	VolumeRenderer::Image img4 = vr.composite(base1.data(), depth1.data());
	TASK_CHECK(img4.rgb == img1.rgb);
	TASK_CHECK(img4.transmittance == img1.transmittance);
	return 0;
}

int case_lifecycle_input_release()
{
	Scene sc = sceneInterleaved();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	float pf[16];
	toFloat16(sc.proj, pf);
	VolumeRenderer::Viewport vp{sc.vpx, sc.vpy, sc.vpw, sc.vph};

	VolumeRenderer vr;
	vr.setRasterMode(swr::RasterMode::Block);
	{
		std::vector<VolumeRenderer::TetCell> tmp = sc.cells;
		vr.capture(tmp.data(), tmp.size(), pf, vp);
		// Destroy and poison the caller's array: the capture must own its data.
		for (size_t i = 0; i < tmp.size(); ++i) {
			tmp[i].sigma = -100.0f;
			tmp[i].j[0] = tmp[i].j[1] = tmp[i].j[2] = -100.0f;
			for (int v = 0; v < 4; ++v)
				tmp[i].v[v][0] = tmp[i].v[v][1] = tmp[i].v[v][2] = std::numeric_limits<float>::quiet_NaN();
		}
	}
	VolumeRenderer::Image img = vr.composite(base.data(), depth.data());
	RefImage ref = renderReference(sc, base, depth);
	MaxErr e = compareInterior(img, ref);
	TASK_CHECK(e.compared > 30);
	TASK_CHECK(e.rgb < kTol);
	TASK_CHECK(e.trans < kTol);
	return 0;
}

int case_lifecycle_two_instances()
{
	Scene a = sceneSkewedCell();
	Scene b = sceneInterleaved();
	std::vector<float> baseA = makeBaseRgb(a.vpw, a.vph);
	std::vector<float> depthA = makeConstDepth(a.vpw, a.vph, 1.0f);
	std::vector<float> baseB = makeBaseRgb(b.vpw, b.vph);
	std::vector<float> depthB = makeConstDepth(b.vpw, b.vph, 1.0f);
	float pfA[16], pfB[16];
	toFloat16(a.proj, pfA);
	toFloat16(b.proj, pfB);
	VolumeRenderer::Viewport vpA{a.vpx, a.vpy, a.vpw, a.vph};
	VolumeRenderer::Viewport vpB{b.vpx, b.vpy, b.vpw, b.vph};

	VolumeRenderer ra, rb;
	ra.setRasterMode(swr::RasterMode::Span);
	rb.setRasterMode(swr::RasterMode::Adaptive);

	// Interleave captures and composites of the two instances.
	ra.capture(a.cells.data(), a.cells.size(), pfA, vpA);
	rb.capture(b.cells.data(), b.cells.size(), pfB, vpB);
	VolumeRenderer::Image imgB = rb.composite(baseB.data(), depthB.data());
	VolumeRenderer::Image imgA = ra.composite(baseA.data(), depthA.data());

	RefImage refA = renderReference(a, baseA, depthA);
	RefImage refB = renderReference(b, baseB, depthB);
	MaxErr eA = compareInterior(imgA, refA);
	MaxErr eB = compareInterior(imgB, refB);
	TASK_CHECK(eA.compared > 30 && eB.compared > 30);
	TASK_CHECK(eA.rgb < kTol && eA.trans < kTol);
	TASK_CHECK(eB.rgb < kTol && eB.trans < kTol);

	// Swap the scenes between the instances and interleave again.
	ra.capture(b.cells.data(), b.cells.size(), pfB, vpB);
	rb.capture(a.cells.data(), a.cells.size(), pfA, vpA);
	VolumeRenderer::Image imgA2 = rb.composite(baseA.data(), depthA.data());
	VolumeRenderer::Image imgB2 = ra.composite(baseB.data(), depthB.data());
	MaxErr eA2 = compareInterior(imgA2, refA);
	MaxErr eB2 = compareInterior(imgB2, refB);
	TASK_CHECK(eA2.rgb < kTol && eA2.trans < kTol);
	TASK_CHECK(eB2.rgb < kTol && eB2.trans < kTol);
	return 0;
}

int expectInvalidCapture(VolumeRenderer &vr, const VolumeRenderer::TetCell *cells, size_t count,
                         const float *proj, const VolumeRenderer::Viewport &vp)
{
	try {
		vr.capture(cells, count, proj, vp);
	} catch (const std::invalid_argument &) {
		return 0;
	}
	return 1; // must have thrown std::invalid_argument
}

int case_lifecycle_invalid_keeps_capture()
{
	Scene sc = sceneSkewedCell();
	std::vector<float> base = makeBaseRgb(sc.vpw, sc.vph);
	std::vector<float> depth = makeConstDepth(sc.vpw, sc.vph, 1.0f);
	float pf[16];
	toFloat16(sc.proj, pf);
	VolumeRenderer::Viewport vp{sc.vpx, sc.vpy, sc.vpw, sc.vph};

	VolumeRenderer vr;
	vr.capture(sc.cells.data(), sc.cells.size(), pf, vp);
	TASK_CHECK(vr.hasCapture());
	TASK_CHECK(vr.capturedCellCount() == sc.cells.size());

	VolumeRenderer::TetCell good = sc.cells[0];

	// Coplanar vertices.
	VolumeRenderer::TetCell coplanar = good;
	for (int v = 0; v < 4; ++v)
		coplanar.v[v][2] = -3.0f;
	TASK_CHECK(expectInvalidCapture(vr, &coplanar, 1, pf, vp) == 0);

	// Negative sigma.
	VolumeRenderer::TetCell negSigma = good;
	negSigma.sigma = -0.5f;
	TASK_CHECK(expectInvalidCapture(vr, &negSigma, 1, pf, vp) == 0);

	// Negative / NaN emission.
	VolumeRenderer::TetCell negJ = good;
	negJ.j[1] = -0.1f;
	TASK_CHECK(expectInvalidCapture(vr, &negJ, 1, pf, vp) == 0);
	VolumeRenderer::TetCell nanJ = good;
	nanJ.j[0] = std::numeric_limits<float>::quiet_NaN();
	TASK_CHECK(expectInvalidCapture(vr, &nanJ, 1, pf, vp) == 0);

	// NaN vertex.
	VolumeRenderer::TetCell nanV = good;
	nanV.v[2][1] = std::numeric_limits<float>::quiet_NaN();
	TASK_CHECK(expectInvalidCapture(vr, &nanV, 1, pf, vp) == 0);

	// Null array with non-zero count.
	TASK_CHECK(expectInvalidCapture(vr, nullptr, 3, pf, vp) == 0);

	// Non-perspective projection (clip.w != -view.z) and singular matrix.
	float ortho[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 1};
	TASK_CHECK(expectInvalidCapture(vr, &good, 1, ortho, vp) == 0);
	float singular[16] = {};
	TASK_CHECK(expectInvalidCapture(vr, &good, 1, singular, vp) == 0);
	float nanProj[16];
	toFloat16(sc.proj, nanProj);
	nanProj[5] = std::numeric_limits<float>::quiet_NaN();
	TASK_CHECK(expectInvalidCapture(vr, &good, 1, nanProj, vp) == 0);

	// Bad viewport dimensions.
	VolumeRenderer::Viewport badVp{0, 0, 0, 32};
	TASK_CHECK(expectInvalidCapture(vr, &good, 1, pf, badVp) == 0);
	badVp = VolumeRenderer::Viewport{0, 0, 44, -2};
	TASK_CHECK(expectInvalidCapture(vr, &good, 1, pf, badVp) == 0);

	// The rejected captures must not have touched the previous capture.
	TASK_CHECK(vr.capturedCellCount() == sc.cells.size());
	VolumeRenderer::Image img = vr.composite(base.data(), depth.data());
	RefImage ref = renderReference(sc, base, depth);
	MaxErr e = compareInterior(img, ref);
	TASK_CHECK(e.compared > 30);
	TASK_CHECK(e.rgb < kTol);
	TASK_CHECK(e.trans < kTol);

	// An empty capture is legal and preserves the base image exactly.
	vr.capture(nullptr, 0, pf, vp);
	TASK_CHECK(vr.hasCapture());
	TASK_CHECK(vr.capturedCellCount() == 0);
	VolumeRenderer::Image empty = vr.composite(base.data(), depth.data());
	TASK_CHECK(empty.rgb.size() == base.size());
	for (size_t i = 0; i < base.size(); ++i)
		TASK_CHECK(empty.rgb[i] == base[i]);
	for (size_t i = 0; i < empty.transmittance.size(); ++i)
		TASK_CHECK(empty.transmittance[i] == 1.0f);
	return 0;
}

} // end anonymous namespace

const struct task_case task_cases[] = {
	{"capture", "skewed_perspective_cell", case_capture_skewed_perspective_cell},
	{"capture", "near_far_clipping", case_capture_near_far_clipping},
	{"capture", "camera_inside_cell", case_capture_camera_inside},
	{"capture", "coplanar_neighbors", case_capture_coplanar_neighbors},
	{"capture", "clip_fan", case_capture_clip_fan},
	{"overlap", "interleaved_media", case_overlap_interleaved_media},
	{"overlap", "zero_extinction_emission", case_overlap_zero_extinction_emission},
	{"overlap", "thin_interval", case_overlap_thin_interval},
	{"overlap", "swapped_submission", case_overlap_swapped_submission},
	{"overlap", "opaque_occluder", case_overlap_opaque_occluder},
	{"lifecycle", "recapture_replaces", case_lifecycle_recapture_replaces},
	{"lifecycle", "repeated_composite_and_base_swap", case_lifecycle_repeated_composite_and_base_swap},
	{"lifecycle", "input_release", case_lifecycle_input_release},
	{"lifecycle", "two_instances", case_lifecycle_two_instances},
	{"lifecycle", "invalid_keeps_capture", case_lifecycle_invalid_keeps_capture},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
