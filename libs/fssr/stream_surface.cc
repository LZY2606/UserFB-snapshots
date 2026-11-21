/*
 * Copyright (C) 2015, Simon Fuhrmann
 * TU Darmstadt - Graphics, Capture and Massively Parallel Computing
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 *
 * Out-of-core isosurface extraction. See fssr/stream_surface.h for the
 * public contract, memory accounting and exception guarantees.
 *
 * Pipeline
 * --------
 *   Emit. Marching Cubes indices are computed in place on the resident
 *   octree; the tree and voxel array stay allocated. A leaf traversal
 *   produces one record per polygon slot:
 *
 *       (edge key, slot ordinal, vertex payload)
 *
 *   The slot ordinal is the dense position of the vertex in the leaf /
 *   polygon / slot traversal order. Records are buffered up to a
 *   budget-derived size, sorted by edge key and spilled to call-owned run
 *   files.
 *
 *   Merge. Runs are merged with bounded fan-in into one stream sorted by
 *   edge key (ties broken by slot ordinal).
 *
 *   Vertex IDs and slot table. The sorted stream is read once. Each new
 *   edge key becomes a vertex; IDs increase with the edge key. The
 *   deduplicated vertex table is buffered to disk. Each record also writes
 *   (slot ordinal -> vertex id) into a disk-backed slot table. Slot
 *   ordinals are dense, so the table is exactly slot_count * 4 bytes and a
 *   small page cache suffices.
 *
 *   Output. The PLY header and vertex table are written to a sibling temp
 *   file. A second leaf traversal rebuilds each polygon in emission order,
 *   reads its IDs from the slot table, runs the reference
 *   MinAreaTriangulation and appends the triangles. A p-gon always yields
 *   p - 2 triangles, so the header's face count is exact without a second
 *   triangulating pass.
 *
 * Working set
 * -----------
 * Every buffer used by the extraction is a TrackedAllocator vector, so the
 * live byte count is exact for those allocations; fixed-size per-leaf
 * scratch in the shared geometry code and I/O bookkeeping are charged as a
 * fixed reserve. All buffer capacities are derived from the budget and are
 * released between phases. The only super-constant term is the O(p^2)
 * table of MinAreaTriangulation for the largest single polygon, which is
 * accounted separately and added to the reported peak.
 *
 * Temporary files live in a call-owned directory and are removed on
 * success and on failure. The target is published by an atomic rename.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "math/geometry.h"
#include "fssr/iso_surface.h"
#include "fssr/memory.h"
#include "fssr/stream_surface.h"
#include "fssr/triangulation.h"
#include "fssr/voxel.h"

FSSR_NAMESPACE_BEGIN

/* --------------------------------------------------------------------- */
/* Allocation tracking (see fssr/memory.h).                              */
/* --------------------------------------------------------------------- */

namespace detail
{
    std::size_t tracked_live_bytes = 0;
    std::size_t tracked_peak_bytes = 0;
    bool tracked_enabled = false;
}

namespace
{
    namespace fs = std::filesystem;

    std::size_t const MIN_BUDGET = 64 * 1024;

    /*
     * Serialized record: 2 x uint64 edge key (16), uint32 slot ordinal (4),
     * uint32 leaf ordinal (4), 9 floats payload (36) = 60 bytes.
     */
    std::size_t const RECORD_BYTES = 60;
    std::size_t const SLOT_BYTES = 4;
    std::size_t const VERTEX_BYTES = 32;
    std::size_t const MAX_MERGE_FAN_IN = 32;
    std::size_t const IO_BUFFER_BYTES = 4096;
    /*
     * Fixed reserve charged against the budget. It covers the transient
     * containers used by the shared geometry helpers for one leaf node
     * (the iso-edge list, twin edges, the vertex-valence map and the
     * polygon loop, each bounded by the constant number of cube edges, so
     * at most ~16 KiB), plus path strings, file-system objects and other
     * bookkeeping (~8 KiB). It does not depend on the total output size.
     */
    std::size_t const LEAF_SCRATCH_BYTES = 16 * 1024;
    std::size_t const BOOKKEEPING_BYTES = LEAF_SCRATCH_BYTES + 8 * 1024;

    template <typename T>
    using TVector = std::vector<T, TrackedAllocator<T> >;

    /** Charges a transient allocation to the tracker for its lifetime. */
    struct ScopedCharge
    {
        explicit ScopedCharge (std::size_t bytes) : bytes(bytes)
        { detail::track_allocate(this->bytes); }
        ~ScopedCharge (void) { detail::track_deallocate(this->bytes); }
        std::size_t bytes;
    };

    template <typename T>
    void release (TVector<T>* vec) { TVector<T>().swap(*vec); }

    void
    throw_io (char const* what, std::string const& path)
    {
        throw std::runtime_error(std::string("extract_stream_surface(): ")
            + what + ": " + path);
    }

    /** A FILE* with an explicitly sized, tracked I/O buffer. */
    class BufferedFile
    {
    public:
        BufferedFile (void) : file(nullptr) { }
        ~BufferedFile (void) { this->close(); }

        bool open (std::string const& path, char const* mode)
        {
            this->close();
            this->file = std::fopen(path.c_str(), mode);
            if (this->file == nullptr)
                return false;
            this->buffer.resize(IO_BUFFER_BYTES);
            if (std::setvbuf(this->file, this->buffer.data(), _IOFBF,
                    this->buffer.size()) != 0)
            {
                this->close();
                return false;
            }
            return true;
        }

        void close (void)
        {
            if (this->file != nullptr)
            {
                std::fclose(this->file);
                this->file = nullptr;
            }
            release(&this->buffer);
        }

        bool good (void) const { return this->file != nullptr; }
        std::FILE* get (void) const { return this->file; }

    private:
        BufferedFile (BufferedFile const&);
        BufferedFile& operator= (BufferedFile const&);

        std::FILE* file;
        TVector<char> buffer;
    };

