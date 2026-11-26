/*****************************************************************************/
/*                                                                           */
/*                                widthflow.c                                */
/*                                                                           */
/*                  65816 state-width control flow analysis                   */
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



#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "widthflow.h"



/*****************************************************************************/
/*                                   Code                                    */
/*****************************************************************************/



int WfParseState (const char* Text, WfState* State)
/* See header. */
{
    unsigned Values[4];
    unsigned I;

    if (Text == 0 || State == 0) {
        return 0;
    }

    for (I = 0; I < 4; ++I) {
        if (*Text != '0' && *Text != '1') {
            return 0;
        }
        Values[I] = (unsigned) (*Text++ - '0');
        if (I < 3) {
            if (*Text++ != ',') {
                return 0;
            }
        }
    }

    if (*Text != '\0') {
        return 0;
    }

    State->E = (unsigned char) Values[0];
    State->M = (unsigned char) Values[1];
    State->X = (unsigned char) Values[2];
    State->C = (unsigned char) Values[3];
    return OpcFlowStateValid (State);
}



static int AddDiagnostic (WfResult* Result, WfDiagnosticKind Kind,
                          uint32_t Address, uint32_t OtherAddress,
                          uint8_t Opcode, uint8_t LengthMask,
                          const char* Message)
/* Append one owned diagnostic. */
{
    WfDiagnostic* D;
    size_t        NewCount = Result->DiagnosticCount + 1;

    D = realloc (Result->Diagnostics, NewCount * sizeof (*D));
    if (D == 0) {
        return 0;
    }
    Result->Diagnostics = D;
    D = &D[Result->DiagnosticCount];
    memset (D, 0, sizeof (*D));

    D->Kind = Kind;
    D->Address = Address;
    D->OtherAddress = OtherAddress;
    D->Opcode = Opcode;
    D->LengthMask = LengthMask;
    snprintf (D->Message, sizeof (D->Message), "%s", Message);

    Result->DiagnosticCount = NewCount;
    return 1;
}



static int AddDiagnosticF (WfResult* Result, WfDiagnosticKind Kind,
                           uint32_t Address, uint32_t OtherAddress,
                           uint8_t Opcode, uint8_t LengthMask,
                           const char* Format, ...);

static int AddDiagnosticF (WfResult* Result, WfDiagnosticKind Kind,
                           uint32_t Address, uint32_t OtherAddress,
                           uint8_t Opcode, uint8_t LengthMask,
                           const char* Format, ...)
{
    WfDiagnostic* D;
    va_list       Args;
    size_t        NewCount = Result->DiagnosticCount + 1;

    D = realloc (Result->Diagnostics, NewCount * sizeof (*D));
    if (D == 0) {
        return 0;
    }
    Result->Diagnostics = D;
    D = &D[Result->DiagnosticCount];
    memset (D, 0, sizeof (*D));

    D->Kind = Kind;
    D->Address = Address;
    D->OtherAddress = OtherAddress;
    D->Opcode = Opcode;
    D->LengthMask = LengthMask;

    va_start (Args, Format);
    vsnprintf (D->Message, sizeof (D->Message), Format, Args);
    va_end (Args);

    Result->DiagnosticCount = NewCount;
    return 1;
}



static void EnqueueStates (WfSite* Site, uint32_t* Queue, size_t* Tail,
                           size_t Position, WfStateMask NewStates)
/* Add only state pairs not already reached.  Each dense pair is queued once. */
{
    WfStateMask Fresh =
        NewStates & (WfStateMask) ~(unsigned) Site->StateMask;
    unsigned    I;

    Site->StateMask |= NewStates;

    for (I = 0; I < OPCFLOW_STATE_COUNT; ++I) {
        WfStateMask Bit = (WfStateMask) (1u << I);
        if ((Fresh & Bit) != 0) {
            Queue[(*Tail)++] = (uint32_t) ((Position << 4) | I);
        }
    }
}



static int TargetOffset (const WfResult* Result, uint32_t Target,
                         size_t* Offset)
/* Map an address target into the supplied interval.  Outside is a clean
** path exit, not an analysis error.
*/
{
    uint32_t End = Result->StartAddress + (uint32_t) Result->Size;
    if (Target >= Result->StartAddress && Target < End) {
        *Offset = (size_t) (Target - Result->StartAddress);
        return 1;
    }
    return 0;
}



static int BranchIsTaken (unsigned char Condition, const WfState* State)
/* For flags we do not track, both outcomes remain possible. */
{
    switch (Condition) {
        case OPCFLOW_COND_BCC:
            return State->C == 0;
        case OPCFLOW_COND_BCS:
            return State->C == 1;
        default:
            return 1;
    }
}



