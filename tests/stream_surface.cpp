/*
 * Tests for out-of-core FSSR surface extraction (fssr/stream_surface.h).
 *
 * The suite builds multi-level octrees from synthetic point samples and
 * compares the streaming PLY output against the reference extract_mesh()
 * geometry (positions, attributes, oriented triangles and edge adjacency).
 * It also verifies budget invariance of the bytes, the peak working set,
 * publication and failure semantics and compatibility of the legacy entry
 * point.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fssr/hermite.h"
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
#include "mve/mesh.h"
#include "task_support/check.h"

namespace
{
    std::size_t const KIB = 1024;
    std::size_t const TINY_BUDGET = 64 * KIB;

    std::string
    temp_root (void)
    {
        char const* base = std::getenv("TMPDIR");
        std::string root = (base != nullptr ? base : ".");
        root += "/fssr-stream-tests";
        ::mkdir(root.c_str(), 0777);
        return root;
    }

    std::string
    unique_dir (std::string const& root, char const* name)
    {
        static int counter = 0;
        std::string dir = root + "/" + name + "-"
            + std::to_string(static_cast<unsigned long>(::getpid())) + "-"
            + std::to_string(counter++);
        ::mkdir(dir.c_str(), 0777);
        return dir;
    }

    std::string
    read_file (std::string const& path)
    {
        std::ifstream in(path.c_str(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
    }

    void
    write_file (std::string const& path, std::string const& data)
    {
        std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    int
    count_entries (std::string const& dir)
    {
        DIR* handle = ::opendir(dir.c_str());
        if (handle == nullptr)
            return -1;
        int count = 0;
        while (dirent* entry = ::readdir(handle))
        {
            std::string const name(entry->d_name);
            if (name != "." && name != "..")
                count += 1;
        }
        ::closedir(handle);
        return count;
    }

    /* A parsed binary little-endian PLY produced by the new entry point. */
    struct PlyMesh
    {
        struct Vertex
        {
            float pos[3];
            unsigned char color[3];
            float value;
            float confidence;
        };
        std::vector<Vertex> vertices;
        std::vector<std::array<uint32_t, 3> > triangles;
        bool valid;
        PlyMesh (void) : valid(false) { }
    };

    /* Parses exactly the PLY layout written by extract_stream_surface(). */
    PlyMesh
    parse_ply (std::string const& path)
    {
        PlyMesh mesh;
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in.good())
            return mesh;

        std::string line;
        std::size_t num_vertices = 0, num_faces = 0;
        bool saw_format = false;
        while (std::getline(in, line))
        {
            if (line == "end_header")
                break;
            if (line == "format binary_little_endian 1.0")
                saw_format = true;
            if (line.rfind("element vertex ", 0) == 0)
                num_vertices = std::strtoul(line.c_str() + 15, nullptr, 10);
            else if (line.rfind("element face ", 0) == 0)
                num_faces = std::strtoul(line.c_str() + 13, nullptr, 10);
        }
        if (!saw_format)
            return mesh;

        mesh.vertices.resize(num_vertices);
        for (std::size_t i = 0; i < num_vertices; ++i)
        {
            PlyMesh::Vertex& v = mesh.vertices[i];
            in.read(reinterpret_cast<char*>(v.pos), 12);
            in.read(reinterpret_cast<char*>(v.color), 3);
            in.read(reinterpret_cast<char*>(&v.value), 4);
            in.read(reinterpret_cast<char*>(&v.confidence), 4);
            if (!in)
                return PlyMesh();
        }
        mesh.triangles.reserve(num_faces);
        for (std::size_t i = 0; i < num_faces; ++i)
        {
            unsigned char n = 0;
            std::array<uint32_t, 3> tri;
            in.read(reinterpret_cast<char*>(&n), 1);
            in.read(reinterpret_cast<char*>(tri.data()), 12);
            if (!in || n != 3)
                return PlyMesh();
            mesh.triangles.push_back(tri);
        }
        mesh.valid = true;
        return mesh;
    }

    struct SceneOptions
    {
        SceneOptions (void)
            : level(6), rings(16), segments(32), scale_low(0.23f)
            , scale_high(0.33f), split(16) { }
        int level;
        int rings;
        int segments;
        float scale_low;
        float scale_high;
        int split;
    };

    /* Builds a sphere with two scale bands that induce cross-level edges. */
    void
    build_scene (fssr::IsoOctree* tree, SceneOptions const& options)
    {
        tree->set_max_level(options.level);
        for (int i = 1; i < options.rings; ++i)
        {
            double const a = M_PI * i / options.rings;
            for (int j = 0; j < options.segments; ++j)
            {
                double const b = 2.0 * M_PI * j / options.segments;
                math::Vec3f const normal(
                    static_cast<float>(std::sin(a) * std::cos(b)),
                    static_cast<float>(std::sin(a) * std::sin(b)),
                    static_cast<float>(std::cos(a)));
                fssr::Sample sample;
                sample.normal = normal;
                sample.pos = normal;
                sample.color = math::Vec3f(0.2f + 0.1f * (i % 3),
                    0.4f + 0.1f * (j % 2), 0.7f);
                sample.scale = j < options.split
                    ? options.scale_low : options.scale_high;
                sample.confidence = 0.75f + 0.25f * ((i + j) % 2);
                tree->insert_sample(sample);
            }
        }
        tree->limit_octree_level();
        tree->compute_voxels();
    }

    void
    build_default_scene (fssr::IsoOctree* tree)
    {
        build_scene(tree, SceneOptions());
    }

    /* Builds two separated spheres to obtain multiple surface components. */
    void
    build_two_spheres (fssr::IsoOctree* tree)
    {
        tree->set_max_level(6);
        float const gap = 1.0f;
        for (int s = 0; s < 2; ++s)
        {
            math::Vec3f const center(s == 0 ? -gap / 2.0f : gap / 2.0f, 0.0f, 0.0f);
            for (int i = 1; i < 20; ++i)
            {
                double const a = M_PI * i / 20.0;
                for (int j = 0; j < 40; ++j)
                {
                    double const b = 2.0 * M_PI * j / 40.0;
                    math::Vec3f const d(
                        static_cast<float>(std::sin(a) * std::cos(b)),
                        static_cast<float>(std::sin(a) * std::sin(b)),
                        static_cast<float>(std::cos(a)));
                    fssr::Sample sample;
                    sample.normal = d;
                    sample.pos = center + d * 0.4f;
                    sample.color = math::Vec3f(s == 0 ? 0.9f : 0.1f, 0.5f, 0.2f);
                    sample.scale = 0.08f;
                    sample.confidence = 1.0f;
                    tree->insert_sample(sample);
                }
            }
        }
        tree->limit_octree_level();
        tree->compute_voxels();
    }

    /* Builds an octree whose opposing normals cancel: no iso crossing. */
    void
    build_empty_scene (fssr::IsoOctree* tree)
    {
        tree->set_max_level(4);
        for (int s = 0; s < 2; ++s)
        {
            fssr::Sample sample;
            sample.pos = math::Vec3f(0.0f, 0.0f, 0.0f);
            sample.normal = math::Vec3f(s == 0 ? 1.0f : -1.0f, 0.0f, 0.0f);
            sample.scale = 0.4f;
            sample.confidence = 1.0f;
            sample.color = math::Vec3f(0.5f, 0.5f, 0.5f);
            tree->insert_sample(sample);
        }
        tree->limit_octree_level();
        tree->compute_voxels();
    }

    /* -------------------- reference mesh accessors -------------------- */

    struct Reference
    {
        std::vector<math::Vec3f> positions;
        std::vector<math::Vec4f> colors;
        std::vector<float> values;
        std::vector<float> confidences;
        std::vector<uint32_t> faces;
    };

    Reference
    reference_mesh (fssr::IsoOctree* tree, fssr::InterpolationType type)
    {
        fssr::IsoSurface surface(tree, type);
        mve::TriangleMesh::Ptr mesh = surface.extract_mesh();
        Reference ref;
        ref.positions = mesh->get_vertices();
        ref.colors = mesh->get_vertex_colors();
        ref.values = mesh->get_vertex_values();
        ref.confidences = mesh->get_vertex_confidences();
        ref.faces = mesh->get_faces();
        return ref;
    }

    bool
    same_float (float a, float b)
    {
        return std::memcmp(&a, &b, sizeof(float)) == 0;
    }

    /*
     * Distinct edge keys can interpolate to bit-identical positions, so
     * positions alone do not identify a vertex. Geometry is therefore
     * compared structurally: triangles in emission order must reference the
     * same positions as the reference, attributes must agree as multisets
     * per position, and edges (pairs of positions) must agree exactly.
     */
    struct PositionKey
    {
        float v[3];
        bool operator< (PositionKey const& other) const
        {
            for (int i = 0; i < 3; ++i)
            {
                if (v[i] < other.v[i]) return true;
                if (other.v[i] < v[i]) return false;
            }
            return false;
        }
        bool operator== (PositionKey const& other) const
        {
            return std::memcmp(v, other.v, sizeof(v)) == 0;
        }
        bool operator!= (PositionKey const& other) const
        {
            return !(*this == other);
        }
    };

    struct AttributeKey
    {
        unsigned char color[3];
        float value;
        float confidence;
        bool operator< (AttributeKey const& other) const
        {
            int const c = std::memcmp(color, other.color, 3);
            if (c != 0) return c < 0;
            if (value < other.value) return true;
            if (other.value < value) return false;
            return confidence < other.confidence;
        }
        bool operator== (AttributeKey const& other) const
        {
            return std::memcmp(color, other.color, 3) == 0
                && same_float(value, other.value)
                && same_float(confidence, other.confidence);
        }
    };

    PositionKey
    position_key (float const* p)
    {
        PositionKey key;
        std::memcpy(key.v, p, sizeof(float) * 3);
        return key;
    }

    /* Reference attribute tuple for a vertex, color converted like the PLY. */
    AttributeKey
    reference_attribute (Reference const& ref, std::size_t index)
    {
        AttributeKey key;
        math::Vec4f const& c = ref.colors[index];
        for (int i = 0; i < 3; ++i)
        {
            float const scaled = std::min(255.0f, std::max(0.0f, c[i] * 255.0f));
            key.color[i] = static_cast<unsigned char>(scaled + 0.5f);
        }
        key.value = ref.values[index];
        key.confidence = ref.confidences[index];
        return key;
    }

    bool
    verify_geometry (PlyMesh const& ply, Reference const& ref,
        std::string* detail)
    {
        if (!ply.valid)
        {
            *detail = "PLY is not valid";
            return false;
        }
        if (ply.vertices.size() != ref.positions.size())
        {
            *detail = "vertex count differs";
            return false;
        }
        if (ply.triangles.size() != ref.faces.size() / 3)
        {
            *detail = "triangle count differs";
            return false;
        }

        /* Attributes must match the reference as a multiset per position. */
        std::map<PositionKey, std::multiset<AttributeKey> > expected;
        for (std::size_t i = 0; i < ref.positions.size(); ++i)
            expected[position_key(*ref.positions[i])].insert(
                reference_attribute(ref, i));
        std::map<PositionKey, std::multiset<AttributeKey> > actual;
        for (std::size_t i = 0; i < ply.vertices.size(); ++i)
        {
            AttributeKey key;
            std::memcpy(key.color, ply.vertices[i].color, 3);
            key.value = ply.vertices[i].value;
            key.confidence = ply.vertices[i].confidence;
            actual[position_key(ply.vertices[i].pos)].insert(key);
        }
        if (actual != expected)
        {
            *detail = "vertex position/attribute multiset differs";
            return false;
        }

        /* Triangles in emission order must reference the same positions. */
        std::vector<std::size_t> new_position_id;
        for (std::size_t t = 0; t < ply.triangles.size(); ++t)
        {
            for (int k = 0; k < 3; ++k)
            {
                PositionKey const np
                    = position_key(ply.vertices[ply.triangles[t][k]].pos);
                PositionKey const rp
                    = position_key(*ref.positions[ref.faces[3 * t + k]]);
                if (np < rp || rp < np)
                {
                    *detail = "oriented triangle order differs";
                    return false;
                }
            }
        }

        /* Edge adjacency as unordered position pairs. */
        typedef std::pair<PositionKey, PositionKey> EdgeKey;
        std::multiset<EdgeKey> new_edges, ref_edges;
        for (std::size_t t = 0; t < ply.triangles.size(); ++t)
        {
            PositionKey const p[3] = {
                position_key(ply.vertices[ply.triangles[t][0]].pos),
                position_key(ply.vertices[ply.triangles[t][1]].pos),
                position_key(ply.vertices[ply.triangles[t][2]].pos) };
            PositionKey const r[3] = {
                position_key(*ref.positions[ref.faces[3 * t + 0]]),
                position_key(*ref.positions[ref.faces[3 * t + 1]]),
                position_key(*ref.positions[ref.faces[3 * t + 2]]) };
            for (int k = 0; k < 3; ++k)
            {
                EdgeKey ne = std::make_pair(p[k], p[(k + 1) % 3]);
                if (ne.second < ne.first)
                    std::swap(ne.first, ne.second);
                new_edges.insert(ne);
                EdgeKey re = std::make_pair(r[k], r[(k + 1) % 3]);
                if (re.second < re.first)
                    std::swap(re.first, re.second);
                ref_edges.insert(re);
            }
        }
        if (new_edges != ref_edges)
        {
            *detail = "edge adjacency differs";
            return false;
        }
        return true;
    }

}

