// checks/target_tests.cpp
// Regression tests for the stochastic VolumeGridDensity queries
// (SampleDistance / RatioTransmittance) and for the preserved legacy
// behavior (Density, sigma_t, tau, IntersectP).
//
// Expected values come from independent sources only: analytic escape
// probabilities and first-collision CDFs for uniform media, analytic
// exponentials for colored transmittance, and a high-precision numerical
// line integral of the existing sigma_t() for varying media.  Statistical
// checks use fixed seeds and report mean and standard error; tolerances
// are a multiple of the standard error plus a small floating-point margin.
#include "suite.h"
#include "volumes/volumegrid.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace {

// Running mean / standard error.
struct Stat {
    double n, s1, s2;
    Stat() : n(0), s1(0), s2(0) {}
    void add(double x) { n += 1; s1 += x; s2 += x*x; }
    double mean() const { return s1/n; }
    double se() const {
        double v = (s2 - s1*s1/n) / (n-1);
        return v > 0 ? sqrt(v/n) : 0.;
    }
};

// Tolerance: standard-error driven plus a floating-point margin.
static double Tol(const Stat &st, double k = 6.) {
    return k*st.se() + 5e-4;
}

// Independent high-precision line integral of the existing sigma_t():
// returns \int sigma_t[channel] dl over the ray's intersection with the
// grid, using midpoint quadrature.  Uses only pre-existing public API.
static double LineTau(const VolumeGridDensity &g, const Ray &r, int channel,
        int nSteps = 20000) {
    float t0, t1;
    if (!g.IntersectP(r, &t0, &t1) || !(t1 > t0)) return 0.;
    const float wlen = r.d.Length();
    double sum = 0.;
    for (int i = 0; i < nSteps; ++i) {
        float t = t0 + (t1-t0) * (i + .5f) / nSteps;
        float rgb[3];
        g.sigma_t(r(t), Vector(0, 0, 1), 0.f).ToRGB(rgb);
        sum += rgb[channel];
    }
    return sum * wlen * (t1-t0) / nSteps;
}

static bool ThrowsInvalid(const std::function<void()> &f) {
    try { f(); }
    catch (const std::invalid_argument &) { return true; }
    catch (...) { return false; }
    return false;
}

static void CheckStat(Suite &s, const char *name, const Stat &st,
        double expected) {
    double tol = Tol(st);
    std::printf("  %-34s mean=%.6f se=%.6f expected=%.6f tol=%.6f\n",
        name, st.mean(), st.se(), expected, tol);
    s.check(std::fabs(st.mean() - expected) < tol, name);
}

// Uniform 4^3 grid, density 1, unit box, configurable transform/sigmas.
struct UniformGrid {
    std::vector<float> d;
    VolumeGridDensity *g;
    UniformGrid(const Transform &v2w, const Spectrum &sa, const Spectrum &ss) {
        d.assign(64, 1.f);
        g = new VolumeGridDensity(sa, ss, 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), v2w, 4, 4, 4, &d[0]);
    }
    ~UniformGrid() { delete g; }
};

