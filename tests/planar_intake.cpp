/******************************************************************************
* Copyright (C) 2018, Divideon.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* This library is also available under a commercial license.
******************************************************************************/

// Planar intake regression.
//
// Part 1 (public encoder API): the same deterministic short sequence is
// encoded through encoder_encode (packed reference, produced by the
// independent helper in planar_intake_ref.*) and through encoder_encode2
// (three separate planes with positive byte strides, guard regions and
// clobbered source buffers). Output NAL units must match in type, order and
// exact bytes, reconstructed pictures must match per POC, changing only the
// padding values must not change the bitstream, a real decoder must decode
// the bitstream back to the encoder reconstruction, and sub_gop_length=8
// must show a coding order different from display order.
//
// Part 2 (Resampler::ConvertFrom internal check): compact, strided and
// source padding entry paths are fed independent patterns and every stored
// sample is checked against an independently recomputed upshifted value,
// including right/bottom extension of the last valid column/row.
//
// A crash, an out of bounds access or a compile error is never treated as a
// successful detection: every check reports the first differing test case,
// plane and coordinates and main() returns non-zero on any failure.

#include <stdint.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "planar_intake_ref.h"
#include "xvc_common_lib/common.h"
#include "xvc_common_lib/resample.h"
#include "xvc_common_lib/simd_cpu.h"
#include "xvc_common_lib/simd_functions.h"
#include "xvc_common_lib/utils.h"
#include "xvc_common_lib/yuv_pic.h"
#include "xvc_dec_lib/xvcdec.h"
#include "xvc_enc_lib/xvcenc.h"

namespace {

using planar_intake_ref::ChromaFormat;
using planar_intake_ref::ExpectedPaddedSample;
using planar_intake_ref::Mismatch;
using planar_intake_ref::SequenceConfig;
using planar_intake_ref::StridedPlane;

int g_num_failures = 0;
int g_num_resampler_cases = 0;
int g_num_end_to_end_cases = 0;
std::string g_current_case;

const int kNumFrames = 17;
const int kQp = 27;
const int kClobberValue = 0xa5;

struct StoredNal {
  std::vector<uint8_t> bytes;
  uint32_t type = 0;
  uint32_t poc = 0;
  int64_t user_data = 0;
};

struct EncodeRun {
  std::vector<StoredNal> nals;
  std::map<int, std::vector<uint8_t> > rec_by_poc;
  std::vector<uint32_t> picture_poc_order;
};

void RecordFailure(const Mismatch &mismatch) {
  if (g_num_failures == 0) {
    const char *dump_path = std::getenv("PLANAR_INTAKE_FAIL_FILE");
    std::FILE *dump = dump_path ? std::fopen(dump_path, "w") : nullptr;
    if (dump) {
      std::fprintf(dump, "case=%s\nframe=%d\nplane=%d\nx=%d\ny=%d\n"
                   "expected=%d\nactual=%d\n",
                   mismatch.case_name.c_str(), mismatch.frame_index,
                   mismatch.plane, mismatch.x, mismatch.y,
                   mismatch.expected, mismatch.actual);
      if (!mismatch.detail.empty()) {
        std::fprintf(dump, "detail=%s\n", mismatch.detail.c_str());
      }
      std::fclose(dump);
    }
  }
  g_num_failures++;
}

void Fail(const Mismatch &mismatch) {
  std::printf("MISMATCH case=%s frame=%d plane=%d x=%d y=%d "
              "expected=%d actual=%d\n",
              mismatch.case_name.c_str(), mismatch.frame_index,
              mismatch.plane, mismatch.x, mismatch.y,
              mismatch.expected, mismatch.actual);
  RecordFailure(mismatch);
}

void FailPlain(const std::string &message) {
  Mismatch mismatch;
  mismatch.case_name = g_current_case;
  mismatch.detail = message;
  std::printf("MISMATCH case=%s detail=%s\n", g_current_case.c_str(),
              message.c_str());
  RecordFailure(mismatch);
}

std::string CaseName(const std::string &prefix, const SequenceConfig &cfg) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer),
                "%s w%d h%d cf%d in%d int%d gop%d frames%d",
                prefix.c_str(), cfg.width, cfg.height,
                static_cast<int>(cfg.chroma_format),
                cfg.input_bitdepth, cfg.internal_bitdepth,
                cfg.sub_gop_length, cfg.num_frames);
  return std::string(buffer);
}

