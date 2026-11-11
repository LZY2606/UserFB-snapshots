/*
 * Distance-field regression checks for msdfgen core.
 *
 * This driver is intentionally self-contained: the reference geometry math
 * below (point/polyline distance, winding number, adaptive Bezier subdivision
 * with an explicit error bound, bilinear reconstruction) does not call any
 * production scanline, distance selector or rasterization code. The only
 * library entry points exercised are the public generateSDF / generateMSDF /
 * generateMTSDF / renderSDF APIs and the Shape / Contour / EdgeHolder
 * constructors.
 *
 * Sign convention used throughout (verified against the public API):
 *   texture value v = 0.5 + d / rangeWidth, where d < 0 means inside.
 * Values are deliberately compared unclamped; the generator does not clamp.
 */

#include "msdfgen.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace msdfgen;

namespace {

// Samples closer than this (shape units) to the boundary are excluded from
// comparisons; the polyline reference error is always far below it.
const double MARGIN = 0.60;
// Numeric comparisons only cover distances still inside the mapped [-2, 2]
// range window (Range(4.0)); beyond it the texture is legitimately saturated.
const double NUMERIC_BAND = 1.75;
const double RANGE_WIDTH = 4.0; // Range(4.0) => lower -2, upper 2
const double FLATTEN_TARGET = 2.0e-4; // guaranteed curve-to-polyline bound
const double FLOAT_TOL = 4.0e-6;      // float32 rounding headroom on values ~ 1

int g_failures = 0;
std::string g_firstSignMismatch;
std::string g_firstRenderMismatch;

void recordFailure(const std::string &line) {
    if (g_failures == 0) printf("FAIL %s\n", line.c_str());
    ++g_failures;
}

// ---------------------------------------------------------------- geometry --

struct RPoint { double x, y; };
struct RSeg { int kind; RPoint p[4]; }; // kind 1 = line, 2 = quad, 3 = cubic
typedef std::vector<RSeg> RContour;

double dot(double ax, double ay, double bx, double by) { return ax*bx+ay*by; }

double distToFiniteLine(RPoint q, RPoint a, RPoint b) {
    double abx = b.x-a.x, aby = b.y-a.y;
    double denom = dot(abx, aby, abx, aby);
    double t = denom > 0 ? dot(q.x-a.x, q.y-a.y, abx, aby)/denom : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    double dx = q.x-(a.x+t*abx), dy = q.y-(a.y+t*aby);
    return std::sqrt(dx*dx+dy*dy);
}

double edgeLen(RPoint a, RPoint b) {
    return std::sqrt((b.x-a.x)*(b.x-a.x)+(b.y-a.y)*(b.y-a.y));
}

// de Casteljau subdivision at t = 1/2.
void splitHalf(const RSeg &s, RSeg &left, RSeg &right) {
    int n = s.kind;
    RPoint q[4];
    for (int i = 0; i <= n; ++i) q[i] = s.p[i];
    left.kind = right.kind = n;
    left.p[0] = q[0];
    for (int k = 1; k <= n; ++k) {
        for (int i = 0; i <= n-k; ++i) {
            q[i].x = 0.5*(q[i].x+q[i+1].x);
            q[i].y = 0.5*(q[i].y+q[i+1].y);
        }
        left.p[k] = q[0];
        right.p[n-k] = q[n-k];
    }
    right.p[n] = s.p[n];
}

/*
 * Recursively flattens one contour into a polyline. A degree-n Bezier curve
 * lies within (n-1)/n of the longest edge of its control polygon's length from
 * the joining chord, so after each half-split the local curve-to-chord
 * deviation is bounded by (n-1)/n * maxControlPolygonEdge. Subdivision stops
 * only when that bound is <= FLATTEN_TARGET, hence every emitted chord
 * approximates its curve within FLATTEN_TARGET and the bound is exact, not
 * empirical. Linear segments contribute their exact chord.
 */
void flattenContour(const RContour &contour, std::vector<RPoint> &poly) {
    for (size_t ei = 0; ei < contour.size(); ++ei) {
        const RSeg &s = contour[ei];
        if (s.kind == 1) {
            if (poly.empty()) poly.push_back(s.p[0]);
            poly.push_back(s.p[1]);
            continue;
        }
        // Iterative DFS over subdivisions, depth-bounded by geometric halving.
        std::vector<RSeg> stack;
        stack.push_back(s);
        while (!stack.empty()) {
            RSeg cur = stack.back();
            stack.pop_back();
            int n = cur.kind;
            double maxEdge = 0;
            for (int k = 0; k < n; ++k) maxEdge = std::max(maxEdge, edgeLen(cur.p[k], cur.p[k+1]));
            double bound = (n-1.0)/n*maxEdge;
            if (bound <= FLATTEN_TARGET) {
                if (poly.empty()) poly.push_back(cur.p[0]);
                poly.push_back(cur.p[n]);
                continue;
            }
            RSeg left, right;
            splitHalf(cur, left, right);
            stack.push_back(right);
            stack.push_back(left);
        }
    }
}

double polylineDistance(RPoint q, const std::vector<RPoint> &poly) {
    double best = 1e300;
    for (size_t i = 0; i+1 < poly.size(); ++i)
        best = std::min(best, distToFiniteLine(q, poly[i], poly[i+1]));
    return best;
}

// Independent nonzero winding number via a ray cast toward +x.
int winding(RPoint q, const std::vector<RPoint> &poly) {
    int wn = 0;
    for (size_t i = 0; i+1 < poly.size(); ++i) {
        RPoint a = poly[i], b = poly[i+1];
        if ((a.y <= q.y && b.y > q.y) || (b.y <= q.y && a.y > q.y)) {
            double xint = a.x+(q.y-a.y)*(b.x-a.x)/(b.y-a.y);
            if (xint > q.x) wn += (b.y > a.y) ? 1 : -1;
        }
    }
    return wn;
}

struct RefField {
    std::vector<double> d;     // signed distance, negative inside
    std::vector<char> inside;  // summed winding != 0
    int w, h;
    double refError;
};

RefField referenceField(const std::vector<RContour> &contours, int w, int h, double scale) {
    std::vector<std::vector<RPoint> > polys(contours.size());
    bool anyCurve = false;
    for (size_t i = 0; i < contours.size(); ++i) {
        flattenContour(contours[i], polys[i]);
        for (size_t j = 0; j < contours[i].size(); ++j)
            if (contours[i][j].kind != 1) anyCurve = true;
    }
    RefField f;
    f.w = w; f.h = h;
    f.refError = anyCurve ? FLATTEN_TARGET : 0.0;
    f.d.resize(w*h);
    f.inside.resize(w*h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        RPoint q = { (x+0.5)/scale, (y+0.5)/scale };
        int wn = 0;
        double dd = 1e300;
        for (size_t i = 0; i < polys.size(); ++i) {
            wn += winding(q, polys[i]);
            dd = std::min(dd, polylineDistance(q, polys[i]));
        }
        size_t k = (size_t) y*w+x;
        f.inside[k] = wn != 0;
        f.d[k] = wn != 0 ? -dd : dd;
    }
    return f;
}

// ------------------------------------------------------------- test shapes --

// Asymmetric quadrilateral with four sharp corners of distinct angles.
RContour arrowRef() {
    RContour c(4);
    RPoint a[4] = {{1,1},{9,2},{7,8},{2,6}};
    for (int i = 0; i < 4; ++i) { c[i].kind = 1; c[i].p[0] = a[i]; c[i].p[1] = a[(i+1)%4]; }
    return c;
}
Shape arrowShape() {
    Shape sh; Contour c;
    Point2 a[4] = {Point2(1,1),Point2(9,2),Point2(7,8),Point2(2,6)};
    for (int i = 0; i < 4; ++i) c.addEdge(EdgeHolder(a[i], a[(i+1)%4]));
    sh.addContour(c); return sh;
}

// Outer square with a reversed triangular inner contour (a hole).
std::vector<RContour> holeRef() {
    std::vector<RContour> cs(2);
    RPoint o[4] = {{1,1},{11,1},{11,11},{1,11}};
    cs[0].resize(4);
    for (int i = 0; i < 4; ++i) { cs[0][i].kind = 1; cs[0][i].p[0] = o[i]; cs[0][i].p[1] = o[(i+1)%4]; }
    RPoint tr[3] = {{4,4},{6,9},{8,4}}; // opposite orientation to the outer contour
    cs[1].resize(3);
    for (int i = 0; i < 3; ++i) { cs[1][i].kind = 1; cs[1][i].p[0] = tr[i]; cs[1][i].p[1] = tr[(i+1)%3]; }
    return cs;
}
Shape holeShape() {
    Shape sh;
    Contour outer;
    Point2 o[4] = {Point2(1,1),Point2(11,1),Point2(11,11),Point2(1,11)};
    for (int i = 0; i < 4; ++i) outer.addEdge(EdgeHolder(o[i], o[(i+1)%4]));
    Contour inner;
    Point2 tr[3] = {Point2(4,4),Point2(6,9),Point2(8,4)};
    for (int i = 0; i < 3; ++i) inner.addEdge(EdgeHolder(tr[i], tr[(i+1)%3]));
    sh.addContour(outer); sh.addContour(inner); return sh;
}

// Closed contour where a quadratic and a cubic Bezier meet.
RContour curveRef() {
    RContour c(2);
    c[0].kind = 2; c[0].p[0] = {1,1}; c[0].p[1] = {5,1}; c[0].p[2] = {5,5};
    c[1].kind = 3; c[1].p[0] = {5,5}; c[1].p[1] = {5,9}; c[1].p[2] = {1,9}; c[1].p[3] = {1,1};
    return c;
}
Shape curveShape() {
    Shape sh; Contour c;
    c.addEdge(EdgeHolder(Point2(1,1), Point2(5,1), Point2(5,5)));
    c.addEdge(EdgeHolder(Point2(5,5), Point2(5,9), Point2(1,9), Point2(1,1)));
    sh.addContour(c); return sh;
}

// ------------------------------------------------------- generation checks --

SDFTransformation transformation(double scale) {
    return SDFTransformation(Projection(Vector2(scale, scale), Vector2(0, 0)), Range(RANGE_WIDTH));
}

float median3f(float a, float b, float c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

// Reports max numeric error and any sign mismatch against the reference.
void checkScalarField(const char *name, const Bitmap<float, 1> &bmp, const RefField &ref) {
    // The reference distance can differ from the truth by refError in shape
    // units; factor 2 is conservative headroom on top of the value mapping.
    double tol = 2.0*ref.refError/RANGE_WIDTH+FLOAT_TOL;
    double maxErr = 0;
    int numericSamples = 0, signBad = 0;
    for (int y = 0; y < bmp.height(); ++y) for (int x = 0; x < bmp.width(); ++x) {
        size_t k = (size_t) y*bmp.width()+x;
        if (std::fabs(ref.d[k]) < MARGIN) continue;
        float v = bmp(x, y)[0];
        bool prodInside = v < 0.5f;
        if (prodInside != (bool) ref.inside[k]) {
            ++signBad;
            if (g_firstSignMismatch.empty())
                g_firstSignMismatch = std::string(name)+" at ("+std::to_string(x)+","+std::to_string(y)+")";
            recordFailure(std::string(name)+": sign mismatch at ("+std::to_string(x)+","+std::to_string(y)+")");
        }
        if (std::fabs(ref.d[k]) <= NUMERIC_BAND) {
            double expected = 0.5+ref.d[k]/RANGE_WIDTH;
            maxErr = std::max(maxErr, std::fabs((double) v-expected));
            ++numericSamples;
        }
    }
    printf("[%s] ref bound %.2e, max numeric error %.3e over %d samples (tol %.3e), sign mismatches %d\n",
        name, ref.refError, maxErr, numericSamples, tol, signBad);
    if (maxErr > tol)
        recordFailure(std::string(name)+": numeric error "+std::to_string(maxErr)+" exceeds tolerance "+std::to_string(tol));
}

void checkMedianSign(const char *name, const float *px, int channels, int w, int h, const RefField &ref) {
    int bad = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        size_t k = (size_t) y*w+x;
        if (std::fabs(ref.d[k]) < MARGIN) continue;
        const float *p = px+((size_t) y*w+x)*channels;
        float med = median3f(p[0], p[1], p[2]);
        if ((med < 0.5f) != (bool) ref.inside[k]) {
            ++bad;
            if (g_firstSignMismatch.empty())
                g_firstSignMismatch = std::string(name)+" at ("+std::to_string(x)+","+std::to_string(y)+")";
            recordFailure(std::string(name)+": median sign mismatch at ("+std::to_string(x)+","+std::to_string(y)+")");
        }
    }
    printf("[%s] median inside/outside sign vs winding: %d mismatches (samples >= %.2f units from edge)\n",
        name, bad, MARGIN);
}

void writePGM(const char *path, const Bitmap<float, 1> &bmp) {
    FILE *f = std::fopen(path, "wb");
    if (!f) { recordFailure(std::string("cannot write ")+path); return; }
    std::fprintf(f, "P5\n%d %d\n255\n", bmp.width(), bmp.height());
    for (int y = 0; y < bmp.height(); ++y) for (int x = 0; x < bmp.width(); ++x) {
        unsigned char v = (unsigned char) (bmp(x, y)[0] >= 0.5f ? 255 : 0);
        std::fwrite(&v, 1, 1, f);
    }
    std::fclose(f);
    printf("[pgm] wrote %s (%dx%d hard-threshold MSDF reconstruction)\n", path, bmp.width(), bmp.height());
}

void runGeneration(const std::string &pgmDir) {
    struct Case {
        const char *name;
        std::vector<RContour> refContours;
        Shape (*make)();
        int w, h;
        double scale;
    };
    std::vector<RContour> arrowC; arrowC.push_back(arrowRef());
    std::vector<RContour> curveC; curveC.push_back(curveRef());
    Case cases[] = {
        {"gen.arrow", arrowC,   arrowShape, 40, 40, 4.0},
        {"gen.hole",  holeRef(), holeShape, 48, 48, 4.0},
        {"gen.curve", curveC,   curveShape, 40, 40, 4.0},
    };
    for (const Case &tc : cases) {
        RefField ref = referenceField(tc.refContours, tc.w, tc.h, tc.scale);
        SDFTransformation tr = transformation(tc.scale);

        Bitmap<float, 1> sdf(tc.w, tc.h);
        generateSDF(BitmapSection<float, 1>(sdf), tc.make(), tr);
        checkScalarField((std::string(tc.name)+".sdf").c_str(), sdf, ref);

        Shape colored = tc.make();
        edgeColoringSimple(colored, 3.0);
        Bitmap<float, 3> msdf(tc.w, tc.h);
        generateMSDF(BitmapSection<float, 3>(msdf), colored, tr);
        checkMedianSign((std::string(tc.name)+".msdf").c_str(), &msdf(0, 0)[0], 3, tc.w, tc.h, ref);

        Shape colored2 = tc.make();
        edgeColoringSimple(colored2, 3.0);
        Bitmap<float, 4> mtsdf(tc.w, tc.h);
        generateMTSDF(BitmapSection<float, 4>(mtsdf), colored2, tr);
        checkMedianSign((std::string(tc.name)+".mtsdf-rgb").c_str(), &mtsdf(0, 0)[0], 4, tc.w, tc.h, ref);
        // Alpha carries the true signed distance, never a channel pseudo-distance.
        Bitmap<float, 1> alphaView(tc.w, tc.h);
        for (int y = 0; y < tc.h; ++y) for (int x = 0; x < tc.w; ++x) alphaView(x, y)[0] = mtsdf(x, y)[3];
        checkScalarField((std::string(tc.name)+".mtsdf-alpha").c_str(), alphaView, ref);

        if (std::strcmp(tc.name, "gen.arrow") == 0 || std::strcmp(tc.name, "gen.hole") == 0) {
            Bitmap<float, 1> recon(tc.w*2, tc.h*2);
            renderSDF(BitmapSection<float, 1>(recon), BitmapConstSection<float, 3>(msdf), Range(0), 0.5f);
            writePGM((pgmDir+"/"+tc.name+".pgm").c_str(), recon);
        }
    }
}

// ------------------------------------------------------- render reference --

struct Tex {
    int w, h, channels;
    bool downward;
    std::vector<float> v; // logical order, row 0 = bottom row
    float at(int x, int logicalY, int c) const {
        int stored = downward ? h-1-logicalY : logicalY;
        return v[((size_t) stored*w+x)*channels+c];
    }
};

double clampd(double x, double hi) { return x < 0 ? 0 : (x > hi ? hi : x); }

// Independent bilinear sample at an output pixel center, replicating the
// documented edge-clamping convention of the library interpolation.
void bilinear(const Tex &t, int ox, int oy, int ow, int oh, double out[4]) {
    double sx = (double) t.w/ow, sy = (double) t.h/oh;
    double px = clampd(sx*(ox+0.5), t.w);
    double py = clampd(sy*(oy+0.5), t.h);
    px -= 0.5; py -= 0.5;
    int l = (int) std::floor(px), b = (int) std::floor(py);
    double fx = px-l, fy = py-b;
    int r = l+1, tp = b+1;
    l = (int) clampd(l, t.w-1); r = (int) clampd(r, t.w-1);
    b = (int) clampd(b, t.h-1); tp = (int) clampd(tp, t.h-1);
    for (int c = 0; c < t.channels; ++c) {
        double bottom = (1-fx)*t.at(l, b, c)+fx*t.at(r, b, c);
        double top = (1-fx)*t.at(l, tp, c)+fx*t.at(r, tp, c);
        out[c] = (1-fy)*bottom+fy*top;
    }
}

double median3d(double a, double b, double c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

// Reference reconstruction for one output pixel, following the public
// renderSDF contract: interpolate channels first, median second (MSDF), then
// either the hard threshold or the range-scaled coverage. No clamping of the
// distance field itself happens before the assertion.
double refRender(const Tex &t, int ox, int oy, int ow, int oh,
                 double symRange, double threshold) {
    double s[4]; bilinear(t, ox, oy, ow, oh, s);
    double sd;
    if (t.channels == 1) sd = s[0];
    else sd = median3d(s[0], s[1], s[2]); // 3- or 4-channel: median of RGB only
    if (symRange == 0.0) return sd >= threshold ? 1.0 : 0.0;
    double eff = symRange*(ow+oh)/(t.w+t.h); // public range rescaling convention
    return clampd(eff*(sd-threshold)+0.5, 1.0);
}

void expectRender(const char *name, const Tex &t, const Bitmap<float, 1> &got,
                  int ow, int oh, double range, double thr, bool hard) {
    double maxErr = 0;
    for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
        double want = refRender(t, x, y, ow, oh, range, thr);
        double g = got(x, y)[0];
        if (hard) {
            if (g != want) {
                if (g_firstRenderMismatch.empty())
                    g_firstRenderMismatch = std::string(name)+" at ("+std::to_string(x)+","+std::to_string(y)+")";
                recordFailure(std::string(name)+": hard value "+std::to_string(g)+" expected "+std::to_string(want)
                    +" at ("+std::to_string(x)+","+std::to_string(y)+")");
                return;
            }
        } else {
            maxErr = std::max(maxErr, std::fabs(g-want));
            if (std::fabs(g-want) > 2.0e-6) {
                if (g_firstRenderMismatch.empty())
                    g_firstRenderMismatch = std::string(name)+" at ("+std::to_string(x)+","+std::to_string(y)+")";
                recordFailure(std::string(name)+": coverage "+std::to_string(g)+" expected "+std::to_string(want)
                    +" at ("+std::to_string(x)+","+std::to_string(y)+")");
                return;
            }
        }
    }
    printf("[%s] %s reconstruction matches independent reference, max error %.3e\n",
        name, hard ? "hard-threshold" : "ranged-coverage", maxErr);
}

BitmapConstSection<float, 1> section1(const Tex &t) {
    const float *base = t.v.data();
    if (t.downward) base += (size_t) t.w*(t.h-1);
    return BitmapConstSection<float, 1>(base, t.w, t.h, t.downward ? -t.w : t.w,
        t.downward ? Y_DOWNWARD : Y_UPWARD);
}
BitmapConstSection<float, 3> section3(const Tex &t) {
    const float *base = t.v.data();
    if (t.downward) base += (size_t) 3*t.w*(t.h-1);
    return BitmapConstSection<float, 3>(base, t.w, t.h, t.downward ? -3*t.w : 3*t.w,
        t.downward ? Y_DOWNWARD : Y_UPWARD);
}
BitmapConstSection<float, 4> section4(const Tex &t) {
    const float *base = t.v.data();
    if (t.downward) base += (size_t) 4*t.w*(t.h-1);
    return BitmapConstSection<float, 4>(base, t.w, t.h, t.downward ? -4*t.w : 4*t.w,
        t.downward ? Y_DOWNWARD : Y_UPWARD);
}

void runRender() {
    // -- 2x2 one-channel texture: hard threshold and ranged coverage --------
    // Values are picked so no output pixel lands exactly on the threshold;
    // the smallest hard-decision margin is 0.025.
    {
        Tex t{2, 2, 1, false, {0.20f, 0.85f, 0.40f, 0.95f}};
        Bitmap<float, 1> hard(5, 3), soft(5, 3); // non-square, non-integer upscale
        renderSDF(BitmapSection<float, 1>(hard), section1(t), Range(0), 0.5f);
        renderSDF(BitmapSection<float, 1>(soft), section1(t), Range(2.0), 0.5f);
        expectRender("render.nonsquare.hard1", t, hard, 5, 3, 0.0, 0.5, true);
        expectRender("render.nonsquare.soft1", t, soft, 5, 3, 2.0, 0.5, false);
    }

    // -- 2x2 RGB texture deliberately violating median/interpolation order --
    // Corner medians: 0.2, 0.2, 0.2, 0.9. At the exact four-way blend (output
    // pixel (2,2) of the 5x5 target) median(interpolated RGB) is 0.55 while
    // interpolating the four corner medians gives 0.375, so an implementation
    // with the two operations swapped is provably wrong.
    {
        Tex t{2, 2, 3, false, {
            0.9f,0.2f,0.2f,  0.2f,0.9f,0.2f,
            0.2f,0.2f,0.9f,  0.9f,0.9f,0.9f }};
        Bitmap<float, 1> hard(5, 5), soft(5, 5);
        renderSDF(BitmapSection<float, 1>(hard), section3(t), Range(0), 0.5f);
        renderSDF(BitmapSection<float, 1>(soft), section3(t), Range(2.0), 0.5f);
        expectRender("render.anti_order.hard", t, hard, 5, 5, 0.0, 0.5, true);
        expectRender("render.anti_order.soft", t, soft, 5, 5, 2.0, 0.5, false);
    }

    // -- Same scenario through the 4-channel overload (median ignores A) -----
    {
        Tex t{2, 2, 4, false, {
            0.9f,0.2f,0.2f,0.01f,  0.2f,0.9f,0.2f,0.99f,
            0.2f,0.2f,0.9f,0.50f,  0.9f,0.9f,0.9f,0.10f }};
        Bitmap<float, 1> hard(5, 5);
        renderSDF(BitmapSection<float, 1>(hard), section4(t), Range(0), 0.5f);
        expectRender("render.anti_order.rgba4", t, hard, 5, 5, 0.0, 0.5, true);
    }

    // -- 3x2 asymmetric texture and Y-axis orientation -----------------------
    {
        Tex up{3, 2, 1, false, {0.1f,0.9f,0.2f, 0.8f,0.3f,0.7f}};
        Bitmap<float, 1> oup(6, 4);
        renderSDF(BitmapSection<float, 1>(oup), section1(up), Range(0), 0.5f);
        expectRender("render.yorient.up", up, oup, 6, 4, 0.0, 0.5, true);

        Tex down = up; down.downward = true;
        Bitmap<float, 1> odown(6, 4);
        renderSDF(BitmapSection<float, 1>(odown), section1(down), Range(0), 0.5f);
        expectRender("render.yorient.down", down, odown, 6, 4, 0.0, 0.5, true);

        bool differs = false;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 6; ++x)
            if (oup(x, y)[0] != odown(x, y)[0]) differs = true;
        if (!differs) recordFailure("render.yorient: downward interpretation is indistinguishable from upward");
        else printf("[render.yorient] upward/downward reconstructions differ as designed\n");
    }