void TestFreeflight(Suite &s) {
    UniformGrid ug(Transform(), Spectrum(1.f), Spectrum(0.f));
    const VolumeGridDensity &g = *ug.g;
    const float ext = 1.3f;

    // Analytic escape probability and first-collision CDF, uniform medium.
    {
        Ray ray(Point(.5f, .5f, -1.f), Vector(0, 0, 1), 0.f);
        RNG rng(12345);
        const int N = 20000;
        Stat esc, cdf;
        for (int i = 0; i < N; ++i) {
            GridDistanceSample r = g.SampleDistance(ray, ext, rng);
            esc.add(r.hit ? 0. : 1.);
            cdf.add((r.hit && r.t < 1.5f) ? 1. : 0.);
        }
        CheckStat(s, "freeflight/uniform escape P", esc, exp(-(double)ext));
        CheckStat(s, "freeflight/uniform CDF(t<1.5)", cdf, 1.-exp(-.65));
    }

    // Non-normalized direction: free path is measured in world length, but
    // reported t values are parameters of the caller's ray O + t D.
    {
        Ray ray(Point(.5f, .5f, -1.f), Vector(0, 0, 2), 0.f);
        RNG rng(999);
        const int N = 20000;
        Stat esc;
        double maxPosErr = 0.;
        bool tInRange = true;
        for (int i = 0; i < N; ++i) {
            GridDistanceSample r = g.SampleDistance(ray, ext, rng);
            esc.add(r.hit ? 0. : 1.);
            if (r.hit) {
                if (r.t < .5f-1e-4f || r.t > 1.f+1e-4f) tInRange = false;
                Point expect = ray(r.t);
                maxPosErr = std::max(maxPosErr,
                    (double)std::fabs(r.position.z - expect.z));
                maxPosErr = std::max(maxPosErr,
                    (double)std::fabs(r.position.x - expect.x));
            }
        }
        CheckStat(s, "freeflight/non-unit dir escape P", esc, exp(-(double)ext));
        s.check(tInRange, "freeflight: hit t outside caller's [t0,t1]");
        s.check(maxPosErr < 1e-3, "freeflight: position != O + t D");
    }

    // Vacuum: immediate escape, no RNG consumed, no bricks visited.
    {
        Ray ray(Point(.5f, .5f, -1.f), Vector(0, 0, 1), 0.f);
        RNG rng(7), ref(7);
        GridTrackingStats st;
        GridDistanceSample r = g.SampleDistance(ray, 0.f, rng, &st);
        s.check(!r.hit && r.t == ray.maxt, "freeflight: vacuum should escape");
        s.check(st.brick_visits == 0 && st.density_evaluations == 0 &&
                st.null_events == 0, "freeflight: vacuum touched the grid");
        s.check(rng.RandomUInt() == ref.RandomUInt(),
                "freeflight: vacuum consumed RNG");
    }

    // Non-intersecting ray and zero-length segment: escape, no RNG.
    {
        Ray miss(Point(.5f, .5f, -1.f), Vector(0, 0, -1), 0.f);
        RNG rng(8), ref(8);
        GridTrackingStats st;
        GridDistanceSample r = g.SampleDistance(miss, ext, rng, &st);
        s.check(!r.hit && r.t == miss.maxt, "freeflight: miss should escape");
        s.check(st.brick_visits == 0, "freeflight: miss visited bricks");
        Ray zero(Point(.5f, .5f, -1.f), Vector(0, 0, 1), 1.5f, 1.5f);
        RNG rng2(9), ref2(9);
        r = g.SampleDistance(zero, ext, rng2, &st);
        s.check(!r.hit, "freeflight: zero-length segment should escape");
        s.check(rng2.RandomUInt() == ref2.RandomUInt(),
                "freeflight: zero-length segment consumed RNG");
    }

    // Varying density ramp: expectation from the independent line integral.
    {
        std::vector<float> d(8);
        for (int i = 0; i < 8; ++i) d[i] = .5f*i;
        VolumeGridDensity ramp(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(), 8, 1, 1, &d[0]);
        Ray ray(Point(-1.f, .5f, .5f), Vector(1, 0, 0), 0.f);
        const float e = .8f;
        double expected = exp(-e * LineTau(ramp, ray, 0));
        RNG rng(4242);
        const int N = 20000;
        Stat esc;
        GridTrackingStats st, tot;
        uint64_t escVisits = 0, escCount = 0;
        for (int i = 0; i < N; ++i) {
            GridDistanceSample r = ramp.SampleDistance(ray, e, rng, &st);
            esc.add(r.hit ? 0. : 1.);
            if (!r.hit) { escVisits += st.brick_visits; ++escCount; }
            tot.density_evaluations += st.density_evaluations;
            tot.null_events += st.null_events;
        }
        CheckStat(s, "freeflight/ramp escape P", esc, expected);
        // Escaping rays cross all 3 bricks of the 8x1x1 grid exactly once.
        s.check(escCount > 0 && escVisits == 3*escCount,
                "freeflight: escape traversal skipped bricks");
        s.check(tot.density_evaluations > 0, "freeflight: no density evaluated");
        s.check(tot.null_events > 0, "freeflight: no null events recorded");
    }

    // Malformed input is rejected with std::invalid_argument.
    {
        Ray zeroDir(Point(0, 0, 0), Vector(0, 0, 0), 0.f);
        RNG rng(1);
        s.check(ThrowsInvalid([&] { g.SampleDistance(zeroDir, 1.f, rng); }),
                "freeflight: zero direction not rejected");
        Ray nanRay(Point(0, 0, 0), Vector(0, 0, 1), 0.f);
        nanRay.o.x = std::nan("");
        s.check(ThrowsInvalid([&] { g.RatioTransmittance(nanRay, rng); }),
                "freeflight: NaN ray not rejected");
        Ray ray(Point(.5f, .5f, -1.f), Vector(0, 0, 1), 0.f);
        s.check(ThrowsInvalid([&] { g.SampleDistance(ray, -1.f, rng); }),
                "freeflight: negative extinction not rejected");
    }
}

