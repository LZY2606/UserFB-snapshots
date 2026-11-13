// Task test suite for the SFT_EVEN_ODD fill rule.
//
// The expected pixel values come from an independent geometric reference
// implemented below: contours are turned into device-space line segments
// (quadratic curves are flattened by a separate de Casteljau subdivision),
// each pixel row is split at every endpoint and edge intersection, and the
// area of every odd crossing interval inside a pixel column is integrated
// analytically from the antiderivative of a linear function. No library
// rasterizer entry is used to produce expected values.

#include "registry.hpp"
#include "font.hpp"
#include "schrift.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using fixture::Bytes;
using fixture::Contour;
using fixture::Point;
using fixture::GlyphBuild;
using fixture::Component;

struct FreeFont { void operator()(SFT_Font *f) const { sft_freefont(f); } };
using Font = std::unique_ptr<SFT_Font, FreeFont>;

struct DPoint { double x, y; };
struct DSeg { DPoint a, b; };

// ---------------------------------------------------------------- geometry

void flatten_quad(DPoint p0, DPoint p1, DPoint p2, int depth,
                  std::vector<DPoint> &out)
{
	// Replicates the library's device-space flatness criterion
	// (is_flat: doubled triangle area <= 2.0) and its recursion depth cap.
	double gx = p1.x - p0.x, gy = p1.y - p0.y;
	double hx = p2.x - p0.x, hy = p2.y - p0.y;
	double area2 = std::abs(gx * hy - hx * gy);
	if (depth >= 10 || area2 <= 2.0) {
		out.push_back(p2);
		return;
	}
	DPoint m01 = { 0.5 * (p0.x + p1.x), 0.5 * (p0.y + p1.y) };
	DPoint m12 = { 0.5 * (p1.x + p2.x), 0.5 * (p1.y + p2.y) };
	DPoint m012 = { 0.5 * (m01.x + m12.x), 0.5 * (m01.y + m12.y) };
	flatten_quad(p0, m01, m012, depth + 1, out);
	flatten_quad(m012, m12, p2, depth + 1, out);
}

// Converts font-unit contours (with on/off curve flags) into device-space
// polygons using the exact transform convention of sft_render: the glyph
// bounding box is rounded to device pixels first, and the pixel-rounded
// corner is what the render matrix translates against.
std::vector<std::vector<DPoint>>
to_device(const std::vector<Contour> &cs, double sx, double sy,
          double xoff, double yoff, bool downward, double upem)
{
	int xmin = cs[0][0].x, xmax = xmin, ymin = cs[0][0].y, ymax = ymin;
	for (const auto &c : cs) for (const auto &p : c) {
		xmin = std::min(xmin, p.x); xmax = std::max(xmax, p.x);
		ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y);
	}
	int box0 = (int) std::floor(xmin * sx / upem + xoff);
	int box1 = (int) std::floor(ymin * sy / upem + yoff);
	int box3 = (int) std::ceil (ymax * sy / upem + yoff);
	double trf0 = sx / upem, trf3, trf4 = xoff - box0, trf5;
	if (downward) {
		trf3 = -sy / upem;
		trf5 = box3 - yoff;
	} else {
		trf3 = sy / upem;
		trf5 = yoff - box1;
	}
	auto mapX = [&](double x) { return x * trf0 + trf4; };
	auto mapY = [&](double y) { return y * trf3 + trf5; };
	std::vector<std::vector<DPoint>> polys;
	for (const auto &c : cs) {
		// Resolve implicit on-curve points exactly like the TrueType decoder.
		std::vector<Point> pts = c;
		if (!pts.empty() && !pts.front().on && !pts.back().on) {
			Point mid = { (pts.front().x + pts.back().x) / 2,
			              (pts.front().y + pts.back().y) / 2, true };
			pts.push_back(mid);
			std::rotate(pts.begin(), pts.end() - 1, pts.end());
		}
		std::vector<DPoint> poly;
		DPoint start = { mapX(pts[0].x), mapY(pts[0].y) };
		poly.push_back(start);
		DPoint loose = start, beg = start;
		bool gotCtrl = false; DPoint ctrl{};
		for (size_t i = 1; i < pts.size(); ++i) {
			DPoint cur = { mapX(pts[i].x), mapY(pts[i].y) };
			if (pts[i].on) {
				if (gotCtrl) {
					flatten_quad(beg, ctrl, cur, 0, poly);
				} else {
					poly.push_back(cur);
				}
				beg = cur; gotCtrl = false;
			} else {
				if (gotCtrl) {
					DPoint center = { 0.5 * (ctrl.x + cur.x),
					                 0.5 * (ctrl.y + cur.y) };
					flatten_quad(beg, ctrl, center, 0, poly);
					beg = center;
				}
				ctrl = cur; gotCtrl = true;
			}
		}
		if (gotCtrl) flatten_quad(beg, ctrl, loose, 0, poly);
		polys.push_back(poly);
	}
	return polys;
}

