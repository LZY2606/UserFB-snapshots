/*!
 * \copy
 *     Copyright (c)  2026, Cisco Systems
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
 * \file    frame_lease.cpp
 *
 * \brief   Verification cases for the Wels*DecodedFrame lease API. Every case
 *          drives the real encoder/decoder and returns 1 only after all its
 *          assertions pass; there are no skip branches.
 *
 *************************************************************************************
 */
#include "codec/api/wels/codec_api.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "verification/driver.h"

namespace {

#define CHECK(cond) do { \
  if (!(cond)) { \
    std::fprintf (stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
    return 0; \
  } \
} while (0)

// FNV-1a over every visible sample of an I420 picture. Strides may exceed the
// visible width, so rows are hashed separately through their stride.
uint64_t HashPicture (const unsigned char* const pData[3], const int iStride[3], int iWidth, int iHeight) {
  uint64_t uHash = 1469598103934665603ULL;
  auto mix = [&uHash] (const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      uHash ^= static_cast<uint64_t> (p[i]);
      uHash *= 1099511628211ULL;
    }
  };
  for (int y = 0; y < iHeight; ++y)
    mix (pData[0] + y * iStride[0], iWidth);
  for (int plane = 1; plane <= 2; ++plane) {
    for (int y = 0; y < iHeight / 2; ++y)
      mix (pData[plane] + y * iStride[plane], iWidth / 2);
  }
  return uHash;
}

struct Snapshot {
  SWelsDecodedFrame* pFrame;
  uint64_t          uHash;
  const unsigned char* pAddr[3];
  int               iStride[3];
  int               iWidth;
  int               iHeight;
  unsigned long long uiTimeStamp;
};

void FillPattern (std::vector<unsigned char>& rImage, int iWidth, int iHeight, int n) {
  rImage.resize (iWidth * iHeight * 3 / 2);
  for (int y = 0; y < iHeight; ++y)
    for (int x = 0; x < iWidth; ++x)
      rImage[y * iWidth + x] = static_cast<unsigned char> ((x * 3 + y * 5 + n * 19) % 220 + 16);
  unsigned char* pU = rImage.data() + iWidth * iHeight;
  unsigned char* pV = pU + (iWidth * iHeight / 4);
  for (int i = 0; i < iWidth * iHeight / 4; ++i) {
    pU[i] = static_cast<unsigned char> (64 + (n * 7 + i * 11) % 128);
    pV[i] = static_cast<unsigned char> (32 + (n * 13 + i * 17) % 192);
  }
}

ISVCEncoder* CreateEncoder (int iWidth, int iHeight, EProfileIdc eProfile = PRO_BASELINE) {
  ISVCEncoder* pEncoder = NULL;
  if (WelsCreateSVCEncoder (&pEncoder) != 0 || pEncoder == NULL)
    return NULL;
  SEncParamBase sParam;
  memset (&sParam, 0, sizeof (sParam));
  sParam.iUsageType = CAMERA_VIDEO_REAL_TIME;
  sParam.fMaxFrameRate = 25;
  sParam.iPicWidth = iWidth;
  sParam.iPicHeight = iHeight;
  sParam.iTargetBitrate = 300000;
  sParam.iRCMode = RC_OFF_MODE;
  if (pEncoder->Initialize (&sParam) != 0) {
    pEncoder->Uninitialize();
    WelsDestroySVCEncoder (pEncoder);
    return NULL;
  }
  SProfileInfo sProfile;
  memset (&sProfile, 0, sizeof (sProfile));
  sProfile.iLayer = 0;
  sProfile.uiProfileIdc = eProfile;
  if (pEncoder->SetOption (ENCODER_OPTION_PROFILE, &sProfile) != 0) {
    pEncoder->Uninitialize();
    WelsDestroySVCEncoder (pEncoder);
    return NULL;
  }
  return pEncoder;
}

void DestroyEncoder (ISVCEncoder* pEncoder) {
  if (pEncoder != NULL) {
    pEncoder->Uninitialize();
    WelsDestroySVCEncoder (pEncoder);
  }
}

ISVCDecoder* CreateDecoder (int iThreadCount = 0) {
  ISVCDecoder* pDecoder = NULL;
  if (WelsCreateDecoder (&pDecoder) != 0 || pDecoder == NULL)
    return NULL;
  SDecodingParam sDecParam;
  memset (&sDecParam, 0, sizeof (sDecParam));
  sDecParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  sDecParam.eEcActiveIdc = ERROR_CON_DISABLE;
  if (pDecoder->Initialize (&sDecParam) != 0) {
    WelsDestroyDecoder (pDecoder);
    return NULL;
  }
  if (iThreadCount > 0)
    pDecoder->SetOption (DECODER_OPTION_NUM_OF_THREADS, &iThreadCount);
  return pDecoder;
}

