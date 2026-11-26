/*
 * periodic_texture.c
 *
 * Builds a small periodic/cellular texture with three Lloyd relaxation
 * rounds and writes texture.svg and cells.csv into the directory given as
 * argv[1].
 *
 * Compile (from the repository root):
 *   cc -std=c99 -O2 -Wall -Wextra -I src -I support \
 *      -include support/config.h examples/periodic_texture.c -lm \
 *      -o build/periodic-texture
 */

#include "config.h"

#define JC_VORONOI_PERIODIC_IMPLEMENTATION
#include "jc_voronoi_periodic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEXTURE_PIXELS 640
#define TEXTURE_MARGIN 36

static const char* k_colors[4] = {
    "#4c78a8", "#f58518", "#54a24b", "#e45756"
};

static void fail(const char* message)
{
    fprintf(stderr, "periodic_texture: %s\n", message);
    exit(1);
}

static void make_path(char* destination, size_t destination_size,
                      const char* directory, const char* filename)
{
    int written = snprintf(destination, destination_size, "%s/%s",
                           directory, filename);
    if (written < 0 || (size_t)written >= destination_size)
        fail("output path is too long");
}

static double map_x(double x, const jcv_rect* rect, double scale)
{
    return TEXTURE_MARGIN + (x - rect->min.x) * scale;
}

static double map_y(double y, const jcv_rect* rect, double scale,
                    double image_height)
{
    return image_height - TEXTURE_MARGIN -
           (y - rect->min.y) * scale;
}

static void write_svg(const char* path, const jcv_periodic_diagram* diagram)
{
    const jcv_rect* rect = &diagram->rect;
    double W = rect->max.x - rect->min.x;
    double H = rect->max.y - rect->min.y;
    double scale = TEXTURE_PIXELS / (W > H ? W : H);
    double width = 2.0 * TEXTURE_MARGIN + W * scale;
    double height = 2.0 * TEXTURE_MARGIN + H * scale;
    FILE* svg = fopen(path, "w");
    int i;

    if (!svg)
        fail("could not open texture.svg for writing");

    fprintf(svg,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" "
            "width=\"%.0f\" height=\"%.0f\" viewBox=\"0 0 %.0f %.0f\">\n",
            width, height, width, height);
    fprintf(svg,
            "  <defs>\n"
            "    <clipPath id=\"tile-clip\">\n"
            "      <rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" "
            "height=\"%.3f\"/>\n"
            "    </clipPath>\n"
            "  </defs>\n",
            (double)TEXTURE_MARGIN, (double)TEXTURE_MARGIN,
            W * scale, H * scale);
    fprintf(svg,
            "  <rect x=\"0\" y=\"0\" width=\"%.0f\" height=\"%.0f\" "
            "fill=\"#f7f7f4\"/>\n", width, height);
    fprintf(svg,
            "  <rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\" "
            "fill=\"#ffffff\"/>\n",
            (double)TEXTURE_MARGIN, (double)TEXTURE_MARGIN,
            W * scale, H * scale);

    fprintf(svg, "  <g clip-path=\"url(#tile-clip)\">\n");

    /*
     * Each jcv_periodic_cell is one complete unwrapped convex polygon.
     * Its -1/0/+1 lattice translations cover every fragment intersecting
     * the half-open fundamental rectangle.  SVG clipping turns those copies
     * into tileable fragments without modulo-wrapping individual vertices.
     */
    for (i = 0; i < diagram->numsites; ++i)
    {
        int tx, ty;
        const char* color = k_colors[i % 4];

        for (ty = -1; ty <= 1; ++ty)
        {
            for (tx = -1; tx <= 1; ++tx)
            {
                const jcv_periodic_cell* cell = &diagram->cells[i];
                double min_x = 1.0e300, min_y = 1.0e300;
                double max_x = -1.0e300, max_y = -1.0e300;
                int k;
                int intersects;

                for (k = 0; k < cell->num_edges; ++k)
                {
                    const jcv_periodic_edge* e =
                        &diagram->edges[cell->first_edge + k];
                    double lx = e->pos[0].x - rect->min.x;
                    double ly = e->pos[0].y - rect->min.y;
                    if (lx < min_x) min_x = lx;
                    if (lx > max_x) max_x = lx;
                    if (ly < min_y) min_y = ly;
                    if (ly > max_y) max_y = ly;
                }

                /* Closed-interval overlap: boundary-only copies are kept. */
                intersects =
                    max_x + (double)tx * W >= 0.0 &&
                    min_x + (double)tx * W <= W &&
                    max_y + (double)ty * H >= 0.0 &&
                    min_y + (double)ty * H <= H;
                if (!intersects)
                    continue;

                fprintf(svg,
                        "    <polygon fill=\"%s\" fill-opacity=\"0.30\" "
                        "stroke=\"%s\" stroke-opacity=\"0.95\" "
                        "stroke-width=\"2.2\" stroke-linejoin=\"round\" "
                        "points=\"",
                        color, color);

                for (k = 0; k < cell->num_edges; ++k)
                {
                    const jcv_periodic_edge* e =
                        &diagram->edges[cell->first_edge + k];
                    double px = (double)e->pos[0].x + (double)tx * W;
                    double py = (double)e->pos[0].y + (double)ty * H;
                    fprintf(svg, "%.3f,%.3f ",
                            map_x(px, rect, scale),
                            map_y(py, rect, scale, height));
                }
                fprintf(svg, "\"/>\n");
            }
        }

        /* The normalized seed itself. */
        fprintf(svg,
                "    <circle cx=\"%.3f\" cy=\"%.3f\" r=\"4.2\" "
                "fill=\"#222222\"/>\n",
                map_x(diagram->cells[i].point.x, rect, scale),
                map_y(diagram->cells[i].point.y, rect, scale, height));
    }

    fprintf(svg, "  </g>\n");

    /* Closed boundary is drawn on top; shared/duplicated seam edges are
       intentional and keep both periodic sides vector-aligned. */
    fprintf(svg,
            "  <rect x=\"%.3f\" y=\"%.3f\" width=\"%.3f\" height=\"%.3f\" "
            "fill=\"none\" stroke=\"#202020\" stroke-width=\"2.4\"/>\n",
            (double)TEXTURE_MARGIN, (double)TEXTURE_MARGIN,
            W * scale, H * scale);
    fprintf(svg, "</svg>\n");

    if (fclose(svg) != 0)
        fail("could not close texture.svg");
}