    /** A readable file that is removed when it goes out of scope. */
    class UniqueFile : public BufferedFile
    {
    public:
        UniqueFile (void) : has_path(false) { }
        ~UniqueFile (void)
        {
            this->close();
            if (this->has_path)
                std::remove(this->path.c_str());
        }
        bool open (std::string const& p, char const* mode)
        {
            this->path = p;
            this->has_path = true;
            return BufferedFile::open(p, mode);
        }
    private:
        std::string path;
        bool has_path;
    };

    /** Little-endian serialization helpers (host independent). */
    bool write_bytes (std::FILE* f, void const* data, std::size_t len)
    {
        return std::fwrite(data, 1, len, f) == len;
    }
    bool read_bytes (std::FILE* f, void* data, std::size_t len)
    {
        return std::fread(data, 1, len, f) == len;
    }

    void put_u32 (unsigned char* b, uint32_t v)
    {
        b[0] = static_cast<unsigned char>(v & 0xff);
        b[1] = static_cast<unsigned char>((v >> 8) & 0xff);
        b[2] = static_cast<unsigned char>((v >> 16) & 0xff);
        b[3] = static_cast<unsigned char>((v >> 24) & 0xff);
    }
    uint32_t get_u32 (unsigned char const* b)
    {
        return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8)
            | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    }
    void put_u64 (unsigned char* b, uint64_t v)
    {
        put_u32(b, static_cast<uint32_t>(v & 0xffffffffu));
        put_u32(b + 4, static_cast<uint32_t>(v >> 32));
    }
    uint64_t get_u64 (unsigned char const* b)
    {
        return static_cast<uint64_t>(get_u32(b))
            | (static_cast<uint64_t>(get_u32(b + 4)) << 32);
    }
    void put_f32 (unsigned char* b, float v)
    {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        put_u32(b, bits);
    }
    float get_f32 (unsigned char const* b)
    {
        uint32_t const bits = get_u32(b);
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }

    uint8_t
    color_to_uchar (float value)
    {
        float const scaled = std::min(255.0f, std::max(0.0f, value * 255.0f));
        return static_cast<uint8_t>(scaled + 0.5f);
    }
}

/* --------------------------------------------------------------------- */

/** One isovertex record, buffered in memory and spilled to run files. */
struct StreamVertexRecord
{
    uint64_t edge_low;
    uint64_t edge_high;
    uint32_t leaf_ordinal;
    uint32_t slot_ordinal;
    float x, y, z;
    float r, g, b;
    float value;
    float confidence;
};

static void
encode_record (StreamVertexRecord const& r, unsigned char* b)
{
    put_u64(b + 0, r.edge_low);
    put_u64(b + 8, r.edge_high);
    put_u32(b + 16, r.leaf_ordinal);
    put_u32(b + 20, r.slot_ordinal);
    put_f32(b + 24, r.x); put_f32(b + 28, r.y); put_f32(b + 32, r.z);
    put_f32(b + 36, r.r); put_f32(b + 40, r.g); put_f32(b + 44, r.b);
    put_f32(b + 48, r.value); put_f32(b + 52, r.confidence);
}

static bool
decode_record (unsigned char const* b, StreamVertexRecord* r)
{
    r->edge_low = get_u64(b + 0);
    r->edge_high = get_u64(b + 8);
    r->leaf_ordinal = get_u32(b + 16);
    r->slot_ordinal = get_u32(b + 20);
    r->x = get_f32(b + 24); r->y = get_f32(b + 28); r->z = get_f32(b + 32);
    r->r = get_f32(b + 36); r->g = get_f32(b + 40); r->b = get_f32(b + 44);
    r->value = get_f32(b + 48); r->confidence = get_f32(b + 52);
    return true;
}

static bool
record_less (StreamVertexRecord const& a, StreamVertexRecord const& b)
{
    if (a.edge_low != b.edge_low) return a.edge_low < b.edge_low;
    if (a.edge_high != b.edge_high) return a.edge_high < b.edge_high;
    return a.slot_ordinal < b.slot_ordinal;
}

/* --------------------------------------------------------------------- */

/* The serialized record must stay in sync with encode/decode below. */
static_assert(sizeof(StreamVertexRecord) == 56,
    "StreamVertexRecord layout changed; update RECORD_BYTES");

class StreamSurfaceImpl
{
public:
    StreamSurfaceImpl (IsoOctree* octree, InterpolationType type,
        std::string const& ply, std::string const& temp, std::size_t budget);
    ~StreamSurfaceImpl (void);

    StreamSurfaceStats run (void);

private:
    void prepare_temp_dir (void);
    void cleanup (void) noexcept;

    std::size_t run_capacity (void) const;
    std::size_t merge_fan_in (void) const;
    std::size_t page_slots (void) const;
    std::size_t page_vertices (void) const;
    std::size_t vertex_chunk (void) const;

    void emit_records (void);
    void flush_run (void);
    void add_run (std::size_t level, std::string const& path);
    void merge_records (void);
    void merge_group (std::vector<std::string> const& paths,
        std::size_t begin, std::size_t end, std::string const& out_path);
    void assign_vertex_ids_and_slots (void);
    void write_output (void);

    void traverse (int mode);
    void handle_emit_polygon (IsoSurface::Ingredient const* verts,
        std::size_t num_verts);
    void handle_output_polygon (IsoSurface::Ingredient const* verts,
        std::size_t num_verts);
    static void trampoline (void* context,
        IsoSurface::Ingredient const* verts, std::size_t num_verts);

    void read_slot (std::size_t slot, uint32_t* id);
    void read_vertex (uint32_t id, float out[8]);
    void write_vertex_buffer (TVector<StreamVertexRecord>* buffer,
        BufferedFile* out);

private:
    IsoOctree* octree;
    IsoSurface surface;
    std::string ply_filename;
    std::string temp_directory;
    std::size_t budget;

    std::string call_dir;
    bool call_dir_created;

    /* Emit / merge. */
    TVector<StreamVertexRecord> run_buffer;
    std::size_t run_limit;
    std::size_t emit_slot;
    std::size_t slot_count;
    /*
     * One sorted run per level; when a level fills, its runs are merged and
     * pushed to the next level. The number of live runs is therefore
     * O(log N) and independent of the fan-in, so the in-memory bookkeeping
     * does not grow with the output size.
     */
    std::vector<std::string> level_runs[2];
    std::size_t merge_level;
    std::size_t run_sequence;
    std::string sorted_path;

