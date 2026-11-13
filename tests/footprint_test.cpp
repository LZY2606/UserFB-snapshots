// Exact footprint area-average tests for FastNoiseLite::GetNoiseFootprint.
//
// Self-contained C++14, no third-party dependencies. Run directly; on full
// success it prints FASTNOISE_FOOTPRINT_OK.
#include "Cpp/FastNoiseLite.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

namespace
{
using FN = FastNoiseLite;

int g_failures = 0;
int g_checks = 0;

void report(bool ok, const std::string& name, const std::string& detail = "")
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::cout << "  FAIL: " << name;
        if (!detail.empty())
            std::cout << " (" << detail << ")";
        std::cout << "\n";
    }
}

void check(bool cond, const std::string& name)
{
    report(cond, name);
}

void checkClose(double got, double expected, double tol, const std::string& name)
{
    double err = std::fabs(got - expected);
    if (err > tol)
    {
        std::ostringstream os;
        os << std::setprecision(15) << "got=" << got << " expected=" << expected << " err=" << err
           << " tol=" << tol;
        report(false, name, os.str());
    }
    else
    {
        report(true, name);
    }
}

template <typename ExType>
bool throws(const std::function<void()>& call)
{
    try
    {
        call();
    }
    catch (const ExType&)
    {
        return true;
    }
    catch (...)
    {
        return false;
    }
    return false;
}

FN makeNone(int seed = 1337, float frequency = 0.73f)
{
    FN n(seed);
    n.SetNoiseType(FN::NoiseType_ValueCubic);
    n.SetFrequency(frequency);
    return n;
}

FN makeFbm(int seed = 1337, float frequency = 0.73f, int octaves = 4, float lacunarity = 1.75f,
           float gain = -0.5f)
{
    FN n = makeNone(seed, frequency);
    n.SetFractalType(FN::FractalType_FBm);
    n.SetFractalOctaves(octaves);
    n.SetFractalLacunarity(lacunarity);
    n.SetFractalGain(gain);
    n.SetFractalWeightedStrength(0);
    return n;
}

// Independent stratified quadrature over the parallelogram, sampling the
// ordinary scalar GetNoise. This deliberately does not touch any footprint
// integration code, so it is an independent numerical reference. A fixed
// per-pixel jitter keeps it deterministic run to run.
double stratifiedReference(const FN& n, double x, double y, const FN::NoiseFootprint2D& fp, int grid,
                           std::uint64_t salt = 0)
{
    std::mt19937_64 rng(0x9e3779b97f4a7c15ULL ^ (static_cast<std::uint64_t>(grid) * 1000003ULL) ^ salt);
    std::uniform_real_distribution<double> jitter(0.0, 1.0);
    double sum = 0.0;
    double scale = 1.0 / static_cast<double>(grid);
    for (int j = 0; j < grid; ++j)
    {
        for (int i = 0; i < grid; ++i)
        {
            double s = (static_cast<double>(i) + jitter(rng)) * scale - 0.5;
            double t = (static_cast<double>(j) + jitter(rng)) * scale - 0.5;
            double px = x + s * fp.ux + t * fp.vx;
            double py = y + s * fp.uy + t * fp.vy;
            sum += static_cast<double>(n.GetNoise(px, py));
        }
    }
    return sum / static_cast<double>(grid * grid);
}

// Same, but with a plain regular grid (no jitter).
double gridReference(const FN& n, double x, double y, const FN::NoiseFootprint2D& fp, int grid)
{
    double sum = 0.0;
    double scale = 1.0 / static_cast<double>(grid);
    for (int j = 0; j < grid; ++j)
    {
        for (int i = 0; i < grid; ++i)
        {
            double s = (static_cast<double>(i) + 0.5) * scale - 0.5;
            double t = (static_cast<double>(j) + 0.5) * scale - 0.5;
            double px = x + s * fp.ux + t * fp.vx;
            double py = y + s * fp.uy + t * fp.vy;
            sum += static_cast<double>(n.GetNoise(px, py));
        }
    }
    return sum / static_cast<double>(grid * grid);
}