// Encode one I420 source picture into a self-contained Annex-B access unit.
std::vector<unsigned char> EncodeOne (ISVCEncoder* pEncoder,
                                      std::vector<unsigned char>& rImage,
                                      int iWidth, int iHeight,
                                      unsigned long long uiTimeStamp) {
  SSourcePicture sPic;
  memset (&sPic, 0, sizeof (sPic));
  sPic.iColorFormat = videoFormatI420;
  sPic.iPicWidth = iWidth;
  sPic.iPicHeight = iHeight;
  sPic.iStride[0] = iWidth;
  sPic.iStride[1] = sPic.iStride[2] = iWidth / 2;
  sPic.pData[0] = rImage.data();
  sPic.pData[1] = rImage.data() + iWidth * iHeight;
  sPic.pData[2] = sPic.pData[1] + iWidth * iHeight / 4;
  sPic.uiTimeStamp = static_cast<long long> (uiTimeStamp);

  SFrameBSInfo sBsInfo;
  memset (&sBsInfo, 0, sizeof (sBsInfo));
  if (pEncoder->EncodeFrame (&sPic, &sBsInfo) != 0)
    return std::vector<unsigned char>();
  std::vector<unsigned char> rAu;
  for (int l = 0; l < sBsInfo.iLayerNum; ++l) {
    const SLayerBSInfo& rsLayer = sBsInfo.sLayerInfo[l];
    int iLen = 0;
    for (int k = 0; k < rsLayer.iNalCount; ++k)
      iLen += rsLayer.pNalLengthInByte[k];
    rAu.insert (rAu.end(), rsLayer.pBsBuf, rsLayer.pBsBuf + iLen);
  }
  return rAu;
}

bool DecodeNoDelay (ISVCDecoder* pDecoder, const std::vector<unsigned char>& rAu,
                    unsigned long long uiInTimeStamp, unsigned char* pDst[3], SBufferInfo* pInfo) {
  memset (pInfo, 0, sizeof (*pInfo));
  pInfo->uiInBsTimeStamp = uiInTimeStamp;
  DECODING_STATE eState = pDecoder->DecodeFrameNoDelay (rAu.empty() ? NULL : rAu.data(),
                          static_cast<int> (rAu.size()), pDst, pInfo);
  return eState == dsErrorFree;
}

Snapshot MakeSnapshot (ISVCDecoder* pDecoder, const unsigned char* pDst[3], const SBufferInfo& rsInfo) {
  Snapshot s;
  memset (&s, 0, sizeof (s));
  if (WelsAcquireDecodedFrame (pDecoder, &s.pFrame) != 0 || s.pFrame == NULL)
    return s;
  SWelsDecodedFrameInfo sFrameInfo;
  memset (&sFrameInfo, 0, sizeof (sFrameInfo));
  if (WelsGetDecodedFrameInfo (s.pFrame, &sFrameInfo) != 0) {
    WelsReleaseDecodedFrame (s.pFrame);
    s.pFrame = NULL;
    return s;
  }
  for (int i = 0; i < 3; ++i) {
    s.pAddr[i] = sFrameInfo.pData[i];
    s.iStride[i] = sFrameInfo.iStride[i];
  }
  s.iWidth = sFrameInfo.iWidth;
  s.iHeight = sFrameInfo.iHeight;
  s.uiTimeStamp = sFrameInfo.uiTimeStamp;
  s.uHash = HashPicture (sFrameInfo.pData, sFrameInfo.iStride, s.iWidth, s.iHeight);

  // The leased planes must alias exactly the addresses the decode returned.
  for (int i = 0; i < 3; ++i) {
    if (s.pAddr[i] != pDst[i]) {
      WelsReleaseDecodedFrame (s.pFrame);
      s.pFrame = NULL;
      return s;
    }
  }
  if (static_cast<int> (rsInfo.UsrData.sSystemBuffer.iWidth) != s.iWidth
      || static_cast<int> (rsInfo.UsrData.sSystemBuffer.iHeight) != s.iHeight
      || rsInfo.uiOutYuvTimeStamp != s.uiTimeStamp) {
    WelsReleaseDecodedFrame (s.pFrame);
    s.pFrame = NULL;
  }
  return s;
}

bool SnapshotIntact (const Snapshot& rs) {
  if (rs.pFrame == NULL)
    return false;
  SWelsDecodedFrameInfo sInfo;
  memset (&sInfo, 0, sizeof (sInfo));
  if (WelsGetDecodedFrameInfo (rs.pFrame, &sInfo) != 0)
    return false;
  for (int i = 0; i < 3; ++i) {
    if (sInfo.pData[i] != rs.pAddr[i] || sInfo.iStride[i] != rs.iStride[i])
      return false;
  }
  if (sInfo.iWidth != rs.iWidth || sInfo.iHeight != rs.iHeight
      || sInfo.uiTimeStamp != rs.uiTimeStamp)
    return false;
  return HashPicture (sInfo.pData, sInfo.iStride, sInfo.iWidth, sInfo.iHeight) == rs.uHash;
}