    /* Disk tables. */
    std::string slot_path;
    std::string vertex_path;
    BufferedFile slot_table;
    BufferedFile vertex_table;
    TVector<uint32_t> slot_page;
    std::size_t slot_page_begin;
    bool slot_page_valid;
    TVector<float> vertex_page;
    std::size_t vertex_page_begin;
    std::size_t vertex_page_count;

    /* Output. */
    BufferedFile output_file;
    std::string output_temp_path;
    bool output_open;
    uint32_t triangle_count;

    /* Traversal state. */
    int traversal_mode;
    std::size_t output_slot;

    /* Per-polygon scratch. */
    TVector<uint32_t> polygon_ids;
    TVector<math::Vec3f> polygon_positions;

    /* Statistics. */
    StreamSurfaceStats stats;
    std::size_t max_polygon_vertices;
    std::size_t peak_working_bytes;
    std::size_t temp_read_bytes;
    std::size_t temp_write_bytes;
};

/* --------------------------------------------------------------------- */

StreamSurfaceStats::StreamSurfaceStats (void)
    : peak_working_bytes(0)
    , temp_read_bytes(0)
    , temp_write_bytes(0)
    , vertex_count(0)
    , triangle_count(0)
    , merge_passes(0)
    , max_polygon_vertices(0)
    , triangulation_workspace_bytes(0)
{
}

StreamSurfaceImpl::StreamSurfaceImpl (IsoOctree* tree, InterpolationType type,
    std::string const& ply, std::string const& temp, std::size_t budget_bytes)
    : octree(tree)
    , surface(tree, type)
    , ply_filename(ply)
    , temp_directory(temp)
    , budget(budget_bytes)
    , call_dir_created(false)
    , run_limit(0)
    , emit_slot(0)
    , slot_count(0)
    , merge_level(0)
    , run_sequence(0)
    , slot_page_begin(0)
    , slot_page_valid(false)
    , vertex_page_begin(0)
    , vertex_page_count(0)
    , output_open(false)
    , triangle_count(0)
    , traversal_mode(0)
    , output_slot(0)
    , max_polygon_vertices(0)
    , peak_working_bytes(0)
    , temp_read_bytes(0)
    , temp_write_bytes(0)
{
}

StreamSurfaceImpl::~StreamSurfaceImpl (void)
{
    this->cleanup();
}

/* --------------------------------------------------------------------- */

std::size_t
StreamSurfaceImpl::run_capacity (void) const
{
    std::size_t const usable = this->budget > BOOKKEEPING_BYTES
        ? this->budget - BOOKKEEPING_BYTES : this->budget / 2;
    /* Half of the usable budget for the in-memory run buffer. */
    return std::max<std::size_t>(1, usable / (2 * RECORD_BYTES));
}

std::size_t
StreamSurfaceImpl::merge_fan_in (void) const
{
    std::size_t const usable = this->budget > BOOKKEEPING_BYTES
        ? this->budget - BOOKKEEPING_BYTES : this->budget / 2;
    /* Each open run keeps one I/O buffer plus one record. */
    std::size_t const fits = (usable / 4) / (IO_BUFFER_BYTES + RECORD_BYTES);
    return std::max<std::size_t>(2, std::min<std::size_t>(MAX_MERGE_FAN_IN, fits));
}

std::size_t
StreamSurfaceImpl::page_slots (void) const
{
    std::size_t const usable = this->budget > BOOKKEEPING_BYTES
        ? this->budget - BOOKKEEPING_BYTES : this->budget / 2;
    return std::max<std::size_t>(1,
        std::min<std::size_t>(4096, usable / (16 * SLOT_BYTES)));
}

std::size_t
StreamSurfaceImpl::page_vertices (void) const
{
    std::size_t const usable = this->budget > BOOKKEEPING_BYTES
        ? this->budget - BOOKKEEPING_BYTES : this->budget / 2;
    return std::max<std::size_t>(1,
        std::min<std::size_t>(4096, usable / (16 * VERTEX_BYTES)));
}

std::size_t
StreamSurfaceImpl::vertex_chunk (void) const
{
    std::size_t const usable = this->budget > BOOKKEEPING_BYTES
        ? this->budget - BOOKKEEPING_BYTES : this->budget / 2;
    return std::max<std::size_t>(1,
        std::min<std::size_t>(4096, usable / (16 * VERTEX_BYTES)));
}

/* --------------------------------------------------------------------- */

void
StreamSurfaceImpl::prepare_temp_dir (void)
{
    std::error_code ec;
    fs::path const base(this->temp_directory);
    if (!fs::exists(base, ec) || ec)
        throw std::runtime_error(
            "extract_stream_surface(): Temporary directory does not exist: "
            + this->temp_directory);
    if (!fs::is_directory(base, ec) || ec)
        throw std::runtime_error(
            "extract_stream_surface(): Temporary path is not a directory: "
            + this->temp_directory);

    static std::atomic<unsigned long> counter(0);
    for (int attempt = 0; attempt < 10000; ++attempt)
    {
        unsigned long const id = counter.fetch_add(1) + 1;
        std::string const name = "fssr-stream-"
            + std::to_string(static_cast<unsigned long>(::getpid())) + "-"
            + std::to_string(id);
        fs::path const candidate = base / name;
        ec.clear();
        if (fs::create_directory(candidate, ec))
        {
            this->call_dir = candidate.string();
            this->call_dir_created = true;
            return;
        }
        if (ec)
            throw std::runtime_error(
                "extract_stream_surface(): Cannot create a temporary directory in "
                + this->temp_directory + ": " + ec.message());
    }
    throw std::runtime_error(
        "extract_stream_surface(): Cannot allocate a unique temporary directory");
}

void
StreamSurfaceImpl::cleanup (void) noexcept
{
    this->slot_table.close();
    this->vertex_table.close();
    if (this->output_open)
    {
        this->output_file.close();
        this->output_open = false;
    }
    if (!this->output_temp_path.empty())
    {
        std::error_code ec;
        fs::remove(this->output_temp_path, ec);
    }
    if (this->call_dir_created && !this->call_dir.empty())
    {
        std::error_code ec;
        fs::remove_all(this->call_dir, ec);
    }
    this->call_dir_created = false;
}

