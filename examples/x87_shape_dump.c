//
// x87 stack shape analysis example.
//
// Analyzes a small hand-written function and prints every reachable instruction with the x87 stack
// shapes observed on entry, the physical slots it touches, and any diagnostics.
//
// Build (from the repository root):
//     cc -std=c99 -DBDDISASM_HAS_MEMSET -DBDDISASM_HAS_VSNPRINTF -Iinc -Ibddisasm/include \
//        examples/x87_shape_dump.c bddisasm/*.c -o x87_shape_dump
//
#include <stdio.h>

#include "bddisasm.h"
#include "bdx86_x87.h"


static const char *
diag_kind_name(
    BDX87_DIAG_KIND Kind
    )
{
    switch (Kind)
    {
    case BDX87_DIAG_UNDERFLOW:      return "underflow";
    case BDX87_DIAG_OVERFLOW:       return "overflow";
    case BDX87_DIAG_UNSUPPORTED:    return "unsupported";
    case BDX87_DIAG_BAD_TARGET:     return "bad-target";
    case BDX87_DIAG_BAD_ENCODING:   return "bad-encoding";
    default:                        return "?";
    }
}


int
main(
    void
    )
{
    static const unsigned char code[] = {
        0xD9, 0xE8,             // FLD1
        0xD9, 0xC0,             // FLD st0
        0xD9, 0xC9,             // FXCH st1
        0xDE, 0xC1,             // FADDP st1, st0
        0x74, 0x02,             // JZ +2
        0xDD, 0xD8,             // FSTP st0
        0xD9, 0xF7,             // FINCSTP
        0xC3                    // RET
    };
    const unsigned long long base = 0x1000;
    BDX87_REPORT *report;
    NDSTATUS status;
    unsigned int i;
    unsigned int s;

    // Start with TOP = 0 and an entirely empty stack.
    status = BdX87AnalyzeCode(code, sizeof(code), base, 0, 0, 0x00, &report);
    if (!ND_SUCCESS(status))
    {
        fprintf(stderr, "BdX87AnalyzeCode failed: 0x%x\n", status);
        return 1;
    }

    for (i = 0; i < report->InsnCount; i++)
    {
        const BDX87_INSN *insn = &report->Insns[i];
        INSTRUX instrux;
        char text[128];

        if (ND_SUCCESS(NdDecodeEx(&instrux, code + insn->Offset, sizeof(code) - insn->Offset,
                                  ND_CODE_64, ND_DATA_64)) &&
            ND_SUCCESS(NdToText(&instrux, insn->Address, sizeof(text), text)))
        {
            printf("%llx: %-16s\n", insn->Address, text);
        }
        else
        {
            printf("%llx: (len %u)\n", insn->Address, insn->Length);
        }

        for (s = 0; s < insn->StateCount; s++)
        {
            const BDX87_STATE *st = &insn->States[s];

            printf("    in  TOP=%u tags=%02x -> out TOP=%u tags=%02x"
                   " read=%02x written=%02x cleared=%02x%s\n",
                   st->In.Top, st->In.TagMask,
                   st->Out.Top, st->Out.TagMask,
                   st->ReadSlots, st->WrittenSlots, st->ClearedSlots,
                   st->Completed ? "" : " (stopped)");
        }
    }

    for (i = 0; i < report->DiagCount; i++)
    {
        const BDX87_DIAG *diag = &report->Diags[i];

        printf("diag %s at %llx", diag_kind_name(diag->Kind), diag->Address);
        if (diag->Slot != BDX87_NO_SLOT)
        {
            printf(" slot %u", diag->Slot);
        }
        printf(" (shape TOP=%u tags=%02x)\n", diag->Shape.Top, diag->Shape.TagMask);
    }

    BdX87FreeReport(report);

    return 0;
}
