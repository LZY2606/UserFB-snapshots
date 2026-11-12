// Regression tests for PlyFile::add_list_property_to_element and the
// polygon subset editing chain.
//
// The outputs are checked two ways: a tinyply round-trip and an independent
// PLY decoder (hand-rolled little-endian byte reads for binary output,
// hand-rolled token parsing for ascii output) so that a same-library
// round-trip cannot mask an encoding bug.
//
// This software is in the public domain.

#include "tinyply.h"
#include "polygon_subset.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace tinyply;

namespace
{

int g_checks = 0;

#define REQUIRE(cond)                                                                                          \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        if (!(cond)) { std::cerr << "REQUIRE failed: " #cond " at " << __FILE__ << ":" << __LINE__ << "\n";    \
                       std::abort(); }                                                                         \
    } while (0)

#define REQUIRE_THROWS_INVALID(expr)                                                                           \
    do {                                                                                                       \
        ++g_checks; bool threw = false;                                                                       \
        try { (void)(expr); } catch (const std::invalid_argument &) { threw = true; }                         \
        if (!threw) { std::cerr << "expected std::invalid_argument: " #expr " at " << __FILE__ << ":"         \
                                << __LINE__ << "\n"; std::abort(); }                                          \
    } while (0)

// -----------------------------------------------------------------------
// Independent, tinyply-free PLY decoder used only by these tests.
// -----------------------------------------------------------------------

enum class DecType : uint8_t { I8, U8, I16, U16, I32, U32, F32, F64, INVALID };

DecType decode_type(const std::string & s)
{
    if (s == "char" || s == "int8") return DecType::I8;
    if (s == "uchar" || s == "uint8") return DecType::U8;
    if (s == "short" || s == "int16") return DecType::I16;
    if (s == "ushort" || s == "uint16") return DecType::U16;
    if (s == "int" || s == "int32") return DecType::I32;
    if (s == "uint" || s == "uint32") return DecType::U32;
    if (s == "float" || s == "float32") return DecType::F32;
    if (s == "double" || s == "float64") return DecType::F64;
    return DecType::INVALID;
}

struct DecProperty
{
    std::string name;
    bool isList{ false };
    DecType listType{ DecType::INVALID };
    DecType valueType{ DecType::INVALID };
    std::vector<double> scalars;                       // one value per row
    std::vector<std::vector<double>> listRows;         // one row of values per element row
};

struct DecElement
{
    std::string name;
    size_t count{ 0 };
    std::vector<DecProperty> properties;
};

struct DecFile
{
    bool binary{ false };
    std::string header;
    std::vector<DecElement> elements;
};

double read_le_value(const std::string & buf, size_t & pos, DecType t)
{
    const auto u8 = [&](size_t k) -> uint8_t { return static_cast<uint8_t>(buf[pos + k]); };
    double out = 0.0;
    switch (t)
    {
    case DecType::I8: { int8_t v = static_cast<int8_t>(u8(0)); pos += 1; out = v; break; }
    case DecType::U8: { uint8_t v = u8(0); pos += 1; out = v; break; }
    case DecType::I16: { int16_t v = static_cast<int16_t>(uint16_t(u8(0)) | (uint16_t(u8(1)) << 8)); pos += 2; out = v; break; }
    case DecType::U16: { uint16_t v = static_cast<uint16_t>(uint16_t(u8(0)) | (uint16_t(u8(1)) << 8)); pos += 2; out = v; break; }
    case DecType::I32: { int32_t v = int32_t(uint32_t(u8(0)) | (uint32_t(u8(1)) << 8) |
                                              (uint32_t(u8(2)) << 16) | (uint32_t(u8(3)) << 24)); pos += 4; out = v; break; }
    case DecType::U32: { uint32_t v = uint32_t(u8(0)) | (uint32_t(u8(1)) << 8) |
                                       (uint32_t(u8(2)) << 16) | (uint32_t(u8(3)) << 24); pos += 4; out = static_cast<double>(v); break; }
    case DecType::F32: { uint32_t bits = uint32_t(u8(0)) | (uint32_t(u8(1)) << 8) |
                                        (uint32_t(u8(2)) << 16) | (uint32_t(u8(3)) << 24);
                         float f; std::memcpy(&f, &bits, 4); pos += 4; out = f; break; }
    case DecType::F64: { uint64_t bits = 0; for (size_t k = 0; k < 8; ++k) bits |= uint64_t(u8(k)) << (8 * k);
                         double d; std::memcpy(&d, &bits, 8); pos += 8; out = d; break; }
    default: throw std::runtime_error("independent decoder: unsupported type");
    }
    return out;
}