// ------------------------------------------------- independent parity area

struct RSeg { double p, q, y0, y1; }; // x(y) = p*y + q

// Integral over [ya,yb] of the odd-region width of one parity interval inside
// pixel column [c,c+1]. Boundary lines are linear in y; root splitting makes
// every integration piece linear, so midpoint x length is exact.
double column_area(const RSeg &l, const RSeg &r, double ya, double yb,
                   double c)
{
	double lo = c, hi = c + 1.0;
	std::vector<double> ys = { ya, yb };
	auto root = [](double p, double q, double v) {
		return p == 0.0 ? 1e300 : (v - q) / p;
	};
	for (const RSeg *s : { &l, &r }) {
		for (double v : { lo, hi }) {
			double y = root(s->p, s->q, v);
			if (y > ya && y < yb) ys.push_back(y);
		}
	}
	std::sort(ys.begin(), ys.end());
	ys.erase(std::unique(ys.begin(), ys.end(),
	      [](double a, double b) { return std::abs(a - b) <= 1e-12; }),
	     ys.end());
	double area = 0.0;
	for (size_t i = 0; i + 1 < ys.size(); ++i) {
		double a = ys[i], b = ys[i + 1], m = 0.5 * (a + b);
		double xl = l.p * m + l.q, xr = r.p * m + r.q;
		double left = std::max(xl, lo), right = std::min(xr, hi);
		if (right > left) area += (right - left) * (b - a);
	}
	return area;
}

std::vector<double> reference_areas(const std::vector<std::vector<DPoint>> &polys,
                                    int width, int height)
{
	std::vector<DSeg> edges;
	for (const auto &poly : polys) {
		for (size_t i = 0; i < poly.size(); ++i) {
			DSeg e = { poly[i], poly[(i + 1) % poly.size()] };
			if (e.a.y != e.b.y) edges.push_back(e);
		}
	}
	std::vector<double> areas((size_t) width * height, 0.0);
	for (int row = 0; row < height; ++row) {
		double bandLo = (double) row, bandHi = bandLo + 1.0;
		std::vector<RSeg> segs;
		for (const auto &e : edges) {
			double y0 = std::max(bandLo, std::min(e.a.y, e.b.y));
			double y1 = std::min(bandHi, std::max(e.a.y, e.b.y));
			if (y1 <= bandLo || y0 >= bandHi) continue;
			double p = (e.b.x - e.a.x) / (e.b.y - e.a.y);
			segs.push_back({ p, e.a.x - p * e.a.y, y0, y1 });
		}
		if (segs.empty()) continue;
		std::vector<double> events = { bandLo, bandHi };
		for (const auto &s : segs) {
			if (s.y0 > bandLo && s.y0 < bandHi) events.push_back(s.y0);
			if (s.y1 > bandLo && s.y1 < bandHi) events.push_back(s.y1);
		}
		for (size_t i = 0; i < segs.size(); ++i)
			for (size_t j = i + 1; j < segs.size(); ++j) {
				double dp = segs[i].p - segs[j].p;
				if (dp == 0.0) continue;
				double y = (segs[j].q - segs[i].q) / dp;
				if (y > bandLo && y < bandHi) events.push_back(y);
			}
		std::sort(events.begin(), events.end());
		for (size_t k = 0; k + 1 < events.size(); ++k) {
			double ya = events[k], yb = events[k + 1];
			if (yb <= ya) continue;
			double ym = 0.5 * (ya + yb);
			std::vector<std::pair<double, int>> cross;
			for (size_t i = 0; i < segs.size(); ++i) {
				if (segs[i].y0 <= ym && segs[i].y1 >= ym)
					cross.push_back({ segs[i].p * ym + segs[i].q, (int) i });
			}
			std::sort(cross.begin(), cross.end());
			for (size_t i = 0; i + 1 < cross.size(); i += 2) {
				const RSeg &l = segs[cross[i].second];
				const RSeg &r = segs[cross[i + 1].second];
				double x0 = std::min(l.p * ya + l.q, l.p * yb + l.q);
				double x1 = std::max(r.p * ya + r.q, r.p * yb + r.q);
				int c0 = std::max(0, (int) std::floor(x0));
				int c1 = std::min(width - 1, (int) std::floor(x1));
				for (int c = c0; c <= c1; ++c)
					areas[(size_t) row * width + c] +=
						column_area(l, r, ya, yb, (double) c);
			}
		}
	}
	return areas;
}