void ReleaseSnapshot (Snapshot& rs) {
  if (rs.pFrame != NULL) {
    WelsReleaseDecodedFrame (rs.pFrame);
    rs.pFrame = NULL;
  }
}

// Case 1: leased pictures are never recycled as decode targets while held,
// and the pool still recycles storage for the unleased long tail.
int PinReuse() {
  const int kiWidth = 64;
  const int kiHeight = 48;
  const int kiFrames = 45;
  ISVCEncoder* pEncoder = CreateEncoder (kiWidth, kiHeight);
  CHECK (pEncoder != NULL);
  ISVCDecoder* pDecoder = CreateDecoder (0);
  CHECK (pDecoder != NULL);

  std::vector<unsigned char> sImage;
  std::vector<Snapshot> sHeld;
  std::vector<const unsigned char*> sLumaAddrs;
  int iDelivered = 0;

  for (int n = 0; n < kiFrames; ++n) {
    FillPattern (sImage, kiWidth, kiHeight, n);
    std::vector<unsigned char> sAu = EncodeOne (pEncoder, sImage, kiWidth, kiHeight, n * 40ULL);
    CHECK (!sAu.empty());
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    CHECK (DecodeNoDelay (pDecoder, sAu, n * 40ULL, pDst, &sInfo));
    if (sInfo.iBufferStatus != 1)
      continue;
    ++iDelivered;
    sLumaAddrs.push_back (pDst[0]);
    if (n == 0 || n == 1 || n == 2 || n == 3) {
      Snapshot sSnap = MakeSnapshot (pDecoder, const_cast<const unsigned char**> (pDst), sInfo);
      CHECK (sSnap.pFrame != NULL);
      sHeld.push_back (sSnap);
    }
  }
  CHECK (iDelivered >= 40);
  CHECK (sHeld.size() == 4);

  // Repeated Acquire does not consume the delivery: two independent handles.
  SWelsDecodedFrame* pA = NULL;
  SWelsDecodedFrame* pB = NULL;
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pA) == 0 && pA != NULL);
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pB) == 0 && pB != NULL);
  SWelsDecodedFrameInfo sInfoA, sInfoB;
  CHECK (WelsGetDecodedFrameInfo (pA, &sInfoA) == 0);
  CHECK (WelsGetDecodedFrameInfo (pB, &sInfoB) == 0);
  CHECK (sInfoA.pData[0] == sInfoB.pData[0]);
  CHECK (WelsRetainDecodedFrame (pA) == 0);
  WelsReleaseDecodedFrame (pA);
  CHECK (SnapshotIntact (sHeld[0])); // remaining reference still valid
  WelsReleaseDecodedFrame (pB);

  // Every held address must stay unique: pool recycling never overwrote it.
  for (size_t h = 0; h < sHeld.size(); ++h) {
    CHECK (SnapshotIntact (sHeld[h]));
    int iSeenAfterHold = 0;
    for (size_t i = 4; i < sLumaAddrs.size(); ++i)
      if (sLumaAddrs[i] == sHeld[h].pAddr[0])
        ++iSeenAfterHold;
    CHECK (iSeenAfterHold == 0);
  }

  // Without leases the finite pool visibly recycles storage.
  size_t iDistinct = 0;
  for (size_t i = 0; i < sLumaAddrs.size(); ++i) {
    bool bSeen = false;
    for (size_t j = 0; j < i; ++j)
      bSeen = bSeen || sLumaAddrs[j] == sLumaAddrs[i];
    if (!bSeen)
      ++iDistinct;
  }
  CHECK (iDistinct < sLumaAddrs.size());

  for (size_t h = 0; h < sHeld.size(); ++h)
    ReleaseSnapshot (sHeld[h]);
  pDecoder->Uninitialize();
  WelsDestroyDecoder (pDecoder);
  DestroyEncoder (pEncoder);
  return 1;
}

