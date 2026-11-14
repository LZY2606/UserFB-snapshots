/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2016: Marco Livesu                                              *
*                                                                               *
*  The MIT License                                                              *
*                                                                               *
*  Permission is hereby granted, free of charge, to any person obtaining a      *
*  copy of this software and associated documentation files (the "Software"),   *
*  to deal in the Software without restriction, including without limitation    *
*  the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
*  and/or sell copies of the Software, and to permit persons to whom the        *
*  Software is furnished to do so, subject to the following conditions:         *
*                                                                               *
*  The above copyright notice and this permission notice shall be included in   *
*  all copies or substantial portions of the Software.                          *
*                                                                               *
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR   *
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,     *
*  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE *
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER       *
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      *
*  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS *
*  IN THE SOFTWARE.                                                             *
*                                                                               *
*  Author(s):                                                                   *
*                                                                               *
*     Marco Livesu (marco.livesu@gmail.com)                                     *
*     http://pers.ge.imati.cnr.it/livesu/                                       *
*                                                                               *
*     Italian National Research Council (CNR)                                   *
*     Institute for Applied Mathematics and Information Technologies (IMATI)    *
*     Via de Marini, 6                                                          *
*     16149 Genoa,                                                              *
*     Italy                                                                     *
*********************************************************************************/
#ifndef CINO_HEXMESH_H
#define CINO_HEXMESH_H

#include <sys/types.h>
#include <vector>
#include <cinolib/geometry/vec_mat.h>
#include <cinolib/meshes/quadmesh.h>
#include <cinolib/meshes/mesh_attributes.h>
#include <cinolib/meshes/abstract_polyhedralmesh.h>

#include <unordered_set>
#include <utility>

namespace cinolib
{

// Result of Hexmesh::subdivide_sheets. All ids refer to the mesh as it was
// right before the call. parent_poly has one entry per output hexahedron,
// vertex_sources one entry per output vertex. Each vertex source is a pair
// (input vertex id, non negative interpolation weight); the weights of each
// list sum to one. Input vertices map to a single source with weight 1, edge
// nodes to two sources of weight 1/2, face nodes to four of weight 1/4 and
// cell center nodes to eight of weight 1/8.
struct HexSheetSubdivision
{
    std::vector<uint>                                   parent_poly;
    std::vector<std::vector<std::pair<uint,double>>>    vertex_sources;
};

template<class M = Mesh_std_attributes, // default template arguments
         class V = Vert_std_attributes,
         class E = Edge_std_attributes,
         class F = Polygon_std_attributes,
         class P = Polyhedron_std_attributes>
class Hexmesh : public AbstractPolyhedralMesh<M,V,E,F,P>
{
    public:

        explicit Hexmesh(){}

        explicit Hexmesh(const char * filename);

        explicit Hexmesh(const std::vector<double> & coords,
                         const std::vector<uint>   & polys);

        explicit Hexmesh(const std::vector<vec3d> & verts,
                         const std::vector<uint>  & polys);

        explicit Hexmesh(const std::vector<vec3d>             & verts,
                         const std::vector<std::vector<uint>> & polys);

        ~Hexmesh(){}

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        MeshType mesh_type() const override { return HEXMESH; }

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        void load(const char * filename) override;
        void save(const char * filename) const override;

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        void update_f_normal(const uint fid) override;
        void print_quality(const bool list_folded_elements = false);

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        uint verts_per_poly(const uint) const override { return  8; }
        uint verts_per_poly()           const          { return  8; }
        uint edges_per_poly(const uint) const override { return 12; }
        uint edges_per_poly()           const          { return 12; }
        uint faces_per_poly(const uint) const override { return  6; }
        uint faces_per_poly()           const          { return  6; }
        uint verts_per_face(const uint) const override { return  4; }
        uint verts_per_face()           const          { return  4; }

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        bool  vert_is_singular(const uint vid) const;
        bool  vert_is_regular (const uint vid) const;
        vec3d verts_average   (const std::vector<uint> & vids) const;

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        bool  edge_is_singular(const uint eid) const;
        bool  edge_is_regular (const uint eid) const;

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        std::vector<uint>        face_sheet(const uint fid) const; // stop at singular edges

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        uint                     poly_face_opposite_to(const uint pid, const uint fid) const;
        uint                     poly_vert_opposite_to(const uint pid, const uint fid, const uint vid) const;
        void                     poly_subdivide       (const std::vector<std::vector<std::vector<uint>>> & split_scheme);

        //::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        // Topological sheet subdivision. Each marked edge seeds a sheet: the
        // selection propagates across every hexahedron from an edge to the
        // locally opposite edge on each of the two incident quads, until the
        // closure is reached. Propagation is purely topological: world-space
        // parallelism is never used, rotated local numberings and curved or
        // self-returning sheets are handled. A hexahedron hit by 1, 2 or 3
        // sheet directions is split into 2, 4 or 8 sub hexahedra respectively;
        // untouched hexahedra are kept as is. Repeated edge ids count once, an
        // empty selection leaves the mesh unchanged, and an out-of-range id
        // throws std::invalid_argument. New nodes are trilinear midpoint nodes
        // (edge midpoints, quad face centers, cell centers), shared by every
        // incident cell and face through topology, never coordinate welding.
        // The returned HexSheetSubdivision uses the numbering in effect at the
        // time of the call; a subsequent call takes the current mesh as input.
        HexSheetSubdivision      subdivide_sheets     (const std::vector<uint> & marked_edges);
        double                   poly_volume          (const uint pid) const override;
        bool                     poly_fix_orientation ();
        void                     poly_local_frame     (const uint pid, vec3d & x, vec3d & y, vec3d & z);
        void                     poly_local_frame     (const uint pid, mat3d & xyz);
        std::unordered_set<uint> poly_sheet           (const uint eid) const;
};

}

#ifndef  CINO_STATIC_LIB
#include "hexmesh.cpp"
#endif

#endif // CINO_HEXMESH_H
