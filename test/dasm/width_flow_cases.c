/* Regression cases for the da65 --auto-65816 state flow analysis.
**
** Registers cases in the groups "modes", "flow" and "listing" for the
** task driver in task_support/driver.c. There is no main() here.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"
#include "widthflow.h"



#define WF_START 0x8000u
#define WF_DIR   ".task-build"



/*****************************************************************************/
/*                                 Helpers                                   */
/*****************************************************************************/



static void WfEnsureDir (void)
/* Make sure the build directory for the test files exists */
{
    system ("mkdir -p " WF_DIR);
}



static int WfWriteBin (const char* Path, const unsigned char* Data, size_t Len)
/* Write a binary file, return true on success */
{
    FILE* F = fopen (Path, "wb");
    if (F == 0) {
        return 0;
    }
    if (fwrite (Data, 1, Len, F) != Len) {
        fclose (F);
        return 0;
    }
    return fclose (F) == 0;
}



static char* WfReadFile (const char* Path, size_t* Len)
/* Read a whole file into a malloced buffer, 0 on error */
{
    FILE* F = fopen (Path, "rb");
    char* Buf;
    long Size;
    if (F == 0) {
        return 0;
    }
    if (fseek (F, 0, SEEK_END) != 0 || (Size = ftell (F)) < 0 ||
        fseek (F, 0, SEEK_SET) != 0) {
        fclose (F);
        return 0;
    }
    Buf = (char*) malloc ((size_t) Size + 1);
    if (Buf == 0 || fread (Buf, 1, (size_t) Size, F) != (size_t) Size) {
        fclose (F);
        free (Buf);
        return 0;
    }
    fclose (F);
    Buf[Size] = '\0';
    if (Len) {
        *Len = (size_t) Size;
    }
    return Buf;
}



static int WfBinEqual (const char* Path, const unsigned char* Data, size_t Len)
/* Return true if the file at Path holds exactly the given bytes */
{
    size_t Have = 0;
    unsigned char* Buf = (unsigned char*) WfReadFile (Path, &Have);
    int Eq = Buf != 0 && Have == Len && memcmp (Buf, Data, Len) == 0;
    free (Buf);
    return Eq;
}



static int WfFileHas (const char* Path, const char* Needle)
/* Return true if the text file at Path contains Needle */
{
    char* Buf = WfReadFile (Path, 0);
    int Has = Buf != 0 && strstr (Buf, Needle) != 0;
    free (Buf);
    return Has;
}



static int WfDa65 (const char* Name, const unsigned char* Code, size_t Len,
                   const char* State, char* OutPath)
/* Run the da65 CLI in auto mode on the given code. Returns the exit code
** of da65. If OutPath is not null, the path of the output file is copied
** there (it must hold at least 128 chars).
*/
{
    char Bin[128], Out[128], Cmd[600];
    int Rc;

    WfEnsureDir ();
    snprintf (Bin, sizeof (Bin), WF_DIR "/wf_%s.bin", Name);
    snprintf (Out, sizeof (Out), WF_DIR "/wf_%s.s", Name);
    if (!WfWriteBin (Bin, Code, Len)) {
        return -1;
    }
    remove (Out);
    snprintf (Cmd, sizeof (Cmd),
              "bin/da65 --cpu 65816 --start-addr 0x%x --auto-65816 %s "
              "-o %s %s", WF_START, State, Out, Bin);
    Rc = system (Cmd);
    if (OutPath) {
        strcpy (OutPath, Out);
    }
    return Rc;
}



static int WfRoundtrip (const char* Name, const unsigned char* Code,
                        size_t Len, const char* State, char* OutPath)
/* Disassemble Code with the da65 CLI in auto mode, reassemble the output
** with the repo ca65/ld65 and compare the bytes. Returns zero on success.
*/
{
    char S[128], Obj[128], ReBin[128], Cmd[600];

    if (WfDa65 (Name, Code, Len, State, S) != 0) {
        return 1;
    }
    snprintf (Obj, sizeof (Obj), WF_DIR "/wf_%s.o", Name);
    snprintf (ReBin, sizeof (ReBin), WF_DIR "/wf_%s.re.bin", Name);
    remove (ReBin);
    snprintf (Cmd, sizeof (Cmd), "bin/ca65 -o %s %s", Obj, S);
    if (system (Cmd) != 0) {
        return 2;
    }
    snprintf (Cmd, sizeof (Cmd), "bin/ld65 -o %s -C test/dasm/65816.cfg %s",
              ReBin, Obj);
    if (system (Cmd) != 0) {
        return 3;
    }
    if (!WfBinEqual (ReBin, Code, Len)) {
        return 4;
    }
    if (OutPath) {
        strcpy (OutPath, S);
    }
    return 0;
}