void ClobberImageArea(const SequenceConfig &cfg,
                      std::vector<StridedPlane> *planes) {
  const int sample_size = cfg.input_bitdepth > 8 ? 2 : 1;
  for (int plane = 0; plane < 3; plane++) {
    StridedPlane &strided = (*planes)[plane];
    for (int y = 0; y < strided.height; y++) {
      for (int x = 0; x < strided.width; x++) {
        uint8_t *dst = strided.data + y * strided.stride_bytes +
          x * sample_size;
        if (sample_size == 1) {
          dst[0] = static_cast<uint8_t>(kClobberValue);
        } else {
          uint16_t *dst16 = reinterpret_cast<uint16_t*>(dst);
          *dst16 = static_cast<uint16_t>(kClobberValue |
                                          (kClobberValue << 8));
        }
      }
    }
  }
}

// Verifies that guard regions and stride padding still hold their padding
// value after the encoder returns. Only initialized, owned bytes are read.
void VerifyPaddingIntact(const SequenceConfig &cfg,
                         const std::vector<StridedPlane> &planes) {
  const int sample_size = cfg.input_bitdepth > 8 ? 2 : 1;
  const auto expected_byte_at = [&](int byte_index) {
    return (cfg.padding_value >> (8 * (byte_index % sample_size))) & 0xff;
  };
  for (int plane = 0; plane < 3; plane++) {
    const StridedPlane &strided = planes[plane];
    const int row_bytes = strided.width * sample_size;
    const int pad_bytes =
      static_cast<int>(strided.stride_bytes) - row_bytes;
    for (int y = 0; y < strided.height; y++) {
      const uint8_t *row_start =
        strided.data + y * strided.stride_bytes;
      for (int b = 0; b < pad_bytes; b++) {
        const int expected = expected_byte_at(row_bytes + b);
        if (row_start[row_bytes + b] != expected) {
          Mismatch mismatch;
          mismatch.case_name = g_current_case;
          mismatch.plane = plane;
          mismatch.x = strided.width + b;
          mismatch.y = y;
          mismatch.expected = expected;
          mismatch.actual = row_start[row_bytes + b];
          Fail(mismatch);
          return;
        }
      }
    }
    for (int b = 0; b < planar_intake_ref::kGuardBytes; b++) {
      const int expected = expected_byte_at(b);
      if (strided.data[b - planar_intake_ref::kGuardBytes] != expected) {
        Mismatch mismatch;
        mismatch.case_name = g_current_case;
        mismatch.plane = plane;
        mismatch.x = -1;
        mismatch.y = -1;
        mismatch.expected = expected;
        mismatch.actual =
          strided.data[b - planar_intake_ref::kGuardBytes];
        Fail(mismatch);
        return;
      }
      const uint8_t *tail =
        strided.data + strided.height * strided.stride_bytes;
      if (tail[b] != expected) {
        Mismatch mismatch;
        mismatch.case_name = g_current_case;
        mismatch.plane = plane;
        mismatch.x = -2;
        mismatch.y = strided.height;
        mismatch.expected = expected;
        mismatch.actual = tail[b];
        Fail(mismatch);
        return;
      }
    }
  }
}

xvc_encoder* CreateEncoder(const xvc_encoder_api *api,
                           const SequenceConfig &cfg) {
  xvc_encoder_parameters *param = api->parameters_create();
  if (api->parameters_set_default(param) != XVC_ENC_OK) {
    api->parameters_destroy(param);
    return nullptr;
  }
  param->width = cfg.width;
  param->height = cfg.height;
  param->chroma_format =
    static_cast<xvc_enc_chroma_format>(cfg.chroma_format);
  param->input_bitdepth = cfg.input_bitdepth;
  param->internal_bitdepth = cfg.internal_bitdepth;
  param->sub_gop_length = cfg.sub_gop_length;
  param->qp = cfg.qp;
  param->threads = 0;
  param->simd_mask = 0;
  param->speed_mode = 2;
  param->framerate = 30.0;
  // source_padding stays enabled so 34x26 exercises the padding branch.
  static char source_padding_setting[] = "source_padding 1";
  param->explicit_encoder_settings = source_padding_setting;
  if (api->parameters_check(param) != XVC_ENC_OK) {
    api->parameters_destroy(param);
    return nullptr;
  }
  xvc_encoder *encoder = api->encoder_create(param);
  api->parameters_destroy(param);
  return encoder;
}

