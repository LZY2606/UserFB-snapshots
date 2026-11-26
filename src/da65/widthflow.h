/*****************************************************************************/
/*                                                                           */
/*                                widthflow.h                                */
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



#ifndef WIDTHFLOW_H
#define WIDTHFLOW_H



#include <stddef.h>
#include <stdint.h>

#include "opcflow.h"



/*****************************************************************************/
/*                                   Data                                    */
/*****************************************************************************/



typedef OpcFlowState WfState;
typedef OpcFlowStateMask WfStateMask;

typedef enum WfDiagnosticKind WfDiagnosticKind;
enum WfDiagnosticKind {
    WF_DIAG_NONE = 0,
    WF_DIAG_TRUNCATED,                  /* Candidate instruction runs past end */
    WF_DIAG_WIDTH_CONFLICT,             /* Same start needs two lengths */
    WF_DIAG_OVERLAP                     /* Reachable start is inside operand */
};

typedef struct WfDiagnostic WfDiagnostic;
struct WfDiagnostic {
    WfDiagnosticKind    Kind;
    uint32_t            Address;        /* Diagnostic's primary address */
    uint32_t            OtherAddress;   /* Conflicting/owning address */
    uint8_t             Opcode;
    uint8_t             LengthMask;     /* Bit (1 << length) candidates */
    char                Message[192];   /* Owned text; never points at input */
};

typedef struct WfSite WfSite;
struct WfSite {
    uint32_t            Address;
    WfStateMask         StateMask;      /* States reaching this position */
    uint8_t             LengthMask;     /* Possible instruction lengths */
    uint8_t             LengthCount;
    uint8_t             Lengths[4];
    uint8_t             Truncated;
};

typedef struct WfResult WfResult;
struct WfResult {
    uint32_t            StartAddress;
    size_t              Size;
    WfSite*             Sites;          /* One entry per input byte offset */
    size_t              SiteCount;
    WfDiagnostic*       Diagnostics;
    size_t              DiagnosticCount;
    int                 Clean;          /* True if no diagnostics were found */
};



/*****************************************************************************/
/*                                   Code                                    */
/*****************************************************************************/



int WfParseState (const char* Text, WfState* State);
/* Parse strict "E,M,X,C" zero/one values.  Returns one on success. */

WfResult* WfAnalyze (const uint8_t* Bytes, size_t Size,
                     uint32_t StartAddress, const WfState* InitialState);
/* Analyze the unique entry at the start of Bytes.  The returned result owns
** all of its memory and does not retain Bytes.  Returns NULL for invalid
** arguments or allocation failure.
*/

void WfResultFree (WfResult* Result);
/* Free a result returned by WfAnalyze. */



/* End of widthflow.h */
#endif
