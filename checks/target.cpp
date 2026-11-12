#include "msdfgen.h"
#include "core/ShapeDistanceFinder.h"
#include "core/ShapeDistanceIndex.h"
#include "harness.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>

using namespace msdfgen;

namespace {

const double PI = acos(-1.0);

// Coordinate scale used for the 1e-9*scale double-distance tolerance.
const double COORD_SCALE = 16.0;
const double DOUBLE_TOL = 1e-9*COORD_SCALE;
const float FLOAT_TOL = 2e-6f;

double refSDF(const Shape &s, Point2 p, bool overlap) {
    if (overlap) {
        ShapeDistanceFinder<OverlappingContourCombiner<TrueDistanceSelector> > f(s);
        return f.distance(p);
    }
    ShapeDistanceFinder<SimpleContourCombiner<TrueDistanceSelector> > f(s);
    return f.distance(p);
}

MultiAndTrueDistance refMT(const Shape &s, Point2 p, bool overlap) {
    if (overlap) {
        ShapeDistanceFinder<OverlappingContourCombiner<MultiAndTrueDistanceSelector> > f(s);
        return f.distance(p);
    }
    ShapeDistanceFinder<SimpleContourCombiner<MultiAndTrueDistanceSelector> > f(s);
    return f.distance(p);
}

std::vector<Point2> gridPoints(double cx, double cy, double half, int steps) {
    std::vector<Point2> pts;
    for (int i = 0; i <= steps; ++i)
        for (int j = 0; j <= steps; ++j) {
            double x = cx-half+2*half*i/steps;
            double y = cy-half+2*half*j/steps;
            pts.push_back(Point2(x, y));
        }
    return pts;
}

std::vector<Point2> farPoints(double cx, double cy) {
    std::vector<Point2> pts;
    for (int k = 0; k < 12; ++k) {
        double ang = 2*PI*k/12;
        double rad = 800.0*(1+(k%3)*37); // well beyond every finite curve box and tangent truncation guess
        pts.push_back(Point2(cx+rad*cos(ang), cy+rad*sin(ang)));
    }
    pts.push_back(Point2(cx+100000, cy));
    pts.push_back(Point2(cx-100000, cy+1));
    pts.push_back(Point2(cx, cy+100000));
    pts.push_back(Point2(cx-50000, cy-50000));
    return pts;
}

void requireClose(double a, double b, double tol, const char *what) {
    if (fabs(a-b) > tol)
        throw std::runtime_error(std::string(what)+": "+std::to_string(a)+" != "+std::to_string(b));
}

Shape makeSquare() {
    Shape s;
    Contour &c = s.addContour();
    c.addEdge(EdgeHolder(Point2(0, 0), Point2(8, 0)));
    c.addEdge(EdgeHolder(Point2(8, 0), Point2(8, 8)));
    c.addEdge(EdgeHolder(Point2(8, 8), Point2(0, 8)));
    c.addEdge(EdgeHolder(Point2(0, 8), Point2(0, 0)));
    return s;
}

Shape makeMixedCurves() {
    Shape s;
    Contour &c = s.addContour();
    c.addEdge(EdgeHolder(Point2(0, 0), Point2(3, -2), Point2(6, 0)));             // quadratic
    c.addEdge(EdgeHolder(Point2(6, 0), Point2(8, 2), Point2(5, 5), Point2(6, 8))); // cubic
    c.addEdge(EdgeHolder(Point2(6, 8), Point2(3, 10), Point2(0, 8)));              // quadratic
    c.addEdge(EdgeHolder(Point2(0, 8), Point2(0, 0)));                             // linear
    return s;
}

Shape makeHole() {
    Shape s;
    Contour &outer = s.addContour();
    outer.addEdge(EdgeHolder(Point2(0, 0), Point2(10, 0)));
    outer.addEdge(EdgeHolder(Point2(10, 0), Point2(10, 10)));
    outer.addEdge(EdgeHolder(Point2(10, 10), Point2(0, 10)));
    outer.addEdge(EdgeHolder(Point2(0, 10), Point2(0, 0)));
    Contour &hole = s.addContour(); // reversed (clockwise) inner contour
    hole.addEdge(EdgeHolder(Point2(3, 3), Point2(3, 7)));
    hole.addEdge(EdgeHolder(Point2(3, 7), Point2(7, 7)));
    hole.addEdge(EdgeHolder(Point2(7, 7), Point2(7, 3)));
    hole.addEdge(EdgeHolder(Point2(7, 3), Point2(3, 3)));
    return s;
}

Shape makeOverlap() {
    Shape s;
    Contour &c1 = s.addContour();
    c1.addEdge(EdgeHolder(Point2(0, 0), Point2(6, 0)));
    c1.addEdge(EdgeHolder(Point2(6, 0), Point2(6, 6)));
    c1.addEdge(EdgeHolder(Point2(6, 6), Point2(0, 6)));
    c1.addEdge(EdgeHolder(Point2(0, 6), Point2(0, 0)));
    Contour &c2 = s.addContour(); // same winding, overlapping
    c2.addEdge(EdgeHolder(Point2(4, 2), Point2(10, 2)));
    c2.addEdge(EdgeHolder(Point2(10, 2), Point2(10, 8)));
    c2.addEdge(EdgeHolder(Point2(10, 8), Point2(4, 8)));
    c2.addEdge(EdgeHolder(Point2(4, 8), Point2(4, 2)));
    return s;
}

Shape makeColoredCorner() {
    Shape s;
    Contour &c = s.addContour();
    c.addEdge(EdgeHolder(Point2(0, 0), Point2(10, 0), RED));
    c.addEdge(EdgeHolder(Point2(10, 0), Point2(10, 10), GREEN));
    c.addEdge(EdgeHolder(Point2(10, 10), Point2(0, 10), BLUE));
    c.addEdge(EdgeHolder(Point2(0, 10), Point2(0, 0), WHITE));
    return s;
}

void writePPM(const std::string &path, const std::vector<unsigned char> &rgb, int w, int h) {
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot write "+path);
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write(reinterpret_cast<const char *>(rgb.data()), w*h*3);
}

unsigned char toByte(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return (unsigned char) (255.0f*v+0.5f);
}

// Compares the pre-correction double distances of every query point against the per-edge reference.
void verifyDoubleDistances(Shape &s, const std::vector<Point2> &pts) {
    edgeColoringSimple(s, 3.0);
    ShapeDistanceIndex idx(s);
    for (int overlap = 0; overlap < 2; ++overlap) {
        for (size_t i = 0; i < pts.size(); ++i) {
            Point2 p = pts[i];
            requireClose(refSDF(s, p, overlap != 0), idx.sdfDistance(p, overlap != 0, NULL), DOUBLE_TOL, "sdf double");
            MultiAndTrueDistance rm = refMT(s, p, overlap != 0);
            MultiAndTrueDistance im = idx.mtsdfDistance(p, overlap != 0, NULL);
            requireClose(rm.r, im.r, DOUBLE_TOL, "mtsdf r");
            requireClose(rm.g, im.g, DOUBLE_TOL, "mtsdf g");
            requireClose(rm.b, im.b, DOUBLE_TOL, "mtsdf b");
            requireClose(rm.a, im.a, DOUBLE_TOL, "mtsdf a");
        }
    }
}

// Compares full float fields generated through both entry points under identical configs.
void verifyFloatFields(Shape &s, int w, int h, const Projection &proj, Range range, bool overlap, const char *tag) {
    edgeColoringSimple(s, 3.0);
    ShapeDistanceIndex idx(s);
    GeneratorConfig gcfg(overlap);
    MSDFGeneratorConfig mcfg(overlap);

    Bitmap<float, 1> sdfRef(w, h), sdfIdx(w, h);
    generateSDF(sdfRef, s, proj, range, gcfg);
    generateSDFIndexed(sdfIdx, idx, proj, range, gcfg, NULL);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = *sdfRef(x, y), b = *sdfIdx(x, y);
            if (fabsf(a-b) > FLOAT_TOL)
                throw std::runtime_error(std::string("float SDF mismatch ")+tag);
            if (!std::isfinite(a) || !std::isfinite(b))
                throw std::runtime_error(std::string("non-finite SDF ")+tag);
        }