void StoreOutput(const xvc_enc_nal_unit *nals, int num_nals,
                 const xvc_enc_pic_buffer &rec_pic, int *next_rec_poc,
                 EncodeRun *run) {
  for (int i = 0; i < num_nals; i++) {
    StoredNal stored;
    stored.bytes.assign(nals[i].bytes, nals[i].bytes + nals[i].size);
    stored.type = nals[i].stats.nal_unit_type;
    stored.poc = nals[i].stats.poc;
    stored.user_data = nals[i].user_data;
    run->nals.push_back(stored);
    if (stored.type <= 5) {
      run->picture_poc_order.push_back(stored.poc);
    }
  }
  if (rec_pic.size > 0 && rec_pic.pic) {
    run->rec_by_poc[*next_rec_poc] =
      std::vector<uint8_t>(rec_pic.pic, rec_pic.pic + rec_pic.size);
    (*next_rec_poc)++;
  }
}

EncodeRun RunPacked(const SequenceConfig &cfg) {
  const xvc_encoder_api *api = xvc_encoder_api_get();
  xvc_encoder *encoder = CreateEncoder(api, cfg);
  EncodeRun run;
  int next_rec_poc = 0;
  assert(encoder);
  for (int frame = 0; frame < cfg.num_frames; frame++) {
    std::vector<uint8_t> packed =
      planar_intake_ref::BuildPackedFrame(frame, cfg);
    xvc_enc_nal_unit *nals = nullptr;
    int num_nals = 0;
    xvc_enc_pic_buffer rec_pic = {0, 0};
    xvc_enc_return_code rc =
      api->encoder_encode(encoder, packed.data(), &nals, &num_nals, &rec_pic);
    if (rc != XVC_ENC_OK) {
      FailPlain("packed encoder_encode failed");
    }
    StoreOutput(nals, num_nals, rec_pic, &next_rec_poc, &run);
  }
  for (int attempt = 0; attempt < 100; attempt++) {
    xvc_enc_nal_unit *nals = nullptr;
    int num_nals = 0;
    xvc_enc_pic_buffer rec_pic = {0, 0};
    xvc_enc_return_code rc =
      api->encoder_flush(encoder, &nals, &num_nals, &rec_pic);
    StoreOutput(nals, num_nals, rec_pic, &next_rec_poc, &run);
    if (rc == XVC_ENC_NO_MORE_OUTPUT) {
      break;
    }
    if (attempt == 99) {
      FailPlain("packed flush did not terminate");
    }
  }
  api->encoder_destroy(encoder);
  return run;
}