void TestRatio(Suite &s) {
    // Colored extinction: analytic per-channel transmittance exp(-sigT L).
    const float sa[3] = { .2f, .5f, .8f };
    const float ss[3] = { .3f, .2f, .1f };
    const float sigT[3] = { .5f, .7f, .9f };
    UniformGrid ug(Transform(), Spectrum::FromRGB(sa), Spectrum::FromRGB(ss));
    const VolumeGridDensity &g = *ug.g;
    Ray ray(Point(.5f, .5f, -1.f), Vector(0, 0, 1), 0.f);

    {
        RNG rng(31337);
        const int N = 20000;
        Stat tr[3];
        bool nonNegative = true, atMostOne = true;
        GridTrackingStats st, tot;
        for (int i = 0; i < N; ++i) {
            Spectrum T = g.RatioTransmittance(ray, rng, &st);
            float rgb[3];
            T.ToRGB(rgb);
            for (int c = 0; c < 3; ++c) {
                tr[c].add(rgb[c]);
                if (!(rgb[c] >= 0.f)) nonNegative = false;
                if (!(rgb[c] <= 1.f + 1e-6f)) atMostOne = false;
            }
            tot.brick_visits += st.brick_visits;
            tot.density_evaluations += st.density_evaluations;
            tot.null_events += st.null_events;
        }
        for (int c = 0; c < 3; ++c) {
            char name[64];
            std::snprintf(name, sizeof name, "ratio/uniform T channel %d", c);
            CheckStat(s, name, tr[c], exp(-(double)sigT[c]));
        }
        s.check(nonNegative, "ratio: negative channel estimate");
        s.check(atMostOne, "ratio: estimate above 1");
        s.check(tot.null_events == 0,
                "ratio: candidates miscounted as null events");
        s.check(tot.density_evaluations > 0, "ratio: no density evaluated");
        s.check(tot.brick_visits >= 2ull*N, "ratio: too few bricks visited");
    }

    // Varying density: expectation from the independent line integral.
    {
        std::vector<float> d(8);
        for (int i = 0; i < 8; ++i) d[i] = .5f*i;
        VolumeGridDensity ramp(Spectrum::FromRGB(sa), Spectrum::FromRGB(ss),
            0.f, Spectrum(0.f), BBox(Point(0, 0, 0), Point(1, 1, 1)),
            Transform(), 8, 1, 1, &d[0]);
        Ray rray(Point(-1.f, .5f, .5f), Vector(1, 0, 0), 0.f);
        RNG rng(777);
        const int N = 20000;
        Stat tr[3];
        for (int i = 0; i < N; ++i) {
            float rgb[3];
            ramp.RatioTransmittance(rray, rng).ToRGB(rgb);
            for (int c = 0; c < 3; ++c) tr[c].add(rgb[c]);
        }
        for (int c = 0; c < 3; ++c) {
            char name[64];
            std::snprintf(name, sizeof name, "ratio/ramp T channel %d", c);
            CheckStat(s, name, tr[c], exp(-LineTau(ramp, rray, c)));
        }
    }

    // Zero extinction: exact all-ones transmittance, no RNG, no traversal.
    {
        UniformGrid vac(Transform(), Spectrum(0.f), Spectrum(0.f));
        RNG rng(11), ref(11);
        GridTrackingStats st;
        float rgb[3];
        vac.g->RatioTransmittance(ray, rng, &st).ToRGB(rgb);
        s.check(rgb[0] == 1.f && rgb[1] == 1.f && rgb[2] == 1.f,
                "ratio: vacuum not exactly 1");
        s.check(st.brick_visits == 0 && st.density_evaluations == 0,
                "ratio: vacuum touched the grid");
        s.check(rng.RandomUInt() == ref.RandomUInt(),
                "ratio: vacuum consumed RNG");
    }

    // Non-intersecting ray: all-ones, no RNG.
    {
        Ray miss(Point(.5f, .5f, -1.f), Vector(0, 0, -1), 0.f);
        RNG rng(12), ref(12);
        float rgb[3];
        g.RatioTransmittance(miss, rng).ToRGB(rgb);
        s.check(rgb[0] == 1.f && rgb[1] == 1.f && rgb[2] == 1.f,
                "ratio: miss not all-ones");
        s.check(rng.RandomUInt() == ref.RandomUInt(),
                "ratio: miss consumed RNG");
    }
}

