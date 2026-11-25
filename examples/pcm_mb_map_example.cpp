/*!
 * \file pcm_mb_map_example.cpp
 *
 * \brief Example: preserve selected macroblocks as raw I_PCM samples while the
 *        rest of each frame is normally H.264 compressed, using
 *        ENCODER_OPTION_PCM_MB_MAP / SPcmMbMap.
 *
 * Build (from the repository root, after building libopenh264.a):
 *   c++ -std=c++11 -Icodec/api examples/pcm_mb_map_example.cpp \
 *       libopenh264.a -pthread -o pcm_mb_map_example
 *
 * The supported envelope shown here is: one spatial layer, I420 8-bit,
 * width/height multiples of 16, RC_OFF_MODE, denoise and frame skip disabled,
 * I/P coding, CAVLC or CABAC, single slice or a fixed slice count, with
 * iLoopFilterDisableIdc = 1 so decoded I_PCM MBs are byte identical to the
 * source. Dynamic byte-limited slicing and SVC layers are not extended.
 */

#include "wels/codec_api.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
  ISVCEncoder* pEnc = NULL;
  if (WelsCreateSVCEncoder (&pEnc) != 0 || pEnc == NULL) {
    std::printf ("WelsCreateSVCEncoder failed\n");
    return 1;
  }

  const int kiWidth = 64;   // 4 MBs
  const int kiHeight = 64;  // 4 MBs
  const int kiMbWidth = kiWidth / 16;
  const int kiMbHeight = kiHeight / 16;

  SEncParamExt sParam;
  std::memset (&sParam, 0, sizeof (sParam));
  if (pEnc->GetDefaultParams (&sParam) != cmResultSuccess)
    return 2;

  sParam.iUsageType            = CAMERA_VIDEO_REAL_TIME;
  sParam.iPicWidth             = kiWidth;
  sParam.iPicHeight            = kiHeight;
  sParam.fMaxFrameRate         = 25.0f;
  sParam.iRCMode               = RC_OFF_MODE;
  sParam.iTargetBitrate        = 200000;
  sParam.iTemporalLayerNum     = 1;
  sParam.iSpatialLayerNum      = 1;
  sParam.bEnableDenoise        = false;
  sParam.bEnableFrameSkip      = false;
  sParam.bEnableBackgroundDetection = false;
  sParam.bEnableAdaptiveQuant  = false;
  sParam.bEnableSceneChangeDetect = false;
  sParam.iMultipleThreadIdc    = 1;
  sParam.iLoopFilterDisableIdc = 1;   // required for byte exact PCM regions
  sParam.iEntropyCodingModeFlag = 1;  // 1: CABAC, 0: CAVLC

  sParam.sSpatialLayers[0].iVideoWidth  = kiWidth;
  sParam.sSpatialLayers[0].iVideoHeight = kiHeight;
  sParam.sSpatialLayers[0].fFrameRate   = 25.0f;
  sParam.sSpatialLayers[0].iSpatialBitrate = 200000;
  sParam.sSpatialLayers[0].uiProfileIdc = PRO_MAIN; // CABAC needs Main profile
  sParam.sSpatialLayers[0].iDLayerQp    = 28;
  sParam.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

  if (pEnc->InitializeExt (&sParam) != cmResultSuccess)
    return 3;

  // Mark a 2x2 macroblock region (MB coordinates 1..2, 1..2) as I_PCM.
  // Zero entries are coded normally; non-zero entries force I_PCM.
  std::vector<unsigned char> mbMap (kiMbWidth * kiMbHeight, 0);
  for (int my = 1; my <= 2; ++my)
    for (int mx = 1; mx <= 2; ++mx)
      mbMap[my * kiMbWidth + mx] = 1;

  SPcmMbMap sPcmMap;
  std::memset (&sPcmMap, 0, sizeof (sPcmMap));
  sPcmMap.pMap      = mbMap.data();
  sPcmMap.iMbWidth  = kiMbWidth;
  sPcmMap.iMbHeight = kiMbHeight;
  sPcmMap.iStride   = kiMbWidth; // bytes per grid row, may be larger

  // SetOption copies the grid synchronously: mbMap can be reused/freed now.
  int iRet = pEnc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &sPcmMap);
  std::printf ("SetOption(ENCODER_OPTION_PCM_MB_MAP) = %d\n", iRet);
  if (iRet != cmResultSuccess)
    return 4;

  // Prepare one recognizable I420 picture (Y plane then Cb then Cr).
  std::vector<unsigned char> yuv (kiWidth * kiHeight * 3 / 2);
  for (int y = 0; y < kiHeight; ++y)
    for (int x = 0; x < kiWidth; ++x)
      yuv[y * kiWidth + x] = (unsigned char) ((x * 7 + y * 13) & 0xff);

  SSourcePicture sPic;
  std::memset (&sPic, 0, sizeof (sPic));
  sPic.iColorFormat = videoFormatI420;
  sPic.iPicWidth    = kiWidth;
  sPic.iPicHeight   = kiHeight;
  sPic.iStride[0]   = kiWidth;
  sPic.iStride[1] = sPic.iStride[2] = kiWidth / 2;
  sPic.pData[0] = yuv.data();
  sPic.pData[1] = yuv.data() + kiWidth * kiHeight;
  sPic.pData[2] = yuv.data() + kiWidth * kiHeight * 5 / 4;
  sPic.uiTimeStamp = 0;

  SFrameBSInfo sBsInfo;
  std::memset (&sBsInfo, 0, sizeof (sBsInfo));
  iRet = pEnc->EncodeFrame (&sPic, &sBsInfo);
  std::printf ("EncodeFrame() = %d, layers=%d\n", iRet, sBsInfo.iLayerNum);

  // ... feed further frames with EncodeFrame() here; the same map stays
  // active until replaced by another ENCODER_OPTION_PCM_MB_MAP.

  // Clear the map: pMap == NULL ignores the other three fields.
  SPcmMbMap sClear;
  std::memset (&sClear, 0, sizeof (sClear));
  sClear.pMap = NULL;
  pEnc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &sClear);

  pEnc->Uninitialize();
  WelsDestroySVCEncoder (pEnc);
  return 0;
}
