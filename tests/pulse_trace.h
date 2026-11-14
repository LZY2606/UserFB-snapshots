/*
 ** Copyright 2003-2010, VisualOn, Inc.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

/*--------------------------------------------------------------------------*
 *                         PULSE_TRACE.H                                    *
 *--------------------------------------------------------------------------*
 * Read-only algebraic-codebook observation hook used only by the contract *
 * test (tests/pulse_contract.c). The hook is compiled solely when          *
 * PULSE_TRACE is defined (see support/build.sh target builds). A normal    *
 * library build never defines it, so there is no callback, no extra        *
 * allocation and the emitted bit stream is byte-for-byte unchanged.        *
 *                                                                          *
 * The observation is taken in ACELP_4t64_fx() after the raw fixed-codebook *
 * excitation (code[]) and the per-track indices (_index[]) have both been  *
 * built and before the coder applies pre-emphasis / pitch sharpening.      *
 *--------------------------------------------------------------------------*/

#ifndef __PULSE_TRACE_H__
#define __PULSE_TRACE_H__

#include "typedef.h"

#define PULSE_NB_SAMPLES 64              /* samples per subframe            */
#define PULSE_NB_TRACKS  4               /* interleaved tracks              */

typedef struct
{
	Word16 nbbits;                       /* 20, 36, 44 or 52                 */
	Word16 pulse_amp;                    /* unit pulse amplitude in code[]   */
	Word16 index[PULSE_NB_TRACKS];       /* raw per-track codebook indices   */
	Word16 code[PULSE_NB_SAMPLES];       /* raw algebraic excitation (Q9)    */
} pulse_obs_t;

/* Installed by the test. NULL (the default) means nothing is observed. */
extern void (*pulse_trace_cb)(const pulse_obs_t * obs);

#endif /*__PULSE_TRACE_H__*/
