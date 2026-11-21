// Example: repair a self-overlapping strip and write an SVG colored by chart.
//
// Usage: injective <output.svg>

#include "thekla/thekla_atlas.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace Thekla;

namespace
{
    // Spiral ribbon: every UV triangle keeps a positive, consistent
    // orientation while successive coils overlap in UV space.
    static void buildSpiral(std::vector<Atlas_Input_Vertex> & vertices,
                            std::vector<Atlas_Input_Face> & faces)
    {
        const double R0 = 20.0, k = 0.2, width = 0.8, turns = 2.2;
        const int segments = 140;

        std::vector< std::pair<double, double> > center;
        for (int i = 0; i <= segments; i++) {
            const double th = 2.0 * M_PI * turns * i / segments;
            const double rad = R0 - k * th;
            center.push_back(std::make_pair(rad * std::cos(th), rad * std::sin(th)));
        }

        const int n = (int)center.size();
        std::vector<int> left(n), right(n);
        for (int i = 0; i < n; i++) {
            double dx, dy;
            if (i + 1 < n) {
                dx = center[i + 1].first - center[i].first;
                dy = center[i + 1].second - center[i].second;
            } else {
                dx = center[i].first - center[i - 1].first;
                dy = center[i].second - center[i - 1].second;
            }
            const double len = std::sqrt(dx * dx + dy * dy);
            dx /= len; dy /= len;
            const double nx = -dy, ny = dx;

            const float lx = float(center[i].first + width * nx);
            const float ly = float(center[i].second + width * ny);
            const float rx = float(center[i].first - width * nx);
            const float ry = float(center[i].second - width * ny);

            Atlas_Input_Vertex vl;
            vl.position[0] = float(i); vl.position[1] = float(width); vl.position[2] = 0;
            vl.normal[0] = 0; vl.normal[1] = 0; vl.normal[2] = 1;
            vl.uv[0] = lx; vl.uv[1] = ly; vl.first_colocal = (int)vertices.size();
            left[i] = (int)vertices.size();
            vertices.push_back(vl);

            Atlas_Input_Vertex vr;
            vr.position[0] = float(i); vr.position[1] = float(-width); vr.position[2] = 0;
            vr.normal[0] = 0; vr.normal[1] = 0; vr.normal[2] = 1;
            vr.uv[0] = rx; vr.uv[1] = ry; vr.first_colocal = (int)vertices.size();
            right[i] = (int)vertices.size();
            vertices.push_back(vr);
        }

        for (int i = 0; i + 1 < n; i++) {
            Atlas_Input_Face a, b;
            a.vertex_index[0] = left[i];
            a.vertex_index[1] = right[i];
            a.vertex_index[2] = right[i + 1];
            b.vertex_index[0] = left[i];
            b.vertex_index[1] = right[i + 1];
            b.vertex_index[2] = left[i + 1];
            a.material_index = b.material_index = 0;
            faces.push_back(a);
            faces.push_back(b);
        }
    }

    static void chartColor(int index, int rgb[3])
    {
        // Deterministic, well separated hues.
        const int palette[][3] = {
            { 230,  86,  69 },
            {  64, 160, 220 },
            {  80, 180, 110 },
            { 235, 170,  50 },
            { 160, 110, 210 },
            {  60, 190, 190 },
            { 220, 120, 170 },
            { 150, 150,  70 },
        };
        const int * c = palette[index % (int)(sizeof(palette) / sizeof(palette[0]))];
        rgb[0] = c[0]; rgb[1] = c[1]; rgb[2] = c[2];
    }
}

int main(int argc, char ** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <output.svg>\n", argv[0]);
        return EXIT_FAILURE;
    }

    std::vector<Atlas_Input_Vertex> vertices;
    std::vector<Atlas_Input_Face> faces;
    buildSpiral(vertices, faces);

    Atlas_Input_Mesh input;
    input.vertex_count = (int)vertices.size();
    input.face_count = (int)faces.size();
    input.vertex_array = &vertices[0];
    input.face_array = &faces[0];

    Atlas_Options options;
    atlas_set_default_options(&options);
    options.charter = Atlas_Charter_Extract;
    options.mapper_options.preserve_uvs = true;
    options.mapper_options.preserve_boundary = true;
    options.packer_options.witness.packing_quality = 1;

    Atlas_Error error;
    Atlas_Injective_Report * report = NULL;
    Atlas_Output_Mesh * output = atlas_generate_injective(&input, &options, 128, &report, &error);

    if (output == NULL || error != Atlas_Error_Success) {
        std::fprintf(stderr, "atlas_generate_injective failed with error %d\n", (int)error);
        atlas_injective_report_free(report);
        return EXIT_FAILURE;
    }

    FILE * svg = std::fopen(argv[1], "w");
    if (svg == NULL) {
        std::fprintf(stderr, "Cannot open %s for writing\n", argv[1]);
        atlas_free(output);
        atlas_injective_report_free(report);
        return EXIT_FAILURE;
    }

    const int width = output->atlas_width;
    const int height = output->atlas_height;
    std::fprintf(svg,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
        "viewBox=\"0 0 %d %d\">\n", width, height, width, height);
    std::fprintf(svg, "<rect width=\"%d\" height=\"%d\" fill=\"#101418\"/>\n", width, height);

    for (int f = 0; f < input.face_count; f++) {
        const int chart = report->face_chart[f];
        int rgb[3];
        chartColor(chart, rgb);

        const float * uv[3];
        for (int k = 0; k < 3; k++) {
            const int vi = output->index_array[3 * f + k];
            uv[k] = output->vertex_array[vi].uv;
        }
        std::fprintf(svg,
            "<polygon points=\"%.3f,%.3f %.3f,%.3f %.3f,%.3f\" "
            "fill=\"#%02x%02x%02x\" stroke=\"#000000\" stroke-width=\"0.15\"/>\n",
            uv[0][0], uv[0][1], uv[1][0], uv[1][1], uv[2][0], uv[2][1],
            rgb[0], rgb[1], rgb[2]);
    }

    std::fprintf(svg, "</svg>\n");
    std::fclose(svg);

    std::printf("Wrote %s: %d source faces repaired into %d charts using %d splits "
                "(%lld narrow phase tests).\n",
        argv[1], input.face_count, report->chart_count, report->split_count,
        report->narrow_phase_count);

    atlas_free(output);
    atlas_injective_report_free(report);
    return EXIT_SUCCESS;
}
