// Regression test for decoding user_data_unregistered SEI messages
// (H.264 SEI payload type 5) through the public OpenH264 API.
//
// The bitstreams are produced by the real encoder, SEI NAL units are generated
// and inserted by this file, and the result is decoded with one and two
// threads.  Output pictures are compared plane by plane against a control
// decode of the same stream with the SEI bytes removed.

#include "codec_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <thread>
#include <vector>
#include <string>

// Worker threads may still be completing the reconstructed picture when the
// caller-side API returns.  Give them a moment before copying planes so the
// comparison observes the finished picture.
static void SettleWorkerOutput (void) {
  std::this_thread::sleep_for (std::chrono::milliseconds (5));
}

static void Fail (const char* kpMessage) {
  std::fprintf (stderr, "FAIL: %s\n", kpMessage);
  std::exit (1);
}

static void Require (bool bCondition, const char* kpMessage) {
  if (! bCondition)
    Fail (kpMessage);
}

static void RequireState (long iState, bool bError, const char* kpMessage) {
  if (bError) {
    if ((iState & dsBitstreamError) == 0) {
      std::fprintf (stderr, "FAIL: %s (state=0x%lx, expected dsBitstreamError)\n", kpMessage, iState);
      std::exit (1);
    }
  } else if (iState != dsErrorFree) {
    std::fprintf (stderr, "FAIL: %s (state=0x%lx, expected dsErrorFree)\n", kpMessage, iState);
    std::exit (1);
  }
}

// ---------------------------------------------------------------------------
// Bitstream helpers
// ---------------------------------------------------------------------------

static size_t kNoPosition = (size_t) -1;

static size_t FindStartCode (const std::vector<unsigned char>& kData, size_t uiFrom) {
  for (size_t i = uiFrom; i + 2 < kData.size(); ++i) {
    if (kData[i] == 0 && kData[i + 1] == 0 && kData[i + 2] == 1)
      return i;
  }
  return kNoPosition;
}

// Position of the first start code that introduces a VCL NAL (types 1 and 5).
static size_t FindFirstVclStartCode (const std::vector<unsigned char>& kData) {
  size_t uiStart = FindStartCode (kData, 0);
  while (uiStart != kNoPosition) {
    const size_t uiHeader = uiStart + 3;
    if (uiHeader < kData.size()) {
      const unsigned int uiType = kData[uiHeader] & 0x1f;
      if (uiType == 1 || uiType == 5)
        return uiStart;
    }
    uiStart = FindStartCode (kData, uiStart + 3);
  }
  return kNoPosition;
}

static int CountVclNals (const std::vector<unsigned char>& kData) {
  int iCount = 0;
  size_t uiStart = FindStartCode (kData, 0);
  while (uiStart != kNoPosition) {
    const size_t uiHeader = uiStart + 3;
    if (uiHeader < kData.size()) {
      const unsigned int uiType = kData[uiHeader] & 0x1f;
      if (uiType == 1 || uiType == 5)
        ++iCount;
    }
    uiStart = FindStartCode (kData, uiStart + 3);
  }
  return iCount;
}

// Apply emulation_prevention_three_byte to an RBSP byte string.
static std::vector<unsigned char> EscapeRbsp (const std::vector<unsigned char>& kRbsp) {
  std::vector<unsigned char> out;
  out.reserve (kRbsp.size() + kRbsp.size() / 2 + 8);
  for (size_t i = 0; i < kRbsp.size(); ++i) {
    if (out.size() >= 2 && out[out.size() - 1] == 0 && out[out.size() - 2] == 0
        && kRbsp[i] <= 0x03) {
      out.push_back (0x03);
    }
    out.push_back (kRbsp[i]);
  }
  return out;
}

static std::vector<unsigned char> MakeNalFromRbsp (const std::vector<unsigned char>& kRbsp) {
  std::vector<unsigned char> nal;
  nal.push_back (0x00);
  nal.push_back (0x00);
  nal.push_back (0x00);
  nal.push_back (0x01);
  nal.push_back (0x06); // nal_ref_idc = 0, nal_unit_type = 6 (SEI)
  const std::vector<unsigned char> escaped = EscapeRbsp (kRbsp);
  nal.insert (nal.end(), escaped.begin(), escaped.end());
  return nal;
}

struct MessageSpec {
  unsigned char uuid[16];
  std::vector<unsigned char> data;
};

static std::vector<unsigned char> BuildSeiRbsp (const std::vector<MessageSpec>& kMessages) {
  std::vector<unsigned char> rbsp;
  for (size_t m = 0; m < kMessages.size(); ++m) {
    const MessageSpec& msg = kMessages[m];
    rbsp.push_back (0x05); // payloadType = user_data_unregistered
    uint32_t uiPayloadSize = 16u + (uint32_t) msg.data.size();
    while (uiPayloadSize >= 255u) {
      rbsp.push_back (0xff);
      uiPayloadSize -= 255u;
    }
    rbsp.push_back ((unsigned char) uiPayloadSize);
    rbsp.insert (rbsp.end(), msg.uuid, msg.uuid + 16);
    rbsp.insert (rbsp.end(), msg.data.begin(), msg.data.end());
  }
  rbsp.push_back (0x80); // rbsp_trailing_bits
  return rbsp;
}

static std::vector<unsigned char> BuildSeiNal (const std::vector<MessageSpec>& kMessages) {
  return MakeNalFromRbsp (BuildSeiRbsp (kMessages));
}