// Split the parallelogram into k equal strips along one edge and verify the
// area-weighted (here equal-area) average reconstructs the whole average.
double splitAlongU(const FN& n, double x, double y, const FN::NoiseFootprint2D& fp, int k)
{
    FN::NoiseFootprint2D piece{ fp.ux / k, fp.uy / k, fp.vx, fp.vy };
    double acc = 0.0;
    for (int r = 0; r < k; ++r)
    {
        double centerS = -0.5 + static_cast<double>(r) / k + 0.5 / k;
        acc += n.GetNoiseFootprint(x + centerS * fp.ux, y + centerS * fp.uy, piece);
    }
    return acc / k;
}

double splitAlongV(const FN& n, double x, double y, const FN::NoiseFootprint2D& fp, int k)
{
    FN::NoiseFootprint2D piece{ fp.ux, fp.uy, fp.vx / k, fp.vy / k };
    double acc = 0.0;
    for (int r = 0; r < k; ++r)
    {
        double centerT = -0.5 + static_cast<double>(r) / k + 0.5 / k;
        acc += n.GetNoiseFootprint(x + centerT * fp.vx, y + centerT * fp.vy, piece);
    }
    return acc / k;
}

// --- Regression of the ordinary scalar GetNoise (same digest as baseline) ---
void scalarRegression()
{
    FN n = makeNone(1337);
    std::uint64_t digest = 1469598103934665603ULL;
    for (int j = -4; j < 5; ++j)
    {
        for (int i = -5; i < 6; ++i)
        {
            float v = n.GetNoise(i * 0.371 + 0.11, j * 0.217 - 0.19);
            check(std::isfinite(v) && std::fabs(v) <= 1.01f, "scalar GetNoise stays bounded");
            std::uint32_t bits;
            std::memcpy(&bits, &v, 4);
            digest ^= bits;
            digest *= 1099511628211ULL;
        }
    }
    check(digest == 0x917c1642529844b8ULL, "scalar GetNoise bit-for-bit regression digest");
}

void singleCellAxisAligned()
{
    // Frequency 1 keeps the footprint inside one unit cell.
    FN one(4242);
    one.SetNoiseType(FN::NoiseType_ValueCubic);
    one.SetFrequency(1.0f);

    FN::NoiseFootprint2D fp{ 0.5, 0.0, 0.0, 0.5 };
    double exact = one.GetNoiseFootprint(0.31, 0.27, fp);
    double ref = gridReference(one, 0.31, 0.27, fp, 300);
    checkClose(exact, ref, 2e-6, "single-cell axis-aligned footprint vs dense grid");
    check(std::fabs(exact) <= 1.01, "axis-aligned average stays in noise range");

    // A footprint wholly inside one cell but still slanted.
    FN::NoiseFootprint2D slanted{ 0.42, 0.13, -0.11, 0.38 };
    double exactS = one.GetNoiseFootprint(-0.21, 0.33, slanted);
    double refS = stratifiedReference(one, -0.21, 0.33, slanted, 300, 1);
    checkClose(exactS, refS, 3e-4, "single-cell slanted footprint vs stratified sampling");
}

void multiCellAndNegativeCoordinates()
{
    FN n = makeNone();
    FN::NoiseFootprint2D big{ 2.7, -1.1, 1.3, 2.2 };
    double exact = n.GetNoiseFootprint(-3.4, 2.6, big);
    double ref = stratifiedReference(n, -3.4, 2.6, big, 220, 2);
    checkClose(exact, ref, 5e-4, "multi-cell negative-coordinate footprint vs sampling");

    FN::NoiseFootprint2D g{ 2.0, 0.0, 0.0, 2.0 };
    FN grid(99);
    grid.SetNoiseType(FN::NoiseType_ValueCubic);
    grid.SetFrequency(1.0f);
    double whole = grid.GetNoiseFootprint(1.0, 1.0, g);
    double quarters = 0.0;
    FN::NoiseFootprint2D one{ 1.0, 0.0, 0.0, 1.0 };
    quarters += grid.GetNoiseFootprint(0.5, 0.5, one);
    quarters += grid.GetNoiseFootprint(1.5, 0.5, one);
    quarters += grid.GetNoiseFootprint(0.5, 1.5, one);
    quarters += grid.GetNoiseFootprint(1.5, 1.5, one);
    quarters *= 0.25;
    checkClose(whole, quarters, 1e-14, "grid-line aligned 2x2 cells merge exactly");
}