std::vector<unsigned char> reference_gray(
		const std::vector<std::vector<DPoint>> &polys, int width, int height)
{
	auto areas = reference_areas(polys, width, height);
	std::vector<unsigned char> out(areas.size());
	for (size_t i = 0; i < areas.size(); ++i) {
		double a = std::max(0.0, std::min(1.0, areas[i]));
		out[i] = (unsigned char) std::floor(a * 255.0 + 0.5);
	}
	return out;
}

// ------------------------------------------------------------- library glue

struct RenderArgs {
	double sx = 1.0, sy = 1.0, xoff = 0.0, yoff = 0.0;
	int flags = SFT_EVEN_ODD;
	int width = 0, height = 0; // 0 means use gmetrics minimum
};

std::vector<unsigned char>
render_font(const Bytes &bytes, SFT_Glyph glyph, const RenderArgs &a,
            SFT_GMetrics *gotMetrics = nullptr, int *rcOut = nullptr)
{
	Font f(sft_loadmem(bytes.data(), bytes.size()));
	if (!f) throw std::runtime_error("loadmem failed");
	SFT s = { f.get(), a.sx, a.sy, a.xoff, a.yoff, a.flags };
	SFT_GMetrics m{};
	if (sft_gmetrics(&s, glyph, &m) < 0) throw std::runtime_error("gmetrics failed");
	if (gotMetrics) *gotMetrics = m;
	int w = a.width ? a.width : m.minWidth;
	int h = a.height ? a.height : m.minHeight;
	std::vector<unsigned char> px((size_t) w * h, 0xA5);
	int rc = sft_render(&s, glyph, { px.data(), w, h });
	if (rcOut) *rcOut = rc;
	if (rc < 0) return {};
	return px;
}

std::vector<unsigned char>
render_contours(const std::vector<Contour> &cs, const RenderArgs &a,
                SFT_GMetrics *m = nullptr)
{
	return render_font(fixture::font(cs), 1, a, m);
}

unsigned gray_of(double area)
{
	return (unsigned) std::floor(std::max(0.0, std::min(1.0, area)) * 255.0 + 0.5);
}

// Assert full-image equality against the independent reference.
void check_vs_ref(Case &t, const std::vector<Contour> &cs, const RenderArgs &a)
{
	auto lib = render_contours(cs, a);
	int w = a.width, h = a.height;
	if (!w || !h) {
		SFT_GMetrics m;
		render_contours(cs, a, &m);
		w = m.minWidth; h = m.minHeight;
	}
	auto dev = to_device(cs, a.sx, a.sy, a.xoff, a.yoff,
	                     (a.flags & SFT_DOWNWARD_Y) != 0, 64.0);
	auto ref = reference_gray(dev, w, h);
	t.check(lib.size() == ref.size(), "size mismatch vs reference");
	unsigned worst = 0;
	for (size_t i = 0; i < ref.size(); ++i)
		worst = std::max(worst, (unsigned) std::abs((int) lib[i] - (int) ref[i]));
	t.check(worst <= 1, "pixel differs from geometric reference by >1");
}

Contour sq(int x0, int y0, int x1, int y1)
{
	return { {x0,y0,true}, {x1,y0,true}, {x1,y1,true}, {x0,y1,true} };
}

// =================================================================== parity