DecFile decode_ply(const std::string & content)
{
    DecFile out;

    size_t payloadStart = 0;
    {
        std::istringstream headerStream(content);
        std::string line;
        while (std::getline(headerStream, line))
        {
            payloadStart += line.size() + 1;
            std::istringstream ls(line);
            std::string token;
            ls >> token;
            if (token == "format")
            {
                std::string fmt; ls >> fmt;
                out.binary = (fmt == "binary_little_endian");
            }
            else if (token == "element")
            {
                DecElement e; ls >> e.name >> e.count;
                out.elements.push_back(e);
            }
            else if (token == "property")
            {
                REQUIRE(!out.elements.empty());
                DecProperty p;
                std::string a; ls >> a;
                if (a == "list")
                {
                    std::string lt, vt; ls >> lt >> vt >> p.name;
                    p.isList = true;
                    p.listType = decode_type(lt);
                    p.valueType = decode_type(vt);
                }
                else
                {
                    p.valueType = decode_type(a);
                    ls >> p.name;
                }
                REQUIRE(p.valueType != DecType::INVALID);
                if (p.isList) REQUIRE(p.listType != DecType::INVALID);
                out.elements.back().properties.push_back(p);
            }
            else if (token == "end_header") break;
        }
        out.header = content.substr(0, payloadStart);
    }

    if (out.binary)
    {
        size_t pos = payloadStart;
        for (auto & e : out.elements)
        {
            for (auto & p : e.properties)
            {
                if (p.isList) p.listRows.resize(e.count);
                else p.scalars.reserve(e.count);
            }
            for (size_t row = 0; row < e.count; ++row)
            {
                for (auto & p : e.properties)
                {
                    if (p.isList)
                    {
                        size_t n = static_cast<size_t>(read_le_value(content, pos, p.listType));
                        p.listRows[row].resize(n);
                        for (size_t k = 0; k < n; ++k)
                            p.listRows[row][k] = read_le_value(content, pos, p.valueType);
                    }
                    else
                    {
                        p.scalars.push_back(read_le_value(content, pos, p.valueType));
                    }
                }
            }
        }
        REQUIRE(pos == content.size());
    }
    else
    {
        std::istringstream payload(content.substr(payloadStart));
        for (auto & e : out.elements)
        {
            for (auto & p : e.properties)
            {
                if (p.isList) p.listRows.resize(e.count);
                else p.scalars.reserve(e.count);
            }
            for (size_t row = 0; row < e.count; ++row)
            {
                std::string line;
                std::getline(payload, line);
                std::istringstream ls(line);
                for (auto & p : e.properties)
                {
                    if (p.isList)
                    {
                        size_t n; ls >> n;
                        p.listRows[row].resize(n);
                        for (size_t k = 0; k < n; ++k) ls >> p.listRows[row][k];
                        REQUIRE(!ls.fail());
                    }
                    else
                    {
                        double v; ls >> v;
                        REQUIRE(!ls.fail());
                        p.scalars.push_back(v);
                    }
                }
            }
        }
    }

    return out;
}

const DecElement & find_dec_element(const DecFile & f, const std::string & name)
{
    for (const auto & e : f.elements) if (e.name == name) return e;
    throw std::runtime_error("independent decoder: element not found: " + name);
}

const DecProperty & find_dec_property(const DecElement & e, const std::string & name)
{
    for (const auto & p : e.properties) if (p.name == name) return p;
    throw std::runtime_error("independent decoder: property not found: " + name);
}

bool nearly_equal(double a, double b)
{
    return std::fabs(a - b) <= 1e-6 * (1.0 + std::fabs(a) + std::fabs(b));
}

// -----------------------------------------------------------------------
// Shared test mesh: five vertices, one triangle and one quad, distinct
// per-face materials and per-corner UVs. Everything is generated here.
// -----------------------------------------------------------------------

struct TestMesh
{
    // One scalar per vertex for each numeric channel: this is the granularity
    // the subset helper remaps (one PLY property == one scalar per vertex).
    std::vector<float> xs, ys, zs;
    std::vector<float> nxs, nys, nzs;
    std::vector<uint32_t> faceIndices;
    std::vector<size_t> faceOffsets;
    std::vector<float> texcoords;
    std::vector<size_t> texcoordOffsets;
    std::vector<int32_t> materials;

    static constexpr size_t vertexCount = 5;
    static constexpr size_t faceCount = 2;
};