    Bitmap<float, 4> mtRef(w, h), mtIdx(w, h);
    generateMTSDF(mtRef, s, proj, range, mcfg);
    generateMTSDFIndexed(mtIdx, idx, proj, range, mcfg, NULL);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 4; ++k) {
                float a = mtRef(x, y)[k], b = mtIdx(x, y)[k];
                if (fabsf(a-b) > FLOAT_TOL)
                    throw std::runtime_error(std::string("float MTSDF mismatch ")+tag);
                if (!std::isfinite(a) || !std::isfinite(b))
                    throw std::runtime_error(std::string("non-finite MTSDF ")+tag);
            }
}

} // namespace

// --------------------------------------------------------------- distance group
CHECK_CASE("distance", double_matches_reference_on_shapes) {
    std::vector<Point2> pts = gridPoints(4, 4, 14, 9);
    std::vector<Point2> far = farPoints(4, 4);
    pts.insert(pts.end(), far.begin(), far.end());
    Shape a = makeSquare();
    Shape b = makeMixedCurves();
    verifyDoubleDistances(a, pts);
    verifyDoubleDistances(b, pts);
}

CHECK_CASE("distance", float_fields_match_under_projection) {
    Shape s = makeMixedCurves();
    // Non-square bitmap, mirrored X scale, translated projection.
    Projection proj(Vector2(-1.3, 0.9), Vector2(7, -3));
    verifyFloatFields(s, 96, 64, proj, Range(6), true, "mirror-nonsquare");
    Projection proj2(Vector2(1.1, 1.1), Vector2(0, 0));
    verifyFloatFields(s, 48, 48, proj2, Range(3), false, "small-range-no-overlap");
}