void test_bowtie(Case &t)
{
	// (0,0),(1,1),(0,1),(1,0): two lobes, total area 1/2 in pixel (0,0).
	auto px = render_contours({{{0,0,true},{64,64,true},{0,64,true},{64,0,true}}}, {});
	t.check(px.size() == 4, "bowtie metrics size");
	t.check(px[0] == 128, "bowtie lobe pixel must be 128");
	t.check(px[1] == 0 && px[2] == 0 && px[3] == 0, "bowtie outside pixels zero");
}

void test_repeated_squares(Case &t)
{
	Contour s = sq(0, 0, 64, 64);
	auto two = render_contours({s, s}, {});
	auto three = render_contours({s, s, s}, {});
	auto one = render_contours({s}, {});
	t.check(two[0] == 0, "two coincident squares must cancel");
	t.check(three[0] == 255, "three coincident squares must fill");
	t.check(one[0] == 255, "single square fills");
}

void test_orientation_and_start(Case &t)
{
	Contour bow = {{0,0,true},{64,64,true},{0,64,true},{64,0,true}};
	Contour rev = bow; std::reverse(rev.begin(), rev.end());
	Contour rot = bow; std::rotate(rot.begin(), rot.begin() + 2, rot.end());
	auto p0 = render_contours({bow}, {});
	auto p1 = render_contours({rev}, {});
	auto p2 = render_contours({rot}, {});
	t.check(p0 == p1, "reversed contour changes parity image");
	t.check(p0 == p2, "rotated start point changes parity image");
	Contour s = sq(0, 0, 64, 64);
	Contour sr = s; std::reverse(sr.begin(), sr.end());
	t.check(render_contours({s}, {}) == render_contours({sr}, {}),
	        "reversed square differs");
}

void test_touching(Case &t)
{
	// Two unit squares sharing a full edge: the shared edge has no area, so
	// the two filled cells stay fully covered and nothing cancels.
	SFT_GMetrics m;
	auto edge = render_contours({sq(0,0,64,64), sq(0,64,64,128)}, {}, &m);
	int W = m.minWidth, H = m.minHeight;
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			unsigned char v = edge[(size_t) y * W + x];
			unsigned char expect = (x == 0 && y < 2) ? 255 : 0;
			t.check(v == expect, "edge-sharing squares must stay fully covered");
		}
	check_vs_ref(t, {sq(0,0,64,64), sq(0,64,64,128)}, {});
	// Two half-unit squares touching at a single corner point inside one
	// pixel: the contact point carries no area, so coverage is the union.
	auto corner = render_contours({sq(0,0,32,32), sq(32,32,64,64)}, {});
	t.check(corner[0] == gray_of(0.5), "point contact contributes no area");
	t.check(corner[1] == 0 && corner[2] == 0, "point contact neighbours zero");
	check_vs_ref(t, {sq(0,0,32,32), sq(32,32,64,64)}, {});
}

void test_slivers(Case &t)
{
	// Two disjoint quarter-thickness bars inside one unit pixel: area 1/2.
	Contour bar1 = sq(0, 0, 64, 16);
	Contour bar2 = sq(0, 32, 64, 48);
	auto disjoint = render_contours({bar1, bar2}, {});
	t.check(disjoint[0] == gray_of(0.5), "disjoint slivers parity area");
	// Crossing bars: parity area = 1/4+1/4-2*(1/16) = 3/8.
	Contour hbar = sq(0, 24, 64, 40);
	Contour vbar = sq(24, 0, 40, 64);
	auto crossing = render_contours({hbar, vbar}, {});
	t.check(crossing[0] == gray_of(0.375), "crossing slivers parity area");
	t.check(crossing[0] != disjoint[0],
	        "implementation must distinguish crossing from disjoint slivers");
}