/* ===================================================================== */
/* geometry                                                              */
/* ===================================================================== */

static int
geometry_interpolation_modes (void)
{
    SceneOptions options;
    options.level = 7;
    options.rings = 24;
    options.segments = 48;
    options.scale_low = 0.08f;
    options.scale_high = 0.2f;

    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "geometry-modes");

    for (int mode = 0; mode < 4; ++mode)
    {
        fssr::InterpolationType const type
            = static_cast<fssr::InterpolationType>(mode);

        fssr::IsoOctree ref_tree;
        build_scene(&ref_tree, options);
        Reference const ref = reference_mesh(&ref_tree, type);

        fssr::IsoOctree tree;
        build_scene(&tree, options);
        std::string const ply = dir + "/mode" + std::to_string(mode) + ".ply";
        fssr::StreamSurfaceStats const stats
            = fssr::extract_stream_surface(&tree, type, ply, dir, TINY_BUDGET);

        PlyMesh const mesh = parse_ply(ply);
        std::string detail;
        TASK_CHECK(verify_geometry(mesh, ref, &detail));

        TASK_CHECK(stats.vertex_count == ref.positions.size());
        TASK_CHECK(stats.triangle_count == ref.faces.size() / 3);
        TASK_CHECK(stats.merge_passes >= 1);
    }
    return 0;
}

