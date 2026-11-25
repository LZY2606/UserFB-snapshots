// Build a thick-walled shell from a generated open surface and write it as OBJ.
//
// Usage: shell <output-directory>
//
// The output directory is created if needed.  result.obj holds the shell; the
// point/face counts are printed to stdout.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

// C has no directory API.  Probe with stdio and fall back to the shell's mkdir;
// restrict the path to characters that are safe inside single quotes.
static int ensure_dir(const char* path)
{
    size_t len = strlen(path);
    if (len == 0 || len > 3000) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        int safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' ||
            c == '-' || c == '+';
        if (!safe) {
            return 0;
        }
    }
    char probe[4096];
    if ((size_t) snprintf(probe, sizeof(probe), "%s/.shell-probe", path) >=
        sizeof(probe)) {
        return 0;
    }
    FILE* f = fopen(probe, "w");
    if (f) {
        fclose(f);
        remove(probe);
        return 1;
    }
    char command[4200];
    if ((size_t) snprintf(command, sizeof(command), "mkdir -p '%s'", path) >=
        sizeof(command)) {
        return 0;
    }
    if (system(command) != 0) {
        return 0;
    }
    f = fopen(probe, "w");
    if (f) {
        fclose(f);
        remove(probe);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }
    const char* outdir = argv[1];
    if (!ensure_dir(outdir)) {
        fprintf(stderr, "could not create output directory: %s\n", outdir);
        return 1;
    }
    char path[4096];
    if ((size_t) snprintf(path, sizeof(path), "%s/result.obj", outdir) >=
        sizeof(path)) {
        fprintf(stderr, "output path too long\n");
        return 1;
    }

    // An open surface with a hole: a square annulus in the XY plane.
    float points[8 * 3] = {
        2, 0, 0, 2, 2, 0, 0, 2, 0, 0, 0, 0,
        1.5f, 0.5f, 0, 1.5f, 1.5f, 0, 0.5f, 1.5f, 0, 0.5f, 0.5f, 0,
    };
    int corners[8 * 3];
    int k = 0;
    for (int i = 0; i < 4; i++) {
        int o0 = i, o1 = (i + 1) % 4, i0 = 4 + i, i1 = 4 + (i + 1) % 4;
        corners[k++] = o0;
        corners[k++] = o1;
        corners[k++] = i1;
        corners[k++] = o0;
        corners[k++] = i1;
        corners[k++] = i0;
    }
    // Build the source with the public empty-mesh helper and the alloc macros.
    par_shapes_mesh* source = par_shapes_create_empty();
    if (!source) {
        return 1;
    }
    source->npoints = 8;
    source->points = PAR_MALLOC(float, 8 * 3);
    source->ntriangles = 8;
    source->triangles = PAR_MALLOC(PAR_SHAPES_T, 8 * 3);
    source->tcoords = PAR_MALLOC(float, 8 * 2);
    if (!source->points || !source->triangles || !source->tcoords) {
        par_shapes_free_mesh(source);
        return 1;
    }
    memcpy(source->points, points, sizeof(points));
    for (int i = 0; i < 8 * 3; i++) {
        source->triangles[i] = (PAR_SHAPES_T) corners[i];
    }
    for (int i = 0; i < 8; i++) {
        source->tcoords[2 * i] = points[3 * i] / 2;
        source->tcoords[2 * i + 1] = points[3 * i + 1] / 2;
    }

    par_shapes_shell* shell = par_shapes_create_shell(source, 0.25f);
    if (!shell) {
        fprintf(stderr, "par_shapes_create_shell failed\n");
        par_shapes_free_mesh(source);
        return 1;
    }
    par_shapes_export(shell->mesh, path);
    printf("source: points=%d faces=%d\n", source->npoints, source->ntriangles);
    printf("shell: points=%d faces=%d\n", shell->mesh->npoints,
        shell->mesh->ntriangles);
    printf("wrote %s\n", path);
    par_shapes_free_shell(shell);
    par_shapes_free_mesh(source);
    return 0;
}
