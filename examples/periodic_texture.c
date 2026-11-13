// Periodic Voronoi texture example.
//
// Usage: periodic-texture <output-directory>
//
// Generates the asymmetric four-seed set inside the unit square, runs three
// Lloyd relaxation steps with alpha = 0.5, and writes
//   texture.svg  the tiling texture, drawn as every translated copy of each
//                unfolded cell clipped to the basic rectangle
//   cells.csv    one row per original seed: id,x,y,area,cx,cy,edges
//
// The SVG deliberately draws each cell copy with its own clip path instead of
// painting both sides of a seam the same colour, so seams that come from the
// periodic adjacency stay visible as real edges.
//
// Build (from the repository root, after `sh support/run.sh target`):
//   cc -std=c99 -O2 -I src -I support -include support/config.h \
//      examples/periodic_texture.c -lm -o build/periodic-texture

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JC_VORONOI_IMPLEMENTATION
#define JC_VORONOI_PERIODIC_IMPLEMENTATION
#include "jc_voronoi.h"
#include "jc_voronoi_periodic.h"

#define TEXTURE_SEEDS 4

static const jcv_point kInitialSeeds[TEXTURE_SEEDS] = {
    { (jcv_real)0.08, (jcv_real)0.17 },
    { (jcv_real)0.58, (jcv_real)0.22 },
    { (jcv_real)0.33, (jcv_real)0.81 },
    { (jcv_real)0.87, (jcv_real)0.69 },
};

static const char* kColors[TEXTURE_SEEDS] = {
    "#e76f51",
    "#2a9d8f",
    "#e9c46a",
    "#264653",
};

