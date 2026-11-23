/***************************************************************************************************

  Example: symbolic provenance for an AVX-512 data-filtering fragment.

  This program never executes the decoded machine code. It builds one provenance graph for a
  short straight-line fragment, then evaluates the same graph twice with two different entry
  mask assignments and prints where each output lane / memory cell comes from.

  Build (CMake enables it automatically when examples are on):
      cc -std=c11 compact_example.c -lZydis -lZycore -o compact_example

***************************************************************************************************/

#include <stdio.h>
#include <string.h>

#include <Zydis/Zydis.h>
#include <Zydis/Compact.h>

/*
 * The fragment (Intel syntax), assembled once and embedded so the example stays standalone:
 *
 *   vmovdqu32  zmm1{k3}{z}, zmm2          ; filtered copy into zmm1
 *   vpcompressd zmm7{k1}, zmm8            ; compact zmm8 into zmm7
 *   vpexpandd  zmm10{k4}, zmm11           ; expand zmm11 into zmm10
 *   vpcompressd [0x401000]{k1}, zmm2      ; compact zmm2 into the memory region
 */
static const unsigned char program[] = {
    0x62, 0xf1, 0x7e, 0xcb, 0x6f, 0xca,             /* vmovdqu32 zmm1{k3}{z}, zmm2  */
    0x62, 0x72, 0x7d, 0x49, 0x8b, 0xc7,             /* vpcompressd zmm7{k1}, zmm8   */
    0x62, 0x52, 0x7d, 0x4c, 0x89, 0xd3,             /* vpexpandd zmm10{k4}, zmm11   */
    0x62, 0xf2, 0x7d, 0x49, 0x8b, 0x14, 0x25,
    0x00, 0x10, 0x40, 0x00                          /* vpcompressd [0x401000]{k1}.. */
};

static const ZyanUSize instruction_length[] = { 6, 6, 6, 11 };
#define PROGRAM_INSTRUCTION_COUNT 4u
#define REGION_BASE 0x401000u

static void print_value(const char* label, const ZydisCompactValue* value)
{
    switch (value->kind)
    {
    case ZYDIS_COMPACT_VALUE_ZERO:
        printf("  %-22s zero\n", label);
        break;
    case ZYDIS_COMPACT_VALUE_ENTRY_LANE:
        printf("  %-22s entry zmm%u lane %u\n", label, value->zmm, value->lane);
        break;
    case ZYDIS_COMPACT_VALUE_ENTRY_MEM:
        printf("  %-22s entry memory 0x%llx\n", label,
            (unsigned long long)value->address);
        break;
    default:
        printf("  %-22s <unknown>\n", label);
        break;
    }
}

int main(void)
{
    ZydisCompactDag* dag = ZYAN_NULL;
    if (!ZYAN_SUCCESS(ZydisCompactDagCreate(&dag)))
    {
        fprintf(stderr, "failed to create the provenance graph\n");
        return 1;
    }

    /* The memory region backing the disp32 store: 16 dword cells, 4-byte aligned. */
    ZyanStatus status = ZydisCompactDagAddRegion(dag, REGION_BASE, 16);
    if (!ZYAN_SUCCESS(status))
    {
        fprintf(stderr, "failed to register the memory region (status 0x%08x)\n", status);
        ZydisCompactDagDestroy(dag);
        return 1;
    }

    /* Decode the whole fragment once. */
    ZyanUSize offset = 0;
    for (ZyanU32 i = 0; i < PROGRAM_INSTRUCTION_COUNT; ++i)
    {
        status = ZydisCompactDagDecode(dag, program + offset,
            instruction_length[i], 0x10000u + offset);
        if (!ZYAN_SUCCESS(status))
        {
            ZyanU32 bad_index;
            ZyanU64 bad_address;
            ZyanUSize bad_length;
            if (ZYAN_SUCCESS(ZydisCompactDagLastUnsupported(dag, &bad_index, &bad_address,
                &bad_length)))
            {
                fprintf(stderr,
                    "unsupported instruction %u at 0x%llx (%zu bytes)\n",
                    bad_index, (unsigned long long)bad_address, bad_length);
            }
            ZydisCompactDagDestroy(dag);
            return 1;
        }
        offset += instruction_length[i];
    }

    printf("built graph with %u nodes and %u instructions\n",
        ZydisCompactDagNodeCount(dag), ZydisCompactDagInstructionCount(dag));

    /* Two concrete entry-mask assignments evaluated against the very same graph. */
    const ZyanU16 assignments[][8] = {
        { 0x0000, 0xAAAA, 0x0F0F, 0x5555, 0x3333, 0, 0, 0 },
        { 0x0000, 0xFFFF, 0x00FF, 0xCCCC, 0x96CF, 0, 0, 0 }
    };

    for (int a = 0; a < 2; ++a)
    {
        printf("\nassignment %d: k1=0x%04x k3=0x%04x k4=0x%04x\n",
            a + 1, assignments[a][1], assignments[a][3], assignments[a][4]);

        /* zmm1 after the masked move (version 1). */
        for (ZyanU8 lane = 0; lane < 4; ++lane)
        {
            ZyanU32 node;
            ZydisCompactValue value;
            char label[32];
            ZydisCompactDagGetLane(dag, 1u, 1u, lane, &node);
            ZydisCompactDagEvaluate(dag, node, assignments[a], &value);
            snprintf(label, sizeof(label), "zmm1[%u]", lane);
            print_value(label, &value);
        }

        /* The first four compacted memory cells (version 4). */
        for (ZyanU8 cell = 0; cell < 4; ++cell)
        {
            ZyanU32 node;
            ZydisCompactValue value;
            char label[32];
            ZydisCompactDagGetMemory(dag, 4u, REGION_BASE + 4u * cell, &node);
            ZydisCompactDagEvaluate(dag, node, assignments[a], &value);
            snprintf(label, sizeof(label), "[0x%llx]",
                (unsigned long long)(REGION_BASE + 4u * cell));
            print_value(label, &value);
        }
    }

    /* All owned storage is released here; the input byte array was never retained. */
    ZydisCompactDagDestroy(dag);
    return 0;
}