void TestMajorant(Suite &s) {
    // High density straddling a brick boundary (samples 3 and 4 of an 8^3
    // grid; brick 0 covers samples 0..3, brick 1 covers 3..7).  A majorant
    // that misses the neighbor sample across the boundary under-estimates
    // the spike and biases the escape rate upward.
    {
        std::vector<float> d(512, 0.f);
        d[4*64 + 4*8 + 3] = 40.f;
        d[4*64 + 4*8 + 4] = 40.f;
        VolumeGridDensity g(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(), 8, 8, 8, &d[0]);
        Ray ray(Point(-1.f, 4.5f/8, 4.5f/8), Vector(1, 0, 0), 0.f);
        const float e = .2f;
        double expected = exp(-e * LineTau(g, ray, 0));
        RNG rng(2024);
        const int N = 20000;
        Stat esc;
        int hits = 0;
        for (int i = 0; i < N; ++i) {
            GridDistanceSample r = g.SampleDistance(ray, e, rng);
            esc.add(r.hit ? 0. : 1.);
            hits += r.hit ? 1 : 0;
        }
        CheckStat(s, "majorant/brick-edge spike escape P", esc, expected);
        s.check(hits > 0, "majorant: spike never hit");
    }

    // Thin support: a single dense sample in a 16^3 grid, traced forward,
    // reversed, and from inside the box.
    {
        std::vector<float> d(4096, 0.f);
        d[8*256 + 8*16 + 8] = 30.f;
        VolumeGridDensity g(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(),
            16, 16, 16, &d[0]);
        Ray fwd(Point(-1.f, 8.5f/16, 8.5f/16), Vector(1, 0, 0), 0.f);
        Ray rev(Point(2.f, 8.5f/16, 8.5f/16), Vector(-1, 0, 0), 0.f);
        Ray in(Point(.25f, 8.5f/16, 8.5f/16), Vector(1, 0, 0), 0.f);
        const Ray rays[3] = { fwd, rev, in };
        const char *names[3] = { "majorant/thin support fwd",
                                 "majorant/thin support reversed",
                                 "majorant/thin support from inside" };
        for (int k = 0; k < 3; ++k) {
            double expected = exp(-LineTau(g, rays[k], 0));
            RNG rng(500 + k);
            const int N = 20000;
            Stat esc;
            for (int i = 0; i < N; ++i)
                esc.add(g.SampleDistance(rays[k], 1.f, rng).hit ? 0. : 1.);
            CheckStat(s, names[k], esc, expected);
        }
    }

    // Many empty bricks around a small dense blob: traversal must skip the
    // empty bricks without evaluating density or drawing RNG.
    {
        std::vector<float> d(32*32*32, 0.f);
        for (int z = 15; z <= 16; ++z)
            for (int y = 15; y <= 16; ++y)
                for (int x = 15; x <= 16; ++x)
                    d[z*1024 + y*32 + x] = 5.f;
        VolumeGridDensity g(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(),
            32, 32, 32, &d[0]);
        Ray blob(Point(-1.f, .5f, .5f), Vector(1, 0, 0), 0.f);
        double expected = exp(-LineTau(g, blob, 0));
        RNG rng(88);
        const int N = 20000;
        Stat esc;
        GridTrackingStats st, tot;
        uint64_t escVisits = 0, escCount = 0;
        for (int i = 0; i < N; ++i) {
            GridDistanceSample r = g.SampleDistance(blob, 1.f, rng, &st);
            esc.add(r.hit ? 0. : 1.);
            if (!r.hit) { escVisits += st.brick_visits; ++escCount; }
            tot.density_evaluations += st.density_evaluations;
        }
        CheckStat(s, "majorant/blob escape P", esc, expected);
        double avgVisits = escCount ? escVisits/(double)escCount : 0.;
        double avgEvals = tot.density_evaluations / (double)N;
        std::printf("  %-34s avg_bricks(escape)=%.2f avg_evals=%.3f\n",
            "majorant/empty-brick culling", avgVisits, avgEvals);
        // Escapes cross all 9 bricks; density is only evaluated inside the
        // 2 bricks that overlap the blob.
        s.check(escCount > 0 && escVisits == 9*escCount,
                "majorant: bricks not visited");
        s.check(avgEvals < 3., "majorant: empty bricks not skipped");

        // Fully empty path: zero evaluations, zero nulls, no RNG at all.
        Ray empty(Point(-1.f, .05f, .05f), Vector(1, 0, 0), 0.f);
        RNG rng1(606), ref1(606);
        GridDistanceSample r = g.SampleDistance(empty, 1.f, rng1, &st);
        s.check(!r.hit && st.density_evaluations == 0 &&
                st.null_events == 0 && st.brick_visits > 0,
                "majorant: empty path evaluated density");
        s.check(rng1.RandomUInt() == ref1.RandomUInt(),
                "majorant: empty path consumed RNG");
        RNG rng2(707), ref2(707);
        float rgb[3];
        g.RatioTransmittance(empty, rng2, &st).ToRGB(rgb);
        s.check(rgb[0] == 1.f && rgb[1] == 1.f && rgb[2] == 1.f &&
                st.density_evaluations == 0,
                "majorant: empty path transmittance not 1");
        s.check(rng2.RandomUInt() == ref2.RandomUInt(),
                "majorant: empty path ratio consumed RNG");
    }
}