EncodeRun RunPlanar(const SequenceConfig &cfg_in, int padding_value) {
  SequenceConfig cfg = cfg_in;
  cfg.padding_value = padding_value;
  const xvc_encoder_api *api = xvc_encoder_api_get();
  xvc_encoder *encoder = CreateEncoder(api, cfg);
  EncodeRun run;
  int next_rec_poc = 0;
  assert(encoder);
  std::vector<StridedPlane> planes =
    planar_intake_ref::BuildStridedFrame(0, cfg);
  for (int frame = 0; frame < cfg.num_frames; frame++) {
    if (frame > 0) {
      planar_intake_ref::RefillFrame(frame, cfg, &planes);
    }
    const uint8_t *plane_bytes[3] = {
      planes[0].data, planes[1].data, planes[2].data
    };
    int plane_stride[3] = {
      static_cast<int>(planes[0].stride_bytes),
      static_cast<int>(planes[1].stride_bytes),
      static_cast<int>(planes[2].stride_bytes)
    };
    xvc_enc_nal_unit *nals = nullptr;
    int num_nals = 0;
    xvc_enc_pic_buffer rec_pic = {0, 0};
    xvc_enc_return_code rc = api->encoder_encode2(
      encoder, plane_bytes, plane_stride, &nals, &num_nals, &rec_pic,
      frame);
    if (rc != XVC_ENC_OK) {
      FailPlain("planar encoder_encode2 failed");
    }
    StoreOutput(nals, num_nals, rec_pic, &next_rec_poc, &run);
    // Padding and guard bytes are not part of the image and must be
    // untouched by the encoder.
    VerifyPaddingIntact(cfg, planes);
    // The source must be fully consumed before the API returns: overwrite
    // the image area immediately and refill a new pattern next iteration.
    ClobberImageArea(cfg, &planes);
  }
  for (int attempt = 0; attempt < 100; attempt++) {
    xvc_enc_nal_unit *nals = nullptr;
    int num_nals = 0;
    xvc_enc_pic_buffer rec_pic = {0, 0};
    xvc_enc_return_code rc =
      api->encoder_flush(encoder, &nals, &num_nals, &rec_pic);
    StoreOutput(nals, num_nals, rec_pic, &next_rec_poc, &run);
    if (rc == XVC_ENC_NO_MORE_OUTPUT) {
      break;
    }
    if (attempt == 99) {
      FailPlain("planar flush did not terminate");
    }
  }
  api->encoder_destroy(encoder);
  return run;
}

void CompareNals(const EncodeRun &reference, const EncodeRun &candidate) {
  if (reference.nals.size() != candidate.nals.size()) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "nal count differs ref=%u cand=%u",
                  static_cast<unsigned>(reference.nals.size()),
                  static_cast<unsigned>(candidate.nals.size()));
    FailPlain(message);
    return;
  }
  for (size_t i = 0; i < reference.nals.size(); i++) {
    if (reference.nals[i].type != candidate.nals[i].type) {
      Mismatch mismatch;
      mismatch.case_name = g_current_case;
      mismatch.frame_index = static_cast<int>(i);
      mismatch.expected = static_cast<int>(reference.nals[i].type);
      mismatch.actual = static_cast<int>(candidate.nals[i].type);
      std::printf("MISMATCH case=%s nal_index=%d expected_type=%d "
                  "actual_type=%d\n", g_current_case.c_str(),
                  static_cast<int>(i), mismatch.expected, mismatch.actual);
      Fail(mismatch);
      return;
    }
    if (reference.nals[i].bytes != candidate.nals[i].bytes) {
      const std::vector<uint8_t> &a = reference.nals[i].bytes;
      const std::vector<uint8_t> &b = candidate.nals[i].bytes;
      size_t first = 0;
      while (first < a.size() && first < b.size() &&
             a[first] == b[first]) {
        first++;
      }
      char message[160];
      std::snprintf(message, sizeof(message),
                    "nal byte diff at nal_index=%d nal_type=%u byte=%u",
                    static_cast<int>(i), reference.nals[i].type,
                    static_cast<unsigned>(first));
      FailPlain(message);
      return;
    }
  }
}

void CompareRecPictures(const EncodeRun &reference,
                        const EncodeRun &candidate) {
  if (reference.rec_by_poc.size() != candidate.rec_by_poc.size()) {
    char message[128];
    std::snprintf(message, sizeof(message),
                  "rec picture count differs ref=%u cand=%u",
                  static_cast<unsigned>(reference.rec_by_poc.size()),
                  static_cast<unsigned>(candidate.rec_by_poc.size()));
    FailPlain(message);
    return;
  }
  for (std::map<int, std::vector<uint8_t> >::const_iterator it =
       reference.rec_by_poc.begin(); it != reference.rec_by_poc.end();
       ++it) {
    int poc = it->first;
    if (candidate.rec_by_poc.count(poc) == 0) {
      char message[64];
      std::snprintf(message, sizeof(message), "missing rec poc=%d", poc);
      FailPlain(message);
      return;
    }
    if (it->second != candidate.rec_by_poc.at(poc)) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "rec bytes differ at poc=%d", poc);
      FailPlain(message);
      return;
    }
  }
}

