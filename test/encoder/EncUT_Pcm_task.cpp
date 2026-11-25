/*!
 * \file    EncUT_Pcm_task.cpp
 *
 * \brief   Task cases for ENCODER_OPTION_PCM_MB_MAP (per-MB I_PCM).
 *
 * Registers three groups through the fixed task_support driver:
 *   syntax    - I_PCM really reaches the bitstream for CAVLC/CABAC,
 *               single/fixed multi slice and I/P frames
 *   reference - decoded PCM samples are byte exact and later P frames do
 *               not drift
 *   ownership - synchronous copy, replace/clear, bad arguments keep the
 *               active map, independent encoders and re-init state
 */

#include "task_support/check.h"

#include "wels/codec_api.h"
#include "wels/codec_app_def.h"

#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

static const int TASK_W = 64;
static const int TASK_H = 64;
static const int TASK_MB_W = TASK_W / 16; // 4
static const int TASK_MB_H = TASK_H / 16; // 4
static const int TASK_MB_NUM = TASK_MB_W * TASK_MB_H;

/* ----------------------------- test textures ---------------------------- */

static uint32_t TaskHash (uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Recognizable, strongly non-flat and plane specific I420 texture.
// bStable keeps consecutive frames identical for reference propagation tests.
static void TaskFillRaw (std::vector<unsigned char>& raw, int iFrame, bool bStable) {
  const int f = bStable ? 3 : iFrame;
  raw.assign (TASK_W * TASK_H * 3 / 2, 0);
  for (int y = 0; y < TASK_H; ++y) {
    for (int x = 0; x < TASK_W; ++x) {
      uint32_t h = TaskHash ((uint32_t) (x * 73856093u ^ y * 19349663u ^ f * 83492791u));
      int v = (x * 5 + y * 11 + f * 17 + (int) (h & 31) - 15) & 255;
      // add a bold checker/edge component so 16x16 blocks have structure
      if (((x >> 2) ^ (y >> 2)) & 1)
        v = (v + 48) & 255;
      raw[y * TASK_W + x] = (unsigned char)v;
    }
  }
  unsigned char* pU = &raw[TASK_W * TASK_H];
  unsigned char* pV = &raw[TASK_W * TASK_H * 5 / 4];
  for (int y = 0; y < TASK_H / 2; ++y) {
    for (int x = 0; x < TASK_W / 2; ++x) {
      uint32_t h = TaskHash ((uint32_t) (x * 2654435761u ^ y * 2246822519u ^ f * 3266489917u));
      pU[y * (TASK_W / 2) + x] = (unsigned char) ((110 + x * 7 - y * 5 + f * 3 + (int) (h & 15)) & 255);
      pV[y * (TASK_W / 2) + x] = (unsigned char) ((150 - x * 4 + y * 9 - f * 5 + (int) ((h >> 8) & 15)) & 255);
    }
  }
}

/* ------------------------------ map helpers ------------------------------ */

static void TaskMapSet (std::vector<unsigned char>& map, int mx, int my) {
  if (mx >= 0 && mx < TASK_MB_W && my >= 0 && my < TASK_MB_H)
    map[my * TASK_MB_W + mx] = 1;
}

static int TaskMapCount (const std::vector<unsigned char>& map) {
  int n = 0;
  for (size_t i = 0; i < map.size(); ++i)
    if (map[i])
      ++n;
  return n;
}

// Apply compact map (stride == MB width). pMap == NULL clears the active map.
static int TaskApplyMap (ISVCEncoder* e, const std::vector<unsigned char>* pMap) {
  SPcmMbMap m;
  std::memset (&m, 0, sizeof (m));
  if (pMap == NULL) {
    m.pMap = NULL;
    return e->SetOption (ENCODER_OPTION_PCM_MB_MAP, &m);
  }
  m.pMap = pMap->data();
  m.iMbWidth = TASK_MB_W;
  m.iMbHeight = TASK_MB_H;
  m.iStride = TASK_MB_W;
  return e->SetOption (ENCODER_OPTION_PCM_MB_MAP, &m);
}

/* --------------------------- decoder observation ------------------------- */

static void TaskTraceCb (void* ctx, int /*level*/, const char* s) {
  if (ctx != NULL && s != NULL && std::strstr (s, "I_PCM") != NULL)
    ++ (*static_cast<int*> (ctx));
}

struct TaskPic {
  std::vector<unsigned char> yuv; // compact I420, stride W / W/2
};

struct TaskCodec {
  ISVCEncoder* enc;
  ISVCDecoder* dec;
  SEncParamExt param;
  int pcmEvents;
  DECODING_STATE lastDs;

  TaskCodec() : enc (NULL), dec (NULL), pcmEvents (0), lastDs (dsErrorFree) {
    std::memset (&param, 0, sizeof (param));
  }

  int init (int iEntropy, int iSliceNum, bool bSupported = true,
            RC_MODES eRcMode = RC_OFF_MODE, bool bDenoise = false, bool bSkip = false,
            SliceModeEnum eSliceMode = SM_SINGLE_SLICE, int iW = TASK_W, int iH = TASK_H) {
    if (WelsCreateSVCEncoder (&enc) != 0 || enc == NULL)
      return 1;
    int quiet = WELS_LOG_QUIET;
    enc->SetOption (ENCODER_OPTION_TRACE_LEVEL, &quiet);
    if (enc->GetDefaultParams (&param) != 0)
      return 2;

    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth = iW;
    param.iPicHeight = iH;
    param.fMaxFrameRate = 25;
    param.iTargetBitrate = 200000;
    param.iRCMode = bSupported ? RC_OFF_MODE : eRcMode;
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.bEnableDenoise = bDenoise;
    param.bEnableFrameSkip = bSkip;
    param.bEnableBackgroundDetection = false;
    param.bEnableAdaptiveQuant = false;
    param.bEnableSceneChangeDetect = false;
    param.iMultipleThreadIdc = 1;
    param.iLoopFilterDisableIdc = 1;
    param.iEntropyCodingModeFlag = iEntropy;
    param.sSpatialLayers[0].iVideoWidth = iW;
    param.sSpatialLayers[0].iVideoHeight = iH;
    param.sSpatialLayers[0].fFrameRate = 25;
    param.sSpatialLayers[0].iSpatialBitrate = 200000;
    param.sSpatialLayers[0].uiProfileIdc = iEntropy ? PRO_MAIN : PRO_BASELINE;
    param.sSpatialLayers[0].iDLayerQp = 28;
    param.sSpatialLayers[0].sSliceArgument.uiSliceMode = bSupported ?
        (iSliceNum > 1 ? SM_FIXEDSLCNUM_SLICE : SM_SINGLE_SLICE) : eSliceMode;
    if (iSliceNum > 1)
      param.sSpatialLayers[0].sSliceArgument.uiSliceNum = iSliceNum;

    if (enc->InitializeExt (&param) != cmResultSuccess)
      return 3;

    if (WelsCreateDecoder (&dec) != 0 || dec == NULL)
      return 4;
    uint32_t uiLevel = WELS_LOG_DETAIL;
    dec->SetOption (DECODER_OPTION_TRACE_LEVEL, &uiLevel);
    WelsTraceCallback cb = TaskTraceCb;
    dec->SetOption (DECODER_OPTION_TRACE_CALLBACK, &cb);
    void* pCbCtx = &pcmEvents;
    dec->SetOption (DECODER_OPTION_TRACE_CALLBACK_CONTEXT, &pCbCtx);

    SDecodingParam dp;
    std::memset (&dp, 0, sizeof (dp));
    dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    dp.eEcActiveIdc = ERROR_CON_DISABLE;
    if (dec->Initialize (&dp) != 0)
      return 5;
    return 0;
  }

  void shutdown() {
    if (enc != NULL) {
      enc->Uninitialize();
      WelsDestroySVCEncoder (enc);
      enc = NULL;
    }
    if (dec != NULL) {
      dec->Uninitialize();
      WelsDestroyDecoder (dec);
      dec = NULL;
    }
  }

  int reinitEnc() {
    if (enc == NULL)
      return 1;
    enc->Uninitialize();
    return enc->InitializeExt (&param) == cmResultSuccess ? 0 : 2;
  }

  ~TaskCodec() {
    shutdown();
  }

  int encode (const std::vector<unsigned char>& raw, int iFrame, std::vector<unsigned char>& bs) {
    SSourcePicture pic;
    std::memset (&pic, 0, sizeof (pic));
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = TASK_W;
    pic.iPicHeight = TASK_H;
    pic.iStride[0] = TASK_W;
    pic.iStride[1] = pic.iStride[2] = TASK_W / 2;
    pic.pData[0] = const_cast<unsigned char*> (raw.data());
    pic.pData[1] = const_cast<unsigned char*> (raw.data() + TASK_W * TASK_H);
    pic.pData[2] = const_cast<unsigned char*> (raw.data() + TASK_W * TASK_H * 5 / 4);
    pic.uiTimeStamp = (long long)iFrame * 40;

    SFrameBSInfo out;
    std::memset (&out, 0, sizeof (out));
    int iRet = enc->EncodeFrame (&pic, &out);
    if (iRet != cmResultSuccess)
      return iRet;

    bs.clear();
    for (int l = 0; l < out.iLayerNum; ++l) {
      SLayerBSInfo& layer = out.sLayerInfo[l];
      int len = 0;
      for (int k = 0; k < layer.iNalCount; ++k)
        len += layer.pNalLengthInByte[k];
      if (len > 0)
        bs.insert (bs.end(), layer.pBsBuf, layer.pBsBuf + len);
    }
    return 0;
  }

  // Decode one access unit, appending compact I420 output pictures.
  int decode (const std::vector<unsigned char>& bs, std::vector<TaskPic>& pics) {
    unsigned char* planes[3] = {NULL, NULL, NULL};
    SBufferInfo info;
    std::memset (&info, 0, sizeof (info));
    lastDs = dec->DecodeFrameNoDelay (bs.empty() ? NULL : bs.data(), (int)bs.size(),
                                      planes, &info);
    if (lastDs != dsErrorFree)
      return 1;
    if (info.iBufferStatus == 1)
      appendPic (planes, info, pics);
    return 0;
  }

  int drain (std::vector<TaskPic>& pics) {
    int eos = 1;
    dec->SetOption (DECODER_OPTION_END_OF_STREAM, &eos);
    for (int attempt = 0; attempt < 6; ++attempt) {
      unsigned char* planes[3] = {NULL, NULL, NULL};
      SBufferInfo info;
      std::memset (&info, 0, sizeof (info));
      lastDs = attempt == 0
             ? dec->DecodeFrameNoDelay (NULL, 0, planes, &info)
             : dec->FlushFrame (planes, &info);
      if (lastDs != dsErrorFree)
        return 1;
      if (info.iBufferStatus == 1)
        appendPic (planes, info, pics);
    }
    return 0;
  }

  void appendPic (unsigned char* planes[3], SBufferInfo& info, std::vector<TaskPic>& pics) {
    const int iW = info.UsrData.sSystemBuffer.iWidth;
    const int iH = info.UsrData.sSystemBuffer.iHeight;
    if (iW != TASK_W || iH != TASK_H)
      return;
    TaskPic p;
    p.yuv.assign (TASK_W * TASK_H * 3 / 2, 0);
    const int sY = info.UsrData.sSystemBuffer.iStride[0];
    const int sC = info.UsrData.sSystemBuffer.iStride[1];
    for (int y = 0; y < TASK_H; ++y)
      std::memcpy (&p.yuv[y * TASK_W], planes[0] + y * sY, TASK_W);
    for (int y = 0; y < TASK_H / 2; ++y) {
      std::memcpy (&p.yuv[TASK_W * TASK_H + y * (TASK_W / 2)], planes[1] + y * sC, TASK_W / 2);
      std::memcpy (&p.yuv[TASK_W * TASK_H * 5 / 4 + y * (TASK_W / 2)], planes[2] + y * sC,
                   TASK_W / 2);
    }
    pics.push_back (p);
  }
};

/* ------------------------------ verification ----------------------------- */

// Count samples that differ from src inside macroblocks selected by map.
static long long TaskPcmMismatch (const TaskPic& pic, const std::vector<unsigned char>& src,
                                  const std::vector<unsigned char>& map) {
  long long bad = 0;
  for (int my = 0; my < TASK_MB_H; ++my) {
    for (int mx = 0; mx < TASK_MB_W; ++mx) {
      if (!map[my * TASK_MB_W + mx])
        continue;
      for (int r = 0; r < 16; ++r) {
        const int y = my * 16 + r;
        for (int x = mx * 16; x < mx * 16 + 16; ++x) {
          if (pic.yuv[y * TASK_W + x] != src[y * TASK_W + x])
            ++bad;
        }
      }
      for (int r = 0; r < 8; ++r) {
        const int y = my * 8 + r;
        for (int x = mx * 8; x < mx * 8 + 8; ++x) {
          if (pic.yuv[TASK_W * TASK_H + y * (TASK_W / 2) + x]
              != src[TASK_W * TASK_H + y * (TASK_W / 2) + x])
            ++bad;
          if (pic.yuv[TASK_W * TASK_H * 5 / 4 + y * (TASK_W / 2) + x]
              != src[TASK_W * TASK_H * 5 / 4 + y * (TASK_W / 2) + x])
            ++bad;
        }
      }
    }
  }
  return bad;
}

// Build the exact 384 byte I_PCM sample block expected for one MB.
static void TaskBuildPcmBlock (const std::vector<unsigned char>& src, int mx, int my,
                               unsigned char block[384]) {
  unsigned char* p = block;
  for (int r = 0; r < 16; ++r) {
    std::memcpy (p, &src[(my * 16 + r) * TASK_W + mx * 16], 16);
    p += 16;
  }
  for (int plane = 1; plane <= 2; ++plane) {
    const size_t off = plane == 1 ? TASK_W * TASK_H : TASK_W * TASK_H * 5 / 4;
    for (int r = 0; r < 8; ++r) {
      std::memcpy (p, &src[off + (my * 8 + r) * (TASK_W / 2) + mx * 8], 8);
      p += 8;
    }
  }
}

// Remove emulation prevention bytes (00 00 03) from an Annex-B EBSP and return
// the raw RBSP bytes used by the encoder before NAL encapsulation.
static std::vector<unsigned char> TaskUnescape (const std::vector<unsigned char>& bs) {
  std::vector<unsigned char> out;
  out.reserve (bs.size());
  int zeros = 0;
  for (size_t i = 0; i < bs.size(); ++i) {
    unsigned char c = bs[i];
    if (zeros >= 2 && c == 0x03 && i + 1 < bs.size() && bs[i + 1] <= 0x03) {
      zeros = 0;
      continue; // drop emulation prevention byte
    }
    out.push_back (c);
    if (c == 0)
      ++zeros;
    else
      zeros = 0;
  }
  return out;
}

static bool TaskContainsPcmBlock (const std::vector<unsigned char>& bs,
                                  const std::vector<unsigned char>& src, int mx, int my) {
  unsigned char block[384];
  TaskBuildPcmBlock (src, mx, my, block);
  std::vector<unsigned char> rbsp = TaskUnescape (bs);
  if (rbsp.size() < 384)
    return false;
  for (size_t i = 0; i + 384 <= rbsp.size(); ++i) {
    if (std::memcmp (&rbsp[i], block, 384) == 0)
      return true;
  }
  return false;
}

// A mixed map that includes the first and last MBs of the frame and a spread
// of interior MBs, leaving plenty of normal/skip macroblocks around.
static std::vector<unsigned char> TaskMixedMap() {
  std::vector<unsigned char> map (TASK_MB_NUM, 0);
  TaskMapSet (map, 0, 0);               // first MB of first slice
  TaskMapSet (map, TASK_MB_W - 1, 0);  // top right
  TaskMapSet (map, 0, TASK_MB_H - 1);  // bottom left
  TaskMapSet (map, TASK_MB_W - 1, TASK_MB_H - 1); // very last MB
  for (int my = 0; my < TASK_MB_H; ++my)
    for (int mx = 0; mx < TASK_MB_W; ++mx)
      if (((mx + 2 * my) % 3) == 1)
        TaskMapSet (map, mx, my);
  return map;
}

/* ------------------------------- test cores ------------------------------ */

// Full I/P sequence with one fixed map; checks syntax observation, raw PCM
// payload presence and byte exact decoded regions.
static int TaskCaseMixedSyntax (int iEntropy, int iSliceNum) {
  TaskCodec c;
  TASK_CHECK (c.init (iEntropy, iSliceNum) == 0);

  std::vector<unsigned char> map = TaskMixedMap();
  const int iMarked = TaskMapCount (map);
  TASK_CHECK (iMarked >= 4);
  TASK_CHECK (TaskApplyMap (c.enc, &map) == cmResultSuccess);

  const int iFrames = 4;
  int iExpectedEvents = 0;
  std::vector<TaskPic> pics;
  std::vector<std::vector<unsigned char> > raws (iFrames);
  std::vector<std::vector<unsigned char> > maps (iFrames);
  std::vector<std::vector<unsigned char> > bss (iFrames);

  for (int f = 0; f < iFrames; ++f) {
    TaskFillRaw (raws[f], f, false);
    maps[f] = map;
    TASK_CHECK (c.encode (raws[f], f, bss[f]) == 0);
    TASK_CHECK (!bss[f].empty());
    iExpectedEvents += iMarked;
    TASK_CHECK (c.decode (bss[f], pics) == 0);
  }
  TASK_CHECK (c.drain (pics) == 0);
  TASK_CHECK ((int)pics.size() == iFrames);

  // The decoder's own syntax parser observed exactly one I_PCM per marked MB.
  TASK_CHECK (c.pcmEvents == iExpectedEvents);

  // Independent stream check: exact 384 byte I_PCM sample blocks are present
  // in the raw RBSP for a corner and an interior MB.
  TASK_CHECK (TaskContainsPcmBlock (bss[0], raws[0], 0, 0));
  TASK_CHECK (TaskContainsPcmBlock (bss[1], raws[1], TASK_MB_W - 1, TASK_MB_H - 1));

  for (int f = 0; f < iFrames && f < (int)pics.size(); ++f) {
    TASK_CHECK (TaskPcmMismatch (pics[f], raws[f], maps[f]) == 0);
  }
  return 0;
}

// Static scene: an I frame carries PCM anchors, later P frames clear the map.
// Zero-MV skip/inter propagation of the exact reconstruction must stay exact.
static int TaskCaseReferenceDrift (int iEntropy) {
  TaskCodec c;
  TASK_CHECK (c.init (iEntropy, 1) == 0);

  std::vector<unsigned char> map (TASK_MB_NUM, 0);
  // contiguous cluster plus isolated anchors, covering multiple neighbors
  TaskMapSet (map, 1, 1);
  TaskMapSet (map, 2, 1);
  TaskMapSet (map, 1, 2);
  TaskMapSet (map, 2, 2);
  TaskMapSet (map, 0, 1);
  TaskMapSet (map, 3, 2);
  const int iMarked = TaskMapCount (map);
  TASK_CHECK (iMarked == 6);
  TASK_CHECK (TaskApplyMap (c.enc, &map) == 0);

  std::vector<unsigned char> raw;
  TaskFillRaw (raw, 0, true);

  std::vector<TaskPic> pics;
  std::vector<unsigned char> bs;

  TASK_CHECK (c.encode (raw, 0, bs) == 0);
  TASK_CHECK (c.decode (bs, pics) == 0);

  // Clear the map: all following P frames are ordinary coding.
  TASK_CHECK (TaskApplyMap (c.enc, NULL) == 0);

  const int iPFrames = 5;
  for (int f = 1; f <= iPFrames; ++f) {
    TASK_CHECK (c.encode (raw, f, bs) == 0);
    TASK_CHECK (c.decode (bs, pics) == 0);
  }
  TASK_CHECK (c.drain (pics) == 0);
  TASK_CHECK ((int)pics.size() == iPFrames + 1);
  TASK_CHECK (c.pcmEvents == iMarked); // only the IDR contained I_PCM syntax

  // Every output, including all later P frames, keeps the anchor area byte
  // exact: any encoder/decoder reference drift shows up as mismatches here.
  for (size_t f = 0; f < pics.size(); ++f) {
    TASK_CHECK (TaskPcmMismatch (pics[f], raw, map) == 0);
  }
  return 0;
}

// Replace the map between frames and clear it; each output must match its own
// frame map and the decoder must observe the right total I_PCM count.
static int TaskCaseChangingMaps (int iEntropy) {
  TaskCodec c;
  TASK_CHECK (c.init (iEntropy, 1) == 0);

  std::vector<unsigned char> mapA (TASK_MB_NUM, 0);
  TaskMapSet (mapA, 0, 0);
  TaskMapSet (mapA, 2, 2);
  TaskMapSet (mapA, 3, 0);
  std::vector<unsigned char> mapB (TASK_MB_NUM, 0);
  TaskMapSet (mapB, 1, 0);
  TaskMapSet (mapB, 1, 3);
  const int nA = TaskMapCount (mapA);
  const int nB = TaskMapCount (mapB);

  std::vector<TaskPic> pics;
  int iExpectedEvents = 0;

  struct FrameSpec {
    std::vector<unsigned char> map;
    bool hasMap;
  };
  std::vector<FrameSpec> specs (4);

  std::vector<std::vector<unsigned char> > raws (4);
  std::vector<std::vector<unsigned char> > bss (4);

  // frame 0: map A
  TASK_CHECK (TaskApplyMap (c.enc, &mapA) == 0);
  TaskFillRaw (raws[0], 0, false);
  specs[0].map = mapA;
  specs[0].hasMap = true;
  TASK_CHECK (c.encode (raws[0], 0, bss[0]) == 0);
  iExpectedEvents += nA;
  TASK_CHECK (c.decode (bss[0], pics) == 0);

  // frame 1: replace with B (caller array may change afterwards)
  TASK_CHECK (TaskApplyMap (c.enc, &mapB) == 0);
  TaskFillRaw (raws[1], 1, false);
  specs[1].map = mapB;
  specs[1].hasMap = true;
  TASK_CHECK (c.encode (raws[1], 1, bss[1]) == 0);
  iExpectedEvents += nB;
  TASK_CHECK (c.decode (bss[1], pics) == 0);

  // frame 2: clear
  TASK_CHECK (TaskApplyMap (c.enc, NULL) == 0);
  TaskFillRaw (raws[2], 2, false);
  specs[2].hasMap = false;
  TASK_CHECK (c.encode (raws[2], 2, bss[2]) == 0);
  TASK_CHECK (c.decode (bss[2], pics) == 0);

  // frame 3: set A again
  TASK_CHECK (TaskApplyMap (c.enc, &mapA) == 0);
  TaskFillRaw (raws[3], 3, false);
  specs[3].map = mapA;
  specs[3].hasMap = true;
  TASK_CHECK (c.encode (raws[3], 3, bss[3]) == 0);
  iExpectedEvents += nA;
  TASK_CHECK (c.decode (bss[3], pics) == 0);

  TASK_CHECK (c.drain (pics) == 0);
  TASK_CHECK ((int)pics.size() == 4);
  TASK_CHECK (c.pcmEvents == iExpectedEvents);

  for (int f = 0; f < 4; ++f) {
    if (specs[f].hasMap)
      TASK_CHECK (TaskPcmMismatch (pics[f], raws[f], specs[f].map) == 0);
  }
  // cleared frame must not carry any I_PCM sample block for mapA positions
  TASK_CHECK (!TaskContainsPcmBlock (bss[2], raws[2], 2, 2));
  return 0;
}

/* ------------------------------ ownership cases -------------------------- */

static int TaskCaseCopyAndStride (void) {
  TaskCodec c;
  TASK_CHECK (c.init (0, 1) == 0);

  // Caller grid uses a stride larger than the MB width and non-byte values.
  const int iCallerStride = TASK_MB_W + 3;
  std::vector<unsigned char> caller (iCallerStride * TASK_MB_H, 0);
  caller[0 * iCallerStride + 0] = 0xFF;
  caller[1 * iCallerStride + 2] = 7;
  caller[3 * iCallerStride + 3] = 200;

  SPcmMbMap m;
  std::memset (&m, 0, sizeof (m));
  m.pMap = caller.data();
  m.iMbWidth = TASK_MB_W;
  m.iMbHeight = TASK_MB_H;
  m.iStride = iCallerStride;
  TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &m) == cmResultSuccess);

  // Rewrite and shrink the caller memory after SetOption: the encoder must
  // keep using its synchronous compact copy.
  std::memset (caller.data(), 0xAB, caller.size());

  std::vector<unsigned char> map (TASK_MB_NUM, 0);
  map[0] = 1;
  map[2 + 1 * TASK_MB_W] = 1;
  map[3 + 3 * TASK_MB_W] = 1;

  std::vector<unsigned char> raw, bs;
  TaskFillRaw (raw, 4, false);
  TASK_CHECK (c.encode (raw, 0, bs) == 0);
  std::vector<TaskPic> pics;
  TASK_CHECK (c.decode (bs, pics) == 0);
  TASK_CHECK (c.drain (pics) == 0);
  TASK_CHECK (pics.size() == 1);
  TASK_CHECK (c.pcmEvents == 3);
  TASK_CHECK (TaskPcmMismatch (pics[0], raw, map) == 0);
  return 0;
}