/* --------------------------------------------------------------------- */

StreamSurfaceStats
StreamSurfaceImpl::run (void)
{
    if (this->octree == nullptr)
        throw std::invalid_argument("extract_stream_surface(): Null octree");
    if (this->ply_filename.empty())
        throw std::invalid_argument("extract_stream_surface(): Empty output file name");
    if (this->temp_directory.empty())
        throw std::invalid_argument("extract_stream_surface(): Empty temporary directory");
    if (this->budget < MIN_BUDGET)
        throw std::invalid_argument(
            "extract_stream_surface(): Working budget must be at least 65536 bytes");

    /* Validate the output location before creating any temporary files. */
    {
        std::error_code ec;
        fs::path const target(this->ply_filename);
        fs::path const parent = target.parent_path().empty()
            ? fs::path(".") : target.parent_path();
        if (!fs::is_directory(parent, ec) || ec)
            throw std::runtime_error(
                "extract_stream_surface(): Output directory does not exist or is "
                "not a directory: " + parent.string());
        if (fs::exists(target, ec) && !fs::is_regular_file(target, ec))
            throw std::runtime_error(
                "extract_stream_surface(): Output path is not a regular file: "
                + this->ply_filename);
    }

    detail::tracking_reset();
    detail::track_allocate(BOOKKEEPING_BYTES);
    this->run_limit = this->run_capacity();

    try
    {
        this->prepare_temp_dir();
        this->surface.sanity_checks();
        /* An octree without a root has no surface; emit a valid empty PLY. */
        if (this->octree->get_root_node() != nullptr)
            this->surface.compute_mc_indices();

        this->emit_records();
        this->merge_records();
        this->assign_vertex_ids_and_slots();
        this->write_output();

        /* Publish atomically. */
        std::error_code ec;
        fs::rename(this->output_temp_path, this->ply_filename, ec);
        if (ec)
            throw std::runtime_error(
                "extract_stream_surface(): Cannot publish output file "
                + this->ply_filename + ": " + ec.message());
        this->output_temp_path.clear();
        this->cleanup();
    }
    catch (...)
    {
        this->cleanup();
        detail::track_deallocate(BOOKKEEPING_BYTES);
        detail::tracking_disable();
        throw;
    }
    detail::track_deallocate(BOOKKEEPING_BYTES);
    detail::tracking_disable();

    this->peak_working_bytes = detail::tracked_peak_bytes;
    this->stats.peak_working_bytes = this->peak_working_bytes;
    this->stats.temp_read_bytes = this->temp_read_bytes;
    this->stats.temp_write_bytes = this->temp_write_bytes;
    this->stats.max_polygon_vertices = this->max_polygon_vertices;
    this->stats.triangulation_workspace_bytes = this->max_polygon_vertices
        * this->max_polygon_vertices * (sizeof(float) + sizeof(int));
    this->stats.triangle_count = this->triangle_count;
    return this->stats;
}

/* --------------------------------------------------------------------- */

void
StreamSurfaceImpl::trampoline (void* context,
    IsoSurface::Ingredient const* verts, std::size_t num_verts)
{
    StreamSurfaceImpl* self = static_cast<StreamSurfaceImpl*>(context);
    if (self->traversal_mode == 1)
        self->handle_emit_polygon(verts, num_verts);
    else
        self->handle_output_polygon(verts, num_verts);
}

void
StreamSurfaceImpl::traverse (int mode)
{
    this->traversal_mode = mode;
    if (this->octree->get_root_node() == nullptr)
    {
        this->traversal_mode = 0;
        return;
    }
    Octree::Iterator iter = this->octree->get_iterator_for_root();
    for (iter.first_leaf(); iter.current != nullptr; iter.next_leaf())
        this->surface.visit_leaf_isopolygons(iter,
            &StreamSurfaceImpl::trampoline, this);
    this->traversal_mode = 0;
}

/* --------------------------------------------------------------------- */

void
StreamSurfaceImpl::emit_records (void)
{
    this->run_buffer.clear();
    this->run_buffer.reserve(this->run_limit);
    this->emit_slot = 0;
    this->slot_count = 0;
    this->level_runs[0].clear();
    this->level_runs[1].clear();
    this->merge_level = 0;
    this->run_sequence = 0;

    this->traverse(1);
    if (!this->run_buffer.empty())
        this->flush_run();

    this->slot_count = this->emit_slot;
    release(&this->run_buffer);
}

void
StreamSurfaceImpl::handle_emit_polygon (IsoSurface::Ingredient const* verts,
    std::size_t num_verts)
{
    for (std::size_t s = 0; s < num_verts; ++s)
    {
        IsoSurface::IsoVertex iso;
        this->surface.get_isovertex(verts[s].edge, verts[s].edge_id, &iso);

        StreamVertexRecord rec;
        rec.edge_low = verts[s].edge.first;
        rec.edge_high = verts[s].edge.second;
        rec.leaf_ordinal = 0;
        rec.slot_ordinal = static_cast<uint32_t>(this->emit_slot);
        rec.x = iso.pos[0]; rec.y = iso.pos[1]; rec.z = iso.pos[2];
        rec.r = iso.data.color[0];
        rec.g = iso.data.color[1];
        rec.b = iso.data.color[2];
        rec.value = iso.data.scale;
        rec.confidence = iso.data.conf;
        this->run_buffer.push_back(rec);
        this->emit_slot += 1;

        if (this->run_buffer.size() >= this->run_limit)
            this->flush_run();
    }
    this->max_polygon_vertices = std::max(this->max_polygon_vertices, num_verts);
}