// Plane geometry of the encoder reconstruction buffer, which uses internal
// (possibly source padded) dimensions and tight packing.
struct PlaneGeometry {
  int width;
  int height;
  ptrdiff_t offset;
};

std::vector<PlaneGeometry>
GetRecGeometry(const SequenceConfig &cfg) {
  const int internal_width = (cfg.width + 7) / 8 * 8;
  const int internal_height = (cfg.height + 7) / 8 * 8;
  std::vector<PlaneGeometry> geometry(3);
  ptrdiff_t offset = 0;
  const int sample_size = cfg.internal_bitdepth > 8 ? 2 : 1;
  for (int plane = 0; plane < 3; plane++) {
    geometry[plane].width =
      planar_intake_ref::PlaneWidth(internal_width, plane,
                                    cfg.chroma_format);
    geometry[plane].height =
      planar_intake_ref::PlaneHeight(internal_height, plane,
                                     cfg.chroma_format);
    geometry[plane].offset = offset;
    offset += static_cast<ptrdiff_t>(geometry[plane].width) *
      geometry[plane].height * sample_size;
  }
  return geometry;
}

// Drains all currently available decoder output pictures and compares each
// against the encoder reconstruction of the same POC. user_data is checked
// against POC here (at output time) rather than per input call, because one
// encoder_encode2 call can emit several delayed picture NAL units.
void DrainDecoderOutputs(const xvc_decoder_api *api, xvc_decoder *decoder,
                         const SequenceConfig &cfg, const EncodeRun &run,
                         std::set<int> *decoded_pocs) {
  const std::vector<PlaneGeometry> geometry = GetRecGeometry(cfg);
  const int sample_size = cfg.internal_bitdepth > 8 ? 2 : 1;
  while (true) {
    xvc_decoded_picture out;
    std::memset(&out, 0, sizeof(out));
    if (api->decoder_get_picture(decoder, &out) != XVC_DEC_OK) {
      break;
    }
    const int poc = static_cast<int>(out.stats.poc);
    if (out.user_data != static_cast<int64_t>(poc)) {
      char message[128];
      std::snprintf(message, sizeof(message),
                    "picture nal user_data=%lld != poc=%d",
                    static_cast<long long>(out.user_data), poc);
      FailPlain(message);
    }
    if (out.stats.width != cfg.width || out.stats.height != cfg.height) {
      char message[128];
      std::snprintf(message, sizeof(message),
                    "decoded size %dx%d != %dx%d at poc=%d",
                    out.stats.width, out.stats.height,
                    cfg.width, cfg.height, poc);
      FailPlain(message);
    }
    if (out.stats.bitdepth != cfg.internal_bitdepth) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "decoded bitdepth %d != %d at poc=%d",
                    out.stats.bitdepth, cfg.internal_bitdepth, poc);
      FailPlain(message);
    }
    if (decoded_pocs->count(poc)) {
      char message[64];
      std::snprintf(message, sizeof(message),
                    "duplicate decoded poc=%d", poc);
      FailPlain(message);
    }
    decoded_pocs->insert(poc);
    if (run.rec_by_poc.count(poc) == 0) {
      char message[64];
      std::snprintf(message, sizeof(message),
                    "decoder produced unknown poc=%d", poc);
      FailPlain(message);
      continue;
    }
    const std::vector<uint8_t> &rec = run.rec_by_poc.at(poc);
    for (int plane = 0; plane < 3; plane++) {
      const int valid_width =
        planar_intake_ref::PlaneWidth(cfg.width, plane,
                                      cfg.chroma_format);
      const int valid_height =
        planar_intake_ref::PlaneHeight(cfg.height, plane,
                                       cfg.chroma_format);
      const uint8_t *rec_plane =
        rec.data() + geometry[plane].offset;
      const ptrdiff_t rec_stride =
        static_cast<ptrdiff_t>(geometry[plane].width) * sample_size;
      for (int y = 0; y < valid_height; y++) {
        for (int x = 0; x < valid_width; x++) {
          int actual = 0;
          int expected = 0;
          const uint8_t *dst =
            reinterpret_cast<const uint8_t*>(out.planes[plane]) +
            y * out.stride[plane] + x * sample_size;
          const uint8_t *src = rec_plane + y * rec_stride +
            x * sample_size;
          if (sample_size == 1) {
            actual = dst[0];
            expected = src[0];
          } else {
            actual = *reinterpret_cast<const uint16_t*>(dst);
            expected = *reinterpret_cast<const uint16_t*>(src);
          }
          if (actual != expected) {
            Mismatch mismatch;
            mismatch.case_name = g_current_case;
            mismatch.plane = plane;
            mismatch.x = x;
            mismatch.y = y;
            mismatch.frame_index = poc;
            mismatch.expected = expected;
            mismatch.actual = actual;
            Fail(mismatch);
            return;
          }
        }
      }
    }
  }
}