static unsigned WfOneState (unsigned State)
/* Return the state set mask that holds exactly the given state */
{
    return 1u << State;
}



/*****************************************************************************/
/*                             Group "modes"                                 */
/*****************************************************************************/



static int ModesRejectsEmulationWithoutMX (void)
/* E=1 with M=0 or X=0 must be rejected */
{
    static const unsigned char Code[] = { 0x60 };
    TASK_CHECK (WfDa65 ("m_emx1", Code, sizeof (Code), "1,0,1,0", 0) != 0);
    TASK_CHECK (WfDa65 ("m_emx2", Code, sizeof (Code), "1,1,0,0", 0) != 0);
    TASK_CHECK (WfDa65 ("m_emx3", Code, sizeof (Code), "1,0,0,1", 0) != 0);
    return 0;
}



static int ModesRejectsWrongCPU (void)
/* --auto-65816 is only valid together with --cpu 65816 */
{
    static const unsigned char Code[] = { 0x60 };
    char Cmd[300];
    int Rc;

    WfEnsureDir ();
    TASK_CHECK (WfWriteBin (WF_DIR "/wf_m_cpu.bin", Code, sizeof (Code)));
    snprintf (Cmd, sizeof (Cmd),
              "bin/da65 --cpu 6502 --auto-65816 0,0,0,0 "
              "-o " WF_DIR "/wf_m_cpu.s " WF_DIR "/wf_m_cpu.bin");
    Rc = system (Cmd);
    TASK_CHECK (Rc != 0);
    snprintf (Cmd, sizeof (Cmd),
              "bin/da65 --auto-65816 0,0,0,0 "
              "-o " WF_DIR "/wf_m_cpu.s " WF_DIR "/wf_m_cpu.bin");
    Rc = system (Cmd);
    TASK_CHECK (Rc != 0);
    return 0;
}



static int ModesRejectsInfoFile (void)
/* --auto-65816 cannot be combined with an info file */
{
    static const unsigned char Code[] = { 0x60 };
    char Cmd[400];
    int Rc;

    WfEnsureDir ();
    TASK_CHECK (WfWriteBin (WF_DIR "/wf_m_info.bin", Code, sizeof (Code)));
    snprintf (Cmd, sizeof (Cmd),
              "bin/da65 --cpu 65816 --auto-65816 0,1,1,0 "
              "-i task_support/state.info "
              "-o " WF_DIR "/wf_m_info.s " WF_DIR "/wf_m_info.bin");
    Rc = system (Cmd);
    TASK_CHECK (Rc != 0);
    return 0;
}



static int ModesRejectsBadArgument (void)
/* Malformed state arguments must be rejected */
{
    static const unsigned char Code[] = { 0x60 };
    TASK_CHECK (WfDa65 ("m_bad1", Code, sizeof (Code), "1,1,1", 0) != 0);
    TASK_CHECK (WfDa65 ("m_bad2", Code, sizeof (Code), "1,2,1,0", 0) != 0);
    TASK_CHECK (WfDa65 ("m_bad3", Code, sizeof (Code), "0,0,0,0,0", 0) != 0);
    TASK_CHECK (WfDa65 ("m_bad4", Code, sizeof (Code), "abcd", 0) != 0);
    return 0;
}



static int ModesAcceptsValidStates (void)
/* Valid combinations run successfully, including emulation mode */
{
    static const unsigned char Code[] = { 0x60 };
    char Out[128];
    TASK_CHECK (WfDa65 ("m_ok1", Code, sizeof (Code), "0,0,0,0", Out) == 0);
    TASK_CHECK (WfFileHas (Out, "rts"));
    TASK_CHECK (WfDa65 ("m_ok2", Code, sizeof (Code), "1,1,1,0", Out) == 0);
    TASK_CHECK (WfDa65 ("m_ok3", Code, sizeof (Code), "0,1,0,1", Out) == 0);
    return 0;
}