static int write_svg(const char* path, const jcv_periodic_diagram* diagram)
{
    FILE* file = fopen(path, "wb");
    if( !file )
        return 0;

    double w = (double)diagram->rect.max.x - (double)diagram->rect.min.x;
    double h = (double)diagram->rect.max.y - (double)diagram->rect.min.y;
    double scale = 480.0;
    double view_w = w * scale;
    double view_h = h * scale;

    fprintf(file, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(file, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.3f\" height=\"%.3f\" "
                  "viewBox=\"%.6f %.6f %.6f %.6f\">\n",
            view_w, view_h, (double)diagram->rect.min.x, (double)diagram->rect.min.y, w, h);
    fprintf(file, "  <rect x=\"%.6f\" y=\"%.6f\" width=\"%.6f\" height=\"%.6f\" fill=\"#101010\"/>\n",
            (double)diagram->rect.min.x, (double)diagram->rect.min.y, w, h);
    fprintf(file, "  <defs>\n");
    fprintf(file, "    <clipPath id=\"basic-rect\">\n");
    fprintf(file, "      <rect x=\"%.6f\" y=\"%.6f\" width=\"%.6f\" height=\"%.6f\"/>\n",
            (double)diagram->rect.min.x, (double)diagram->rect.min.y, w, h);
    fprintf(file, "    </clipPath>\n");
    fprintf(file, "  </defs>\n");

    for( int i = 0; i < diagram->numsites; ++i )
    {
        const jcv_periodic_cell* cell = &diagram->cells[i];
        int kx_min = (int)floor(((double)diagram->rect.min.x - (double)cell->point.x) / w) - 1;
        int kx_max = (int)ceil(((double)diagram->rect.max.x - (double)cell->point.x) / w) + 1;
        int ky_min = (int)floor(((double)diagram->rect.min.y - (double)cell->point.y) / h) - 1;
        int ky_max = (int)ceil(((double)diagram->rect.max.y - (double)cell->point.y) / h) + 1;

        // Every translated copy that can intersect the basic rectangle is
        // emitted and clipped.  Copies of the same original seed share a colour,
        // but their edges are still drawn individually, so a seam is a real
        // edge at the exact periodic boundary rather than a painted-over gap.
        for( int ky = ky_min; ky <= ky_max; ++ky )
        {
            for( int kx = kx_min; kx <= kx_max; ++kx )
            {
                double shift_x = (double)kx * w;
                double shift_y = (double)ky * h;

                // Cheap bounding-box rejection before emitting a copy.
                double bmin_x = (double)cell->point.x + shift_x;
                double bmin_y = (double)cell->point.y + shift_y;
                double bmax_x = bmin_x;
                double bmax_y = bmin_y;
                for( int e = 0; e < cell->num_edges; ++e )
                {
                    const jcv_periodic_edge* edge = &diagram->edges[cell->first_edge + e];
                    double ex = (double)edge->pos[0].x + shift_x;
                    double ey = (double)edge->pos[0].y + shift_y;
                    if( ex < bmin_x ) bmin_x = ex;
                    if( ex > bmax_x ) bmax_x = ex;
                    if( ey < bmin_y ) bmin_y = ey;
                    if( ey > bmax_y ) bmax_y = ey;
                }
                double margin = 0.01 * (w > h ? w : h);
                if( bmax_x < (double)diagram->rect.min.x - margin ||
                    bmin_x > (double)diagram->rect.max.x + margin ||
                    bmax_y < (double)diagram->rect.min.y - margin ||
                    bmin_y > (double)diagram->rect.max.y + margin )
                {
                    continue;
                }

                fprintf(file, "  <g clip-path=\"url(#basic-rect)\">\n");
                fprintf(file, "    <polygon points=\"");
                for( int e = 0; e < cell->num_edges; ++e )
                {
                    const jcv_periodic_edge* edge = &diagram->edges[cell->first_edge + e];
                    fprintf(file, "%.9f,%.9f ", (double)edge->pos[0].x + shift_x,
                            (double)edge->pos[0].y + shift_y);
                }
                fprintf(file, "\" fill=\"%s\" stroke=\"#101010\" stroke-width=\"%.6f\"/>\n",
                        kColors[i], 0.004 * (w > h ? w : h));
                fprintf(file, "  </g>\n");
            }
        }
    }

    fprintf(file, "  <rect x=\"%.6f\" y=\"%.6f\" width=\"%.6f\" height=\"%.6f\" "
                  "fill=\"none\" stroke=\"#101010\" stroke-width=\"%.6f\"/>\n",
            (double)diagram->rect.min.x, (double)diagram->rect.min.y, w, h,
            0.006 * (w > h ? w : h));
    fprintf(file, "</svg>\n");

    int ok = ferror(file) == 0;
    if( fclose(file) != 0 )
        ok = 0;
    return ok;
}

static int write_csv(const char* path, const jcv_periodic_diagram* diagram)
{
    FILE* file = fopen(path, "wb");
    if( !file )
        return 0;
    fprintf(file, "id,x,y,area,cx,cy,edges\n");
    for( int i = 0; i < diagram->numsites; ++i )
    {
        const jcv_periodic_cell* cell = &diagram->cells[i];
        fprintf(file, "%d,%.17g,%.17g,%.17g,%.17g,%.17g,%d\n",
                i, (double)cell->point.x, (double)cell->point.y, (double)cell->area,
                (double)cell->centroid.x, (double)cell->centroid.y, cell->num_edges);
    }
    int ok = ferror(file) == 0;
    if( fclose(file) != 0 )
        ok = 0;
    return ok;
}

int main(int argc, char** argv)
{
    if( argc != 2 )
    {
        fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 2;
    }

    jcv_rect rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)1.0 } };
    jcv_point seeds[TEXTURE_SEEDS];
    memcpy(seeds, kInitialSeeds, sizeof(seeds));

    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));
    if( !jcv_periodic_generate(TEXTURE_SEEDS, seeds, &rect, &diagram) )
    {
        fprintf(stderr, "periodic generation failed\n");
        return 1;
    }

    for( int iteration = 0; iteration < 3; ++iteration )
    {
        jcv_point relaxed[TEXTURE_SEEDS];
        if( !jcv_periodic_relax(&diagram, (jcv_real)0.5, relaxed) )
        {
            fprintf(stderr, "periodic relaxation failed\n");
            jcv_periodic_free(&diagram);
            return 1;
        }
        memcpy(seeds, relaxed, sizeof(seeds));
        jcv_periodic_free(&diagram);
        if( !jcv_periodic_generate(TEXTURE_SEEDS, seeds, &rect, &diagram) )
        {
            fprintf(stderr, "periodic regeneration failed\n");
            return 1;
        }
    }

    char svg_path[4096];
    char csv_path[4096];
    if( snprintf(svg_path, sizeof(svg_path), "%s/texture.svg", argv[1]) >= (int)sizeof(svg_path) ||
        snprintf(csv_path, sizeof(csv_path), "%s/cells.csv", argv[1]) >= (int)sizeof(csv_path) )
    {
        fprintf(stderr, "output path is too long\n");
        jcv_periodic_free(&diagram);
        return 1;
    }

    int ok = write_svg(svg_path, &diagram);
    ok = write_csv(csv_path, &diagram) && ok;

    printf("wrote %s and %s\n", svg_path, csv_path);
    jcv_periodic_free(&diagram);
    return ok ? 0 : 1;
}