CHECK_CASE("distance", values_are_not_clamped) {
    Shape s = makeSquare();
    edgeColoringSimple(s, 3.0);
    ShapeDistanceIndex idx(s);
    Projection proj(Vector2(1, 1), Vector2(0, 0));
    Bitmap<float, 1> sdf(16, 16);
    generateSDFIndexed(sdf, idx, proj, Range(2), GeneratorConfig(true), NULL);
    bool sawBelowZero = false, sawAboveOne = false;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            float v = *sdf(x, y);
            if (v < 0) sawBelowZero = true;
            if (v > 1) sawAboveOne = true;
        }
    REQUIRE(sawBelowZero);
    REQUIRE(sawAboveOne);
}

// --------------------------------------------------------------- endpoints group
CHECK_CASE("endpoints", manual_straight_edge_distances) {
    // Square spanning [0,8]^2; all values derived by hand from perpendicular/corner geometry.
    Shape sq = makeSquare();
    ShapeDistanceIndex idx(sq);
    // Interior point nearest the left edge -> -3.
    requireClose(idx.sdfDistance(Point2(3, 4), false, NULL), -3.0, 1e-12, "manual inside");
    // Exterior point level with the right edge -> +4.
    requireClose(idx.sdfDistance(Point2(12, 1), false, NULL), 4.0, 1e-12, "manual exterior x");
    // Exterior point level with the top edge -> +4.
    requireClose(idx.sdfDistance(Point2(5, 12), false, NULL), 4.0, 1e-12, "manual exterior y");
    // Point diagonally outside the (8,8) corner -> +sqrt(8).
    requireClose(idx.sdfDistance(Point2(10, 10), false, NULL), sqrt(8.0), 1e-12, "manual corner");
    // A single contour gives the same answer through the overlap combiner.
    requireClose(idx.sdfDistance(Point2(10, 10), true, NULL), sqrt(8.0), 1e-12, "manual overlap corner");
}

CHECK_CASE("endpoints", manual_pseudo_distance_corner) {
    Shape s = makeColoredCorner();
    ShapeDistanceIndex idx(s);
    // At p=(12,-1), outside the bottom/right corner (10,0):
    //  red   perpendicular = cross((2,-1),(1,0)) = +1
    //  green perpendicular = cross((2,-1),(0,1)) = +2
    //  alpha true distance = sqrt(5)
    MultiAndTrueDistance d = idx.mtsdfDistance(Point2(12, -1), false, NULL);
    requireClose(d.r, 1.0, 1e-12, "pseudo red");
    requireClose(d.g, 2.0, 1e-12, "pseudo green");
    requireClose(d.a, sqrt(5.0), 1e-12, "pseudo alpha");
}

CHECK_CASE("endpoints", tangent_ridge_survives_far_pruning) {
    // The perpendicular pseudo channel stays exactly zero along an endpoint tangent ridge at any
    // distance, so a finite box pruner would corrupt it. The index must keep the ridge exact.
    Shape s = makeColoredCorner();
    ShapeDistanceIndex idx(s);
    for (double k = 1; k <= 100000; k *= 10) {
        Point2 p(10+k, 0); // ridge of the (10,0) corner along +x
        MultiAndTrueDistance d = idx.mtsdfDistance(p, false, NULL);
        MultiAndTrueDistance ref = refMT(s, p, false);
        requireClose(d.b, ref.b, DOUBLE_TOL, "ridge blue");
        requireClose(d.a, ref.a, DOUBLE_TOL, "ridge alpha");
    }
}

