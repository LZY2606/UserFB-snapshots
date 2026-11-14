/*****************************************************************************/
/*                                                                           */
/*                                widthflow.c                                */
/*                                                                           */
/*            65816 processor state flow analysis for the disassembler       */
/*                                                                           */
/*                                                                           */
/*                                                                           */
/* (C) 2026, Ullrich von Bassewitz                                           */
/*                Roemerstrasse 52                                           */
/*                D-70794 Filderstadt                                        */
/* EMail:         uz@cc65.org                                                */
/*                                                                           */
/*                                                                           */
/* This software is provided 'as-is', without any expressed or implied       */
/* warranty.  In no event will the authors be held liable for any damages    */
/* arising from the use of this software.                                    */
/*                                                                           */
/* Permission is granted to anyone to use this software for any purpose,     */
/* including commercial applications, and to alter it and redistribute it    */
/* freely, subject to the following restrictions:                            */
/*                                                                           */
/* 1. The origin of this software must not be misrepresented; you must not   */
/*    claim that you wrote the original software. If you use this software   */
/*    in a product, an acknowledgment in the product documentation would be  */
/*    appreciated but is not required.                                       */
/* 2. Altered source versions must be plainly marked as such, and must not   */
/*    be misrepresented as being the original software.                      */
/* 3. This notice may not be removed or altered from any source              */
/*    distribution.                                                          */
/*                                                                           */
/*****************************************************************************/



#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* common */
#include "xmalloc.h"

/* da65 */
#include "handler.h"
#include "opc65816.h"
#include "widthflow.h"



/*****************************************************************************/
/*                                   Data                                    */
/*****************************************************************************/



/* Coverage of a byte by the decoded instruction stream */
enum {
    cvNone,                     /* Byte not reached yet */
    cvStart,                    /* Byte is the start of an instruction */
    cvOperand                   /* Byte is part of an instruction operand */
};

/* One worklist entry: a state reaching an offset */
typedef struct WfWork WfWork;
struct WfWork {
    uint32_t    Offs;           /* Offset into the code */
    uint8_t     State;          /* Reaching state (WF_ bit combination) */
};

/* Internal analysis context */
typedef struct WfFlow WfFlow;
struct WfFlow {
    const uint8_t* Code;        /* Code to analyze */
    uint32_t    Len;            /* Number of code bytes */
    uint32_t    StartAddr;      /* Load address of the code */
    WfResult    Res;            /* Result under construction */
    uint8_t*    Cover;          /* Coverage per offset */
    uint32_t*   Owner;          /* Insn start covering an offset */
    WfWork*     Work;           /* Worklist */
    unsigned    WorkCount;      /* Entries in the worklist */
    unsigned    WorkAlloc;      /* Allocated worklist entries */
    unsigned    DiagAlloc;      /* Allocated diagnostic entries */
};



/*****************************************************************************/
/*                                   Code                                    */
/*****************************************************************************/



static unsigned WfNormalize (unsigned State)
/* Enforce the hardware invariant: in emulation mode M and X are always set */
{
    if (State & WF_E) {
        State |= WF_M | WF_X;
    }
    return State;
}



static unsigned WfLowBit (unsigned Mask)
/* Return the bit number of the lowest bit set in Mask (Mask must be != 0) */
{
    unsigned N = 0;
    while ((Mask & 1u) == 0) {
        Mask >>= 1;
        ++N;
    }
    return N;
}



static void WfDiagf (WfFlow* F, uint32_t Addr, const char* Format, ...)
/* Add a diagnostic message to the result. The text is owned by the result. */
{
    char    Buf[256];
    va_list ap;
    WfDiag* D;

    va_start (ap, Format);
    vsnprintf (Buf, sizeof (Buf), Format, ap);
    va_end (ap);

    if (F->Res.DiagCount >= F->DiagAlloc) {
        F->DiagAlloc = F->DiagAlloc? F->DiagAlloc * 2 : 8;
        F->Res.Diags = xrealloc (F->Res.Diags, F->DiagAlloc * sizeof (WfDiag));
    }
    D = &F->Res.Diags[F->Res.DiagCount++];
    D->Addr = Addr;
    D->Text = xstrdup (Buf);
}