void DecodeAndCompare(const SequenceConfig &cfg, const EncodeRun &run) {
  const xvc_decoder_api *api = xvc_decoder_api_get();
  xvc_decoder_parameters *param = api->parameters_create();
  assert(api->parameters_set_default(param) == XVC_DEC_OK);
  param->threads = 0;
  param->simd_mask = 0;
  xvc_decoder *decoder = api->decoder_create(param);
  assert(decoder);
  api->parameters_destroy(param);

  std::set<int> decoded_pocs;
  for (size_t i = 0; i < run.nals.size(); i++) {
    xvc_dec_return_code rc = api->decoder_decode_nal(
      decoder, run.nals[i].bytes.data(), run.nals[i].bytes.size(),
      run.nals[i].user_data);
    if (rc != XVC_DEC_OK) {
      char message[96];
      std::snprintf(message, sizeof(message),
                    "decoder rejected nal %d rc=%d",
                    static_cast<int>(i), static_cast<int>(rc));
      FailPlain(message);
      api->decoder_destroy(decoder);
      return;
    }
    // The decoder applies a sliding window: drain every available output
    // picture after each NAL so its internal picture buffers are reused
    // rather than overwritten.
    DrainDecoderOutputs(api, decoder, cfg, run, &decoded_pocs);
  }
  assert(api->decoder_flush(decoder) == XVC_DEC_OK);
  DrainDecoderOutputs(api, decoder, cfg, run, &decoded_pocs);
  if (static_cast<int>(decoded_pocs.size()) != cfg.num_frames) {
    char message[96];
    std::snprintf(message, sizeof(message),
                  "decoded %u pictures, expected %d",
                  static_cast<unsigned>(decoded_pocs.size()),
                  cfg.num_frames);
    FailPlain(message);
  }
  int conforming = 0;
  api->decoder_check_conformance(decoder, &conforming);
  if (conforming != 0) {
    FailPlain("decoder reported non-conforming pictures");
  }
  api->decoder_destroy(decoder);
}

void CheckCodingOrder(const SequenceConfig &cfg, const EncodeRun &run) {
  if (static_cast<int>(run.picture_poc_order.size()) != cfg.num_frames) {
    char message[96];
    std::snprintf(message, sizeof(message),
                  "picture nal count %u != %d",
                  static_cast<unsigned>(run.picture_poc_order.size()),
                  cfg.num_frames);
    FailPlain(message);
    return;
  }
  bool has_inversion = false;
  for (size_t i = 1; i < run.picture_poc_order.size(); i++) {
    if (run.picture_poc_order[i] < run.picture_poc_order[i - 1]) {
      has_inversion = true;
      break;
    }
  }
  if (cfg.sub_gop_length == 1) {
    if (has_inversion) {
      FailPlain("gop=1 unexpectedly reordered pictures");
    }
  } else {
    if (!has_inversion) {
      FailPlain("gop=8 produced no display/coding order difference");
    }
  }
}