void test_three_layers(Case &t)
{
	// Outer square (0..2), hole (0.5..1.5), island (0.75..1.25): three
	// nesting levels. Each covered 1x1 cell has area
	// 1 - (1/2)^2 + (1/4)^2 = 13/16, and the total fill is
	// 4 - 1 + 1/4 = 13/4.
	Contour outer = sq(0, 0, 128, 128);
	Contour hole = sq(32, 32, 96, 96);
	Contour island = sq(48, 48, 80, 80);
	SFT_GMetrics m;
	auto px = render_contours({outer, hole, island}, {}, &m);
	t.check(m.minWidth == 3 && m.minHeight == 3, "three-layer metrics");
	for (int y = 0; y < 2; ++y)
		for (int x = 0; x < 2; ++x)
			t.check(px[(size_t) y * 3 + x] == gray_of(13.0 / 16.0),
			        "nested ring/island cell coverage");
	// Padding row and column are empty.
	for (int i = 0; i < 3; ++i) {
		t.check(px[2 * 3 + i] == 0, "padding row must stay zero");
		t.check(px[i * 3 + 2] == 0, "padding column must stay zero");
	}
	double total = 0.0;
	for (unsigned char v : px) total += v / 255.0;
	t.check(std::abs(total - 13.0 / 4.0) <= 3.0 / 255.0,
	        "total three-layer fill area");
	check_vs_ref(t, {outer, hole, island}, {});
}

void test_fold_vs_region(Case &t)
{
	// Material on which "fold total coverage last" and "parity region first"
	// necessarily disagree: crossing quarter bars give 3/8, never a folded
	// 0/1 coverage value, while the legacy winding path renders the bowtie
	// to zero and the double square to full coverage.
	Contour hbar = sq(0, 24, 64, 40);
	Contour vbar = sq(24, 0, 40, 64);
	auto parity = render_contours({hbar, vbar}, {});
	RenderArgs legacy{}; legacy.flags = 0;
	auto winding = render_contours({hbar, vbar}, legacy);
	t.check(parity[0] == gray_of(0.375), "parity region pixel");
	t.check(winding[0] != parity[0], "winding and parity must differ here");
}

// ================================================================= geometry

void test_partial_overlap(Case &t)
{
	Contour a = sq(0, 0, 64, 64);
	Contour b = sq(32, 32, 96, 96);
	check_vs_ref(t, {a, b}, {});
}

void test_fractional_self_intersect(Case &t)
{
	Contour bow = { {0,0,true},{80,64,true},{8,64,true},{84,0,true} };
	check_vs_ref(t, {bow}, {});
	Contour bow2 = { {4,2,true},{70,60,true},{2,58,true},{76,6,true} };
	check_vs_ref(t, {bow2}, {});
}

void test_collinear_edges(Case &t)
{
	// Partially overlapping collinear edges, and a fully shared edge.
	check_vs_ref(t, {sq(0,0,64,64), sq(32,64,64,128)}, {});
	check_vs_ref(t, {sq(0,0,64,64), sq(0,64,64,128), sq(10,10,54,54)}, {});
}

void test_fractional_transforms(Case &t)
{
	Contour bow = { {0,0,true},{64,64,true},{0,64,true},{64,0,true} };
	RenderArgs a{};
	a.sx = 1.37; a.sy = 0.83; a.xoff = 0.29; a.yoff = 0.61;
	check_vs_ref(t, {bow}, a);
	Contour pair = sq(0, 0, 64, 64);
	a.flags = SFT_EVEN_ODD | SFT_DOWNWARD_Y;
	check_vs_ref(t, {pair, sq(16,16,80,80)}, a);
}

void test_window_rules(Case &t)
{
	Contour bow = { {0,0,true},{128,128,true},{0,128,true},{128,0,true} };
	RenderArgs full{};
	SFT_GMetrics m;
	auto big = render_contours({bow}, full, &m);
	int W = m.minWidth, H = m.minHeight;
	// Smaller positive-size image = top-left sub-rectangle of the full bitmap.
	RenderArgs small{}; small.width = W - 1; small.height = H - 2;
	auto sub = render_contours({bow}, small);
	bool ok = sub.size() == (size_t) small.width * small.height;
	for (int y = 0; y < small.height && ok; ++y)
		for (int x = 0; x < small.width; ++x)
			if (sub[(size_t) y * small.width + x] !=
			    big[(size_t) y * W + x]) ok = false;
	t.check(ok, "small image must equal full bitmap sub-rectangle");
	// Larger image: the excess pixels stay zero.
	RenderArgs large{}; large.width = W + 3; large.height = H + 2;
	auto grown = render_contours({bow}, large);
	bool zeros = true;
	for (int y = 0; y < large.height; ++y)
		for (int x = 0; x < large.width; ++x) {
			unsigned char v = grown[(size_t) y * large.width + x];
			bool inside = y < H && x < W;
			if (!inside && v != 0) zeros = false;
			if (inside && v != big[(size_t) y * W + x]) zeros = false;
		}
	t.check(zeros, "pixels outside the metrics rectangle must be zero");
}