CHECK_CASE("endpoints", degenerate_endpoint_tangents) {
    // A cubic with coincident first control points has a zero start tangent, exercising the
    // normalize(true) fallback directions used by both the selector and the index's ray setup.
    Shape s;
    Contour &c = s.addContour();
    c.addEdge(EdgeHolder(Point2(0, 0), Point2(0, 0), Point2(4, 1), Point2(4, 0))); // zero start tangent
    c.addEdge(EdgeHolder(Point2(4, 0), Point2(4, 4)));
    c.addEdge(EdgeHolder(Point2(4, 4), Point2(0, 4)));
    c.addEdge(EdgeHolder(Point2(0, 4), Point2(0, 0)));
    REQUIRE(s.validate());
    std::vector<Point2> pts = gridPoints(2, 2, 12, 7);
    pts.push_back(Point2(-500, 500));
    verifyDoubleDistances(s, pts);
}

// --------------------------------------------------------------- contours group
CHECK_CASE("contours", reversed_hole_uses_overlap_rules) {
    Shape s = makeHole();
    std::vector<Point2> pts = gridPoints(5, 5, 13, 9);
    pts.push_back(Point2(500, -500));
    // verifyDoubleDistances exercises both overlapSupport=true (directed contour merge) and false
    // (simple nearest contour), each against its matching per-edge reference entry point.
    verifyDoubleDistances(s, pts);
}

CHECK_CASE("contours", overlapping_same_winding_contours) {
    Shape s = makeOverlap();
    std::vector<Point2> pts = gridPoints(5, 4, 14, 8);
    pts.push_back(Point2(-900, 900));
    verifyDoubleDistances(s, pts);
}

CHECK_CASE("contours", multicolor_sharp_corner) {
    Shape s = makeColoredCorner();
    std::vector<Point2> pts = gridPoints(5, 5, 14, 9);
    pts.push_back(Point2(2000, 2000));
    verifyDoubleDistances(s, pts);
}

CHECK_CASE("contours", y_axis_orientation_round_trip) {
    Shape up = makeSquare();
    Shape down = makeSquare();
    down.setYAxisOrientation(Y_DOWNWARD);
    edgeColoringSimple(up, 3.0);
    edgeColoringSimple(down, 3.0);
    ShapeDistanceIndex idxUp(up), idxDown(down);
    REQUIRE(idxUp.getShape().getYAxisOrientation() == Y_UPWARD);
    REQUIRE(idxDown.getShape().getYAxisOrientation() == Y_DOWNWARD);
    Projection proj(Vector2(1, 1), Vector2(0, 0));
    Bitmap<float, 1> a(12, 12), b(12, 12);
    generateSDFIndexed(a, idxUp, proj, Range(4), GeneratorConfig(true), NULL);
    generateSDFIndexed(b, idxDown, proj, Range(4), GeneratorConfig(true), NULL);
    // Reoriented output rows must agree.
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 12; ++x)
            requireClose(*a(x, y), *b(x, 11-y), FLOAT_TOL, "y orientation");
}

CHECK_CASE("contours", index_survives_shape_edit_and_destroy) {
    Shape s = makeSquare();
    edgeColoringSimple(s, 3.0);
    ShapeDistanceIndex *heapIdx = new ShapeDistanceIndex(s);
    double before = heapIdx->sdfDistance(Point2(3, 4), true, NULL);
    // Mutate the source shape: add a contour and recolor; the owned snapshot must be unaffected.
    Contour &extra = s.addContour();
    extra.addEdge(EdgeHolder(Point2(20, 20), Point2(24, 20)));
    extra.addEdge(EdgeHolder(Point2(24, 20), Point2(24, 24)));
    extra.addEdge(EdgeHolder(Point2(24, 24), Point2(20, 24)));
    extra.addEdge(EdgeHolder(Point2(20, 24), Point2(20, 20)));
    edgeColoringSimple(s, 3.0);
    double afterEdit = heapIdx->sdfDistance(Point2(3, 4), true, NULL);
    REQUIRE(heapIdx->edgeCount() == 4);
    requireClose(before, afterEdit, 1e-12, "snapshot after edit");
    delete heapIdx; // would dangle if the index referenced the source's edges
    REQUIRE(true);
}

