// Reusable mixed-polygon subset extraction for tinyply.
//
// Given a decoded polygon mesh (variable-length face index lists, per-corner
// texcoords and per-face materials) and an ordered sequence of face ids to
// keep, extract_polygon_subset renumbers the referenced vertices by first
// appearance, preserves the original corner order of every kept face, and
// produces buffers that can be fed straight back into a tinyply::PlyFile.
//
// This software is in the public domain.

#pragma once

#ifndef tinyply_polygon_subset_h
#define tinyply_polygon_subset_h

#include "tinyply.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tinyply_extras
{

// One decoded scalar vertex property (e.g. "x", "y", "nx"), stored as
// vertexCount tightly-packed scalars of the given type.
struct VertexPropertyData
{
    std::string name;
    tinyply::Type type{ tinyply::Type::INVALID };
    const uint8_t * data{ nullptr }; // vertexCount * PropertyTable[type].stride bytes
};

// A non-owning view of a decoded polygon mesh.
struct PolygonMeshView
{
    size_t vertexCount{ 0 };
    std::vector<VertexPropertyData> vertexProperties;

    size_t faceCount{ 0 };
    const uint32_t * faceIndices{ nullptr };   // faceOffsets[faceCount] entries
    const size_t * faceOffsets{ nullptr };     // faceCount + 1 entries, scalar units

    // Per-corner texture coordinates. When hasTexcoords is true both pointers
    // must be non-null and the texcoord range of every face holds exactly
    // twice as many scalars as the face has corners.
    bool hasTexcoords{ false };
    const float * texcoords{ nullptr };        // texcoordOffsets[faceCount] floats
    const size_t * texcoordOffsets{ nullptr }; // faceCount + 1 entries, scalar units

    // One material index per face; optional.
    bool hasMaterials{ false };
    const int32_t * faceMaterials{ nullptr };  // faceCount entries
};

// Owned output of a subset extraction: every buffer is a compacted copy, so
// the input view may be destroyed before the result is written.
struct PolygonSubset
{
    size_t vertexCount{ 0 };
    std::vector<size_t> newToOldVertices; // new vertex id -> original vertex id

    struct VertexChannel
    {
        std::string name;
        tinyply::Type type{ tinyply::Type::INVALID };
        std::vector<uint8_t> data; // vertexCount scalars, tightly packed
    };
    std::vector<VertexChannel> vertexChannels;

    std::vector<uint32_t> faceIndices;
    std::vector<size_t> faceOffsets;       // (kept face count) + 1 entries

    bool hasTexcoords{ false };
    std::vector<float> texcoords;
    std::vector<size_t> texcoordOffsets;   // (kept face count) + 1 entries

    bool hasMaterials{ false };
    std::vector<int32_t> faceMaterials;
};

// Extracts the faces listed in keepFaces, in the given order. Vertices are
// renumbered by first appearance among the kept corners; unreferenced
// vertices are discarded and the new-to-old mapping is returned in the
// result. Throws std::invalid_argument on any malformed input (duplicate or
// out-of-range face ids, out-of-range vertex references, texcoord counts
// that are not twice the corner counts) without producing a partial result.
PolygonSubset extract_polygon_subset(const PolygonMeshView & mesh,
    const std::vector<size_t> & keepFaces);

// Writes a subset into a PlyFile: vertex channels and per-face material use
// the fixed-length interface, while the ragged face indices and per-corner
// texcoords go through PlyFile::add_list_property_to_element. The element
// names are "vertex" and "face".
void add_polygon_subset_to_ply(tinyply::PlyFile & file, const PolygonSubset & subset);

} // end namespace tinyply_extras

#endif // end tinyply_polygon_subset_h
