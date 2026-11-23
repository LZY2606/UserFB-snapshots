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
 *
 * Symbolic provenance ("compaction DAG") for straight-line AVX-512 data-filtering code.
 *
 * This component does not execute machine code and never dereferences host pointers as data.
 * It decodes a stream of ordinary x86-64 instruction bytes with `ZydisDecoderDecodeFull` and
 * builds a symbolic expression graph that records where every 32-bit vector lane, every mask
 * bit and every memory dword ultimately comes from.
 *
 * The identity leaves of the graph are:
 *
 * - the 16 dword lanes of each entry vector register (`ZYDIS_COMPACT_NODE_ENTRY_LANE`),
 * - the 16 entry bits of each mask register k0..k7 (`ZYDIS_COMPACT_NODE_ENTRY_MASK`),
 * - each dword cell of a configured memory region (`ZYDIS_COMPACT_NODE_ENTRY_MEM`).
 *
 * Entry mask values are symbolic variables: the graph is built once without choosing any
 * concrete mask assignment. The same graph can be evaluated any number of times with
 * different concrete entry masks via `ZydisCompactDagEvaluate`. No mask combination is
 * enumerated while building the graph and no execution trace is duplicated per assignment.
 * The graph never performs numeric data computation; it only tracks provenance.
 */

#ifndef ZYDIS_COMPACT_H
#define ZYDIS_COMPACT_H

#include <Zycore/Types.h>
#include <Zydis/Defines.h>
#include <Zydis/Status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Limits and status codes                                                                        */
/* ============================================================================================== */

/**
 * The maximum number of dword lanes in a vector register (512 bit / 32 bit).
 */
#define ZYDIS_COMPACT_MAX_LANES 16u

/**
 * The number of meaningful bits in a mask register (mask instructions operate on 16 bits).
 */
#define ZYDIS_COMPACT_MASK_BITS 16u

/**
 * The maximum number of nodes a single graph may hold. Exceeding it fails the offending
 * `ZydisCompactDagDecode` call with `ZYDIS_STATUS_COMPACT_LIMIT`; no truncated graph is
 * published (the state produced by completed earlier instructions remains queryable).
 */
#define ZYDIS_COMPACT_NODE_LIMIT 65536u

/**
 * The maximum number of instructions that can be decoded into one graph.
 */
#define ZYDIS_COMPACT_INSTRUCTION_LIMIT 4096u

/**
 * The maximum number of disjoint memory regions accepted by one graph.
 */
#define ZYDIS_COMPACT_REGION_LIMIT 256u

/**
 * The maximum number of versioned memory-cell records held by one graph.
 */
#define ZYDIS_COMPACT_MEMORY_LIMIT 65536u

/**
 * The maximum recursion depth used while evaluating a node, guarding against pathologically
 * deep graphs.
 */
#define ZYDIS_COMPACT_EVAL_DEPTH 4096u

/**
 * An instruction outside the supported set was encountered. The failing offset is available
 * through `ZydisCompactDagLastUnsupported`.
 */
#define ZYDIS_STATUS_COMPACT_UNSUPPORTED \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYDIS, 0x10u)

/**
 * A graph resource limit (`ZYDIS_COMPACT_*_LIMIT`) was exhausted.
 */
#define ZYDIS_STATUS_COMPACT_LIMIT \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYDIS, 0x11u)

/**
 * A memory access targets an address that is not covered by a configured region.
 */
#define ZYDIS_STATUS_COMPACT_NO_REGION \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYDIS, 0x12u)

/**
 * A memory access resolved to an address that is not 4-byte aligned.
 */