TestMesh make_test_mesh()
{
    TestMesh m;
    const float pos[5][3] = {
        { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f },
        { 0.f, 1.f, 0.f }, { 2.f, 0.5f, 0.f }
    };
    for (size_t v = 0; v < 5; ++v)
    {
        m.xs.push_back(pos[v][0]);
        m.ys.push_back(pos[v][1]);
        m.zs.push_back(pos[v][2]);
        m.nxs.push_back(0.1f * static_cast<float>(v));
        m.nys.push_back(0.2f * static_cast<float>(v));
        m.nzs.push_back(1.f);
    }

    // face 0: triangle (0,1,2); face 1: quad (1,3,2,4)
    m.faceIndices = { 0, 1, 2, 1, 3, 2, 4 };
    m.faceOffsets = { 0, 3, 7 };

    const float triUv[6] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f };
    const float quadUv[8] = { 0.f, 0.f, 0.5f, 0.f, 1.f, 0.f, 1.f, 1.f };
    m.texcoords.assign(triUv, triUv + 6);
    m.texcoords.insert(m.texcoords.end(), quadUv, quadUv + 8);
    m.texcoordOffsets = { 0, 6, 14 };

    m.materials = { 7, 11 };
    return m;
}

tinyply_extras::PolygonMeshView make_mesh_view(const TestMesh & m)
{
    tinyply_extras::PolygonMeshView view;
    view.vertexCount = TestMesh::vertexCount;
    view.vertexProperties = {
        { "x",  Type::FLOAT32, reinterpret_cast<const uint8_t *>(m.xs.data()) },
        { "y",  Type::FLOAT32, reinterpret_cast<const uint8_t *>(m.ys.data()) },
        { "z",  Type::FLOAT32, reinterpret_cast<const uint8_t *>(m.zs.data()) },
        { "nx", Type::FLOAT32, reinterpret_cast<const uint8_t *>(m.nxs.data()) },
        { "ny", Type::FLOAT32, reinterpret_cast<const uint8_t *>(m.nys.data()) },
        { "nz", Type::FLOAT32, reinterpret_cast<const uint8_t *>(m.nzs.data()) }
    };
    view.faceCount = TestMesh::faceCount;
    view.faceIndices = m.faceIndices.data();
    view.faceOffsets = m.faceOffsets.data();
    view.hasTexcoords = true;
    view.texcoords = m.texcoords.data();
    view.texcoordOffsets = m.texcoordOffsets.data();
    view.hasMaterials = true;
    view.faceMaterials = m.materials.data();
    return view;
}

std::string encode(PlyFile & file, bool binary)
{
    std::ostringstream ss(std::ios::out | std::ios::binary);
    file.write(ss, binary);
    return ss.str();
}

// tinyply round-trip of an encoded file; only properties present in the
// header are requested, so the same helper works for every test layout.
struct RoundTrip
{
    std::map<std::string, std::shared_ptr<PlyData>> faceProps;
    std::map<std::string, std::shared_ptr<PlyData>> vertexProps;
};

RoundTrip roundtrip(const std::string & bytes)
{
    std::istringstream ss(bytes, std::ios::in | std::ios::binary);
    PlyFile f;
    REQUIRE(f.parse_header(ss));
    RoundTrip rt;
    for (const PlyElement & e : f.get_elements())
    {
        for (const PlyProperty & p : e.properties)
        {
            auto data = f.request_properties_from_element(e.name, { p.name }, 0);
            if (e.name == "face") rt.faceProps.emplace(p.name, data);
            else if (e.name == "vertex") rt.vertexProps.emplace(p.name, data);
        }
    }
    f.read(ss);
    return rt;
}

const std::shared_ptr<PlyData> & rt_face(const RoundTrip & rt, const std::string & name)
{
    auto it = rt.faceProps.find(name);
    REQUIRE(it != rt.faceProps.end());
    return it->second;
}

const std::shared_ptr<PlyData> & rt_vertex(const RoundTrip & rt, const std::string & name)
{
    auto it = rt.vertexProps.find(name);
    REQUIRE(it != rt.vertexProps.end());
    return it->second;
}