void test_curved_glyph(Case &t)
{
	// Curved diamond with a curved hole built from quadratic Beziers.
	Contour outer = {
		{64,0,true},{96,32,false},{128,64,true},{96,96,false},
		{64,128,true},{32,96,false},{0,64,true},{32,32,false}
	};
	Contour hole = {
		{64,40,true},{88,64,false},{64,88,true},{40,64,false}
	};
	RenderArgs a{}; a.sx = a.sy = 2.0;
	check_vs_ref(t, {outer, hole}, a);
	// Self-intersecting curved contour (curved bowtie).
	Contour curvedBow = {
		{0,0,true},{96,32,false},{128,64,true},{32,96,false},
		{0,128,true},{96,96,false},{128,64,true},{32,32,false}
	};
	check_vs_ref(t, {curvedBow}, {});
}

void test_curve_small_window(Case &t)
{
	Contour circle = {
		{64,0,true},{100,0,false},{128,28,false},{128,64,true},
		{128,100,false},{100,128,false},{64,128,true},
		{28,128,false},{0,100,false},{0,64,true},
		{0,28,false},{28,0,false}
	};
	RenderArgs a{}; a.sx = a.sy = 3.0;
	SFT_GMetrics m;
	auto full = render_contours({circle}, a, &m);
	RenderArgs small = a; small.width = 2; small.height = 2;
	auto sub = render_contours({circle}, small);
	bool ok = true;
	for (int y = 0; y < 2; ++y)
		for (int x = 0; x < 2; ++x)
			if (sub[(size_t) y * 2 + x] != full[(size_t) y * m.minWidth + x])
				ok = false;
	t.check(ok, "curved geometry through a small window");
}

void test_compound_glyph(Case &t)
{
	// Glyph 1: unit square. Glyph 2: compound placing two copies with a
	// half-unit offset; glyph 3: two fully coincident copies.
	GlyphBuild empty, square;
	square.simple = { sq(0, 0, 64, 64) };
	GlyphBuild shifted, doubled;
	shifted.isCompound = true;
	shifted.compound = { {1,0,0}, {1,32,32} };
	doubled.isCompound = true;
	doubled.compound = { {1,0,0}, {1,0,0} };
	Bytes bytes = fixture::font_multi({empty, square, shifted, doubled},
	                                  {{65,2},{66,3},{67,1}});
	Font f(sft_loadmem(bytes.data(), bytes.size()));
	t.check(f != nullptr, "compound font load");
	SFT s = { f.get(), 1, 1, 0, 0, SFT_EVEN_ODD };
	SFT_Glyph g2 = 0, g3 = 0;
	t.check(sft_lookup(&s, 65, &g2) == 0 && g2 == 2, "lookup compound");
	t.check(sft_lookup(&s, 66, &g3) == 0 && g3 == 3, "lookup doubled compound");
	SFT_GMetrics m2; t.check(sft_gmetrics(&s, g2, &m2) == 0, "gmetrics compound");
	std::vector<unsigned char> px((size_t) m2.minWidth * m2.minHeight, 0);
	t.check(sft_render(&s, g2, {px.data(), m2.minWidth, m2.minHeight}) == 0,
	        "render compound");
	// Independent reference built straight from the component placement.
	std::vector<Contour> refInput = { sq(0,0,64,64), sq(32,32,96,96) };
	auto dev = to_device(refInput, 1, 1, 0, 0, false, 64.0);
	auto ref = reference_gray(dev, m2.minWidth, m2.minHeight);
	unsigned worst = 0;
	for (size_t i = 0; i < ref.size(); ++i)
		worst = std::max(worst, (unsigned) std::abs((int) px[i] - (int) ref[i]));
	t.check(worst <= 1, "compound parity vs geometric reference");
	SFT_GMetrics m3; t.check(sft_gmetrics(&s, g3, &m3) == 0, "gmetrics doubled");
	std::vector<unsigned char> px3((size_t) m3.minWidth * m3.minHeight, 0);
	t.check(sft_render(&s, g3, {px3.data(), m3.minWidth, m3.minHeight}) == 0,
	        "render doubled compound");
	t.check(px3[0] == 0, "coincident compound components cancel");
}