static int TaskCaseBadMapKeepsActive (void) {
  TaskCodec c;
  TASK_CHECK (c.init (0, 1) == 0);

  std::vector<unsigned char> good (TASK_MB_NUM, 0);
  good[0] = 1;
  SPcmMbMap m;
  std::memset (&m, 0, sizeof (m));
  m.pMap = good.data();
  m.iMbWidth = TASK_MB_W;
  m.iMbHeight = TASK_MB_H;
  m.iStride = TASK_MB_W;
  TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &m) == cmResultSuccess);

  // Wrong width.
  SPcmMbMap bad = m;
  bad.iMbWidth = TASK_MB_W + 1;
  TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &bad) != cmResultSuccess);

  // Wrong height.
  bad = m;
  bad.iMbHeight = TASK_MB_H + 2;
  TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &bad) != cmResultSuccess);

  // Stride smaller than grid width.
  bad = m;
  bad.iStride = TASK_MB_W - 1;
  TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &bad) != cmResultSuccess);

  // NULL option pointer itself is rejected.
  TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, NULL) != cmResultSuccess);

  // The active good map must still be in place.
  std::vector<unsigned char> raw, bs;
  TaskFillRaw (raw, 1, false);
  TASK_CHECK (c.encode (raw, 0, bs) == 0);
  std::vector<TaskPic> pics;
  TASK_CHECK (c.decode (bs, pics) == 0);
  TASK_CHECK (c.drain (pics) == 0);
  TASK_CHECK (pics.size() == 1);
  TASK_CHECK (c.pcmEvents == 1);
  TASK_CHECK (TaskPcmMismatch (pics[0], raw, good) == 0);

  // Clearing with pMap = NULL and arbitrary/zero geometry succeeds.
  SPcmMbMap clear;
  std::memset (&clear, 0, sizeof (clear));
  clear.pMap = NULL;
  clear.iMbWidth = -123;
  clear.iMbHeight = 999;
  clear.iStride = -7;
  TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &clear) == cmResultSuccess);
  return 0;
}

