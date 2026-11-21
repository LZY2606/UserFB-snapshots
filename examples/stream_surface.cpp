/*
 * Example for out-of-core FSSR surface extraction.
 *
 * Builds a sphere with colored samples and two scale bands from scratch,
 * computes the voxels and writes the surface to a PLY file using the
 * streaming extraction entry point. The reported statistics illustrate the
 * bounded working set and the temporary I/O volume.
 *
 * Usage: stream_surface <output.ply> <temporary-directory> [budget-bytes]
 */

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "fssr/hermite.h"
#include "fssr/iso_octree.h"
#include "fssr/stream_surface.h"

int
main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0]
            << " <output.ply> <temporary-directory> [budget-bytes]\n";
        return 1;
    }

    std::string const output = argv[1];
    std::string const temp_dir = argv[2];
    std::size_t const budget = argc > 3
        ? static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10))
        : static_cast<std::size_t>(256 * 1024);

    /* Two scale bands on one sphere produce cross-level octree edges. */
    int const rings = 24;
    int const segments = 48;
    float const scale_fine = 0.08f;
    float const scale_coarse = 0.22f;

    fssr::IsoOctree tree;
    tree.set_max_level(7);
    for (int i = 1; i < rings; ++i)
    {
        double const theta = M_PI * i / rings;
        for (int j = 0; j < segments; ++j)
        {
            double const phi = 2.0 * M_PI * j / segments;
            math::Vec3f const normal(
                static_cast<float>(std::sin(theta) * std::cos(phi)),
                static_cast<float>(std::sin(theta) * std::sin(phi)),
                static_cast<float>(std::cos(theta)));

            fssr::Sample sample;
            sample.normal = normal;
            sample.pos = normal;
            sample.color = math::Vec3f(0.25f + 0.5f * (i % 2),
                0.35f + 0.4f * (j % 3) / 2.0f, 0.8f);
            sample.scale = j < segments / 2 ? scale_fine : scale_coarse;
            sample.confidence = 0.6f + 0.4f * ((i + j) % 2);
            tree.insert_sample(sample);
        }
    }
    tree.limit_octree_level();
    tree.compute_voxels();

    std::cout << "Extracting surface to " << output
        << " with a working budget of " << budget << " bytes..." << std::endl;

    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_CUBIC, output, temp_dir, budget);

    std::cout << "Wrote " << stats.vertex_count << " vertices and "
        << stats.triangle_count << " triangles." << std::endl;
    std::cout << "Peak working bytes: " << stats.peak_working_bytes
        << " (budget " << budget << ")" << std::endl;
    std::cout << "Triangulation workspace: "
        << stats.triangulation_workspace_bytes << " bytes for polygons of up to "
        << stats.max_polygon_vertices << " vertices" << std::endl;
    std::cout << "Temporary file I/O: read " << stats.temp_read_bytes
        << " bytes, wrote " << stats.temp_write_bytes << " bytes in "
        << stats.merge_passes << " merge passes." << std::endl;
    return 0;
}