// ============================================================== compatibility

void test_legacy_pixels_unchanged(Case &t)
{
	// The exact grayscale baseline numbers for the 16px rectangle.
	Bytes bytes = fixture::font({{{1,0},{41,0},{41,45},{1,45}}});
	Font f(sft_loadmem(bytes.data(), bytes.size()));
	SFT s = { f.get(), 16, 16, 0, 0, SFT_DOWNWARD_Y };
	SFT_Glyph g = 0;
	t.check(sft_lookup(&s, 65, &g) == 0 && g == 1, "lookup baseline glyph");
	SFT_GMetrics m;
	t.check(sft_gmetrics(&s, g, &m) == 0, "gmetrics baseline");
	t.check(m.advanceWidth == 13 && m.minWidth == 12 && m.minHeight == 13,
	        "baseline metrics");
	std::vector<unsigned char> px((size_t) m.minWidth * m.minHeight, 0);
	t.check(sft_render(&s, g, {px.data(), m.minWidth, m.minHeight}) == 0,
	        "render baseline");
	unsigned count = 0, sum = 0;
	for (unsigned char v : px) { count += v != 0; sum += v; }
	t.check(count == 132 && sum == 28690, "legacy grayscale baseline changed");
}

void test_simple_shapes_agree(Case &t)
{
	// For non-self-intersecting, non-overlapping geometry both rules agree.
	Contour square = sq(4, 4, 60, 60);
	Contour tri = { {0,0,true},{64,0,true},{64,64,true} };
	Contour curve = {
		{64,0,true},{96,32,false},{128,64,true},{96,96,false},
		{64,128,true},{32,96,false},{0,64,true},{32,32,false}
	};
	for (const Contour *c : { &square, &tri, &curve }) {
		RenderArgs eo{}, legacy{}; legacy.flags = 0;
		auto a = render_contours({*c}, eo);
		auto b = render_contours({*c}, legacy);
		unsigned worst = 0;
		for (size_t i = 0; i < a.size(); ++i)
			worst = std::max(worst, (unsigned) std::abs((int) a[i] - (int) b[i]));
		t.check(worst <= 1, "parity and winding paths disagree on simple fill");
	}
}

void test_y_directions(Case &t)
{
	Contour bow = { {0,0,true},{96,64,true},{8,64,true},{80,0,true} };
	RenderArgs up{}, down{};
	down.flags = SFT_EVEN_ODD | SFT_DOWNWARD_Y;
	// Each orientation is validated against the independent reference, which
	// applies the same axis-flip transform itself.
	check_vs_ref(t, {bow}, up);
	check_vs_ref(t, {bow}, down);
	SFT_GMetrics mu, md;
	render_contours({bow}, up, &mu);
	render_contours({bow}, down, &md);
	t.check(mu.minWidth == md.minWidth && mu.minHeight == md.minHeight &&
	        mu.advanceWidth == md.advanceWidth,
	        "metrics differ with Y direction");
	// Identity: downward(G) is the upward image of the glyph mirrored about
	// its own vertical bbox centre, since both transforms anchor the bbox at
	// the image origin.
	int ymin = 0, ymax = 64;
	for (const auto &p : bow) { ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y); }
	Contour mirrored = bow;
	for (auto &p : mirrored) p.y = ymin + ymax - p.y;
	auto a = render_contours({bow}, down);
	auto b = render_contours({mirrored}, up);
	t.check(a == b, "downward Y must equal upward render of mirrored glyph");
}

void test_metrics_independent_of_rule(Case &t)
{
	Contour bow = { {0,0,true},{64,64,true},{0,64,true},{64,0,true} };
	SFT_GMetrics a, b;
	render_contours({bow}, {}, &a);
	RenderArgs legacy{}; legacy.flags = SFT_DOWNWARD_Y;
	render_contours({bow}, legacy, &b);
	SFT_GMetrics c;
	RenderArgs both{}; both.flags = SFT_EVEN_ODD | SFT_DOWNWARD_Y;
	render_contours({bow}, both, &c);
	t.check(b.advanceWidth == c.advanceWidth &&
	        b.leftSideBearing == c.leftSideBearing &&
	        b.yOffset == c.yOffset &&
	        b.minWidth == c.minWidth && b.minHeight == c.minHeight,
	        "metrics must not depend on the fill rule flag");
	t.check(a.minWidth == c.minWidth && a.minHeight == c.minHeight,
	        "metric rectangle changed with fill rule");
}