void CheckResamplerOne(const SequenceConfig &cfg, int frame_index,
                       bool strided_entry, bool use_padding) {
  std::set<xvc::CpuCapability> no_caps;
  xvc::SimdFunctions simd(no_caps);
  xvc::Resampler resampler(simd.resampler);

  const xvc::ChromaFormat chroma =
    static_cast<xvc::ChromaFormat>(cfg.chroma_format);
  xvc::PictureFormat src_format(cfg.width, cfg.height, cfg.input_bitdepth,
                                chroma, xvc::ColorMatrix::kUndefined, false);
  const int internal_width = (cfg.width + 7) / 8 * 8;
  const int internal_height = (cfg.height + 7) / 8 * 8;
  const int crop_width =
    use_padding ? internal_width - cfg.width : 0;
  const int crop_height =
    use_padding ? internal_height - cfg.height : 0;
  xvc::YuvPicture dst_pic(chroma, internal_width, internal_height,
                          cfg.internal_bitdepth, false,
                          crop_width, crop_height);

  if (!strided_entry) {
    // Compact entry through the packed byte overload. With a non multiple
    // size the packed source still enters the source padding branch, only
    // with a compact source stride.
    std::vector<uint8_t> packed =
      planar_intake_ref::BuildPackedFrame(frame_index, cfg);
    resampler.ConvertFrom(src_format, packed.data(), &dst_pic);
  } else {
    // Separate plane entry, with byte strides, including the source
    // padding branch for non multiple sizes.
    std::vector<StridedPlane> planes =
      planar_intake_ref::BuildStridedFrame(frame_index, cfg);
    xvc::Resampler::PicPlanes input_planes;
    for (int plane = 0; plane < 3; plane++) {
      input_planes[plane] =
        std::make_pair(planes[plane].data, planes[plane].stride_bytes);
    }
    resampler.ConvertFrom(src_format, input_planes, &dst_pic);
  }

  // Every stored sample must match the independent upshifted expectation.
  // The YuvPicture was allocated without border margins, so only initialized
  // samples are ever read.
  for (int plane = 0; plane < 3; plane++) {
    const xvc::YuvComponent comp = static_cast<xvc::YuvComponent>(plane);
    const int pic_width = dst_pic.GetWidth(comp);
    const int pic_height = dst_pic.GetHeight(comp);
    const xvc::Sample *src = dst_pic.GetSamplePtr(comp, 0, 0);
    const ptrdiff_t stride = dst_pic.GetStride(comp);
    for (int y = 0; y < pic_height; y++) {
      for (int x = 0; x < pic_width; x++) {
        const int expected = ExpectedPaddedSample(
          plane, x, y, frame_index, cfg);
        const int actual = static_cast<int>(src[y * stride + x]);
        if (actual != expected) {
          Mismatch mismatch;
          mismatch.case_name = g_current_case;
          mismatch.plane = plane;
          mismatch.x = x;
          mismatch.y = y;
          mismatch.frame_index = frame_index;
          mismatch.expected = expected;
          mismatch.actual = actual;
          Fail(mismatch);
          return;
        }
      }
    }
    if (use_padding) {
      // Explicitly prove right/bottom extension of the last valid row/col.
      const int valid_width =
        planar_intake_ref::PlaneWidth(cfg.width, plane,
                                      cfg.chroma_format);
      const int valid_height =
        planar_intake_ref::PlaneHeight(cfg.height, plane,
                                       cfg.chroma_format);
      if (valid_width < pic_width) {
        const xvc::Sample last = src[valid_width - 1];
        const xvc::Sample extended = src[valid_width];
        if (extended != last) {
          Mismatch mismatch;
          mismatch.case_name = g_current_case;
          mismatch.plane = plane;
          mismatch.x = valid_width;
          mismatch.y = 0;
          mismatch.frame_index = frame_index;
          mismatch.expected = static_cast<int>(last);
          mismatch.actual = static_cast<int>(extended);
          Fail(mismatch);
          return;
        }
      }
      if (valid_height < pic_height) {
        const xvc::Sample last =
          src[(valid_height - 1) * stride];
        const xvc::Sample extended =
          src[valid_height * stride];
        if (extended != last) {
          Mismatch mismatch;
          mismatch.case_name = g_current_case;
          mismatch.plane = plane;
          mismatch.x = 0;
          mismatch.y = valid_height;
          mismatch.frame_index = frame_index;
          mismatch.expected = static_cast<int>(last);
          mismatch.actual = static_cast<int>(extended);
          Fail(mismatch);
          return;
        }
      }
    }
  }
  g_num_resampler_cases++;
}

