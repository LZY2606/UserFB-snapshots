// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Texture-preserving remeshing example. A small triangle grid with a
// halfedge texture coordinate property ("h:tex") and a texture seam is
// remeshed without losing the per-corner UV mapping.

#include <pmp/algorithms/remeshing.h>
#include <pmp/surface_mesh.h>

#include <cmath>
#include <iostream>

using namespace pmp;

int main()
{
    // build an asymmetric 8x6 triangle grid
    const int nx = 8, ny = 6;
    SurfaceMesh mesh;
    std::vector<Vertex> v(nx * ny);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
        {
            const Scalar x = Scalar(0.4) * i;
            const Scalar y = Scalar(0.4) * j;
            const Scalar z = Scalar(0.02) * std::sin(x) * std::cos(y);
            v[j * nx + i] = mesh.add_vertex(Point(x, y, z));
        }
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i)
        {
            mesh.add_triangle(v[j * nx + i], v[j * nx + i + 1],
                              v[(j + 1) * nx + i + 1]);
            mesh.add_triangle(v[j * nx + i], v[(j + 1) * nx + i + 1],
                              v[(j + 1) * nx + i]);
        }

    // assign per-halfedge texture coordinates. The two sides of the middle
    // grid column use unrelated UV mappings, producing a texture seam; the
    // values deliberately live outside [0, 1].
    auto tex = mesh.halfedge_property<TexCoord>("h:tex");
    for (auto f : mesh.faces())
    {
        Scalar cx = 0;
        for (auto vv : mesh.vertices(f))
            cx += mesh.position(vv)[0];
        cx /= Scalar(3.0);
        const bool left = cx < Scalar(0.4) * (nx / 2);
        for (auto h : mesh.halfedges(f))
        {
            const Point& p = mesh.position(mesh.to_vertex(h));
            tex[h] = left
                         ? TexCoord(Scalar(7.3) * p[0] + Scalar(3.1),
                                    Scalar(-3.1) * p[1] - Scalar(2.7))
                         : TexCoord(Scalar(-5.9) * p[0] + Scalar(17.3),
                                    Scalar(4.7) * p[1] + Scalar(11.9));
        }
    }

    const size_t faces_before = mesh.n_faces();

    //! [texture-remeshing]
    // The last argument enables texture preservation. It only takes effect
    // because the mesh carries the halfedge property "h:tex".
    uniform_remeshing(mesh, /*edge_length=*/Scalar(0.18),
                      /*iterations=*/6,
                      /*use_projection=*/true,
                      /*preserve_texture=*/true);

    // adaptive remeshing offers the same final parameter:
    // adaptive_remeshing(mesh, 0.05, 0.4, 0.01, 6, true, true);
    //! [texture-remeshing]

    std::cout << "faces " << faces_before << " -> " << mesh.n_faces() << "\n";

    // every output corner still carries a UV transferred from the original
    // charts; seam vertices hold two distinct corner UVs
    size_t seam_vertices = 0;
    for (auto vv : mesh.vertices())
    {
        std::vector<TexCoord> seen;
        for (auto h : mesh.halfedges(vv))
        {
            const Halfedge corner = mesh.opposite_halfedge(h);
            if (!mesh.face(corner).is_valid())
                continue;
            const TexCoord t = tex[corner];
            bool duplicate = false;
            for (const auto& s : seen)
                if (norm(s - t) <= Scalar(1e-6))
                    duplicate = true;
            if (!duplicate)
                seen.push_back(t);
        }
        if (seen.size() > 1)
            ++seam_vertices;
    }
    std::cout << "vertices carrying multiple corner UVs: " << seam_vertices
              << "\n";

    return 0;
}