static int TaskCaseTwoEncoders (void) {
  TaskCodec a, b;
  TASK_CHECK (a.init (0, 1) == 0);
  TASK_CHECK (b.init (1, 1) == 0); // different entropy too

  std::vector<unsigned char> mapA (TASK_MB_NUM, 0);
  mapA[0] = 1;
  mapA[TASK_MB_NUM - 1] = 1;
  std::vector<unsigned char> mapB (TASK_MB_NUM, 0);
  mapB[5] = 1;
  mapB[10] = 1;
  mapB[11] = 1;

  TASK_CHECK (TaskApplyMap (a.enc, &mapA) == 0);
  TASK_CHECK (TaskApplyMap (b.enc, &mapB) == 0);

  std::vector<unsigned char> raw;
  TaskFillRaw (raw, 2, false);

  std::vector<unsigned char> bsA, bsB;
  TASK_CHECK (a.encode (raw, 0, bsA) == 0);
  TASK_CHECK (b.encode (raw, 0, bsB) == 0);

  std::vector<TaskPic> pA, pB;
  TASK_CHECK (a.decode (bsA, pA) == 0);
  TASK_CHECK (b.decode (bsB, pB) == 0);
  TASK_CHECK (a.drain (pA) == 0);
  TASK_CHECK (b.drain (pB) == 0);
  TASK_CHECK (pA.size() == 1 && pB.size() == 1);

  TASK_CHECK (a.pcmEvents == 2);
  TASK_CHECK (b.pcmEvents == 3);
  TASK_CHECK (TaskPcmMismatch (pA[0], raw, mapA) == 0);
  TASK_CHECK (TaskPcmMismatch (pB[0], raw, mapB) == 0);

  // Encoder A must not encode B's MBs as PCM and vice versa: check the other
  // map positions are not raw PCM blocks in the opposite stream.
  TASK_CHECK (!TaskContainsPcmBlock (bsA, raw, 1, 1)); // MB 5 -> (1,1)
  return 0;
}

