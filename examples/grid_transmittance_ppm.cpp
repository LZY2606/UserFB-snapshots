// examples/grid_transmittance_ppm.cpp
//
// Windowless PPM example for the stochastic VolumeGridDensity queries.
// Renders a pinhole view of a density-grid volume in front of a colored
// background; every pixel's color is formed by averaging real
// RatioTransmittance() samples along the pixel ray, i.e. the image shows
// the colored shadow the participating medium casts.  No windowing
// system, OpenEXR or scene files are involved.
//
// Usage: grid_transmittance_ppm [output.ppm]

#include "volumes/volumegrid.h"
#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "grid_transmittance.ppm";

    // Density grid: a soft blob with a smaller dense core, 24^3 samples.
    const int n = 24;
    std::vector<float> density(n*n*n);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                float u = (x+.5f)/n * 2.f - 1.f;
                float v = (y+.5f)/n * 2.f - 1.f;
                float w = (z+.5f)/n * 2.f - 1.f;
                float d = 1.6f * expf(-4.f*(u*u + v*v + w*w));
                float cu = u-.35f, cv = v+.2f, cw = w-.1f;
                d += 3.f * expf(-25.f*(cu*cu + cv*cv + cw*cw));
                density[z*n*n + y*n + x] = d;
            }

    // Colored extinction: blue is attenuated most, so the shadow of the
    // cloud comes out warm.  The volume sits 3.5..5.5 units down +z.
    const float sa[3] = { .15f, .5f, 1.2f };
    const float ss[3] = { .3f, .25f, .2f };
    VolumeGridDensity grid(Spectrum::FromRGB(sa), Spectrum::FromRGB(ss), 0.f,
        Spectrum(0.f), BBox(Point(-1, -1, -1), Point(1, 1, 1)),
        Translate(Vector(0.f, 0.f, 4.5f)), n, n, n, &density[0]);

    const int W = 128, H = 128, SPP = 16;
    std::vector<unsigned char> img(W*H*3);
    RNG rng(2024u);
    GridTrackingStats stats;
    double totalVisits = 0., totalEvals = 0.;
    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
            float u = (i+.5f)/W * 2.f - 1.f;
            float v = 1.f - (j+.5f)/H * 2.f;
            Ray ray(Point(0, 0, 0), Normalize(Vector(u, v, 2.2f)), 0.f);
            float acc[3] = { 0.f, 0.f, 0.f };
            for (int s = 0; s < SPP; ++s) {
                // The pixel color is built from genuine transmittance
                // samples of the medium.
                Spectrum T = grid.RatioTransmittance(ray, rng, &stats);
                totalVisits += (double)stats.brick_visits;
                totalEvals += (double)stats.density_evaluations;
                float rgb[3];
                T.ToRGB(rgb);
                for (int c = 0; c < 3; ++c) acc[c] += rgb[c];
            }
            float bg[3] = { .95f, .85f + .1f*v, .6f + .3f*(v*.5f + .5f) };
            for (int c = 0; c < 3; ++c) {
                float col = acc[c]/SPP * bg[c];
                col = col < 0.f ? 0.f : (col > 1.f ? 1.f : col);
                img[(j*W + i)*3 + c] =
                    (unsigned char)(255.f*powf(col, 1.f/2.2f) + .5f);
            }
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(&img[0], 3, W*H, f);
    fclose(f);
    double rays = (double)W*H*SPP;
    std::printf("wrote %s (%dx%d, %d spp); avg brick_visits=%.2f "
        "avg density_evals=%.2f per query\n", path, W, H, SPP,
        totalVisits/rays, totalEvals/rays);
    return 0;
}