static void WfEnqueue (WfFlow* F, uint32_t Offs, unsigned State)
/* Add a state/offset pair to the worklist unless it is already known */
{
    WfWork* W;

    /* Leaving the code buffer ends this path */
    if (Offs >= F->Len) {
        return;
    }

    /* Each offset/state pair is processed at most once */
    State = WfNormalize (State);
    if (F->Res.StateSets[Offs] & (1u << State)) {
        return;
    }
    F->Res.StateSets[Offs] |= (uint16_t) (1u << State);

    if (F->WorkCount >= F->WorkAlloc) {
        F->WorkAlloc = F->WorkAlloc? F->WorkAlloc * 2 : 64;
        F->Work = xrealloc (F->Work, F->WorkAlloc * sizeof (WfWork));
    }
    W = &F->Work[F->WorkCount++];
    W->Offs  = Offs;
    W->State = (uint8_t) State;
}



static unsigned WfNextStates (const WfFlow* F, uint32_t Offs, unsigned State,
                              unsigned char Out[8])
/* Compute the successor states of State after executing the instruction at
** Offs. Returns the number of successor states (1..8).
*/
{
    unsigned char Opc = F->Code[Offs];

    switch (Opc65816StateEffect (Opc)) {

        case fxRep: {
            /* REP clears the status bits given in the immediate */
            unsigned Imm = F->Code[Offs + 1];
            if (Imm & 0x01) State &= ~WF_C;
            if (Imm & 0x10) State &= ~WF_X;
            if (Imm & 0x20) State &= ~WF_M;
            Out[0] = (unsigned char) WfNormalize (State);
            return 1;
        }

        case fxSep: {
            /* SEP sets the status bits given in the immediate */
            unsigned Imm = F->Code[Offs + 1];
            if (Imm & 0x01) State |= WF_C;
            if (Imm & 0x10) State |= WF_X;
            if (Imm & 0x20) State |= WF_M;
            Out[0] = (unsigned char) WfNormalize (State);
            return 1;
        }

        case fxClc:
            Out[0] = (unsigned char) (State & ~WF_C);
            return 1;

        case fxSec:
            Out[0] = (unsigned char) (State | WF_C);
            return 1;

        case fxXce: {
            /* Exchange the carry and the emulation flag */
            unsigned New = State & ~(WF_E | WF_C);
            if (State & WF_C) New |= WF_E;
            if (State & WF_E) New |= WF_C;
            Out[0] = (unsigned char) WfNormalize (New);
            return 1;
        }

        case fxPlp: {
            /* M, X and C are pulled from the stack: all legal combinations.
            ** E is not part of the status register and is kept.
            */
            unsigned N = 0;
            unsigned M, X, C;
            unsigned Seen = 0;
            for (M = 0; M <= 1; ++M) {
                for (X = 0; X <= 1; ++X) {
                    for (C = 0; C <= 1; ++C) {
                        unsigned New = State & WF_E;
                        if (M) New |= WF_M;
                        if (X) New |= WF_X;
                        if (C) New |= WF_C;
                        New = WfNormalize (New);
                        /* Emulation mode collapses M and X to 1, avoid
                        ** generating the same state twice.
                        */
                        if ((Seen & (1u << New)) == 0) {
                            Seen |= 1u << New;
                            Out[N++] = (unsigned char) New;
                        }
                    }
                }
            }
            return N;
        }

        case fxCarry:
            /* The carry is recomputed, both values are possible */
            Out[0] = (unsigned char) (State & ~WF_C);
            Out[1] = (unsigned char) (State | WF_C);
            return 2;

        default:
            Out[0] = (unsigned char) State;
            return 1;
    }
}



static uint32_t WfBranchTarget (const WfFlow* F, uint32_t Offs, unsigned Size)
/* Compute the target of a branch or jump instruction at Offs. Size is the
** instruction size; the displacement is relative to the instruction end.
*/
{
    const OpcDesc* D = &OpcTable_65816[F->Code[Offs]];

    if (D->Handler == OH_JmpAbsolute) {
        /* Direct jump: absolute 16 bit operand */
        return F->Code[Offs + 1] | ((uint32_t) F->Code[Offs + 2] << 8);
    } else if (Size == 3) {
        /* 16 bit displacement (BRL) */
        int32_t Disp = (int16_t) (F->Code[Offs + 1] |
                                  ((uint32_t) F->Code[Offs + 2] << 8));
        return (uint32_t) ((F->StartAddr + Offs + Size + Disp) & 0xFFFF);
    } else {
        /* 8 bit displacement */
        int32_t Disp = (int8_t) F->Code[Offs + 1];
        return (uint32_t) ((F->StartAddr + Offs + Size + Disp) & 0xFFFF);
    }
}



