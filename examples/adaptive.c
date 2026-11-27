// Adaptive torus with a periodic local bump.
//
// Usage: adaptive <output-directory>
//
// Writes <output-directory>/adaptive.obj through par_shapes_export and prints
// the leaf-depth distribution together with point and triangle counts.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

#define TAU (2.0f * PAR_PI)

// Torus of major radius 1 and minor radius 0.3, plus a smooth bump that
// straddles the periodic u = 0 seam, so refinement must stay synchronized on
// both sides of the texture wrap.
typedef struct {
    float minor;
    float bump_radius;
    float bump_height;
} adaptive_config;

static void adaptive_surface(float const* uv, float* p, void* user)
{
    adaptive_config const* cfg = user;
    float theta = uv[0] * TAU;
    float phi = uv[1] * TAU;

    // Periodic distance from the u = 0 seam, then planar distance to the
    // bump center at (u, v) = (0, 0.5).
    float du = fminf(uv[0], 1.0f - uv[0]);
    float dv = uv[1] - 0.5f;
    float r = hypotf(du, dv) / cfg->bump_radius;
    float bump = r < 1.0f ?
        cfg->bump_height * powf(1.0f - r * r, 3.0f) : 0.0f;

    float beta = 1.0f + cfg->minor * cosf(phi);
    p[0] = cosf(theta) * beta;
    p[1] = sinf(theta) * beta;
    p[2] = cfg->minor * sinf(phi) + bump;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }

    // Small radii keep the base torus nearly flat (so it settles at shallow
    // depths), while the comparatively large periodic bump drives deep,
    // localized refinement across the seam.
    adaptive_config cfg = {0.01f, 0.10f, 0.18f};
    const float tolerance = 0.005f;
    const int max_depth = 7;

    // Both axes are periodic; the callback is smooth across u = 0 and v = 0.
    par_shapes_adaptive* result = par_shapes_create_adaptive(
        adaptive_surface, &cfg, tolerance, max_depth, true, true);
    if (!result) {
        fprintf(stderr, "adaptive generation failed\n");
        return 1;
    }

    int histogram[9] = {0};
    for (int i = 0; i < result->nleaves; i++) {
        histogram[result->leaves[i].depth]++;
    }
    printf("adaptive torus: %d leaves, %d points, %d triangles\n",
        result->nleaves, result->mesh->npoints,
        result->mesh->ntriangles);
    printf("leaf depth distribution:\n");
    for (int d = 2; d <= max_depth; d++) {
        if (histogram[d]) {
            printf("  depth %d: %d leaves\n", d, histogram[d]);
        }
    }

    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/adaptive.obj", argv[1]);
    if (written <= 0 || (size_t) written >= sizeof(path)) {
        fprintf(stderr, "output path too long\n");
        par_shapes_free_adaptive(result);
        return 1;
    }
    par_shapes_export(result->mesh, path);
    printf("wrote %s\n", path);

    par_shapes_free_adaptive(result);
    return 0;
}