// Full verification of one encoded subset against expected renumbering.
void verify_subset_output(const std::string & bytes, bool binary, const TestMesh & m,
    const std::vector<size_t> & keepFaces, const std::vector<size_t> & expectedNewToOld)
{
    DecFile dec = decode_ply(bytes);
    REQUIRE(dec.binary == binary);

    const DecElement & verts = find_dec_element(dec, "vertex");
    const DecElement & faces = find_dec_element(dec, "face");

    // Header property registration order: lists first, fixed material last.
    REQUIRE(faces.properties.size() == 3);
    REQUIRE(faces.properties[0].name == "vertex_indices");
    REQUIRE(faces.properties[0].isList);
    REQUIRE(faces.properties[1].name == "texcoord");
    REQUIRE(faces.properties[1].isList);
    REQUIRE(faces.properties[2].name == "material_index");
    REQUIRE(!faces.properties[2].isList);

    const size_t kept = keepFaces.size();
    REQUIRE(verts.count == expectedNewToOld.size());
    REQUIRE(faces.count == kept);

    const DecProperty & decIdx = find_dec_property(faces, "vertex_indices");
    const DecProperty & decUv = find_dec_property(faces, "texcoord");
    const DecProperty & decMat = find_dec_property(faces, "material_index");
    const DecProperty & decX = find_dec_property(verts, "x");

    // Expected ragged layout straight from the source mesh.
    std::vector<size_t> expectedIndexOffsets = { 0 };
    std::vector<size_t> expectedUvOffsets = { 0 };
    std::map<uint32_t, uint32_t> oldToNew;
    std::vector<uint32_t> expectedIndices;
    std::vector<float> expectedUv;
    std::vector<int32_t> expectedMaterials;

    for (size_t outRow = 0; outRow < kept; ++outRow)
    {
        const size_t f = keepFaces[outRow];
        const size_t c0 = m.faceOffsets[f], c1 = m.faceOffsets[f + 1];
        for (size_t c = c0; c < c1; ++c)
        {
            const uint32_t oldId = m.faceIndices[c];
            if (!oldToNew.count(oldId))
                oldToNew.emplace(oldId, static_cast<uint32_t>(oldToNew.size()));
            expectedIndices.push_back(oldToNew[oldId]);
        }
        expectedIndexOffsets.push_back(expectedIndices.size());

        const size_t u0 = m.texcoordOffsets[f], u1 = m.texcoordOffsets[f + 1];
        expectedUv.insert(expectedUv.end(), m.texcoords.data() + u0, m.texcoords.data() + u1);
        expectedUvOffsets.push_back(expectedUv.size());

        expectedMaterials.push_back(m.materials[f]);
    }

    // Independent decode: list segmentation and values.
    for (size_t row = 0; row < kept; ++row)
    {
        const size_t begin = expectedIndexOffsets[row];
        const size_t end = expectedIndexOffsets[row + 1];
        REQUIRE(decIdx.listRows[row].size() == end - begin);
        for (size_t k = begin; k < end; ++k)
            REQUIRE(static_cast<uint32_t>(decIdx.listRows[row][k - begin]) == expectedIndices[k]);

        const size_t ub = expectedUvOffsets[row], ue = expectedUvOffsets[row + 1];
        REQUIRE(decUv.listRows[row].size() == ue - ub);
        for (size_t k = ub; k < ue; ++k)
            REQUIRE(nearly_equal(decUv.listRows[row][k - ub], expectedUv[k]));

        REQUIRE(static_cast<int32_t>(decMat.scalars[row]) == expectedMaterials[row]);
    }

    // Renumbered vertex channel values follow the new-to-old mapping.
    for (size_t v = 0; v < expectedNewToOld.size(); ++v)
    {
        const size_t oldId = expectedNewToOld[v];
        REQUIRE(nearly_equal(decX.scalars[v], m.xs[oldId]));
    }

    // tinyply must read its own output back identically.
    RoundTrip rt = roundtrip(bytes);
    const auto & rtIndices = rt_face(rt, "vertex_indices");
    const auto & rtUv = rt_face(rt, "texcoord");
    const auto & rtMaterial = rt_face(rt, "material_index");
    const auto & rtX = rt_vertex(rt, "x");

    REQUIRE(rtIndices->count == kept);
    // tinyply only materializes list_sizes when row lengths differ; for
    // uniform rows the field stays empty, so check it conditionally.
    if (!rtIndices->list_sizes.empty())
    {
        REQUIRE(rtIndices->list_sizes.size() == kept);
        for (size_t row = 0; row < kept; ++row)
            REQUIRE(rtIndices->list_sizes[row] == expectedIndexOffsets[row + 1] - expectedIndexOffsets[row]);
    }
    REQUIRE(rtIndices->buffer.size_bytes() == expectedIndices.size() * sizeof(uint32_t));
    REQUIRE(std::memcmp(rtIndices->buffer.get(), expectedIndices.data(),
        expectedIndices.size() * sizeof(uint32_t)) == 0);

    REQUIRE(rtUv->buffer.size_bytes() == expectedUv.size() * sizeof(float));
    REQUIRE(std::memcmp(rtUv->buffer.get(), expectedUv.data(),
        expectedUv.size() * sizeof(float)) == 0);
    REQUIRE(rtMaterial->buffer.size_bytes() == expectedMaterials.size() * sizeof(int32_t));
    REQUIRE(std::memcmp(rtMaterial->buffer.get(), expectedMaterials.data(),
        expectedMaterials.size() * sizeof(int32_t)) == 0);

    REQUIRE(rtX->count == expectedNewToOld.size());
    for (size_t v = 0; v < expectedNewToOld.size(); ++v)
    {
        float got;
        std::memcpy(&got, rtX->buffer.get() + v * sizeof(float), sizeof(float));
        REQUIRE(nearly_equal(got, m.xs[expectedNewToOld[v]]));
    }
}