void invariance()
{
    FN n = makeNone();
    FN::NoiseFootprint2D fp{ 0.9, 0.35, -0.4, 0.8 };
    double base = n.GetNoiseFootprint(0.37, -0.91, fp);

    FN::NoiseFootprint2D swapped{ fp.vx, fp.vy, fp.ux, fp.uy };
    FN::NoiseFootprint2D negU{ -fp.ux, -fp.uy, fp.vx, fp.vy };
    FN::NoiseFootprint2D negV{ fp.ux, fp.uy, -fp.vx, -fp.vy };
    FN::NoiseFootprint2D reversed{ -fp.ux, -fp.uy, -fp.vx, -fp.vy };

    checkClose(n.GetNoiseFootprint(0.37, -0.91, swapped), base, 1e-14, "edge swap invariance");
    checkClose(n.GetNoiseFootprint(0.37, -0.91, negU), base, 1e-14, "one edge reversed invariance");
    checkClose(n.GetNoiseFootprint(0.37, -0.91, negV), base, 1e-14, "other edge reversed invariance");
    checkClose(n.GetNoiseFootprint(0.37, -0.91, reversed), base, 1e-14, "winding reversal invariance");
}

void conservation()
{
    // Strict double tolerance: the pieces reuse the same cell polynomials and
    // only differ in how the area is partitioned.
    FN n = makeNone();
    FN::NoiseFootprint2D fp{ 1.15, 0.46, -0.52, 0.93 };
    double whole = n.GetNoiseFootprint(-1.73, 0.62, fp);

    checkClose(splitAlongU(n, -1.73, 0.62, fp, 4), whole, 2e-13, "area conservation splitting along u");
    checkClose(splitAlongV(n, -1.73, 0.62, fp, 4), whole, 2e-13, "area conservation splitting along v");
    checkClose(splitAlongU(n, -1.73, 0.62, fp, 7), whole, 4e-13, "area conservation seven-way split");

    // Two-dimensional 2x2 partition.
    FN::NoiseFootprint2D quarter{ fp.ux * 0.5, fp.uy * 0.5, fp.vx * 0.5, fp.vy * 0.5 };
    double acc = 0.0;
    for (int su = -1; su <= 1; su += 2)
        for (int sv = -1; sv <= 1; sv += 2)
            acc += n.GetNoiseFootprint(-1.73 + su * 0.25 * fp.ux + sv * 0.25 * fp.vx,
                                       0.62 + su * 0.25 * fp.uy + sv * 0.25 * fp.vy, quarter);
    checkClose(acc * 0.25, whole, 2e-13, "area conservation under 2x2 partition");
}