/*****************************************************************************/
/*                              Group "flow"                                 */
/*****************************************************************************/



static int FlowRepSepStates (void)
/* REP/SEP update M and X and the immediate width follows the entry state */
{
    /* rep #$30; lda #$1234; sep #$20; lda #$56; rts */
    static const unsigned char Code[] = {
        0xc2, 0x30, 0xa9, 0x34, 0x12, 0xe2, 0x20, 0xa9, 0x56, 0x60
    };
    WfResult R;
    TASK_CHECK (WfAnalyze (Code, sizeof (Code), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfStateSetAt (&R, WF_START + 0) == WfOneState (WF_M | WF_X));
    TASK_CHECK (WfStateSetAt (&R, WF_START + 2) == WfOneState (0));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 2) == 3);
    TASK_CHECK (WfStateSetAt (&R, WF_START + 5) == WfOneState (0));
    /* SEP #$20 sets M only, X stays clear */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 7) == WfOneState (WF_M));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 7) == 2);
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 9) == 1);
    /* Operand bytes carry no state of their own */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 3) == 0);
    WfDone (&R);
    return 0;
}



static int FlowSecClcStates (void)
/* SEC/CLC set and clear the carry */
{
    /* sec; clc; rts */
    static const unsigned char Code[] = { 0x38, 0x18, 0x60 };
    WfResult R;
    TASK_CHECK (WfAnalyze (Code, sizeof (Code), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfStateSetAt (&R, WF_START + 0) == WfOneState (WF_M | WF_X));
    TASK_CHECK (WfStateSetAt (&R, WF_START + 1) ==
                WfOneState (WF_M | WF_X | WF_C));
    TASK_CHECK (WfStateSetAt (&R, WF_START + 2) == WfOneState (WF_M | WF_X));
    WfDone (&R);
    return 0;
}



static int FlowXceStates (void)
/* XCE swaps E and C; entering emulation mode forces M=X=1 and REP cannot
** clear the flags there.
*/
{
    /* sec; xce; rts */
    static const unsigned char CodeA[] = { 0x38, 0xfb, 0x60 };
    /* xce; rep #$20; lda #$00; rts (entered with C=1) */
    static const unsigned char CodeB[] = { 0xfb, 0xc2, 0x20, 0xa9, 0x00, 0x60 };
    WfResult R;

    TASK_CHECK (WfAnalyze (CodeA, sizeof (CodeA), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfStateSetAt (&R, WF_START + 1) ==
                WfOneState (WF_M | WF_X | WF_C));
    /* XCE: E becomes the old C, C becomes the old E */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 2) ==
                WfOneState (WF_E | WF_M | WF_X));
    WfDone (&R);

    TASK_CHECK (WfAnalyze (CodeB, sizeof (CodeB), WF_START, 0, 0, 0, 1, &R));
    /* E=1 forces M=X=1 */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 1) ==
                WfOneState (WF_E | WF_M | WF_X));
    /* REP #$20 cannot clear M in emulation mode */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 3) ==
                WfOneState (WF_E | WF_M | WF_X));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 3) == 2);
    WfDone (&R);
    return 0;
}