void TestAffine(Suite &s) {
    const float e = .9f;
    // Escape expectation for a uniform grid under any affine transform:
    // exp(-e * world-space path length inside the grid), with the segment
    // taken from the pre-existing IntersectP().
    struct Case { const char *name; Transform v2w; Ray ray; };
    Case cases[3] = {
        { "affine/translate escape P",
          Translate(Vector(2.f, 3.f, 4.f)),
          Ray(Point(2.5f, 3.5f, 0.f), Vector(0, 0, 1), 0.f) },
        { "affine/non-uniform scale escape P",
          Scale(2.f, 3.f, 4.f),
          Ray(Point(1.f, 1.5f, -2.f), Vector(0, 0, 2), 0.f) },
        { "affine/shear escape P",
          Transform(Matrix4x4(1.f, .5f, 0.f, 0.f,
                              0.f, 1.f, 0.f, 0.f,
                              0.f, 0.f, 1.f, 0.f,
                              0.f, 0.f, 0.f, 1.f)),
          Ray(Point(.5f, .1f, -1.f), Vector(.1f, .4f, 1.f), 0.f) },
    };
    for (int k = 0; k < 3; ++k) {
        UniformGrid ug(cases[k].v2w, Spectrum(1.f), Spectrum(0.f));
        float t0, t1;
        s.check(ug.g->IntersectP(cases[k].ray, &t0, &t1),
                "affine: test ray misses its grid");
        double L = (t1-t0) * cases[k].ray.d.Length();
        double expected = exp(-e * L);
        RNG rng(9000 + k);
        const int N = 20000;
        Stat esc;
        bool tInRange = true;
        for (int i = 0; i < N; ++i) {
            GridDistanceSample r = ug.g->SampleDistance(cases[k].ray, e, rng);
            esc.add(r.hit ? 0. : 1.);
            if (r.hit && (r.t < t0-1e-3f || r.t > t1+1e-3f)) tInRange = false;
        }
        CheckStat(s, cases[k].name, esc, expected);
        s.check(tInRange, "affine: hit t outside intersected segment");
    }

    // Colored ratio tracking through a scaled+translated grid.
    {
        const float sa[3] = { .2f, .5f, .8f };
        const float ss[3] = { .3f, .2f, .1f };
        const float sigT[3] = { .5f, .7f, .9f };
        UniformGrid ug(Translate(Vector(1.f, 2.f, 3.f)) * Scale(2.f, 2.f, 2.f),
            Spectrum::FromRGB(sa), Spectrum::FromRGB(ss));
        Ray ray(Point(2.f, 3.f, 0.f), Vector(0, 0, 1), 0.f);
        float t0, t1;
        s.check(ug.g->IntersectP(ray, &t0, &t1), "affine: ratio ray misses");
        double L = (t1-t0) * ray.d.Length();
        RNG rng(4711);
        const int N = 20000;
        Stat tr[3];
        for (int i = 0; i < N; ++i) {
            float rgb[3];
            ug.g->RatioTransmittance(ray, rng).ToRGB(rgb);
            for (int c = 0; c < 3; ++c) tr[c].add(rgb[c]);
        }
        for (int c = 0; c < 3; ++c) {
            char name[64];
            std::snprintf(name, sizeof name, "affine/ratio T channel %d", c);
            CheckStat(s, name, tr[c], exp(-sigT[c] * L));
        }
    }
}