void
StreamSurfaceImpl::flush_run (void)
{
    if (this->run_buffer.empty())
        return;

    std::sort(this->run_buffer.begin(), this->run_buffer.end(), record_less);

    std::string const path = this->call_dir + "/run-"
        + std::to_string(this->run_sequence++) + ".bin";
    BufferedFile out;
    if (!out.open(path, "wb"))
        throw_io("Cannot write temporary run file", path);
    unsigned char bytes[RECORD_BYTES];
    for (std::size_t i = 0; i < this->run_buffer.size(); ++i)
    {
        encode_record(this->run_buffer[i], bytes);
        if (!write_bytes(out.get(), bytes, RECORD_BYTES))
            throw_io("I/O error while writing temporary run file", path);
    }
    if (std::fflush(out.get()) != 0)
        throw_io("I/O error while flushing temporary run file", path);

    this->temp_write_bytes += this->run_buffer.size() * RECORD_BYTES;
    this->run_buffer.clear();
    this->add_run(this->merge_level, path);
}

/* --------------------------------------------------------------------- */

void
StreamSurfaceImpl::add_run (std::size_t level, std::string const& first_path)
{
    /*
     * Append a sorted run to a level. When the level reaches the fan-in,
     * merge its runs into one and push it to the next level. There are at
     * most two active levels at any time and at most 2 * fan-in paths, so
     * the bookkeeping is O(1) in the output size.
     */
    std::size_t const fan_in = this->merge_fan_in();
    std::string path = first_path;
    while (true)
    {
        std::size_t const slot = level % 2;
        this->level_runs[slot].push_back(path);
        if (this->level_runs[slot].size() < fan_in)
            return;

        std::vector<std::string> group;
        group.swap(this->level_runs[slot]);
        std::string const merged = this->call_dir + "/merge-"
            + std::to_string(this->merge_level) + "-"
            + std::to_string(this->run_sequence++) + ".bin";
        this->stats.merge_passes += 1;
        this->merge_group(group, 0, group.size(), merged);
        for (std::size_t i = 0; i < group.size(); ++i)
        {
            std::error_code ec;
            std::uintmax_t const size = fs::file_size(group[i], ec);
            if (!ec)
                this->temp_read_bytes += static_cast<std::size_t>(size);
            std::remove(group[i].c_str());
        }
        this->merge_level += 1;
        level += 1;
        path = merged;
    }
}

void
StreamSurfaceImpl::merge_records (void)
{
    std::vector<std::string> all;
    for (std::size_t i = 0; i < 2; ++i)
        for (std::size_t j = 0; j < this->level_runs[i].size(); ++j)
            all.push_back(this->level_runs[i][j]);
    for (std::size_t i = 0; i < 2; ++i)
        this->level_runs[i].clear();

    if (all.empty())
    {
        this->sorted_path = this->call_dir + "/sorted.bin";
        BufferedFile out;
        if (!out.open(this->sorted_path, "wb"))
            throw_io("Cannot create empty sorted run", this->sorted_path);
        return;
    }

    /* Merge the remaining runs, at most one final fan-in group per round. */
    std::string const merged = this->call_dir + "/sorted.bin";
    if (all.size() == 1)
    {
        std::error_code ec;
        fs::rename(all[0], merged, ec);
        if (ec)
        {
            fs::copy_file(all[0], merged, fs::copy_options::overwrite_existing, ec);
            if (ec)
                throw_io("Cannot create sorted run", merged);
            std::remove(all[0].c_str());
        }
    }
    else
    {
        /* `all` holds at most 2*fan_in runs; merge in fan-in groups first. */
        std::size_t const fan_in = this->merge_fan_in();
        std::vector<std::string> current = all;
        while (current.size() > 1)
        {
            std::vector<std::string> next;
            for (std::size_t i = 0; i < current.size(); i += fan_in)
            {
                std::size_t const end = std::min(current.size(), i + fan_in);
                if (end - i == 1)
                {
                    next.push_back(current[i]);
                    continue;
                }
                std::string const out_path = this->call_dir + "/final-"
                    + std::to_string(this->run_sequence++) + ".bin";
                this->stats.merge_passes += 1;
                this->merge_group(current, i, end, out_path);
                next.push_back(out_path);
            }
            for (std::size_t i = 0; i < current.size(); ++i)
            {
                if (std::find(next.begin(), next.end(), current[i]) == next.end())
                {
                    std::error_code ec;
                    std::uintmax_t const size = fs::file_size(current[i], ec);
                    if (!ec)
                        this->temp_read_bytes += static_cast<std::size_t>(size);
                    std::remove(current[i].c_str());
                }
            }
            current.swap(next);
        }
        std::error_code ec;
        fs::rename(current[0], merged, ec);
        if (ec)
        {
            fs::copy_file(current[0], merged, fs::copy_options::overwrite_existing, ec);
            if (ec)
                throw_io("Cannot create sorted run", merged);
            std::remove(current[0].c_str());
        }
    }
    this->sorted_path = merged;
}

void
StreamSurfaceImpl::merge_group (std::vector<std::string> const& paths,
    std::size_t begin, std::size_t end, std::string const& out_path)
{
    std::size_t const count = end - begin;
    std::vector<UniqueFile> inputs(count);
    std::vector<std::size_t> open;
    std::vector<StreamVertexRecord> current(count);
    std::vector<bool> valid(count, false);

    for (std::size_t i = begin; i < end; ++i)
    {
        if (!inputs[i - begin].open(paths[i], "rb"))
            throw_io("Cannot read temporary run file", paths[i]);
        unsigned char bytes[RECORD_BYTES];
        if (read_bytes(inputs[i - begin].get(), bytes, RECORD_BYTES))
        {
            decode_record(bytes, &current[i - begin]);
            valid[i - begin] = true;
            open.push_back(i - begin);
        }
    }

    ScopedCharge charge(count * (sizeof(StreamVertexRecord) + sizeof(bool))
        + open.capacity() * sizeof(std::size_t) + IO_BUFFER_BYTES);

    BufferedFile out;
    if (!out.open(out_path, "wb"))
        throw_io("Cannot write merged run file", out_path);

    std::size_t read_bytes_count = 0, write_bytes_count = 0;
    unsigned char bytes[RECORD_BYTES];
    while (!open.empty())
    {
        std::size_t best = 0;
        for (std::size_t i = 1; i < open.size(); ++i)
            if (record_less(current[open[i]], current[open[best]]))
                best = i;

        std::size_t const chosen = open[best];
        encode_record(current[chosen], bytes);
        if (!write_bytes(out.get(), bytes, RECORD_BYTES))
            throw_io("I/O error while writing merged run file", out_path);
        write_bytes_count += RECORD_BYTES;

        if (read_bytes(inputs[chosen].get(), bytes, RECORD_BYTES))
        {
            decode_record(bytes, &current[chosen]);
            read_bytes_count += RECORD_BYTES;
        }
        else
        {
            valid[chosen] = false;
            open.erase(open.begin() + best);
        }
    }
    if (std::fflush(out.get()) != 0)
        throw_io("I/O error while flushing merged run file", out_path);

    this->temp_read_bytes += read_bytes_count;
    this->temp_write_bytes += write_bytes_count;
}