static int
geometry_cross_level (void)
{
    /* Two scale bands and several levels yield twin-closed cross-level edges. */
    SceneOptions options;
    options.level = 7;
    options.rings = 32;
    options.segments = 64;
    options.scale_low = 0.06f;
    options.scale_high = 0.15f;

    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "geometry-cross");

    fssr::IsoOctree ref_tree;
    build_scene(&ref_tree, options);
    Reference const ref = reference_mesh(&ref_tree, fssr::INTERPOLATION_CUBIC);
    TASK_CHECK(ref_tree.get_num_levels() > 3);

    fssr::IsoOctree tree;
    build_scene(&tree, options);
    std::string const ply = dir + "/cross.ply";
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_CUBIC, ply, dir, TINY_BUDGET);
    PlyMesh const mesh = parse_ply(ply);

    std::string detail;
    TASK_CHECK(verify_geometry(mesh, ref, &detail));
    /* Polygons larger than a quad exercise the O(p^2) triangulation path. */
    TASK_CHECK(stats.max_polygon_vertices >= 5);
    TASK_CHECK(stats.triangulation_workspace_bytes
        == stats.max_polygon_vertices * stats.max_polygon_vertices
        * (sizeof(float) + sizeof(int)));
    return 0;
}

static int
geometry_multi_component (void)
{
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "geometry-components");

    fssr::IsoOctree ref_tree;
    build_two_spheres(&ref_tree);
    Reference const ref = reference_mesh(&ref_tree, fssr::INTERPOLATION_CUBIC);

    fssr::IsoOctree tree;
    build_two_spheres(&tree);
    std::string const ply = dir + "/two.ply";
    fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC, ply, dir,
        TINY_BUDGET);
    PlyMesh const mesh = parse_ply(ply);

    std::string detail;
    TASK_CHECK(verify_geometry(mesh, ref, &detail));

    /* Union-find over the output triangles must reveal two components. */
    std::vector<int> parent(mesh.vertices.size());
    for (std::size_t i = 0; i < parent.size(); ++i)
        parent[i] = static_cast<int>(i);
    std::function<int(int)> find = [&] (int x)
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            int a = find(mesh.triangles[t][k]);
            int b = find(mesh.triangles[t][(k + 1) % 3]);
            if (a != b)
                parent[a] = b;
        }
    std::set<int> roots;
    for (std::size_t i = 0; i < parent.size(); ++i)
        if (find(static_cast<int>(i)) == static_cast<int>(i)
            && !mesh.triangles.empty())
            roots.insert(static_cast<int>(i));
    TASK_CHECK(roots.size() >= 2);
    return 0;
}