void test_subset_editing_chain()
{
    std::cout << "[test] mixed polygon subset editing chain\n";
    const TestMesh m = make_test_mesh();
    const auto view = make_mesh_view(m);

    // Reverse face selection: quad first, triangle second.
    {
        const std::vector<size_t> keep = { 1, 0 };
        tinyply_extras::PolygonSubset subset = tinyply_extras::extract_polygon_subset(view, keep);

        const std::vector<size_t> expectedMapping = { 1, 3, 2, 4, 0 };
        REQUIRE(subset.newToOldVertices == expectedMapping);
        REQUIRE(subset.vertexCount == 5);
        REQUIRE((subset.faceOffsets == std::vector<size_t>{ 0, 4, 7 }));
        REQUIRE((subset.faceIndices == std::vector<uint32_t>{ 0, 1, 2, 3, 4, 0, 2 }));
        REQUIRE((subset.texcoordOffsets == std::vector<size_t>{ 0, 8, 14 }));
        REQUIRE((subset.faceMaterials == std::vector<int32_t>{ 11, 7 }));
        // Corner UV order of both faces is preserved verbatim: quad UVs first,
        // then triangle UVs, each taken from the original rows unchanged.
        REQUIRE(std::memcmp(subset.texcoords.data(), m.texcoords.data() + 6, 8 * sizeof(float)) == 0);
        REQUIRE(std::memcmp(subset.texcoords.data() + 8, m.texcoords.data(), 6 * sizeof(float)) == 0);

        PlyFile file;
        tinyply_extras::add_polygon_subset_to_ply(file, subset);

        const std::string ascii = encode(file, false);
        const std::string binary = encode(file, true);
        verify_subset_output(ascii, false, m, keep, expectedMapping);
        verify_subset_output(binary, true, m, keep, expectedMapping);
    }

    // Keep only the quad: its four vertices get renumbered 0..3.
    {
        const std::vector<size_t> keep = { 1 };
        tinyply_extras::PolygonSubset subset = tinyply_extras::extract_polygon_subset(view, keep);

        const std::vector<size_t> expectedMapping = { 1, 3, 2, 4 };
        REQUIRE(subset.newToOldVertices == expectedMapping);
        REQUIRE(subset.vertexCount == 4);
        REQUIRE((subset.faceOffsets == std::vector<size_t>{ 0, 4 }));
        REQUIRE((subset.faceIndices == std::vector<uint32_t>{ 0, 1, 2, 3 }));
        REQUIRE((subset.texcoordOffsets == std::vector<size_t>{ 0, 8 }));
        REQUIRE((subset.faceMaterials == std::vector<int32_t>{ 11 }));

        PlyFile file;
        tinyply_extras::add_polygon_subset_to_ply(file, subset);

        const std::string ascii = encode(file, false);
        const std::string binary = encode(file, true);
        verify_subset_output(ascii, false, m, keep, expectedMapping);
        verify_subset_output(binary, true, m, keep, expectedMapping);
    }

    // Subset input validation: no partial results, std::invalid_argument.
    {
        REQUIRE_THROWS_INVALID(tinyply_extras::extract_polygon_subset(view, { 0, 0 }));    // duplicate
        REQUIRE_THROWS_INVALID(tinyply_extras::extract_polygon_subset(view, { 2 }));       // out of range
        REQUIRE_THROWS_INVALID(tinyply_extras::extract_polygon_subset(view, { 99 }));      // out of range

        auto badView = view;
        std::vector<uint32_t> badIndices = m.faceIndices;
        badIndices[0] = 42;
        badView.faceIndices = badIndices.data();
        REQUIRE_THROWS_INVALID(tinyply_extras::extract_polygon_subset(badView, { 0 }));   // bad vertex ref

        auto badUvView = view;
        std::vector<size_t> badUvOffsets = { 0, 5, 14 }; // tri row no longer 2 scalars/corner
        badUvView.texcoordOffsets = badUvOffsets.data();
        REQUIRE_THROWS_INVALID(tinyply_extras::extract_polygon_subset(badUvView, { 0, 1 }));
    }
}