// Case 2: leases survive an in-stream resolution switch and Uninitialize.
int Resolution() {
  const int kiW0 = 64;
  const int kiH0 = 48;
  const int kiW1 = 80;
  const int kiH1 = 60;
  ISVCEncoder* pEncoder = CreateEncoder (kiW0, kiH0);
  CHECK (pEncoder != NULL);
  ISVCDecoder* pDecoder = CreateDecoder (0);
  CHECK (pDecoder != NULL);

  std::vector<unsigned char> sImage;
  std::vector<Snapshot> sHeld;
  for (int n = 0; n < 16; ++n) {
    FillPattern (sImage, kiW0, kiH0, n);
    std::vector<unsigned char> sAu = EncodeOne (pEncoder, sImage, kiW0, kiH0, n * 40ULL);
    CHECK (!sAu.empty());
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    CHECK (DecodeNoDelay (pDecoder, sAu, n * 40ULL, pDst, &sInfo));
    if (sInfo.iBufferStatus == 1 && (n == 2 || n == 5)) {
      Snapshot sSnap = MakeSnapshot (pDecoder, const_cast<const unsigned char**> (pDst), sInfo);
      CHECK (sSnap.pFrame != NULL);
      CHECK (sSnap.iWidth == kiW0 && sSnap.iHeight == kiH0);
      sHeld.push_back (sSnap);
    }
  }
  CHECK (sHeld.size() == 2);

  DestroyEncoder (pEncoder);
  pEncoder = CreateEncoder (kiW1, kiH1);
  CHECK (pEncoder != NULL);
  Snapshot sNewSnap;
  memset (&sNewSnap, 0, sizeof (sNewSnap));
  for (int n = 0; n < 20; ++n) {
    FillPattern (sImage, kiW1, kiH1, 100 + n);
    std::vector<unsigned char> sAu = EncodeOne (pEncoder, sImage, kiW1, kiH1, 1000ULL + n * 40ULL);
    CHECK (!sAu.empty());
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    CHECK (DecodeNoDelay (pDecoder, sAu, 1000ULL + n * 40ULL, pDst, &sInfo));
    if (sInfo.iBufferStatus == 1) {
      CHECK (sInfo.UsrData.sSystemBuffer.iWidth == kiW1);
      CHECK (sInfo.UsrData.sSystemBuffer.iHeight == kiH1);
      if (n == 19)
        sNewSnap = MakeSnapshot (pDecoder, const_cast<const unsigned char**> (pDst), sInfo);
    }
  }
  CHECK (sNewSnap.pFrame != NULL);
  CHECK (sNewSnap.iWidth == kiW1 && sNewSnap.iHeight == kiH1);

  // Old-resolution storage survives the DPB rebuild for the new sequence.
  for (size_t h = 0; h < sHeld.size(); ++h) {
    CHECK (SnapshotIntact (sHeld[h]));
    CHECK (sHeld[h].iWidth == kiW0 && sHeld[h].iHeight == kiH0);
  }
  CHECK (SnapshotIntact (sNewSnap));

  // Uninitialize must not reclaim leased storage either.
  pDecoder->Uninitialize();
  for (size_t h = 0; h < sHeld.size(); ++h)
    CHECK (SnapshotIntact (sHeld[h]));
  CHECK (SnapshotIntact (sNewSnap));

  ReleaseSnapshot (sNewSnap);
  for (size_t h = 0; h < sHeld.size(); ++h)
    ReleaseSnapshot (sHeld[h]);
  WelsDestroyDecoder (pDecoder);
  DestroyEncoder (pEncoder);

  // A second decoder: acquire, then destroy the owner and read afterwards.
  ISVCEncoder* pEncoder2 = CreateEncoder (kiW0, kiH0);
  CHECK (pEncoder2 != NULL);
  ISVCDecoder* pDecoder2 = CreateDecoder (0);
  CHECK (pDecoder2 != NULL);
  std::vector<Snapshot> sAfterDestroy;
  for (int n = 0; n < 8; ++n) {
    FillPattern (sImage, kiW0, kiH0, 200 + n);
    std::vector<unsigned char> sAu = EncodeOne (pEncoder2, sImage, kiW0, kiH0, n * 40ULL);
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    CHECK (DecodeNoDelay (pDecoder2, sAu, n * 40ULL, pDst, &sInfo));
    if (sInfo.iBufferStatus == 1 && (n == 1 || n == 4)) {
      Snapshot sSnap = MakeSnapshot (pDecoder2, const_cast<const unsigned char**> (pDst), sInfo);
      CHECK (sSnap.pFrame != NULL);
      sAfterDestroy.push_back (sSnap);
    }
  }
  CHECK (sAfterDestroy.size() == 2);
  WelsDestroyDecoder (pDecoder2);
  for (size_t h = 0; h < sAfterDestroy.size(); ++h)
    CHECK (SnapshotIntact (sAfterDestroy[h]));
  for (size_t h = 0; h < sAfterDestroy.size(); ++h)
    ReleaseSnapshot (sAfterDestroy[h]);
  WelsReleaseDecodedFrame (NULL); // must be a safe no-op
  DestroyEncoder (pEncoder2);
  return 1;
}

