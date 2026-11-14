/*****************************************************************************/
/*                                                                           */
/*                                widthflow.h                                */
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



#ifndef WIDTHFLOW_H
#define WIDTHFLOW_H



#include <stdint.h>



/*****************************************************************************/
/*                                   Data                                    */
/*****************************************************************************/



/* State bits tracked by the analysis. A state is a combination of these
** bits; in emulation mode (WF_E set) WF_M and WF_X are always set, too.
*/
enum {
    WF_E = 0x01,                /* Emulation mode */
    WF_M = 0x02,                /* M flag: 8 bit memory/accumulator */
    WF_X = 0x04,                /* X flag: 8 bit index registers */
    WF_C = 0x08                 /* Carry flag */
};

/* A diagnostic message with the address it refers to. The message text is
** owned by the result and never borrows the analyzed input.
*/
typedef struct WfDiag WfDiag;
struct WfDiag {
    uint32_t    Addr;           /* Address the diagnostic refers to */
    char*       Text;           /* Owned message text */
};

/* Result of the state flow analysis. All arrays are indexed by the offset
** from the start address and have Len entries.
*/
typedef struct WfResult WfResult;
struct WfResult {
    uint32_t    StartAddr;      /* Start address of the analyzed code */
    uint32_t    Len;            /* Number of bytes analyzed */
    uint16_t*   StateSets;      /* Reachable states per offset (bitmask) */
    uint8_t*    LenCandidates;  /* Possible insn lengths per offset (bitmask) */
    WfDiag*     Diags;          /* Diagnostics (ambiguities, truncation) */
    unsigned    DiagCount;      /* Number of diagnostics */
    int         Ok;             /* True if the analysis found no problems */
};



/*****************************************************************************/
/*                                   Code                                    */
/*****************************************************************************/



int WfAnalyze (const uint8_t* Code, uint32_t Len, uint32_t StartAddr,
               unsigned E, unsigned M, unsigned X, unsigned C,
               WfResult* Res);
/* Run the 65816 state flow analysis over Code, which holds Len bytes loaded
** at StartAddr. E, M, X and C are the processor state bits at the start
** address, which is the only entry point. The result is placed into Res and
** must be released with WfDone. Returns true if the code could be decoded
** without ambiguities; in this case every reachable offset has exactly one
** instruction length candidate. Otherwise Res->Diags describes the problems
** (ambiguities or truncated instructions) together with their addresses.
*/

unsigned WfStateSetAt (const WfResult* Res, uint32_t Addr);
/* Return the set of states (bitmask of WF_ bit combinations) that reach the
** given address, or zero if the address is not reachable code.
*/

unsigned WfInsnLenAt (const WfResult* Res, uint32_t Addr);
/* Return the instruction length at the given address, or zero if the
** address is not the start of a reachable instruction. The result is only
** meaningful if the analysis reported no problems.
*/

void WfDone (WfResult* Res);
/* Release all memory held by an analysis result */



/* End of widthflow.h */
#endif