static int CompareDiagnostics (const void* A, const void* B)
/* Keep reports independent of worklist insertion order. */
{
    const WfDiagnostic* DA = A;
    const WfDiagnostic* DB = B;

    if (DA->Address != DB->Address) {
        return DA->Address < DB->Address ? -1 : 1;
    }
    if (DA->Kind != DB->Kind) {
        return (int) DA->Kind - (int) DB->Kind;
    }
    if (DA->OtherAddress != DB->OtherAddress) {
        return DA->OtherAddress < DB->OtherAddress ? -1 : 1;
    }
    return 0;
}



WfResult* WfAnalyze (const uint8_t* Bytes, size_t Size,
                     uint32_t StartAddress, const WfState* InitialState)
/* See header. */
{
    WfResult*     Result = 0;
    uint32_t*     Queue = 0;
    size_t*       OverlapOwner = 0;
    size_t        Head = 0, Tail = 0;
    size_t        I;
    unsigned      InitialIndex;

    if (InitialState == 0 || !OpcFlowStateValid (InitialState)) {
        return 0;
    }
    if (StartAddress > 0xFFFFu || Size > 0x10000u ||
        (uint64_t) StartAddress + (uint64_t) Size > 0x10000ull) {
        return 0;
    }
    if (Size > 0 && Bytes == 0) {
        return 0;
    }

    Result = calloc (1, sizeof (*Result));
    if (Result == 0) {
        return 0;
    }
    Result->StartAddress = StartAddress;
    Result->Size = Size;
    Result->Clean = 1;

    if (Size == 0) {
        return Result;
    }

    Result->SiteCount = Size;
    Result->Sites = calloc (Size, sizeof (*Result->Sites));
    Queue = malloc ((size_t) OPCFLOW_STATE_COUNT * Size * sizeof (*Queue));
    OverlapOwner = malloc (Size * sizeof (*OverlapOwner));
    if (Result->Sites == 0 || Queue == 0 || OverlapOwner == 0) {
        WfResultFree (Result);
        free (Queue);
        free (OverlapOwner);
        return 0;
    }

    for (I = 0; I < Size; ++I) {
        Result->Sites[I].Address = StartAddress + (uint32_t) I;
        OverlapOwner[I] = SIZE_MAX;
    }

    InitialIndex = OpcFlowStateIndex (InitialState);
    EnqueueStates (&Result->Sites[0], Queue, &Tail, 0,
                   (WfStateMask) (1u << InitialIndex));

    while (Head < Tail) {
        uint32_t        Encoded = Queue[Head++];
        size_t          Pos = (size_t) (Encoded >> 4);
        unsigned        StateIndex = Encoded & 15u;
        WfSite*         Site = &Result->Sites[Pos];
        WfState         State;
        uint8_t         Opcode = Bytes[Pos];
        unsigned        Length;
        Opc65816Flow    Flow;
        WfStateMask     NextStates;
        uint32_t        Target;
        size_t          TargetPos;
        int             Taken, FallThrough;

        OpcFlowStateFromIndex (StateIndex, &State);
        Length = Opc65816Length (Opcode, &State);
        Site->LengthMask |= (uint8_t) (1u << Length);

        /* Never fabricate operand bytes from zero-filled storage. */
        if (Length == 0 || Pos + Length > Size) {
            if (!Site->Truncated) {
                char Message[160];
                Site->Truncated = 1;
                snprintf (Message, sizeof (Message),
                          "truncated instruction at $%04X: needs %u byte%s, "
                          "but only %zu remain",
                          Site->Address, Length, Length == 1 ? "" : "s",
                          Size - Pos);
                if (!AddDiagnostic (Result, WF_DIAG_TRUNCATED, Site->Address,
                                    StartAddress + (uint32_t) Size,
                                    Opcode, (uint8_t) (1u << Length),
                                    Message)) {
                    goto AllocFailure;
                }
            }
            continue;
        }

        Opc65816Describe (Opcode, &Flow);
        NextStates = Opc65816NextStates (
            Opcode, Length >= 2 ? Bytes[Pos + 1] : 0, StateIndex);

        switch (Flow.Kind) {

            case OPCFLOW_FLOW_CALL:
                /* JSR/JSL are deliberate analysis barriers.  The known
                ** target is described by the opcode layer for labels/output,
                ** but no callee or return-state edge is inferred here.
                */
                break;

            case OPCFLOW_FLOW_BARRIER:
                /* Indirect transfer, return, exception or stop. */
                break;

            case OPCFLOW_FLOW_UNCONDITIONAL:
                Target = Opc65816Target (Site->Address, Opcode, Length,
                                         Bytes + Pos, Size - Pos);
                if (Target != UINT32_MAX &&
                    TargetOffset (Result, Target, &TargetPos)) {
                    EnqueueStates (&Result->Sites[TargetPos], Queue, &Tail,
                                   TargetPos, NextStates);
                }
                break;

            case OPCFLOW_FLOW_CONDITIONAL:
                Taken = BranchIsTaken (Flow.Condition, &State);
                if (Flow.Condition == OPCFLOW_COND_BCC ||
                    Flow.Condition == OPCFLOW_COND_BCS) {
                    FallThrough = !Taken;
                } else {
                    /* N/V/Z are not abstracted, so retain both edges. */
                    FallThrough = 1;
                }

                if (Taken) {
                    Target = Opc65816Target (Site->Address, Opcode, Length,
                                             Bytes + Pos, Size - Pos);
                    if (Target != UINT32_MAX &&
                        TargetOffset (Result, Target, &TargetPos)) {
                        EnqueueStates (&Result->Sites[TargetPos], Queue,
                                       &Tail, TargetPos, NextStates);
                    }
                }

                if (FallThrough && Pos + Length < Size) {
                    EnqueueStates (&Result->Sites[Pos + Length], Queue,
                                   &Tail, Pos + Length, NextStates);
                }
                break;

            case OPCFLOW_FLOW_FALLTHROUGH:
            default:
                if (Pos + Length < Size) {
                    EnqueueStates (&Result->Sites[Pos + Length], Queue, &Tail,
                                   Pos + Length, NextStates);
                }
                break;
        }
    }

    /* Build stable length candidate arrays and report same-start conflicts. */
    for (I = 0; I < Size; ++I) {
        WfSite* Site = &Result->Sites[I];
        unsigned L;
        char     LengthText[32];
        char     Message[192];
        size_t   Used = 0;

        if (Site->StateMask == 0) {
            continue;
        }

        for (L = 1; L <= 4; ++L) {
            if ((Site->LengthMask & (1u << L)) != 0) {
                Site->Lengths[Site->LengthCount++] = (uint8_t) L;
            }
        }

        if (Site->LengthCount > 1) {
            for (L = 0; L < Site->LengthCount; ++L) {
                int Written = snprintf (LengthText + Used,
                                        sizeof (LengthText) - Used,
                                        "%s%u", L ? "/" : "",
                                        Site->Lengths[L]);
                if (Written < 0 || (size_t) Written >=
                    sizeof (LengthText) - Used) {
                    break;
                }
                Used += (size_t) Written;
            }
            snprintf (Message, sizeof (Message),
                      "width ambiguity at $%04X: same start requires %s-byte "
                      "instruction lengths",
                      Site->Address, LengthText);
            if (!AddDiagnostic (Result, WF_DIAG_WIDTH_CONFLICT,
                                Site->Address, Site->Address, Bytes[I],
                                Site->LengthMask, Message)) {
                goto AllocFailure;
            }
        }
    }

    /* Find reachable starts inside another reachable instruction's operands.
    ** Instructions are at most four bytes, so this scan is linear.
    */
    for (I = 0; I < Size; ++I) {
        WfSite* Site = &Result->Sites[I];
        unsigned K, L;

        if (Site->StateMask == 0) {
            continue;
        }

        for (L = 0; L < Site->LengthCount; ++L) {
            unsigned Length = Site->Lengths[L];
            if (I + Length > Size) {
                continue;
            }
            for (K = 1; K < Length; ++K) {
                size_t Victim = I + K;
                if (Result->Sites[Victim].StateMask != 0 &&
                    OverlapOwner[Victim] == SIZE_MAX) {
                    OverlapOwner[Victim] = I;
                }
            }
        }
    }

    for (I = 0; I < Size; ++I) {
        if (OverlapOwner[I] != SIZE_MAX) {
            uint32_t OwnerAddr = StartAddress + (uint32_t) OverlapOwner[I];
            char     Message[192];
            snprintf (Message, sizeof (Message),
                      "instruction start $%04X lies in operand byte(s) of "
                      "instruction at $%04X",
                      StartAddress + (uint32_t) I, OwnerAddr);
            if (!AddDiagnosticF (Result, WF_DIAG_OVERLAP,
                                 StartAddress + (uint32_t) I, OwnerAddr,
                                 Bytes[I], 0, "%s", Message)) {
                goto AllocFailure;
            }
        }
    }

    if (Result->DiagnosticCount != 0) {
        qsort (Result->Diagnostics, Result->DiagnosticCount,
               sizeof (*Result->Diagnostics), CompareDiagnostics);
        Result->Clean = 0;
    }

    free (OverlapOwner);
    free (Queue);
    return Result;

AllocFailure:
    free (OverlapOwner);
    free (Queue);
    WfResultFree (Result);
    return 0;
}



void WfResultFree (WfResult* Result)
/* See header. */
{
    if (Result == 0) {
        return;
    }
    free (Result->Diagnostics);
    free (Result->Sites);
    free (Result);
}