// Case 3: handles outlive WelsDestroyDecoder, including extra retained refs.
int DestroyOwner() {
  const int kiWidth = 64;
  const int kiHeight = 48;
  ISVCEncoder* pEncoder = CreateEncoder (kiWidth, kiHeight);
  CHECK (pEncoder != NULL);
  ISVCDecoder* pDecoder = CreateDecoder (0);
  CHECK (pDecoder != NULL);

  std::vector<unsigned char> sImage;
  std::vector<Snapshot> sHeld;
  SWelsDecodedFrame* pExtra1 = NULL;
  SWelsDecodedFrame* pExtra2 = NULL;
  for (int n = 0; n < 12; ++n) {
    FillPattern (sImage, kiWidth, kiHeight, n);
    std::vector<unsigned char> sAu = EncodeOne (pEncoder, sImage, kiWidth, kiHeight, n * 40ULL);
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    CHECK (DecodeNoDelay (pDecoder, sAu, n * 40ULL, pDst, &sInfo));
    if (sInfo.iBufferStatus == 1 && (n == 2 || n == 6 || n == 10)) {
      Snapshot sSnap = MakeSnapshot (pDecoder, const_cast<const unsigned char**> (pDst), sInfo);
      CHECK (sSnap.pFrame != NULL);
      sHeld.push_back (sSnap);
      if (n == 2) {
        // Independent handles to the same eligible picture; repeated Acquire
        // must not consume the delivery.
        CHECK (WelsAcquireDecodedFrame (pDecoder, &pExtra1) == 0 && pExtra1 != NULL);
        CHECK (WelsAcquireDecodedFrame (pDecoder, &pExtra2) == 0 && pExtra2 != NULL);
        SWelsDecodedFrameInfo sE1, sE2;
        CHECK (WelsGetDecodedFrameInfo (pExtra1, &sE1) == 0);
        CHECK (WelsGetDecodedFrameInfo (pExtra2, &sE2) == 0);
        CHECK (sE1.pData[0] == sSnap.pAddr[0] && sE2.pData[0] == sSnap.pAddr[0]);
      }
    }
  }
  CHECK (sHeld.size() == 3);

  WelsDestroyDecoder (pDecoder);
  DestroyEncoder (pEncoder);

  // Every handle stays readable after the owner is gone.
  for (size_t h = 0; h < sHeld.size(); ++h)
    CHECK (SnapshotIntact (sHeld[h]));
  SWelsDecodedFrameInfo sTmp;
  CHECK (WelsGetDecodedFrameInfo (pExtra1, &sTmp) == 0 && sTmp.pData[0] == sHeld[0].pAddr[0]);
  CHECK (WelsGetDecodedFrameInfo (pExtra2, &sTmp) == 0 && sTmp.pData[0] == sHeld[0].pAddr[0]);

  // Interleaved, out-of-order releases; the still-held anchor remains valid.
  WelsReleaseDecodedFrame (pExtra1);
  CHECK (SnapshotIntact (sHeld[0]));
  WelsReleaseDecodedFrame (pExtra2);
  CHECK (SnapshotIntact (sHeld[0]));
  WelsReleaseDecodedFrame (sHeld[2].pFrame); // different picture, out of order
  CHECK (SnapshotIntact (sHeld[1]));
  CHECK (SnapshotIntact (sHeld[0]));
  WelsReleaseDecodedFrame (sHeld[1].pFrame);
  CHECK (SnapshotIntact (sHeld[0]));
  WelsReleaseDecodedFrame (sHeld[0].pFrame); // last reference, after destroy
  for (size_t h = 0; h < sHeld.size(); ++h)
    sHeld[h].pFrame = NULL;
  return 1;
}

struct DrainItem {
  SWelsDecodedFrame* pFrame;
  uint64_t uHash;
  int iWidth;
  int iHeight;
};