static int TaskCaseReinitClearsMap (void) {
  TaskCodec c;
  TASK_CHECK (c.init (0, 1) == 0);

  std::vector<unsigned char> map (TASK_MB_NUM, 1); // all PCM
  TASK_CHECK (TaskApplyMap (c.enc, &map) == 0);

  TASK_CHECK (c.reinitEnc() == 0);

  std::vector<unsigned char> raw, bs;
  TaskFillRaw (raw, 0, false);
  TASK_CHECK (c.encode (raw, 0, bs) == 0);
  std::vector<TaskPic> pics;
  TASK_CHECK (c.decode (bs, pics) == 0);
  TASK_CHECK (c.drain (pics) == 0);
  TASK_CHECK (pics.size() == 1);
  TASK_CHECK (c.pcmEvents == 0); // fresh context has no map
  return 0;
}

static int TaskCaseUnsupportedConfig (void) {
  // RC on.
  {
    TaskCodec c;
    TASK_CHECK (c.init (0, 1, false, RC_QUALITY_MODE) == 0);
    std::vector<unsigned char> map (TASK_MB_NUM, 1);
    TASK_CHECK (TaskApplyMap (c.enc, &map) != cmResultSuccess);
  }
  // denoise on.
  {
    TaskCodec c;
    TASK_CHECK (c.init (0, 1, false, RC_OFF_MODE, true, false) == 0);
    std::vector<unsigned char> map (TASK_MB_NUM, 1);
    TASK_CHECK (TaskApplyMap (c.enc, &map) != cmResultSuccess);
  }
  // frame skip on.
  {
    TaskCodec c;
    TASK_CHECK (c.init (0, 1, false, RC_OFF_MODE, false, true) == 0);
    std::vector<unsigned char> map (TASK_MB_NUM, 1);
    TASK_CHECK (TaskApplyMap (c.enc, &map) != cmResultSuccess);
  }
  // dynamic byte limited slicing.
  {
    TaskCodec c;
    TASK_CHECK (c.init (0, 1, false, RC_OFF_MODE, false, false, SM_SIZELIMITED_SLICE) == 0);
    std::vector<unsigned char> map (TASK_MB_NUM, 1);
    TASK_CHECK (TaskApplyMap (c.enc, &map) != cmResultSuccess);
  }
  return 0;
}

