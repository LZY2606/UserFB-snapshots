// Implementation of the reusable mixed-polygon subset extraction.
//
// This software is in the public domain.

#include "polygon_subset.h"

#include <cstring>
#include <unordered_map>

namespace tinyply_extras
{

using namespace tinyply;

PolygonSubset extract_polygon_subset(const PolygonMeshView & mesh, const std::vector<size_t> & keepFaces)
{
    // ---- Validate the face selection before touching any output buffer. ----

    std::vector<bool> faceSeen(mesh.faceCount, false);
    for (size_t f : keepFaces)
    {
        if (f >= mesh.faceCount)
            throw std::invalid_argument("keepFaces contains an out-of-range face id");
        if (faceSeen[f])
            throw std::invalid_argument("keepFaces contains a duplicate face id");
        faceSeen[f] = true;
    }

    if (mesh.faceCount > 0 && mesh.faceOffsets == nullptr)
        throw std::invalid_argument("faceOffsets is null for a non-empty mesh");

    const size_t totalCorners = mesh.faceCount > 0 ? mesh.faceOffsets[mesh.faceCount] : 0;
    if (totalCorners > 0 && mesh.faceIndices == nullptr)
        throw std::invalid_argument("faceIndices is null while the mesh has corners");

    if (mesh.faceCount > 0 && mesh.faceOffsets[0] != 0)
        throw std::invalid_argument("faceOffsets must start at zero");

    for (size_t f = 0; f < mesh.faceCount; ++f)
    {
        if (mesh.faceOffsets[f + 1] < mesh.faceOffsets[f])
            throw std::invalid_argument("faceOffsets must be monotonically non-decreasing");
    }

    // Every corner must reference a known vertex.
    for (size_t c = 0; c < totalCorners; ++c)
    {
        if (mesh.faceIndices[c] >= mesh.vertexCount)
            throw std::invalid_argument("a face references an out-of-range vertex id");
    }

    // Texcoord ranges, when present, must hold exactly two scalars per corner.
    if (mesh.hasTexcoords)
    {
        if (mesh.texcoordOffsets == nullptr)
            throw std::invalid_argument("texcoordOffsets is null while texcoords are enabled");
        if (mesh.texcoordOffsets[0] != 0)
            throw std::invalid_argument("texcoordOffsets must start at zero");

        for (size_t f = 0; f < mesh.faceCount; ++f)
        {
            if (mesh.texcoordOffsets[f + 1] < mesh.texcoordOffsets[f])
                throw std::invalid_argument("texcoordOffsets must be monotonically non-decreasing");
            const size_t corners = mesh.faceOffsets[f + 1] - mesh.faceOffsets[f];
            const size_t uvScalars = mesh.texcoordOffsets[f + 1] - mesh.texcoordOffsets[f];
            if (uvScalars != corners * 2)
                throw std::invalid_argument("texcoord scalar count must be twice the corner count of each face");
        }

        const size_t totalUv = mesh.texcoordOffsets[mesh.faceCount];
        if (totalUv > 0 && mesh.texcoords == nullptr)
            throw std::invalid_argument("texcoords is null while the texcoord payload is non-empty");
    }

    if (mesh.hasMaterials && mesh.faceCount > 0 && mesh.faceMaterials == nullptr)
        throw std::invalid_argument("faceMaterials is null while materials are enabled");

    for (const auto & channel : mesh.vertexProperties)
    {
        if (channel.type == Type::INVALID || PropertyTable[channel.type].stride == 0)
            throw std::invalid_argument("vertex property '" + channel.name + "' has an invalid type");
        if (mesh.vertexCount > 0 && channel.data == nullptr)
            throw std::invalid_argument("vertex property '" + channel.name + "' has a null data pointer");
    }

    // ---- All inputs are valid: build the compacted subset. ----

    PolygonSubset subset;

    std::unordered_map<uint32_t, uint32_t> oldToNew;
    oldToNew.reserve(mesh.vertexCount);

    subset.faceOffsets.push_back(0);
    if (mesh.hasTexcoords) subset.texcoordOffsets.push_back(0);

    for (size_t f : keepFaces)
    {
        const size_t begin = mesh.faceOffsets[f];
        const size_t end = mesh.faceOffsets[f + 1];

        for (size_t c = begin; c < end; ++c)
        {
            const uint32_t oldId = mesh.faceIndices[c];
            auto found = oldToNew.find(oldId);
            if (found == oldToNew.end())
            {
                const uint32_t newId = static_cast<uint32_t>(subset.newToOldVertices.size());
                oldToNew.emplace(oldId, newId);
                subset.newToOldVertices.push_back(oldId);
            }
            subset.faceIndices.push_back(oldToNew[oldId]);
        }
        subset.faceOffsets.push_back(subset.faceIndices.size());

        if (mesh.hasTexcoords)
        {
            const size_t uvBegin = mesh.texcoordOffsets[f];
            const size_t uvEnd = mesh.texcoordOffsets[f + 1];
            subset.texcoords.insert(subset.texcoords.end(),
                mesh.texcoords + uvBegin, mesh.texcoords + uvEnd);
            subset.texcoordOffsets.push_back(subset.texcoords.size());
        }

        if (mesh.hasMaterials) subset.faceMaterials.push_back(mesh.faceMaterials[f]);
    }

    subset.vertexCount = subset.newToOldVertices.size();
    subset.hasTexcoords = mesh.hasTexcoords;
    subset.hasMaterials = mesh.hasMaterials;

    // Reorder every numeric vertex channel along the new vertex numbering.
    for (const auto & channel : mesh.vertexProperties)
    {
        const size_t stride = static_cast<size_t>(PropertyTable[channel.type].stride);
        PolygonSubset::VertexChannel out;
        out.name = channel.name;
        out.type = channel.type;
        out.data.resize(subset.vertexCount * stride);
        for (size_t v = 0; v < subset.vertexCount; ++v)
        {
            const size_t oldId = subset.newToOldVertices[v];
            std::memcpy(out.data.data() + v * stride, channel.data + oldId * stride, stride);
        }
        subset.vertexChannels.push_back(std::move(out));
    }

    return subset;
}

void add_polygon_subset_to_ply(PlyFile & file, const PolygonSubset & subset)
{
    // Fixed-length vertex channels still use the borrowing interface; the
    // subset object owns these buffers and must outlive the write call.
    for (const auto & channel : subset.vertexChannels)
    {
        file.add_properties_to_element("vertex", { channel.name }, channel.type,
            subset.vertexCount, channel.data.data(), Type::INVALID, 0);
    }

    const size_t faceCount = subset.faceOffsets.size() > 0 ? subset.faceOffsets.size() - 1 : 0;

    // Ragged lists go through the owning, offset-based interface. The library
    // copies both payloads and offsets, so nothing depends on the original
    // file object these faces were decoded from.
    file.add_list_property_to_element("face", "vertex_indices", Type::UINT32, faceCount,
        subset.faceOffsets.data(),
        reinterpret_cast<const uint8_t *>(subset.faceIndices.data()),
        subset.faceIndices.size() * sizeof(uint32_t), Type::UINT8);

    if (subset.hasTexcoords)
    {
        file.add_list_property_to_element("face", "texcoord", Type::FLOAT32, faceCount,
            subset.texcoordOffsets.data(),
            reinterpret_cast<const uint8_t *>(subset.texcoords.data()),
            subset.texcoords.size() * sizeof(float), Type::UINT8);
    }

    if (subset.hasMaterials)
    {
        file.add_properties_to_element("face", { "material_index" }, Type::INT32, faceCount,
            reinterpret_cast<const uint8_t *>(subset.faceMaterials.data()), Type::INVALID, 0);
    }
}

} // end namespace tinyply_extras