void test_empty_and_zero_rows()
{
    std::cout << "[test] empty lists (including an empty row between non-empty rows)\n";
    PlyFile file;

    const uint8_t valuesA[5] = { 1, 2, 3, 4, 5 };
    const size_t offsetsA[4] = { 0, 2, 2, 5 }; // rows: [1,2], [], [3,4,5]
    file.add_list_property_to_element("face", "a", Type::UINT8, 3, offsetsA,
        valuesA, sizeof(valuesA), Type::UINT8);

    const uint16_t valuesB[4] = { 10, 20, 30, 40 };
    const size_t offsetsB[4] = { 0, 0, 3, 4 }; // rows: [], [10,20,30], [40]
    file.add_list_property_to_element("face", "b", Type::UINT16, 3, offsetsB,
        reinterpret_cast<const uint8_t *>(valuesB), sizeof(valuesB), Type::UINT16);

    // Fixed per-row property coexisting with the ragged lists.
    const int32_t mats[3] = { -1, 2, -3 };
    file.add_properties_to_element("face", { "material_index" }, Type::INT32, 3,
        reinterpret_cast<const uint8_t *>(mats), Type::INVALID, 0);

    for (bool binary : { false, true })
    {
        DecFile dec = decode_ply(encode(file, binary));
        REQUIRE(dec.binary == binary);
        const DecElement & faces = find_dec_element(dec, "face");
        const DecProperty & a = find_dec_property(faces, "a");
        const DecProperty & b = find_dec_property(faces, "b");
        const DecProperty & mat = find_dec_property(faces, "material_index");

        REQUIRE(a.listRows.size() == 3);
        REQUIRE((a.listRows[0] == std::vector<double>{ 1, 2 }));
        REQUIRE(a.listRows[1].empty());
        REQUIRE((a.listRows[2] == std::vector<double>{ 3, 4, 5 }));

        REQUIRE(b.listRows.size() == 3);
        REQUIRE(b.listRows[0].empty());
        REQUIRE((b.listRows[1] == std::vector<double>{ 10, 20, 30 }));
        REQUIRE((b.listRows[2] == std::vector<double>{ 40 }));

        REQUIRE((mat.scalars == std::vector<double>{ -1, 2, -3 }));

        RoundTrip rt = roundtrip(encode(file, binary));
        REQUIRE((rt_face(rt, "a")->list_sizes == std::vector<size_t>{ 2, 0, 3 }));
    }

    std::cout << "[test] zero rows with a single zero offset\n";
    {
        PlyFile empty;
        const size_t zeroOffset[1] = { 0 };
        empty.add_list_property_to_element("face", "vertex_indices", Type::UINT32, 0,
            zeroOffset, nullptr, 0, Type::UINT8);

        for (bool binary : { false, true })
        {
            const std::string bytes = encode(empty, binary);
            DecFile dec = decode_ply(bytes);
            const DecElement & faces = find_dec_element(dec, "face");
            REQUIRE(faces.count == 0);
            REQUIRE(faces.properties.size() == 1);
            REQUIRE(faces.properties[0].isList);

            RoundTrip rt = roundtrip(bytes);
            REQUIRE(rt_face(rt, "vertex_indices")->count == 0);
            REQUIRE(rt_face(rt, "vertex_indices")->buffer.size_bytes() == 0);
        }
    }

    std::cout << "[test] non-zero rows that are all empty accept a null data pointer\n";
    {
        PlyFile f;
        const size_t allEmpty[4] = { 0, 0, 0, 0 }; // three rows, zero payload overall
        f.add_list_property_to_element("face", "a", Type::UINT8, 3, allEmpty,
            nullptr, 0, Type::UINT8);

        for (bool binary : { false, true })
        {
            DecFile dec = decode_ply(encode(f, binary));
            const DecProperty & a = find_dec_property(find_dec_element(dec, "face"), "a");
            REQUIRE(a.listRows.size() == 3);
            for (const auto & row : a.listRows) REQUIRE(row.empty());
        }
    }
}

