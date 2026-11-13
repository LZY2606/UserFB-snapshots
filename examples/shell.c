#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void ensure_directory(char const* path)
{
    struct stat info;
    if (stat(path, &info) == 0) {
        return;
    }
    mkdir(path, 0755);
}

// Flat annulus in the XY plane: an open surface with an outer boundary and
// an inner hole, so the resulting shell has two sealed boundary loops.
static par_shapes_mesh* make_annulus(int slices)
{
    int npoints = 2 * slices;
    int ntriangles = 2 * slices;
    par_shapes_mesh* mesh = (par_shapes_mesh*) calloc(1, sizeof(*mesh));
    mesh->npoints = npoints;
    mesh->ntriangles = ntriangles;
    mesh->points = (float*) malloc(sizeof(float) * 3 * npoints);
    mesh->normals = (float*) calloc(3 * npoints, sizeof(float));
    mesh->tcoords = (float*) malloc(sizeof(float) * 2 * npoints);
    mesh->triangles = (PAR_SHAPES_T*)
        malloc(sizeof(PAR_SHAPES_T) * 3 * ntriangles);
    for (int i = 0; i < slices; i++) {
        float angle = 2.0f * (float) M_PI * i / slices;
        float ca = cosf(angle);
        float sa = sinf(angle);
        float* outer = mesh->points + 3 * i;
        float* inner = mesh->points + 3 * (slices + i);
        outer[0] = ca;
        outer[1] = sa;
        outer[2] = 0.0f;
        inner[0] = 0.5f * ca;
        inner[1] = 0.5f * sa;
        inner[2] = 0.0f;
        for (int k = 0; k < 2; k++) {
            mesh->normals[3 * i + k] = 0.0f;
            mesh->normals[3 * (slices + i) + k] = 0.0f;
        }
        mesh->normals[3 * i + 2] = 1.0f;
        mesh->normals[3 * (slices + i) + 2] = 1.0f;
        mesh->tcoords[2 * i] = (float) i / slices;
        mesh->tcoords[2 * i + 1] = 1.0f;
        mesh->tcoords[2 * (slices + i)] = (float) i / slices;
        mesh->tcoords[2 * (slices + i) + 1] = 0.0f;
    }
    int t = 0;
    for (int i = 0; i < slices; i++) {
        int j = (i + 1) % slices;
        int oi = i, oj = j, ii = slices + i, ij = slices + j;
        mesh->triangles[t++] = (PAR_SHAPES_T) oi;
        mesh->triangles[t++] = (PAR_SHAPES_T) oj;
        mesh->triangles[t++] = (PAR_SHAPES_T) ij;
        mesh->triangles[t++] = (PAR_SHAPES_T) oi;
        mesh->triangles[t++] = (PAR_SHAPES_T) ij;
        mesh->triangles[t++] = (PAR_SHAPES_T) ii;
    }
    return mesh;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }
    char const* outdir = argv[1];
    ensure_directory(outdir);

    par_shapes_mesh* source = make_annulus(24);
    float thickness = 0.1f;
    par_shapes_shell* shell = par_shapes_create_shell(source, thickness);
    if (!shell) {
        fprintf(stderr, "failed to create shell\n");
        par_shapes_free_mesh(source);
        return 1;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/result.obj", outdir);
    par_shapes_export(shell->mesh, path);

    int inner = 0, outer = 0;
    for (int i = 0; i < shell->mesh->npoints; i++) {
        if (shell->vertex_layer[i] == 0) {
            inner++;
        } else {
            outer++;
        }
    }
    int skin_points = 2 * source->npoints;
    printf("source: points=%d triangles=%d\n", source->npoints,
        source->ntriangles);
    printf("thickness: %g\n", thickness);
    printf("shell: points=%d triangles=%d normals=%s tcoords=%s\n",
        shell->mesh->npoints, shell->mesh->ntriangles,
        shell->mesh->normals ? "yes" : "no",
        shell->mesh->tcoords ? "yes" : "no");
    printf("shell layers: outer=%d inner=%d rim-copies=%d\n", outer, inner,
        shell->mesh->npoints - skin_points);
    printf("wrote %s\n", path);

    par_shapes_free_shell(shell);
    par_shapes_free_mesh(source);
    return 0;
}