static int TaskCaseBadGeometry (void) {
  // A 16-aligned non-square picture accepts a correctly sized grid.
  {
    TaskCodec c;
    const int iW = 64;
    const int iH = 80; // 4 x 5 MB grid
    TASK_CHECK (c.init (0, 1, true, RC_OFF_MODE, false, false, SM_SINGLE_SLICE, iW, iH) == 0);
    std::vector<unsigned char> map ((iW / 16) * (iH / 16), 1);
    SPcmMbMap m;
    std::memset (&m, 0, sizeof (m));
    m.pMap = map.data();
    m.iMbWidth = iW / 16;
    m.iMbHeight = iH / 16;
    m.iStride = m.iMbWidth;
    TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &m) == cmResultSuccess);

    // Grid height off by one is rejected.
    SPcmMbMap bad = m;
    bad.iMbHeight = m.iMbHeight + 1;
    TASK_CHECK (c.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &bad) != cmResultSuccess);
  }

  // A picture whose height is not a multiple of 16 must reject a non-empty
  // map even if the encoder accepts and pads the resolution itself.
  TaskCodec c2;
  const int iW = 64;
  const int iHbad = 72;
  int iInit = c2.init (0, 1, true, RC_OFF_MODE, false, false, SM_SINGLE_SLICE, iW, iHbad);
  if (iInit == 0) {
    SPcmMbMap m;
    std::memset (&m, 0, sizeof (m));
    std::vector<unsigned char> big (16 * 16, 1);
    m.pMap = big.data();
    m.iMbWidth = iW / 16;
    m.iMbHeight = (iHbad + 15) / 16;
    m.iStride = m.iMbWidth;
    TASK_CHECK (c2.enc->SetOption (ENCODER_OPTION_PCM_MB_MAP, &m) != cmResultSuccess);
  }
  // If the encoder itself rejects 72p initialization, that is also an
  // acceptable outcome; either way this check executed.
  TASK_CHECK (iInit == 0 || iInit == 3);
  return 0;
}

