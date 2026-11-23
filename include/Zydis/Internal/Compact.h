/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Internal definitions of the symbolic provenance ("compaction DAG") component.
 */

#ifndef ZYDIS_INTERNAL_COMPACT_H
#define ZYDIS_INTERNAL_COMPACT_H

#include <Zycore/Types.h>
#include <Zydis/Compact.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

#define ZYDIS_COMPACT_ZMM_COUNT      32u
#define ZYDIS_COMPACT_K_COUNT        8u
#define ZYDIS_COMPACT_INVALID_NODE   0xFFFFFFFFu
#define ZYDIS_COMPACT_INVALID_CELL   0xFFFFFFFFu

/* ============================================================================================== */
/* Graph node                                                                                     */
/* ============================================================================================== */

typedef struct ZydisCompactNode_
{
    ZydisCompactNodeType type;
    ZyanU32 child[3];
    ZyanU8 lane;
    ZyanU8 zmm;
    ZyanU8 k;
    ZyanU8 width;
    ZyanU64 address;
} ZydisCompactNode;

/* ============================================================================================== */
/* State snapshots                                                                                */
/* ============================================================================================== */

typedef struct ZydisCompactVecVersion_
{
    /* All 32 vector registers, each with its 16 dword lanes, flattened by zmm id. */
    ZyanU32 lane[ZYDIS_COMPACT_ZMM_COUNT * ZYDIS_COMPACT_MAX_LANES];
} ZydisCompactVecVersion;

typedef struct ZydisCompactMaskVersion_
{
    /* All 8 mask registers, each with its 16 bits, flattened by k id. */
    ZyanU32 bit[ZYDIS_COMPACT_K_COUNT * ZYDIS_COMPACT_MASK_BITS];
} ZydisCompactMaskVersion;

/* ---------------------------------------------------------------------------------------------- */
/* Memory                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

typedef struct ZydisCompactRegionInternal_
{
    ZyanU64 address;
    ZyanUSize cells;
} ZydisCompactRegionInternal;

/**
 * A versioned memory cell. Each entry cell is materialized lazily on first access; later
 * writes append further records. `version` is the state version (1-based instruction index)
 * at which the record was produced; entry leaves carry version zero.
 */
typedef struct ZydisCompactMemCell_
{
    ZyanU64 address;
    ZyanU32 node;
    ZyanU32 version;
} ZydisCompactMemCell;

/* ---------------------------------------------------------------------------------------------- */
/* Instruction log                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

typedef struct ZydisCompactInstrInfo_
{
    ZyanU64 address;
    ZyanU8 length;
} ZydisCompactInstrInfo;

/* ---------------------------------------------------------------------------------------------- */
/* DAG                                                                                             */
/* ---------------------------------------------------------------------------------------------- */

struct ZydisCompactDag_
{
    /** Owned node storage (grown on demand). */
    ZydisCompactNode* nodes;
    ZyanU32 node_count;
    ZyanUSize node_capacity;

    /** Vector state, one entry per decoded instruction (index 0 is the entry state). */
    ZydisCompactVecVersion* vec;
    ZyanU32 vec_count;
    ZyanUSize vec_capacity;

    /** Mask state, same indexing as `vec`. */
    ZydisCompactMaskVersion* mask;
    ZyanU32 mask_count;
    ZyanUSize mask_capacity;

    /** Lazy materialized memory-cell records. */
    ZydisCompactMemCell* mem;
    ZyanU32 mem_count;
    ZyanUSize mem_capacity;

    /** Configured regions. */
    ZydisCompactRegionInternal* regions;
    ZyanUSize region_count;
    ZyanUSize region_capacity;

    /** Decoded instruction log. */
    ZydisCompactInstrInfo* instrs;
    ZyanU32 instr_count;
    ZyanUSize instr_capacity;

    /** Id of the shared logical-zero node. */
    ZyanU32 zero_node;
    /** Id of the shared logical-one node. */
    ZyanU32 one_node;

    /** Poison flag: set after the first failed decode. */
    ZyanBool poisoned;

    /** Details of the last unsupported instruction. */
    ZyanBool has_unsupported;
    ZyanU64 unsupported_address;
    ZyanU8 unsupported_length;
};

/* ============================================================================================== */
/* Internal helpers                                                                               */
/* ============================================================================================== */

/**
 * Appends a node to the graph and returns its id in `*out_id`.
 * Returns `ZYDIS_STATUS_COMPACT_LIMIT` if `ZYDIS_COMPACT_NODE_LIMIT` is exceeded.
 */
ZyanStatus ZydisCompactAddNode(ZydisCompactDag* dag, const ZydisCompactNode* node,
    ZyanU32* out_id);

/**
 * Returns the current (latest) vector/mask state.
 */
ZydisCompactVecVersion* ZydisCompactCurVec(ZydisCompactDag* dag);
ZydisCompactMaskVersion* ZydisCompactCurMask(ZydisCompactDag* dag);

/**
 * Resolves the region containing `address` and the cell index within it. A `ZYAN_NULL`
 * region output means "not in any region".
 */
ZyanStatus ZydisCompactResolveRegion(const ZydisCompactDag* dag, ZyanU64 address,
    const ZydisCompactRegionInternal** out_region, ZyanUSize* out_index);

/**
 * Reads the current provenance node of a memory cell, materializing its `ENTRY_MEM` leaf
 * on first access.
 */
ZyanStatus ZydisCompactMemRead(ZydisCompactDag* dag, ZyanU64 address, ZyanU32* out_node);

/**
 * Appends a new versioned record for a memory cell. `version` is the 1-based instruction
 * index producing the write.
 */
ZyanStatus ZydisCompactMemWrite(ZydisCompactDag* dag, ZyanU64 address, ZyanU32 node,
    ZyanU32 version);

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_INTERNAL_COMPACT_H */