void test_int8_length_boundary()
{
    std::cout << "[test] int8 list length boundary 127/128\n";
    {
        std::vector<uint8_t> values(127, 9);
        const size_t offsets[2] = { 0, 127 };
        PlyFile file;
        file.add_list_property_to_element("face", "a", Type::UINT8, 1, offsets,
            values.data(), values.size(), Type::INT8);

        for (bool binary : { false, true })
        {
            DecFile dec = decode_ply(encode(file, binary));
            const DecProperty & a = find_dec_property(find_dec_element(dec, "face"), "a");
            REQUIRE(a.listRows.size() == 1);
            REQUIRE(a.listRows[0].size() == 127);
        }
    }
    {
        std::vector<uint8_t> values(128, 9);
        const size_t offsets[2] = { 0, 128 };
        PlyFile file;
        REQUIRE_THROWS_INVALID(file.add_list_property_to_element("face", "a", Type::UINT8, 1,
            offsets, values.data(), values.size(), Type::INT8));
    }
}

void test_bad_arguments_and_recovery()
{
    std::cout << "[test] invalid offsets, byte counts and duplicate properties\n";

    const uint8_t data[5] = { 1, 2, 3, 4, 5 };

    {
        PlyFile f;
        const size_t badFirst[2] = { 1, 2 };
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 1,
            badFirst, data, 1, Type::UINT8));
    }
    {
        PlyFile f;
        const size_t nonMono[3] = { 0, 2, 1 };
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 2,
            nonMono, data, 1, Type::UINT8));
    }
    {
        PlyFile f;
        const size_t offsets[2] = { 0, 3 };
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 1,
            offsets, data, 2, Type::UINT8)); // dataBytes too small
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 1,
            offsets, nullptr, 3, Type::UINT8)); // non-empty payload, null data
    }
    {
        PlyFile f;
        const size_t offsets[2] = { 0, 1 };
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::INVALID, 1,
            offsets, data, 1, Type::UINT8));
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 1,
            offsets, data, 1, Type::INVALID));
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 1,
            offsets, data, 1, Type::FLOAT32)); // list count must be integral
    }
    {
        // Multiplication overflow must be detected before it is performed.
        PlyFile f;
        const size_t huge = (std::numeric_limits<size_t>::max)() / 4;
        const size_t offsets[2] = { 0, huge };
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::FLOAT64, 1,
            offsets, nullptr, 0, Type::UINT32));
    }
    {
        // Row count mismatch against an existing element: rejected, and the
        // existing property still writes while a later valid add is accepted.
        PlyFile f;
        const uint8_t scalars[2] = { 1, 2 };
        f.add_properties_to_element("e", { "s" }, Type::UINT8, 2, scalars, Type::INVALID, 0);

        const size_t offsets[2] = { 0, 1 };
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 1,
            offsets, data, 1, Type::UINT8));

        // A correctly-sized list property on the same element must still go in.
        const uint8_t listData[3] = { 7, 8, 9 };
        const size_t goodOffsets[3] = { 0, 1, 3 };
        f.add_list_property_to_element("e", "p", Type::UINT8, 2, goodOffsets,
            listData, sizeof(listData), Type::UINT8);

        DecFile dec = decode_ply(encode(f, false));
        const DecElement & e = find_dec_element(dec, "e");
        REQUIRE(e.count == 2);
        REQUIRE(e.properties.size() == 2);
        const DecProperty & s = find_dec_property(e, "s");
        const DecProperty & p = find_dec_property(e, "p");
        REQUIRE((s.scalars == std::vector<double>{ 1, 2 }));
        REQUIRE((p.listRows[0] == std::vector<double>{ 7 }));
        REQUIRE((p.listRows[1] == std::vector<double>{ 8, 9 }));
    }
    {
        // Duplicate property is rejected and the existing property still writes.
        PlyFile f;
        const size_t offsetsA[3] = { 0, 1, 2 };
        f.add_list_property_to_element("e", "p", Type::UINT8, 2, offsetsA, data, 2, Type::UINT8);
        REQUIRE_THROWS_INVALID(f.add_list_property_to_element("e", "p", Type::UINT8, 2,
            offsetsA, data, 2, Type::UINT8));

        const std::string bytes = encode(f, false);
        DecFile dec = decode_ply(bytes);
        const DecElement & e = find_dec_element(dec, "e");
        REQUIRE(e.properties.size() == 1);
        REQUIRE(e.properties[0].name == "p");
        REQUIRE((e.properties[0].listRows[0] == std::vector<double>{ 1 }));
        REQUIRE((e.properties[0].listRows[1] == std::vector<double>{ 2 }));
    }
}