static void WfFollow (WfFlow* F, uint32_t Target, const unsigned char* States,
                      unsigned StateCount)
/* Follow a control transfer to the absolute address Target with the given
** successor states. Targets outside the code buffer leave the analysis.
*/
{
    unsigned I;
    if (Target < F->StartAddr || Target - F->StartAddr >= F->Len) {
        return;
    }
    for (I = 0; I < StateCount; ++I) {
        WfEnqueue (F, Target - F->StartAddr, States[I]);
    }
}



static void WfStep (WfFlow* F, uint32_t Offs, unsigned State)
/* Decode the instruction at Offs in State and propagate the flow */
{
    unsigned char Opc = F->Code[Offs];
    unsigned char Next[8];
    unsigned    I, N, Size, Bit;
    uint32_t    Addr = F->StartAddr + Offs;

    /* The instruction length depends on the state on entry */
    Size = Opc65816InsnSize (Opc, (State & WF_M) != 0, (State & WF_X) != 0);

    /* A truncated instruction must not be padded with bytes from behind
    ** the buffer.
    */
    if (Offs + Size > F->Len) {
        WfDiagf (F, Addr,
                 "instruction at $%04" PRIX32 " is truncated: needs %u "
                 "bytes, only %u left", Addr, Size,
                 (unsigned) (F->Len - Offs));
        F->Res.Ok = 0;
        return;
    }

    /* All states reaching this offset must agree on the instruction length */
    Bit = 1u << Size;
    if (F->Res.LenCandidates[Offs] == 0) {
        F->Res.LenCandidates[Offs] = (uint8_t) Bit;
    } else if ((F->Res.LenCandidates[Offs] & Bit) == 0) {
        unsigned Other = WfLowBit (F->Res.LenCandidates[Offs]);
        F->Res.LenCandidates[Offs] |= (uint8_t) Bit;
        WfDiagf (F, Addr,
                 "ambiguous instruction length at $%04" PRIX32 ": %u and "
                 "%u bytes", Addr, Other, Size);
        F->Res.Ok = 0;
        return;
    }

    /* The instruction start must not lie in the operand of another
    ** instruction, and its operand must not cover another start.
    */
    if (F->Cover[Offs] == cvOperand) {
        WfDiagf (F, Addr,
                 "instruction at $%04" PRIX32 " starts inside the operand "
                 "of the instruction at $%04" PRIX32,
                 Addr, F->StartAddr + F->Owner[Offs]);
        F->Res.Ok = 0;
        return;
    }
    F->Cover[Offs] = cvStart;
    F->Owner[Offs] = Offs;
    for (I = 1; I < Size; ++I) {
        if (F->Cover[Offs + I] == cvStart) {
            WfDiagf (F, Addr,
                     "operand of the instruction at $%04" PRIX32 " overlaps "
                     "the instruction at $%04" PRIX32,
                     Addr, F->StartAddr + Offs + I);
            F->Res.Ok = 0;
            return;
        }
        F->Cover[Offs + I] = cvOperand;
        F->Owner[Offs + I] = Offs;
    }

    /* Compute the successor states and propagate the control flow */
    N = WfNextStates (F, Offs, State, Next);
    switch (Opc65816FlowKind (Opc)) {

        case fkNormal:
            for (I = 0; I < N; ++I) {
                WfEnqueue (F, Offs + Size, Next[I]);
            }
            break;

        case fkCond:
            /* Condition on an untracked flag: keep both edges */
            for (I = 0; I < N; ++I) {
                WfEnqueue (F, Offs + Size, Next[I]);
            }
            WfFollow (F, WfBranchTarget (F, Offs, Size), Next, N);
            break;

        case fkCondC0:
            /* BCC: prune the edge that the known carry makes impossible */
            for (I = 0; I < N; ++I) {
                if (Next[I] & WF_C) {
                    WfEnqueue (F, Offs + Size, Next[I]);
                } else {
                    WfFollow (F, WfBranchTarget (F, Offs, Size), &Next[I], 1);
                }
            }
            break;

        case fkCondC1:
            /* BCS: prune the edge that the known carry makes impossible */
            for (I = 0; I < N; ++I) {
                if (Next[I] & WF_C) {
                    WfFollow (F, WfBranchTarget (F, Offs, Size), &Next[I], 1);
                } else {
                    WfEnqueue (F, Offs + Size, Next[I]);
                }
            }
            break;

        case fkJump:
            /* Unconditional transfer: only the target is reachable */
            WfFollow (F, WfBranchTarget (F, Offs, Size), Next, N);
            break;

        case fkStop:
            /* Barrier: do not assume anything about a possible return */
            break;
    }
}



