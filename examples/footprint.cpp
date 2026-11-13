// Footprint-average versus center-sample comparison image.
//
// Usage: footprint <output.pgm>
//
// Writes a grayscale PGM containing two horizontal strips of the same 2D
// ValueCubic field rendered through an oblique (slanted) pixel mapping:
//   * top strip    - ordinary center-point GetNoise(...) sampling
//   * bottom strip - exact GetNoiseFootprint(...) area averaging
// The center strip shows the high-frequency grain/shimmer of taking only the
// value at the pixel center; the footprint strip is the anti-aliased area
// average over the pixel's true tilted footprint.
//
// Self-contained C++14, no third-party dependencies.
#include "Cpp/FastNoiseLite.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
using FN = FastNoiseLite;

const int kWidth = 256;
const int kStripHeight = 64; // each strip is this many pixels tall

// World-space mapping of a screen pixel. A pixel covers the parallelogram
// centered at its center with edge vectors kColumnStep and kRowStep. These
// vectors are expressed in the SAME units as the coordinates passed to
// GetNoise/GetNoiseFootprint (noise input coordinate units, before the
// configured frequency is applied); each is a FULL pixel edge, so the
// footprint spans s, t in [-0.5, 0.5]. The skew term makes the footprint
// slanted, as when a surface is viewed obliquely.
const FN::NoiseFootprint2D kPixelFootprint = {
    /* ux, uy (column step) */ 0.085, 0.028,
    /* vx, vy (row step)    */ -0.022, 0.085
};

void pixelCenter(int i, int j, double& x, double& y)
{
    x = -6.0 + i * kPixelFootprint.ux + j * kPixelFootprint.vx;
    y = i * kPixelFootprint.uy + j * kPixelFootprint.vy;
}

std::uint8_t toByte(float v)
{
    if (v < -1.0f)
        v = -1.0f;
    if (v > 1.0f)
        v = 1.0f;
    return static_cast<std::uint8_t>((v + 1.0f) * 127.5f);
}

std::uint8_t toByte(double v)
{
    if (v < -1.0)
        v = -1.0;
    if (v > 1.0)
        v = 1.0;
    return static_cast<std::uint8_t>(v * 127.5 + 127.5);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "footprint") << " <output.pgm>\n";
        return 2;
    }

    FN noise(1337);
    noise.SetNoiseType(FN::NoiseType_ValueCubic);
    // A frequency high enough that the field changes across one pixel, which
    // is exactly where center-point sampling shimmers and area averaging helps.
    noise.SetFrequency(6.0f);
    // FBm with zero weighted strength is one of the supported configurations.
    noise.SetFractalType(FN::FractalType_FBm);
    noise.SetFractalOctaves(4);
    noise.SetFractalLacunarity(2.0f);
    noise.SetFractalGain(0.5f);
    noise.SetFractalWeightedStrength(0);

    const int height = kStripHeight * 2;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * height);

    // Top strip: center sampling only.
    for (int j = 0; j < kStripHeight; ++j)
    {
        for (int i = 0; i < kWidth; ++i)
        {
            double x, y;
            pixelCenter(i, j, x, y);
            pixels[static_cast<std::size_t>(j) * kWidth + i] = toByte(noise.GetNoise(x, y));
        }
    }

    // Bottom strip: exact area average over the same slanted pixel footprint
    // covering the identical world region as the top strip.
    for (int j = 0; j < kStripHeight; ++j)
    {
        for (int i = 0; i < kWidth; ++i)
        {
            double x, y;
            pixelCenter(i, j, x, y);
            double average = noise.GetNoiseFootprint(x, y, kPixelFootprint);
            pixels[static_cast<std::size_t>(kStripHeight + j) * kWidth + i] = toByte(average);
        }
    }

    std::ofstream out(argv[1], std::ios::binary);
    if (!out)
    {
        std::cerr << "failed to open output file: " << argv[1] << "\n";
        return 1;
    }
    out << "P5\n"
        << "# top: center GetNoise; bottom: GetNoiseFootprint area average\n"
        << kWidth << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixels.size()));
    if (!out)
    {
        std::cerr << "failed to write output file: " << argv[1] << "\n";
        return 1;
    }

    std::cout << "wrote " << argv[1] << " (" << kWidth << "x" << height
              << " PGM): upper strip center-sampled, lower strip footprint-averaged\n";
    std::cout << "edge vector units: noise input coordinate units (the same units as x, y); "
                 "each vector is one full pixel edge, footprint spans s,t in [-0.5,0.5]\n";
    std::cout << "supported range: NoiseType_ValueCubic with FractalType_None, or "
                 "FractalType_FBm with weighted strength 0; finite arguments; "
                 "1..12 octaves; finite gain with |gain|<=1; per-octave vertex "
                 "|coordinate|<=1048576 and at most 65536 intersected cells\n";
    std::cout << "FASTNOISE_FOOTPRINT_EXAMPLE_OK\n";
    return 0;
}
