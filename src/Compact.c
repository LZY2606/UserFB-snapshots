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
 * Symbolic provenance ("compaction DAG") for straight-line AVX-512 data-filtering code.
 */

#include <stdlib.h>
#include <string.h>

#include <Zydis/Zydis.h>
#include <Zydis/Register.h>
#include <Zydis/Internal/Compact.h>

/* ============================================================================================== */
/* Storage helpers                                                                                */
/* ============================================================================================== */

static ZyanStatus compact_grow(void** buffer, ZyanUSize* capacity, ZyanUSize required,
    ZyanUSize element_size)
{
    if (required <= *capacity)
    {
        return ZYAN_STATUS_SUCCESS;
    }

    ZyanUSize new_cap = *capacity ? *capacity : 16u;
    while (new_cap < required)
    {
        new_cap *= 2u;
    }

    void* const new_buffer = realloc(*buffer, (size_t)new_cap * (size_t)element_size);
    if (new_buffer == ZYAN_NULL)
    {
        return ZYAN_STATUS_NOT_ENOUGH_MEMORY;
    }

    *buffer = new_buffer;
    *capacity = new_cap;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus compact_add_node(ZydisCompactDag* dag, const ZydisCompactNode* node,
    ZyanU32* out_id)
{
    if (dag->node_count >= ZYDIS_COMPACT_NODE_LIMIT)
    {
        return ZYDIS_STATUS_COMPACT_LIMIT;
    }

    ZYAN_CHECK(compact_grow((void**)&dag->nodes, &dag->node_capacity,
        (ZyanUSize)dag->node_count + 1u, sizeof(*dag->nodes)));

    const ZyanU32 id = dag->node_count++;
    dag->nodes[id] = *node;
    if (out_id != ZYAN_NULL)
    {
        *out_id = id;
    }
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisCompactAddNode(ZydisCompactDag* dag, const ZydisCompactNode* node,
    ZyanU32* out_id)
{
    return compact_add_node(dag, node, out_id);
}

/* ---------------------------------------------------------------------------------------------- */
/* Node constructors                                                                              */
/* ---------------------------------------------------------------------------------------------- */

static ZydisCompactNode compact_blank(ZydisCompactNodeType type)
{
    ZydisCompactNode node;
    memset(&node, 0, sizeof(node));
    node.type = type;
    node.child[0] = node.child[1] = node.child[2] = ZYDIS_COMPACT_INVALID_NODE;
    return node;
}

static ZyanStatus compact_mk_entry_lane(ZydisCompactDag* dag, ZyanU8 zmm, ZyanU8 lane,
    ZyanU32* out_id)
{
    ZydisCompactNode node = compact_blank(ZYDIS_COMPACT_NODE_ENTRY_LANE);
    node.zmm = zmm;
    node.lane = lane;
    return compact_add_node(dag, &node, out_id);
}

static ZyanStatus compact_mk_entry_mask(ZydisCompactDag* dag, ZyanU8 k, ZyanU8 bit,
    ZyanU32* out_id)
{
    ZydisCompactNode node = compact_blank(ZYDIS_COMPACT_NODE_ENTRY_MASK);
    node.k = k;
    node.lane = bit;
    return compact_add_node(dag, &node, out_id);
}

static ZyanStatus compact_mk_entry_mem(ZydisCompactDag* dag, ZyanU64 address, ZyanU32* out_id)
{
    ZydisCompactNode node = compact_blank(ZYDIS_COMPACT_NODE_ENTRY_MEM);
    node.address = address;
    return compact_add_node(dag, &node, out_id);
}

static ZyanStatus compact_mk_unary(ZydisCompactDag* dag, ZydisCompactNodeType type,
    ZyanU32 child, ZyanU32* out_id)
{
    if (type == ZYDIS_COMPACT_NODE_MASK_NOT)
    {
        if (child == dag->zero_node)
        {
            *out_id = dag->one_node;
            return ZYAN_STATUS_SUCCESS;
        }
        if (child == dag->one_node)
        {
            *out_id = dag->zero_node;
            return ZYAN_STATUS_SUCCESS;
        }
    }
    ZydisCompactNode node = compact_blank(type);
    node.child[0] = child;
    return compact_add_node(dag, &node, out_id);
}

static ZyanStatus compact_mk_binary(ZydisCompactDag* dag, ZydisCompactNodeType type,
    ZyanU32 a, ZyanU32 b, ZyanU32* out_id)
{
    // Constant folding for the boolean mask operators keeps the graph compact.
    switch (type)
    {
    case ZYDIS_COMPACT_NODE_MASK_AND:
        if (a == dag->zero_node || b == dag->zero_node)
        {
            *out_id = dag->zero_node;
            return ZYAN_STATUS_SUCCESS;
        }
        if (a == dag->one_node)
        {
            *out_id = b;
            return ZYAN_STATUS_SUCCESS;
        }
        if (b == dag->one_node)
        {
            *out_id = a;
            return ZYAN_STATUS_SUCCESS;
        }
        break;
    case ZYDIS_COMPACT_NODE_MASK_OR:
        if (a == dag->one_node || b == dag->one_node)
        {
            *out_id = dag->one_node;
            return ZYAN_STATUS_SUCCESS;
        }
        if (a == dag->zero_node)
        {
            *out_id = b;
            return ZYAN_STATUS_SUCCESS;
        }
        if (b == dag->zero_node)
        {
            *out_id = a;
            return ZYAN_STATUS_SUCCESS;
        }
        break;
    case ZYDIS_COMPACT_NODE_MASK_XOR:
        if (a == dag->zero_node)
        {
            *out_id = b;
            return ZYAN_STATUS_SUCCESS;
        }
        if (b == dag->zero_node)
        {
            *out_id = a;
            return ZYAN_STATUS_SUCCESS;
        }
        if (a == dag->one_node && b == dag->one_node)
        {
            *out_id = dag->zero_node;
            return ZYAN_STATUS_SUCCESS;
        }
        break;
    default:
        break;
    }

    ZydisCompactNode node = compact_blank(type);
    node.child[0] = a;
    node.child[1] = b;
    return compact_add_node(dag, &node, out_id);
}

static ZyanStatus compact_mk_shift(ZydisCompactDag* dag, ZydisCompactNodeType type,
    ZyanU32 child, ZyanU8 amount, ZyanU32* out_id)
{
    if (amount == 0u)
    {
        *out_id = child;
        return ZYAN_STATUS_SUCCESS;
    }
    if (amount >= ZYDIS_COMPACT_MASK_BITS)
    {
        *out_id = dag->zero_node;
        return ZYAN_STATUS_SUCCESS;
    }
    if (child == dag->zero_node)
    {
        *out_id = dag->zero_node;
        return ZYAN_STATUS_SUCCESS;
    }
    ZydisCompactNode node = compact_blank(type);
    node.child[0] = child;
    node.lane = amount;
    return compact_add_node(dag, &node, out_id);
}

/**
 * Builds one rank-chain node: the prefix popcount of the low `prefix` mask bits.
 * `prev` is the rank node for `prefix - 1` bits and `bit` is the condition node of mask
 * bit `prefix - 1`. The base node (`prefix == 0`) evaluates to zero.
 */
static ZyanStatus compact_mk_rank(ZydisCompactDag* dag, ZyanU32 prev, ZyanU32 bit,
    ZyanU8 prefix, ZyanU32* out_id)
{
    ZydisCompactNode node = compact_blank(ZYDIS_COMPACT_NODE_RANK);
    node.child[0] = prev;
    node.child[1] = bit;
    node.width = prefix;
    return compact_add_node(dag, &node, out_id);
}

static ZyanStatus compact_mk_eq(ZydisCompactDag* dag, ZyanU32 rank_node, ZyanU8 constant,
    ZyanU32* out_id)
{
    ZydisCompactNode node = compact_blank(ZYDIS_COMPACT_NODE_EQ);
    node.child[0] = rank_node;
    node.lane = constant;
    return compact_add_node(dag, &node, out_id);
}

static ZyanStatus compact_mk_select(ZydisCompactDag* dag, ZyanU32 cond, ZyanU32 on,
    ZyanU32 off, ZyanU32* out_id)
{
    if (cond == dag->one_node)
    {
        *out_id = on;
        return ZYAN_STATUS_SUCCESS;
    }
    if (cond == dag->zero_node)
    {
        *out_id = off;
        return ZYAN_STATUS_SUCCESS;
    }
    ZydisCompactNode node = compact_blank(ZYDIS_COMPACT_NODE_SELECT);
    node.child[0] = cond;
    node.child[1] = on;
    node.child[2] = off;
    return compact_add_node(dag, &node, out_id);
}

/* ============================================================================================== */
/* Lifecycle                                                                                      */
/* ============================================================================================== */

ZyanStatus ZydisCompactDagCreate(ZydisCompactDag** out_dag)
{
    if (out_dag == ZYAN_NULL)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZydisCompactDag* const dag = (ZydisCompactDag*)calloc(1u, sizeof(*dag));
    if (dag == ZYAN_NULL)
    {
        return ZYAN_STATUS_NOT_ENOUGH_MEMORY;
    }

    ZydisCompactNode zero = compact_blank(ZYDIS_COMPACT_NODE_ZERO);
    ZydisCompactNode one = compact_blank(ZYDIS_COMPACT_NODE_ONE);
    ZyanStatus status = compact_add_node(dag, &zero, &dag->zero_node);
    if (ZYAN_SUCCESS(status))
    {
        status = compact_add_node(dag, &one, &dag->one_node);
    }
    if (!ZYAN_SUCCESS(status))
    {
        free(dag->nodes);
        free(dag);
        return status;
    }

    // Entry vector state: every lane is its own identity leaf.
    status = compact_grow((void**)&dag->vec, &dag->vec_capacity, 1u, sizeof(*dag->vec));
    if (ZYAN_SUCCESS(status))
    {
        for (ZyanU8 z = 0; z < ZYDIS_COMPACT_ZMM_COUNT; ++z)
        {
            for (ZyanU8 lane = 0; lane < ZYDIS_COMPACT_MAX_LANES; ++lane)
            {
                status = compact_mk_entry_lane(dag, z, lane,
                    &dag->vec[0].lane[(ZyanUSize)z * ZYDIS_COMPACT_MAX_LANES + lane]);
                if (!ZYAN_SUCCESS(status))
                {
                    break;
                }
            }
            if (!ZYAN_SUCCESS(status))
            {
                break;
            }
        }
    }
    dag->vec_count = 1u;

    // Entry mask state: every bit is its own symbolic variable.
    if (ZYAN_SUCCESS(status))
    {
        status = compact_grow((void**)&dag->mask, &dag->mask_capacity, 1u,
            sizeof(*dag->mask));
    }
    if (ZYAN_SUCCESS(status))
    {
        for (ZyanU8 k = 0; k < ZYDIS_COMPACT_K_COUNT; ++k)
        {
            for (ZyanU8 bit = 0; bit < ZYDIS_COMPACT_MASK_BITS; ++bit)
            {
                status = compact_mk_entry_mask(dag, k, bit,
                    &dag->mask[0].bit[(ZyanUSize)k * ZYDIS_COMPACT_MASK_BITS + bit]);
                if (!ZYAN_SUCCESS(status))
                {
                    break;
                }
            }
            if (!ZYAN_SUCCESS(status))
            {
                break;
            }
        }
    }
    dag->mask_count = 1u;

    if (!ZYAN_SUCCESS(status))
    {
        ZydisCompactDagDestroy(dag);
        return status;
    }

    *out_dag = dag;
    return ZYAN_STATUS_SUCCESS;
}

void ZydisCompactDagDestroy(ZydisCompactDag* dag)
{
    if (dag == ZYAN_NULL)
    {
        return;
    }
    free(dag->nodes);
    free(dag->vec);
    free(dag->mask);
    free(dag->mem);
    free(dag->regions);
    free(dag->instrs);
    free(dag);
}

/* ============================================================================================== */
/* Regions and memory                                                                             */
/* ============================================================================================== */

ZyanStatus ZydisCompactDagAddRegion(ZydisCompactDag* dag, ZyanU64 address, ZyanUSize cells)
{
    if ((dag == ZYAN_NULL) || (cells == 0u))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if ((address & 3u) != 0u)
    {
        return ZYDIS_STATUS_COMPACT_MISALIGNED;
    }
    if (dag->region_count >= ZYDIS_COMPACT_REGION_LIMIT)
    {
        return ZYDIS_STATUS_COMPACT_LIMIT;
    }

    // The cell span must not wrap the 64-bit address space.
    const ZyanU64 byte_size = (ZyanU64)cells * 4u;
    if (byte_size / 4u != (ZyanU64)cells || (address + byte_size) < address)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }
    const ZyanU64 end = address + byte_size;

    for (ZyanUSize i = 0; i < dag->region_count; ++i)
    {
        const ZydisCompactRegionInternal* const r = &dag->regions[i];
        const ZyanU64 r_end = r->address + (ZyanU64)r->cells * 4u;
        if ((address < r_end) && (r->address < end))
        {
            return ZYAN_STATUS_OUT_OF_RANGE;
        }
    }

    ZYAN_CHECK(compact_grow((void**)&dag->regions, &dag->region_capacity,
        dag->region_count + 1u, sizeof(*dag->regions)));
    dag->regions[dag->region_count].address = address;
    dag->regions[dag->region_count].cells = cells;
    ++dag->region_count;
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisCompactResolveRegion(const ZydisCompactDag* dag, ZyanU64 address,
    const ZydisCompactRegionInternal** out_region, ZyanUSize* out_index)
{
    for (ZyanUSize i = 0; i < dag->region_count; ++i)
    {
        const ZydisCompactRegionInternal* const r = &dag->regions[i];
        const ZyanU64 byte_size = (ZyanU64)r->cells * 4u;
        if ((address >= r->address) && (address < r->address + byte_size))
        {
            if (out_region != ZYAN_NULL)
            {
                *out_region = r;
            }
            if (out_index != ZYAN_NULL)
            {
                *out_index = (ZyanUSize)((address - r->address) / 4u);
            }
            return ZYAN_STATUS_SUCCESS;
        }
    }
    if (out_region != ZYAN_NULL)
    {
        *out_region = ZYAN_NULL;
    }
    return ZYDIS_STATUS_COMPACT_NO_REGION;
}

ZyanStatus ZydisCompactMemRead(ZydisCompactDag* dag, ZyanU64 address, ZyanU32* out_node)
{
    if ((address & 3u) != 0u)
    {
        return ZYDIS_STATUS_COMPACT_MISALIGNED;
    }
    const ZydisCompactRegionInternal* region;
    ZYAN_CHECK(ZydisCompactResolveRegion(dag, address, &region, ZYAN_NULL));

    // Newest record wins.
    for (ZyanU32 i = dag->mem_count; i > 0u; --i)
    {
        if (dag->mem[i - 1u].address == address)
        {
            *out_node = dag->mem[i - 1u].node;
            return ZYAN_STATUS_SUCCESS;
        }
    }

    // First touch: materialize the identity leaf of the entry cell.
    if (dag->mem_count >= ZYDIS_COMPACT_MEMORY_LIMIT)
    {
        return ZYDIS_STATUS_COMPACT_LIMIT;
    }
    ZYAN_CHECK(compact_grow((void**)&dag->mem, &dag->mem_capacity,
        (ZyanUSize)dag->mem_count + 1u, sizeof(*dag->mem)));
    ZyanU32 leaf;
    ZYAN_CHECK(compact_mk_entry_mem(dag, address, &leaf));
    dag->mem[dag->mem_count].address = address;
    dag->mem[dag->mem_count].node = leaf;
    dag->mem[dag->mem_count].version = 0u;
    ++dag->mem_count;
    *out_node = leaf;
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisCompactMemWrite(ZydisCompactDag* dag, ZyanU64 address, ZyanU32 node,
    ZyanU32 version)
{
    if ((address & 3u) != 0u)
    {
        return ZYDIS_STATUS_COMPACT_MISALIGNED;
    }
    const ZydisCompactRegionInternal* region;
    ZYAN_CHECK(ZydisCompactResolveRegion(dag, address, &region, ZYAN_NULL));
    if (dag->mem_count >= ZYDIS_COMPACT_MEMORY_LIMIT)
    {
        return ZYDIS_STATUS_COMPACT_LIMIT;
    }
    ZYAN_CHECK(compact_grow((void**)&dag->mem, &dag->mem_capacity,
        (ZyanUSize)dag->mem_count + 1u, sizeof(*dag->mem)));
    dag->mem[dag->mem_count].address = address;
    dag->mem[dag->mem_count].node = node;
    dag->mem[dag->mem_count].version = version;
    ++dag->mem_count;
    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* State bookkeeping                                                                              */
/* ============================================================================================== */

ZydisCompactVecVersion* ZydisCompactCurVec(ZydisCompactDag* dag)
{
    return &dag->vec[dag->vec_count - 1u];
}

ZydisCompactMaskVersion* ZydisCompactCurMask(ZydisCompactDag* dag)
{
    return &dag->mask[dag->mask_count - 1u];
}

static ZyanStatus compact_append_state(ZydisCompactDag* dag)
{
    if (dag->instr_count >= ZYDIS_COMPACT_INSTRUCTION_LIMIT)
    {
        return ZYDIS_STATUS_COMPACT_LIMIT;
    }
    ZYAN_CHECK(compact_grow((void**)&dag->vec, &dag->vec_capacity,
        (ZyanUSize)dag->vec_count + 1u, sizeof(*dag->vec)));
    ZYAN_CHECK(compact_grow((void**)&dag->mask, &dag->mask_capacity,
        (ZyanUSize)dag->mask_count + 1u, sizeof(*dag->mask)));
    dag->vec[dag->vec_count] = dag->vec[dag->vec_count - 1u];
    dag->mask[dag->mask_count] = dag->mask[dag->mask_count - 1u];
    ++dag->vec_count;
    ++dag->mask_count;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus compact_commit_instr(ZydisCompactDag* dag, const ZydisDecodedInstruction* ins,
    ZyanU64 runtime_address)
{
    ZYAN_CHECK(compact_grow((void**)&dag->instrs, &dag->instr_capacity,
        (ZyanUSize)dag->instr_count + 1u, sizeof(*dag->instrs)));
    dag->instrs[dag->instr_count].address = runtime_address;
    dag->instrs[dag->instr_count].length = ins->length;
    ++dag->instr_count;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus compact_fail_unsupported(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, ZyanU64 runtime_address)
{
    dag->poisoned = ZYAN_TRUE;
    dag->has_unsupported = ZYAN_TRUE;
    dag->unsupported_address = runtime_address;
    dag->unsupported_length = (ins != ZYAN_NULL) ? ins->length : 0u;
    return ZYDIS_STATUS_COMPACT_UNSUPPORTED;
}

/* ============================================================================================== */
/* Operand helpers                                                                                */
/* ============================================================================================== */

static ZyanBool compact_is_mask_reg(ZydisRegister reg)
{
    return ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_MASK;
}

static ZyanBool compact_is_vector_reg(ZydisRegister reg)
{
    const ZydisRegisterClass cls = ZydisRegisterGetClass(reg);
    return (cls == ZYDIS_REGCLASS_XMM) || (cls == ZYDIS_REGCLASS_YMM) ||
           (cls == ZYDIS_REGCLASS_ZMM);
}

/**
 * Resolves the per-lane condition node of a vector write mask. A k0 mask means "every lane
 * selected" and never reads the symbolic k0 bits.
 */
static ZyanStatus compact_mask_condition(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, ZyanU8 lane, ZyanU32* out_node)
{
    const ZydisRegister mask_reg = ins->avx.mask.reg;
    if ((mask_reg == ZYDIS_REGISTER_K0) ||
        (ins->avx.mask.mode == ZYDIS_MASK_MODE_DISABLED) ||
        (ins->avx.mask.mode == ZYDIS_MASK_MODE_NONE))
    {
        *out_node = dag->one_node;
        return ZYAN_STATUS_SUCCESS;
    }
    if (!compact_is_mask_reg(mask_reg))
    {
        return ZYDIS_STATUS_COMPACT_UNSUPPORTED;
    }
    const ZyanI32 kid = ZydisRegisterGetId(mask_reg);
    if ((kid < 0) || ((ZyanU32)kid >= ZYDIS_COMPACT_K_COUNT))
    {
        return ZYDIS_STATUS_COMPACT_UNSUPPORTED;
    }
    *out_node = ZydisCompactCurMask(dag)->bit[(ZyanUSize)kid *
        ZYDIS_COMPACT_MASK_BITS + lane];
    return ZYAN_STATUS_SUCCESS;
}

static ZyanBool compact_is_zeroing(const ZydisDecodedInstruction* ins)
{
    return (ins->avx.mask.mode == ZYDIS_MASK_MODE_ZEROING) ||
           (ins->avx.mask.mode == ZYDIS_MASK_MODE_CONTROL_ZEROING);
}

static ZyanBool compact_mask_is_active(const ZydisDecodedInstruction* ins)
{
    return (ins->avx.mask.reg != ZYDIS_REGISTER_K0) &&
           (ins->avx.mask.mode != ZYDIS_MASK_MODE_DISABLED) &&
           (ins->avx.mask.mode != ZYDIS_MASK_MODE_NONE);
}

/**
 * Builds the rank chain for a vector instruction with `lanes` active lanes. `ranks[i]`, for
 * `i` in 0..lanes, evaluates to the popcount of mask bits 0..i-1. Mask bits beyond the
 * valid lane count never enter the chain. The caller provides `lanes + 1` slots.
 */
static ZyanStatus compact_build_ranks(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, ZyanU8 lanes, ZyanU32* ranks)
{
    ZYAN_CHECK(compact_mk_rank(dag, dag->zero_node, dag->zero_node, 0u, &ranks[0]));
    for (ZyanU8 i = 1u; i <= lanes; ++i)
    {
        ZyanU32 bit;
        ZYAN_CHECK(compact_mask_condition(dag, ins, (ZyanU8)(i - 1u), &bit));
        ZYAN_CHECK(compact_mk_rank(dag, ranks[i - 1u], bit, i, &ranks[i]));
    }
    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* Mask instructions                                                                              */
/* ============================================================================================== */

static ZyanStatus compact_handle_k_instr(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* ops,
    ZyanU64 runtime_address)
{
    const ZydisDecodedOperand* dst = ZYAN_NULL;
    const ZydisDecodedOperand* src1 = ZYAN_NULL;
    const ZydisDecodedOperand* src2 = ZYAN_NULL;
    const ZydisDecodedOperand* imm = ZYAN_NULL;

    for (ZyanU8 i = 0u; i < ins->operand_count; ++i)
    {
        const ZydisDecodedOperand* const op = &ops[i];
        if (op->visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN)
        {
            continue;
        }
        if (op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
        {
            imm = op;
            continue;
        }
        if (op->type != ZYDIS_OPERAND_TYPE_REGISTER ||
            !compact_is_mask_reg(op->reg.value))
        {
            return compact_fail_unsupported(dag, ins, runtime_address);
        }
        if ((op->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0u)
        {
            dst = op;
        }
        else if (src1 == ZYAN_NULL)
        {
            src1 = op;
        }
        else
        {
            src2 = op;
        }
    }

    if (dst == ZYAN_NULL || src1 == ZYAN_NULL)
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    const ZyanI32 dst_id = ZydisRegisterGetId(dst->reg.value);
    const ZyanI32 s1_id = ZydisRegisterGetId(src1->reg.value);
    if ((dst_id < 0) || ((ZyanU32)dst_id >= ZYDIS_COMPACT_K_COUNT) ||
        (s1_id < 0) || ((ZyanU32)s1_id >= ZYDIS_COMPACT_K_COUNT))
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    // Snapshot the source state first: dst and src may be the same register.
    const ZydisCompactMaskVersion old_mask = *ZydisCompactCurMask(dag);
    ZYAN_CHECK(compact_append_state(dag));
    ZydisCompactMaskVersion* const state = ZydisCompactCurMask(dag);

    switch (ins->mnemonic)
    {
    case ZYDIS_MNEMONIC_KMOVW:
        for (ZyanU8 bit = 0u; bit < ZYDIS_COMPACT_MASK_BITS; ++bit)
        {
            state->bit[(ZyanUSize)dst_id * ZYDIS_COMPACT_MASK_BITS + bit] =
                old_mask.bit[(ZyanUSize)s1_id * ZYDIS_COMPACT_MASK_BITS + bit];
        }
        break;
    case ZYDIS_MNEMONIC_KANDW:
    case ZYDIS_MNEMONIC_KORW:
    case ZYDIS_MNEMONIC_KXORW:
    {
        if (src2 == ZYAN_NULL)
        {
            return compact_fail_unsupported(dag, ins, runtime_address);
        }
        const ZyanI32 s2_id = ZydisRegisterGetId(src2->reg.value);
        if ((s2_id < 0) || ((ZyanU32)s2_id >= ZYDIS_COMPACT_K_COUNT))
        {
            return compact_fail_unsupported(dag, ins, runtime_address);
        }
        const ZydisCompactNodeType type =
            (ins->mnemonic == ZYDIS_MNEMONIC_KANDW)
                ? ZYDIS_COMPACT_NODE_MASK_AND
                : (ins->mnemonic == ZYDIS_MNEMONIC_KORW)
                    ? ZYDIS_COMPACT_NODE_MASK_OR
                    : ZYDIS_COMPACT_NODE_MASK_XOR;
        for (ZyanU8 bit = 0u; bit < ZYDIS_COMPACT_MASK_BITS; ++bit)
        {
            ZyanU32 node;
            ZYAN_CHECK(compact_mk_binary(dag, type,
                old_mask.bit[(ZyanUSize)s1_id * ZYDIS_COMPACT_MASK_BITS + bit],
                old_mask.bit[(ZyanUSize)s2_id * ZYDIS_COMPACT_MASK_BITS + bit], &node));
            state->bit[(ZyanUSize)dst_id * ZYDIS_COMPACT_MASK_BITS + bit] = node;
        }
        break;
    }
    case ZYDIS_MNEMONIC_KNOTW:
        for (ZyanU8 bit = 0u; bit < ZYDIS_COMPACT_MASK_BITS; ++bit)
        {
            ZyanU32 node;
            ZYAN_CHECK(compact_mk_unary(dag, ZYDIS_COMPACT_NODE_MASK_NOT,
                old_mask.bit[(ZyanUSize)s1_id * ZYDIS_COMPACT_MASK_BITS + bit], &node));
            state->bit[(ZyanUSize)dst_id * ZYDIS_COMPACT_MASK_BITS + bit] = node;
        }
        break;
    case ZYDIS_MNEMONIC_KSHIFTLW:
    case ZYDIS_MNEMONIC_KSHIFTRW:
    {
        if (imm == ZYAN_NULL)
        {
            return compact_fail_unsupported(dag, ins, runtime_address);
        }
        const ZyanU8 amount = (ZyanU8)(imm->imm.value.u & 0xFFFFu);
        const ZydisCompactNodeType type =
            (ins->mnemonic == ZYDIS_MNEMONIC_KSHIFTLW)
                ? ZYDIS_COMPACT_NODE_MASK_SHLL
                : ZYDIS_COMPACT_NODE_MASK_SHRL;
        for (ZyanU8 bit = 0u; bit < ZYDIS_COMPACT_MASK_BITS; ++bit)
        {
            ZyanU32 source = dag->zero_node;
            if (ins->mnemonic == ZYDIS_MNEMONIC_KSHIFTLW)
            {
                if ((ZyanU32)bit >= amount &&
                    (ZyanU32)bit - amount < ZYDIS_COMPACT_MASK_BITS)
                {
                    source = old_mask.bit[(ZyanUSize)s1_id *
                        ZYDIS_COMPACT_MASK_BITS + (bit - amount)];
                }
            }
            else
            {
                if ((ZyanU32)bit + amount < ZYDIS_COMPACT_MASK_BITS)
                {
                    source = old_mask.bit[(ZyanUSize)s1_id *
                        ZYDIS_COMPACT_MASK_BITS + (bit + amount)];
                }
            }
            ZyanU32 node;
            // The shift node retains the symbolic operation; the vacated position itself
            // is represented by routing the corresponding source bit (or ZERO) through it.
            ZYAN_CHECK(compact_mk_shift(dag, type, source, amount, &node));
            state->bit[(ZyanUSize)dst_id * ZYDIS_COMPACT_MASK_BITS + bit] = node;
        }
        break;
    }
    default:
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* VMOVDQU32                                                                                      */
/* ============================================================================================== */

static ZyanStatus compact_handle_move(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* ops,
    ZyanU64 runtime_address)
{
    const ZydisDecodedOperand* dst = ZYAN_NULL;
    const ZydisDecodedOperand* src = ZYAN_NULL;
    for (ZyanU8 i = 0u; i < ins->operand_count; ++i)
    {
        const ZydisDecodedOperand* const op = &ops[i];
        if (op->visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN)
        {
            continue;
        }
        if (op->type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            return compact_fail_unsupported(dag, ins, runtime_address);
        }
        if (op->type != ZYDIS_OPERAND_TYPE_REGISTER || compact_is_mask_reg(op->reg.value))
        {
            continue;
        }
        if ((op->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0u)
        {
            dst = op;
        }
        else
        {
            src = op;
        }
    }

    if (dst == ZYAN_NULL || src == ZYAN_NULL)
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    const ZyanU16 lanes = ins->avx.vector_length / 32u;
    if ((lanes != 4u) && (lanes != 8u) && (lanes != 16u))
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    const ZyanI32 dst_zmm = ZydisRegisterGetId(dst->reg.value);
    const ZyanI32 src_zmm = ZydisRegisterGetId(src->reg.value);
    if ((dst_zmm < 0) || (src_zmm < 0))
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    // Capture the old versions before appending the new state (src and dst may be the same
    // register).
    const ZydisCompactVecVersion old_vec = *ZydisCompactCurVec(dag);
    ZYAN_CHECK(compact_append_state(dag));
    ZydisCompactVecVersion* const new_vec = ZydisCompactCurVec(dag);

    const ZyanBool zeroing = compact_is_zeroing(ins);
    const ZyanBool active = compact_mask_is_active(ins);
    for (ZyanU8 lane = 0u; lane < (ZyanU8)lanes; ++lane)
    {
        const ZyanU32 src_node = old_vec.lane[(ZyanUSize)src_zmm *
            ZYDIS_COMPACT_MAX_LANES + lane];
        ZyanU32 result;
        if (!active)
        {
            result = src_node;
        }
        else
        {
            ZyanU32 cond;
            ZYAN_CHECK(compact_mask_condition(dag, ins, lane, &cond));
            const ZyanU32 fallback = zeroing
                ? dag->zero_node
                : old_vec.lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane];
            ZYAN_CHECK(compact_mk_select(dag, cond, src_node, fallback, &result));
        }
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] = result;
    }
    // A narrow vector write zeroes the upper lanes of the enclosing ZMM.
    for (ZyanU8 lane = (ZyanU8)lanes; lane < ZYDIS_COMPACT_MAX_LANES; ++lane)
    {
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] =
            dag->zero_node;
    }
    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* Memory addressing                                                                              */
/* ============================================================================================== */

static ZyanStatus compact_resolve_memory(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* op,
    ZyanU64 runtime_address, ZyanU64* out_address)
{
    if (op->type != ZYDIS_OPERAND_TYPE_MEMORY)
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }
    const ZyanBool rip_relative = (op->mem.base == ZYDIS_REGISTER_RIP) ||
                                  (op->mem.base == ZYDIS_REGISTER_EIP);
    const ZyanBool bare_disp = (op->mem.base == ZYDIS_REGISTER_NONE) &&
                               (op->mem.index == ZYDIS_REGISTER_NONE);
    if (!rip_relative && !bare_disp)
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }
    ZyanU64 address;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(ins, op, runtime_address, &address)))
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }
    if ((address & 3u) != 0u)
    {
        return ZYDIS_STATUS_COMPACT_MISALIGNED;
    }
    *out_address = address;
    return ZYAN_STATUS_SUCCESS;
}

/**
 * Verifies that the whole dword span `base .. base + lanes*4` lies inside registered
 * regions. Regions are disjoint so each cell is covered at most once.
 */
static ZyanStatus compact_check_span(ZydisCompactDag* dag, ZyanU64 base, ZyanU8 lanes)
{
    for (ZyanU8 lane = 0u; lane < lanes; ++lane)
    {
        const ZydisCompactRegionInternal* region;
        const ZyanStatus st = ZydisCompactResolveRegion(dag,
            base + (ZyanU64)lane * 4u, &region, ZYAN_NULL);
        if (!ZYAN_SUCCESS(st))
        {
            return st;
        }
    }
    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* VPCOMPRESSD / VPEXPANDD                                                                        */
/* ============================================================================================== */

/**
 * Maps a source index to its provenance node.
 */
typedef ZyanStatus (*ZydisCompactSrcReader)(ZydisCompactDag* dag, ZyanU8 index,
    void* user_data, ZyanU32* out_node);

/**
 * Compression pick for destination position `position` together with the predicate that the
 * position belongs to the compact prefix.
 *
 * A source lane `i` is placed at destination index `ranks[i]` (the popcount of active bits
 * below it) only when the lane's own mask bit is set. The per-lane condition is therefore
 * `mask[i] AND (ranks[i] == position)`. At most one active lane has any given rank, so the
 * select chain yields exactly one source; `*out_exists` is the OR of all the conditions.
 */
static ZyanStatus compact_compress_pick(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZyanU32* ranks, ZyanU8 lanes,
    ZyanU8 position, ZydisCompactSrcReader reader, void* user_data,
    ZyanU32* out_picked, ZyanU32* out_exists)
{
    ZyanU32 picked = dag->zero_node;
    ZyanU32 exists = dag->zero_node;
    for (ZyanI32 i = (ZyanI32)lanes - 1; i >= 0; --i)
    {
        ZyanU32 src_node;
        ZYAN_CHECK(reader(dag, (ZyanU8)i, user_data, &src_node));
        ZyanU32 eq;
        ZYAN_CHECK(compact_mk_eq(dag, ranks[i], position, &eq));
        ZyanU32 bit;
        ZYAN_CHECK(compact_mask_condition(dag, ins, (ZyanU8)i, &bit));
        ZyanU32 cond;
        ZYAN_CHECK(compact_mk_binary(dag, ZYDIS_COMPACT_NODE_MASK_AND, bit, eq, &cond));
        ZYAN_CHECK(compact_mk_select(dag, cond, src_node, picked, &picked));
        ZYAN_CHECK(compact_mk_binary(dag, ZYDIS_COMPACT_NODE_MASK_OR, exists, cond,
            &exists));
    }
    *out_picked = picked;
    *out_exists = exists;
    return ZYAN_STATUS_SUCCESS;
}

/**
 * Expansion pick for destination position `position`: the source index is the rank of
 * `position` itself (the popcount of mask bits below it). The EQ node compares that rank
 * against each concrete index 0..`position`; at most one matches.
 */
static ZyanStatus compact_expand_pick(ZydisCompactDag* dag, const ZyanU32* ranks,
    ZyanU8 position, ZydisCompactSrcReader reader, void* user_data, ZyanU32* out_node)
{
    ZyanU32 result = dag->zero_node;
    for (ZyanI32 r = (ZyanI32)position; r >= 0; --r)
    {
        ZyanU32 src_node;
        ZYAN_CHECK(reader(dag, (ZyanU8)r, user_data, &src_node));
        ZyanU32 eq;
        ZYAN_CHECK(compact_mk_eq(dag, ranks[position], (ZyanU8)r, &eq));
        ZYAN_CHECK(compact_mk_select(dag, eq, src_node, result, &result));
    }
    *out_node = result;
    return ZYAN_STATUS_SUCCESS;
}

typedef struct ZydisCompactVecSrc_
{
    const ZydisCompactVecVersion* vec;
    ZyanU8 zmm;
} ZydisCompactVecSrc;

static ZyanStatus compact_vec_reader(ZydisCompactDag* dag, ZyanU8 index, void* user_data,
    ZyanU32* out_node)
{
    (void)dag;
    const ZydisCompactVecSrc* const src = (const ZydisCompactVecSrc*)user_data;
    *out_node = src->vec->lane[(ZyanUSize)src->zmm * ZYDIS_COMPACT_MAX_LANES + index];
    return ZYAN_STATUS_SUCCESS;
}

typedef struct ZydisCompactMemSrc_
{
    ZyanU64 base;
} ZydisCompactMemSrc;

static ZyanStatus compact_mem_reader(ZydisCompactDag* dag, ZyanU8 index, void* user_data,
    ZyanU32* out_node)
{
    const ZydisCompactMemSrc* const src = (const ZydisCompactMemSrc*)user_data;
    return ZydisCompactMemRead(dag, src->base + (ZyanU64)index * 4u, out_node);
}

static ZyanStatus compact_handle_compress_reg(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* dst_op,
    const ZydisDecodedOperand* src_op)
{
    const ZyanU8 lanes = (ZyanU8)(ins->avx.vector_length / 32u);
    const ZyanI32 dst_zmm = ZydisRegisterGetId(dst_op->reg.value);
    const ZyanI32 src_zmm = ZydisRegisterGetId(src_op->reg.value);
    if ((dst_zmm < 0) || (src_zmm < 0))
    {
        return ZYDIS_STATUS_COMPACT_UNSUPPORTED;
    }

    const ZydisCompactVecVersion old_vec = *ZydisCompactCurVec(dag);
    ZyanU32 ranks[ZYDIS_COMPACT_MAX_LANES + 1u];
    const ZyanBool active = compact_mask_is_active(ins);
    if (active)
    {
        ZYAN_CHECK(compact_build_ranks(dag, ins, lanes, ranks));
    }

    ZYAN_CHECK(compact_append_state(dag));
    ZydisCompactVecVersion* const new_vec = ZydisCompactCurVec(dag);
    const ZyanBool zeroing = compact_is_zeroing(ins);

    ZydisCompactVecSrc reader_data = { &old_vec, (ZyanU8)src_zmm };
    for (ZyanU8 lane = 0u; lane < lanes; ++lane)
    {
        ZyanU32 result;
        if (!active)
        {
            // Unmasked compaction is an in-order copy.
            result = old_vec.lane[(ZyanUSize)src_zmm * ZYDIS_COMPACT_MAX_LANES + lane];
        }
        else
        {
            ZyanU32 picked;
            ZyanU32 exists;
            ZYAN_CHECK(compact_compress_pick(dag, ins, ranks, lanes, lane,
                compact_vec_reader, &reader_data, &picked, &exists));
            const ZyanU32 fallback = zeroing
                ? dag->zero_node
                : old_vec.lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane];
            ZYAN_CHECK(compact_mk_select(dag, exists, picked, fallback, &result));
        }
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] = result;
    }
    for (ZyanU8 lane = lanes; lane < ZYDIS_COMPACT_MAX_LANES; ++lane)
    {
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] =
            dag->zero_node;
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus compact_handle_compress_mem(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* mem_op,
    const ZydisDecodedOperand* src_op, ZyanU64 runtime_address)
{
    const ZyanU8 lanes = (ZyanU8)(ins->avx.vector_length / 32u);
    const ZyanI32 src_zmm = ZydisRegisterGetId(src_op->reg.value);
    if (src_zmm < 0)
    {
        return ZYDIS_STATUS_COMPACT_UNSUPPORTED;
    }
    ZyanU64 base;
    ZYAN_CHECK(compact_resolve_memory(dag, ins, mem_op, runtime_address, &base));
    ZYAN_CHECK(compact_check_span(dag, base, lanes));

    const ZydisCompactVecVersion old_vec = *ZydisCompactCurVec(dag);
    ZyanU32 ranks[ZYDIS_COMPACT_MAX_LANES + 1u];
    const ZyanBool active = compact_mask_is_active(ins);
    if (active)
    {
        ZYAN_CHECK(compact_build_ranks(dag, ins, lanes, ranks));
    }

    ZYAN_CHECK(compact_append_state(dag));
    const ZyanU32 version = dag->instr_count + 1u;

    if (!active)
    {
        for (ZyanU8 lane = 0u; lane < lanes; ++lane)
        {
            ZYAN_CHECK(ZydisCompactMemWrite(dag, base + (ZyanU64)lane * 4u,
                old_vec.lane[(ZyanUSize)src_zmm * ZYDIS_COMPACT_MAX_LANES + lane],
                version));
        }
        return ZYAN_STATUS_SUCCESS;
    }

    // Only the compact prefix of length popcount(mask) is overwritten; every touched cell is
    // expressed conditionally so positions beyond the prefix keep their previous version.
    ZydisCompactVecSrc reader_data = { &old_vec, (ZyanU8)src_zmm };
    for (ZyanU8 lane = 0u; lane < lanes; ++lane)
    {
        ZyanU32 picked;
        ZyanU32 exists;
        ZYAN_CHECK(compact_compress_pick(dag, ins, ranks, lanes, lane,
            compact_vec_reader, &reader_data, &picked, &exists));
        ZyanU32 previous;
        ZYAN_CHECK(ZydisCompactMemRead(dag, base + (ZyanU64)lane * 4u, &previous));
        ZyanU32 merged;
        ZYAN_CHECK(compact_mk_select(dag, exists, picked, previous, &merged));
        ZYAN_CHECK(ZydisCompactMemWrite(dag, base + (ZyanU64)lane * 4u, merged, version));
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus compact_handle_expand_reg(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* dst_op,
    const ZydisDecodedOperand* src_op)
{
    const ZyanU8 lanes = (ZyanU8)(ins->avx.vector_length / 32u);
    const ZyanI32 dst_zmm = ZydisRegisterGetId(dst_op->reg.value);
    const ZyanI32 src_zmm = ZydisRegisterGetId(src_op->reg.value);
    if ((dst_zmm < 0) || (src_zmm < 0))
    {
        return ZYDIS_STATUS_COMPACT_UNSUPPORTED;
    }

    const ZydisCompactVecVersion old_vec = *ZydisCompactCurVec(dag);
    ZyanU32 ranks[ZYDIS_COMPACT_MAX_LANES + 1u];
    const ZyanBool active = compact_mask_is_active(ins);
    if (active)
    {
        ZYAN_CHECK(compact_build_ranks(dag, ins, lanes, ranks));
    }

    ZYAN_CHECK(compact_append_state(dag));
    ZydisCompactVecVersion* const new_vec = ZydisCompactCurVec(dag);
    const ZyanBool zeroing = compact_is_zeroing(ins);

    ZydisCompactVecSrc reader_data = { &old_vec, (ZyanU8)src_zmm };
    for (ZyanU8 lane = 0u; lane < lanes; ++lane)
    {
        ZyanU32 result;
        if (!active)
        {
            result = old_vec.lane[(ZyanUSize)src_zmm * ZYDIS_COMPACT_MAX_LANES + lane];
        }
        else
        {
            ZyanU32 picked;
            // Only source indices 0..lane can have been consumed at position `lane`.
            ZYAN_CHECK(compact_expand_pick(dag, ranks, lane, compact_vec_reader,
                &reader_data, &picked));
            ZyanU32 cond;
            ZYAN_CHECK(compact_mask_condition(dag, ins, lane, &cond));
            const ZyanU32 fallback = zeroing
                ? dag->zero_node
                : old_vec.lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane];
            ZYAN_CHECK(compact_mk_select(dag, cond, picked, fallback, &result));
        }
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] = result;
    }
    for (ZyanU8 lane = lanes; lane < ZYDIS_COMPACT_MAX_LANES; ++lane)
    {
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] =
            dag->zero_node;
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus compact_handle_expand_mem(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* dst_op,
    const ZydisDecodedOperand* mem_op, ZyanU64 runtime_address)
{
    const ZyanU8 lanes = (ZyanU8)(ins->avx.vector_length / 32u);
    const ZyanI32 dst_zmm = ZydisRegisterGetId(dst_op->reg.value);
    if (dst_zmm < 0)
    {
        return ZYDIS_STATUS_COMPACT_UNSUPPORTED;
    }
    ZyanU64 base;
    ZYAN_CHECK(compact_resolve_memory(dag, ins, mem_op, runtime_address, &base));
    ZYAN_CHECK(compact_check_span(dag, base, lanes));

    const ZydisCompactVecVersion old_vec = *ZydisCompactCurVec(dag);
    ZyanU32 ranks[ZYDIS_COMPACT_MAX_LANES + 1u];
    const ZyanBool active = compact_mask_is_active(ins);
    if (active)
    {
        ZYAN_CHECK(compact_build_ranks(dag, ins, lanes, ranks));
    }

    ZYAN_CHECK(compact_append_state(dag));
    ZydisCompactVecVersion* const new_vec = ZydisCompactCurVec(dag);
    const ZyanBool zeroing = compact_is_zeroing(ins);

    ZydisCompactMemSrc reader_data = { base };
    for (ZyanU8 lane = 0u; lane < lanes; ++lane)
    {
        ZyanU32 result;
        if (!active)
        {
            ZYAN_CHECK(ZydisCompactMemRead(dag, base + (ZyanU64)lane * 4u, &result));
        }
        else
        {
            ZyanU32 picked;
            ZYAN_CHECK(compact_expand_pick(dag, ranks, lane, compact_mem_reader,
                &reader_data, &picked));
            ZyanU32 cond;
            ZYAN_CHECK(compact_mask_condition(dag, ins, lane, &cond));
            const ZyanU32 fallback = zeroing
                ? dag->zero_node
                : old_vec.lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane];
            ZYAN_CHECK(compact_mk_select(dag, cond, picked, fallback, &result));
        }
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] = result;
    }
    for (ZyanU8 lane = lanes; lane < ZYDIS_COMPACT_MAX_LANES; ++lane)
    {
        new_vec->lane[(ZyanUSize)dst_zmm * ZYDIS_COMPACT_MAX_LANES + lane] =
            dag->zero_node;
    }
    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* Compaction dispatch                                                                            */
/* ============================================================================================== */

static ZyanStatus compact_handle_compaction(ZydisCompactDag* dag,
    const ZydisDecodedInstruction* ins, const ZydisDecodedOperand* ops,
    ZyanU64 runtime_address)
{
    const ZydisDecodedOperand* dst = ZYAN_NULL;
    const ZydisDecodedOperand* src = ZYAN_NULL;
    for (ZyanU8 i = 0u; i < ins->operand_count; ++i)
    {
        const ZydisDecodedOperand* const op = &ops[i];
        if (op->visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN)
        {
            continue;
        }
        if (op->type == ZYDIS_OPERAND_TYPE_REGISTER && compact_is_mask_reg(op->reg.value))
        {
            continue;
        }
        if ((op->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0u)
        {
            dst = op;
        }
        else
        {
            src = op;
        }
    }
    if (dst == ZYAN_NULL || src == ZYAN_NULL)
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    const ZyanU16 lanes = ins->avx.vector_length / 32u;
    if ((lanes != 4u) && (lanes != 8u) && (lanes != 16u))
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }
    if (dst->type == ZYDIS_OPERAND_TYPE_REGISTER &&
        !compact_is_vector_reg(dst->reg.value))
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }
    if (src->type == ZYDIS_OPERAND_TYPE_REGISTER &&
        !compact_is_vector_reg(src->reg.value))
    {
        return compact_fail_unsupported(dag, ins, runtime_address);
    }

    switch (ins->mnemonic)
    {
    case ZYDIS_MNEMONIC_VPCOMPRESSD:
        if (dst->type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            return compact_handle_compress_mem(dag, ins, dst, src, runtime_address);
        }
        return compact_handle_compress_reg(dag, ins, dst, src);
    case ZYDIS_MNEMONIC_VPEXPANDD:
        if (src->type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            return compact_handle_expand_mem(dag, ins, dst, src, runtime_address);
        }
        return compact_handle_expand_reg(dag, ins, dst, src);
    default:
        break;
    }
    return compact_fail_unsupported(dag, ins, runtime_address);
}

/* ============================================================================================== */
/* Public decode entry point                                                                      */
/* ============================================================================================== */

ZyanStatus ZydisCompactDagDecode(ZydisCompactDag* dag, const void* buffer, ZyanUSize length,
    ZyanU64 runtime_address)
{
    if ((dag == ZYAN_NULL) || (buffer == ZYAN_NULL) || (length == 0u))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (dag->poisoned)
    {
        return ZYAN_STATUS_INVALID_OPERATION;
    }

    ZydisDecoder decoder;
    ZYAN_CHECK(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_STACK_WIDTH_64));

    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    const ZyanStatus decode_status =
        ZydisDecoderDecodeFull(&decoder, buffer, length, &ins, ops);
    if (!ZYAN_SUCCESS(decode_status))
    {
        dag->poisoned = ZYAN_TRUE;
        dag->has_unsupported = ZYAN_TRUE;
        dag->unsupported_address = runtime_address;
        dag->unsupported_length = 0u;
        return decode_status;
    }

    ZyanStatus result;
    switch (ins.mnemonic)
    {
    case ZYDIS_MNEMONIC_KMOVW:
    case ZYDIS_MNEMONIC_KANDW:
    case ZYDIS_MNEMONIC_KORW:
    case ZYDIS_MNEMONIC_KXORW:
    case ZYDIS_MNEMONIC_KNOTW:
    case ZYDIS_MNEMONIC_KSHIFTLW:
    case ZYDIS_MNEMONIC_KSHIFTRW:
        result = compact_handle_k_instr(dag, &ins, ops, runtime_address);
        break;
    case ZYDIS_MNEMONIC_VMOVDQU32:
        result = compact_handle_move(dag, &ins, ops, runtime_address);
        break;
    case ZYDIS_MNEMONIC_VPCOMPRESSD:
    case ZYDIS_MNEMONIC_VPEXPANDD:
        result = compact_handle_compaction(dag, &ins, ops, runtime_address);
        break;
    default:
        result = compact_fail_unsupported(dag, &ins, runtime_address);
        break;
    }

    if (!ZYAN_SUCCESS(result))
    {
        /* A resource limit may fire after partial construction: refuse further decodes so
           a truncated graph is never published as if it were complete. */
        if (result == ZYDIS_STATUS_COMPACT_LIMIT)
        {
            dag->poisoned = ZYAN_TRUE;
        }
        return result;
    }
    return compact_commit_instr(dag, &ins, runtime_address);
}

/* ============================================================================================== */
/* Public queries                                                                                 */
/* ============================================================================================== */

ZyanU32 ZydisCompactDagInstructionCount(const ZydisCompactDag* dag)
{
    return dag->instr_count;
}

ZyanStatus ZydisCompactDagLastUnsupported(const ZydisCompactDag* dag,
    ZyanU32* instruction_index, ZyanU64* instruction_address, ZyanUSize* instruction_length)
{
    if ((dag == ZYAN_NULL) || !dag->has_unsupported)
    {
        return ZYAN_STATUS_NOT_FOUND;
    }
    if (instruction_index != ZYAN_NULL)
    {
        *instruction_index = dag->instr_count;
    }
    if (instruction_address != ZYAN_NULL)
    {
        *instruction_address = dag->unsupported_address;
    }
    if (instruction_length != ZYAN_NULL)
    {
        *instruction_length = dag->unsupported_length;
    }
    return ZYAN_STATUS_SUCCESS;
}

ZyanU32 ZydisCompactDagNodeCount(const ZydisCompactDag* dag)
{
    return dag->node_count;
}

ZyanStatus ZydisCompactDagGetNode(const ZydisCompactDag* dag, ZyanU32 node_id,
    ZydisCompactNodeInfo* info)
{
    if ((dag == ZYAN_NULL) || (info == ZYAN_NULL))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (node_id >= dag->node_count)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }
    const ZydisCompactNode* const node = &dag->nodes[node_id];
    info->type = node->type;
    for (ZyanU8 i = 0u; i < 3u; ++i)
    {
        info->child[i] = (node->child[i] == ZYDIS_COMPACT_INVALID_NODE)
            ? dag->zero_node : node->child[i];
    }
    info->lane = node->lane;
    info->zmm = node->zmm;
    info->k = node->k;
    info->width = node->width;
    info->address = node->address;
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisCompactDagGetLane(const ZydisCompactDag* dag, ZyanU32 instruction,
    ZyanU8 zmm, ZyanU8 lane, ZyanU32* out_node)
{
    if ((dag == ZYAN_NULL) || (out_node == ZYAN_NULL) ||
        (zmm >= ZYDIS_COMPACT_ZMM_COUNT) || (lane >= ZYDIS_COMPACT_MAX_LANES) ||
        (instruction > dag->instr_count))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    *out_node = dag->vec[instruction].lane[(ZyanUSize)zmm *
        ZYDIS_COMPACT_MAX_LANES + lane];
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisCompactDagGetMaskBit(const ZydisCompactDag* dag, ZyanU32 instruction,
    ZyanU8 k, ZyanU8 bit, ZyanU32* out_node)
{
    if ((dag == ZYAN_NULL) || (out_node == ZYAN_NULL) ||
        (k >= ZYDIS_COMPACT_K_COUNT) || (bit >= ZYDIS_COMPACT_MASK_BITS) ||
        (instruction > dag->instr_count))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    *out_node = dag->mask[instruction].bit[(ZyanUSize)k *
        ZYDIS_COMPACT_MASK_BITS + bit];
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisCompactDagGetMemory(ZydisCompactDag* dag, ZyanU32 instruction,
    ZyanU64 address, ZyanU32* out_node)
{
    if ((dag == ZYAN_NULL) || (out_node == ZYAN_NULL) ||
        (instruction > dag->instr_count))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if ((address & 3u) != 0u)
    {
        return ZYDIS_STATUS_COMPACT_MISALIGNED;
    }
    const ZydisCompactRegionInternal* region;
    ZYAN_CHECK(ZydisCompactResolveRegion(dag, address, &region, ZYAN_NULL));

    // Only records produced up to `instruction` are visible at that version.
    for (ZyanU32 i = dag->mem_count; i > 0u; --i)
    {
        const ZydisCompactMemCell* const cell = &dag->mem[i - 1u];
        if (cell->address == address && cell->version <= instruction)
        {
            *out_node = cell->node;
            return ZYAN_STATUS_SUCCESS;
        }
    }

    // No visible write: the cell holds its entry identity leaf. Reuse an existing version-0
    // record for the address instead of materializing a second leaf.
    for (ZyanU32 i = 0u; i < dag->mem_count; ++i)
    {
        if (dag->mem[i].address == address && dag->mem[i].version == 0u)
        {
            *out_node = dag->mem[i].node;
            return ZYAN_STATUS_SUCCESS;
        }
    }
    if (dag->mem_count >= ZYDIS_COMPACT_MEMORY_LIMIT)
    {
        return ZYDIS_STATUS_COMPACT_LIMIT;
    }
    ZYAN_CHECK(compact_grow((void**)&dag->mem, &dag->mem_capacity,
        (ZyanUSize)dag->mem_count + 1u, sizeof(*dag->mem)));
    ZyanU32 leaf;
    ZYAN_CHECK(compact_mk_entry_mem(dag, address, &leaf));
    dag->mem[dag->mem_count].address = address;
    dag->mem[dag->mem_count].node = leaf;
    dag->mem[dag->mem_count].version = 0u;
    ++dag->mem_count;
    *out_node = leaf;
    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* Evaluation                                                                                     */
/* ============================================================================================== */

typedef struct ZydisCompactEval_
{
    const ZydisCompactDag* dag;
    const ZyanU16* entry_masks;
    ZyanU32 depth;
} ZydisCompactEval;

static ZyanStatus compact_eval_mask(ZydisCompactEval* ctx, ZyanU32 node_id, ZyanU32* out_bit);

static ZyanStatus compact_eval_rank(ZydisCompactEval* ctx, ZyanU32 node_id, ZyanU32* out_rank)
{
    if (++ctx->depth > ZYDIS_COMPACT_EVAL_DEPTH)
    {
        return ZYAN_STATUS_OUT_OF_RESOURCES;
    }
    const ZydisCompactNode* const node = &ctx->dag->nodes[node_id];
    if (node->type != ZYDIS_COMPACT_NODE_RANK)
    {
        return ZYAN_STATUS_INVALID_OPERATION;
    }
    if (node->width == 0u)
    {
        *out_rank = 0u;
        return ZYAN_STATUS_SUCCESS;
    }
    ZyanU32 prev;
    ZYAN_CHECK(compact_eval_rank(ctx, node->child[0], &prev));
    ZyanU32 bit;
    ZYAN_CHECK(compact_eval_mask(ctx, node->child[1], &bit));
    *out_rank = prev + bit;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus compact_eval_mask(ZydisCompactEval* ctx, ZyanU32 node_id, ZyanU32* out_bit)
{
    if (++ctx->depth > ZYDIS_COMPACT_EVAL_DEPTH)
    {
        return ZYAN_STATUS_OUT_OF_RESOURCES;
    }
    if (node_id >= ctx->dag->node_count)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }
    const ZydisCompactNode* const node = &ctx->dag->nodes[node_id];

    switch (node->type)
    {
    case ZYDIS_COMPACT_NODE_ZERO:
        *out_bit = 0u;
        return ZYAN_STATUS_SUCCESS;
    case ZYDIS_COMPACT_NODE_ONE:
        *out_bit = 1u;
        return ZYAN_STATUS_SUCCESS;
    case ZYDIS_COMPACT_NODE_ENTRY_MASK:
        *out_bit = (ZyanU32)((ctx->entry_masks[node->k] >> node->lane) & 1u);
        return ZYAN_STATUS_SUCCESS;
    case ZYDIS_COMPACT_NODE_MASK_AND:
    case ZYDIS_COMPACT_NODE_MASK_OR:
    case ZYDIS_COMPACT_NODE_MASK_XOR:
    {
        ZyanU32 a;
        ZyanU32 b;
        ZYAN_CHECK(compact_eval_mask(ctx, node->child[0], &a));
        ZYAN_CHECK(compact_eval_mask(ctx, node->child[1], &b));
        if (node->type == ZYDIS_COMPACT_NODE_MASK_AND)
        {
            *out_bit = a & b;
        }
        else if (node->type == ZYDIS_COMPACT_NODE_MASK_OR)
        {
            *out_bit = a | b;
        }
        else
        {
            *out_bit = a ^ b;
        }
        return ZYAN_STATUS_SUCCESS;
    }
    case ZYDIS_COMPACT_NODE_MASK_NOT:
    {
        ZyanU32 a;
        ZYAN_CHECK(compact_eval_mask(ctx, node->child[0], &a));
        *out_bit = a ^ 1u;
        return ZYAN_STATUS_SUCCESS;
    }
    case ZYDIS_COMPACT_NODE_MASK_SHLL:
    case ZYDIS_COMPACT_NODE_MASK_SHRL:
    {
        ZyanU32 a;
        ZYAN_CHECK(compact_eval_mask(ctx, node->child[0], &a));
        if (a == 0u)
        {
            *out_bit = 0u;
            return ZYAN_STATUS_SUCCESS;
        }
        // The child already encodes the bit placed at this output position; a surviving
        // bit contributes a set output, a vacated position contributes zero.
        *out_bit = 1u;
        return ZYAN_STATUS_SUCCESS;
    }
    case ZYDIS_COMPACT_NODE_RANK:
        return compact_eval_rank(ctx, node_id, out_bit);
    case ZYDIS_COMPACT_NODE_EQ:
    {
        ZyanU32 rank;
        ZYAN_CHECK(compact_eval_rank(ctx, node->child[0], &rank));
        *out_bit = (rank == (ZyanU32)node->lane) ? 1u : 0u;
        return ZYAN_STATUS_SUCCESS;
    }
    default:
        return ZYAN_STATUS_INVALID_OPERATION;
    }
}

static ZyanStatus compact_eval_data(ZydisCompactEval* ctx, ZyanU32 node_id,
    ZydisCompactValue* out_value)
{
    if (++ctx->depth > ZYDIS_COMPACT_EVAL_DEPTH)
    {
        return ZYAN_STATUS_OUT_OF_RESOURCES;
    }
    if (node_id >= ctx->dag->node_count)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }
    const ZydisCompactNode* const node = &ctx->dag->nodes[node_id];

    switch (node->type)
    {
    case ZYDIS_COMPACT_NODE_ZERO:
        out_value->kind = ZYDIS_COMPACT_VALUE_ZERO;
        out_value->zmm = 0u;
        out_value->lane = 0u;
        out_value->address = 0u;
        return ZYAN_STATUS_SUCCESS;
    case ZYDIS_COMPACT_NODE_ENTRY_LANE:
        out_value->kind = ZYDIS_COMPACT_VALUE_ENTRY_LANE;
        out_value->zmm = node->zmm;
        out_value->lane = node->lane;
        out_value->address = 0u;
        return ZYAN_STATUS_SUCCESS;
    case ZYDIS_COMPACT_NODE_ENTRY_MEM:
        out_value->kind = ZYDIS_COMPACT_VALUE_ENTRY_MEM;
        out_value->zmm = 0u;
        out_value->lane = 0u;
        out_value->address = node->address;
        return ZYAN_STATUS_SUCCESS;
    case ZYDIS_COMPACT_NODE_SELECT:
    {
        ZyanU32 cond;
        ZYAN_CHECK(compact_eval_mask(ctx, node->child[0], &cond));
        return compact_eval_data(ctx, cond ? node->child[1] : node->child[2], out_value);
    }
    default:
        return ZYAN_STATUS_INVALID_OPERATION;
    }
}

ZyanStatus ZydisCompactDagEvaluate(const ZydisCompactDag* dag, ZyanU32 node_id,
    const ZyanU16 entry_masks[8], ZydisCompactValue* out_value)
{
    if ((dag == ZYAN_NULL) || (entry_masks == ZYAN_NULL) || (out_value == ZYAN_NULL))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (node_id >= dag->node_count)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }
    ZydisCompactEval ctx = { dag, entry_masks, 0u };
    return compact_eval_data(&ctx, node_id, out_value);
}

ZyanStatus ZydisCompactDagEvaluateMask(const ZydisCompactDag* dag, ZyanU32 node_id,
    const ZyanU16 entry_masks[8], ZyanU8* out_bit)
{
    if ((dag == ZYAN_NULL) || (entry_masks == ZYAN_NULL) || (out_bit == ZYAN_NULL))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (node_id >= dag->node_count)
    {
        return ZYAN_STATUS_OUT_OF_RANGE;
    }
    ZydisCompactEval ctx = { dag, entry_masks, 0u };
    ZyanU32 bit;
    const ZyanStatus status = compact_eval_mask(&ctx, node_id, &bit);
    if (ZYAN_SUCCESS(status))
    {
        *out_bit = (ZyanU8)bit;
    }
    return status;
}

/* ============================================================================================== */