void TestLegacy(Suite &s) {
    // Existing Density / sigma_t / tau / IntersectP behavior is unchanged.
    float density[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    VolumeGridDensity g(Spectrum(.25f), Spectrum(.75f), 0.f, Spectrum(0.f),
        BBox(Point(0, 0, 0), Point(1, 1, 1)), Scale(2.f, 3.f, 4.f),
        2, 2, 2, density);
    s.check(std::fabs(g.Density(Point(.5f, .5f, .5f)) - 4.5f) < 1e-6f,
            "legacy: center density");
    s.check(g.Density(Point(0, 0, 0)) == 1.f &&
            g.Density(Point(1, 1, 1)) == 8.f,
            "legacy: edge clamp density");
    s.check(g.Density(Point(-1, 0, 0)) == 0.f, "legacy: outside density");
    Ray r(Point(1, 1.5f, -2), Vector(0, 0, 2), 0.f, 4.f);
    float t0, t1;
    s.check(g.IntersectP(r, &t0, &t1) && t0 == 1.f && t1 == 3.f,
            "legacy: IntersectP interval");
    float rgb[3];
    g.tau(r, .001f, .5f).ToRGB(rgb);
    s.check(std::fabs(rgb[0] - 18.f) < .03f, "legacy: fixed-step tau");
    float srgb[3];
    g.sigma_t(Point(1, 1.5f, 2), Vector(0, 0, 1), 0.f).ToRGB(srgb);
    s.check(std::fabs(srgb[0] - 4.5f) < 1e-6f, "legacy: sigma_t");

    // Constructor validation: bad density, dimensions, coefficients and
    // transforms are rejected with std::invalid_argument.
    float bad[8] = { 1, 2, 3, -4, 5, 6, 7, 8 };
    s.check(ThrowsInvalid([&] {
        VolumeGridDensity v(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(),
            2, 2, 2, bad); }), "legacy: negative density accepted");
    float nanD[8] = { 1, 2, 3, std::nanf(""), 5, 6, 7, 8 };
    s.check(ThrowsInvalid([&] {
        VolumeGridDensity v(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(),
            2, 2, 2, nanD); }), "legacy: NaN density accepted");
    s.check(ThrowsInvalid([&] {
        VolumeGridDensity v(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(),
            0, 2, 2, density); }), "legacy: zero dimension accepted");
    s.check(ThrowsInvalid([&] {
        VolumeGridDensity v(Spectrum(-1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Transform(),
            2, 2, 2, density); }), "legacy: negative sigma_a accepted");
    s.check(ThrowsInvalid([&] {
        VolumeGridDensity v(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(1, 1, 1)), Scale(0.f, 1.f, 1.f),
            2, 2, 2, density); }), "legacy: singular transform accepted");
    s.check(ThrowsInvalid([&] {
        VolumeGridDensity v(Spectrum(1.f), Spectrum(0.f), 0.f, Spectrum(0.f),
            BBox(Point(0, 0, 0), Point(0, 1, 1)), Transform(),
            2, 2, 2, density); }), "legacy: degenerate extent accepted");
}

} // namespace

void register_tests(Suite &s) {
    s.add("freeflight", TestFreeflight);
    s.add("ratio", TestRatio);
    s.add("majorant", TestMajorant);
    s.add("affine", TestAffine);
    s.add("legacy", TestLegacy);
}
