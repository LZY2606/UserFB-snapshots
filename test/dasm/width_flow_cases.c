/*
 * width_flow_cases.c - task-defined tests for automatic 65816 width flow.
 * This file is intentionally compiled without main(); task_support/driver.c
 * supplies the runner.
 */

#include "task_support/check.h"
#include "widthflow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



/*****************************************************************************/
/*                                   Helpers                                 */
/*****************************************************************************/



#define STATE_BIT(E, M, X, C) \
    ((WfStateMask) (1u << OPCFLOW_STATE_INDEX ((E), (M), (X), (C))))

static int HasState (const WfResult* R, size_t Offset, int E, int M, int X, int C)
{
    return R != 0 &&
           (R->Sites[Offset].StateMask & STATE_BIT (E, M, X, C)) != 0;
}



static const WfDiagnostic* FindDiag (const WfResult* R,
                                     WfDiagnosticKind Kind,
                                     uint32_t Address)
{
    size_t I;
    for (I = 0; I < R->DiagnosticCount; ++I) {
        if (R->Diagnostics[I].Kind == Kind &&
            R->Diagnostics[I].Address == Address) {
            return &R->Diagnostics[I];
        }
    }
    return 0;
}



static int HasLength (const WfResult* R, size_t Offset, unsigned Length)
{
    return (R->Sites[Offset].LengthMask & (uint8_t) (1u << Length)) != 0;
}



static char* ReadTextFile (const char* Path)
{
    FILE* F = fopen (Path, "rb");
    long  Size;
    char* Text;

    if (F == 0) {
        return 0;
    }
    if (fseek (F, 0, SEEK_END) != 0 || (Size = ftell (F)) < 0) {
        fclose (F);
        return 0;
    }
    rewind (F);
    Text = malloc ((size_t) Size + 1);
    if (Text == 0) {
        fclose (F);
        return 0;
    }
    if (fread (Text, 1, (size_t) Size, F) != (size_t) Size) {
        free (Text);
        fclose (F);
        return 0;
    }
    Text[Size] = '\0';
    fclose (F);
    return Text;
}



static int WriteBinary (const char* Path, const uint8_t* Bytes, size_t Size)
{
    FILE* F = fopen (Path, "wb");
    int   Ok;

    if (F == 0) {
        return 0;
    }
    Ok = fwrite (Bytes, 1, Size, F) == Size;
    return fclose (F) == 0 && Ok;
}



static int ContainsAll (const char* Text, const char* const* Needles,
                        size_t Count)
{
    size_t I;
    if (Text == 0) {
        return 0;
    }
    for (I = 0; I < Count; ++I) {
        if (strstr (Text, Needles[I]) == 0) {
            return 0;
        }
    }
    return 1;
}



/* Run da65 --auto-65816, then reassemble with the repository ca65/ld65. */
static int CliRoundtrip (const char* Name, const uint8_t* Bytes, size_t Size,
                        const char* State, const char* const* Needles,
                        size_t NeedleCount)
{
    char Bin[300], Asm[300], Obj[300], Out[300], Cmd[1400];
    char* Text;
    int   Ok;

    if (system ("mkdir -p .task-build/wf") != 0) {
        return 0;
    }

    snprintf (Bin, sizeof (Bin), ".task-build/wf/%s.bin", Name);
    snprintf (Asm, sizeof (Asm), ".task-build/wf/%s.s", Name);
    snprintf (Obj, sizeof (Obj), ".task-build/wf/%s.o", Name);
    snprintf (Out, sizeof (Out), ".task-build/wf/%s.out.bin", Name);
    remove (Asm);
    remove (Obj);
    remove (Out);

    if (!WriteBinary (Bin, Bytes, Size)) {
        return 0;
    }

    snprintf (Cmd, sizeof (Cmd),
              "bin/da65 --cpu 65816 --auto-65816 %s --start-addr 0x8000 "
              "-o %s %s", State, Asm, Bin);
    if (system (Cmd) != 0) {
        return 0;
    }

    Text = ReadTextFile (Asm);
    Ok = ContainsAll (Text, Needles, NeedleCount);
    free (Text);
    if (!Ok) {
        return 0;
    }

    snprintf (Cmd, sizeof (Cmd),
              "bin/ca65 --cpu 65816 -o %s %s && "
              "bin/ld65 -o %s -C test/dasm/65816.cfg %s && "
              "cmp -s %s %s",
              Obj, Asm, Out, Obj, Bin, Out);
    return system (Cmd) == 0;
}



