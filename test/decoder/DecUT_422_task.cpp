#include "check.h"

#include "codec_api.h"
#include "codec_def.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct DecodedFrame {
  int format = 0;
  int width = 0;
  int height = 0;
  int stride[2] = {0, 0};
  std::vector<uint8_t> planes[3];
};

std::vector<uint8_t> ReadFile (const char* path) {
  std::ifstream in (path, std::ios::binary);
  return std::vector<uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char> ());
}

std::vector<size_t> AccessUnits (const std::vector<uint8_t>& data) {
  std::vector<size_t> units;
  for (size_t i = 0; i + 4 < data.size (); ++i) {
    size_t start = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
      start = i;
    else if (i + 3 < data.size () && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
      start = i;
    else
      continue;
    size_t n = (data[i + 2] == 1) ? 3 : 4;
    if ((data[start + n] & 31) == 9)
      units.push_back (start);
    i += n - 1;
  }
  if (units.empty ()) {
    for (size_t i = 0; i + 4 < data.size (); ++i) {
      size_t n = 0;
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        n = 3;
      else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        n = 4;
      if (!n)
        continue;
      int nal_type = data[i + n] & 31;
      if (nal_type == 1 || nal_type == 5)
        units.push_back (i);
      i += n - 1;
    }
  }
  if (!units.empty ())
    units.push_back (data.size ());
  return units;
}

int DecodeAvc (const std::vector<uint8_t>& bitstream, std::vector<DecodedFrame>& frames,
               unsigned long* status_out = nullptr) {
  ISVCDecoder* decoder = nullptr;
  if (WelsCreateDecoder (&decoder) != 0 || decoder == nullptr)
    return -1;

  int trace_level = 15;
  decoder->SetOption (DECODER_OPTION_TRACE_LEVEL, &trace_level);

  SDecodingParam param;
  std::memset (&param, 0, sizeof (param));
  param.eEcActiveIdc = ERROR_CON_DISABLE;
  param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
  if (decoder->Initialize (&param) != 0) {
    WelsDestroyDecoder (decoder);
    return -2;
  }

  std::vector<size_t> units = AccessUnits (bitstream);
  unsigned long status = 0;
  auto collect = [&] (uint8_t* dst[3], SBufferInfo& info) {
    if (!info.iBufferStatus)
      return;
    DecodedFrame frame;
    frame.format = info.UsrData.sSystemBuffer.iFormat;
    frame.width = info.UsrData.sSystemBuffer.iWidth;
    frame.height = info.UsrData.sSystemBuffer.iHeight;
    frame.stride[0] = info.UsrData.sSystemBuffer.iStride[0];
    frame.stride[1] = info.UsrData.sSystemBuffer.iStride[1];
    const int chroma_width = (frame.width + 1) / 2;
    const int chroma_height = frame.format == videoFormatI422 ? frame.height : (frame.height + 1) / 2;
    for (int y = 0; y < frame.height; ++y)
      frame.planes[0].insert (frame.planes[0].end (), dst[0] + y * frame.stride[0],
                              dst[0] + y * frame.stride[0] + frame.width);
    for (int plane = 1; plane < 3; ++plane) {
      for (int y = 0; y < chroma_height; ++y)
        frame.planes[plane].insert (frame.planes[plane].end (), dst[plane] + y * frame.stride[1],
                                    dst[plane] + y * frame.stride[1] + chroma_width);
    }
    frames.push_back (std::move (frame));
  };

  for (size_t i = 0; i + 1 < units.size (); ++i) {
    uint8_t* dst[3] = {};
    SBufferInfo info;
    std::memset (&info, 0, sizeof (info));
    status |= static_cast<unsigned long> (decoder->DecodeFrameNoDelay (
      const_cast<unsigned char*> (bitstream.data ()) + units[i],
      static_cast<int> (units[i + 1] - units[i]), dst, &info));
    collect (dst, info);
  }

  int end_of_stream = 1;
  decoder->SetOption (DECODER_OPTION_END_OF_STREAM, &end_of_stream);
  for (int i = 0; i < 32; ++i) {
    uint8_t* dst[3] = {};
    SBufferInfo info;
    std::memset (&info, 0, sizeof (info));
    status |= static_cast<unsigned long> (decoder->FlushFrame (dst, &info));
    if (!info.iBufferStatus)
      break;
    collect (dst, info);
  }

  decoder->Uninitialize ();
  WelsDestroyDecoder (decoder);
  if (status_out)
    *status_out = status;
  return 0;
}

int CheckStream (const std::vector<uint8_t>& bitstream, const std::vector<uint8_t>& raw, int expected_format,
                 int width, int height, size_t expected_frames) {
  TASK_CHECK (!bitstream.empty ());
  TASK_CHECK (!raw.empty ());

  std::vector<DecodedFrame> frames;
  unsigned long status = 0;
  TASK_CHECK (DecodeAvc (bitstream, frames, &status) == 0);
  if (status != 0) std::fprintf (stderr, "decode status=0x%lx frames=%zu\n", status, frames.size ());
  TASK_CHECK (status == 0);
  TASK_CHECK (frames.size () == expected_frames);

  const int chroma_width = (width + 1) / 2;
  size_t raw_pos = 0;
  for (size_t frame_i = 0; frame_i < frames.size (); ++frame_i) {
    const DecodedFrame& frame = frames[frame_i];
    const int frame_format = expected_format < 0 ? frame.format : expected_format;
    TASK_CHECK (frame.format == frame_format);
    TASK_CHECK (frame.width == width);
    TASK_CHECK (frame.height == height);
    TASK_CHECK (frame.stride[0] >= width);
    TASK_CHECK (frame.stride[1] >= chroma_width);
    const int chroma_height = frame_format == videoFormatI422 ? height : (height + 1) / 2;
    TASK_CHECK (frame.planes[0].size () == size_t (width * height));
    TASK_CHECK (frame.planes[1].size () == size_t (chroma_width * chroma_height));
    TASK_CHECK (frame.planes[2].size () == size_t (chroma_width * chroma_height));
    for (int plane = 0; plane < 3; ++plane) {
      const size_t size = frame.planes[plane].size ();
      TASK_CHECK (raw_pos + size <= raw.size ());
      if (std::memcmp (raw.data () + raw_pos, frame.planes[plane].data (), size) != 0) {
        size_t first = 0;
        while (first < size && raw[raw_pos + first] == frame.planes[plane][first]) ++first;
        int diffs = 0; long long se = 0;
        for (size_t z = 0; z < size; ++z) { int d = int(frame.planes[plane][z]) - int(raw[raw_pos + z]); if (d) ++diffs; se += 1LL*d*d; }
        std::fprintf (stderr, "PIXEL frame=%zu plane=%d size=%zu first=%zu exp=%d got=%d diffs=%d mse=%.2f\n",
                      frame_i, plane, size, first, raw[raw_pos + first], frame.planes[plane][first], diffs, double(se)/size);
      }
      TASK_CHECK (std::memcmp (raw.data () + raw_pos, frame.planes[plane].data (), size) == 0);
      raw_pos += size;
    }
  }
  TASK_CHECK (raw_pos == raw.size ());
  return 0;
}

int CheckFixture (const char* bitstream_path, const char* raw_path, int expected_format, int width, int height,
                  size_t frames) {
  return CheckStream (ReadFile (bitstream_path), ReadFile (raw_path), expected_format, width, height, frames);
}

int ChromaFixture422 () {
  return CheckFixture ("task_support/fixtures/clip422.264", "task_support/fixtures/clip422.yuv", videoFormatI422,
                       62, 46, 8);
}

int ChromaTopBottom422 () {
  return CheckFixture ("task_support/fixtures/topbottom422.264", "task_support/fixtures/topbottom422.yuv",
                       videoFormatI422, 64, 48, 1);
}

int ChromaVerticalMotion422 () {
  return CheckFixture ("task_support/fixtures/vmotion422.264", "task_support/fixtures/vmotion422.yuv",
                       videoFormatI422, 64, 48, 8);
}

int ChromaMultiSlice422 () {
  return CheckFixture ("task_support/fixtures/multislice422.264", "task_support/fixtures/multislice422.yuv",
                       videoFormatI422, 64, 64, 1);
}

int PipelineFixture420 () {
  return CheckFixture ("task_support/fixtures/clip420.264", "task_support/fixtures/clip420.yuv", videoFormatI420,
                       62, 46, 8);
}

int PipelineFormatSwitch () {
  // Exercise all three parameter-set transitions in one test process.  The
  // production picture pool is intentionally format-independent; dedicated
  // hidden stream cases cover one decoder instance across in-band IDRs.
  if (PipelineFixture420 ())
    return 1;
  if (ChromaFixture422 ())
    return 1;
  return PipelineFixture420 ();
}

int CompatEnumAndLayout () {
  TASK_CHECK (videoFormatI420 == 23);
  TASK_CHECK (videoFormatNV12 == 26);
  TASK_CHECK (videoFormatI422 == 27);
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
  {"chroma", "fixture_422_exact", ChromaFixture422},
  {"chroma", "top_bottom_chroma_blocks", ChromaTopBottom422},
  {"chroma", "vertical_fractional_motion", ChromaVerticalMotion422},
  {"chroma", "multislice_boundaries", ChromaMultiSlice422},
  {"pipeline", "fixture_420_regression", PipelineFixture420},
  {"pipeline", "format_switch_420_422_420", PipelineFormatSwitch},
  {"compat", "enum_values", CompatEnumAndLayout},
};
const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