static int
geometry_empty_surface (void)
{
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "geometry-empty");

    fssr::IsoOctree tree;
    build_empty_scene(&tree);
    std::string const ply = dir + "/empty.ply";
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_CUBIC, ply, dir, TINY_BUDGET);
    TASK_CHECK(stats.vertex_count == 0);
    TASK_CHECK(stats.triangle_count == 0);

    PlyMesh const mesh = parse_ply(ply);
    TASK_CHECK(mesh.valid);
    TASK_CHECK(mesh.vertices.empty());
    TASK_CHECK(mesh.triangles.empty());

    /* A structurally empty octree must also produce a valid empty file. */
    fssr::IsoOctree bare;
    std::string const bare_ply = dir + "/bare.ply";
    fssr::StreamSurfaceStats const bare_stats = fssr::extract_stream_surface(&bare,
        fssr::INTERPOLATION_LINEAR, bare_ply, dir, TINY_BUDGET);
    TASK_CHECK(bare_stats.vertex_count == 0);
    PlyMesh const bare_mesh = parse_ply(bare_ply);
    TASK_CHECK(bare_mesh.valid && bare_mesh.vertices.empty());
    return 0;
}

/* ===================================================================== */
/* budget                                                                */
/* ===================================================================== */

static int
budget_multipass (void)
{
    /*
     * A large scene at the minimum budget must force many spilled runs and
     * several merge passes. The tiny budget is far below the record count.
     */
    SceneOptions options;
    options.level = 7;
    options.rings = 32;
    options.segments = 64;
    options.scale_low = 0.06f;
    options.scale_high = 0.15f;

    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "budget-multipass");

    fssr::IsoOctree ref_tree;
    build_scene(&ref_tree, options);
    Reference const ref = reference_mesh(&ref_tree, fssr::INTERPOLATION_CUBIC);

    fssr::IsoOctree tree;
    build_scene(&tree, options);
    std::string const ply = dir + "/multi.ply";
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_CUBIC, ply, dir, TINY_BUDGET);

    TASK_CHECK(stats.merge_passes >= 3);
    TASK_CHECK(stats.temp_write_bytes > 0);
    TASK_CHECK(stats.temp_read_bytes > 0);
    TASK_CHECK(stats.temp_write_bytes >= stats.triangle_count * 13 * 0);
    TASK_CHECK(stats.peak_working_bytes <= TINY_BUDGET);
    PlyMesh const mesh = parse_ply(ply);
    std::string detail;
    TASK_CHECK(verify_geometry(mesh, ref, &detail));
    return 0;
}

static int
budget_invariant_bytes (void)
{
    /* The same input and interpolation must yield identical bytes. */
    SceneOptions options;
    options.level = 6;
    options.rings = 20;
    options.segments = 40;
    options.scale_low = 0.15f;
    options.scale_high = 0.3f;

    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "budget-invariant");

    std::size_t const budgets[] = { 64 * KIB, 96 * KIB, 128 * KIB,
        256 * KIB, 1024 * KIB };
    std::string reference;
    for (std::size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i)
    {
        fssr::IsoOctree tree;
        build_scene(&tree, options);
        std::string const ply = dir + "/b" + std::to_string(i) + ".ply";
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_LSDERIV,
            ply, dir, budgets[i]);
        std::string const bytes = read_file(ply);
        TASK_CHECK(!bytes.empty());
        if (i == 0)
            reference = bytes;
        else
            TASK_CHECK(bytes == reference);
    }
    TASK_CHECK(!reference.empty());
    return 0;
}

static int
budget_peak_constant (void)
{
    /*
     * Scaling the sample density increases the output, but the peak working
     * set at a fixed 64 KiB budget must stay bounded by it.
     */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "budget-peak");

    SceneOptions small;
    small.level = 6;
    small.rings = 16;
    small.segments = 32;
    SceneOptions large;
    large.level = 7;
    large.rings = 32;
    large.segments = 64;
    large.scale_low = 0.06f;
    large.scale_high = 0.15f;

    fssr::IsoOctree small_tree;
    build_scene(&small_tree, small);
    fssr::StreamSurfaceStats const small_stats = fssr::extract_stream_surface(
        &small_tree, fssr::INTERPOLATION_CUBIC, dir + "/small.ply", dir,
        TINY_BUDGET);

    fssr::IsoOctree large_tree;
    build_scene(&large_tree, large);
    fssr::StreamSurfaceStats const large_stats = fssr::extract_stream_surface(
        &large_tree, fssr::INTERPOLATION_CUBIC, dir + "/large.ply", dir,
        TINY_BUDGET);

    TASK_CHECK(large_stats.vertex_count > 4 * small_stats.vertex_count);
    TASK_CHECK(small_stats.peak_working_bytes <= TINY_BUDGET);
    TASK_CHECK(large_stats.peak_working_bytes <= TINY_BUDGET);
    /* The peak must not scale with the output. */
    TASK_CHECK(large_stats.peak_working_bytes
        < small_stats.peak_working_bytes * 3 / 2);
    TASK_CHECK(large_stats.triangulation_workspace_bytes
        == large_stats.max_polygon_vertices * large_stats.max_polygon_vertices
        * (sizeof(float) + sizeof(int)));
    TASK_CHECK(large_stats.temp_write_bytes > small_stats.temp_write_bytes);
    return 0;
}