void signsAndZeros()
{
    FN::NoiseFootprint2D fp{ 0.55, 0.22, -0.18, 0.48 };

    {
        FN p = makeFbm();
        p.SetFrequency(-0.61f);
        double exact = p.GetNoiseFootprint(0.37, -0.91, fp);
        checkClose(exact, stratifiedReference(p, 0.37, -0.91, fp, 200, 3), 8e-4, "negative frequency");
    }
    {
        FN p = makeFbm();
        p.SetFractalLacunarity(-2.0f);
        double exact = p.GetNoiseFootprint(0.37, -0.91, fp);
        checkClose(exact, stratifiedReference(p, 0.37, -0.91, fp, 200, 4), 8e-4, "negative lacunarity");
    }
    {
        // Zero frequency: every octave is a constant field, so the footprint
        // average is position independent and matches the scalar pipeline up
        // to float rounding.
        FN p = makeFbm();
        p.SetFrequency(0.0f);
        double avg = p.GetNoiseFootprint(0.37, -0.91, fp);
        FN::NoiseFootprint2D elsewhere{ 3.1, -1.7, 0.9, 2.4 };
        double avgOther = p.GetNoiseFootprint(-12.0, 8.0, elsewhere);
        check(avg == avgOther, "zero frequency collapses to a position-independent constant");
        checkClose(avg, static_cast<double>(p.GetNoise(0.37, -0.91)), 1e-7,
                   "zero-frequency average matches scalar GetNoise");
    }
    {
        // Zero lacunarity: octaves 1..n-1 collapse to point contributions and
        // must still be counted. Sampling confirms the full sum.
        FN p = makeFbm();
        p.SetFractalLacunarity(0.0f);
        double exact = p.GetNoiseFootprint(0.37, -0.91, fp);
        checkClose(exact, stratifiedReference(p, 0.37, -0.91, fp, 240, 5), 5e-4,
                   "zero lacunarity keeps collapsed octave contributions");
        FN single = makeFbm(1337, 0.73f, 1);
        double oneOctave = single.GetNoiseFootprint(0.37, -0.91, fp);
        check(exact != oneOctave, "zero lacunarity still adds the later constant octaves");
    }
    {
        // Zero gain: only the first (bounding-normalized) octave survives.
        FN p = makeFbm();
        p.SetFractalGain(0.0f);
        FN first = makeFbm(1337);
        first.SetFractalOctaves(1);
        checkClose(p.GetNoiseFootprint(0.37, -0.91, fp), first.GetNoiseFootprint(0.37, -0.91, fp), 1e-12,
                   "zero gain leaves a single octave");
    }
}

void negativeGainFbm()
{
    FN n = makeFbm(1337, 0.73f, 5, 1.75f, -0.62f);
    FN::NoiseFootprint2D fp{ 0.8, 0.3, -0.35, 0.7 };
    double exact = n.GetNoiseFootprint(0.17, -0.58, fp);
    double ref = stratifiedReference(n, 0.17, -0.58, fp, 220, 6);
    checkClose(exact, ref, 6e-4, "multi-octave FBm with negative gain vs sampling");
    check(std::fabs(exact) <= 1.01, "negative-gain FBm average stays bounded");

    // Manual per-octave assembly using frequency-1 double-coordinate layers.
    FN unit(0);
    unit.SetNoiseType(FN::NoiseType_ValueCubic);
    unit.SetFrequency(1.0f);
    float gainF = -0.62f;
    float g = std::fabs(gainF), ampF = g, total = 1.0f;
    for (int i = 1; i < 5; ++i)
    {
        total += ampF;
        ampF *= g;
    }
    float bounding = 1.0f / total;

    double scale = 1.0, amp = static_cast<double>(bounding), gain = gainF, manual = 0.0;
    double freq = static_cast<double>(0.73f); // match the library's float mFrequency promoted to double
    double lac = 1.75;
    for (int i = 0; i < 5; ++i)
    {
        FN layer = unit;
        layer.SetSeed(1337 + i);
        FN::NoiseFootprint2D lfp{ fp.ux * freq * scale, fp.uy * freq * scale, fp.vx * freq * scale,
                                  fp.vy * freq * scale };
        manual += layer.GetNoiseFootprint(0.17 * freq * scale, -0.58 * freq * scale, lfp) * amp;
        amp *= gain;
        scale *= lac;
    }
    checkClose(exact, manual, 2e-9, "FBm average equals independently assembled octave averages");
}

void pointFallback()
{
    FN n = makeFbm();
    FN::NoiseFootprint2D point{ 0.0, 0.0, 0.0, 0.0 };
    checkClose(n.GetNoiseFootprint(0.37, -0.91, point), static_cast<double>(n.GetNoise(0.37, -0.91)), 0.0,
               "all-zero footprint returns GetNoise exactly");

    FN none = makeNone();
    checkClose(none.GetNoiseFootprint(-2.4, 1.3, point), static_cast<double>(none.GetNoise(-2.4, 1.3)), 0.0,
               "point fallback for FractalType_None");
}