// --------------------------------------------------------------- pruning group
CHECK_CASE("pruning", distant_and_local_queries_are_independent) {
    Shape s = makeMixedCurves();
    edgeColoringSimple(s, 3.0);
    ShapeDistanceIndex idx(s);
    Point2 near(3, 4), far(-12345, 6789);
    double n1 = idx.sdfDistance(near, true, NULL);
    double fjumped = idx.sdfDistance(far, true, NULL);
    double n2 = idx.sdfDistance(near, true, NULL);
    requireClose(n1, n2, 1e-12, "local repeat after jump");
    requireClose(fjumped, refSDF(s, far, true), DOUBLE_TOL, "jumped query");
    MultiAndTrueDistance m1 = idx.mtsdfDistance(near, true, NULL);
    MultiAndTrueDistance mj = idx.mtsdfDistance(far, true, NULL);
    MultiAndTrueDistance m2 = idx.mtsdfDistance(near, true, NULL);
    requireClose(m1.r, m2.r, 1e-12, "mt local repeat after jump");
    requireClose(mj.a, refMT(s, far, true).a, DOUBLE_TOL, "mt jumped alpha");
}

CHECK_CASE("pruning", alternating_projections_share_no_minima) {
    Shape s = makeMixedCurves();
    edgeColoringSimple(s, 3.0);
    ShapeDistanceIndex idx(s);
    Projection pa(Vector2(1, 1), Vector2(0, 0));
    Projection pb(Vector2(-2.5, 1.7), Vector2(9, -4));
    Bitmap<float, 1> a1(40, 40), a2(40, 40), b1(40, 40), b2(40, 40);
    generateSDFIndexed(a1, idx, pa, Range(5), GeneratorConfig(true), NULL);
    generateSDFIndexed(b1, idx, pb, Range(5), GeneratorConfig(true), NULL);
    generateSDFIndexed(a2, idx, pa, Range(5), GeneratorConfig(true), NULL);
    generateSDFIndexed(b2, idx, pb, Range(5), GeneratorConfig(true), NULL);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            requireClose(*a1(x, y), *a2(x, y), 1e-12, "projection A stable");
            requireClose(*b1(x, y), *b2(x, y), 1e-12, "projection B stable");
        }
}

CHECK_CASE("pruning", stats_count_real_work_and_linear_storage) {
    const int N = 512;
    Shape circle;
    Contour &c = circle.addContour();
    double R = 40;
    for (int i = 0; i < N; ++i) {
        double a0 = 2*PI*i/N, a1 = 2*PI*(i+1)/N;
        c.addEdge(EdgeHolder(Point2(R*cos(a0), R*sin(a0)), Point2(R*cos(a1), R*sin(a1))));
    }
    edgeColoringSimple(circle, 3.0);
    ShapeDistanceIndex idx(circle);
    // Linear storage: a binary hierarchy over E edges never needs 2E nodes.
    REQUIRE(idx.nodeCount() <= 2*idx.edgeCount());
    REQUIRE(idx.edgeCount() == N);

    DistanceQueryStats st;
    double d = idx.sdfDistance(Point2(3, 0), true, &st);
    REQUIRE(std::isfinite(d));
    REQUIRE(st.nodeTests > 0);
    REQUIRE(st.leafEdgeVisits > 0);
    REQUIRE(st.exactDistanceCalls > 0);
    REQUIRE(st.exactDistanceCalls <= st.leafEdgeVisits);
    REQUIRE(st.leafEdgeVisits <= idx.edgeCount());
    long long savedNodeTests = st.nodeTests, savedLeaf = st.leafEdgeVisits, savedExact = st.exactDistanceCalls;

    // Construction must not be counted in generation statistics: a fresh stats object starts at zero.
    DistanceQueryStats gen;
    Bitmap<float, 1> sdf(128, 128);
    Projection proj(Vector2(1.2, 1.2), Vector2(-64, -64));
    generateSDFIndexed(sdf, idx, proj, Range(8), GeneratorConfig(true), &gen);
    REQUIRE(gen.nodeTests > 0);
    (void) savedNodeTests; (void) savedLeaf; (void) savedExact;

    long long fullTraversal = 128LL*128*idx.edgeCount();
    std::printf("[pruning] smooth contour edges=%d nodes=%d storagePerEdge=%.2f nodes\n",
                idx.edgeCount(), idx.nodeCount(), (double) idx.nodeCount()/idx.edgeCount());
    std::printf("[pruning] SDF generation nodeTests=%lld leafEdgeVisits=%lld exactDistanceCalls=%lld full=%lld leafRatio=%.4f\n",
                (long long) gen.nodeTests, (long long) gen.leafEdgeVisits, (long long) gen.exactDistanceCalls,
                fullTraversal, (double) gen.leafEdgeVisits/fullTraversal);
    // Required spatial pruning: less than a quarter of the leaves of a full per-pixel traversal.
    REQUIRE(gen.leafEdgeVisits < fullTraversal/4);
}