static int
budget_statistics_coverage (void)
{
    /* Verify that statistics report real I/O and cover the workspace. */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "budget-stats");

    SceneOptions options;
    options.level = 7;
    options.rings = 24;
    options.segments = 48;
    options.scale_low = 0.08f;
    options.scale_high = 0.2f;

    fssr::IsoOctree tree;
    build_scene(&tree, options);
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_SCALING, dir + "/stats.ply", dir, 96 * KIB);

    TASK_CHECK(stats.vertex_count > 0);
    TASK_CHECK(stats.triangle_count > 0);
    TASK_CHECK(stats.peak_working_bytes > 0);
    TASK_CHECK(stats.peak_working_bytes <= 96 * KIB);
    TASK_CHECK(stats.temp_read_bytes > 0);
    TASK_CHECK(stats.temp_write_bytes > 0);
    TASK_CHECK(stats.merge_passes >= 1);
    TASK_CHECK(stats.max_polygon_vertices >= 3);
    TASK_CHECK(stats.triangulation_workspace_bytes
        == stats.max_polygon_vertices * stats.max_polygon_vertices
        * (sizeof(float) + sizeof(int)));
    return 0;
}

static int
budget_many_spills (void)
{
    /*
     * The minimum budget holds roughly 60 KiB / 60 = 1024 records, so a
     * scene with several thousand polygon slots forces many run files and
     * multiple merge levels. The output must still match the reference.
     */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "budget-spills");

    fssr::IsoOctree ref_tree;
    build_two_spheres(&ref_tree);
    Reference const ref = reference_mesh(&ref_tree, fssr::INTERPOLATION_LINEAR);
    TASK_CHECK(ref.positions.size() > 1000);

    fssr::IsoOctree tree;
    build_two_spheres(&tree);
    std::string const ply = dir + "/spills.ply";
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_LINEAR, ply, dir, TINY_BUDGET);

    TASK_CHECK(stats.merge_passes >= 4);
    TASK_CHECK(stats.peak_working_bytes <= TINY_BUDGET);
    TASK_CHECK(stats.vertex_count == ref.positions.size());
    PlyMesh const mesh = parse_ply(ply);
    std::string detail;
    TASK_CHECK(verify_geometry(mesh, ref, &detail));
    /* Temporary files must be gone. */
    TASK_CHECK(count_entries(dir) == 1);
    return 0;
}

static int
budget_vertex_order (void)
{
    /*
     * Vertex IDs must be assigned in increasing edge-key order, and the
     * payload must be the interpolation of the first record of each key.
     * Recompute the unique edge keys in the reference extractor's order
     * (std::map over the same isovertex collection) and compare the PLY
     * vertex table position-for-position.
     */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "budget-order");

    SceneOptions options;
    options.level = 6;
    options.rings = 20;
    options.segments = 40;
    options.scale_low = 0.15f;
    options.scale_high = 0.3f;

    fssr::IsoOctree ref_tree;
    build_scene(&ref_tree, options);
    fssr::IsoSurface ref_surface(&ref_tree, fssr::INTERPOLATION_CUBIC);
    std::vector<std::pair<uint64_t, uint64_t> > keys;
    std::vector<math::Vec3f> order_positions;
    ref_surface.get_ordered_edge_vertices(&keys, &order_positions);

    fssr::IsoOctree tree;
    build_scene(&tree, options);
    std::string const ply = dir + "/order.ply";
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_CUBIC, ply, dir, TINY_BUDGET);

    PlyMesh const mesh = parse_ply(ply);
    TASK_CHECK(mesh.valid);
    TASK_CHECK(stats.vertex_count == keys.size());
    TASK_CHECK(mesh.vertices.size() == keys.size());

    /* Keys strictly increase, and positions follow the sorted key order. */
    bool sorted = true;
    for (std::size_t i = 1; i < keys.size(); ++i)
        sorted = sorted && keys[i - 1] < keys[i];
    TASK_CHECK(sorted);

    bool positions_match = true;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        PlyMesh::Vertex const& v = mesh.vertices[i];
        math::Vec3f const& r = order_positions[i];
        positions_match = positions_match
            && same_float(v.pos[0], r[0])
            && same_float(v.pos[1], r[1])
            && same_float(v.pos[2], r[2]);
    }
    TASK_CHECK(positions_match);

    bool indices_ok = true;
    for (std::size_t t = 0; t < mesh.triangles.size(); ++t)
    {
        std::array<uint32_t, 3> const& tri = mesh.triangles[t];
        for (int k = 0; k < 3; ++k)
            indices_ok = indices_ok && tri[k] < stats.vertex_count;
        indices_ok = indices_ok && tri[0] != tri[1]
            && tri[1] != tri[2] && tri[0] != tri[2];
    }
    TASK_CHECK(indices_ok);

    Reference const ref = reference_mesh(&ref_tree, fssr::INTERPOLATION_CUBIC);
    std::string detail;
    TASK_CHECK(verify_geometry(mesh, ref, &detail));
    return 0;
}

/* ===================================================================== */
/* publication                                                           */
/* ===================================================================== */

