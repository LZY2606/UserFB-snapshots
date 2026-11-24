/* Generates rounded versions of an oblique tetrahedron and a rotated,
   unequal-sided box.  Usage: rounded <output-directory>. */
#include "par_shapes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static par_shapes_mesh* make_mesh(int npoints, float const* points,
    int ntriangles, int const* indices) {
    par_shapes_mesh* mesh = (par_shapes_mesh*)calloc(1, sizeof(*mesh));
    if (!mesh) return NULL;
    mesh->npoints = npoints;
    mesh->ntriangles = ntriangles;
    mesh->points = (float*)malloc((size_t)3 * npoints * sizeof(float));
    mesh->triangles = (PAR_SHAPES_T*)malloc(
        (size_t)3 * ntriangles * sizeof(PAR_SHAPES_T));
    if (!mesh->points || !mesh->triangles) {
        free(mesh->points);
        free(mesh->triangles);
        free(mesh);
        return NULL;
    }
    memcpy(mesh->points, points, (size_t)3 * npoints * sizeof(float));
    for (int i = 0; i < 3 * ntriangles; ++i)
        mesh->triangles[i] = (PAR_SHAPES_T)indices[i];
    return mesh;
}

static void orient_outward(par_shapes_mesh* mesh) {
    double vol6 = 0.0;
    for (int f = 0; f < mesh->ntriangles; ++f) {
        float const* a = mesh->points + 3 * mesh->triangles[3*f];
        float const* b = mesh->points + 3 * mesh->triangles[3*f+1];
        float const* c = mesh->points + 3 * mesh->triangles[3*f+2];
        double ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
        double ac[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
        double cr[3] = {
            ab[1]*ac[2]-ab[2]*ac[1],
            ab[2]*ac[0]-ab[0]*ac[2],
            ab[0]*ac[1]-ab[1]*ac[0] };
        vol6 += a[0]*cr[0] + a[1]*cr[1] + a[2]*cr[2];
    }
    if (vol6 < 0.0) {
        for (int f = 0; f < mesh->ntriangles; ++f) {
            PAR_SHAPES_T tmp = mesh->triangles[3*f+1];
            mesh->triangles[3*f+1] = mesh->triangles[3*f+2];
            mesh->triangles[3*f+2] = tmp;
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }

    /* Non-orthogonal tetrahedron. */
    float tetra_points[12] = {
        0.18f, 0.10f, 0.00f,
        1.72f, 0.02f, 0.24f,
        0.08f, 1.31f,-0.18f,
        0.34f, 0.42f, 1.55f };
    int tetra_indices[12] = { 0,1,2, 0,3,1, 0,2,3, 1,3,2 };
    par_shapes_mesh* tetra_source = make_mesh(4, tetra_points, 4, tetra_indices);

    /* Unequal-sided box: full side lengths 2.60, 1.44 and 0.96. */
    float box_points[24] = {
        -1.30f,-0.72f,-0.48f,  1.30f,-0.72f,-0.48f,
         1.30f, 0.72f,-0.48f, -1.30f, 0.72f,-0.48f,
        -1.30f,-0.72f, 0.48f,  1.30f,-0.72f, 0.48f,
         1.30f, 0.72f, 0.48f, -1.30f, 0.72f, 0.48f };
    int box_indices[36] = {
        0,2,1, 0,3,2,
        4,5,6, 4,6,7,
        0,1,5, 0,5,4,
        1,2,6, 1,6,5,
        2,3,7, 2,7,6,
        3,0,4, 3,4,7 };
    par_shapes_mesh* box_source = make_mesh(8, box_points, 12, box_indices);
    if (!tetra_source || !box_source) {
        fprintf(stderr, "example allocation failed\n");
        free(tetra_source);
        free(box_source);
        return 1;
    }

    orient_outward(tetra_source);
    orient_outward(box_source);
    float axis[3] = { 0.35f, 0.55f, 0.76f };
    float axis_len = sqrtf(axis[0]*axis[0]+axis[1]*axis[1]+axis[2]*axis[2]);
    axis[0] /= axis_len; axis[1] /= axis_len; axis[2] /= axis_len;
    par_shapes_rotate(box_source, 0.67f, axis);

    const float radius = 0.22f;
    const float max_error = 0.012f;
    par_shapes_mesh* rounded_tetra =
        par_shapes_create_rounded(tetra_source, radius, max_error);
    par_shapes_mesh* rounded_box =
        par_shapes_create_rounded(box_source, radius, max_error);
    if (!rounded_tetra || !rounded_box) {
        fprintf(stderr, "par_shapes_create_rounded failed tetra=%p box=%p\n",
            (void*)rounded_tetra, (void*)rounded_box);
        if (rounded_tetra) par_shapes_free_mesh(rounded_tetra);
        if (rounded_box) par_shapes_free_mesh(rounded_box);
        par_shapes_free_mesh(tetra_source);
        par_shapes_free_mesh(box_source);
        return 1;
    }

    /* Separate the two independent solids before putting them in one OBJ. */
    par_shapes_translate(rounded_tetra, -2.2f, 0.0f, 0.0f);
    par_shapes_translate(rounded_box, 2.2f, 0.0f, 0.0f);
    int tetra_count = rounded_tetra->npoints;
    int tetra_triangles = rounded_tetra->ntriangles;
    int box_count = rounded_box->npoints;
    int box_triangles = rounded_box->ntriangles;

    par_shapes_mesh* scene = par_shapes_create_empty();
    if (!scene) {
        fprintf(stderr, "scene allocation failed\n");
        par_shapes_free_mesh(rounded_tetra);
        par_shapes_free_mesh(rounded_box);
        par_shapes_free_mesh(tetra_source);
        par_shapes_free_mesh(box_source);
        return 1;
    }
    par_shapes_merge_and_free(scene, rounded_tetra);
    par_shapes_merge_and_free(scene, rounded_box);

    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/rounded.obj", argv[1]);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "output path is too long\n");
        par_shapes_free_mesh(scene);
        par_shapes_free_mesh(tetra_source);
        par_shapes_free_mesh(box_source);
        return 2;
    }
    par_shapes_export(scene, path);

    printf("rounded tetra: points=%d triangles=%d\n",
        tetra_count, tetra_triangles);
    printf("rotated unequal box: points=%d triangles=%d\n",
        box_count, box_triangles);
    printf("rounded.obj: points=%d triangles=%d\n",
        scene->npoints, scene->ntriangles);

    par_shapes_free_mesh(scene);
    par_shapes_free_mesh(tetra_source);
    par_shapes_free_mesh(box_source);
    return 0;
}
