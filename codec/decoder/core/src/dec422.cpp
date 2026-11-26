#include "dec422.h"

#include "wels_common_defs.h"
#include "wels_common_basis.h"

#include <string.h>

namespace WelsDec {

static inline uint8_t ClipPixel422 (int32_t x) {
  return x < 0 ? 0 : (x > 255 ? 255 : static_cast<uint8_t> (x));
}

static void FillBlock4x4 (uint8_t* pPred, int32_t iStride, uint8_t uiValue) {
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 4; ++x)
      pPred[y * iStride + x] = uiValue;
  }
}

static void FillChromaBlocks (uint8_t* pPred, int32_t iStride, const uint8_t uiValue[8]) {
  for (int32_t y = 0; y < 4; ++y) {
    for (int32_t x = 0; x < 2; ++x)
      FillBlock4x4 (pPred + (y << 2) * iStride + (x << 2), iStride, uiValue[(y << 1) + x]);
  }
}

void WelsIChromaPred422 (uint8_t* pPred, int32_t iStride, int8_t iMode) {
  const uint8_t* pTop = pPred - iStride;
  const uint8_t* pLeft = pPred - 1;

  if (iMode == C_PRED_V) {
    for (int32_t y = 0; y < 16; ++y) {
      for (int32_t x = 0; x < 8; ++x)
        pPred[y * iStride + x] = pTop[x];
    }
    return;
  }

  if (iMode == C_PRED_H) {
    for (int32_t y = 0; y < 16; ++y) {
      const uint8_t uiValue = pLeft[y * iStride];
      for (int32_t x = 0; x < 8; ++x)
        pPred[y * iStride + x] = uiValue;
    }
    return;
  }

  if (iMode == C_PRED_DC_128) {
    uint8_t uiDc[8];
    for (int32_t i = 0; i < 8; ++i)
      uiDc[i] = 128;
    FillChromaBlocks (pPred, iStride, uiDc);
    return;
  }

  if (iMode == C_PRED_DC_T) {
    const int32_t iDc0 = (pTop[0] + pTop[1] + pTop[2] + pTop[3] + 2) >> 2;
    const int32_t iDc1 = (pTop[4] + pTop[5] + pTop[6] + pTop[7] + 2) >> 2;
    uint8_t uiDc[8];
    for (int32_t y = 0; y < 4; ++y) {
      uiDc[2 * y] = static_cast<uint8_t> (iDc0);
      uiDc[2 * y + 1] = static_cast<uint8_t> (iDc1);
    }
    FillChromaBlocks (pPred, iStride, uiDc);
    return;
  }

  if (iMode == C_PRED_DC_L) {
    uint8_t uiDc[8];
    for (int32_t y = 0; y < 4; ++y) {
      const int32_t iBase = y << 2;
      const int32_t iDc = (pLeft[iBase * iStride] + pLeft[(iBase + 1) * iStride] +
                           pLeft[(iBase + 2) * iStride] + pLeft[(iBase + 3) * iStride] + 2) >> 2;
      uiDc[2 * y] = uiDc[2 * y + 1] = static_cast<uint8_t> (iDc);
    }
    FillChromaBlocks (pPred, iStride, uiDc);
    return;
  }

  if (iMode == C_PRED_P) {
    // Equivalent to the normative 8x16 chroma plane process.
    int32_t iH = pTop[4] - pTop[2];
    int32_t iV = pLeft[8 * iStride] - pLeft[6 * iStride];
    for (int32_t k = 2; k <= 4; ++k)
      iH += k * (pTop[3 + k] - pTop[3 - k]);
    for (int32_t k = 2; k <= 8; ++k)
      iV += k * (pLeft[(7 + k) * iStride] - pLeft[(7 - k) * iStride]);
    iH = (17 * iH + 16) >> 5;
    iV = (5 * iV + 32) >> 6;
    const int32_t iA = 16 * (pLeft[15 * iStride] + pTop[7] + 1) - 7 * iV - 3 * iH;
    for (int32_t y = 0; y < 16; ++y) {
      for (int32_t x = 0; x < 8; ++x) {
        const int32_t iVal = (iA + y * iV + x * iH) >> 5;
        pPred[y * iStride + x] = ClipPixel422 (iVal);
      }
    }
    return;
  }

  // C_PRED_DC: both upper and left samples are available.
  int32_t iS0 = 0, iTopRight = 0;
  int32_t iLeft[4] = {0, 0, 0, 0};
  for (int32_t i = 0; i < 4; ++i) {
    iS0 += pTop[i] + pLeft[i * iStride];
    iTopRight += pTop[4 + i];
    iLeft[0] += pLeft[(4 + i) * iStride];
    iLeft[1] += pLeft[(8 + i) * iStride];
    iLeft[2] += pLeft[(12 + i) * iStride];
  }
  const int32_t iDc0 = (iS0 + 4) >> 3;
  const int32_t iDc1 = (iTopRight + 2) >> 2;
  const int32_t iDc2 = (iLeft[0] + 2) >> 2;
  const int32_t iDc3 = (iTopRight + iLeft[0] + 4) >> 3;
  const int32_t iDc4 = (iLeft[1] + 2) >> 2;
  const int32_t iDc5 = (iTopRight + iLeft[1] + 4) >> 3;
  const int32_t iDc6 = (iLeft[2] + 2) >> 2;
  const int32_t iDc7 = (iTopRight + iLeft[2] + 4) >> 3;
  const uint8_t uiDc[8] = {
    static_cast<uint8_t> (iDc0), static_cast<uint8_t> (iDc1),
    static_cast<uint8_t> (iDc2), static_cast<uint8_t> (iDc3),
    static_cast<uint8_t> (iDc4), static_cast<uint8_t> (iDc5),
    static_cast<uint8_t> (iDc6), static_cast<uint8_t> (iDc7)
  };
  FillChromaBlocks (pPred, iStride, uiDc);
}