#define ZYDIS_STATUS_COMPACT_MISALIGNED \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYDIS, 0x13u)

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZydisCompactNodeType` enum.
 *
 * Node semantics:
 *
 * - `ZYDIS_COMPACT_NODE_ZERO` / `ZYDIS_COMPACT_NODE_ONE`: logical constants. `ZERO` is both
 *   the boolean false and the provenance of a lane that is deterministically zeroed.
 * - `ZYDIS_COMPACT_NODE_ENTRY_LANE`, `ZYDIS_COMPACT_NODE_ENTRY_MASK`,
 *   `ZYDIS_COMPACT_NODE_ENTRY_MEM`: identity leaves. See `ZydisCompactNodeInfo`.
 * - `ZYDIS_COMPACT_NODE_MASK_*`: boolean mask logic. `SHLL`/`SHRL` are the 16-bit logical
 *   shifts; the shift amount is in `ZydisCompactNodeInfo.lane`.
 * - `ZYDIS_COMPACT_NODE_RANK`: prefix popcount. Evaluates to the number of set bits among
 *   the low `ZydisCompactNodeInfo.width` bits of its mask child.
 * - `ZYDIS_COMPACT_NODE_EQ`: boolean equality between a rank child and the constant in
 *   `ZydisCompactNodeInfo.lane`.
 * - `ZYDIS_COMPACT_NODE_SELECT`: conditional data source. `child[0]` is the boolean
 *   condition, `child[1]` the source selected when it is set, `child[2]` the fallback.
 */
typedef enum ZydisCompactNodeType_
{
    ZYDIS_COMPACT_NODE_ZERO,
    ZYDIS_COMPACT_NODE_ONE,
    ZYDIS_COMPACT_NODE_ENTRY_LANE,
    ZYDIS_COMPACT_NODE_ENTRY_MASK,
    ZYDIS_COMPACT_NODE_ENTRY_MEM,
    ZYDIS_COMPACT_NODE_MASK_AND,
    ZYDIS_COMPACT_NODE_MASK_OR,
    ZYDIS_COMPACT_NODE_MASK_XOR,
    ZYDIS_COMPACT_NODE_MASK_NOT,
    ZYDIS_COMPACT_NODE_MASK_SHLL,
    ZYDIS_COMPACT_NODE_MASK_SHRL,
    ZYDIS_COMPACT_NODE_RANK,
    ZYDIS_COMPACT_NODE_EQ,
    ZYDIS_COMPACT_NODE_SELECT,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_COMPACT_NODE_MAX_VALUE = ZYDIS_COMPACT_NODE_SELECT,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_COMPACT_NODE_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_COMPACT_NODE_MAX_VALUE)
} ZydisCompactNodeType;

/**
 * Public description of a graph node.
 */
typedef struct ZydisCompactNodeInfo_
{
    /**
     * The node type.
     */
    ZydisCompactNodeType type;
    /**
     * The ids of the child nodes (unused slots contain the id of the shared `ZERO` node).
     *
     * - binary mask nodes: `child[0]`, `child[1]`
     * - unary mask nodes, `RANK`: `child[0]`
     * - `EQ`: `child[0]` is the rank node
     * - `SELECT`: `child[0]` condition, `child[1]` on-source, `child[2]` off-source
     */
    ZyanU32 child[3];
    /**
     * Small integer parameter.
     *
     * - `ENTRY_LANE` / `ENTRY_MASK`: the lane index / bit index
     * - `MASK_SHLL` / `MASK_SHRL`: the shift amount
     * - `EQ`: the constant compared against the rank
     */
    ZyanU8 lane;
    /**
     * Vector register id (0..31) for `ENTRY_LANE` nodes.
     */
    ZyanU8 zmm;
    /**
     * Mask register id (0..7) for `ENTRY_MASK` nodes.
     */
    ZyanU8 k;
    /**
     * Prefix width in bits for `RANK` nodes (the popcount only considers bits below it).
     */
    ZyanU8 width;
    /**
     * Virtual address of the dword cell for `ENTRY_MEM` nodes.
     */
    ZyanU64 address;
} ZydisCompactNodeInfo;

/**
 * Defines the `ZydisCompactValueKind` enum returned by `ZydisCompactDagEvaluate`.
 */
typedef enum ZydisCompactValueKind_
{
    /**
     * The evaluated value is deterministically zero.
     */
    ZYDIS_COMPACT_VALUE_ZERO,
    /**
     * The evaluated value originates from an entry vector lane.
     */
    ZYDIS_COMPACT_VALUE_ENTRY_LANE,
    /**
     * The evaluated value originates from an entry memory cell.
     */
    ZYDIS_COMPACT_VALUE_ENTRY_MEM,

    ZYDIS_COMPACT_VALUE_MAX_VALUE = ZYDIS_COMPACT_VALUE_ENTRY_MEM
} ZydisCompactValueKind;

/**
 * The concrete provenance produced by evaluating a data node under one mask assignment.
 */
typedef struct ZydisCompactValue_
{
    /**
     * The kind of provenance.
     */
    ZydisCompactValueKind kind;
    /**
     * Entry vector register id (0..31), valid for `ZYDIS_COMPACT_VALUE_ENTRY_LANE`.
     */
    ZyanU8 zmm;
    /**
     * Entry lane index (0..15), valid for `ZYDIS_COMPACT_VALUE_ENTRY_LANE`.
     */
    ZyanU8 lane;
    /**
     * Entry memory cell address, valid for `ZYDIS_COMPACT_VALUE_ENTRY_MEM`.
     */
    ZyanU64 address;
} ZydisCompactValue;

/**
 * A disjoint, non-wrapping, 4-byte aligned virtual memory region.
 */
typedef struct ZydisCompactRegion_
{
    /**
     * Inclusive start virtual address. Must be 4-byte aligned.
     */
    ZyanU64 address;
    /**
     * The number of dword (4-byte) cells contained in the region.
     */
    ZyanUSize cells;
} ZydisCompactRegion;

/**
 * Opaque provenance graph. All instances are created with `ZydisCompactDagCreate` and
 * destroyed with `ZydisCompactDagDestroy`.
 */
typedef struct ZydisCompactDag_ ZydisCompactDag;

/* ============================================================================================== */
/* Lifecycle and regions                                                                          */
/* ============================================================================================== */

/**
 * Creates an empty provenance graph.
 *
 * The graph starts in the entry state: every vector lane, mask bit and (lazily created)
 * memory cell is its own identity leaf.
 *
 * @param   out_dag Receives the newly created graph.
 *
 * @return  `ZYAN_STATUS_SUCCESS`, `ZYAN_STATUS_INVALID_ARGUMENT` or a Zycore allocation
 *          status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagCreate(ZydisCompactDag** out_dag);

/**
 * Destroys the given graph and releases all memory it owns. Passing `ZYAN_NULL` is a no-op.
 * The graph does not keep references to any input buffer.
 *
 * @param   dag The graph to destroy.
 */
ZYDIS_EXPORT void ZydisCompactDagDestroy(ZydisCompactDag* dag);

/**
 * Registers a memory region.
 *
 * Regions must be 4-byte aligned, non-empty, non-wrapping and must not overlap any region
 * registered earlier. At most `ZYDIS_COMPACT_REGION_LIMIT` regions are accepted.
 *
 * @param   dag     The graph.
 * @param   address The inclusive, 4-byte aligned start address.
 * @param   cells   The number of dword cells.
 *
 * @return  `ZYAN_STATUS_SUCCESS`, `ZYAN_STATUS_INVALID_ARGUMENT`,
 *          `ZYAN_STATUS_OUT_OF_RANGE` (overlap/wrap/alignment) or
 *          `ZYDIS_STATUS_COMPACT_LIMIT`.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagAddRegion(ZydisCompactDag* dag, ZyanU64 address,
    ZyanUSize cells);

/* ============================================================================================== */
/* Decoding                                                                                       */
/* ============================================================================================== */

/**
 * Decodes the single instruction at the beginning of `buffer` and folds it into the graph.
 *
 * Supported instructions (128/256/512 vector widths where applicable):
 *
 * - `KMOVW` between mask registers;
 * - `KANDW`, `KORW`, `KXORW`, `KNOTW`;
 * - `KSHIFTLW`, `KSHIFTRW`;
 * - register-form `VMOVDQU32` with merging or zeroing masking;
 * - `VPCOMPRESSD` and `VPEXPANDD` in register and memory forms.
 *
 * Memory operands must be RIP-relative or a bare disp32 without base/index registers, and
 * the resolved effective address must be 4-byte aligned. Any other addressing mode, any
 * unsupported instruction or any invalid operand shape fails with
 * `ZYDIS_STATUS_COMPACT_UNSUPPORTED` (or a more specific status code); the offset is
 * retrievable with `ZydisCompactDagLastUnsupported`.
 *
 * An unsupported or undecodable instruction, and any resource-limit failure, poisons the
 * graph: subsequent `ZydisCompactDagDecode` calls return `ZYAN_STATUS_INVALID_OPERATION`
 * without touching the state produced by the instructions that completed earlier. A region
 * (`ZYDIS_STATUS_COMPACT_NO_REGION`) or alignment (`ZYDIS_STATUS_COMPACT_MISALIGNED`)
 * failure only rejects the offending instruction and leaves the graph usable.
 *
 * k0 used as a vector write mask means "all lanes selected" and its bits are not read; when
 * k0 is an operand of a mask instruction it is an ordinary 16-bit mask register.
 *
 * @param   dag             The graph.
 * @param   buffer          The instruction bytes. The buffer is not retained.
 * @param   length          The number of readable bytes at `buffer`.
 * @param   runtime_address The virtual address of the instruction, used for RIP-relative
 *                          effective-address calculation.
 *
 * @return  `ZYAN_STATUS_SUCCESS` or a failure status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagDecode(ZydisCompactDag* dag, const void* buffer,
    ZyanUSize length, ZyanU64 runtime_address);

/**
 * Returns the number of instructions successfully folded into the graph.
 *
 * @param   dag The graph.
 *
 * @return  The number of decoded instructions.
 */
ZYDIS_EXPORT ZyanU32 ZydisCompactDagInstructionCount(const ZydisCompactDag* dag);

/**
 * Returns the location of the instruction that caused the last unsupported-instruction
 * failure.
 *
 * @param   dag                  The graph.
 * @param   instruction_index    Receives the zero-based index the failed instruction would
 *                               have had (equal to `ZydisCompactDagInstructionCount`).
 * @param   instruction_address  Receives the runtime address passed for the instruction.
 * @param   instruction_length   Receives the decoded instruction length in bytes.
 *
 * @return  `ZYAN_STATUS_SUCCESS` if an unsupported instruction was recorded,
 *          `ZYAN_STATUS_NOT_FOUND` otherwise.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagLastUnsupported(const ZydisCompactDag* dag,
    ZyanU32* instruction_index, ZyanU64* instruction_address, ZyanUSize* instruction_length);

/* ============================================================================================== */
/* Graph inspection and version queries                                                           */
/* ============================================================================================== */

/**
 * Returns the number of nodes currently held by the graph. Node ids are stable for the
 * lifetime of the graph and range from zero to node count minus one.
 *
 * @param   dag The graph.
 *
 * @return  The number of nodes.
 */
ZYDIS_EXPORT ZyanU32 ZydisCompactDagNodeCount(const ZydisCompactDag* dag);

/**
 * Copies the public description of a node.
 *
 * @param   dag     The graph.
 * @param   node_id The node id.
 * @param   info    Receives the node description.
 *
 * @return  `ZYAN_STATUS_SUCCESS`, `ZYAN_STATUS_INVALID_ARGUMENT` or
 *          `ZYAN_STATUS_OUT_OF_RANGE`.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagGetNode(const ZydisCompactDag* dag, ZyanU32 node_id,
    ZydisCompactNodeInfo* info);

/**
 * Queries the provenance node of a vector lane in the state after `instruction` instructions.
 *
 * Instruction index zero denotes the entry state. Lanes beyond a narrow write report the
 * shared `ZERO` node because a 128/256-bit write zeroes the upper lanes of the enclosing
 * ZMM.
 *
 * @param   dag         The graph.
 * @param   instruction The instruction version, 0..`ZydisCompactDagInstructionCount`.
 * @param   zmm         The vector register id, 0..31.
 * @param   lane        The dword lane index, 0..`ZYDIS_COMPACT_MAX_LANES - 1`.
 * @param   out_node    Receives the node id.
 *
 * @return  `ZYAN_STATUS_SUCCESS` or an error status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagGetLane(const ZydisCompactDag* dag, ZyanU32 instruction,
    ZyanU8 zmm, ZyanU8 lane, ZyanU32* out_node);

/**
 * Queries the provenance node of a mask bit in the state after `instruction` instructions.
 * Bits above the 16 implemented mask bits always read as zero.
 *
 * @param   dag         The graph.
 * @param   instruction The instruction version, 0..`ZydisCompactDagInstructionCount`.
 * @param   k           The mask register id, 0..7.
 * @param   bit         The bit index, 0..`ZYDIS_COMPACT_MASK_BITS - 1`.
 * @param   out_node    Receives the node id.
 *
 * @return  `ZYAN_STATUS_SUCCESS` or an error status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagGetMaskBit(const ZydisCompactDag* dag,
    ZyanU32 instruction, ZyanU8 k, ZyanU8 bit, ZyanU32* out_node);

/**
 * Queries the provenance node of a memory dword cell in the state after `instruction`
 * instructions. The address must be 4-byte aligned and contained in a registered region;
 * cells never written still resolve to their `ENTRY_MEM` leaf.
 *
 * @param   dag         The graph.
 * @param   instruction The instruction version, 0..`ZydisCompactDagInstructionCount`.
 * @param   address     The 4-byte aligned virtual cell address.
 * @param   out_node    Receives the node id.
 *
 * @return  `ZYAN_STATUS_SUCCESS`, `ZYAN_STATUS_COMPACT_NO_REGION`,
 *          `ZYAN_STATUS_COMPACT_MISALIGNED` or another error status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagGetMemory(ZydisCompactDag* dag,
    ZyanU32 instruction, ZyanU64 address, ZyanU32* out_node);

/* ============================================================================================== */
/* Evaluation                                                                                     */
/* ============================================================================================== */

/**
 * Evaluates a data node under a concrete entry-mask assignment.
 *
 * The graph itself is not modified; the same graph may be evaluated repeatedly with
 * different assignments. Mask bits above a vector instruction's valid lane count never
 * participate in rank counting, matching hardware behavior.
 *
 * @param   dag         The graph.
 * @param   node_id     The data node to evaluate (as returned by the state queries).
 * @param   entry_masks The concrete 16-bit value of each entry mask register k0..k7.
 * @param   out_value   Receives the resolved provenance.
 *
 * @return  `ZYAN_STATUS_SUCCESS`, `ZYAN_STATUS_INVALID_ARGUMENT`,
 *          `ZYAN_STATUS_OUT_OF_RANGE` or `ZYAN_STATUS_OUT_OF_RESOURCES` if the evaluation
 *          depth exceeds `ZYDIS_COMPACT_EVAL_DEPTH`.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagEvaluate(const ZydisCompactDag* dag, ZyanU32 node_id,
    const ZyanU16 entry_masks[8], ZydisCompactValue* out_value);

/**
 * Evaluates a mask-typed node (mask logic, rank or equality node) under a concrete
 * entry-mask assignment.
 *
 * Use this to inspect the symbolic mask DAG (for example the nodes returned by
 * `ZydisCompactDagGetMaskBit`); data provenance is queried with
 * `ZydisCompactDagEvaluate`. The graph itself is not modified.
 *
 * Boolean nodes (`MASK_*`, `EQ`) return 0 or 1. A `RANK` node returns its prefix-popcount
 * value (0..16), which is why the result is a byte rather than a strict boolean.
 *
 * @param   dag         The graph.
 * @param   node_id     The mask node to evaluate.
 * @param   entry_masks The concrete 16-bit value of each entry mask register k0..k7.
 * @param   out_bit     Receives the result.
 *
 * @return  `ZYAN_STATUS_SUCCESS`, `ZYAN_STATUS_INVALID_ARGUMENT`,
 *          `ZYAN_STATUS_OUT_OF_RANGE`, `ZYAN_STATUS_INVALID_OPERATION` if the node is not
 *          mask-typed, or `ZYAN_STATUS_OUT_OF_RESOURCES` on depth overflow.
 */
ZYDIS_EXPORT ZyanStatus ZydisCompactDagEvaluateMask(const ZydisCompactDag* dag,
    ZyanU32 node_id, const ZyanU16 entry_masks[8], ZyanU8* out_bit);

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_COMPACT_H */