static int FlowCarryDualPath (void)
/* ADC recomputes the carry: both values flow into a following BCC/BCS and
** known carries prune the impossible edge.
*/
{
    /* sec; adc #$01; bcc +1; nop; rts */
    static const unsigned char CodeA[] = { 0x38, 0x69, 0x01, 0x90, 0x01,
                                           0xea, 0x60 };
    /* sec; bcs +1; nop; rts */
    static const unsigned char CodeB[] = { 0x38, 0xb0, 0x01, 0xea, 0x60 };
    /* clc; bcc +1; nop; rts */
    static const unsigned char CodeC[] = { 0x18, 0x90, 0x01, 0xea, 0x60 };
    /* sec; bcc +1; nop; rts */
    static const unsigned char CodeD[] = { 0x38, 0x90, 0x01, 0xea, 0x60 };
    WfResult R;

    TASK_CHECK (WfAnalyze (CodeA, sizeof (CodeA), WF_START, 0, 1, 1, 0, &R));
    /* After ADC both carry values are possible */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 3) ==
                (WfOneState (WF_M | WF_X) | WfOneState (WF_M | WF_X | WF_C)));
    /* Both edges of the BCC are followed and merge again */
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 5) == 1);
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 6) == 1);
    TASK_CHECK (WfStateSetAt (&R, WF_START + 6) ==
                (WfOneState (WF_M | WF_X) | WfOneState (WF_M | WF_X | WF_C)));
    WfDone (&R);

    /* C known to be 1: BCS only follows the target */
    TASK_CHECK (WfAnalyze (CodeB, sizeof (CodeB), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 3) == 0);
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 4) == 1);
    WfDone (&R);

    /* C known to be 0: BCC only follows the target */
    TASK_CHECK (WfAnalyze (CodeC, sizeof (CodeC), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 3) == 0);
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 4) == 1);
    WfDone (&R);

    /* C known to be 1: BCC only falls through */
    TASK_CHECK (WfAnalyze (CodeD, sizeof (CodeD), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 3) == 1);
    TASK_CHECK (WfStateSetAt (&R, WF_START + 4) ==
                WfOneState (WF_M | WF_X | WF_C));
    WfDone (&R);
    return 0;
}



static int FlowBranchMerge (void)
/* Different states may merge at one address; decoding continues as long as
** the instruction lengths agree.
*/
{
    /* bne +2; rep #$20; rep #$10; rts */
    static const unsigned char Code[] = { 0xd0, 0x02, 0xc2, 0x20,
                                          0xc2, 0x10, 0x60 };
    WfResult R;
    TASK_CHECK (WfAnalyze (Code, sizeof (Code), WF_START, 0, 1, 1, 0, &R));
    /* The branch target is reached with M=1 (branch) and M=0 (REP path) */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 4) ==
                (WfOneState (WF_M | WF_X) | WfOneState (WF_X)));
    /* After REP #$10 the merged states differ in M only */
    TASK_CHECK (WfStateSetAt (&R, WF_START + 6) ==
                (WfOneState (WF_M) | WfOneState (0)));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 6) == 1);
    WfDone (&R);
    return 0;
}



static int FlowPlpWidthAmbiguity (void)
/* PLP makes M unknown; a following M immediate has no unique width */
{
    /* plp; lda #$1234; rts */
    static const unsigned char Code[] = { 0x28, 0xa9, 0x34, 0x12, 0x60 };
    WfResult R;
    TASK_CHECK (!WfAnalyze (Code, sizeof (Code), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (R.DiagCount >= 1);
    TASK_CHECK (R.Diags[0].Addr == WF_START + 1);
    TASK_CHECK (strstr (R.Diags[0].Text, "8001") != 0);
    /* No unique length at the ambiguous position */
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 1) == 0);
    WfDone (&R);
    return 0;
}