// Case 4: cross-thread Retain/Release while decoding continues, two decoder
// threads, plus a real FlushFrame() delivery through the reorder buffer.
int DrainThread() {
  const int kiWidth = 64;
  const int kiHeight = 48;
  const int kiFrames = 42;
  ISVCEncoder* pEncoder = CreateEncoder (kiWidth, kiHeight);
  CHECK (pEncoder != NULL);
  ISVCDecoder* pDecoder = CreateDecoder (2);
  CHECK (pDecoder != NULL);

  std::mutex sMutex;
  std::condition_variable sCv;
  std::queue<DrainItem> sQueue;
  std::atomic<int> iChecked (0);
  std::atomic<bool> bProduceDone (false);
  std::atomic<bool> bWorkerFailed (false);

  std::thread sWorker ([&] () {
    for (;;) {
      DrainItem sItem;
      {
        std::unique_lock<std::mutex> sLock (sMutex);
        sCv.wait (sLock, [&] { return !sQueue.empty() || bProduceDone.load(); });
        if (sQueue.empty())
          return;
        sItem = sQueue.front();
        sQueue.pop();
      }
      // Independent downstream reference on another thread.
      if (WelsRetainDecodedFrame (sItem.pFrame) != 0) {
        bWorkerFailed = true;
        WelsReleaseDecodedFrame (sItem.pFrame);
        continue;
      }
      SWelsDecodedFrameInfo sInfo;
      if (WelsGetDecodedFrameInfo (sItem.pFrame, &sInfo) != 0
          || HashPicture (sInfo.pData, sInfo.iStride, sItem.iWidth, sItem.iHeight) != sItem.uHash)
        bWorkerFailed = true;
      WelsReleaseDecodedFrame (sItem.pFrame); // worker's retain
      WelsReleaseDecodedFrame (sItem.pFrame); // original reference handed over
      ++iChecked;
    }
  });

  std::vector<unsigned char> sImage;
  int iProduced = 0;
  for (int n = 0; n < kiFrames; ++n) {
    FillPattern (sImage, kiWidth, kiHeight, n);
    std::vector<unsigned char> sAu = EncodeOne (pEncoder, sImage, kiWidth, kiHeight, n * 40ULL);
    CHECK (!sAu.empty());
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    CHECK (DecodeNoDelay (pDecoder, sAu, n * 40ULL, pDst, &sInfo));
    if (sInfo.iBufferStatus != 1)
      continue;
    SWelsDecodedFrame* pFrame = NULL;
    CHECK (WelsAcquireDecodedFrame (pDecoder, &pFrame) == 0 && pFrame != NULL);
    SWelsDecodedFrameInfo sFrameInfo;
    CHECK (WelsGetDecodedFrameInfo (pFrame, &sFrameInfo) == 0);
    DrainItem sItem;
    sItem.pFrame = pFrame;
    sItem.uHash = HashPicture (sFrameInfo.pData, sFrameInfo.iStride, sFrameInfo.iWidth, sFrameInfo.iHeight);
    sItem.iWidth = sFrameInfo.iWidth;
    sItem.iHeight = sFrameInfo.iHeight;
    {
      std::lock_guard<std::mutex> sLock (sMutex);
      sQueue.push (sItem);
    }
    sCv.notify_one();
    ++iProduced;
  }
  CHECK (iProduced >= 40);
  {
    std::lock_guard<std::mutex> sLock (sMutex);
    bProduceDone = true;
  }
  sCv.notify_one();
  sWorker.join();
  CHECK (iChecked.load() == iProduced);
  CHECK (!bWorkerFailed.load());

  // Flush path on a fresh two-thread decoder fed by a fresh encoder so the
  // first access unit carries SPS/PPS. DecodeFrame2() leaves decoded pictures
  // in the reorder queue (DecodeFrameNoDelay would drain them), so signaling
  // EOS and calling FlushFrame() must deliver a queued picture. This also
  // exercises lease eligibility for a FlushFrame() (not decode) output.
  ISVCEncoder* pFlushEncoder = CreateEncoder (kiWidth, kiHeight);
  CHECK (pFlushEncoder != NULL);
  ISVCDecoder* pFlushDecoder = CreateDecoder (2);
  CHECK (pFlushDecoder != NULL);
  for (int n = 0; n < 8; ++n) {
    FillPattern (sImage, kiWidth, kiHeight, 500 + n);
    std::vector<unsigned char> sAu = EncodeOne (pFlushEncoder, sImage, kiWidth, kiHeight, 5000ULL + n * 40ULL);
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    memset (&sInfo, 0, sizeof (sInfo));
    sInfo.uiInBsTimeStamp = 5000ULL + n * 40ULL;
    CHECK (pFlushDecoder->DecodeFrame2 (sAu.data(), static_cast<int> (sAu.size()), pDst, &sInfo) == dsErrorFree);
  }
  int iRemaining = 0;
  CHECK (pFlushDecoder->GetOption (DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &iRemaining) == cmResultSuccess);
  // In threaded mode DecodeFrame2() keeps pictures queued; a single-threaded
  // baseline decoder may have drained everything already, so a zero remainder
  // is tolerated there. Either way every FlushFrame() delivery is leased.
  int iEos = 1;
  CHECK (pFlushDecoder->SetOption (DECODER_OPTION_END_OF_STREAM, &iEos) == cmResultSuccess);
  Snapshot sFlushedSnap;
  memset (&sFlushedSnap, 0, sizeof (sFlushedSnap));
  int iFlushCount = (iRemaining > 0) ? iRemaining : 1;
  for (int i = 0; i < iFlushCount; ++i) {
    unsigned char* pFlushDst[3] = {NULL, NULL, NULL};
    SBufferInfo sFlushInfo;
    memset (&sFlushInfo, 0, sizeof (sFlushInfo));
    CHECK (pFlushDecoder->FlushFrame (pFlushDst, &sFlushInfo) == dsErrorFree);
    if (sFlushInfo.iBufferStatus != 1)
      continue;
    Snapshot sSnap = MakeSnapshot (pFlushDecoder, const_cast<const unsigned char**> (pFlushDst), sFlushInfo);
    CHECK (sSnap.pFrame != NULL);
    CHECK (SnapshotIntact (sSnap));
    sFlushedSnap = sSnap; // keep the last flushed picture leased until teardown
  }
  if (iRemaining > 0)
    CHECK (sFlushedSnap.pFrame != NULL);

  pFlushDecoder->Uninitialize();
  // The flushed lease must survive decoder teardown.
  if (sFlushedSnap.pFrame != NULL) {
    CHECK (SnapshotIntact (sFlushedSnap));
    WelsDestroyDecoder (pFlushDecoder);
    CHECK (SnapshotIntact (sFlushedSnap));
    ReleaseSnapshot (sFlushedSnap);
  } else {
    WelsDestroyDecoder (pFlushDecoder);
  }
  pDecoder->Uninitialize();
  WelsDestroyDecoder (pDecoder);
  DestroyEncoder (pFlushEncoder);
  DestroyEncoder (pEncoder);
  return 1;
}

