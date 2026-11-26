/*****************************************************************************/
/*                                                                           */
/*                                 opcflow.c                                 */
/*                                                                           */
/*              65816 opcode length/state/control flow descriptions           */
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



#include <string.h>

/* da65 */
#include "handler.h"
#include "opc65816.h"
#include "opcflow.h"



/*****************************************************************************/
/*                                   Code                                    */
/*****************************************************************************/



int OpcFlowStateValid (const OpcFlowState* State)
/* See header. */
{
    if (State == 0) {
        return 0;
    }
    if (State->E > 1 || State->M > 1 || State->X > 1 || State->C > 1) {
        return 0;
    }
    /* In emulation mode the index and accumulator sizes are always 8 bit. */
    return !(State->E && (!State->M || !State->X));
}



unsigned OpcFlowStateIndex (const OpcFlowState* State)
/* See header. */
{
    return OPCFLOW_STATE_INDEX (State->E, State->M, State->X, State->C);
}



void OpcFlowStateFromIndex (unsigned Index, OpcFlowState* State)
/* See header. */
{
    State->E = (Index >> 3) & 1;
    State->M = (Index >> 2) & 1;
    State->X = (Index >> 1) & 1;
    State->C = Index & 1;
}



unsigned Opc65816LengthForWidth (uint8_t Opcode,
                                 unsigned Mem16, unsigned Index16)
/* See header.  The existing opcode table supplies the base size and says
** which opcodes have a state-dependent immediate.  Only the one-byte width
** adjustment is added here.
*/
{
    const OpcDesc* D = &OpcTable_65816[Opcode];
    unsigned       Length = D->Size;

    if ((D->Flags & flSizeChanges) != 0) {
        if (D->Handler == OH_Immediate65816M) {
            if (Mem16) {
                ++Length;
            }
        } else if (D->Handler == OH_Immediate65816X) {
            if (Index16) {
                ++Length;
            }
        }
    }

    return Length;
}



unsigned Opc65816Length (uint8_t Opcode, const OpcFlowState* State)
/* See header.  M/X=0 means the corresponding register is 16 bit. */
{
    return Opc65816LengthForWidth (Opcode, State->M == 0, State->X == 0);
}



static int MnemonicIsCarryGenerating (const char* Mnemo)
/* Return true for instruction families whose result may set or clear C.
** The actual value is data dependent, so flow analysis must retain both.
*/
{
    static const char* const Names[] = {
        "adc", "sbc", "cmp", "cpx", "cpy",
        "asl", "lsr", "rol", "ror"
    };
    unsigned I;

    for (I = 0; I < sizeof (Names) / sizeof (Names[0]); ++I) {
        if (strcmp (Mnemo, Names[I]) == 0) {
            return 1;
        }
    }
    return 0;
}



void Opc65816Describe (uint8_t Opcode, Opc65816Flow* Info)
/* See header. */
{
    const OpcDesc* D = &OpcTable_65816[Opcode];

    memset (Info, 0, sizeof (*Info));
    Info->Kind = OPCFLOW_FLOW_FALLTHROUGH;
    Info->SetsCarry = MnemonicIsCarryGenerating (D->Mnemo);

    switch (Opcode) {

        /* Conditional relative branches. */
        case 0x10: Info->Condition = OPCFLOW_COND_BPL; goto CondRel8;
        case 0x30: Info->Condition = OPCFLOW_COND_BMI; goto CondRel8;
        case 0x50: Info->Condition = OPCFLOW_COND_BVC; goto CondRel8;
        case 0x70: Info->Condition = OPCFLOW_COND_BVS; goto CondRel8;
        case 0x90: Info->Condition = OPCFLOW_COND_BCC; goto CondRel8;
        case 0xB0: Info->Condition = OPCFLOW_COND_BCS; goto CondRel8;
        case 0xD0: Info->Condition = OPCFLOW_COND_BNE; goto CondRel8;
        case 0xF0: Info->Condition = OPCFLOW_COND_BEQ;
        CondRel8:
            Info->Kind   = OPCFLOW_FLOW_CONDITIONAL;
            Info->Target = OPCFLOW_TARGET_REL8;
            return;

        /* Unconditional branches and direct jumps. */
        case 0x80:                              /* BRA */
            Info->Kind   = OPCFLOW_FLOW_UNCONDITIONAL;
            Info->Target = OPCFLOW_TARGET_REL8;
            return;

        case 0x82:                              /* BRL */
            Info->Kind   = OPCFLOW_FLOW_UNCONDITIONAL;
            Info->Target = OPCFLOW_TARGET_REL16;
            return;

        case 0x4C:                              /* JMP abs */
            Info->Kind   = OPCFLOW_FLOW_UNCONDITIONAL;
            Info->Target = OPCFLOW_TARGET_ABS16;
            return;

        case 0x5C:                              /* JML long direct */
            Info->Kind   = OPCFLOW_FLOW_UNCONDITIONAL;
            Info->Target = OPCFLOW_TARGET_ABS24;
            return;

        /* Direct calls have a known entry target, but no return-state edge. */
        case 0x20:                              /* JSR abs */
            Info->Kind   = OPCFLOW_FLOW_CALL;
            Info->Target = OPCFLOW_TARGET_ABS16;
            return;

        case 0x22:                              /* JSL long */
            Info->Kind   = OPCFLOW_FLOW_CALL;
            Info->Target = OPCFLOW_TARGET_ABS24;
            return;

        /* Indirect transfers have no statically known target. */
        case 0x6C:                              /* JMP (abs) */
        case 0x7C:                              /* JMP (abs,X) */
        case 0xDC:                              /* JML [abs] */
        case 0xFC:                              /* JSR (abs,X) */
            Info->Kind   = OPCFLOW_FLOW_BARRIER;
            Info->Target = OPCFLOW_TARGET_INDIRECT;
            return;

        /* Returns and exception/interrupt exits. */
        case 0x40:                              /* RTI */
        case 0x60:                              /* RTS */
        case 0x6B:                              /* RTL */
            Info->Kind = OPCFLOW_FLOW_BARRIER;
            return;

        /* Exception instructions and CPU-synchronizing stops. */
        case 0x00:                              /* BRK */
        case 0x02:                              /* COP */
        case 0xCB:                              /* WAI */
        case 0xDB:                              /* STP */
            Info->Kind = OPCFLOW_FLOW_BARRIER;
            return;

        default:
            return;
    }
}



