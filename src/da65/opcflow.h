/*****************************************************************************/
/*                                                                           */
/*                                 opcflow.h                                 */
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



#ifndef OPCFLOW_H
#define OPCFLOW_H



#include <stddef.h>
#include <stdint.h>



/*****************************************************************************/
/*                                   Data                                    */
/*****************************************************************************/



/* Complete processor state relevant to width/flow analysis. */
typedef struct OpcFlowState OpcFlowState;
struct OpcFlowState {
    unsigned char E;                    /* Emulation flag */
    unsigned char M;                    /* Memory/accumulator width flag */
    unsigned char X;                    /* Index width flag */
    unsigned char C;                    /* Carry flag */
};

/* There are only four binary state bits, so a uint16_t can hold a set. */
#define OPCFLOW_STATE_COUNT     16u
typedef uint16_t OpcFlowStateMask;

#define OPCFLOW_STATE_INDEX(E, M, X, C) \
    ((((unsigned)(E)) << 3) | (((unsigned)(M)) << 2) | \
     (((unsigned)(X)) << 1) |  ((unsigned)(C)))

enum {
    OPCFLOW_FLOW_FALLTHROUGH = 0,       /* Ordinary next instruction */
    OPCFLOW_FLOW_CONDITIONAL,           /* Conditional branch */
    OPCFLOW_FLOW_UNCONDITIONAL,         /* BRA/BRL/direct JMP/JML */
    OPCFLOW_FLOW_CALL,                  /* Direct JSR/JSL target known */
    OPCFLOW_FLOW_BARRIER                /* No state may fall through/return */
};

enum {
    OPCFLOW_COND_NONE = 0,
    OPCFLOW_COND_BPL,
    OPCFLOW_COND_BMI,
    OPCFLOW_COND_BVC,
    OPCFLOW_COND_BVS,
    OPCFLOW_COND_BCC,
    OPCFLOW_COND_BCS,
    OPCFLOW_COND_BNE,
    OPCFLOW_COND_BEQ
};

enum {
    OPCFLOW_TARGET_NONE = 0,
    OPCFLOW_TARGET_REL8,
    OPCFLOW_TARGET_REL16,
    OPCFLOW_TARGET_ABS16,
    OPCFLOW_TARGET_ABS24,
    OPCFLOW_TARGET_INDIRECT
};

typedef struct Opc65816Flow Opc65816Flow;
struct Opc65816Flow {
    unsigned char       Kind;           /* OPCFLOW_FLOW_xxx */
    unsigned char       Condition;      /* OPCFLOW_COND_xxx */
    unsigned char       Target;         /* OPCFLOW_TARGET_xxx */
    unsigned char       SetsCarry;      /* Instruction gives C two values */
};



/*****************************************************************************/
/*                                   Code                                    */
/*****************************************************************************/



int OpcFlowStateValid (const OpcFlowState* State);
/* Return true if the state is a legal complete 65816 state. */

unsigned OpcFlowStateIndex (const OpcFlowState* State);
/* Map a valid state to its dense bit index. */

void OpcFlowStateFromIndex (unsigned Index, OpcFlowState* State);
/* Map a dense bit index back to E,M,X,C values. */

unsigned Opc65816LengthForWidth (uint8_t Opcode,
                                 unsigned Mem16, unsigned Index16);
/* Return the complete instruction length.  Mem16/Index16 describe the state
** on entry to the instruction, not the state after it executes.
*/

unsigned Opc65816Length (uint8_t Opcode, const OpcFlowState* State);
/* State-wrapper around Opc65816LengthForWidth. */

void Opc65816Describe (uint8_t Opcode, Opc65816Flow* Info);
/* Fill the control/state-flow description for one opcode. */

OpcFlowStateMask Opc65816NextStates (uint8_t Opcode, uint8_t Immediate,
                                     unsigned StateIndex);
/* Return the set of possible states after executing Opcode from StateIndex.
** Immediate is used by REP/SEP and must be supplied by callers only after
** verifying that the complete instruction is available.
*/

uint32_t Opc65816Target (uint32_t Addr, uint8_t Opcode, unsigned Length,
                         const uint8_t* Bytes, size_t Available);
/* Calculate a direct/relative target.  Bytes points at the opcode and
** Available must include the complete instruction.  Returns UINT32_MAX if
** the target kind is indirect/none or the instruction is truncated.
*/



/* End of opcflow.h */
#endif
