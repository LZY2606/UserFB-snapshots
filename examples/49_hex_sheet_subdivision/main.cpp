// Sheet-wise subdivision of a hexahedral mesh.
//
// A global midpoint refinement cuts every cell in all three parametric
// directions. Hexmesh::subdivide_sheets instead refines only the topological
// sheets that pass through a user selection of edges: from each marked edge
// the selection crosses every incident quad through its topologically
// opposite edge, until the closure is reached. No world-space parallelism is
// ever tested, so curved meshes, rotated local vertex numberings and sheets
// that return to a cell from another local direction are all handled.
//
// A cell hit by one, two or three sheet directions becomes respectively two,
// four or eight sub hexahedra; cells not hit by any sheet are kept untouched
// (a whole connected component is never uniformly octasplit).
//
// This sample is headless: it builds a small two-cell hex mesh, refines one
// sheet in place, inspects the returned parent/source mapping and prints the
// validity of the result. Compile it like every other cinolib example.

#include <cinolib/meshes/hexmesh.h>
#include <cinolib/hex_sheet_subdivision.h>
#include <cinolib/subdivision_midpoint.h>

#include <iostream>
#include <stdexcept>

using namespace cinolib;

int main()
{
    // two axis aligned unit hexahedra sharing a quad
    std::vector<vec3d> verts;
    for(int z=0; z<2; ++z)
    for(int y=0; y<2; ++y)
    for(int x=0; x<3; ++x) verts.emplace_back(x,y,z);

    auto vid = [](const int x, const int y, const int z)
    {
        return uint(x + 3*y + 6*z);
    };
    std::vector<std::vector<uint>> polys =
    {
        {vid(0,0,0),vid(1,0,0),vid(1,1,0),vid(0,1,0),
         vid(0,0,1),vid(1,0,1),vid(1,1,1),vid(0,1,1)},
        {vid(1,0,0),vid(2,0,0),vid(2,1,0),vid(1,1,0),
         vid(1,0,1),vid(2,0,1),vid(2,1,1),vid(1,1,1)}
    };
    Hexmesh<> m(verts, polys);

    // select a y-directed edge of the first cell (between vertices 0 and 3).
    // The corresponding sheet crosses both cells, so both will be split once.
    const int seed = m.edge_id(0u, 3u);
    if(seed < 0) return 1;

    // ------------------------------------------------------------------
    // 1) sheet subdivision, applied directly to the mesh
    // ------------------------------------------------------------------
    HexSheetSubdivision info = m.subdivide_sheets({uint(seed)});

    std::cout << "cells after one sheet cut: " << m.num_polys() << "\n";
    std::cout << "parent of each output cell:";
    for(const uint parent : info.parent_poly) std::cout << " " << parent;
    std::cout << "\n";

    // every output vertex is expressed from the pre-call numbering. Input
    // vertices have a single unit-weight source; midpoint nodes carry two,
    // four or eight non-negative weights that sum to one.
    for(uint v=0; v<m.num_verts(); ++v)
    {
        const auto & sources = info.vertex_sources.at(v);
        double sum = 0.0;
        for(auto [input_vid, w] : sources)
        {
            if(w < 0.0 || input_vid >= info.vertex_sources.size()) return 2;
            sum += w;
        }
        if(std::abs(sum - 1.0) > 1e-12) return 3;
    }

    // ------------------------------------------------------------------
    // 2) validity scope guaranteed by the caller's assumptions
    // ------------------------------------------------------------------
    // The input is assumed to be a finite, conforming, non degenerate hex
    // mesh whose per-cell trilinear maps have a positive Jacobian over the
    // whole parameter cube. Under that assumption the output stays valid:
    // every child has positive volume, interior faces have opposite winding
    // on their two incident cells and there are no hanging nodes.
    for(uint pid=0; pid<m.num_polys(); ++pid)
    {
        if(m.poly_volume(pid) <= 0.0) return 4;
    }
    for(uint fid=0; fid<m.num_faces(); ++fid)
    {
        const auto & incident = m.adj_f2p(fid);
        if(incident.size() == 2 &&
           m.poly_face_is_CCW(incident[0],fid) == m.poly_face_is_CCW(incident[1],fid))
        {
            return 5; // interior face with identical winding on both sides
        }
    }
    std::cout << "all child cells positive, all interior faces conforming\n";

    // ------------------------------------------------------------------
    // 3) midpoint entry points (uniform three-direction refinement)
    // ------------------------------------------------------------------
    Hexmesh<> uniform;
    subdivision_midpoint(m, uniform);                 // no-map call
    std::cout << "uniform midpoint cells: " << uniform.num_polys() << "\n";

    Hexmesh<> uniform_mapped;
    std::map<uint,uint> edge_verts, face_verts, poly_verts;
    subdivision_midpoint(m, uniform_mapped,          // mapped call
                         edge_verts, face_verts, poly_verts);
    std::cout << "created nodes: "
              << edge_verts.size() << " edge midpoints, "
              << face_verts.size() << " face centers, "
              << poly_verts.size() << " cell centers\n";

    // edge cases: an empty selection leaves the mesh untouched, repeated or
    // reordered ids give the same result, out-of-range ids throw
    const uint verts_before = m.num_verts();
    m.subdivide_sheets({});
    if(m.num_verts() != verts_before) return 6;
    bool threw = false;
    try { m.subdivide_sheets({uint(-1)}); }
    catch(const std::invalid_argument &) { threw = true; }
    if(!threw) return 7;

    std::cout << "HEX SHEET SUBDIVISION EXAMPLE OK\n";
    return 0;
}