CHECK_CASE("pruning", colored_contour_mtsdf_prunes_half_and_reconstructs) {
    const int M = 256;
    Shape star;
    Contour &c = star.addContour();
    for (int i = 0; i < M; ++i) {
        double a0 = 2*PI*i/M, a1 = 2*PI*(i+1)/M;
        double r0 = (i&1) ? 18.0 : 34.0;
        double r1 = ((i+1)&1) ? 18.0 : 34.0;
        c.addEdge(EdgeHolder(Point2(r0*cos(a0), r0*sin(a0)), Point2(r1*cos(a1), r1*sin(a1))));
    }
    edgeColoringSimple(star, 3.0);
    ShapeDistanceIndex idx(star);
    REQUIRE(idx.nodeCount() <= 2*idx.edgeCount());

    int W = 128, H = 128;
    Projection proj(Vector2(1.2, 1.2), Vector2(-64, -64));
    Bitmap<float, 4> ref(W, H), out(W, H);
    generateMTSDF(ref, star, proj, Range(8));
    DistanceQueryStats st;
    generateMTSDFIndexed(out, idx, proj, Range(8), MSDFGeneratorConfig(true), &st);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            for (int k = 0; k < 4; ++k)
                requireClose(ref(x, y)[k], out(x, y)[k], FLOAT_TOL, "mtsdf pruning exactness");

    long long fullTraversal = (long long) W*H*idx.edgeCount();
    std::printf("[pruning] colored contour edges=%d nodes=%d\n", idx.edgeCount(), idx.nodeCount());
    std::printf("[pruning] MTSDF generation nodeTests=%lld leafEdgeVisits=%lld exactDistanceCalls=%lld full=%lld leafRatio=%.4f\n",
                (long long) st.nodeTests, (long long) st.leafEdgeVisits, (long long) st.exactDistanceCalls,
                fullTraversal, (double) st.leafEdgeVisits/fullTraversal);
    REQUIRE(st.leafEdgeVisits < fullTraversal/2);
    REQUIRE(st.exactDistanceCalls <= st.leafEdgeVisits);

    // Emit a PPM reconstruction from the median of the three color channels (documented artifact).
    std::vector<unsigned char> rgb((size_t) W*H*3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float m = median(out(x, y)[0], out(x, y)[1], out(x, y)[2]);
            unsigned char g = toByte(m);
            size_t o = (size_t) (y*W+x)*3;
            rgb[o] = rgb[o+1] = rgb[o+2] = g;
        }
    writePPM("checks/mtsdf-reconstruction.ppm", rgb, W, H);

    // Also emit the smooth-contour SDF image requested alongside the MTSDF one.
    const int N = 512;
    Shape circle;
    Contour &cc = circle.addContour();
    double R = 40;
    for (int i = 0; i < N; ++i) {
        double a0 = 2*PI*i/N, a1 = 2*PI*(i+1)/N;
        cc.addEdge(EdgeHolder(Point2(R*cos(a0), R*sin(a0)), Point2(R*cos(a1), R*sin(a1))));
    }
    edgeColoringSimple(circle, 3.0);
    ShapeDistanceIndex cidx(circle);
    Bitmap<float, 1> sdf(W, H);
    generateSDFIndexed(sdf, cidx, proj, Range(8), GeneratorConfig(true), NULL);
    std::vector<unsigned char> gray((size_t) W*H*3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char g = toByte(*sdf(x, y));
            size_t o = (size_t) (y*W+x)*3;
            gray[o] = gray[o+1] = gray[o+2] = g;
        }
    writePPM("checks/smooth-sdf.ppm", gray, W, H);
}