static int CliExpectFailureNoOutput (const char* Name,
                                     const uint8_t* Bytes, size_t Size,
                                     const char* State)
{
    char Bin[300], Asm[300], Cmd[1200];

    if (system ("mkdir -p .task-build/wf") != 0) {
        return 0;
    }
    snprintf (Bin, sizeof (Bin), ".task-build/wf/%s.bin", Name);
    snprintf (Asm, sizeof (Asm), ".task-build/wf/%s-failed.s", Name);
    remove (Bin);

    if (!WriteBinary (Bin, Bytes, Size)) {
        return 0;
    }
    /* A stale output from a previous successful run must not survive. */
    if (!WriteBinary (Asm, (const uint8_t*) "stale assembly\n", 15)) {
        return 0;
    }

    snprintf (Cmd, sizeof (Cmd),
              "bin/da65 --cpu 65816 --auto-65816 %s --start-addr 0x8000 "
              "-o %s %s >/dev/null 2>&1", State, Asm, Bin);
    if (system (Cmd) == 0) {
        return 0;
    }

    /* Analysis must happen before the output file is created/truncated. */
    {
        FILE* F = fopen (Asm, "r");
        if (F != 0) {
            fclose (F);
            return 0;
        }
    }
    return 1;
}



/*****************************************************************************/
/*                                  Modes                                    */
/*****************************************************************************/



static int modes_parse_state (void)
{
    WfState S;

    TASK_CHECK (WfParseState ("0,0,1,0", &S));
    TASK_CHECK (S.E == 0 && S.M == 0 && S.X == 1 && S.C == 0);
    TASK_CHECK (WfParseState ("1,1,1,1", &S));
    TASK_CHECK (S.E == 1 && S.M == 1 && S.X == 1 && S.C == 1);

    TASK_CHECK (!WfParseState ("1,0,1,0", &S));
    TASK_CHECK (!WfParseState ("0,0,1", &S));
    TASK_CHECK (!WfParseState ("0,0,1,2", &S));
    TASK_CHECK (!WfParseState ("0,0,1,0,", &S));
    TASK_CHECK (!WfParseState (" 0,0,1,0", &S));

    S = (WfState) { 1, 0, 1, 0 };
    TASK_CHECK (WfAnalyze ((const uint8_t*) "", 0, 0x8000, &S) == 0);
    return 0;
}



static int modes_rep (void)
{
    static const uint8_t Code[] = { 0xC2, 0x30, 0xA9, 0x34, 0x12 };
    WfState Initial = { 0, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0);
    TASK_CHECK (R->Clean);
    TASK_CHECK (R->Sites[0].Address == 0x8000);
    TASK_CHECK (HasState (R, 0, 0, 1, 1, 0));
    TASK_CHECK (R->Sites[2].Address == 0x8002);
    TASK_CHECK (HasState (R, 2, 0, 0, 0, 0));
    TASK_CHECK (HasLength (R, 2, 3));
    WfResultFree (R);
    return 0;
}



static int modes_sep (void)
{
    static const uint8_t Code[] = {
        0xC2, 0x30,             /* REP #$30 */
        0xE2, 0x30,             /* SEP #$30 */
        0xA9, 0x56,             /* LDA #$56 */
        0xA2, 0x78,             /* LDX #$78 */
        0x60
    };
    WfState Initial = { 0, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0);
    TASK_CHECK (R->Clean);
    TASK_CHECK (R->Sites[4].Address == 0x8004);
    TASK_CHECK (HasState (R, 4, 0, 1, 1, 0));
    TASK_CHECK (HasLength (R, 4, 2));
    TASK_CHECK (R->Sites[6].Address == 0x8006);
    TASK_CHECK (HasLength (R, 6, 2));
    WfResultFree (R);
    return 0;
}