static void write_csv(const char* path, const jcv_periodic_diagram* diagram)
{
    FILE* csv = fopen(path, "w");
    int i;
    if (!csv)
        fail("could not open cells.csv for writing");

    fprintf(csv, "id,x,y,area,cx,cy,edges\n");
    for (i = 0; i < diagram->numsites; ++i)
    {
        const jcv_periodic_cell* c = &diagram->cells[i];
        fprintf(csv, "%d,%.12g,%.12g,%.12g,%.12g,%.12g,%d\n",
                i, (double)c->point.x, (double)c->point.y,
                (double)c->area, (double)c->centroid.x,
                (double)c->centroid.y, c->num_edges);
    }

    if (fclose(csv) != 0)
        fail("could not close cells.csv");
}

int main(int argc, char** argv)
{
    jcv_rect rect = {{0.0, 0.0}, {1.0, 1.0}};
    jcv_point seeds[4] = {
        {0.08, 0.17}, {0.58, 0.22}, {0.33, 0.81}, {0.87, 0.69}
    };
    jcv_periodic_diagram diagram;
    char svg_path[4096];
    char csv_path[4096];
    int round;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s output-directory\n",
                argc > 0 ? argv[0] : "periodic_texture");
        return 2;
    }

    memset(&diagram, 0, sizeof(diagram));
    if (!jcv_periodic_generate(4, seeds, &rect, &diagram))
        fail("initial periodic generation failed");

    for (round = 0; round < 3; ++round)
    {
        jcv_point relaxed[4];

        if (!jcv_periodic_relax(&diagram, (jcv_real)0.5, relaxed))
            fail("periodic relaxation failed");

        jcv_periodic_free(&diagram);
        if (!jcv_periodic_generate(4, relaxed, &rect, &diagram))
            fail("regeneration after Lloyd relaxation failed");
    }

    make_path(svg_path, sizeof(svg_path), argv[1], "texture.svg");
    make_path(csv_path, sizeof(csv_path), argv[1], "cells.csv");
    write_svg(svg_path, &diagram);
    write_csv(csv_path, &diagram);

    printf("periodic texture written to %s and %s\n",
           svg_path, csv_path);
    jcv_periodic_free(&diagram);
    return 0;
}