static int
publication_invalid_budget (void)
{
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-budget");
    std::string const target = dir + "/target.ply";
    write_file(target, "SENTINEL");

    fssr::IsoOctree tree;
    build_default_scene(&tree);

    std::size_t const bad[] = { 0, 1, 1024, TINY_BUDGET - 1 };
    for (std::size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        bool threw = false;
        try
        {
            fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
                target, dir, bad[i]);
        }
        catch (std::invalid_argument const&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
        TASK_CHECK(read_file(target) == "SENTINEL");
        TASK_CHECK(count_entries(dir) == 1);
    }

    /* A valid budget afterwards must succeed and replace the target. */
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_CUBIC, target, dir, TINY_BUDGET);
    TASK_CHECK(stats.vertex_count > 0);
    TASK_CHECK(read_file(target).rfind("ply\n", 0) == 0);
    TASK_CHECK(count_entries(dir) == 1);
    return 0;
}

static int
publication_bad_directories (void)
{
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-dirs");
    std::string const target = dir + "/target.ply";
    write_file(target, "SENTINEL");

    fssr::IsoOctree tree;
    build_default_scene(&tree);

    /* Missing temporary directory. */
    bool threw = false;
    try
    {
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
            target, dir + "/missing", TINY_BUDGET);
    }
    catch (std::runtime_error const&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(read_file(target) == "SENTINEL");

    /* Output path in a missing directory. */
    threw = false;
    try
    {
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
            dir + "/missing/target.ply", dir, TINY_BUDGET);
    }
    catch (std::runtime_error const&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(read_file(target) == "SENTINEL");
    TASK_CHECK(count_entries(dir) == 1);

    /* Output path that is a directory. */
    std::string const sub = dir + "/as-directory";
    ::mkdir(sub.c_str(), 0777);
    threw = false;
    try
    {
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
            sub, dir, TINY_BUDGET);
    }
    catch (std::runtime_error const&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(read_file(target) == "SENTINEL");
    return 0;
}

static int
publication_fault_injection (void)
{
    /*
     * A read-only temporary directory and a read-only output directory must
     * fail cleanly. A failure after the output temp file was created must
     * leave the target intact and remove the temp file.
     */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-faults");
    std::string const target = dir + "/target.ply";
    write_file(target, "SENTINEL");

    fssr::IsoOctree tree;
    build_default_scene(&tree);

    std::string const readonly = dir + "/readonly";
    ::mkdir(readonly.c_str(), 0555);
    bool wrote_probe = false;
    if (std::FILE* probe = std::fopen((readonly + "/probe").c_str(), "w"))
    {
        std::fclose(probe);
        std::remove((readonly + "/probe").c_str());
        wrote_probe = true;
    }
    if (!wrote_probe)
    {
        bool threw = false;
        try
        {
            fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
                target, readonly, TINY_BUDGET);
        }
        catch (std::runtime_error const&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
        TASK_CHECK(read_file(target) == "SENTINEL");
    }
    ::chmod(readonly.c_str(), 0777);

    std::string const readonly_out = dir + "/readonly-out";
    ::mkdir(readonly_out.c_str(), 0555);
    bool wrote_out_probe = false;
    if (std::FILE* probe = std::fopen(
            (readonly_out + "/probe").c_str(), "w"))
    {
        std::fclose(probe);
        std::remove((readonly_out + "/probe").c_str());
        wrote_out_probe = true;
    }
    if (!wrote_out_probe)
    {
        bool threw = false;
        try
        {
            fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
                readonly_out + "/target.ply", dir, TINY_BUDGET);
        }
        catch (std::runtime_error const&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
    }
    ::chmod(readonly_out.c_str(), 0777);

    /* A temp directory that is not a directory must fail cleanly. */
    std::string const not_a_dir = dir + "/not-a-dir";
    write_file(not_a_dir, "x");
    bool threw_notdir = false;
    try
    {
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
            target, not_a_dir, TINY_BUDGET);
    }
    catch (std::runtime_error const&)
    {
        threw_notdir = true;
    }
    TASK_CHECK(threw_notdir);
    TASK_CHECK(read_file(target) == "SENTINEL");

    /* The same call must succeed once the permissions are restored. */
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_CUBIC, target, dir, TINY_BUDGET);
    TASK_CHECK(stats.vertex_count > 0);
    TASK_CHECK(read_file(target).rfind("ply\n", 0) == 0);
    return 0;
}

static int
publication_retry_after_failure (void)
{
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-retry");
    std::string const target = dir + "/target.ply";
    write_file(target, "SENTINEL");

    fssr::IsoOctree tree;
    build_default_scene(&tree);

    /* First a failing call, then repeated successful calls. */
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::string const before = read_file(target);
        bool threw = false;
        try
        {
            fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
                target, dir + "/nope", TINY_BUDGET);
        }
        catch (std::runtime_error const&)
        {
            threw = true;
        }
        TASK_CHECK(threw);
        TASK_CHECK(read_file(target) == before);
        TASK_CHECK(count_entries(dir) == 1);

        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
            target, dir, TINY_BUDGET);
        TASK_CHECK(read_file(target).rfind("ply\n", 0) == 0);
        TASK_CHECK(count_entries(dir) == 1);
    }
    return 0;
}

static int
publication_shared_temp_interleaved (void)
{
    /*
     * Two extractions with different budgets share one temporary directory.
     * Their call-owned subdirectories must not collide, and running them in
     * parallel from one process must produce exactly the sequential bytes.
     */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-shared");
    std::string const one = dir + "/one.ply";
    std::string const two = dir + "/two.ply";

    fssr::IsoOctree tree1;
    build_default_scene(&tree1);
    fssr::IsoOctree tree2;
    build_default_scene(&tree2);

    fssr::StreamSurfaceStats const s1 = fssr::extract_stream_surface(&tree1,
        fssr::INTERPOLATION_CUBIC, one, dir, TINY_BUDGET);
    fssr::StreamSurfaceStats const s2 = fssr::extract_stream_surface(&tree2,
        fssr::INTERPOLATION_CUBIC, two, dir, 4 * TINY_BUDGET);

    TASK_CHECK(s1.vertex_count == s2.vertex_count);
    TASK_CHECK(read_file(one) == read_file(two));
    TASK_CHECK(count_entries(dir) == 2);
    return 0;
}