/* --------------------------------------------------------------------- */

void
StreamSurfaceImpl::assign_vertex_ids_and_slots (void)
{
    /*
     * Assign vertex IDs in increasing edge-key order, stream the vertex
     * table to disk and scatter (slot -> id) into the slot table.
     */
    this->vertex_path = this->call_dir + "/vertices.bin";
    BufferedFile vout;
    if (!vout.open(this->vertex_path, "wb"))
        throw_io("Cannot write vertex table", this->vertex_path);

    std::string const slot_file = this->call_dir + "/slots.bin";
    {
        /* Size the slot table: slots are dense. */
        std::FILE* raw = std::fopen(slot_file.c_str(), "wb");
        if (raw == nullptr)
            throw_io("Cannot create slot table", slot_file);
        bool ok = true;
        if (this->slot_count > 0)
        {
            if (std::fseek(raw,
                    static_cast<long>(this->slot_count * SLOT_BYTES - 1), SEEK_SET) != 0)
                ok = false;
            else if (std::fputc(0, raw) == EOF)
                ok = false;
        }
        if (std::fclose(raw) != 0)
            ok = false;
        if (!ok)
            throw_io("I/O error while sizing slot table", slot_file);
    }

    BufferedFile table;
    if (!table.open(slot_file, "r+b"))
        throw_io("Cannot open slot table", slot_file);

    std::size_t const page = this->page_slots();
    std::size_t const cache_pages = std::max<std::size_t>(2,
        std::min<std::size_t>(64, (this->budget / 8) / (page * SLOT_BYTES + 1)));

    /*
     * Records arrive sorted by edge key, so slot ordinals arrive in an
     * essentially random order. Keep a small LRU of pages and preserve the
     * entries of every page on eviction with a read-modify-write cycle.
     * This keeps the slot scatter correct with a bounded working set.
     */
    struct CachedPage
    {
        CachedPage (void) : index(0), last_use(0) { }
        std::size_t index;
        std::size_t last_use;
        TVector<uint32_t> data;
    };
    std::vector<CachedPage> cache(cache_pages);
    std::vector<bool> entry_valid(cache_pages, false);
    std::size_t use_counter = 0;

    auto write_page = [&] (CachedPage const& entry) -> bool
    {
        std::size_t const n = std::min(page,
            this->slot_count - entry.index * page);
        if (std::fseek(table.get(),
                static_cast<long>(entry.index * page * SLOT_BYTES), SEEK_SET) != 0)
            return false;
        if (!write_bytes(table.get(), entry.data.data(), n * SLOT_BYTES))
            return false;
        this->temp_write_bytes += n * SLOT_BYTES;
        return true;
    };
    auto read_page = [&] (CachedPage* entry, std::size_t index) -> bool
    {
        entry->index = index;
        entry->data.resize(page);
        std::fill(entry->data.begin(), entry->data.end(), 0u);
        std::size_t const n = std::min(page, this->slot_count - index * page);
        if (std::fseek(table.get(),
                static_cast<long>(index * page * SLOT_BYTES), SEEK_SET) != 0)
            return false;
        if (!read_bytes(table.get(), entry->data.data(), n * SLOT_BYTES))
            return false;
        this->temp_read_bytes += n * SLOT_BYTES;
        return true;
    };
    auto find_cached = [&] (std::size_t index) -> CachedPage*
    {
        for (std::size_t i = 0; i < cache.size(); ++i)
            if (entry_valid[i] && cache[i].index == index)
                return &cache[i];
        return nullptr;
    };
    auto acquire = [&] (std::size_t index) -> CachedPage*
    {
        CachedPage* hit = find_cached(index);
        if (hit != nullptr)
        {
            hit->last_use = ++use_counter;
            return hit;
        }
        std::size_t victim = 0;
        bool free_slot = false;
        for (std::size_t i = 0; i < cache.size(); ++i)
        {
            if (!entry_valid[i])
            {
                victim = i;
                free_slot = true;
                break;
            }
            if (cache[i].last_use < cache[victim].last_use)
                victim = i;
        }
        if (!free_slot && entry_valid[victim])
            if (!write_page(cache[victim]))
                throw_io("I/O error while writing slot table", slot_file);
        if (!read_page(&cache[victim], index))
            throw_io("I/O error while reading slot table", slot_file);
        entry_valid[victim] = true;
        cache[victim].last_use = ++use_counter;
        return &cache[victim];
    };

    auto put_slot = [&] (std::size_t slot, uint32_t id)
    {
        CachedPage* entry = acquire(slot / page);
        entry->data[slot % page] = id;
    };
    auto flush_all = [&] (void)
    {
        for (std::size_t i = 0; i < cache.size(); ++i)
            if (entry_valid[i])
                if (!write_page(cache[i]))
                    throw_io("I/O error while writing slot table", slot_file);
    };

    BufferedFile in;
    if (!in.open(this->sorted_path, "rb"))
        throw_io("Cannot read sorted run file", this->sorted_path);

    std::size_t const capacity = std::max<std::size_t>(1,
        std::min<std::size_t>(this->run_limit, 4096));
    TVector<StreamVertexRecord> buffer;
    buffer.reserve(capacity);

    /* Translate the sorted run into vertex IDs and slot entries. */
    uint64_t prev_low = 0, prev_high = 0;
    bool have_prev = false;
    uint32_t next_id = 0;
    std::size_t read_bytes_count = 0;
    unsigned char bytes[RECORD_BYTES];
    while (read_bytes(in.get(), bytes, RECORD_BYTES))
    {
        read_bytes_count += RECORD_BYTES;
        StreamVertexRecord rec;
        decode_record(bytes, &rec);

        bool const is_new = !have_prev
            || rec.edge_low != prev_low || rec.edge_high != prev_high;
        if (is_new)
        {
            if (next_id == std::numeric_limits<uint32_t>::max())
                throw std::runtime_error(
                    "extract_stream_surface(): More than 2^32-1 vertices; PLY "
                    "uint32 face indices cannot represent the mesh");
            prev_low = rec.edge_low;
            prev_high = rec.edge_high;
            have_prev = true;
            next_id += 1;
            buffer.push_back(rec);
            if (buffer.size() == buffer.capacity())
            {
                this->write_vertex_buffer(&buffer, &vout);
                buffer.clear();
            }
        }
        put_slot(rec.slot_ordinal, next_id - 1);
    }
    flush_all();
    this->write_vertex_buffer(&buffer, &vout);
    if (std::fflush(vout.get()) != 0)
        throw_io("I/O error while writing vertex table", this->vertex_path);

    this->slot_path = slot_file;
    this->stats.vertex_count = next_id;
    this->temp_read_bytes += read_bytes_count;
    this->temp_write_bytes += static_cast<std::size_t>(next_id) * VERTEX_BYTES;

    release(&buffer);
    release(&this->slot_page);
}