void test_ownership_copy()
{
    std::cout << "[test] library owns copies: mutating source arrays after the call\n";
    std::vector<uint8_t> data = { 5, 6, 7, 8 };
    std::vector<size_t> offsets = { 0, 1, 4 };

    PlyFile file;
    file.add_list_property_to_element("e", "p", Type::UINT8, 2, offsets.data(),
        data.data(), data.size(), Type::UINT8);

    // Destroy the caller's content; the encoded result must be unaffected.
    std::fill(data.begin(), data.end(), 0xEE);
    std::fill(offsets.begin(), offsets.end(), std::numeric_limits<size_t>::max());

    for (bool binary : { false, true })
    {
        DecFile dec = decode_ply(encode(file, binary));
        const DecProperty & p = find_dec_property(find_dec_element(dec, "e"), "p");
        REQUIRE((p.listRows[0] == std::vector<double>{ 5 }));
        REQUIRE((p.listRows[1] == std::vector<double>{ 6, 7, 8 }));
    }
}

void test_legacy_fixed_triangles()
{
    std::cout << "[test] legacy add_properties_to_element fixed triangle lists remain unchanged\n";
    const uint32_t triangles[6] = { 0, 1, 2, 2, 1, 3 };
    PlyFile file;
    file.add_properties_to_element("face", { "vertex_indices" }, Type::UINT32, 2,
        reinterpret_cast<const uint8_t *>(triangles), Type::UINT8, 3);

    for (bool binary : { false, true })
    {
        const std::string bytes = encode(file, binary);
        DecFile dec = decode_ply(bytes);
        const DecProperty & p = find_dec_property(find_dec_element(dec, "face"), "vertex_indices");
        REQUIRE(p.listRows.size() == 2);
        REQUIRE((p.listRows[0] == std::vector<double>{ 0, 1, 2 }));
        REQUIRE((p.listRows[1] == std::vector<double>{ 2, 1, 3 }));

        std::istringstream ss(bytes, std::ios::in | std::ios::binary);
        PlyFile g;
        REQUIRE(g.parse_header(ss));
        auto faces = g.request_properties_from_element("face", { "vertex_indices" }, 0);
        g.read(ss);
        REQUIRE(faces->buffer.size_bytes() == sizeof(triangles));
        REQUIRE(std::memcmp(faces->buffer.get(), triangles, sizeof(triangles)) == 0);
    }
}

void test_repeated_writes_reset_cursors()
{
    std::cout << "[test] ascii/binary/ascii writes of one object keep identical segmentation\n";
    const uint8_t values[7] = { 3, 1, 4, 1, 5, 9, 2 };
    const size_t offsets[3] = { 0, 3, 7 }; // rows of length 3 and 4, no triangles introduced

    PlyFile file;
    file.add_list_property_to_element("face", "a", Type::UINT8, 2, offsets,
        values, sizeof(values), Type::UINT8);
    file.add_list_property_to_element("face", "b", Type::UINT8, 2, offsets,
        values, sizeof(values), Type::UINT16);

    const std::string ascii1 = encode(file, false);
    const std::string binary = encode(file, true);
    const std::string ascii2 = encode(file, false);

    REQUIRE(ascii1 == ascii2); // no cursor state leaks between writes

    DecFile d1 = decode_ply(ascii1);
    DecFile d2 = decode_ply(binary);
    const DecProperty & a1 = find_dec_property(find_dec_element(d1, "face"), "a");
    const DecProperty & a2 = find_dec_property(find_dec_element(d2, "face"), "a");
    const DecProperty & b1 = find_dec_property(find_dec_element(d1, "face"), "b");
    const DecProperty & b2 = find_dec_property(find_dec_element(d2, "face"), "b");

    for (const DecProperty * pair : { &a1, &a2, &b1, &b2 })
    {
        REQUIRE(pair->listRows.size() == 2);
        REQUIRE((pair->listRows[0] == std::vector<double>{ 3, 1, 4 }));
        REQUIRE((pair->listRows[1] == std::vector<double>{ 1, 5, 9, 2 }));
    }
}

} // end anonymous namespace

int main()
{
    test_subset_editing_chain();
    test_empty_and_zero_rows();
    test_int8_length_boundary();
    test_bad_arguments_and_recovery();
    test_ownership_copy();
    test_legacy_fixed_triangles();
    test_repeated_writes_reset_cursors();

    std::cout << "ragged mesh export PASS (" << g_checks << " checks)\n";
    return 0;
}
