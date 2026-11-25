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
 * \file    svc_encode_pcm.cpp
 *
 * \brief   Per-macroblock I_PCM support driven by ENCODER_OPTION_PCM_MB_MAP
 *
 *************************************************************************************
 */

#include <string.h>
#include "svc_encode_pcm.h"
#include "svc_enc_golomb.h"
#include "macros.h"
#include "memory_align.h"

namespace WelsEnc {

// I_PCM payload size for 4:2:0 8-bit: 256 luma + 64 Cb + 64 Cr samples.
#define WELS_PCM_LUMA_SAMPLES   256
#define WELS_PCM_CHROMA_SAMPLES 64
#define WELS_PCM_MB_BYTES       (WELS_PCM_LUMA_SAMPLES + 2 * WELS_PCM_CHROMA_SAMPLES) // 384

bool WelsPcmMbSelected (const sWelsEncCtx* pEncCtx, int32_t iMbX, int32_t iMbY) {
  if (NULL == pEncCtx || NULL == pEncCtx->pPcmMbMap)
    return false;
  if (iMbX < 0 || iMbY < 0)
    return false;
  return pEncCtx->pPcmMbMap[iMbY * pEncCtx->iPcmMbMapStride + iMbX] != 0;
}

void WelsPcmMbPrepare (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb) {
  SDqLayer* pCurLayer = pEncCtx->pCurDqLayer;
  SMbCache* pMbCache  = &pSlice->sMbCacheInfo;

  // Copy the original source MB straight into the reconstruction/reference
  // picture. pCsMb aliases pDecMb for the single-layer configuration, and the
  // deblocking filter is disabled by the supported configuration, so this is
  // the final reconstructed sample value used by neighbors and later frames.
  const int32_t kiEncStrideY = pCurLayer->iEncStride[0];
  const int32_t kiEncStrideC = pCurLayer->iEncStride[1];
  const int32_t kiDecStrideY = pCurLayer->pDecPic->iLineSize[0];
  const int32_t kiDecStrideC = pCurLayer->pDecPic->iLineSize[1];

  const uint8_t* pSrcY = pMbCache->SPicData.pEncMb[0];
  const uint8_t* pSrcU = pMbCache->SPicData.pEncMb[1];
  const uint8_t* pSrcV = pMbCache->SPicData.pEncMb[2];
  uint8_t* pDstY = pMbCache->SPicData.pDecMb[0];
  uint8_t* pDstU = pMbCache->SPicData.pDecMb[1];
  uint8_t* pDstV = pMbCache->SPicData.pDecMb[2];

  int32_t iRow;
  for (iRow = 0; iRow < 16; ++iRow) {
    memcpy (pDstY, pSrcY, 16);
    pSrcY += kiEncStrideY;
    pDstY += kiDecStrideY;
  }
  for (iRow = 0; iRow < 8; ++iRow) {
    memcpy (pDstU, pSrcU, 8);
    memcpy (pDstV, pSrcV, 8);
    pSrcU += kiEncStrideC;
    pSrcV += kiEncStrideC;
    pDstU += kiDecStrideC;
    pDstV += kiDecStrideC;
  }

  // MB state that later neighbor/cache/context derivations inspect. All
  // non-zero-count entries are 16 (Rec. H.264 9.2.1 for I_PCM) and cbp is
  // all ones: decoder context derivation treats an I_PCM neighbor as having
  // every block/bit coded, which an all-ones uiCbp reproduces on the encoder.
  pCurMb->uiMbType       = MB_TYPE_INTRA_PCM;
  pCurMb->uiCbp          = 0x2F; // luma 15 + chroma cbp 2
  // Decoder CABAC cbf context derivation treats an I_PCM neighbor as having all
  // DC coded-block-flags present; advertise luma/Cb/Cr DC bits so encoder
  // neighbor contexts match (ParseCbfInfoCabac PCM special case).
  pCurMb->iCbpDc         = 0x07;
  pCurMb->uiChromPredMode = 0;
  pCurMb->iLumaDQp       = 0;
  pCurMb->uiLumaQp       = pSlice->uiLastMbQp;
  pCurMb->uiChromaQp     = g_kuiChromaQpTable[CLIP3_QP_0_51 (
                            pCurMb->uiLumaQp + pCurLayer->sLayerInfo.pPpsP->uiChromaQpIndexOffset)];

  memset (pCurMb->pNonZeroCount, 16, MB_LUMA_CHROMA_BLOCK4x4_NUM);

  SMVUnitXY sZeroMv;
  sZeroMv.iMvX = 0;
  sZeroMv.iMvY = 0;
  int32_t i;
  for (i = 0; i < MB_BLOCK4x4_NUM; ++i) {
    pCurMb->sMvd[i].sAssignMv (sZeroMv);
    pCurMb->sMv[i].sAssignMv (sZeroMv);
  }
  if (pCurMb->pRefIndex != NULL) {
    for (i = 0; i < 4; ++i)
      pCurMb->pRefIndex[i] = 0;
  }
}

// Write pcm_alignment_zero_bit (zero bits, unlike BsAlign's one padding) and
// flush so that pBs->pCurBuf addresses the first byte of PCM sample data.
static void WelsPcmWriteZeroAlign (SBitStringAux* pBs) {
  const int32_t kiUsedBits = (32 - pBs->iLeftBits) & 7;
  const int32_t kiPadBits  = (8 - kiUsedBits) & 7;
  if (kiPadBits > 0)
    BsWriteBits (pBs, kiPadBits, 0);
  BsFlush (pBs);
}

// Copy the 384 raw samples into the byte-aligned bitstream position.
int32_t WelsWritePcmMbSynCavlc (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb) {
  SBitStringAux* pBs    = pSlice->pSliceBsa;
  SMbCache* pMbCache    = &pSlice->sMbCacheInfo;
  SDqLayer* pCurLayer   = pEncCtx->pCurDqLayer;

  // I slice mb_type I_PCM = 25, P slice mb_type I_PCM = 30 (5 + 25).
  const uint32_t kuiMbTypeCode = (pEncCtx->eSliceType == I_SLICE) ? 25u : 30u;
  BsWriteUE (pBs, kuiMbTypeCode);

  WelsPcmWriteZeroAlign (pBs);

  if (pBs->pEndBuf - pBs->pCurBuf < WELS_PCM_MB_BYTES)
    return ENC_RETURN_VLCOVERFLOWFOUND;

  uint8_t* pDst = pBs->pCurBuf;
  const uint8_t* pSrcY = pMbCache->SPicData.pEncMb[0];
  const uint8_t* pSrcU = pMbCache->SPicData.pEncMb[1];
  const uint8_t* pSrcV = pMbCache->SPicData.pEncMb[2];
  const int32_t kiStrideY = pCurLayer->iEncStride[0];
  const int32_t kiStrideC = pCurLayer->iEncStride[1];

  int32_t iRow;
  for (iRow = 0; iRow < 16; ++iRow) {
    memcpy (pDst, pSrcY, 16);
    pDst  += 16;
    pSrcY += kiStrideY;
  }
  for (iRow = 0; iRow < 8; ++iRow) {
    memcpy (pDst, pSrcU, 8);
    pDst  += 8;
    pSrcU += kiStrideC;
  }
  for (iRow = 0; iRow < 8; ++iRow) {
    memcpy (pDst, pSrcV, 8);
    pDst  += 8;
    pSrcV += kiStrideC;
  }

  pBs->pCurBuf  += WELS_PCM_MB_BYTES;
  pBs->uiCurBits = 0;
  pBs->iLeftBits = 32;

  (void)pCurMb;
  return ENC_RETURN_SUCCESS;
}

// Flush the I_PCM mb_type terminate bin into bytes, mirroring the tail of
// WelsCabacEncodeFlush() without emitting the final slice terminate again.
static void WelsCabacPcmDrain (SCabacCtx* pCbCtx) {
  cabac_low_t uiLow       = pCbCtx->m_uiLow;
  int32_t iLowBitCnt      = pCbCtx->m_iLowBitCnt;
  uint8_t* pBufCur        = pCbCtx->m_pBufCur;

  uiLow <<= CABAC_LOW_WIDTH - 1 - iLowBitCnt;
  if (uiLow & (cabac_low_t (1) << (CABAC_LOW_WIDTH - 1))) {
    uint8_t* pCarry = pBufCur;
    for (; pCarry > pCbCtx->m_pBufStart; --pCarry) {
      if (++ (* (pCarry - 1)))
        break;
    }
  }
  for (; (iLowBitCnt -= 8) >= 0; uiLow <<= 8)
    *pBufCur++ = (uint8_t) (uiLow >> (CABAC_LOW_WIDTH - 9));

  pCbCtx->m_pBufCur = pBufCur;
}

int32_t WelsWritePcmMbSynCabac (sWelsEncCtx* pEncCtx, SSlice* pSlice, SMB* pCurMb) {
  SCabacCtx* pCabacCtx = &pSlice->sCabacCtx;
  SBitStringAux* pBs   = pSlice->pSliceBsa;
  SMbCache* pMbCache   = &pSlice->sMbCacheInfo;
  const int32_t kiMbWidth = pEncCtx->pCurDqLayer->iMbWidth;

  // mb_type I_PCM coding, mirroring WelsCabacMbType but using terminate bin 1
  // as the I_PCM marker (decoder ParseMBType*SliceCabac).
  if (pEncCtx->eSliceType == I_SLICE) {
    int32_t iCtx = 3;
    if ((pCurMb->uiNeighborAvail & LEFT_MB_POS) && !IS_INTRA4x4 ((pCurMb - 1)->uiMbType))
      ++iCtx;
    if ((pCurMb->uiNeighborAvail & TOP_MB_POS) && !IS_INTRA4x4 ((pCurMb - kiMbWidth)->uiMbType))
      ++iCtx;
    WelsCabacEncodeDecision (pCabacCtx, iCtx, 1);
  } else {
    // P slice: intra prefix (ctx 14 -> 1, ctx 17 -> 1), then terminate marker.
    WelsCabacEncodeDecision (pCabacCtx, 14, 1);
    WelsCabacEncodeDecision (pCabacCtx, 17, 1);
  }
  WelsCabacEncodeTerminate (pCabacCtx, 1);

  // Push the terminate bytes out and locate the byte aligned PCM payload.
  WelsCabacPcmDrain (pCabacCtx);

  if (pCabacCtx->m_pBufEnd - pCabacCtx->m_pBufCur < WELS_PCM_MB_BYTES)
    return ENC_RETURN_VLCOVERFLOWFOUND;

  SDqLayer* pCurLayer = pEncCtx->pCurDqLayer;
  uint8_t* pDst       = pCabacCtx->m_pBufCur;
  const uint8_t* pSrcY = pMbCache->SPicData.pEncMb[0];
  const uint8_t* pSrcU = pMbCache->SPicData.pEncMb[1];
  const uint8_t* pSrcV = pMbCache->SPicData.pEncMb[2];
  const int32_t kiStrideY = pCurLayer->iEncStride[0];
  const int32_t kiStrideC = pCurLayer->iEncStride[1];

  int32_t iRow;
  for (iRow = 0; iRow < 16; ++iRow) {
    memcpy (pDst, pSrcY, 16);
    pDst  += 16;
    pSrcY += kiStrideY;
  }
  for (iRow = 0; iRow < 8; ++iRow) {
    memcpy (pDst, pSrcU, 8);
    pDst  += 8;
    pSrcU += kiStrideC;
  }
  for (iRow = 0; iRow < 8; ++iRow) {
    memcpy (pDst, pSrcV, 8);
    pDst  += 8;
    pSrcV += kiStrideC;
  }

  // Restart the arithmetic engine right after the PCM payload, keeping the
  // probability contexts (the decoder only re-initializes the engine here).
  WelsCabacEncodeInit (pCabacCtx, pDst, pCabacCtx->m_pBufEnd);

  // Keep the golomb writer position consistent for position queries. Its
  // pStartBuf/pEndBuf must stay untouched: the CABAC engine starts after the
  // byte-aligned slice header while the golomb writer covers the whole NAL.
  pBs->pCurBuf   = pDst;
  pBs->uiCurBits = 0;
  pBs->iLeftBits = 32;

  return ENC_RETURN_SUCCESS;
}

int32_t WelsPcmEnsureOutputBuffers (sWelsEncCtx* pEncCtx) {
  if (NULL == pEncCtx || NULL == pEncCtx->pMemAlign || NULL == pEncCtx->pSvcParam)
    return ENC_RETURN_UNEXPECTED;

  const int32_t kiWidth  = pEncCtx->pSvcParam->iPicWidth;
  const int32_t kiHeight = pEncCtx->pSvcParam->iPicHeight;
  if (kiWidth <= 0 || kiHeight <= 0)
    return ENC_RETURN_UNEXPECTED;

  // Worst-case all-PCM slice RBSP ~ (384 + mb_type/alignment) bytes/MB ~=
  // 1.51*W*H, and WelsEncodeNal's conservative emulation-prevention bound is
  // 1.5x that (~2.26*W*H). Provide 2.5*W*H plus fixed slack.
  const int32_t kiNeed = (int32_t) ((int64_t) kiWidth * kiHeight * 5 / 2 + 8192);
  CMemoryAlign* pMa = pEncCtx->pMemAlign;

  if (pEncCtx->iFrameBsSize < kiNeed) {
    uint8_t* pNew = static_cast<uint8_t*> (pMa->WelsMallocz (kiNeed, "pFrameBs"));
    if (NULL == pNew)
      return ENC_RETURN_MEMALLOCERR;
    if (NULL != pEncCtx->pFrameBs)
      pMa->WelsFree (pEncCtx->pFrameBs, "pFrameBs");
    pEncCtx->pFrameBs   = pNew;
    pEncCtx->iFrameBsSize = kiNeed;
  }

  if (NULL != pEncCtx->pOut && static_cast<int32_t> (pEncCtx->pOut->uiSize) < kiNeed) {
    uint8_t* pNew = static_cast<uint8_t*> (pMa->WelsMallocz (kiNeed, "pOut->pBsBuffer"));
    if (NULL == pNew)
      return ENC_RETURN_MEMALLOCERR;
    if (NULL != pEncCtx->pOut->pBsBuffer)
      pMa->WelsFree (pEncCtx->pOut->pBsBuffer, "pOut->pBsBuffer");
    pEncCtx->pOut->pBsBuffer = pNew;
    pEncCtx->pOut->uiSize    = static_cast<uint32_t> (kiNeed);
    InitBits (&pEncCtx->pOut->sBsWrite, pNew, kiNeed);
  }

  return ENC_RETURN_SUCCESS;
}

} // namespace WelsEnc