void test_failure_is_atomic(Case &t)
{
	Bytes bytes = fixture::corrupt_then_good_font(std::vector<Contour>{sq(0, 0, 64, 64)});
	Font f(sft_loadmem(bytes.data(), bytes.size()));
	t.check(f != nullptr, "load failure-test font");
	SFT s = { f.get(), 1, 1, 0, 0, SFT_EVEN_ODD };
	SFT_GMetrics m;
	t.check(sft_gmetrics(&s, 1, &m) == 0, "gmetrics on corrupt glyph");
	std::vector<unsigned char> px((size_t) m.minWidth * m.minHeight, 0x7E);
	int rc = sft_render(&s, 1, {px.data(), m.minWidth, m.minHeight});
	t.check(rc == -1, "corrupt outline must fail");
	bool untouched = true;
	for (unsigned char v : px) if (v != 0x7E) untouched = false;
	t.check(untouched, "failed render left a partial image");
	// The same font must still render its healthy glyph afterwards.
	SFT_GMetrics m2;
	t.check(sft_gmetrics(&s, 2, &m2) == 0, "gmetrics after failure");
	std::vector<unsigned char> ok((size_t) m2.minWidth * m2.minHeight, 0);
	t.check(sft_render(&s, 2, {ok.data(), m2.minWidth, m2.minHeight}) == 0,
	        "font unusable after a failed render");
	t.check(ok[0] == 255, "healthy glyph wrong after failure");
}

void test_huge_image_failure(Case &t)
{
	Bytes bytes = fixture::font({sq(0, 0, 64, 64)});
	Font f(sft_loadmem(bytes.data(), bytes.size()));
	SFT s = { f.get(), 1, 1, 0, 0, SFT_EVEN_ODD };
	unsigned char tiny[4] = { 0x33, 0x33, 0x33, 0x33 };
	SFT_Image img = { tiny, 1 << 30, 1 << 30 };
	int rc = sft_render(&s, 1, img);
	t.check(rc == -1, "undrawable image must fail");
	bool untouched = tiny[0] == 0x33 && tiny[1] == 0x33 &&
	                 tiny[2] == 0x33 && tiny[3] == 0x33;
	t.check(untouched, "allocation failure touched the destination");
}

} // namespace

void register_task_tests(Registry &r)
{
	r.add("parity", "bowtie_lobes", test_bowtie);
	r.add("parity", "repeated_squares", test_repeated_squares);
	r.add("parity", "orientation_and_start", test_orientation_and_start);
	r.add("parity", "touching_edges_and_points", test_touching);
	r.add("parity", "sliver_overlap_geometry", test_slivers);
	r.add("parity", "three_layer_hole_island", test_three_layers);
	r.add("parity", "fold_vs_parity_region", test_fold_vs_region);

	r.add("geometry", "partial_overlap", test_partial_overlap);
	r.add("geometry", "fractional_self_intersection", test_fractional_self_intersect);
	r.add("geometry", "collinear_overlapping_edges", test_collinear_edges);
	r.add("geometry", "fractional_nonuniform_transform", test_fractional_transforms);
	r.add("geometry", "window_subrect_and_zero_padding", test_window_rules);
	r.add("geometry", "curved_outlines", test_curved_glyph);
	r.add("geometry", "curve_through_small_window", test_curve_small_window);
	r.add("geometry", "compound_glyph_parity", test_compound_glyph);

	r.add("compatibility", "legacy_grayscale_baseline", test_legacy_pixels_unchanged);
	r.add("compatibility", "simple_shapes_both_rules", test_simple_shapes_agree);
	r.add("compatibility", "both_y_directions", test_y_directions);
	r.add("compatibility", "metrics_independent_of_rule", test_metrics_independent_of_rule);
	r.add("compatibility", "decode_failure_atomic", test_failure_is_atomic);
	r.add("compatibility", "allocation_failure_atomic", test_huge_image_failure);
}
