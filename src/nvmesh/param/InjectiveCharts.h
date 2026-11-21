// Injective chart generation: detect global UV overlaps and repair them by
// recursively cutting charts along dual-graph tree edges.
//
// This code is in the public domain.

#pragma once
#ifndef NV_MESH_INJECTIVECHARTS_H
#define NV_MESH_INJECTIVECHARTS_H

#include "nvcore/Array.h"
#include "nvcore/nvcore.h"
#include "nvmesh/nvmesh.h"


namespace nv
{
    class Atlas;

    namespace HalfEdge { class Mesh; }

    enum InjectiveStatus
    {
        InjectiveStatus_Ok,
        InjectiveStatus_Invalid_Mesh,
        InjectiveStatus_Budget_Exceeded,
        InjectiveStatus_Uninjectable,
    };

    struct InjectiveOptions
    {
        InjectiveOptions() :
            packing_quality(1),
            texels_per_unit(8.0f),
            block_align(true)
        {
        }

        int packing_quality;
        float texels_per_unit;
        bool block_align;
    };

    // One recursive cut. Chart identifiers are stable node ids assigned in
    // creation order (initial extracted charts first, descendants afterwards).
    struct InjectiveSplitRecord
    {
        InjectiveSplitRecord() :
            parent_node(0),
            child_node(),
            witness_face(),
            cut_edge()
        {
            child_node[0] = child_node[1] = 0;
            witness_face[0] = witness_face[1] = 0;
            cut_edge[0] = cut_edge[1] = 0;
        }

        uint parent_node;
        uint child_node[2];

        // Lexicographically smallest overlapping source face pair, or the
        // single local-failure witness face replicated in both entries.
        uint witness_face[2];

        // Dual-graph tree edge (source faces at both ends) that was cut.
        uint cut_edge[2];

        Array<uint> parent_faces;
        Array<uint> child_faces[2];
    };

    struct InjectiveReport
    {
        InjectiveReport() :
            narrow_phase_count(0)
        {
        }

        // Final chart index (0 .. chart_count-1) for every source face.
        Array<uint> face_chart;

        // Source faces of every final chart, indexed by final chart index.
        Array< Array<uint> > chart_faces;

        // Stable node id of the parent of every final chart (~0 means the
        // chart was produced by the initial extraction and was never cut).
        Array<uint> chart_parent_node;

        // Source faces of every chart node, indexed by stable node id.
        Array< Array<uint> > node_faces;

        // Parent node of every chart node (~0 for initial nodes).
        Array<uint> node_parent;

        Array<InjectiveSplitRecord> splits;

        // Number of exact triangle/triangle narrow-phase intersection tests.
        uint64 narrow_phase_count;
    };

    // Run extraction, injective validation, recursive re-cutting and finally
    // conservative packing. On success a newly allocated Atlas is returned in
    // atlas_out (caller owns it) and report is filled. On failure no atlas is
    // returned, the report content is undefined and the input mesh is left
    // unmodified.
    NVMESH_API InjectiveStatus generateInjectiveAtlas(
        HalfEdge::Mesh * mesh,
        uint max_charts,
        const InjectiveOptions & options,
        Atlas *& atlas_out,
        InjectiveReport & report);

} // nv namespace

#endif // NV_MESH_INJECTIVECHARTS_H