int WfAnalyze (const uint8_t* Code, uint32_t Len, uint32_t StartAddr,
               unsigned E, unsigned M, unsigned X, unsigned C,
               WfResult* Res)
/* Run the 65816 state flow analysis over Code, which holds Len bytes loaded
** at StartAddr. E, M, X and C are the processor state bits at the start
** address, which is the only entry point. The result is placed into Res and
** must be released with WfDone. Returns true if the code could be decoded
** without ambiguities; in this case every reachable offset has exactly one
** instruction length candidate. Otherwise Res->Diags describes the problems
** (ambiguities or truncated instructions) together with their addresses.
*/
{
    WfFlow F;
    unsigned State = 0;

    /* Initialize the context */
    memset (&F, 0, sizeof (F));
    F.Code        = Code;
    F.Len         = Len;
    F.StartAddr   = StartAddr;
    F.Res.StartAddr = StartAddr;
    F.Res.Len     = Len;
    F.Res.Ok      = 1;
    if (Len > 0) {
        F.Res.StateSets     = (uint16_t*) xmalloc (Len * sizeof (uint16_t));
        F.Res.LenCandidates = (uint8_t*)  xmalloc (Len * sizeof (uint8_t));
        F.Cover             = (uint8_t*)  xmalloc (Len * sizeof (uint8_t));
        F.Owner             = (uint32_t*) xmalloc (Len * sizeof (uint32_t));
        memset (F.Res.StateSets, 0, Len * sizeof (uint16_t));
        memset (F.Res.LenCandidates, 0, Len * sizeof (uint8_t));
        memset (F.Cover, 0, Len * sizeof (uint8_t));
        memset (F.Owner, 0, Len * sizeof (uint32_t));
    }

    /* The start address is the only entry point */
    if (E) State |= WF_E;
    if (M) State |= WF_M;
    if (X) State |= WF_X;
    if (C) State |= WF_C;
    WfEnqueue (&F, 0, State);

    /* Process the worklist until it is empty or a problem is found */
    while (F.WorkCount > 0 && F.Res.Ok) {
        WfWork W = F.Work[--F.WorkCount];
        WfStep (&F, W.Offs, W.State);
    }

    /* Done, move the result to the caller */
    xfree (F.Work);
    xfree (F.Cover);
    xfree (F.Owner);
    *Res = F.Res;
    return Res->Ok;
}



unsigned WfStateSetAt (const WfResult* Res, uint32_t Addr)
/* Return the set of states (bitmask of WF_ bit combinations) that reach the
** given address, or zero if the address is not reachable code.
*/
{
    if (Addr < Res->StartAddr || Addr - Res->StartAddr >= Res->Len) {
        return 0;
    }
    return Res->StateSets[Addr - Res->StartAddr];
}



unsigned WfInsnLenAt (const WfResult* Res, uint32_t Addr)
/* Return the instruction length at the given address, or zero if the
** address is not the start of a reachable instruction. The result is only
** meaningful if the analysis reported no problems.
*/
{
    unsigned Mask;
    if (Addr < Res->StartAddr || Addr - Res->StartAddr >= Res->Len) {
        return 0;
    }
    Mask = Res->LenCandidates[Addr - Res->StartAddr];
    if (Mask == 0 || (Mask & (Mask - 1)) != 0) {
        /* No candidate, or more than one (ambiguous) */
        return 0;
    }
    return WfLowBit (Mask);
}



void WfDone (WfResult* Res)
/* Release all memory held by an analysis result */
{
    unsigned I;
    for (I = 0; I < Res->DiagCount; ++I) {
        xfree (Res->Diags[I].Text);
    }
    xfree (Res->Diags);
    xfree (Res->StateSets);
    xfree (Res->LenCandidates);
    memset (Res, 0, sizeof (*Res));
}
