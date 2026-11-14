/*****************************************************************************/
/*                                                                           */
/*                                 opc65816.h                                */
/*                                                                           */
/*                       65816 opcode description table                      */
/*                                                                           */
/*                                                                           */
/*                                                                           */
/* (C) 2003      Ullrich von Bassewitz                                       */
/*               Roemerstrasse 52                                            */
/*               D-70794 Filderstadt                                         */
/* EMail:        uz@cc65.org                                                 */
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



#ifndef OPC65816_H
#define OPC65816_H



#include "opcdesc.h"



/*****************************************************************************/
/*                                   Data                                    */
/*****************************************************************************/



/* Descriptions for all opcodes */
extern const OpcDesc OpcTable_65816[256];



/* Effect of an instruction on the processor state bits tracked by the
** 65816 state flow analysis.
*/
typedef enum {
    fxNone,                     /* No effect, state is kept */
    fxRep,                      /* REP: clear status bits from immediate */
    fxSep,                      /* SEP: set status bits from immediate */
    fxClc,                      /* CLC: clear carry */
    fxSec,                      /* SEC: set carry */
    fxXce,                      /* XCE: exchange carry and emulation flag */
    fxPlp,                      /* PLP: pull M/X/C from the stack (unknown) */
    fxCarry                     /* Carry is recomputed (ADC/SBC/CMP/shifts) */
} Opc65816StateFx;

/* Control flow classification used by the 65816 state flow analysis */
typedef enum {
    fkNormal,                   /* Falls through to the next instruction */
    fkCond,                     /* Conditional branch, both edges possible */
    fkCondC0,                   /* BCC: branches if carry is clear */
    fkCondC1,                   /* BCS: branches if carry is set */
    fkJump,                     /* BRA/BRL/JMP abs: target only */
    fkStop                      /* Barrier: JSR/JSL/RTx/indirect/BRK/WAI/STP */
} Opc65816Flow;



/*****************************************************************************/
/*                                   Code                                    */
/*****************************************************************************/



unsigned Opc65816InsnSize (unsigned char Opc, unsigned M8, unsigned X8);
/* Return the size of the given instruction in bytes. M8 and X8 are the
** values of the M and X status bits (nonzero = 8 bit) on entry to the
** instruction.
*/

Opc65816StateFx Opc65816StateEffect (unsigned char Opc);
/* Return the effect of the given opcode on the tracked processor state */

Opc65816Flow Opc65816FlowKind (unsigned char Opc);
/* Return the control flow classification of the given opcode */



/* End of opc65816.h */

#endif
