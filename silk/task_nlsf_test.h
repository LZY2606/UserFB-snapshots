/* Copyright (c) 2026 Xiph.Org Foundation */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Read-only observation hooks for the SILK NLSF contract tests.
 *
 * This interface only exists in builds compiled with -DTASK_NLSF_TEST.
 * The callback pointer defaults to NULL, so when no test registers a
 * callback the hooks are inert and normal decoding is unaffected.
 * Normal builds (without TASK_NLSF_TEST) see no new symbols, no ABI
 * change and no extra allocations.
 */
#ifndef TASK_NLSF_TEST_H
#define TASK_NLSF_TEST_H

#include "opus_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Events reported through silk_task_nlsf_hook:
 *
 * TASK_NLSF_EVENT_NLSF:
 *   Fired inside silk_decode_parameters() after the current-frame NLSF
 *   vector has been decoded and the interpolation coefficient has been
 *   clamped for first frames after a reset, but before interpolation
 *   and before prevNLSF_Q15 is updated.  nlsf_Q15 points to the current
 *   frame's (stabilized) NLSF vector; psDec->prevNLSF_Q15 still holds
 *   the previously committed vector.
 *
 * TASK_NLSF_EVENT_LPC:
 *   Fired after interpolation and NLSF2A conversion, after prevNLSF_Q15
 *   has been updated, but before any after-loss bandwidth expansion.
 *   psDecCtrl->PredCoef_Q12[0..1] hold the two LPC sets.
 *
 * TASK_NLSF_EVENT_LPC_FINAL:
 *   Fired after the after-loss bandwidth expansion block (which is a
 *   no-op when lossCnt == 0).  psDecCtrl->PredCoef_Q12[0..1] hold the
 *   final LPC sets used for synthesis.
 *
 * All pointers are only valid for the duration of the callback and must
 * be treated as read-only.
 */
#define TASK_NLSF_EVENT_NLSF      0
#define TASK_NLSF_EVENT_LPC       1
#define TASK_NLSF_EVENT_LPC_FINAL 2

typedef void (*task_nlsf_hook_fn)(
    int              event,     /* I    One of TASK_NLSF_EVENT_*                          */
    const void      *psDec,     /* I    Decoder state (silk_decoder_state), read-only     */
    const void      *psDecCtrl, /* I    Decoder control (silk_decoder_control) or NULL    */
    const opus_int16 *nlsf_Q15  /* I    Current NLSF vector for TASK_NLSF_EVENT_NLSF      */
);

extern task_nlsf_hook_fn silk_task_nlsf_hook;

#ifdef __cplusplus
}
#endif

#endif /* TASK_NLSF_TEST_H */
