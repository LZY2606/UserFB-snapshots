/*
 * Copyright (C) 2015, Simon Fuhrmann
 * TU Darmstadt - Graphics, Capture and Massively Parallel Computing
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 *
 * Isosurface extraction for octrees. The algorithm is based on the paper:
 *
 *   Unconstrained Isosurface Extraction on Arbitrary Octrees
 *   Michael Kazhdan, Allison Klein, Ketan Dalal, Hugues Hoppe
 *   Symposium on Geometry Processing, 2007
 */

#ifndef FSSR_ISO_SURFACE_HEADER
#define FSSR_ISO_SURFACE_HEADER

#include <vector>
#include <map>
#include <cstdint>
#include <cstddef>
#include <string>

#include "math/algo.h"
#include "fssr/defines.h"
#include "fssr/hermite.h"
#include "fssr/iso_octree.h"
#include "fssr/memory.h"
#include "fssr/octree.h"
#include "fssr/voxel.h"

FSSR_NAMESPACE_BEGIN

struct StreamSurfaceStats;
class StreamSurfaceImpl;

/**
 * The surfacing algorithm requires the octree hierarchy and the vector
 * of voxels. The octree is modified in that every octree node gets the
 * Marching Cubes index assigned. Otherwise the octree is unchanged.
 */
class IsoSurface
{
public:
    IsoSurface (IsoOctree* octree,
        InterpolationType interpolation_type = INTERPOLATION_CUBIC);
    mve::TriangleMesh::Ptr extract_mesh (void);

    /**
     * Out-of-core extraction entry point. Extracts the isosurface into a
     * binary PLY file while keeping the working set bounded by the given
     * byte budget. See fssr/stream_surface.h for details.
     */
    StreamSurfaceStats extract_stream_mesh (std::string const& ply_filename,
        std::string const& temp_directory, std::size_t budget_bytes);

    /**
     * Returns the ordered list of edge keys that receive vertex IDs, in
     * the order in which the streaming extractor assigns them, together
     * with the interpolated position of each. Intended for verification.
     */
    void get_ordered_edge_vertices (
        std::vector<std::pair<uint64_t, uint64_t> >* keys,
        std::vector<math::Vec3f>* positions);

    /** Grants the streaming implementation access to the geometry helpers. */
    friend class StreamSurfaceImpl;

private:
    /** The isovertex contains interpolated position and voxel data. */
    struct IsoVertex
    {
        math::Vec3f pos;
        VoxelData data;
    };

    /** The edge index identifies an octree edge using two voxel indices. */
    typedef std::pair<uint64_t, uint64_t> EdgeIndex;

    /** Additional information for an edge. */
    struct EdgeInfo
    {
        Octree::Iterator iter;
        int edge_id;
    };

    /** An Ingredient describes a point on a cube edge used by polygon loops. */
    struct Ingredient
    {
        EdgeIndex edge;
        Octree::Iterator iter;
        int edge_id;
    };

    /** Callback receiving one closed polygon as an ordered ingredient list. */
    typedef void (*PolygonVisitor) (void* context,
        Ingredient const* vertices, std::size_t num_vertices);

    /** An IsoEdge is a connection between two cube edges. */
    struct IsoEdge
    {
        EdgeIndex first;
        EdgeIndex second;
        EdgeInfo first_info;
        EdgeInfo second_info;
    };

    /** Vector of IsoVertex elements. */
    typedef std::vector<IsoVertex> IsoVertexVector;
    /** Maps and edge to an isovertex ID. */
    typedef std::map<EdgeIndex, std::size_t> EdgeVertexMap;
    /** List of polygons, each indexing vertices. */
    typedef std::vector<std::vector<std::size_t> > PolygonList;
    /** List of iso edges connecting vertices on cube edges. */
    typedef std::vector<IsoEdge> IsoEdgeList;

private:
    void sanity_checks (void);
    void compute_mc_index (Octree::Iterator const& iter);
    void compute_mc_indices (void);
    void compute_isovertices (Octree::Iterator const& iter,
        EdgeVertexMap* edgemap, IsoVertexVector* isovertices);
    bool is_isovertex_on_edge (int mc_index, int edge_id);
    void get_finest_cube_edge (Octree::Iterator const& iter,
        int edge_id, EdgeIndex* edge_index, EdgeInfo* edge_info);
    void get_finest_isoedges (Octree::Iterator const& iter,
        int face_id, IsoEdgeList* isoedges, bool descend_only);
    void get_isovertex (EdgeIndex const& edge_index,
        int edge_id, IsoVertex* iso_vertex);
    void compute_isopolygons(Octree::Iterator const& iter,
        EdgeVertexMap const& edgemap, PolygonList* polygons);
    void visit_leaf_isopolygons (Octree::Iterator const& iter,
        PolygonVisitor visitor, void* context);
    void compute_triangulation(IsoVertexVector const& isovertices,
        PolygonList const& polygons, mve::TriangleMesh::Ptr mesh);
    VoxelData const* get_voxel_data (VoxelIndex const& index);
    std::size_t lookup_edge_vertex (EdgeVertexMap const& edgemap,
        EdgeIndex const& edge);
    void find_twin_vertex(EdgeInfo const& edge_info,
        EdgeIndex* twin, EdgeInfo* twin_info);

private:
    Octree* octree;
    IsoOctree::VoxelVector const* voxels;
    InterpolationType interpolation_type;
};

/* --------------------------------------------------------------------- */

inline
IsoSurface::IsoSurface (IsoOctree* octree, InterpolationType interpolation_type)
    : octree(octree)
    , voxels(&octree->get_voxels())
    , interpolation_type(interpolation_type)
{
}

inline VoxelData const*
IsoSurface::get_voxel_data (VoxelIndex const& index)
{
    return math::algo::binary_search(*this->voxels, index);
}

FSSR_NAMESPACE_END

#endif // FSSR_ISO_SURFACE_HEADER