std::vector<SequenceConfig> BuildBitdepthMatrix() {
  std::vector<SequenceConfig> matrix;
  SequenceConfig base;
  base.width = 32;
  base.height = 24;
  base.num_frames = kNumFrames;
  base.qp = kQp;
  const int bd_pairs[][2] = {
    {8, 8},
#if XVC_HIGH_BITDEPTH
    {8, 10},
    {10, 10},
    {10, 12},
#endif
  };
  const ChromaFormat chroma_formats[] = {
    planar_intake_ref::kChroma420,
    planar_intake_ref::kChroma444
  };
  const int sizes[][2] = { {32, 24}, {34, 26} };
  for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    for (size_t c = 0; c < 2; c++) {
      for (size_t b = 0; b < sizeof(bd_pairs) / sizeof(bd_pairs[0]); b++) {
        SequenceConfig cfg = base;
        cfg.width = sizes[s][0];
        cfg.height = sizes[s][1];
        cfg.chroma_format = chroma_formats[c];
        cfg.input_bitdepth = bd_pairs[b][0];
        cfg.internal_bitdepth = bd_pairs[b][1];
        matrix.push_back(cfg);
      }
    }
  }
  return matrix;
}

void RunResamplerChecks() {
  const std::vector<SequenceConfig> matrix = BuildBitdepthMatrix();
  const int frames[] = {0, 5};
  for (size_t i = 0; i < matrix.size(); i++) {
    const SequenceConfig &cfg = matrix[i];
    const bool use_padding =
      cfg.width != ((cfg.width + 7) / 8 * 8) ||
      cfg.height != ((cfg.height + 7) / 8 * 8);
    for (size_t f = 0; f < sizeof(frames) / sizeof(frames[0]); f++) {
      g_current_case = CaseName("resampler-compact", cfg);
      CheckResamplerOne(cfg, frames[f], false, use_padding);
      g_current_case = CaseName("resampler-strided", cfg);
      CheckResamplerOne(cfg, frames[f], true, use_padding);
    }
  }
}

void RunEndToEndChecks() {
  const std::vector<SequenceConfig> matrix = BuildBitdepthMatrix();
  const int gop_lengths[] = {1, 8};
  for (size_t i = 0; i < matrix.size(); i++) {
    for (size_t g = 0; g < 2; g++) {
      SequenceConfig cfg = matrix[i];
      cfg.sub_gop_length = gop_lengths[g];
      g_current_case = CaseName("encode", cfg);

      // Both padding values are above every image pattern sample, so a
      // padding read shows up as a real content difference.
      const int max_padding =
        planar_intake_ref::MaxPaddingValue(cfg.input_bitdepth);
      const int padding_a = max_padding - 1;
      const int padding_b = max_padding;
      EncodeRun packed = RunPacked(cfg);
      EncodeRun planar_a = RunPlanar(cfg, padding_a);
      EncodeRun planar_b = RunPlanar(cfg, padding_b);

      CompareNals(packed, planar_a);
      CompareRecPictures(packed, planar_a);
      // Different padding values must leave the bitstream untouched.
      CompareNals(planar_a, planar_b);
      CompareRecPictures(planar_a, planar_b);

      CheckCodingOrder(cfg, planar_a);
      DecodeAndCompare(cfg, planar_a);
      g_num_end_to_end_cases++;
    }
  }
}

}  // namespace

int main() {
  RunResamplerChecks();
  RunEndToEndChecks();
  std::printf("resampler check cases: %d\n", g_num_resampler_cases);
  std::printf("end-to-end cases: %d\n", g_num_end_to_end_cases);
  if (g_num_resampler_cases == 0 || g_num_end_to_end_cases == 0) {
    std::printf("xvc planar intake: no cases executed\n");
    return 1;
  }
  if (g_num_failures != 0) {
    std::printf("xvc planar intake: %d failures\n", g_num_failures);
    return 1;
  }
  std::printf("xvc planar intake: ok\n");
  return 0;
}
