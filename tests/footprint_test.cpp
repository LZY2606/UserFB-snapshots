// Tests for FastNoiseLite::GetNoiseFootprint.
//
// Build and run:
//   c++ -std=c++14 -O2 -I . -fwrapv tests/footprint_test.cpp -o footprint_test
//   ./footprint_test
//
// The tests use only the C++14 standard library and abort on the first failure.
// On success they print FASTNOISE_FOOTPRINT_OK.

#include "Cpp/FastNoiseLite.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <stdexcept>

namespace
{

void Fail(const char* expression, const char* file, int line, const char* detail)
{
    std::fprintf(stderr, "FAILED %s:%d: %s", file, line, expression);
    if (detail && detail[0])
        std::fprintf(stderr, " (%s)", detail);
    std::fprintf(stderr, "\n");
    std::abort();
}

void Check(bool ok, const char* expression, const char* file, int line, const char* detail = 0)
{
    if (!ok)
        Fail(expression, file, line, detail);
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

// Tolerances. The closed-form integral has no discretization error; these
// bounds cover double roundoff through the clipping and monomial integration.
const double kPointTolerance = 1e-12;
const double kExactTolerance = 1e-9;
// FBm references go through float-valued SetFrequency for each layer.
const double kFBmTolerance = 1e-7;
// Area-split conservation is algebraically exact, so hold it to a much tighter
// bound than the independent integration comparison.
const double kSplitTolerance = 1e-13;

FastNoiseLite MakeNoise(int seed = 1337, float frequency = 0.73f)
{
    FastNoiseLite noise(seed);
    noise.SetNoiseType(FastNoiseLite::NoiseType_ValueCubic);
    noise.SetFrequency(frequency);
    return noise;
}

FastNoiseLite MakeFBm(int seed, float frequency, int octaves, float lacunarity, float gain)
{
    FastNoiseLite noise = MakeNoise(seed, frequency);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(octaves);
    noise.SetFractalLacunarity(lacunarity);
    noise.SetFractalGain(gain);
    noise.SetFractalWeightedStrength(0.0f);
    return noise;
}

double Footprint(const FastNoiseLite& noise, double x, double y, double ux, double uy, double vx, double vy)
{
    FastNoiseLite::NoiseFootprint2D footprint;
    footprint.ux = ux;
    footprint.uy = uy;
    footprint.vx = vx;
    footprint.vy = vy;
    return noise.GetNoiseFootprint(x, y, footprint);
}

bool Near(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

// ---------------------------------------------------------------------------
// Independent double-precision reference of the same lattice random field.
// This deliberately re-derives the hash and the bicubic interpolation rather
// than calling into the library, so the comparison does not just restate the
// implementation.

double RefCubicLerp(double a, double b, double c, double d, double t)
{
    double p = (d - c) - (a - b);
    return t * t * t * p + t * t * ((a - b) - p) + t * (c - a) + b;
}

double RefLattice(int seed, int cellX, int cellY)
{
    uint32_t h = (uint32_t)seed ^ ((uint32_t)cellX * 501125321u) ^ ((uint32_t)cellY * 1136930381u);
    h *= 0x27d4eb2du;
    h *= h;
    h ^= h << 19;
    return (double)(int32_t)h * (1.0 / 2147483648.0);
}

double RefField(double x, double y, double frequency, int seed)
{
    double xn = frequency * x;
    double yn = frequency * y;
    int x1 = (int)std::floor(xn);
    int y1 = (int)std::floor(yn);
    double xs = xn - (double)x1;
    double ys = yn - (double)y1;
    double rows[4];
    for (int r = 0; r < 4; r++)
        rows[r] = RefCubicLerp(RefLattice(seed, x1 - 1, y1 + r - 1), RefLattice(seed, x1, y1 + r - 1),
                               RefLattice(seed, x1 + 1, y1 + r - 1), RefLattice(seed, x1 + 2, y1 + r - 1), xs);
    return RefCubicLerp(rows[0], rows[1], rows[2], rows[3], ys) * (double)(1.0f / (1.5f * 1.5f));
}

// FBm reference as sum of independent single-layer footprint averages.
double RefFBm(double x, double y, double ux, double uy, double vx, double vy,
              int seed, float frequency, int octaves, float lacunarity, float gain)
{
    float gainAbs = std::fabs(gain);
    float amp = gainAbs;
    float ampFractal = 1.0f;
    for (int i = 1; i < octaves; i++)
    {
        ampFractal += amp;
        amp *= gainAbs;
    }
    double bounding = 1.0 / (double)ampFractal;

    double sum = 0;
    double layerFrequency = (double)frequency;
    double weight = bounding;
    for (int i = 0; i < octaves; i++)
    {
        FastNoiseLite layer = MakeNoise(seed + i, (float)layerFrequency);
        sum += weight * Footprint(layer, x, y, ux, uy, vx, vy);
        layerFrequency *= (double)lacunarity;
        weight *= (double)gain;
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Dense midpoint sampling of the public GetNoise over an axis-aligned box.
// Used only for reference comparisons, never as the implementation.

double DenseAxisAlignedAverage(const FastNoiseLite& noise, double x, double y, double hx, double hy, int samples)
{
    double sum = 0;
    for (int a = 0; a < samples; a++)
        for (int b = 0; b < samples; b++)
        {
            double s = (a + 0.5) / samples - 0.5;
            double t = (b + 0.5) / samples - 0.5;
            sum += (double)noise.GetNoise(x + 2 * hx * s, y + 2 * hy * t);
        }
    return sum / ((double)samples * samples);
}

// ---------------------------------------------------------------------------
// Tests.

void TestPointFallback()
{
    FastNoiseLite noise = MakeNoise();
    for (int j = -6; j <= 6; j++)
        for (int i = -5; i <= 5; i++)
        {
            double x = i * 0.371 + 0.11;
            double y = j * 0.217 - 0.19;
            CHECK(Footprint(noise, x, y, 0, 0, 0, 0) == (double)noise.GetNoise(x, y));
        }

    // The fallback also applies with FBm, but it is still the plain scalar value.
    FastNoiseLite fbm = MakeFBm(1337, 0.73f, 4, 1.75f, 0.5f);
    CHECK(Footprint(fbm, 0.31, -0.27, 0, 0, 0, 0) == (double)fbm.GetNoise(0.31, -0.27));
}

void TestSingleCellAxisAligned()
{
    FastNoiseLite noise = MakeNoise(1337, 1.0f);
    struct Case
    {
        double x, y, hx, hy;
    } cases[] = {
        {0.5, 0.5, 0.2, 0.2},
        {-0.5, 0.5, 0.3, 0.15},
        {0.5, -0.5, 0.1, 0.35},
        {-0.5, -0.5, 0.4, 0.4},
        {0.37, 0.61, 0.12, 0.08},
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
    {
        double x = cases[k].x, y = cases[k].y, hx = cases[k].hx, hy = cases[k].hy;
        double value = Footprint(noise, x, y, 2 * hx, 0, 0, 2 * hy);

        // Compare against its own dense sampled integral and against the
        // independent polynomial integral within one cell.
        double sampled = DenseAxisAlignedAverage(noise, x, y, hx, hy, 256);
        CHECK(Near(value, sampled, 2e-6));

        CHECK(std::isfinite(value));
        CHECK(value >= -1.0 && value <= 1.0);

        // Shrinking the footprint to nothing tends to the point value.
        double tiny = Footprint(noise, x, y, 2 * hx * 1e-6, 0, 0, 2 * hy * 1e-6);
        CHECK(Near(tiny, (double)noise.GetNoise(x, y), 2e-5));
    }
}

void TestSingleCellTilted()
{
    FastNoiseLite noise = MakeNoise(1337, 1.0f);
    double x = 0.37, y = -0.62;
    double ux = 0.31, uy = 0.12, vx = -0.09, vy = 0.27;
    double value = Footprint(noise, x, y, ux, uy, vx, vy);
    CHECK(std::isfinite(value));
    CHECK(value >= -1.0 && value <= 1.0);

    // Independent double field sampled densely over the parallelogram.
    double sum = 0;
    const int samples = 384;
    for (int a = 0; a < samples; a++)
        for (int b = 0; b < samples; b++)
        {
            double s = (a + 0.5) / samples - 0.5;
            double t = (b + 0.5) / samples - 0.5;
            sum += RefField(x + s * ux + t * vx, y + s * uy + t * vy, 1.0, 1337);
        }
    sum /= ((double)samples * samples);
    CHECK(Near(value, sum, 3e-6));
}

void TestMultiCellAndNegativeCoordinates()
{
    FastNoiseLite noise = MakeNoise(1337, 1.0f);
    struct Case
    {
        double x, y, ux, uy, vx, vy;
    } cases[] = {
        {-5.3, -4.1, 2.7, 0.9, -0.6, 3.1},
        {-11.3, -8.9, 3.3, 1.1, 0.7, 3.7},
        {2.3, -2.6, 1.9, 0.4, 0.3, -2.2},
        {0.0, 0.0, 4.0, 0.0, 0.0, 4.0},
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
    {
        double x = cases[k].x, y = cases[k].y;
        double ux = cases[k].ux, uy = cases[k].uy, vx = cases[k].vx, vy = cases[k].vy;
        double value = Footprint(noise, x, y, ux, uy, vx, vy);
        CHECK(std::isfinite(value));
        CHECK(value >= -1.0 && value <= 1.0);

        // Non-degenerate multi-cell means should agree with dense GetNoise
        // sampling to the O(1/N^2) midpoint error.
        double sum = 0;
        const int samples = 320;
        for (int a = 0; a < samples; a++)
            for (int b = 0; b < samples; b++)
            {
                double s = (a + 0.5) / samples - 0.5;
                double t = (b + 0.5) / samples - 0.5;
                sum += (double)noise.GetNoise(x + s * ux + t * vx, y + s * uy + t * vy);
            }
        sum /= ((double)samples * samples);
        CHECK(Near(value, sum, 1e-5));
    }
}

void TestEdgesOnIntegerLatticeLines()
{
    FastNoiseLite noise = MakeNoise(1337, 1.0f);

    // Footprint corner exactly on an integer lattice point, and edges lying
    // exactly on integer lattice lines.
    double values[] = {
        Footprint(noise, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0),
        Footprint(noise, 0.5, 0.0, 1.0, 0.0, 0.0, 1.0),
        Footprint(noise, 0.0, 0.5, 1.0, 0.0, 0.0, 1.0),
        Footprint(noise, -3.0, -2.0, 0.75, 0.0, 0.0, 0.5),
        Footprint(noise, 2.0, -1.0, 1.0, 1.0, -1.0, 1.0),
    };
    for (int k = 0; k < 5; k++)
    {
        CHECK(std::isfinite(values[k]));
        CHECK(values[k] >= -1.0 && values[k] <= 1.0);
    }

    // An edge exactly on a lattice line must not change the result when the
    // same region is split there.
    double whole = Footprint(noise, 0.5, 0.25, 1.0, 0.0, 0.0, 0.5);
    double left = Footprint(noise, 0.25, 0.25, 0.5, 0.0, 0.0, 0.5);
    double right = Footprint(noise, 0.75, 0.25, 0.5, 0.0, 0.0, 0.5);
    CHECK(Near(0.5 * (left + right), whole, kSplitTolerance));
}

void TestOrientationInvariance()
{
    struct Case
    {
        double x, y, ux, uy, vx, vy;
    } cases[] = {
        {0.31, -1.27, 0.83, 0.42, -0.27, 0.61},
        {-4.7, 3.3, 1.7, -0.9, 0.4, 1.3},
        {0.13, 0.92, 0.51, 0.0, 0.0, 0.44},
    };
    FastNoiseLite noise = MakeNoise(1337, 0.73f);
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
    {
        double x = cases[k].x, y = cases[k].y;
        double ux = cases[k].ux, uy = cases[k].uy, vx = cases[k].vx, vy = cases[k].vy;
        double base = Footprint(noise, x, y, ux, uy, vx, vy);
        CHECK(Near(Footprint(noise, x, y, vx, vy, ux, uy), base, kPointTolerance));
        CHECK(Near(Footprint(noise, x, y, -ux, -uy, vx, vy), base, kPointTolerance));
        CHECK(Near(Footprint(noise, x, y, ux, uy, -vx, -vy), base, kPointTolerance));
        CHECK(Near(Footprint(noise, x, y, -ux, -uy, -vx, -vy), base, kPointTolerance));
        CHECK(Near(Footprint(noise, x, y, -vx, -vy, -ux, -uy), base, kPointTolerance));
    }
}

void TestAreaSplitConservation()
{
    // Splitting the footprint along a line and recombining with the relative
    // areas must reproduce the whole-footprint average. This is algebraically
    // exact, so it is held to the stricter kSplitTolerance.
    FastNoiseLite noise = MakeNoise(1337, 0.73f);
    struct Case
    {
        double x, y, ux, uy, vx, vy;
    } cases[] = {
        {0.31, -1.27, 0.83, 0.42, -0.27, 0.61},
        {-4.7, 3.3, 1.7, -0.9, 0.4, 1.3},
        {2.2, 0.7, 0.6, 0.25, -0.15, 0.9},
        {-0.2, 0.4, 1.3, 0.0, 0.0, 0.8},
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
    {
        double x = cases[k].x, y = cases[k].y;
        double ux = cases[k].ux, uy = cases[k].uy, vx = cases[k].vx, vy = cases[k].vy;
        double whole = Footprint(noise, x, y, ux, uy, vx, vy);

        // Equal halves along u.
        double a = Footprint(noise, x - 0.25 * ux, y - 0.25 * uy, 0.5 * ux, 0.5 * uy, vx, vy);
        double b = Footprint(noise, x + 0.25 * ux, y + 0.25 * uy, 0.5 * ux, 0.5 * uy, vx, vy);
        CHECK(Near(0.5 * (a + b), whole, kSplitTolerance));

        // Uneven split at c along u, centres offset by the relative areas.
        double c = 0.3;
        double ca = 0.5 * c - 0.5;
        double cb = 0.5 * c;
        double left = Footprint(noise, x + ca * ux, y + ca * uy, c * ux, c * uy, vx, vy);
        double right = Footprint(noise, x + cb * ux, y + cb * uy, (1 - c) * ux, (1 - c) * uy, vx, vy);
        CHECK(Near(c * left + (1 - c) * right, whole, kSplitTolerance));

        // Uneven split along v as well.
        double d = 0.65;
        double da = 0.5 * d - 0.5;
        double db = 0.5 * d;
        double lo = Footprint(noise, x + da * vx, y + da * vy, ux, uy, d * vx, d * vy);
        double hi = Footprint(noise, x + db * vx, y + db * vy, ux, uy, (1 - d) * vx, (1 - d) * vy);
        CHECK(Near(d * lo + (1 - d) * hi, whole, kSplitTolerance));

        // Four-way split.
        double q00 = Footprint(noise, x - 0.25 * (ux + vx), y - 0.25 * (uy + vy), 0.5 * ux, 0.5 * uy, 0.5 * vx, 0.5 * vy);
        double q10 = Footprint(noise, x + 0.25 * (ux - vx), y + 0.25 * (uy - vy), 0.5 * ux, 0.5 * uy, 0.5 * vx, 0.5 * vy);
        double q01 = Footprint(noise, x - 0.25 * (ux - vx), y - 0.25 * (uy - vy), 0.5 * ux, 0.5 * uy, 0.5 * vx, 0.5 * vy);
        double q11 = Footprint(noise, x + 0.25 * (ux + vx), y + 0.25 * (uy + vy), 0.5 * ux, 0.5 * uy, 0.5 * vx, 0.5 * vy);
        CHECK(Near(0.25 * (q00 + q10 + q01 + q11), whole, kSplitTolerance));
    }
}

void TestHighDensityIndependentIntegration()
{
    // A multi-cell tilted footprint compared against a dense independent
    // double-field integral. Reports the sampling convergence and error scale.
    double x = -5.3, y = -4.1, ux = 2.7, uy = 0.9, vx = -0.6, vy = 3.1;
    double frequency = 0.73;
    FastNoiseLite noise = MakeNoise(1337, (float)frequency);
    double exact = Footprint(noise, x, y, ux, uy, vx, vy);
    CHECK(std::isfinite(exact));

    double previous = 0;
    int sizes[5] = {32, 64, 128, 256, 512};
    double errors[5];
    for (int k = 0; k < 5; k++)
    {
        int samples = sizes[k];
        double sum = 0;
        for (int a = 0; a < samples; a++)
            for (int b = 0; b < samples; b++)
            {
                double s = (a + 0.5) / samples - 0.5;
                double t = (b + 0.5) / samples - 0.5;
                sum += RefField(x + s * ux + t * vx, y + s * uy + t * vy, frequency, 1337);
            }
        sum /= ((double)samples * samples);
        errors[k] = std::fabs(sum - exact);
        if (k > 0)
        {
            // Midpoint integration on this smooth bicubic is O(1/N^2).
            // Check the observed error decreases by at least ~3.2x per doubling.
            CHECK(errors[k] < errors[k - 1] * 0.32);
        }
        previous = sum;
    }
    (void)previous;

    std::printf("  high-density sweep: exact=%.12f\n", exact);
    for (int k = 0; k < 5; k++)
        std::printf("    N=%4d error=%.3e\n", sizes[k], errors[k]);

    // The final refinement must be within the closed-form integration error.
    CHECK(errors[4] < 2e-6);
    CHECK(errors[4] < 1e-2 * errors[0]);
}

void TestFrequencyAndLacunaritySigns()
{
    // Positive and negative frequencies and lacunarities, including negative
    // gain multi-octave FBm, all against a per-layer reference.
    struct Case
    {
        int seed;
        float frequency;
        int octaves;
        float lacunarity;
        float gain;
    } cases[] = {
        {1337, 0.73f, 4, 1.75f, 0.5f},
        {1337, 0.73f, 4, 1.75f, -0.5f},
        {4242, -0.9f, 3, 2.0f, 0.5f},
        {4242, 0.9f, 3, -2.0f, 0.5f},
        {777, -0.4f, 5, -1.3f, -0.9f},
        {777, 1.9f, 2, 0.0f, 0.7f},
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
    {
        double x = -1.37, y = 2.11, ux = 0.73, uy = 0.21, vx = -0.15, vy = 0.62;
        FastNoiseLite noise = MakeFBm(cases[k].seed, cases[k].frequency, cases[k].octaves,
                                      cases[k].lacunarity, cases[k].gain);
        double value = Footprint(noise, x, y, ux, uy, vx, vy);
        double reference = RefFBm(x, y, ux, uy, vx, vy, cases[k].seed, cases[k].frequency,
                                  cases[k].octaves, cases[k].lacunarity, cases[k].gain);
        CHECK(std::isfinite(value));
        // The per-layer reference builds float-frequency single-layer
        // instances (SetFrequency takes float), so allow for that rounding;
        // the implementation itself keeps freq * lacunarity^i in double.
        CHECK(Near(value, reference, kFBmTolerance));
    }
}

void TestZeroFrequencyAndZeroLacunarity()
{
    // Zero frequency with FractalType_None: the field is constant and the
    // average equals the point value.
    // The constant layer is evaluated in double, so it matches the float
    // GetNoise only to float precision, not to kPointTolerance.
    FastNoiseLite none = MakeNoise(1337, 0.0f);
    double point = (double)none.GetNoise(-2.7, 4.4);
    CHECK(Near(Footprint(none, -2.7, 4.4, 1.3, 0.2, -0.3, 1.9), point, 1e-7));

    // Zero frequency in FBm: every layer is constant; the constant of each layer
    // still contributes with its own seed and weight.
    FastNoiseLite fbm = MakeFBm(1337, 0.0f, 4, 1.75f, 0.5f);
    double value = Footprint(fbm, 2.3, -1.1, 0.9, 0.2, -0.1, 0.8);
    double reference = RefFBm(2.3, -1.1, 0.9, 0.2, -0.1, 0.8, 1337, 0.0f, 4, 1.75f, 0.5f);
    CHECK(Near(value, reference, kFBmTolerance));

    // Zero lacunarity: every octave after the first has zero frequency in both
    // senses (freq * 0), so those layers are constants and still contribute.
    FastNoiseLite lac = MakeFBm(1337, 0.73f, 4, 0.0f, 0.5f);
    double lacValue = Footprint(lac, 0.31, -1.27, 0.83, 0.42, -0.27, 0.61);
    double lacReference = RefFBm(0.31, -1.27, 0.83, 0.42, -0.27, 0.61, 1337, 0.73f, 4, 0.0f, 0.5f);
    CHECK(Near(lacValue, lacReference, kFBmTolerance));

    // Negative lacunarity alternates the sign of every later layer's frequency.
    FastNoiseLite alt = MakeFBm(4242, 0.9f, 3, -2.0f, 0.5f);
    double altValue = Footprint(alt, 0.31, -1.27, 0.83, 0.42, -0.27, 0.61);
    double altReference = RefFBm(0.31, -1.27, 0.83, 0.42, -0.27, 0.61, 4242, 0.9f, 3, -2.0f, 0.5f);
    CHECK(Near(altValue, altReference, kFBmTolerance));
}

void TestRejectedInputs()
{
    FastNoiseLite noise = MakeNoise();

    // Zero-area footprints other than the exact point fallback.
    bool threw = false;
    try
    {
        Footprint(noise, 0, 0, 1.0, 2.0, 2.0, 4.0);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    // Non-finite position or footprint.
    double nan = std::numeric_limits<double>::quiet_NaN();
    double inf = std::numeric_limits<double>::infinity();
    struct Bad
    {
        double x, y, ux, uy, vx, vy;
    } bad[] = {
        {nan, 0, 0.1, 0, 0, 0.1},
        {0, inf, 0.1, 0, 0, 0.1},
        {0, 0, nan, 0, 0, 0.1},
        {0, 0, 0.1, inf, 0, 0.1},
        {0, 0, 0.1, 0, nan, 0.1},
        {0, 0, 0.1, 0, 0, inf},
    };
    for (int k = 0; k < (int)(sizeof(bad) / sizeof(bad[0])); k++)
    {
        threw = false;
        try
        {
            Footprint(noise, bad[k].x, bad[k].y, bad[k].ux, bad[k].uy, bad[k].vx, bad[k].vy);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw);
    }

    // Unsupported noise type.
    FastNoiseLite perlin = MakeNoise();
    perlin.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    threw = false;
    try
    {
        Footprint(perlin, 0, 0, 0.1, 0, 0, 0.1);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    // Unsupported fractal type.
    FastNoiseLite ridged = MakeNoise();
    ridged.SetFractalType(FastNoiseLite::FractalType_Ridged);
    threw = false;
    try
    {
        Footprint(ridged, 0, 0, 0.1, 0, 0, 0.1);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    // Non-zero weighted strength with FBm.
    FastNoiseLite weighted = MakeFBm(1337, 0.73f, 4, 1.75f, 0.5f);
    weighted.SetFractalWeightedStrength(0.25f);
    threw = false;
    try
    {
        Footprint(weighted, 0, 0, 0.1, 0, 0, 0.1);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    // Octave counts outside 1...12.
    int octaves[] = {0, 13, -3};
    for (int k = 0; k < 3; k++)
    {
        FastNoiseLite outOfRange = MakeFBm(1337, 0.73f, 4, 1.75f, 0.5f);
        outOfRange.SetFractalOctaves(octaves[k]);
        threw = false;
        try
        {
            Footprint(outOfRange, 0, 0, 0.1, 0, 0, 0.1);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw);
    }

    // Gain must be finite and within [-1, 1] in absolute value.
    float gains[] = {1.0001f, -1.0001f, 2.0f};
    for (int k = 0; k < 3; k++)
    {
        FastNoiseLite badGain = MakeFBm(1337, 0.73f, 4, 1.75f, 0.5f);
        badGain.SetFractalGain(gains[k]);
        threw = false;
        try
        {
            Footprint(badGain, 0, 0, 0.1, 0, 0, 0.1);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw);
    }
    FastNoiseLite nanGain = MakeFBm(1337, 0.73f, 4, 1.75f, 0.5f);
    nanGain.SetFractalGain(std::numeric_limits<float>::quiet_NaN());
    threw = false;
    try
    {
        Footprint(nanGain, 0, 0, 0.1, 0, 0, 0.1);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    // Non-finite frequency and lacunarity.
    FastNoiseLite infFrequency = MakeNoise();
    infFrequency.SetFrequency(std::numeric_limits<float>::infinity());
    threw = false;
    try
    {
        Footprint(infFrequency, 0, 0, 0.1, 0, 0, 0.1);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    FastNoiseLite infLacunarity = MakeFBm(1337, 0.73f, 4, 1.75f, 0.5f);
    infLacunarity.SetFractalLacunarity(std::numeric_limits<float>::infinity());
    threw = false;
    try
    {
        Footprint(infLacunarity, 0, 0, 0.1, 0, 0, 0.1);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    // Configuration and input validation happen before the point fallback.
    threw = false;
    try
    {
        Footprint(nanGain, 0, 0, 0, 0, 0, 0);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try
    {
        Footprint(noise, nan, 0, 0, 0, 0, 0);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    CHECK(threw);
}

void TestCostLimits()
{
    FastNoiseLite noise = MakeNoise(1337, 1.0f);

    // 256 x 256 cells is the maximum allowed; 257 x 257 is rejected.
    bool ok = true;
    try
    {
        double value = Footprint(noise, 0, 0, 256.0, 0, 0, 256.0);
        CHECK(std::isfinite(value));
    }
    catch (const std::exception&)
    {
        ok = false;
    }
    CHECK(ok);

    bool threw = false;
    try
    {
        Footprint(noise, 0, 0, 257.0, 0, 0, 257.0);
    }
    catch (const std::length_error&)
    {
        threw = true;
    }
    CHECK(threw);

    // Transformed corner coordinates beyond 2^20 are rejected.
    threw = false;
    try
    {
        Footprint(noise, 3.0e6, 0, 0.1, 0, 0, 0.1);
    }
    catch (const std::length_error&)
    {
        threw = true;
    }
    CHECK(threw);

    // Non-finite intermediate (huge frequency) is rejected, not returned.
    FastNoiseLite huge = MakeNoise();
    huge.SetFrequency(1.0e30f);
    threw = false;
    try
    {
        Footprint(huge, 0, 0, 1.0e10, 0, 0, 1.0e10);
    }
    catch (const std::length_error&)
    {
        threw = true;
    }
    CHECK(threw);

    // If a later FBm octave exceeds the limit, the whole query must fail even
    // though the first octave was cheap.
    FastNoiseLite late = MakeFBm(1337, 0.01f, 3, 100.0f, 0.5f);
    threw = false;
    try
    {
        Footprint(late, 0, 0, 3.0, 0, 0, 3.0);
    }
    catch (const std::length_error&)
    {
        threw = true;
    }
    CHECK(threw);

    // Boundary check: 2^20 exactly at a corner is allowed.
    FastNoiseLite exactBound = MakeNoise(1337, 1.0f);
    ok = true;
    try
    {
        double value = Footprint(exactBound, 1048575.5, 0, 1.0, 0, 0, 1.0);
        CHECK(std::isfinite(value));
    }
    catch (const std::exception&)
    {
        ok = false;
    }
    CHECK(ok);
}

void TestGetNoiseRegression()
{
    // GetNoise itself must keep its existing float behaviour. These values were
    // captured from the unmodified library.
    FastNoiseLite noise = MakeNoise(1337, 0.73f);
    struct Point
    {
        double x, y;
    } points[] = {
        {-1.7, 0.4},
        {-0.87, -0.21},
        {-0.04, -0.82},
        {0.79, -1.43},
        {1.62, -2.04},
    };
    const float expected[] = {
        -0.0179639328f,
        0.0270021763f,
        0.300795704f,
        0.384077668f,
        0.0260190163f,
    };
    for (int k = 0; k < 5; k++)
    {
        float actual = noise.GetNoise(points[k].x, points[k].y);
        CHECK(Near((double)actual, (double)expected[k], 1e-7));
    }

    // And the double reference field agrees with the float path to float
    // precision, confirming the shared lattice values were not changed.
    double worst = 0;
    for (int j = -12; j <= 12; j++)
        for (int i = -12; i <= 12; i++)
        {
            double x = i * 0.37 + 0.11;
            double y = j * 0.23 - 0.19;
            double d = std::fabs((double)noise.GetNoise(x, y) - RefField(x, y, 0.73, 1337));
            if (d > worst)
                worst = d;
        }
    CHECK(worst < 1e-6);

    // GetNoiseFootprint must not modify the object: repeat and get the same result.
    FastNoiseLite before = MakeNoise();
    double first = Footprint(before, 0.1, 0.2, 0.3, 0.1, -0.05, 0.2);
    double second = Footprint(before, 0.1, 0.2, 0.3, 0.1, -0.05, 0.2);
    CHECK(first == second);
    CHECK((double)before.GetNoise(0.1, 0.2) == (double)MakeNoise().GetNoise(0.1, 0.2));
}

void TestFBmConsistencyWithGetNoise()
{
    // With a tiny footprint the FBm footprint average tends to the scalar FBm
    // value. This ties the octave ordering, seeds and weights to GetNoise.
    FastNoiseLite noise = MakeFBm(1337, 0.73f, 4, 1.75f, -0.5f);
    double x = -0.37, y = 0.92;
    double tiny = Footprint(noise, x, y, 1e-5, 2e-6, -3e-6, 1e-5);
    CHECK(Near(tiny, (double)noise.GetNoise(x, y), 1e-7));

    // Single octave, weight one: the FBm footprint equals the None footprint
    // when the frequency matches.
    FastNoiseLite fbmOne = MakeFBm(1337, 0.73f, 1, 1.75f, 0.5f);
    FastNoiseLite none = MakeNoise(1337, 0.73f);
    CHECK(Near(Footprint(fbmOne, x, y, 0.4, 0.1, -0.1, 0.3),
               Footprint(none, x, y, 0.4, 0.1, -0.1, 0.3), kExactTolerance));
}

} // namespace

int main()
{
    TestPointFallback();
    TestSingleCellAxisAligned();
    TestSingleCellTilted();
    TestMultiCellAndNegativeCoordinates();
    TestEdgesOnIntegerLatticeLines();
    TestOrientationInvariance();
    TestAreaSplitConservation();
    TestHighDensityIndependentIntegration();
    TestFrequencyAndLacunaritySigns();
    TestZeroFrequencyAndZeroLacunarity();
    TestRejectedInputs();
    TestCostLimits();
    TestGetNoiseRegression();
    TestFBmConsistencyWithGetNoise();

    std::printf("FASTNOISE_FOOTPRINT_OK\n");
    return 0;
}