void WelsExpandChromaPicture422 (uint8_t* pDst, int32_t iStride, int32_t iPicWidth, int32_t iPicHeight) {
  const int32_t iHorizontalPadding = 16;
  const int32_t iVerticalPadding = 32;
  uint8_t* pLastLine = pDst + (iPicHeight - 1) * iStride;
  const uint8_t uiTl = pDst[0];
  const uint8_t uiTr = pDst[iPicWidth - 1];
  const uint8_t uiBl = pLastLine[0];
  const uint8_t uiBr = pLastLine[iPicWidth - 1];

  for (int32_t i = 1; i <= iVerticalPadding; ++i) {
    uint8_t* pTop = pDst - i * iStride;
    uint8_t* pBottom = pLastLine + i * iStride;
    memcpy (pTop, pDst, iPicWidth);
    memcpy (pBottom, pLastLine, iPicWidth);
    memset (pTop - iHorizontalPadding, uiTl, iHorizontalPadding);
    memset (pTop + iPicWidth, uiTr, iHorizontalPadding);
    memset (pBottom - iHorizontalPadding, uiBl, iHorizontalPadding);
    memset (pBottom + iPicWidth, uiBr, iHorizontalPadding);
  }

  for (int32_t y = 0; y < iPicHeight; ++y) {
    uint8_t* pRow = pDst + y * iStride;
    memset (pRow - iHorizontalPadding, pRow[0], iHorizontalPadding);
    memset (pRow + iPicWidth, pRow[iPicWidth - 1], iHorizontalPadding);
  }
}

void WelsMcChroma422_c (const uint8_t* pSrc, int32_t iSrcStride, uint8_t* pDst, int32_t iDstStride,
                        int16_t iMvX, int16_t iMvY, int32_t iWidth, int32_t iHeight) {
  const int32_t iDx = iMvX & 7;
  const int32_t iDy = iMvY & 7;
  if (iDx == 0 && iDy == 0) {
    for (int32_t y = 0; y < iHeight; ++y) {
      for (int32_t x = 0; x < iWidth; ++x)
        pDst[y * iDstStride + x] = pSrc[y * iSrcStride + x];
    }
    return;
  }

  const int32_t iWeightA = (8 - iDx) * (8 - iDy);
  const int32_t iWeightB = iDx * (8 - iDy);
  const int32_t iWeightC = (8 - iDx) * iDy;
  const int32_t iWeightD = iDx * iDy;
  for (int32_t y = 0; y < iHeight; ++y) {
    const uint8_t* pRow = pSrc + y * iSrcStride;
    const uint8_t* pNext = pRow + iSrcStride;
    uint8_t* pOut = pDst + y * iDstStride;
    for (int32_t x = 0; x < iWidth; ++x) {
      const int32_t iValue = (iWeightA * pRow[x] + iWeightB * pRow[x + 1] +
                              iWeightC * pNext[x] + iWeightD * pNext[x + 1] + 32) >> 6;
      pOut[x] = ClipPixel422 (iValue);
    }
  }
}

} // namespace WelsDec