static int FlowLoopStates (void)
/* Loops terminate because offset/state pairs are visited only once */
{
    /* lda #$00; bra -4 (to the lda) */
    static const unsigned char CodeA[] = { 0xa9, 0x00, 0x80, 0xfc };
    /* brl -3 (to itself) */
    static const unsigned char CodeB[] = { 0x82, 0xfd, 0xff };
    WfResult R;

    TASK_CHECK (WfAnalyze (CodeA, sizeof (CodeA), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 0) == 2);
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 2) == 2);
    TASK_CHECK (WfStateSetAt (&R, WF_START + 0) == WfOneState (WF_M | WF_X));
    WfDone (&R);

    TASK_CHECK (WfAnalyze (CodeB, sizeof (CodeB), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (WfInsnLenAt (&R, WF_START + 0) == 3);
    WfDone (&R);
    return 0;
}



static int FlowLengthConflict (void)
/* Two paths reach the same address with different M: the immediate there
** has no unique length and must be reported.
*/
{
    /* beq +2; sep #$20; lda #$1234; rts -- entered with M=0 */
    static const unsigned char Code[] = { 0xf0, 0x02, 0xe2, 0x20,
                                          0xa9, 0x34, 0x12, 0x60 };
    WfResult R;
    TASK_CHECK (!WfAnalyze (Code, sizeof (Code), WF_START, 0, 0, 0, 0, &R));
    TASK_CHECK (R.DiagCount >= 1);
    TASK_CHECK (R.Diags[0].Addr == WF_START + 4);
    TASK_CHECK (strstr (R.Diags[0].Text, "8004") != 0);
    WfDone (&R);
    return 0;
}



static int FlowTruncation (void)
/* A wide immediate at the end of the buffer must not be padded with
** bytes from behind the buffer.
*/
{
    /* lda #$xx34 with M=0: needs three bytes, only two present */
    static const unsigned char Code[] = { 0xa9, 0x34 };
    WfResult R;
    TASK_CHECK (!WfAnalyze (Code, sizeof (Code), WF_START, 0, 0, 0, 0, &R));
    TASK_CHECK (R.DiagCount >= 1);
    TASK_CHECK (R.Diags[0].Addr == WF_START);
    TASK_CHECK (strstr (R.Diags[0].Text, "truncated") != 0);
    WfDone (&R);
    return 0;
}



static int FlowStartInOperand (void)
/* A branch into the operand of another instruction is an ambiguity */
{
    /* beq +1 (into the operand of the lda); lda #$03; rts */
    static const unsigned char Code[] = { 0xf0, 0x01, 0xa9, 0x03, 0x60 };
    WfResult R;
    TASK_CHECK (!WfAnalyze (Code, sizeof (Code), WF_START, 0, 1, 1, 0, &R));
    TASK_CHECK (R.DiagCount >= 1);
    TASK_CHECK (strstr (R.Diags[0].Text, "8003") != 0);
    WfDone (&R);
    return 0;
}



/*****************************************************************************/
/*                             Group "listing"                               */
/*****************************************************************************/



static int ListingAccumulatorRoundtrip (void)
/* REP/SEP switch the accumulator width; the listing reassembles to the
** identical bytes.
*/
{
    /* rep #$20; lda #$1234; sep #$20; lda #$56; rts */
    static const unsigned char Code[] = {
        0xc2, 0x20, 0xa9, 0x34, 0x12, 0xe2, 0x20, 0xa9, 0x56, 0x60
    };
    char Out[128];
    TASK_CHECK (WfRoundtrip ("l_acc", Code, sizeof (Code), "0,1,1,0", Out) == 0);
    TASK_CHECK (WfFileHas (Out, ".a16"));
    TASK_CHECK (WfFileHas (Out, "lda     #$1234"));
    TASK_CHECK (WfFileHas (Out, ".a8"));
    TASK_CHECK (WfFileHas (Out, "lda     #$56"));
    TASK_CHECK (WfFileHas (Out, "rts"));
    return 0;
}



static int ListingIndexRoundtrip (void)
/* REP/SEP of the X flag switch the index immediate width */
{
    /* rep #$10; ldx #$1234; sep #$10; ldx #$34; rts */
    static const unsigned char Code[] = {
        0xc2, 0x10, 0xa2, 0x34, 0x12, 0xe2, 0x10, 0xa2, 0x34, 0x60
    };
    char Out[128];
    TASK_CHECK (WfRoundtrip ("l_idx", Code, sizeof (Code), "0,1,1,0", Out) == 0);
    TASK_CHECK (WfFileHas (Out, ".i16"));
    TASK_CHECK (WfFileHas (Out, "ldx     #$1234"));
    TASK_CHECK (WfFileHas (Out, ".i8"));
    TASK_CHECK (WfFileHas (Out, "ldx     #$34"));
    return 0;
}



static int ListingDataTail (void)
/* Bytes behind a barrier are not reached and stay data */
{
    /* lda #$01; rts; then three data bytes */
    static const unsigned char Code[] = { 0xa9, 0x01, 0x60, 0xaa, 0xbb, 0xcc };
    char Out[128];
    TASK_CHECK (WfRoundtrip ("l_data", Code, sizeof (Code), "0,1,1,0", Out) == 0);
    TASK_CHECK (WfFileHas (Out, "lda     #$01"));
    TASK_CHECK (WfFileHas (Out, "rts"));
    TASK_CHECK (WfFileHas (Out, ".byte"));
    TASK_CHECK (WfFileHas (Out, "$AA"));
    return 0;
}



static int ListingBranchRoundtrip (void)
/* Branches get labels at the right places and the carry dependent edge
** pruning keeps the listing exact.
*/
{
    /* lda #$05; adc #$01; bcc L; sep #$20; L: rep #$20; rts */
    static const unsigned char Code[] = {
        0xa9, 0x05, 0x69, 0x01, 0x90, 0x02, 0xe2, 0x20, 0xc2, 0x20, 0x60
    };
    char Out[128];
    TASK_CHECK (WfRoundtrip ("l_bra", Code, sizeof (Code), "0,1,1,0", Out) == 0);
    TASK_CHECK (WfFileHas (Out, "bcc"));
    TASK_CHECK (WfFileHas (Out, "L8008"));
    TASK_CHECK (WfFileHas (Out, "sep     #$20"));
    TASK_CHECK (WfFileHas (Out, "rep     #$20"));
    return 0;
}



static int ListingEmulationRoundtrip (void)
/* XCE into emulation mode forces 8 bit immediates */
{
    /* sec; xce; lda #$42; ldx #$17; rts */
    static const unsigned char Code[] = { 0x38, 0xfb, 0xa9, 0x42,
                                          0xa2, 0x17, 0x60 };
    char Out[128];
    TASK_CHECK (WfRoundtrip ("l_emu", Code, sizeof (Code), "0,1,1,0", Out) == 0);
    TASK_CHECK (WfFileHas (Out, "xce"));
    TASK_CHECK (WfFileHas (Out, "lda     #$42"));
    TASK_CHECK (WfFileHas (Out, "ldx     #$17"));
    return 0;
}



static int ListingAmbiguityFails (void)
/* Ambiguous code makes da65 fail without publishing an output file */
{
    /* plp; lda #$1234; rts */
    static const unsigned char Code[] = { 0x28, 0xa9, 0x34, 0x12, 0x60 };
    char Out[128];
    FILE* F;

    TASK_CHECK (WfDa65 ("l_amb", Code, sizeof (Code), "0,1,1,0", Out) != 0);
    F = fopen (Out, "r");
    TASK_CHECK (F == 0);
    if (F) {
        fclose (F);
    }
    return 0;
}



static int ListingTruncationFails (void)
/* A truncated instruction makes da65 fail without an output file */
{
    /* lda #$xx34 with M=0 at the end of the buffer */
    static const unsigned char Code[] = { 0xa9, 0x34 };
    char Out[128];
    FILE* F;

    TASK_CHECK (WfDa65 ("l_trunc", Code, sizeof (Code), "0,0,0,0", Out) != 0);
    F = fopen (Out, "r");
    TASK_CHECK (F == 0);
    if (F) {
        fclose (F);
    }
    return 0;
}



/*****************************************************************************/
/*                              Case table                                   */
/*****************************************************************************/



const struct task_case task_cases[] = {
    { "modes",   "rejects_emulation_without_mx", ModesRejectsEmulationWithoutMX },
    { "modes",   "rejects_wrong_cpu",            ModesRejectsWrongCPU },
    { "modes",   "rejects_info_file",            ModesRejectsInfoFile },
    { "modes",   "rejects_bad_argument",         ModesRejectsBadArgument },
    { "modes",   "accepts_valid_states",         ModesAcceptsValidStates },
    { "flow",    "rep_sep_states",               FlowRepSepStates },
    { "flow",    "sec_clc_states",               FlowSecClcStates },
    { "flow",    "xce_states",                   FlowXceStates },
    { "flow",    "carry_dual_path",              FlowCarryDualPath },
    { "flow",    "branch_merge",                 FlowBranchMerge },
    { "flow",    "plp_width_ambiguity",          FlowPlpWidthAmbiguity },
    { "flow",    "loop_states",                  FlowLoopStates },
    { "flow",    "length_conflict",              FlowLengthConflict },
    { "flow",    "truncation",                   FlowTruncation },
    { "flow",    "start_in_operand",             FlowStartInOperand },
    { "listing", "accumulator_roundtrip",        ListingAccumulatorRoundtrip },
    { "listing", "index_roundtrip",              ListingIndexRoundtrip },
    { "listing", "data_tail",                    ListingDataTail },
    { "listing", "branch_roundtrip",             ListingBranchRoundtrip },
    { "listing", "emulation_roundtrip",          ListingEmulationRoundtrip },
    { "listing", "ambiguity_fails",              ListingAmbiguityFails },
    { "listing", "truncation_fails",             ListingTruncationFails },
};
const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