static std::vector<unsigned char> InsertBeforeFirstVcl (const std::vector<unsigned char>& kAu,
    const std::vector<unsigned char>& kNal) {
  const size_t uiPos = FindFirstVclStartCode (kAu);
  Require (uiPos != kNoPosition, "no VCL NAL found in encoded access unit");
  std::vector<unsigned char> out;
  out.reserve (kAu.size() + kNal.size());
  out.insert (out.end(), kAu.begin(), kAu.begin() + uiPos);
  out.insert (out.end(), kNal.begin(), kNal.end());
  out.insert (out.end(), kAu.begin() + uiPos, kAu.end());
  return out;
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct EncodedFrame {
  std::vector<unsigned char> au;
};

static void ConfigureSpatialLayer (SSpatialLayerConfig& layer, int iWidth, int iHeight, int iBitrate) {
  layer.iVideoWidth = iWidth;
  layer.iVideoHeight = iHeight;
  layer.fFrameRate = 25.0f;
  layer.iSpatialBitrate = iBitrate;
  layer.iMaxSpatialBitrate = iBitrate;
  layer.uiProfileIdc = PRO_BASELINE;
  layer.uiLevelIdc = LEVEL_UNKNOWN;
  layer.iDLayerQp = 26;
  layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
  layer.sSliceArgument.uiSliceNum = 1;
}

static void FillPicture (SSourcePicture& pic, std::vector<unsigned char>& pixels,
                         int iWidth, int iHeight, int iFrame) {
  pixels.assign (iWidth * iHeight * 3 / 2, 128);
  for (int i = 0; i < iWidth * iHeight; ++i)
    pixels[i] = (unsigned char) ((i + iFrame * 9) % 180 + 32);
  for (int i = iWidth * iHeight; i < iWidth * iHeight * 3 / 2; ++i)
    pixels[i] = (unsigned char) ((i + iFrame * 5) % 160 + 48);

  std::memset (&pic, 0, sizeof (pic));
  pic.iColorFormat = videoFormatI420;
  pic.iPicWidth = iWidth;
  pic.iPicHeight = iHeight;
  pic.iStride[0] = iWidth;
  pic.iStride[1] = iWidth / 2;
  pic.iStride[2] = iWidth / 2;
  pic.pData[0] = pixels.data();
  pic.pData[1] = pixels.data() + iWidth * iHeight;
  pic.pData[2] = pixels.data() + iWidth * iHeight + (iWidth * iHeight) / 4;
}

static std::vector<EncodedFrame> EncodeSequence (int iFrames, int iWidth, int iHeight, bool bMultiSlice) {
  ISVCEncoder* enc = NULL;
  Require (WelsCreateSVCEncoder (&enc) == 0, "WelsCreateSVCEncoder");

  if (bMultiSlice) {
    SEncParamExt ep;
    std::memset (&ep, 0, sizeof (ep));
    Require (enc->GetDefaultParams (&ep) == 0, "GetDefaultParams");
    ep.iUsageType = CAMERA_VIDEO_REAL_TIME;
    ep.iPicWidth = iWidth;
    ep.iPicHeight = iHeight;
    ep.iTargetBitrate = 200000;
    ep.iRCMode = RC_OFF_MODE;
    ep.fMaxFrameRate = 25.0f;
    ep.iTemporalLayerNum = 1;
    ep.iSpatialLayerNum = 1;
    ConfigureSpatialLayer (ep.sSpatialLayers[0], iWidth, iHeight, 200000);
    ep.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_FIXEDSLCNUM_SLICE;
    ep.sSpatialLayers[0].sSliceArgument.uiSliceNum = 4;
    ep.iMultipleThreadIdc = 1;
    ep.iComplexityMode = LOW_COMPLEXITY;
    ep.uiIntraPeriod = 1;
    ep.iNumRefFrame = 1;
    ep.bEnableFrameCroppingFlag = true;
    Require (enc->InitializeExt (&ep) == 0, "InitializeExt");
  } else {
    SEncParamBase ep;
    std::memset (&ep, 0, sizeof (ep));
    ep.iUsageType = CAMERA_VIDEO_REAL_TIME;
    ep.fMaxFrameRate = 25.0f;
    ep.iPicWidth = iWidth;
    ep.iPicHeight = iHeight;
    ep.iTargetBitrate = 100000;
    ep.iRCMode = RC_OFF_MODE;
    Require (enc->Initialize (&ep) == 0, "Initialize");
  }

  std::vector<EncodedFrame> frames;
  std::vector<unsigned char> pixels;
  for (int f = 0; f < iFrames; ++f) {
    SSourcePicture pic;
    FillPicture (pic, pixels, iWidth, iHeight, f);
    pic.uiTimeStamp = (unsigned long long) f * 40;
    SFrameBSInfo bs;
    std::memset (&bs, 0, sizeof (bs));
    Require (enc->EncodeFrame (&pic, &bs) == 0, "EncodeFrame");
    Require (bs.eFrameType != videoFrameTypeSkip, "encoder skipped a frame");

    EncodedFrame frame;
    for (int l = 0; l < bs.iLayerNum; ++l) {
      SLayerBSInfo& layer = bs.sLayerInfo[l];
      int iLayerBytes = 0;
      for (int n = 0; n < layer.iNalCount; ++n)
        iLayerBytes += layer.pNalLengthInByte[n];
      frame.au.insert (frame.au.end(), layer.pBsBuf, layer.pBsBuf + iLayerBytes);
    }
    Require (! frame.au.empty(), "empty encoded access unit");
    frames.push_back (frame);
  }

  enc->Uninitialize();
  WelsDestroySVCEncoder (enc);
  return frames;
}

// ---------------------------------------------------------------------------
// Decoder harness
// ---------------------------------------------------------------------------

struct OutputFrame {
  int iWidth;
  int iHeight;
  int iStride[2];
  unsigned long long uiTimestamp;
  std::vector<unsigned char> y;
  std::vector<unsigned char> u;
  std::vector<unsigned char> v;
};

struct QueryResult {
  unsigned int uiCount;
  std::vector<MessageSpec> messages;
  unsigned long long uiTimestamp;
};

struct Call {
  std::vector<unsigned char> data;
  unsigned long long uiTimestamp;
};

struct Step {
  bool bProduced;
  QueryResult query;
};

struct DecodedStream {
  std::vector<OutputFrame> outputs;
  std::vector<Step> steps;
};

static OutputFrame CaptureOutput (unsigned char** ppDst, const SBufferInfo& kInfo) {
  OutputFrame out;
  const int iWidth = kInfo.UsrData.sSystemBuffer.iWidth;
  const int iHeight = kInfo.UsrData.sSystemBuffer.iHeight;
  out.iWidth = iWidth;
  out.iHeight = iHeight;
  out.iStride[0] = kInfo.UsrData.sSystemBuffer.iStride[0];
  out.iStride[1] = kInfo.UsrData.sSystemBuffer.iStride[1];
  out.uiTimestamp = kInfo.uiOutYuvTimeStamp;

  out.y.resize ((size_t) iWidth * iHeight);
  out.u.resize ((size_t) (iWidth / 2) * (iHeight / 2));
  out.v.resize ((size_t) (iWidth / 2) * (iHeight / 2));
  for (int row = 0; row < iHeight; ++row)
    std::memcpy (&out.y[(size_t) row * iWidth], ppDst[0] + (size_t) row * out.iStride[0], iWidth);
  for (int row = 0; row < iHeight / 2; ++row) {
    std::memcpy (&out.u[(size_t) row * (iWidth / 2)], ppDst[1] + (size_t) row * out.iStride[1], iWidth / 2);
    std::memcpy (&out.v[(size_t) row * (iWidth / 2)], ppDst[2] + (size_t) row * out.iStride[1], iWidth / 2);
  }
  return out;
}

static QueryResult QueryUserData (ISVCDecoder* dec) {
  SUserDataUnregisteredInfo info;
  std::memset (&info, 0, sizeof (info));
  const long iRet = dec->GetOption (DECODER_OPTION_GET_USER_DATA_UNREGISTERED, &info);
  Require (iRet == cmResultSuccess, "GetOption(DECODER_OPTION_GET_USER_DATA_UNREGISTERED)");

  QueryResult result;
  result.uiCount = info.count;
  result.uiTimestamp = info.timestamp;
  if (info.count != 0) {
    Require (info.messages != NULL, "GetOption returned a NULL messages array");
    for (unsigned int i = 0; i < info.count; ++i) {
      MessageSpec msg;
      std::memcpy (msg.uuid, info.messages[i].uuid, sizeof (msg.uuid));
      if (info.messages[i].size != 0) {
        Require (info.messages[i].data != NULL, "GetOption returned NULL non-empty data");
        msg.data.assign (info.messages[i].data, info.messages[i].data + info.messages[i].size);
      } else {
        Require (info.messages[i].data == NULL || info.messages[i].size == 0,
                 "bad zero-length message representation");
      }
      result.messages.push_back (msg);
    }
  } else {
    Require (info.messages == NULL, "GetOption returned messages for an empty result");
  }
  return result;
}

static ISVCDecoder* CreateDecoder (int iThreads, bool bEnableUserData) {
  ISVCDecoder* dec = NULL;
  Require (WelsCreateDecoder (&dec) == 0, "WelsCreateDecoder");

  int iThreadsValue = iThreads;
  Require (dec->SetOption (DECODER_OPTION_NUM_OF_THREADS, &iThreadsValue) == cmResultSuccess,
           "SetOption(NUM_OF_THREADS)");

  SDecodingParam dp;
  std::memset (&dp, 0, sizeof (dp));
  dp.uiTargetDqLayer = 255;
  dp.eEcActiveIdc = ERROR_CON_DISABLE;
  dp.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  Require (dec->Initialize (&dp) == cmResultSuccess, "Initialize");

  int iEnabled = bEnableUserData ? 1 : 0;
  Require (dec->SetOption (DECODER_OPTION_USER_DATA_UNREGISTERED_ENABLED, &iEnabled) == cmResultSuccess,
           "SetOption(USER_DATA_UNREGISTERED_ENABLED)");
  return dec;
}

static void DestroyDecoder (ISVCDecoder* dec) {
  dec->Uninitialize();
  WelsDestroyDecoder (dec);
}

static DecodedStream RunCalls (int iThreads, bool bEnableUserData, const std::vector<Call>& kCalls,
                               bool bDrain) {
  ISVCDecoder* dec = CreateDecoder (iThreads, bEnableUserData);
  DecodedStream stream;

  for (size_t i = 0; i < kCalls.size(); ++i) {
    unsigned char* ppDst[3] = { NULL, NULL, NULL };
    SBufferInfo bi;
    std::memset (&bi, 0, sizeof (bi));
    bi.uiInBsTimeStamp = kCalls[i].uiTimestamp;
    const unsigned char* pSrc = kCalls[i].data.empty() ? NULL : kCalls[i].data.data();
    long iRet = dec->DecodeFrameNoDelay (pSrc, (int) kCalls[i].data.size(), ppDst, &bi);
    RequireState (iRet, false, "DecodeFrameNoDelay on a valid stream");
    if (iThreads > 0)
      SettleWorkerOutput();

    Step step;
    step.bProduced = bi.iBufferStatus == 1;
    step.query = QueryUserData (dec);
    stream.steps.push_back (step);
    if (step.bProduced) {
      Require (ppDst[0] != NULL && ppDst[1] != NULL && ppDst[2] != NULL,
               "output picture has NULL planes");
      stream.outputs.push_back (CaptureOutput (ppDst, bi));
    }
  }

  if (bDrain) {
    int iEndOfStream = 1;
    Require (dec->SetOption (DECODER_OPTION_END_OF_STREAM, &iEndOfStream) == cmResultSuccess,
             "SetOption(END_OF_STREAM)");

    // This option waits for all worker contexts to become idle before it
    // returns the number of pictures still held in the reorder buffer.
    int32_t iRemaining = 0;
    Require (dec->GetOption (DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &iRemaining) == cmResultSuccess,
             "GetOption(NUM_OF_FRAMES_REMAINING_IN_BUFFER)");
    for (int32_t i = 0; i < iRemaining; ++i) {
      unsigned char* ppDst[3] = { NULL, NULL, NULL };
      SBufferInfo bi;
      std::memset (&bi, 0, sizeof (bi));
      long iRet = dec->FlushFrame (ppDst, &bi);
      RequireState (iRet, false, "FlushFrame");
      if (iThreads > 0)
        SettleWorkerOutput();
      Step step;
      step.bProduced = bi.iBufferStatus == 1;
      step.query = QueryUserData (dec);
      stream.steps.push_back (step);
      Require (step.bProduced, "FlushFrame returned no picture while frames remained");
      Require (ppDst[0] != NULL, "flushed picture has NULL plane");
      stream.outputs.push_back (CaptureOutput (ppDst, bi));
    }
  }

  DestroyDecoder (dec);
  return stream;
}

static void CompareFrames (const OutputFrame& kTest, const OutputFrame& kControl, const char* kpTag) {
  Require (kTest.iWidth == kControl.iWidth && kTest.iHeight == kControl.iHeight,
           "output dimensions differ from the control decode");
  Require (kTest.y.size() == kControl.y.size() && kTest.u.size() == kControl.u.size()
           && kTest.v.size() == kControl.v.size(), "output plane sizes differ from the control decode");
  if (std::memcmp (kTest.y.data(), kControl.y.data(), kTest.y.size()) != 0
      || std::memcmp (kTest.u.data(), kControl.u.data(), kTest.u.size()) != 0
      || std::memcmp (kTest.v.data(), kControl.v.data(), kTest.v.size()) != 0) {
    int iDiff = 0;
    size_t iFirst = kTest.y.size();
    for (size_t i = 0; i < kTest.y.size(); ++i) {
      if (kTest.y[i] != kControl.y[i]) {
        if (iDiff == 0) iFirst = i;
        ++iDiff;
      }
    }
    std::fprintf (stderr, "FAIL: %s: decoded picture differs from the control decode (Y diffs=%d first=%u)\n",
                  kpTag, iDiff, (unsigned int) iFirst);
    std::fprintf (stderr, "  test: %u %u %u %u  control: %u %u %u %u  chroma sizes %u/%u\n",
                  kTest.y[0], kTest.y[1], kTest.y[2], kTest.y[3],
                  kControl.y[0], kControl.y[1], kControl.y[2], kControl.y[3],
                  (unsigned int) kTest.u.size(), (unsigned int) kTest.y.size());
    std::exit (1);
  }
}

// Match outputs by picture timestamp.  Duplicate timestamps are consumed in
// order, so the two access units that deliberately share a timestamp still
// compare against their respective control pictures.
static void CompareStreams (const DecodedStream& kTest, const DecodedStream& kControl, const char* kpTag) {
  Require (kTest.outputs.size() == kControl.outputs.size(), "annotated/control output count mismatch");
  size_t uiControlIndex = 0;
  for (size_t i = 0; i < kTest.outputs.size(); ++i) {
    while (uiControlIndex < kControl.outputs.size()
           && kControl.outputs[uiControlIndex].uiTimestamp != kTest.outputs[i].uiTimestamp)
      ++uiControlIndex;
    Require (uiControlIndex < kControl.outputs.size(), "control decode has no picture for this timestamp");
    char tag[96];
    std::snprintf (tag, sizeof (tag), "%s frame=%u", kpTag, (unsigned int) i);
    CompareFrames (kTest.outputs[i], kControl.outputs[uiControlIndex], tag);
    ++uiControlIndex;
  }
}

static bool QueryMatches (const QueryResult& kQuery, const std::vector<MessageSpec>& kExpected,
                          unsigned long long uiExpectedTimestamp) {
  if (kQuery.uiCount != kExpected.size())
    return false;
  if (kQuery.uiTimestamp != uiExpectedTimestamp)
    return false;
  for (size_t i = 0; i < kExpected.size(); ++i) {
    if (std::memcmp (kQuery.messages[i].uuid, kExpected[i].uuid, 16) != 0)
      return false;
    if (kQuery.messages[i].data.size() != kExpected[i].data.size())
      return false;
    if (! kExpected[i].data.empty()
        && std::memcmp (kQuery.messages[i].data.data(), kExpected[i].data.data(),
                        kExpected[i].data.size()) != 0)
      return false;
  }
  return true;
}

struct ExpectedOutput {
  std::vector<MessageSpec> messages;
  unsigned long long uiTimestamp;
};

// Every public call must either expose the messages of the picture it returned,
// or expose nothing at all when it returned no picture.
static void CheckQueries (const DecodedStream& kStream, const std::vector<ExpectedOutput>& kExpected,
                          const char* kpTag) {
  size_t uiOutputIndex = 0;
  for (size_t i = 0; i < kStream.steps.size(); ++i) {
    const Step& step = kStream.steps[i];
    if (step.bProduced) {
      Require (uiOutputIndex < kExpected.size(), "more output pictures than expected");
      if (! QueryMatches (step.query, kExpected[uiOutputIndex].messages,
                          kExpected[uiOutputIndex].uiTimestamp)) {
        std::fprintf (stderr, "FAIL: %s: query mismatch for output picture %u (count=%u timestamp=%llu)\n",
                     kpTag, (unsigned int) uiOutputIndex, step.query.uiCount, step.query.uiTimestamp);
        std::exit (1);
      }
      ++uiOutputIndex;
    } else {
      Require (step.query.uiCount == 0, "query returned messages for a call without an output picture");
    }
  }
  Require (uiOutputIndex == kExpected.size(), "fewer output pictures than expected");
}

// ---------------------------------------------------------------------------
// Message generators
// ---------------------------------------------------------------------------

static MessageSpec MakeMessage (unsigned char uiSeed, const std::vector<unsigned char>& kData) {
  MessageSpec msg;
  for (int i = 0; i < 16; ++i)
    msg.uuid[i] = (unsigned char) (uiSeed * 13 + i * 7);
  msg.data = kData;
  return msg;
}

static std::vector<unsigned char> LargePayloadWithStartCodeBytes() {
  std::vector<unsigned char> data (300);
  for (int i = 0; i < 300; ++i)
    data[i] = (unsigned char) ((i * 7 + 3) & 0xff);
  // Force the byte sequences that need emulation prevention on the wire.
  data[10] = 0x00; data[11] = 0x00; data[12] = 0x01;
  data[20] = 0x00; data[21] = 0x00; data[22] = 0x02;
  data[30] = 0x00; data[31] = 0x00; data[32] = 0x03;
  data[40] = 0x00; data[41] = 0x00; data[42] = 0x00;
  return data;
}

// ---------------------------------------------------------------------------
// Test sections
// ---------------------------------------------------------------------------

static void TestMainReplay (int iThreads) {
  const int iWidth = 32;
  const int iHeight = 32;
  std::vector<EncodedFrame> frames = EncodeSequence (7, iWidth, iHeight, false);

  const unsigned char kUuidA = 1;
  const unsigned char kUuidB = 2;
  const unsigned char kUuidC = 3;
  const unsigned char kUuidD = 4;
  const unsigned char kUuidE = 5;

  std::vector<MessageSpec> groupA;
  groupA.push_back (MakeMessage (kUuidA, LargePayloadWithStartCodeBytes()));
  groupA.push_back (MakeMessage (kUuidA, std::vector<unsigned char> ())); // same UUID, empty data
  groupA.push_back (MakeMessage (kUuidB, std::vector<unsigned char> { 0xfa, 0x00, 0x00, 0x01, 0xce }));

  std::vector<MessageSpec> groupB;
  groupB.push_back (MakeMessage (kUuidC, std::vector<unsigned char> { 'h', 'e', 'l', 'l', 'o' }));

  std::vector<MessageSpec> groupC;
  groupC.push_back (MakeMessage (kUuidD, std::vector<unsigned char> (200, 0x5a)));

  std::vector<MessageSpec> groupD;
  groupD.push_back (MakeMessage (kUuidE, std::vector<unsigned char> { 0x7f, 0x00, 0x00, 0x03, 0x00 }));

  const std::vector<unsigned char> seiA = BuildSeiNal (groupA);
  const std::vector<unsigned char> seiB = BuildSeiNal (groupB);
  const std::vector<unsigned char> seiC = BuildSeiNal (groupC);
  const std::vector<unsigned char> seiD = BuildSeiNal (groupD);

  // Timestamps: frames 3 and 4 deliberately share the same input timestamp.
  const unsigned long long ts[7] = { 1000, 1040, 1080, 1120, 1120, 1160, 1200 };

  std::vector<Call> calls;
  std::vector<Call> controlCalls;

  // frame 0: prefix SEI group A inside the access unit.
  calls.push_back (Call { InsertBeforeFirstVcl (frames[0].au, seiA), ts[0] });
  controlCalls.push_back (Call { frames[0].au, ts[0] });

  // frame 1: no messages.
  calls.push_back (Call { frames[1].au, ts[1] });
  controlCalls.push_back (Call { frames[1].au, ts[1] });

  // SEI-only call that precedes the first VCL access unit of frame 2.
  calls.push_back (Call { seiB, ts[2] });
  calls.push_back (Call { frames[2].au, ts[2] });
  // Keep the control call schedule structurally identical.  The standalone
  // SEI call carries no picture data; without it the two decoders would run
  // with different worker-thread timing and the pixel comparison would be
  // needlessly racy.
  controlCalls.push_back (Call { seiB, ts[2] });
  controlCalls.push_back (Call { frames[2].au, ts[2] });

  // frames 3 and 4: same input timestamp, distinct message groups.
  calls.push_back (Call { InsertBeforeFirstVcl (frames[3].au, seiC), ts[3] });
  controlCalls.push_back (Call { frames[3].au, ts[3] });
  calls.push_back (Call { InsertBeforeFirstVcl (frames[4].au, seiD), ts[4] });
  controlCalls.push_back (Call { frames[4].au, ts[4] });

  // frame 5/6: no messages after a message-bearing frame.
  calls.push_back (Call { frames[5].au, ts[5] });
  controlCalls.push_back (Call { frames[5].au, ts[5] });
  calls.push_back (Call { frames[6].au, ts[6] });
  controlCalls.push_back (Call { frames[6].au, ts[6] });

  std::vector<ExpectedOutput> expected (7);
  expected[0].messages = groupA; expected[0].uiTimestamp = ts[0];
  expected[1].messages = std::vector<MessageSpec> (); expected[1].uiTimestamp = ts[1];
  expected[2].messages = groupB; expected[2].uiTimestamp = ts[2];
  expected[3].messages = groupC; expected[3].uiTimestamp = ts[3];
  expected[4].messages = groupD; expected[4].uiTimestamp = ts[4];
  expected[5].messages = std::vector<MessageSpec> (); expected[5].uiTimestamp = ts[5];
  expected[6].messages = std::vector<MessageSpec> (); expected[6].uiTimestamp = ts[6];

  DecodedStream annotated = RunCalls (iThreads, true, calls, true);
  DecodedStream control = RunCalls (iThreads, false, controlCalls, true);

  CheckQueries (annotated, expected, "main replay");
  Require (annotated.outputs.size() == 7, "unexpected number of output pictures");
  char tag[64];
  std::snprintf (tag, sizeof (tag), "main replay threads=%d", iThreads);
  CompareStreams (annotated, control, tag);
}

static void TestConsecutiveQueries (void) {
  const int iWidth = 32;
  const int iHeight = 32;
  std::vector<EncodedFrame> frames = EncodeSequence (2, iWidth, iHeight, false);
  std::vector<MessageSpec> group = { MakeMessage (11, std::vector<unsigned char> { 1, 2, 0, 0, 1 }) };
  std::vector<Call> calls = {
    Call { InsertBeforeFirstVcl (frames[0].au, BuildSeiNal (group)), 2000 },
    Call { frames[1].au, 2040 }
  };
  DecodedStream stream = RunCalls (0, true, calls, true);
  CheckQueries (stream, { ExpectedOutput { group, 2000 }, ExpectedOutput { {}, 2040 } },
                "consecutive query setup");

  // A second GetOption must not consume the data.
  ISVCDecoder* dec = CreateDecoder (0, true);
  unsigned char* ppDst[3] = { NULL, NULL, NULL };
  SBufferInfo bi;
  std::memset (&bi, 0, sizeof (bi));
  bi.uiInBsTimeStamp = 2100;
  const std::vector<unsigned char> au = InsertBeforeFirstVcl (frames[0].au, BuildSeiNal (group));
  Require (dec->DecodeFrameNoDelay (au.data(), (int) au.size(), ppDst, &bi) == dsErrorFree,
           "consecutive query decode");
  Require (bi.iBufferStatus == 1, "consecutive query decode produced no picture");
  QueryResult first = QueryUserData (dec);
  QueryResult second = QueryUserData (dec);
  Require (first.uiCount == group.size() && second.uiCount == group.size(),
           "consecutive queries differ");
  for (size_t i = 0; i < group.size(); ++i) {
    Require (std::memcmp (first.messages[i].uuid, second.messages[i].uuid, 16) == 0,
             "consecutive queries returned different UUIDs");
    Require (first.messages[i].data == second.messages[i].data,
             "consecutive queries returned different data");
  }
  DestroyDecoder (dec);
}

static void TestMultiSlice (int iThreads) {
  const int iWidth = 128;
  const int iHeight = 128;
  std::vector<EncodedFrame> frames = EncodeSequence (2, iWidth, iHeight, true);
  Require (CountVclNals (frames[0].au) >= 2, "multi-slice encoder did not produce multiple VCL NALs");

  std::vector<MessageSpec> group = {
    MakeMessage (21, std::vector<unsigned char> ()),
    MakeMessage (22, std::vector<unsigned char> { 0x00, 0x00, 0x01, 0x00, 0x00, 0x03 })
  };
  std::vector<Call> calls = {
    Call { InsertBeforeFirstVcl (frames[0].au, BuildSeiNal (group)), 3000 },
    Call { frames[1].au, 3040 }
  };
  std::vector<Call> controlCalls = {
    Call { frames[0].au, 3000 },
    Call { frames[1].au, 3040 }
  };

  DecodedStream annotated = RunCalls (iThreads, true, calls, true);
  DecodedStream control = RunCalls (iThreads, false, controlCalls, true);
  CheckQueries (annotated, { ExpectedOutput { group, 3000 }, ExpectedOutput { {}, 3040 } },
                "multi-slice");
  Require (annotated.outputs.size() == 2, "multi-slice output count");
  CompareStreams (annotated, control, "multi-slice");
}

static void TestDisableEnable (int iThreads) {
  const int iWidth = 32;
  const int iHeight = 32;
  std::vector<EncodedFrame> frames = EncodeSequence (4, iWidth, iHeight, false);

  std::vector<MessageSpec> groupA = { MakeMessage (31, std::vector<unsigned char> { 'A' }) };
  std::vector<MessageSpec> groupB = { MakeMessage (32, std::vector<unsigned char> { 'B' }) };
  std::vector<MessageSpec> groupC = { MakeMessage (33, std::vector<unsigned char> { 'C' }) };

  // Malformed payload: type 5 with fewer than 16 bytes, used while disabled
  // to prove the disabled parser does not alter error handling.
  std::vector<unsigned char> badRbsp;
  badRbsp.push_back (0x05);
  badRbsp.push_back (10);
  for (int i = 0; i < 10; ++i)
    badRbsp.push_back ((unsigned char) i);
  badRbsp.push_back (0x80);

  ISVCDecoder* dec = CreateDecoder (iThreads, true);

  // Enable, frame 0 carries group A.
  unsigned char* ppDst[3] = { NULL, NULL, NULL };
  SBufferInfo bi;
  std::memset (&bi, 0, sizeof (bi));
  bi.uiInBsTimeStamp = 4000;
  const std::vector<unsigned char> au0 = InsertBeforeFirstVcl (frames[0].au, BuildSeiNal (groupA));
  Require (dec->DecodeFrameNoDelay (au0.data(), (int) au0.size(), ppDst, &bi) == dsErrorFree,
           "disable/enable frame 0");
  Require (bi.iBufferStatus == 1, "disable/enable frame 0 produced no picture");
  QueryResult q0 = QueryUserData (dec);
  Require (QueryMatches (q0, groupA, 4000), "disable/enable frame 0 messages");
  OutputFrame out0 = CaptureOutput (ppDst, bi);

  // Pending SEI-only call; disabling must discard it.
  std::vector<unsigned char> seiOnly = BuildSeiNal (groupB);
  std::memset (&bi, 0, sizeof (bi));
  bi.uiInBsTimeStamp = 4040;
  Require (dec->DecodeFrameNoDelay (seiOnly.data(), (int) seiOnly.size(), ppDst, &bi) == dsErrorFree,
           "SEI-only call accepted");
  Require (bi.iBufferStatus == 0, "SEI-only call produced a picture");
  Require (QueryUserData (dec).uiCount == 0, "SEI-only call published messages");

  int iEnabled = 0;
  Require (dec->SetOption (DECODER_OPTION_USER_DATA_UNREGISTERED_ENABLED, &iEnabled) == cmResultSuccess,
           "disable user data");
  Require (QueryUserData (dec).uiCount == 0, "disabling did not clear published data");
  iEnabled = 1;
  Require (dec->SetOption (DECODER_OPTION_USER_DATA_UNREGISTERED_ENABLED, &iEnabled) == cmResultSuccess,
           "re-enable user data");
  Require (QueryUserData (dec).uiCount == 0, "re-enabling did not start empty");

  // Frame 1 has no SEI; the discarded pending group must not reappear.
  std::memset (&bi, 0, sizeof (bi));
  bi.uiInBsTimeStamp = 4080;
  Require (dec->DecodeFrameNoDelay (frames[1].au.data(), (int) frames[1].au.size(), ppDst, &bi) == dsErrorFree,
           "disable/enable frame 1");
  Require (bi.iBufferStatus == 1, "disable/enable frame 1 produced no picture");
  Require (QueryUserData (dec).uiCount == 0, "discarded pending messages were republished");
  OutputFrame out1 = CaptureOutput (ppDst, bi);

  // While disabled, malformed SEI must be ignored: normal picture and no error.
  iEnabled = 0;
  Require (dec->SetOption (DECODER_OPTION_USER_DATA_UNREGISTERED_ENABLED, &iEnabled) == cmResultSuccess,
           "disable before malformed SEI");
  std::vector<unsigned char> au2 = InsertBeforeFirstVcl (frames[2].au, MakeNalFromRbsp (badRbsp));
  std::memset (&bi, 0, sizeof (bi));
  bi.uiInBsTimeStamp = 4120;
  Require (dec->DecodeFrameNoDelay (au2.data(), (int) au2.size(), ppDst, &bi) == dsErrorFree,
           "disabled decoder rejected malformed SEI");
  Require (bi.iBufferStatus == 1, "disabled decoder produced no picture for malformed SEI");
  Require (QueryUserData (dec).uiCount == 0, "disabled decoder published messages");
  OutputFrame disabledOut = CaptureOutput (ppDst, bi);

  // Re-enable and decode frame 3 with group C.
  iEnabled = 1;
  Require (dec->SetOption (DECODER_OPTION_USER_DATA_UNREGISTERED_ENABLED, &iEnabled) == cmResultSuccess,
           "enable after disabled decode");
  std::vector<unsigned char> au3 = InsertBeforeFirstVcl (frames[3].au, BuildSeiNal (groupC));
  std::memset (&bi, 0, sizeof (bi));
  bi.uiInBsTimeStamp = 4160;
  Require (dec->DecodeFrameNoDelay (au3.data(), (int) au3.size(), ppDst, &bi) == dsErrorFree,
           "re-enabled frame 3");
  Require (bi.iBufferStatus == 1, "re-enabled frame 3 produced no picture");
  QueryResult q3 = QueryUserData (dec);
  Require (QueryMatches (q3, groupC, 4160), "re-enabled frame 3 messages");
  OutputFrame enabledOut = CaptureOutput (ppDst, bi);

  // Control decode of the same four original access units.
  std::vector<Call> controlCalls = {
    Call { frames[0].au, 4000 }, Call { frames[1].au, 4080 },
    Call { frames[2].au, 4120 }, Call { frames[3].au, 4160 }
  };
  DecodedStream controlStream = RunCalls (iThreads, false, controlCalls, true);
  Require (controlStream.outputs.size() == 4, "disable/enable control output count");
  CompareFrames (out0, controlStream.outputs[0], "enabled frame 0 picture");
  CompareFrames (out1, controlStream.outputs[1], "post-toggle frame 1 picture");
  CompareFrames (disabledOut, controlStream.outputs[2], "disabled malformed-SEI picture");
  CompareFrames (enabledOut, controlStream.outputs[3], "re-enabled picture");
  DestroyDecoder (dec);
}

static void TestMalformedSei (void) {
  const int iWidth = 32;
  const int iHeight = 32;
  std::vector<EncodedFrame> frames = EncodeSequence (1, iWidth, iHeight, false);

  std::vector<MessageSpec> goodGroup = { MakeMessage (41, std::vector<unsigned char> { 'g', 'o', 'o', 'd' }) };

  // A valid message followed by an invalid payload of 10 bytes: nothing from
  // this NAL may be published.
  std::vector<unsigned char> partialRbsp;
  partialRbsp.push_back (0x05);
  partialRbsp.push_back (16 + 4);
  for (int i = 0; i < 16; ++i) partialRbsp.push_back ((unsigned char) i);
  partialRbsp.push_back ('t'); partialRbsp.push_back ('e');
  partialRbsp.push_back ('s'); partialRbsp.push_back ('t');
  partialRbsp.push_back (0x05);
  partialRbsp.push_back (10);
  for (int i = 0; i < 10; ++i) partialRbsp.push_back ((unsigned char) i);
  partialRbsp.push_back (0x80);

  // Declared payload size runs past the end of the NAL.
  std::vector<unsigned char> overrunRbsp;
  overrunRbsp.push_back (0x05);
  overrunRbsp.push_back (40); // declared: 40 bytes, only 16 supplied
  for (int i = 0; i < 16; ++i) overrunRbsp.push_back ((unsigned char) i);
  overrunRbsp.push_back (0x80);

  // A payloadSize using an unterminated 0xff run: the extension length cannot
  // be represented within the NAL and must be rejected.
  std::vector<unsigned char> overflowRbsp;
  overflowRbsp.push_back (0x05);
  for (int i = 0; i < 512; ++i)
    overflowRbsp.push_back (0xff);

  // Valid message but bad rbsp_trailing_bits.
  std::vector<unsigned char> badTrailingRbsp;
  badTrailingRbsp.push_back (0x05);
  badTrailingRbsp.push_back (16 + 2);
  for (int i = 0; i < 16; ++i) badTrailingRbsp.push_back ((unsigned char) (0x80 + i));
  badTrailingRbsp.push_back (0xaa); badTrailingRbsp.push_back (0xbb);
  badTrailingRbsp.push_back (0x40); // not rbsp_trailing_bits

  struct BadCase {
    const char* name;
    std::vector<unsigned char> rbsp;
  } badCases[4] = {
    { "partial message", partialRbsp },
    { "payload overrun", overrunRbsp },
    { "extension overflow", overflowRbsp },
    { "bad trailing bits", badTrailingRbsp }
  };

  // Control picture for the recovery IDR.
  DecodedStream control = RunCalls (0, false, { Call { frames[0].au, 5000 } }, false);
  Require (control.outputs.size() == 1, "malformed-SEI control decode produced no picture");

  for (int c = 0; c < 4; ++c) {
    ISVCDecoder* dec = CreateDecoder (0, true);

    // First a successful picture with messages, so the error call below could
    // wrongly reuse them if the decoder kept stale published data.
    unsigned char* ppDst[3] = { NULL, NULL, NULL };
    SBufferInfo bi;
    std::memset (&bi, 0, sizeof (bi));
    bi.uiInBsTimeStamp = 5100;
    std::vector<unsigned char> auWithSei = InsertBeforeFirstVcl (frames[0].au, BuildSeiNal (goodGroup));
    long iRet = dec->DecodeFrameNoDelay (auWithSei.data(), (int) auWithSei.size(), ppDst, &bi);
    RequireState (iRet, false, "malformed case initial valid decode");
    Require (bi.iBufferStatus == 1, "malformed case initial decode produced no picture");
    Require (QueryMatches (QueryUserData (dec), goodGroup, 5100), "malformed case initial messages");

    // The malformed SEI-only call: dsBitstreamError, empty query.
    std::vector<unsigned char> badNal = MakeNalFromRbsp (badCases[c].rbsp);
    std::memset (&bi, 0, sizeof (bi));
    bi.uiInBsTimeStamp = 5140;
    iRet = dec->DecodeFrameNoDelay (badNal.data(), (int) badNal.size(), ppDst, &bi);
    RequireState (iRet, true, badCases[c].name);
    Require (bi.iBufferStatus == 0, "malformed SEI call produced a picture");
    QueryResult badQuery = QueryUserData (dec);
    Require (badQuery.uiCount == 0, "malformed SEI published messages (partial or previous frame)");

    // An independent valid IDR must still decode and expose its own messages.
    std::vector<MessageSpec> recoveryGroup = { MakeMessage ((unsigned char) (50 + c), std::vector<unsigned char> { (unsigned char) c }) };
    std::vector<unsigned char> recoveryAu = InsertBeforeFirstVcl (frames[0].au, BuildSeiNal (recoveryGroup));
    std::memset (&bi, 0, sizeof (bi));
    bi.uiInBsTimeStamp = 5180;
    iRet = dec->DecodeFrameNoDelay (recoveryAu.data(), (int) recoveryAu.size(), ppDst, &bi);
    RequireState (iRet, false, "recovery IDR decode");
    Require (bi.iBufferStatus == 1, "recovery IDR produced no picture");
    Require (QueryMatches (QueryUserData (dec), recoveryGroup, 5180), "recovery IDR messages");
    OutputFrame recovery = CaptureOutput (ppDst, bi);
    CompareFrames (recovery, control.outputs[0], badCases[c].name);

    DestroyDecoder (dec);
  }

  // The disabled decoder must keep the old behavior for invalid SEI bytes.
  ISVCDecoder* disabled = CreateDecoder (0, false);
  unsigned char* ppDst[3] = { NULL, NULL, NULL };
  SBufferInfo bi;
  std::memset (&bi, 0, sizeof (bi));
  bi.uiInBsTimeStamp = 5200;
  std::vector<unsigned char> disabledAu = InsertBeforeFirstVcl (frames[0].au, MakeNalFromRbsp (partialRbsp));
  long iRet = disabled->DecodeFrameNoDelay (disabledAu.data(), (int) disabledAu.size(), ppDst, &bi);
  RequireState (iRet, false, "disabled malformed SEI decode");
  Require (bi.iBufferStatus == 1, "disabled malformed SEI produced no picture");
  Require (QueryUserData (disabled).uiCount == 0, "disabled decoder published messages");
  CompareFrames (CaptureOutput (ppDst, bi), control.outputs[0], "disabled malformed-SEI picture");
  DestroyDecoder (disabled);
}

int main (void) {
  std::fprintf (stderr, "running user-data SEI tests\n");

  TestMainReplay (0);
  TestMainReplay (1);
  TestMainReplay (2);
  TestConsecutiveQueries ();
  TestMultiSlice (0);
  TestMultiSlice (1);
  TestMultiSlice (2);
  TestDisableEnable (0);
  TestMalformedSei ();

  std::printf ("openh264 user-data sei: ok\n");
  return 0;
}