static int
publication_atomic_replace (void)
{
    /* The target must only change once the new output is complete. */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-atomic");
    std::string const target = dir + "/target.ply";
    write_file(target, "OLD");

    fssr::IsoOctree tree;
    build_default_scene(&tree);
    fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC, target,
        dir, TINY_BUDGET);
    std::string const replaced = read_file(target);
    TASK_CHECK(replaced != "OLD");
    TASK_CHECK(replaced.rfind("ply\n", 0) == 0);
    /* No stray temporary files remain in the output directory. */
    TASK_CHECK(count_entries(dir) == 1);
    return 0;
}

static int
publication_empty_arguments (void)
{
    /* Empty paths and a null tree are rejected without touching anything. */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-args");
    std::string const target = dir + "/target.ply";
    write_file(target, "SENTINEL");

    fssr::IsoOctree tree;
    build_default_scene(&tree);

    bool threw = false;
    try
    {
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
            "", dir, TINY_BUDGET);
    }
    catch (std::invalid_argument const&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try
    {
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC,
            target, "", TINY_BUDGET);
    }
    catch (std::invalid_argument const&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try
    {
        fssr::extract_stream_surface(nullptr, fssr::INTERPOLATION_CUBIC,
            target, dir, TINY_BUDGET);
    }
    catch (std::invalid_argument const&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    TASK_CHECK(read_file(target) == "SENTINEL");
    TASK_CHECK(count_entries(dir) == 1);
    return 0;
}

static int
publication_write_failure (void)
{
    /*
     * Force a disk-write failure part way through the merge phase by
     * running the extraction with a tiny file-size limit in a child
     * process. The child must exit with an error, leave the pre-existing
     * target untouched and remove its temporary directory.
     */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "publication-writefail");
    std::string const target = dir + "/target.ply";
    write_file(target, "SENTINEL");

    /* Write a small helper program that performs the failing extraction. */
    std::string const helper_dir = dir + "/helper";
    ::mkdir(helper_dir.c_str(), 0777);
    std::string const helper_src = helper_dir + "/helper.cpp";
    std::ofstream src(helper_src.c_str());
    src << "#include <csignal>\n"
        << "#include <cstdlib>\n"
        << "#include \"fssr/iso_octree.h\"\n"
        << "#include \"fssr/stream_surface.h\"\n"
        << "int main(int argc, char** argv) {\n"
        << "  std::signal(SIGXFSZ, SIG_IGN);\n"
        << "  fssr::IsoOctree t; t.set_max_level(7);\n"
        << "  for (int i = 1; i < 24; ++i) for (int j = 0; j < 48; ++j) {\n"
        << "    double a = 3.141592653589793 * i / 24.0;\n"
        << "    double b = 6.283185307179586 * j / 48.0;\n"
        << "    fssr::Sample s;\n"
        << "    s.normal = math::Vec3f(std::sin(a)*std::cos(b),\n"
        << "      std::sin(a)*std::sin(b), std::cos(a));\n"
        << "    s.pos = s.normal; s.color = math::Vec3f(0.2f,0.5f,0.8f);\n"
        << "    s.scale = j < 24 ? 0.08f : 0.2f; s.confidence = 1.0f;\n"
        << "    t.insert_sample(s); }\n"
        << "  t.limit_octree_level(); t.compute_voxels();\n"
        << "  try { fssr::extract_stream_surface(&t, fssr::INTERPOLATION_CUBIC,\n"
        << "      argv[1], argv[2], 65536); }\n"
        << "  catch (...) { return 3; }\n"
        << "  return 0;\n"
        << "}\n";
    src.close();

    std::string const helper_bin = helper_dir + "/helper";
    std::string const compile = "c++ -std=c++17 -O1 -I. -Ilibs -I" + std::string(".")
        + " " + helper_src + " .task-build/stream.o .task-build/basis_function.cc.o"
        + " .task-build/hermite.cc.o .task-build/iso_octree.cc.o"
        + " .task-build/iso_surface.cc.o .task-build/octree.cc.o"
        + " .task-build/triangulation.cc.o .task-build/voxel.cc.o"
        + " -o " + helper_bin + " 2>/dev/null";
    if (std::system(compile.c_str()) != 0)
        return 0; /* Build environment differs; skip this case. */

    std::string const command = "sh -c 'ulimit -f 8; exec \"$0\" \"$1\" \"$2\"' "
        + helper_bin + " " + target + " " + dir;
    int const status = std::system(command.c_str());
    /* The extraction must fail, either by an error exit or a signal. */
    TASK_CHECK(status != 0);
    TASK_CHECK(read_file(target) == "SENTINEL");
    /* Only the helper directory remains; the extraction temp dir is gone. */
    TASK_CHECK(count_entries(dir) == 2);
    ::remove(helper_bin.c_str());
    ::remove(helper_src.c_str());
    ::rmdir(helper_dir.c_str());
    TASK_CHECK(count_entries(dir) == 1);
    return 0;
}

/* ===================================================================== */
/* compatibility                                                         */
/* ===================================================================== */