/* Defined out of line to keep the class body readable. */
void
StreamSurfaceImpl::write_vertex_buffer (TVector<StreamVertexRecord>* buffer,
    BufferedFile* out)
{
    unsigned char bytes[VERTEX_BYTES];
    for (std::size_t i = 0; i < buffer->size(); ++i)
    {
        StreamVertexRecord const& r = (*buffer)[i];
        put_f32(bytes + 0, r.x); put_f32(bytes + 4, r.y); put_f32(bytes + 8, r.z);
        put_f32(bytes + 12, r.r); put_f32(bytes + 16, r.g); put_f32(bytes + 20, r.b);
        put_f32(bytes + 24, r.value); put_f32(bytes + 28, r.confidence);
        if (!write_bytes(out->get(), bytes, VERTEX_BYTES))
            throw_io("I/O error while writing vertex table", this->vertex_path);
    }
}

/* --------------------------------------------------------------------- */

void
StreamSurfaceImpl::read_slot (std::size_t slot, uint32_t* id)
{
    std::size_t const page = this->slot_page.size();
    std::size_t const target = slot / page;
    if (!this->slot_page_valid || target != this->slot_page_begin
        || this->slot_page.size() != page)
    {
        std::size_t const n = std::min(page, this->slot_count - target * page);
        if (std::fseek(this->slot_table.get(),
                static_cast<long>(target * page * SLOT_BYTES), SEEK_SET) != 0)
            throw_io("Cannot seek in slot table", this->slot_path);
        if (!read_bytes(this->slot_table.get(), this->slot_page.data(),
                n * SLOT_BYTES))
            throw_io("I/O error while reading slot table", this->slot_path);
        this->temp_read_bytes += n * SLOT_BYTES;
        this->slot_page_begin = target;
        this->slot_page_valid = true;
    }
    *id = this->slot_page[slot - this->slot_page_begin * page];
}

void
StreamSurfaceImpl::read_vertex (uint32_t id, float out[8])
{
    std::size_t const page = this->page_vertices();
    std::size_t const target = id / page;
    if (this->vertex_page_count == 0 || target != this->vertex_page_begin
        || this->vertex_page.size() != page * 8)
    {
        this->vertex_page.resize(page * 8);
        std::size_t const n = std::min(page,
            this->stats.vertex_count - target * page);
        if (std::fseek(this->vertex_table.get(),
                static_cast<long>(target * page * VERTEX_BYTES), SEEK_SET) != 0)
            throw_io("Cannot seek in vertex table", this->vertex_path);
        if (!read_bytes(this->vertex_table.get(), this->vertex_page.data(),
                n * VERTEX_BYTES))
            throw_io("I/O error while reading vertex table", this->vertex_path);
        this->temp_read_bytes += n * VERTEX_BYTES;
        this->vertex_page_begin = target;
        this->vertex_page_count = n;
    }
    std::size_t const offset = (id - target * page) * 8;
    std::memcpy(out, &this->vertex_page[offset], 8 * sizeof(float));
}

/* --------------------------------------------------------------------- */

