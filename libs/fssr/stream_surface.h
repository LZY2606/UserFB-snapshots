/*
 * Copyright (C) 2015, Simon Fuhrmann
 * TU Darmstadt - Graphics, Capture and Massively Parallel Computing
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 *
 * Out-of-core isosurface extraction for FSSR.
 *
 * The extraction reads the octree and voxel array (which stay resident)
 * and writes the final PLY file using a bounded amount of working memory.
 * Edge records, connection information and polygon references are spilled
 * to temporary files owned by the individual extraction call.
 *
 * Working set accounting
 * ----------------------
 * Every dynamic container used by the extraction is a TrackedAllocator
 * container, so the reported peak_working_bytes is the exact live sum of
 * their capacities (including std::vector over-allocation and the page
 * caches), plus a fixed reserve for the transient containers of the shared
 * per-leaf geometry code and for paths and file objects. No buffer grows
 * with the total output: the run buffer, merge fan-in, slot/vertex page
 * caches and I/O buffers are all derived from the budget, and each phase
 * releases its buffers before the next one starts.
 *
 * The only size-dependent term is the O(p^2) minimum-area triangulation
 * table of the largest single leaf polygon (p is the number of vertices of
 * that polygon). It is charged against the reported peak while it is live
 * and is also exposed separately as triangulation_workspace_bytes. The
 * contract is therefore:
 *
 *     peak_working_bytes  <=  budget
 *     peak_working_bytes  does not grow with the number of output elements
 *
 * The triangulation workspace is included in peak_working_bytes but is
 * additionally available through triangulation_workspace_bytes for callers
 * that want to budget it explicitly.
 *
 * External connection flow
 * ------------------------
 *   1. A leaf traversal emits one record per polygon slot, buffers them and
 *      spills edge-key-sorted runs to the call-owned temporary directory.
 *   2. Runs are merged with bounded fan-in into one sorted stream.
 *   3. The sorted stream assigns vertex IDs in increasing edge-key order,
 *      writes the deduplicated vertex table and scatters each record's slot
 *      ordinal to its vertex ID in a dense on-disk slot table.
 *   4. A second leaf traversal rebuilds each polygon in the original order,
 *      reads its IDs from the slot table, runs the reference
 *      MinAreaTriangulation and appends the triangles to the output file.
 *
 * Temporary files
 * ---------------
 * A unique directory is created inside the caller supplied temporary
 * directory for every call.  Two calls may therefore share a temporary
 * directory, even concurrently, without interfering.  All files are removed
 * when the call returns, including on error.
 *
 * Publication and exception guarantees
 * ------------------------------------
 * The PLY file is first written to a temporary file in the directory of
 * the final target and is atomically renamed only after every stage has
 * succeeded.  On invalid arguments or I/O errors the target file is left
 * untouched, temporary files are removed, and a std::invalid_argument or
 * std::runtime_error with an explanatory message is thrown.  A failed call
 * can be retried immediately.
 */

#ifndef FSSR_STREAM_SURFACE_HEADER
#define FSSR_STREAM_SURFACE_HEADER

#include <cstddef>
#include <string>

#include "fssr/defines.h"
#include "fssr/hermite.h"
#include "fssr/iso_octree.h"

FSSR_NAMESPACE_BEGIN

/** Statistics reported by one out-of-core extraction. */
struct StreamSurfaceStats
{
    StreamSurfaceStats (void);

    /** Peak bytes allocated by the extraction (see header comment). */
    std::size_t peak_working_bytes;
    /** Bytes read from temporary files (runs, tables, payloads). */
    std::size_t temp_read_bytes;
    /** Bytes written to temporary files (runs, tables, payloads). */
    std::size_t temp_write_bytes;
    /** Number of vertices in the output mesh. */
    std::size_t vertex_count;
    /** Number of triangles in the output mesh. */
    std::size_t triangle_count;
    /** Number of external merge levels performed for the edge records. */
    std::size_t merge_passes;
    /** Largest number of vertices of a single leaf polygon. */
    std::size_t max_polygon_vertices;
    /** O(p^2) triangulation workspace for the largest leaf polygon. */
    std::size_t triangulation_workspace_bytes;
};

/**
 * Extracts the isosurface of an IsoOctree (after compute_voxels() has
 * been called) into a binary little-endian PLY file without ever holding
 * the complete vertex/edge/polygon tables in memory.
 *
 * Vertex IDs are assigned in increasing order of the edge key, which is
 * the ordered pair of the two VoxelIndex values of the finest cube edge.
 * Triangles are emitted in leaf traversal order, and within a leaf in the
 * original polygon and triangulation order.
 *
 * @param octree               Octree with computed voxels.
 * @param interpolation_type   Isosurface interpolation mode.
 * @param ply_filename         Destination PLY file.
 * @param temp_directory       Existing directory for call-owned temp files.
 * @param budget_bytes         Working memory budget in bytes (>= 64 KiB).
 * @throws std::invalid_argument for invalid budgets or empty paths.
 * @throws std::runtime_error for unwritable directories or I/O failures.
 */
StreamSurfaceStats
extract_stream_surface (IsoOctree* octree,
    InterpolationType interpolation_type,
    std::string const& ply_filename,
    std::string const& temp_directory,
    std::size_t budget_bytes);

/** Internal implementation class (defined in stream_surface.cc). */
class StreamSurfaceImpl;

FSSR_NAMESPACE_END

#endif /* FSSR_STREAM_SURFACE_HEADER */