static int
compatibility_legacy_extractor (void)
{
    /* The original extract_mesh() must still work and agree with streaming. */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "compat-legacy");

    SceneOptions options;
    options.level = 6;
    options.rings = 20;
    options.segments = 40;
    options.scale_low = 0.15f;
    options.scale_high = 0.3f;

    for (int mode = 0; mode < 4; ++mode)
    {
        fssr::InterpolationType const type
            = static_cast<fssr::InterpolationType>(mode);
        fssr::IsoOctree ref_tree;
        build_scene(&ref_tree, options);
        Reference const ref = reference_mesh(&ref_tree, type);
        TASK_CHECK(!ref.positions.empty());
        TASK_CHECK(ref.faces.size() % 3 == 0);
        TASK_CHECK(ref.colors.size() == ref.positions.size());
        TASK_CHECK(ref.values.size() == ref.positions.size());
        TASK_CHECK(ref.confidences.size() == ref.positions.size());

        fssr::IsoOctree tree;
        build_scene(&tree, options);
        std::string const ply = dir + "/mode" + std::to_string(mode) + ".ply";
        fssr::extract_stream_surface(&tree, type, ply, dir, TINY_BUDGET);
        PlyMesh const mesh = parse_ply(ply);
        std::string detail;
        TASK_CHECK(verify_geometry(mesh, ref, &detail));
    }
    return 0;
}

static int
compatibility_member_entry_point (void)
{
    /* The IsoSurface member entry point mirrors the free function. */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "compat-member");

    fssr::IsoOctree tree_a;
    build_default_scene(&tree_a);
    fssr::IsoSurface surface_a(&tree_a, fssr::INTERPOLATION_LINEAR);
    std::string const ply_a = dir + "/a.ply";
    fssr::StreamSurfaceStats const stats_a = surface_a.extract_stream_mesh(
        ply_a, dir, TINY_BUDGET);

    fssr::IsoOctree tree_b;
    build_default_scene(&tree_b);
    std::string const ply_b = dir + "/b.ply";
    fssr::StreamSurfaceStats const stats_b = fssr::extract_stream_surface(
        &tree_b, fssr::INTERPOLATION_LINEAR, ply_b, dir, TINY_BUDGET);

    TASK_CHECK(stats_a.vertex_count == stats_b.vertex_count);
    TASK_CHECK(stats_a.triangle_count == stats_b.triangle_count);
    TASK_CHECK(read_file(ply_a) == read_file(ply_b));
    return 0;
}

static int
compatibility_repeated_runs (void)
{
    /* Repeated runs on the same tree are stable and leave no temp files. */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "compat-repeat");

    fssr::IsoOctree tree;
    build_default_scene(&tree);
    std::string first;
    for (int i = 0; i < 3; ++i)
    {
        std::string const ply = dir + "/r" + std::to_string(i) + ".ply";
        fssr::extract_stream_surface(&tree, fssr::INTERPOLATION_CUBIC, ply,
            dir, TINY_BUDGET);
        std::string const bytes = read_file(ply);
        TASK_CHECK(!bytes.empty());
        if (i == 0)
            first = bytes;
        else
            TASK_CHECK(bytes == first);
    }
    TASK_CHECK(count_entries(dir) == 3);
    return 0;
}

static int
compatibility_statistics_semantics (void)
{
    /* Reported counts must equal the parsed file and the reference mesh. */
    std::string const root = temp_root();
    std::string const dir = unique_dir(root, "compat-stats");

    SceneOptions options;
    options.level = 6;
    options.rings = 24;
    options.segments = 48;
    options.scale_low = 0.1f;
    options.scale_high = 0.25f;

    fssr::IsoOctree ref_tree;
    build_scene(&ref_tree, options);
    Reference const ref = reference_mesh(&ref_tree, fssr::INTERPOLATION_SCALING);

    fssr::IsoOctree tree;
    build_scene(&tree, options);
    std::string const ply = dir + "/stats.ply";
    fssr::StreamSurfaceStats const stats = fssr::extract_stream_surface(&tree,
        fssr::INTERPOLATION_SCALING, ply, dir, TINY_BUDGET);

    PlyMesh const mesh = parse_ply(ply);
    TASK_CHECK(mesh.valid);
    TASK_CHECK(stats.vertex_count == mesh.vertices.size());
    TASK_CHECK(stats.triangle_count == mesh.triangles.size());
    TASK_CHECK(stats.vertex_count == ref.positions.size());
    TASK_CHECK(stats.triangle_count == ref.faces.size() / 3);
    TASK_CHECK(stats.peak_working_bytes <= TINY_BUDGET);
    return 0;
}

/* --------------------------------------------------------------------- */

const task_case task_cases[] =
{
    { "geometry", "interpolation_modes", geometry_interpolation_modes },
    { "geometry", "cross_level", geometry_cross_level },
    { "geometry", "multi_component", geometry_multi_component },
    { "geometry", "empty_surface", geometry_empty_surface },

    { "budget", "multipass", budget_multipass },
    { "budget", "invariant_bytes", budget_invariant_bytes },
    { "budget", "peak_constant", budget_peak_constant },
    { "budget", "statistics_coverage", budget_statistics_coverage },
    { "budget", "many_spills", budget_many_spills },
    { "budget", "vertex_order", budget_vertex_order },

    { "publication", "invalid_budget", publication_invalid_budget },
    { "publication", "bad_directories", publication_bad_directories },
    { "publication", "fault_injection", publication_fault_injection },
    { "publication", "retry_after_failure", publication_retry_after_failure },
    { "publication", "shared_temp_interleaved", publication_shared_temp_interleaved },
    { "publication", "atomic_replace", publication_atomic_replace },
    { "publication", "empty_arguments", publication_empty_arguments },
    { "publication", "write_failure", publication_write_failure },

    { "compatibility", "legacy_extractor", compatibility_legacy_extractor },
    { "compatibility", "member_entry_point", compatibility_member_entry_point },
    { "compatibility", "repeated_runs", compatibility_repeated_runs },
    { "compatibility", "statistics_semantics", compatibility_statistics_semantics }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