void rejectedInputs()
{
    FN::NoiseFootprint2D fp{ 0.4, 0.1, -0.1, 0.4 };

    FN perlin(1);
    perlin.SetNoiseType(FN::NoiseType_Perlin);
    check(throws<std::invalid_argument>([&] { perlin.GetNoiseFootprint(0, 0, fp); }),
          "non-ValueCubic noise type rejected");

    FN ridged = makeFbm();
    ridged.SetFractalType(FN::FractalType_Ridged);
    check(throws<std::invalid_argument>([&] { ridged.GetNoiseFootprint(0, 0, fp); }),
          "unsupported fractal type rejected");

    FN weighted = makeFbm();
    weighted.SetFractalWeightedStrength(0.5f);
    check(throws<std::invalid_argument>([&] { weighted.GetNoiseFootprint(0, 0, fp); }),
          "nonzero weighted strength rejected");

    FN octHigh = makeFbm();
    octHigh.SetFractalOctaves(13);
    check(throws<std::invalid_argument>([&] { octHigh.GetNoiseFootprint(0, 0, fp); }), "13 octaves rejected");
    FN octLow = makeFbm();
    octLow.SetFractalOctaves(0);
    check(throws<std::invalid_argument>([&] { octLow.GetNoiseFootprint(0, 0, fp); }), "0 octaves rejected");

    FN gainBig = makeFbm();
    gainBig.SetFractalGain(1.5f);
    check(throws<std::invalid_argument>([&] { gainBig.GetNoiseFootprint(0, 0, fp); }), "gain > 1 rejected");
    FN gainNan = makeFbm();
    gainNan.SetFractalGain(std::nanf(""));
    check(throws<std::invalid_argument>([&] { gainNan.GetNoiseFootprint(0, 0, fp); }), "NaN gain rejected");

    FN none = makeNone();
    FN::NoiseFootprint2D parallel{ 1.0, 2.0, 2.0, 4.0 };
    check(throws<std::invalid_argument>([&] { none.GetNoiseFootprint(0, 0, parallel); }),
          "parallel (zero-area, non-point) footprint rejected");
    FN::NoiseFootprint2D oneEdgeZero{ 1.0, 0.0, 0.0, 0.0 };
    check(throws<std::invalid_argument>([&] { none.GetNoiseFootprint(0, 0, oneEdgeZero); }),
          "single-line footprint rejected instead of treated as a point");

    FN::NoiseFootprint2D nanFoot{ std::nan(""), 0.0, 0.0, 1.0 };
    check(throws<std::invalid_argument>([&] { none.GetNoiseFootprint(0, 0, nanFoot); }),
          "NaN footprint component rejected");
    FN::NoiseFootprint2D nanV{ 1.0, 0.0, 0.0, std::nan("") };
    check(throws<std::invalid_argument>([&] { none.GetNoiseFootprint(0, 0, nanV); }),
          "NaN footprint component rejected (v)");
    check(throws<std::invalid_argument>([&] { none.GetNoiseFootprint(std::nan(""), 0.0, fp); }),
          "NaN position rejected");

    FN infFreq = makeNone();
    infFreq.SetFrequency(INFINITY);
    FN::NoiseFootprint2D point{ 0, 0, 0, 0 };
    check(throws<std::invalid_argument>([&] { infFreq.GetNoiseFootprint(0, 0, point); }),
          "infinite frequency rejected even for a point footprint");
    FN infLac = makeFbm();
    infLac.SetFractalLacunarity(INFINITY);
    check(throws<std::invalid_argument>([&] { infLac.GetNoiseFootprint(0, 0, point); }),
          "infinite lacunarity rejected");
    check(throws<std::invalid_argument>([&] { weighted.GetNoiseFootprint(0, 0, point); }),
          "configuration validated before point-footprint fallback");

    // Boundary gains are legal.
    FN gainOne = makeFbm();
    gainOne.SetFractalGain(1.0f);
    check(std::isfinite(gainOne.GetNoiseFootprint(0.1, 0.2, fp)), "gain of +1 accepted");
    FN gainMinusOne = makeFbm();
    gainMinusOne.SetFractalGain(-1.0f);
    check(std::isfinite(gainMinusOne.GetNoiseFootprint(0.1, 0.2, fp)), "gain of -1 accepted");

    // Workload limits.
    FN limited(1);
    limited.SetNoiseType(FN::NoiseType_ValueCubic);
    limited.SetFrequency(1.0f);
    FN::NoiseFootprint2D tooManyCells{ 300.0, 0.0, 0.0, 300.0 };
    check(throws<std::length_error>([&] { limited.GetNoiseFootprint(0, 0, tooManyCells); }),
          "footprint spanning more than 65536 cells throws length_error");
    FN::NoiseFootprint2D small{ 0.2, 0.0, 0.0, 0.2 };
    check(throws<std::length_error>([&] { limited.GetNoiseFootprint(2000000.0, 0.0, small); }),
          "vertex beyond 1048576 throws length_error");
    check(std::isfinite(limited.GetNoiseFootprint(1048576.0 - 0.1, 0.0, small)),
          "vertex at the inclusive coordinate limit is accepted");

    // A late octave breaching the limit must fail the whole call, never
    // return a partial-octave sum.
    FN late = makeFbm(7, 0.5f, 12, 2.0f, 0.5f);
    FN::NoiseFootprint2D lateFp{ 0.4, 0.1, -0.1, 0.4 };
    check(throws<std::length_error>([&] { late.GetNoiseFootprint(0.2, 0.3, lateFp); }),
          "oversized high octave aborts the entire footprint query");
}

