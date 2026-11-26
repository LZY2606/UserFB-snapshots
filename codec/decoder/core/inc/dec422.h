#ifndef WELS_DEC_422_H__
#define WELS_DEC_422_H__

#include <stdint.h>

namespace WelsDec {

// Progressive 8-bit H.264 4:2:2 chroma macroblock support.
void WelsIChromaPred422 (uint8_t* pPred, int32_t iStride, int8_t iMode);
void WelsMcChroma422_c (const uint8_t* pSrc, int32_t iSrcStride, uint8_t* pDst, int32_t iDstStride,
                        int16_t iMvX, int16_t iMvY, int32_t iWidth, int32_t iHeight);
void WelsExpandChromaPicture422 (uint8_t* pDst, int32_t iStride, int32_t iPicWidth, int32_t iPicHeight);

} // namespace WelsDec

#endif // WELS_DEC_422_H__