// Case 5: no eligible picture after empty calls, before init output, or in
// parse-only mode; failed Acquire always NULLs the output handle.
int EmptyOutput() {
  const int kiWidth = 64;
  const int kiHeight = 48;
  ISVCDecoder* pDecoder = CreateDecoder (0);
  CHECK (pDecoder != NULL);
  SWelsDecodedFrame* pFrame = reinterpret_cast<SWelsDecodedFrame*> (0x1);
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pFrame) != 0);
  CHECK (pFrame == NULL);
  CHECK (WelsAcquireDecodedFrame (NULL, &pFrame) != 0);
  CHECK (pFrame == NULL);
  CHECK (WelsAcquireDecodedFrame (pDecoder, NULL) != 0);

  // Empty flush with nothing buffered: no output, no leasable picture.
  unsigned char* pDst[3] = {NULL, NULL, NULL};
  SBufferInfo sInfo;
  memset (&sInfo, 0, sizeof (sInfo));
  CHECK (pDecoder->FlushFrame (pDst, &sInfo) == dsErrorFree);
  CHECK (sInfo.iBufferStatus == 0);
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pFrame) != 0 && pFrame == NULL);

  ISVCEncoder* pEncoder = CreateEncoder (kiWidth, kiHeight);
  CHECK (pEncoder != NULL);
  std::vector<unsigned char> sImage;
  FillPattern (sImage, kiWidth, kiHeight, 0);
  std::vector<unsigned char> sAu = EncodeOne (pEncoder, sImage, kiWidth, kiHeight, 40ULL);
  CHECK (!sAu.empty());
  CHECK (DecodeNoDelay (pDecoder, sAu, 40ULL, pDst, &sInfo));
  CHECK (sInfo.iBufferStatus == 1);
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pFrame) == 0 && pFrame != NULL);
  SWelsDecodedFrameInfo sFrameInfo;
  CHECK (WelsGetDecodedFrameInfo (pFrame, &sFrameInfo) == 0);
  CHECK (sFrameInfo.pData[0] != NULL && sFrameInfo.iWidth == kiWidth);
  WelsReleaseDecodedFrame (pFrame);

  // Another empty flush must not revive the previously delivered picture.
  memset (&sInfo, 0, sizeof (sInfo));
  CHECK (pDecoder->FlushFrame (pDst, &sInfo) == dsErrorFree);
  CHECK (sInfo.iBufferStatus == 0);
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pFrame) != 0 && pFrame == NULL);
  CHECK (WelsGetDecodedFrameInfo (NULL, &sFrameInfo) != 0);
  CHECK (WelsRetainDecodedFrame (NULL) != 0);

  pDecoder->Uninitialize();
  WelsDestroyDecoder (pDecoder);

  // Parse-only decoder never produces leasable reconstruction pictures.
  ISVCDecoder* pParseDecoder = NULL;
  CHECK (WelsCreateDecoder (&pParseDecoder) == 0 && pParseDecoder != NULL);
  SDecodingParam sParseParam;
  memset (&sParseParam, 0, sizeof (sParseParam));
  sParseParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  sParseParam.bParseOnly = true;
  CHECK (pParseDecoder->Initialize (&sParseParam) == 0);
  SParserBsInfo sParseInfo;
  memset (&sParseInfo, 0, sizeof (sParseInfo));
  sParseInfo.pDstBuff = static_cast<unsigned char*> (malloc (1024 * 1024));
  CHECK (sParseInfo.pDstBuff != NULL);
  CHECK (pParseDecoder->DecodeParser (sAu.data(), static_cast<int> (sAu.size()), &sParseInfo) == dsErrorFree);
  CHECK (WelsAcquireDecodedFrame (pParseDecoder, &pFrame) != 0 && pFrame == NULL);
  free (sParseInfo.pDstBuff);
  pParseDecoder->Uninitialize();
  WelsDestroyDecoder (pParseDecoder);
  DestroyEncoder (pEncoder);
  return 1;
}