void crossCellSlantedConvergence()
{
    FN n = makeNone(1337, 0.64f);
    // Deliberately slanted, crossing many cell boundaries and the origin.
    FN::NoiseFootprint2D fp{ 3.2, 1.4, -1.7, 2.6 };
    double exact = n.GetNoiseFootprint(-0.83, 0.41, fp);

    std::cout << "  cross-cell slanted footprint convergence (independent stratified sampling):\n";
    std::cout << "    samples/side   samples       error vs exact integral\n";
    double previous = 0.0;
    bool shrinks = true;
    int grids[] = { 16, 32, 64, 128, 256 };
    for (int gsize : grids)
    {
        double ref = stratifiedReference(n, -0.83, 0.41, fp, gsize, 99);
        double err = std::fabs(ref - exact);
        std::cout << "    " << std::setw(11) << gsize << "   " << std::setw(9) << (gsize * gsize) << "   "
                  << std::scientific << std::setprecision(3) << err << std::defaultfloat << "\n";
        if (gsize != 16 && err > previous * 2.0)
            shrinks = false;
        previous = err;
    }
    double finest = stratifiedReference(n, -0.83, 0.41, fp, 384, 100);
    double finestErr = std::fabs(finest - exact);
    std::cout << "    " << std::setw(11) << 384 << "   " << std::setw(9) << (384 * 384) << "   "
              << std::scientific << std::setprecision(3) << finestErr << std::defaultfloat
              << "   (error scale ~ O(1/sqrt(N)))\n";
    check(shrinks, "sampling error shrinks as the grid refines");
    check(finestErr < 1e-3, "high-density independent sampling agrees with the exact integral");
    check(std::fabs(exact) <= 1.01, "cross-cell average stays bounded");

    // Conservation on the same cross-cell footprint.
    checkClose(splitAlongU(n, -0.83, 0.41, fp, 3), exact, 3e-13,
               "cross-cell footprint conservation when split along u");
}

void constCallDoesNotMutate()
{
    FN n = makeFbm();
    const FN& c = n;
    FN::NoiseFootprint2D fp{ 0.3, 0.1, -0.1, 0.3 };
    double a = c.GetNoiseFootprint(0.2, 0.3, fp);
    double b = c.GetNoiseFootprint(0.2, 0.3, fp);
    check(a == b && std::isfinite(a), "repeated const queries are deterministic");
}
} // namespace

int main()
{
    std::cout << "FastNoiseLite footprint tests\n";
    scalarRegression();
    singleCellAxisAligned();
    multiCellAndNegativeCoordinates();
    invariance();
    conservation();
    signsAndZeros();
    negativeGainFbm();
    pointFallback();
    rejectedInputs();
    crossCellSlantedConvergence();
    constCallDoesNotMutate();

    std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
    if (g_failures != 0)
    {
        std::cout << "FASTNOISE_FOOTPRINT_FAILED\n";
        return 1;
    }
    std::cout << "FASTNOISE_FOOTPRINT_OK\n";
    return 0;
}