void
StreamSurfaceImpl::write_output (void)
{
    /*
     * Count triangles exactly: a p-gon triangulates to p - 2 triangles.
     * No triangulation is needed for the count.
     */
    this->triangle_count = 0;
    this->traversal_mode = 3;
    {
        if (this->octree->get_root_node() != nullptr)
        {
            Octree::Iterator iter = this->octree->get_iterator_for_root();
            for (iter.first_leaf(); iter.current != nullptr; iter.next_leaf())
                this->surface.visit_leaf_isopolygons(iter,
                    &StreamSurfaceImpl::trampoline, this);
        }
    }
    this->traversal_mode = 0;
    this->stats.triangle_count = this->triangle_count;

    fs::path const target(this->ply_filename);
    fs::path const parent = target.parent_path().empty()
        ? fs::path(".") : target.parent_path();
    std::string const filename = target.filename().string();
    std::string const token = std::to_string(
        static_cast<unsigned long>(::getpid())) + "-"
        + std::to_string(reinterpret_cast<std::uintptr_t>(this));
    this->output_temp_path = (parent / (filename + ".fssr-tmp-" + token)).string();

    if (!this->output_file.open(this->output_temp_path, "wb"))
        throw_io("Cannot create output file", this->output_temp_path);
    this->output_open = true;

    {
        std::string header = "ply\n";
        header += "format binary_little_endian 1.0\n";
        header += "comment Created by FSSR out-of-core extraction\n";
        header += "element vertex " + std::to_string(this->stats.vertex_count) + "\n";
        header += "property float x\n";
        header += "property float y\n";
        header += "property float z\n";
        header += "property uchar red\n";
        header += "property uchar green\n";
        header += "property uchar blue\n";
        header += "property float value\n";
        header += "property float confidence\n";
        header += "element face " + std::to_string(this->triangle_count) + "\n";
        header += "property list uchar uint vertex_indices\n";
        header += "end_header\n";
        if (!write_bytes(this->output_file.get(), header.data(), header.size()))
            throw_io("I/O error while writing PLY header", this->output_temp_path);
        this->temp_write_bytes += header.size();
    }

    /* Stream the vertex table, converting colors to uchar. */
    {
        BufferedFile vin;
        if (!vin.open(this->vertex_path, "rb"))
            throw_io("Cannot read vertex table", this->vertex_path);
        std::size_t const chunk = std::max<std::size_t>(1,
            std::min<std::size_t>(this->vertex_chunk(),
                std::max<std::size_t>(1, this->stats.vertex_count)));
        TVector<unsigned char> outbuf;
        outbuf.reserve(chunk * 16);
        TVector<float> raw;
        raw.reserve(chunk * 8);
        std::size_t remaining = this->stats.vertex_count;
        while (remaining > 0)
        {
            std::size_t const take = std::min(chunk, remaining);
            raw.resize(take * 8);
            if (!read_bytes(vin.get(), raw.data(), take * VERTEX_BYTES))
                throw_io("I/O error while reading vertex table", this->vertex_path);
            this->temp_read_bytes += take * VERTEX_BYTES;
            outbuf.clear();
            for (std::size_t i = 0; i < take; ++i)
            {
                float const* v = &raw[i * 8];
                unsigned char const* bytes =
                    reinterpret_cast<unsigned char const*>(&v[0]);
                outbuf.insert(outbuf.end(), bytes, bytes + 12);
                unsigned char color[3] = {
                    color_to_uchar(v[3]), color_to_uchar(v[4]), color_to_uchar(v[5]) };
                outbuf.insert(outbuf.end(), color, color + 3);
                unsigned char const* tail =
                    reinterpret_cast<unsigned char const*>(&v[6]);
                outbuf.insert(outbuf.end(), tail, tail + 8);
            }
            if (!write_bytes(this->output_file.get(), outbuf.data(), outbuf.size()))
                throw_io("I/O error while writing vertices", this->output_temp_path);
            this->temp_write_bytes += outbuf.size();
            remaining -= take;
        }
        release(&outbuf);
        release(&raw);
    }

    /* Random-access tables for the output traversal. */
    if (!this->slot_table.open(this->slot_path, "rb"))
        throw_io("Cannot read slot table", this->slot_path);
    this->slot_page.resize(this->page_slots());
    this->slot_page_valid = false;
    if (!this->vertex_table.open(this->vertex_path, "rb"))
        throw_io("Cannot read vertex table", this->vertex_path);
    this->vertex_page_count = 0;

    this->output_slot = 0;
    this->traverse(2);

    this->slot_table.close();
    this->vertex_table.close();
    release(&this->slot_page);
    release(&this->vertex_page);
    release(&this->polygon_ids);
    release(&this->polygon_positions);

    if (std::fflush(this->output_file.get()) != 0)
        throw_io("I/O error while flushing output file", this->output_temp_path);
    this->output_file.close();
    this->output_open = false;
}

void
StreamSurfaceImpl::handle_output_polygon (IsoSurface::Ingredient const* /*verts*/,
    std::size_t num_verts)
{
    if (this->traversal_mode == 3)
    {
        if (num_verts >= 3)
            this->triangle_count += static_cast<uint32_t>(num_verts - 2);
        return;
    }

    if (num_verts < 3)
        return;

    this->polygon_ids.resize(num_verts);
    for (std::size_t s = 0; s < num_verts; ++s)
        this->read_slot(this->output_slot + s, &this->polygon_ids[s]);
    this->output_slot += num_verts;

    this->polygon_positions.resize(num_verts);
    for (std::size_t s = 0; s < num_verts; ++s)
    {
        float v[8];
        this->read_vertex(this->polygon_ids[s], v);
        this->polygon_positions[s] = math::Vec3f(v[0], v[1], v[2]);
    }

    /*
     * MinAreaTriangulation allocates two p^2 tables (float + int), which is
     * the only super-constant workspace. Charge it explicitly so the
     * reported peak covers it.
     */
    ScopedCharge charge(static_cast<std::size_t>(num_verts) * num_verts
        * (sizeof(float) + sizeof(int)));

    MinAreaTriangulation tri;
    std::vector<unsigned int> local;
    {
        ScopedCharge local_charge(3 * this->polygon_positions.size()
            * sizeof(math::Vec3f) + this->polygon_positions.size()
            * sizeof(unsigned int) * 2);
        std::vector<math::Vec3f> plain(this->polygon_positions.begin(),
            this->polygon_positions.end());
        tri.triangulate(plain, &local);
    }

    unsigned char face[13];
    for (std::size_t t = 0; t + 2 < local.size(); t += 3)
    {
        face[0] = 3;
        put_u32(face + 1, this->polygon_ids[local[t + 0]]);
        put_u32(face + 5, this->polygon_ids[local[t + 1]]);
        put_u32(face + 9, this->polygon_ids[local[t + 2]]);
        if (!write_bytes(this->output_file.get(), face, 13))
            throw_io("I/O error while writing triangles", this->output_temp_path);
        this->temp_write_bytes += 13;
    }
}

/* --------------------------------------------------------------------- */

StreamSurfaceStats
extract_stream_surface (IsoOctree* octree, InterpolationType interpolation_type,
    std::string const& ply_filename, std::string const& temp_directory,
    std::size_t budget_bytes)
{
    StreamSurfaceImpl impl(octree, interpolation_type, ply_filename,
        temp_directory, budget_bytes);
    return impl.run();
}

StreamSurfaceStats
IsoSurface::extract_stream_mesh (std::string const& ply_filename,
    std::string const& temp_directory, std::size_t budget_bytes)
{
    return extract_stream_surface(static_cast<IsoOctree*>(this->octree),
        this->interpolation_type, ply_filename, temp_directory, budget_bytes);
}

FSSR_NAMESPACE_END