static OpcFlowStateMask OneStateMask (const OpcFlowState* State)
{
    return (OpcFlowStateMask) (1u << OpcFlowStateIndex (State));
}



OpcFlowStateMask Opc65816NextStates (uint8_t Opcode, uint8_t Immediate,
                                     unsigned StateIndex)
/* See header. */
{
    OpcFlowState     State;
    OpcFlowState     Other;
    OpcFlowStateMask Result = 0;

    OpcFlowStateFromIndex (StateIndex, &State);

    switch (Opcode) {

        case 0xC2:                              /* REP */
            if ((Immediate & 0x01) != 0) {
                State.C = 0;
            }
            /* In emulation mode M/X are forced one and REP cannot clear. */
            if (State.E == 0) {
                if ((Immediate & 0x10) != 0) {
                    State.X = 0;
                }
                if ((Immediate & 0x20) != 0) {
                    State.M = 0;
                }
            }
            return OneStateMask (&State);

        case 0xE2:                              /* SEP */
            if ((Immediate & 0x01) != 0) {
                State.C = 1;
            }
            if ((Immediate & 0x10) != 0) {
                State.X = 1;
            }
            if ((Immediate & 0x20) != 0) {
                State.M = 1;
            }
            return OneStateMask (&State);

        case 0x18:                              /* CLC */
            State.C = 0;
            return OneStateMask (&State);

        case 0x38:                              /* SEC */
            State.C = 1;
            return OneStateMask (&State);

        case 0xFB:                              /* XCE */
        {
            unsigned char OldE = State.E;
            unsigned char OldC = State.C;
            State.E = OldC;
            State.C = OldE;
            if (State.E != 0) {
                State.M = 1;
                State.X = 1;
            }
            return OneStateMask (&State);
        }

        case 0x28:                              /* PLP */
        {
            unsigned I;
            /* PLP makes M/X/C unknown but leaves E alone.  Enumerate all
            ** legal combinations instead of carrying the old bits forward.
            */
            for (I = 0; I < OPCFLOW_STATE_COUNT; ++I) {
                OpcFlowState Candidate;
                OpcFlowStateFromIndex (I, &Candidate);
                if (Candidate.E == State.E && OpcFlowStateValid (&Candidate)) {
                    Result |= (OpcFlowStateMask) (1u << I);
                }
            }
            return Result;
        }

        default:
            break;
    }

    Opc65816Flow FlowInfo;
    Opc65816Describe (Opcode, &FlowInfo);
    if (FlowInfo.SetsCarry) {
        Other = State;
        State.C = 0;
        Other.C = 1;
        return OneStateMask (&State) | OneStateMask (&Other);
    }

    return OneStateMask (&State);
}



uint32_t Opc65816Target (uint32_t Addr, uint8_t Opcode, unsigned Length,
                         const uint8_t* Bytes, size_t Available)
/* See header. */
{
    Opc65816Flow Info;
    int16_t      SignedWord;
    uint16_t     Word;

    Opc65816Describe (Opcode, &Info);
    if (Bytes == 0 || Available < Length || Info.Target == OPCFLOW_TARGET_NONE ||
        Info.Target == OPCFLOW_TARGET_INDIRECT) {
        return UINT32_MAX;
    }

    switch (Info.Target) {

        case OPCFLOW_TARGET_REL8:
            if (Length < 2) {
                return UINT32_MAX;
            }
            return (Addr + Length + (int8_t) Bytes[1]) & 0xFFFFu;

        case OPCFLOW_TARGET_REL16:
            if (Length < 3) {
                return UINT32_MAX;
            }
            Word = (uint16_t) (Bytes[1] | (Bytes[2] << 8));
            SignedWord = (int16_t) Word;
            return (Addr + Length + SignedWord) & 0xFFFFu;

        case OPCFLOW_TARGET_ABS16:
            if (Length < 3) {
                return UINT32_MAX;
            }
            return (uint32_t) Bytes[1] | ((uint32_t) Bytes[2] << 8);

        case OPCFLOW_TARGET_ABS24:
            if (Length < 4) {
                return UINT32_MAX;
            }
            return (uint32_t) Bytes[1] |
                   ((uint32_t) Bytes[2] << 8) |
                   ((uint32_t) Bytes[3] << 16);

        default:
            return UINT32_MAX;
    }
}
