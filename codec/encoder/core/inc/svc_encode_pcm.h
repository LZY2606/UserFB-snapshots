/*!
 * \copy
 *     Copyright (c) 2024, Cisco Systems
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or without
 *     modification, are permitted provided that the following conditions
 *     are met:
 *
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *
 *        * Redistributions in binary form must reproduce the above copyright
 *          notice, this list of conditions and the following disclaimer in
 *          the documentation and/or other materials provided with the
 *          distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *     BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *     CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *     LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *     ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * \file    svc_encode_pcm.h
 *
 * \brief   Per-macroblock I_PCM support driven by ENCODER_OPTION_PCM_MB_MAP
 *
 *************************************************************************************
 */

#ifndef SVC_ENCODE_PCM_H_
#define SVC_ENCODE_PCM_H_

#include "svc_set_mb_syn.h"

namespace WelsEnc {

// Non-zero when the active PCM map forces the MB at raster coordinate
// (iMbX, iMbY) to be coded as I_PCM. Always false when no map is active.
bool WelsPcmMbSelected (const sWelsEncCtx* pEncCtx, int32_t iMbX, int32_t iMbY);

// Initialize all MB/cache state needed for a forced I_PCM MB and copy the
// original source samples into the reconstruction picture, so that encoder
// intra/inter neighbors and future reference pictures see exact pixels.
// Must be called after WelsMdIntraInit() located the MB planes.
void WelsPcmMbPrepare (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb);

// Dense I_PCM maps need more bitstream/NAL destination headroom than the
// lossy-size based allocation estimate. Grow the frame/output buffers to a
// worst-case all-PCM size. Returns ENC_RETURN_SUCCESS or a malloc error.
int32_t WelsPcmEnsureOutputBuffers (sWelsEncCtx* pEncCtx);

// Write the I_PCM mb_type (25 in I slices / 30 in P slices), zero bit
// alignment and the 384 raw samples for CAVLC and CABAC respectively.
// The common skip-run / mb_skip_flag prefix has already been coded by the
// caller. CABAC variant terminates and restarts the arithmetic engine as
// required for I_PCM.
int32_t WelsWritePcmMbSynCavlc (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb);
int32_t WelsWritePcmMbSynCabac (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb);

} // namespace WelsEnc

#endif // SVC_ENCODE_PCM_H_
