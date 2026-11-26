// Renders a side-by-side strip: center-sampled ValueCubic vs the exact
// footprint average, both under the same FBm settings.
//
// Build and run:
//   c++ -std=c++14 -O2 -I . -fwrapv examples/footprint.cpp -o footprint_example
//   ./footprint_example out.pgm
//
// The output is a binary PGM. Left half uses GetNoise at pixel centers; right
// half uses GetNoiseFootprint with the pixel-sized footprint. The footprint
// side vectors are expressed in input coordinate units, before frequency and
// before any fractal scaling, and are the full side lengths of the pixel (the
// covered region is center + s*u + t*v with s, t in [-0.5, 0.5]).

#include "Cpp/FastNoiseLite.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{

const int kWidth = 360;      // per half
const int kHeight = 240;
const double kOriginX = -7.2; // input-coordinate start of the image
const double kOriginY = -4.8;
const double kStep = 0.04;     // input units per pixel -> footprint sides

unsigned char ToByte(float value)
{
    double t = (value + 1.0) * 0.5;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    return (unsigned char)(t * 255.0 + 0.5);
}

bool WritePGM(const char* path, const std::vector<unsigned char>& pixels, int width, int height)
{
    std::FILE* file = std::fopen(path, "wb");
    if (!file)
        return false;
    std::fprintf(file, "P5\n%d %d\n255\n", width, height);
    bool ok = std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
    std::fclose(file);
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s output.pgm\n", argv[0]);
        return 2;
    }

    FastNoiseLite noise(1337);
    noise.SetNoiseType(FastNoiseLite::NoiseType_ValueCubic);
    noise.SetFrequency(5.0f);
    noise.SetFractalType(FastNoiseLite::FractalType_FBm);
    noise.SetFractalOctaves(5);
    noise.SetFractalLacunarity(2.0f);
    noise.SetFractalGain(0.5f);
    noise.SetFractalWeightedStrength(0.0f);

    // Tilted, anisotropic pixel footprint: a slanted view along a grazing angle.
    const double ux = kStep * 1.0;
    const double uy = kStep * 0.35;
    const double vx = kStep * -0.20;
    const double vy = kStep * 0.95;

    const int totalWidth = kWidth * 2 + 8;
    std::vector<unsigned char> pixels((size_t)totalWidth * kHeight, 0);

    FastNoiseLite::NoiseFootprint2D footprint;
    footprint.ux = ux;
    footprint.uy = uy;
    footprint.vx = vx;
    footprint.vy = vy;

    for (int row = 0; row < kHeight; row++)
    {
        for (int col = 0; col < kWidth; col++)
        {
            double x = kOriginX + col * kStep;
            double y = kOriginY + row * kStep;

            unsigned char center = ToByte(noise.GetNoise(x, y));
            double averaged = noise.GetNoiseFootprint(x, y, footprint);
            unsigned char average = ToByte((float)averaged);

            pixels[(size_t)row * totalWidth + col] = center;
            pixels[(size_t)row * totalWidth + kWidth + 8 + col] = average;
        }
    }

    if (!WritePGM(argv[1], pixels, totalWidth, kHeight))
    {
        std::fprintf(stderr, "failed to write %s\n", argv[1]);
        return 1;
    }

    std::printf("wrote %s (%dx%d)\n", argv[1], totalWidth, kHeight);
    std::printf("left: center GetNoise, right: GetNoiseFootprint average\n");
    std::printf("footprint sides in input coordinate units: u=(%g, %g) v=(%g, %g)\n", ux, uy, vx, vy);
    std::printf("region is (x,y) + s*u + t*v with s,t in [-0.5, 0.5]; values are pre-frequency\n");
    std::printf("FASTNOISE_FOOTPRINT_EXAMPLE_OK\n");
    return 0;
}
