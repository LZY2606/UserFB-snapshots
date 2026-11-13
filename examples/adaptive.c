// Generates a torus with a localized periodic bump straddling the u seam
// using the adaptive parametric surface generator, writes
// <outdir>/adaptive.obj, and prints the leaf depth distribution plus
// mesh statistics.  Usage: adaptive [output-directory]
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

static void bumpy_torus(float const* uv, float* p, void* userdata)
{
    (void) userdata;
    float du = fabsf(uv[0]);
    du = fminf(du, 1.0f - du);
    float dv = uv[1] - 0.3f;
    float rr = du * du + dv * dv;
    float minor = 0.3f;
    float R = 0.15f;
    if (rr < R * R) {
        float t = 1.0f - rr / (R * R);
        minor += 0.1f * t * t * t;
    }
    float theta = uv[0] * 2.0f * (float) PAR_PI;
    float phi = uv[1] * 2.0f * (float) PAR_PI;
    float beta = 1.0f + minor * cosf(phi);
    p[0] = cosf(theta) * beta;
    p[1] = sinf(theta) * beta;
    p[2] = minor * sinf(phi);
}

int main(int argc, char** argv)
{
    char const* outdir = argc > 1 ? argv[1] : ".";
    par_shapes_adaptive* a = par_shapes_create_adaptive(bumpy_torus, 0,
        0.007f, 7, true, true);
    if (!a) {
        fprintf(stderr, "adaptive generation failed\n");
        return 1;
    }
    int hist[9] = {0};
    for (int i = 0; i < a->nleaves; i++) {
        hist[a->leaves[i].depth]++;
    }
    printf("adaptive torus: %d leaves, %d points, %d triangles\n",
        a->nleaves, a->mesh->npoints, a->mesh->ntriangles);
    for (int d = 2; d <= 8; d++) {
        if (hist[d]) {
            printf("  depth %d: %d leaves\n", d, hist[d]);
        }
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/adaptive.obj", outdir);
    par_shapes_export(a->mesh, path);
    printf("wrote %s\n", path);
    par_shapes_free_adaptive(a);
    return 0;
}