/* ------------------------------- group entries --------------------------- */

static int TaskSynCavlcSingle()    { return TaskCaseMixedSyntax (0, 1); }
static int TaskSynCabacSingle()    { return TaskCaseMixedSyntax (1, 1); }
static int TaskSynCavlcSlices4()   { return TaskCaseMixedSyntax (0, 4); }
static int TaskSynCabacSlices4()   { return TaskCaseMixedSyntax (1, 4); }
static int TaskRefCavlcDrift()     { return TaskCaseReferenceDrift (0); }
static int TaskRefCabacDrift()     { return TaskCaseReferenceDrift (1); }
static int TaskRefChangingCavlc()  { return TaskCaseChangingMaps (0); }
static int TaskRefChangingCabac()  { return TaskCaseChangingMaps (1); }
static int TaskOwnCopyStride()     { return TaskCaseCopyAndStride(); }
static int TaskOwnBadKeeps()       { return TaskCaseBadMapKeepsActive(); }
static int TaskOwnTwoEncoders()    { return TaskCaseTwoEncoders(); }
static int TaskOwnReinit()         { return TaskCaseReinitClearsMap(); }
static int TaskOwnUnsupported()    { return TaskCaseUnsupportedConfig(); }
static int TaskOwnBadGeometry()    { return TaskCaseBadGeometry(); }

} // namespace

// check.h declares the registry with ordinary C++ linkage; driver.c is built
// as C++ too, so no extern "C" here.
const struct task_case task_cases[] = {
  {"syntax",    "cavlc_single",       TaskSynCavlcSingle},
  {"syntax",    "cabac_single",       TaskSynCabacSingle},
  {"syntax",    "cavlc_fixed4slice",  TaskSynCavlcSlices4},
  {"syntax",    "cabac_fixed4slice",  TaskSynCabacSlices4},
  {"reference", "cavlc_static_drift", TaskRefCavlcDrift},
  {"reference", "cabac_static_drift", TaskRefCabacDrift},
  {"reference", "cavlc_changing_map", TaskRefChangingCavlc},
  {"reference", "cabac_changing_map", TaskRefChangingCabac},
  {"ownership", "copy_and_stride",    TaskOwnCopyStride},
  {"ownership", "bad_args_keep_map",  TaskOwnBadKeeps},
  {"ownership", "two_encoders",       TaskOwnTwoEncoders},
  {"ownership", "reinit_clears",      TaskOwnReinit},
  {"ownership", "unsupported_config", TaskOwnUnsupported},
  {"ownership", "bad_geometry",       TaskOwnBadGeometry},
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