static int modes_clc (void)
{
    static const uint8_t Code[] = { 0x18, 0xEA };
    WfState Initial = { 0, 1, 1, 1 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (R->Sites[1].Address == 0x8001);
    TASK_CHECK (HasState (R, 1, 0, 1, 1, 0));
    WfResultFree (R);
    return 0;
}



static int modes_sec (void)
{
    static const uint8_t Code[] = { 0x38, 0xEA };
    WfState Initial = { 0, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (R->Sites[1].Address == 0x8001);
    TASK_CHECK (HasState (R, 1, 0, 1, 1, 1));
    WfResultFree (R);
    return 0;
}



static int modes_xce_enter_emulation (void)
{
    static const uint8_t Code[] = { 0xFB, 0xEA };
    WfState Initial = { 0, 0, 0, 1 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (R->Sites[1].Address == 0x8001);
    TASK_CHECK (HasState (R, 1, 1, 1, 1, 0));
    WfResultFree (R);
    return 0;
}



static int modes_xce_leave_emulation (void)
{
    static const uint8_t Code[] = { 0xFB, 0xEA };
    WfState Initial = { 1, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (R->Sites[1].Address == 0x8001);
    TASK_CHECK (HasState (R, 1, 0, 1, 1, 1));
    WfResultFree (R);
    return 0;
}



static int modes_emulation_rep_cannot_clear (void)
{
    static const uint8_t Code[] = {
        0xC2, 0x30, 0xA9, 0x34, 0x12, 0x00, 0x60
    };
    WfState Initial = { 1, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (HasState (R, 2, 1, 1, 1, 0));
    TASK_CHECK (HasLength (R, 2, 2));
    TASK_CHECK (!HasLength (R, 2, 3));
    WfResultFree (R);
    return 0;
}



static int modes_plp_legal_combinations (void)
{
    static const uint8_t Code[] = { 0x28, 0xEA };
    WfState Native = { 0, 0, 0, 0 };
    WfState Emulation = { 1, 1, 1, 0 };
    WfResult* A = WfAnalyze (Code, sizeof Code, 0x8000, &Native);
    WfResult* B = WfAnalyze (Code, sizeof Code, 0x8000, &Emulation);

    TASK_CHECK (A != 0 && A->Clean);
    TASK_CHECK (A->Sites[1].StateMask == 0x00FFu);
    TASK_CHECK (B != 0 && B->Clean);
    TASK_CHECK (B->Sites[1].StateMask == (STATE_BIT (1, 1, 1, 0) |
                                          STATE_BIT (1, 1, 1, 1)));
    WfResultFree (A);
    WfResultFree (B);
    return 0;
}



static int modes_carry_generating_opcodes (void)
{
    unsigned Op;
    unsigned Count = 0;
    const unsigned Idx = OPCFLOW_STATE_INDEX (0, 1, 1, 0);

    for (Op = 0; Op < 256; ++Op) {
        Opc65816Flow Info;
        OpcFlowStateMask Mask;
        Opc65816Describe ((uint8_t) Op, &Info);
        if (!Info.SetsCarry) {
            continue;
        }
        ++Count;
        Mask = Opc65816NextStates ((uint8_t) Op, 0, Idx);
        TASK_CHECK (Mask == (STATE_BIT (0, 1, 1, 0) |
                             STATE_BIT (0, 1, 1, 1)));
    }
    TASK_CHECK (Count >= 20);
    return 0;
}



/*****************************************************************************/
/*                                   Flow                                    */
/*****************************************************************************/



static int flow_branch_merge (void)
{
    static const uint8_t Code[] = { 0xD0, 0x01, 0xEA, 0xEA };
    WfState Initial = { 1, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (HasState (R, 2, 1, 1, 1, 0));
    TASK_CHECK (HasState (R, 3, 1, 1, 1, 0));
    TASK_CHECK (R->Sites[3].StateMask == STATE_BIT (1, 1, 1, 0));
    WfResultFree (R);
    return 0;
}



static int flow_bcc_known_c (void)
{
    static const uint8_t Code[] = {
        0x90, 0x01, 0x80, 0x00, 0xEA, 0x60
    };
    WfState CSet = { 1, 1, 1, 1 };
    WfState CClear = { 1, 1, 1, 0 };
    WfResult* A = WfAnalyze (Code, sizeof Code, 0x8000, &CSet);
    WfResult* B = WfAnalyze (Code, sizeof Code, 0x8000, &CClear);

    TASK_CHECK (A != 0 && A->Clean);
    TASK_CHECK (A->Sites[2].StateMask != 0);
    TASK_CHECK (A->Sites[3].StateMask == 0);
    TASK_CHECK (A->Sites[4].StateMask != 0);

    TASK_CHECK (B != 0 && B->Clean);
    TASK_CHECK (B->Sites[2].StateMask == 0);
    TASK_CHECK (B->Sites[3].StateMask != 0);
    WfResultFree (A);
    WfResultFree (B);
    return 0;
}



static int flow_bcs_known_c (void)
{
    static const uint8_t Code[] = {
        0xB0, 0x01, 0x80, 0x00, 0xEA, 0x60
    };
    WfState CSet = { 1, 1, 1, 1 };
    WfState CClear = { 1, 1, 1, 0 };
    WfResult* A = WfAnalyze (Code, sizeof Code, 0x8000, &CSet);
    WfResult* B = WfAnalyze (Code, sizeof Code, 0x8000, &CClear);

    TASK_CHECK (A != 0 && A->Clean);
    TASK_CHECK (A->Sites[2].StateMask == 0);
    TASK_CHECK (A->Sites[3].StateMask != 0);

    TASK_CHECK (B != 0 && B->Clean);
    TASK_CHECK (B->Sites[2].StateMask != 0);
    TASK_CHECK (B->Sites[3].StateMask == 0);
    WfResultFree (A);
    WfResultFree (B);
    return 0;
}



static int flow_c_rewrite_dual_paths (void)
{
    static const uint8_t Code[] = {
        0xC9, 0x00,             /* CMP #$00: C becomes two possibilities */
        0x90, 0x01,             /* BCC +1 */
        0xEA,                   /* fall-through path */
        0xEA                    /* merge */
    };
    WfState Initial = { 0, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (R->Sites[2].StateMask ==
                (STATE_BIT (0, 1, 1, 0) | STATE_BIT (0, 1, 1, 1)));
    TASK_CHECK (R->Sites[4].StateMask == STATE_BIT (0, 1, 1, 1));
    TASK_CHECK (R->Sites[5].StateMask ==
                (STATE_BIT (0, 1, 1, 0) | STATE_BIT (0, 1, 1, 1)));
    WfResultFree (R);
    return 0;
}



static int flow_plp_width_conflict (void)
{
    static const uint8_t Code[] = { 0x28, 0xA9, 0x01, 0x02 };
    WfState Initial = { 0, 0, 0, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);
    const WfDiagnostic* D;

    TASK_CHECK (R != 0 && !R->Clean);
    D = FindDiag (R, WF_DIAG_WIDTH_CONFLICT, 0x8001);
    TASK_CHECK (D != 0);
    TASK_CHECK ((D->LengthMask & (1u << 2)) != 0);
    TASK_CHECK ((D->LengthMask & (1u << 3)) != 0);
    TASK_CHECK (FindDiag (R, WF_DIAG_OVERLAP, 0x8003) != 0);
    WfResultFree (R);
    return 0;
}



static int flow_same_start_length_conflict (void)
{
    static const uint8_t Code[] = {
        0xD0, 0x02,             /* BNE to $8004, keeping M=1 */
        0xC2, 0x20,             /* REP #$20 on fall-through, making M=0 */
        0xA9, 0x34, 0xEA        /* common LDA start needs 2 or 3 bytes */
    };
    WfState Initial = { 0, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);
    const WfDiagnostic* D;

    TASK_CHECK (R != 0 && !R->Clean);
    TASK_CHECK (HasState (R, 4, 0, 1, 1, 0));
    TASK_CHECK (HasState (R, 4, 0, 0, 1, 0));
    D = FindDiag (R, WF_DIAG_WIDTH_CONFLICT, 0x8004);
    TASK_CHECK (D != 0);
    TASK_CHECK (HasLength (R, 4, 2) && HasLength (R, 4, 3));
    WfResultFree (R);
    return 0;
}



static int flow_overlap (void)
{
    static const uint8_t Code[] = { 0xD0, 0x01, 0xA9, 0xEA };
    WfState Initial = { 1, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);
    const WfDiagnostic* D = FindDiag (R, WF_DIAG_OVERLAP, 0x8003);

    TASK_CHECK (R != 0 && !R->Clean);
    TASK_CHECK (D != 0);
    TASK_CHECK (D->OtherAddress == 0x8002);
    WfResultFree (R);
    return 0;
}



static int flow_loop (void)
{
    static const uint8_t Code[] = {
        0xEA,
        0xD0, 0xFD,             /* BNE back to $8000 */
        0x60
    };
    WfState Initial = { 1, 1, 1, 0 };
    WfResult* R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);

    TASK_CHECK (R != 0 && R->Clean);
    TASK_CHECK (R->Sites[0].StateMask == STATE_BIT (1, 1, 1, 0));
    TASK_CHECK (R->Sites[1].StateMask != 0);
    TASK_CHECK (R->Sites[3].StateMask != 0);
    WfResultFree (R);
    return 0;
}



static int flow_outside_target (void)
{
    static const uint8_t Bra[] = { 0x80, 0x7F };
    static const uint8_t Jmp[] = { 0x4C, 0x34, 0x12 };
    WfState Initial = { 1, 1, 1, 0 };
    WfResult* A = WfAnalyze (Bra, sizeof Bra, 0x8000, &Initial);
    WfResult* B = WfAnalyze (Jmp, sizeof Jmp, 0x8000, &Initial);

    TASK_CHECK (A != 0 && A->Clean);
    TASK_CHECK (A->Sites[0].StateMask != 0 && A->Sites[1].StateMask == 0);
    TASK_CHECK (B != 0 && B->Clean);
    TASK_CHECK (B->Sites[0].StateMask != 0 && B->Sites[1].StateMask == 0);
    WfResultFree (A);
    WfResultFree (B);
    return 0;
}



static int flow_truncation (void)
{
    static const uint8_t Rep[] = { 0xC2 };
    static const uint8_t WideLda[] = { 0xA9, 0x01 };
    WfState Native16 = { 0, 0, 1, 0 };
    WfState Emul = { 1, 1, 1, 0 };
    WfResult* A = WfAnalyze (Rep, sizeof Rep, 0x8000, &Emul);
    WfResult* B = WfAnalyze (WideLda, sizeof WideLda, 0x8000, &Native16);

    TASK_CHECK (A != 0 && !A->Clean);
    TASK_CHECK (FindDiag (A, WF_DIAG_TRUNCATED, 0x8000) != 0);
    TASK_CHECK (B != 0 && !B->Clean);
    TASK_CHECK (FindDiag (B, WF_DIAG_TRUNCATED, 0x8000) != 0);
    WfResultFree (A);
    WfResultFree (B);
    return 0;
}



static int flow_barriers (void)
{
    static const uint8_t Opcodes[] = {
        0x00, 0x02, 0x20, 0x22, 0x40, 0x60, 0x6B,
        0x6C, 0x7C, 0xCB, 0xDB, 0xDC, 0xFC
    };
    WfState Initial = { 1, 1, 1, 0 };
    unsigned I;

    for (I = 0; I < sizeof Opcodes; ++I) {
        uint8_t Code[8] = { 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA };
        WfResult* R;
        unsigned Length;

        Code[0] = Opcodes[I];
        if (Opcodes[I] == 0x20) {
            Code[1] = 0x34; Code[2] = 0x12;     /* $1234: outside */
        } else if (Opcodes[I] == 0x22) {
            Code[1] = 0x78; Code[2] = 0x56; Code[3] = 0x34;
        }
        Length = Opc65816Length (Opcodes[I], &Initial);
        R = WfAnalyze (Code, sizeof Code, 0x8000, &Initial);
        TASK_CHECK (R != 0);
        TASK_CHECK (R->Sites[Length].StateMask == 0);
        WfResultFree (R);
    }

    {
        /* A direct JSR target is still a barrier in this state flow. */
        static const uint8_t Call[] = {
            0x20, 0x04, 0x80, 0xEA, 0x60, 0xEA
        };
        WfResult* R = WfAnalyze (Call, sizeof Call, 0x8000, &Initial);
        TASK_CHECK (R != 0);
        TASK_CHECK (R->Sites[3].StateMask == 0);
        TASK_CHECK (R->Sites[4].StateMask == 0);
        WfResultFree (R);
    }
    return 0;
}



/*****************************************************************************/
/*                                  Listing                                  */
/*****************************************************************************/



static int listing_basic_roundtrip (void)
{
    static const uint8_t Code[] = {
        0xC2, 0x20, 0xA9, 0x34, 0x12,
        0xE2, 0x20, 0xA9, 0x56, 0x60
    };
    static const char* Needles[] = {
        ".a16", ".i8", "rep", "#$20", "lda", "#$1234",
        ".a8", "#$56", "rts"
    };
    TASK_CHECK (CliRoundtrip ("basic", Code, sizeof Code, "0,0,1,0",
                              Needles, sizeof Needles / sizeof Needles[0]));
    return 0;
}



static int listing_branch_roundtrip (void)
{
    static const uint8_t Code[] = { 0xD0, 0x01, 0xEA, 0xEA };
    static const char* Needles[] = { "bne", "L8003", "nop" };
    TASK_CHECK (CliRoundtrip ("branch", Code, sizeof Code, "1,1,1,0",
                              Needles, sizeof Needles / sizeof Needles[0]));
    return 0;
}



static int listing_loop_roundtrip (void)
{
    static const uint8_t Code[] = { 0xEA, 0xD0, 0xFD, 0x60 };
    static const char* Needles[] = { "L8000", "bne", "rts" };
    TASK_CHECK (CliRoundtrip ("loop", Code, sizeof Code, "1,1,1,0",
                              Needles, sizeof Needles / sizeof Needles[0]));
    return 0;
}



static int listing_data_tail_roundtrip (void)
{
    static const uint8_t Code[] = {
        0x60, 0xA9, 0x34, 0x12, 0xC2, 0x20, 0xEA
    };
    static const char* Needles[] = { "rts", ".byte", "$A9", "$34" };
    TASK_CHECK (CliRoundtrip ("data-tail", Code, sizeof Code, "1,1,1,0",
                              Needles, sizeof Needles / sizeof Needles[0]));
    return 0;
}



static int listing_jsr_barrier_roundtrip (void)
{
    uint8_t Code[17];
    static const char* Needles[] = { "jsr", "L8010", ".byte", "$60" };

    memset (Code, 0xEA, sizeof Code);
    Code[0] = 0x20;
    Code[1] = 0x10;
    Code[2] = 0x80;
    Code[16] = 0x60;

    TASK_CHECK (CliRoundtrip ("jsr-barrier", Code, sizeof Code, "1,1,1,0",
                              Needles, sizeof Needles / sizeof Needles[0]));
    return 0;
}



static int listing_index_width_roundtrip (void)
{
    static const uint8_t Code[] = {
        0xC2, 0x10,             /* REP #$10: X is 16 bit */
        0xA2, 0x34, 0x12,       /* LDX #$1234 */
        0xE2, 0x10,             /* SEP #$10: X is 8 bit */
        0xA2, 0x56,             /* LDX #$56 */
        0x60
    };
    static const char* Needles[] = {
        ".i8", ".i16", "ldx", "#$1234", "#$56", "rts"
    };
    TASK_CHECK (CliRoundtrip ("index-width", Code, sizeof Code, "0,1,1,0",
                              Needles, sizeof Needles / sizeof Needles[0]));
    return 0;
}



static int listing_outside_branch_roundtrip (void)
{
    static const uint8_t Code[] = { 0x80, 0x7F };
    static const char* Needles[] = { "bra", "*+129" };
    TASK_CHECK (CliRoundtrip ("outside-branch", Code, sizeof Code,
                              "1,1,1,0", Needles,
                              sizeof Needles / sizeof Needles[0]));
    return 0;
}



static int listing_ambiguous_and_truncated (void)
{
    static const uint8_t PlpConflict[] = { 0x28, 0xA9, 0x01, 0x02 };
    static const uint8_t Truncated[] = { 0xA9, 0x01 };

    TASK_CHECK (CliExpectFailureNoOutput ("plp-conflict", PlpConflict,
                                          sizeof PlpConflict, "0,0,0,0"));
    TASK_CHECK (CliExpectFailureNoOutput ("truncated", Truncated,
                                          sizeof Truncated, "0,0,1,0"));
    return 0;
}



static int listing_option_validation (void)
{
    int BadEmulation, BadCpu, InfoConflict;

    system ("mkdir -p .task-build/wf");
    remove (".task-build/wf/bad-e.s");
    BadEmulation = system (
        "bin/da65 --cpu 65816 --auto-65816 1,0,1,0 "
        "--start-addr 0x8000 -o .task-build/wf/bad-e.s "
        ".task-build/state.bin >/dev/null 2>&1");
    TASK_CHECK (BadEmulation != 0);
    TASK_CHECK (fopen (".task-build/wf/bad-e.s", "r") == 0);

    BadCpu = system (
        "bin/da65 --cpu 6502 --auto-65816 0,0,1,0 "
        "-o .task-build/wf/bad-cpu.s .task-build/state.bin "
        ">/dev/null 2>&1");
    TASK_CHECK (BadCpu != 0);

    InfoConflict = system (
        "bin/da65 --cpu 65816 --auto-65816 0,0,1,0 "
        "-i task_support/state.info --start-addr 0x8000 "
        "-o .task-build/wf/info.s .task-build/state.bin "
        ">/dev/null 2>&1");
    TASK_CHECK (InfoConflict != 0);
    return 0;
}



/*****************************************************************************/
/*                              Case registry                                */
/*****************************************************************************/



const struct task_case task_cases[] = {
    { "modes",   "parse_state",                  modes_parse_state },
    { "modes",   "rep_updates_state",            modes_rep },
    { "modes",   "sep_updates_state",            modes_sep },
    { "modes",   "clc_clears_c",                 modes_clc },
    { "modes",   "sec_sets_c",                   modes_sec },
    { "modes",   "xce_enter_emulation",          modes_xce_enter_emulation },
    { "modes",   "xce_leave_emulation",          modes_xce_leave_emulation },
    { "modes",   "emulation_rep_protects_mx",     modes_emulation_rep_cannot_clear },
    { "modes",   "plp_legal_combinations",       modes_plp_legal_combinations },
    { "modes",   "carry_generating_opcodes",     modes_carry_generating_opcodes },

    { "flow",    "branch_merge",                 flow_branch_merge },
    { "flow",    "bcc_known_c_prunes_edge",      flow_bcc_known_c },
    { "flow",    "bcs_known_c_prunes_edge",      flow_bcs_known_c },
    { "flow",    "c_rewrite_dual_paths",         flow_c_rewrite_dual_paths },
    { "flow",    "plp_width_conflict",           flow_plp_width_conflict },
    { "flow",    "same_start_length_conflict",   flow_same_start_length_conflict },
    { "flow",    "overlap_start_in_operand",     flow_overlap },
    { "flow",    "loop_dedup",                   flow_loop },
    { "flow",    "outside_target_leaves",        flow_outside_target },
    { "flow",    "truncation_diagnostic",        flow_truncation },
    { "flow",    "barriers_have_no_fallthrough", flow_barriers },

    { "listing", "basic_roundtrip",              listing_basic_roundtrip },
    { "listing", "branch_roundtrip",             listing_branch_roundtrip },
    { "listing", "loop_roundtrip",               listing_loop_roundtrip },
    { "listing", "index_width_roundtrip",        listing_index_width_roundtrip },
    { "listing", "data_tail_roundtrip",          listing_data_tail_roundtrip },
    { "listing", "jsr_barrier_roundtrip",        listing_jsr_barrier_roundtrip },
    { "listing", "outside_branch_roundtrip",     listing_outside_branch_roundtrip },
    { "listing", "ambiguous_and_truncated",      listing_ambiguous_and_truncated },
    { "listing", "option_validation",            listing_option_validation },
};

const size_t task_case_count =
    sizeof task_cases / sizeof task_cases[0];