// Case 6: same-timestamp distinct pictures, repeated Acquire of one picture,
// and interleaved Retain/Release ordering including a downstream thread.
int ReleaseOrder() {
  const int kiWidth = 64;
  const int kiHeight = 48;
  ISVCEncoder* pEncoder = CreateEncoder (kiWidth, kiHeight);
  CHECK (pEncoder != NULL);
  ISVCDecoder* pDecoder = CreateDecoder (0);
  CHECK (pDecoder != NULL);
  std::vector<unsigned char> sImage;

  const unsigned long long kuiSharedTs = 777ULL;
  Snapshot sSnap1, sSnap2;
  memset (&sSnap1, 0, sizeof (sSnap1));
  memset (&sSnap2, 0, sizeof (sSnap2));
  for (int n = 0; n < 4; ++n) {
    FillPattern (sImage, kiWidth, kiHeight, 400 + n);
    // Two distinct pictures intentionally carry the same output timestamp.
    std::vector<unsigned char> sAu = EncodeOne (pEncoder, sImage, kiWidth, kiHeight, kuiSharedTs);
    unsigned char* pDst[3] = {NULL, NULL, NULL};
    SBufferInfo sInfo;
    CHECK (DecodeNoDelay (pDecoder, sAu, kuiSharedTs, pDst, &sInfo));
    if (sInfo.iBufferStatus == 1 && (n == 0 || n == 1)) {
      Snapshot sSnap = MakeSnapshot (pDecoder, const_cast<const unsigned char**> (pDst), sInfo);
      CHECK (sSnap.pFrame != NULL);
      CHECK (sSnap.uiTimeStamp == kuiSharedTs);
      if (n == 0)
        sSnap1 = sSnap;
      else
        sSnap2 = sSnap;
    }
  }
  CHECK (sSnap1.pFrame != NULL && sSnap2.pFrame != NULL);
  CHECK (sSnap1.pAddr[0] != sSnap2.pAddr[0]);
  CHECK (sSnap1.uHash != sSnap2.uHash);

  // Repeated Acquire of the currently eligible picture yields independent
  // handles to the same storage without consuming eligibility.
  SWelsDecodedFrame* pX = NULL;
  SWelsDecodedFrame* pY = NULL;
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pX) == 0);
  CHECK (WelsAcquireDecodedFrame (pDecoder, &pY) == 0);
  SWelsDecodedFrameInfo sIx, sIy;
  CHECK (WelsGetDecodedFrameInfo (pX, &sIx) == 0 && WelsGetDecodedFrameInfo (pY, &sIy) == 0);
  CHECK (sIx.pData[0] == sIy.pData[0]);
  WelsReleaseDecodedFrame (pY);
  CHECK (WelsGetDecodedFrameInfo (pX, &sIx) == 0);
  WelsReleaseDecodedFrame (pX);

  // Build several references and release them from another thread in mixed
  // order; the picture must remain readable until its final release.
  CHECK (WelsRetainDecodedFrame (sSnap1.pFrame) == 0);
  CHECK (WelsRetainDecodedFrame (sSnap1.pFrame) == 0);
  SWelsDecodedFrame* pShared = sSnap1.pFrame;
  std::thread sWorker ([&] () {
    WelsReleaseDecodedFrame (pShared);
  });
  sWorker.join();
  CHECK (SnapshotIntact (sSnap1));
  WelsReleaseDecodedFrame (sSnap1.pFrame);
  CHECK (SnapshotIntact (sSnap1));
  WelsReleaseDecodedFrame (sSnap1.pFrame);
  sSnap1.pFrame = NULL;

  CHECK (SnapshotIntact (sSnap2));
  ReleaseSnapshot (sSnap2);
  pDecoder->Uninitialize();
  WelsDestroyDecoder (pDecoder);
  DestroyEncoder (pEncoder);
  return 1;
}

} // namespace

// External linkage: verification/driver_main.cpp references these symbols.
test_case cases[] = {
  {"pin-reuse", PinReuse},
  {"resolution", Resolution},
  {"destroy-owner", DestroyOwner},
  {"drain-thread", DrainThread},
  {"empty-output", EmptyOutput},
  {"release-order", ReleaseOrder},
};
size_t case_count = sizeof (cases) / sizeof (cases[0]);