    // -- Downscale (3x2 -> 2x2) ----------------------------------------------
    {
        Tex t{3, 2, 1, false, {0.1f,0.9f,0.2f, 0.8f,0.3f,0.7f}};
        Bitmap<float, 1> soft(2, 2);
        renderSDF(BitmapSection<float, 1>(soft), section1(t), Range(2.0), 0.5f);
        expectRender("render.downscale", t, soft, 2, 2, 2.0, 0.5, false);
    }

    // -- Non-default sdThreshold, hard and ranged ----------------------------
    {
        Tex t{2, 2, 3, false, {
            0.7f,0.1f,0.1f,  0.1f,0.7f,0.1f,
            0.1f,0.1f,0.7f,  0.7f,0.7f,0.7f }};
        Bitmap<float, 1> hard(4, 4), soft(4, 4);
        renderSDF(BitmapSection<float, 1>(hard), section3(t), Range(0), 0.35f);
        renderSDF(BitmapSection<float, 1>(soft), section3(t), Range(3.0), 0.35f);
        expectRender("render.threshold.hard", t, hard, 4, 4, 0.0, 0.35, true);
        expectRender("render.threshold.soft", t, soft, 4, 4, 3.0, 0.35, false);
    }
}

} // namespace

int main(int argc, char **argv) {
    std::string pgmDir = argc > 1 ? argv[1] : ".";
    runGeneration(pgmDir);
    runRender();
    if (g_firstSignMismatch.empty()) printf("[summary] no sign mismatches\n");
    else printf("[summary] first sign mismatch: %s\n", g_firstSignMismatch.c_str());
    if (g_firstRenderMismatch.empty()) printf("[summary] no reconstruction mismatches\n");
    else printf("[summary] first reconstruction mismatch: %s\n", g_firstRenderMismatch.c_str());
    if (g_failures == 0) {
        printf("ALL CHECKS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
