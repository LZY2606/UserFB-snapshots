/*
 *  Copyright 2025 The LibYuv Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS. All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// Standalone tests for the v210 packed 10 bit 4:2:2 format.  This file has
// its own main() and does not use GoogleTest, so it can be built and run in
// environments without the test dependencies.  The expected v210 words are
// produced two independent ways: hand computed constants for one group, and
// a slot-by-slot reference packer written straight from the format spec.

#include "libyuv.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                        \
  do {                                                           \
    if (!(cond)) {                                               \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

#define EXPECT_EQ(a, b) EXPECT_TRUE((a) == (b))

const int kSentinel = 0xA5;
const int kPlaneSentinel = 0xCC;

uint32_t ReadLE32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

void WriteLE32(uint8_t* p, uint32_t v) {
  memcpy(p, &v, sizeof(v));
}

// Deterministic 10 bit sample values.  Deliberately include small values so
// the lowest two bits are exercised.
uint16_t YValue(int row, int x) {
  return static_cast<uint16_t>((row * 131 + x * 17 + 7) & 0x3FF);
}
uint16_t UValue(int row, int x) {
  return static_cast<uint16_t>((row * 257 + x * 31 + 11) & 0x3FF);
}
uint16_t VValue(int row, int x) {
  return static_cast<uint16_t>((1023 - UValue(row, x)) & 0x3FF);
}

int GroupCount(int width) {
  return (width + 5) / 6;
}

// Independent v210 packer: every named slot is placed separately straight
// from the specification.  Slots without a sample stay zero.
void RefPackRow(const uint16_t* y,
                const uint16_t* u,
                const uint16_t* v,
                int width,
                uint8_t* out) {
  const int groups = GroupCount(width);
  for (int g = 0; g < groups; ++g) {
    const int remaining = width - g * 6 > 6 ? 6 : width - g * 6;
    uint32_t word[4] = {0, 0, 0, 0};
    if (remaining >= 1) {
      word[0] |= u[g * 3 + 0] & 0x3FF;
      word[0] |= (y[g * 6 + 0] & 0x3FF) << 10;
      word[0] |= (v[g * 3 + 0] & 0x3FF) << 20;
    }
    if (remaining >= 2) {
      word[1] |= y[g * 6 + 1] & 0x3FF;
    }
    if (remaining >= 3) {
      word[1] |= (u[g * 3 + 1] & 0x3FF) << 10;
      word[1] |= (y[g * 6 + 2] & 0x3FF) << 20;
    }
    if (remaining >= 4) {
      word[2] |= v[g * 3 + 1] & 0x3FF;
      word[2] |= (y[g * 6 + 3] & 0x3FF) << 10;
    }
    if (remaining >= 5) {
      word[2] |= (u[g * 3 + 2] & 0x3FF) << 20;
      word[3] |= y[g * 6 + 4] & 0x3FF;
      word[3] |= (v[g * 3 + 2] & 0x3FF) << 10;
    }
    if (remaining >= 6) {
      word[3] |= (y[g * 6 + 5] & 0x3FF) << 20;
    }
    for (int k = 0; k < 4; ++k) {
      WriteLE32(out + g * 16 + k * 4, word[k]);
    }
  }
}

// Independent v210 unpacker, mirroring RefPackRow slot by slot.  Writes only
// the samples that exist.
void RefUnpackRow(const uint8_t* in,
                  int width,
                  uint16_t* y,
                  uint16_t* u,
                  uint16_t* v) {
  const int groups = GroupCount(width);
  for (int g = 0; g < groups; ++g) {
    const int remaining = width - g * 6 > 6 ? 6 : width - g * 6;
    const uint32_t word0 = ReadLE32(in + g * 16);
    const uint32_t word1 = ReadLE32(in + g * 16 + 4);
    const uint32_t word2 = ReadLE32(in + g * 16 + 8);
    const uint32_t word3 = ReadLE32(in + g * 16 + 12);
    if (remaining >= 1) {
      u[g * 3 + 0] = static_cast<uint16_t>(word0 & 0x3FF);
      y[g * 6 + 0] = static_cast<uint16_t>((word0 >> 10) & 0x3FF);
      v[g * 3 + 0] = static_cast<uint16_t>((word0 >> 20) & 0x3FF);
    }
    if (remaining >= 2) {
      y[g * 6 + 1] = static_cast<uint16_t>(word1 & 0x3FF);
    }
    if (remaining >= 3) {
      u[g * 3 + 1] = static_cast<uint16_t>((word1 >> 10) & 0x3FF);
      y[g * 6 + 2] = static_cast<uint16_t>((word1 >> 20) & 0x3FF);
    }
    if (remaining >= 4) {
      v[g * 3 + 1] = static_cast<uint16_t>(word2 & 0x3FF);
      y[g * 6 + 3] = static_cast<uint16_t>((word2 >> 10) & 0x3FF);
    }
    if (remaining >= 5) {
      u[g * 3 + 2] = static_cast<uint16_t>((word2 >> 20) & 0x3FF);
      y[g * 6 + 4] = static_cast<uint16_t>(word3 & 0x3FF);
      v[g * 3 + 2] = static_cast<uint16_t>((word3 >> 10) & 0x3FF);
    }
    if (remaining >= 6) {
      y[g * 6 + 5] = static_cast<uint16_t>((word3 >> 20) & 0x3FF);
    }
  }
}

// One group with hand computed word values.  Inputs:
//   U = {11, 22, 33}, Y = {101..106}, V = {44, 55, 66}
//   word0 = U0 | Y0<<10 | V0<<20 = 0x02C1940B
//   word1 = Y1 | U1<<10 | Y2<<20 = 0x06705866
//   word2 = V1 | Y3<<10 | U2<<20 = 0x0211A037
//   word3 = Y4 | V2<<10 | Y5<<20 = 0x06A10869
void TestHandwrittenWords() {
  static const uint16_t u[3] = {11, 22, 33};
  static const uint16_t y[6] = {101, 102, 103, 104, 105, 106};
  static const uint16_t v[3] = {44, 55, 66};
  static const uint32_t kExpected[4] = {0x02C1940Bu, 0x06705866u,
                                        0x0211A037u, 0x06A10869u};
  uint8_t packed[16];
  uint16_t got_y[6];
  uint16_t got_u[3];
  uint16_t got_v[3];
  // Set the two unused high bits of every word; unpacking must ignore them.
  for (int i = 0; i < 4; ++i) {
    WriteLE32(packed + i * 4, kExpected[i] | 0xC0000000u);
  }
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, got_y, 6, got_u, 3, got_v, 3, 6, 1),
            0);
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(got_y[i], y[i]);
  }
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(got_u[i], u[i]);
    EXPECT_EQ(got_v[i], v[i]);
  }
  // Packing must write the high bits zero and match the hand constants.
  memset(packed, 0xFF, sizeof(packed));
  EXPECT_EQ(libyuv::I210ToV210(y, 6, u, 3, v, 3, packed, 16, 6, 1), 0);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(ReadLE32(packed + i * 4), kExpected[i]);
  }
}

// Programmatic multi-row images at several widths, with padded strides,
// misaligned packed addresses, row-tail sentinels and both height directions.
void TestFrameRoundTrips(int width, int height) {
  const int halfwidth = width / 2;
  const int groups = GroupCount(width);
  const int row_bytes = groups * 16;
  const int packed_stride = row_bytes + 13;
  const int y_stride = width + 5;
  const int uv_stride = halfwidth + 3;
  const int rows = height < 0 ? -height : height;

  // Source I210 planes, non compact.
  uint16_t* src_y =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * y_stride * rows));
  uint16_t* src_u =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * uv_stride * rows));
  uint16_t* src_v =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * uv_stride * rows));
  // Packed buffers, deliberately starting at an odd address.
  uint8_t* packed_storage =
      static_cast<uint8_t*>(malloc(packed_stride * rows + 4));
  uint8_t* ref_packed = packed_storage + 1;
  uint8_t* lib_packed_storage =
      static_cast<uint8_t*>(malloc(packed_stride * rows + 4));
  uint8_t* lib_packed = lib_packed_storage + 3;
  // Destination I210 planes, non compact, prefilled with sentinels.
  uint16_t* dst_y =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * y_stride * rows));
  uint16_t* dst_u =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * uv_stride * rows));
  uint16_t* dst_v =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * uv_stride * rows));

  for (int r = 0; r < rows; ++r) {
    for (int x = 0; x < width; ++x) {
      src_y[r * y_stride + x] = YValue(r, x);
    }
    for (int x = 0; x < halfwidth; ++x) {
      src_u[r * uv_stride + x] = UValue(r, x);
      src_v[r * uv_stride + x] = VValue(r, x);
    }
  }
  memset(ref_packed, kSentinel, packed_stride * rows);
  memset(lib_packed, kSentinel, packed_stride * rows);
  for (int r = 0; r < rows; ++r) {
    RefPackRow(src_y + r * y_stride, src_u + r * uv_stride,
               src_v + r * uv_stride, width, ref_packed + r * packed_stride);
  }

  // I210 -> v210 must match the independent packer on the group bytes and
  // leave the row padding untouched.
  EXPECT_EQ(libyuv::I210ToV210(src_y, y_stride, src_u, uv_stride, src_v,
                               uv_stride, lib_packed, packed_stride, width,
                               height),
            0);
  for (int r = 0; r < rows; ++r) {
    const int out_row = height < 0 ? rows - 1 - r : r;
    for (int b = 0; b < row_bytes; ++b) {
      EXPECT_EQ(lib_packed[out_row * packed_stride + b],
                ref_packed[r * packed_stride + b]);
    }
    for (int b = row_bytes; b < packed_stride; ++b) {
      EXPECT_EQ(lib_packed[out_row * packed_stride + b], kSentinel);
    }
  }

  // v210 -> I210 from the independent packing.  Real samples must match,
  // padding cells must keep their sentinel values.
  memset(dst_y, kPlaneSentinel, sizeof(uint16_t) * y_stride * rows);
  memset(dst_u, kPlaneSentinel, sizeof(uint16_t) * uv_stride * rows);
  memset(dst_v, kPlaneSentinel, sizeof(uint16_t) * uv_stride * rows);
  EXPECT_EQ(libyuv::V210ToI210(ref_packed, packed_stride, dst_y, y_stride,
                               dst_u, uv_stride, dst_v, uv_stride, width,
                               height),
            0);
  for (int r = 0; r < rows; ++r) {
    const int src_row = height < 0 ? rows - 1 - r : r;
    for (int x = 0; x < width; ++x) {
      EXPECT_EQ(dst_y[r * y_stride + x], YValue(src_row, x));
    }
    for (int x = 0; x < halfwidth; ++x) {
      EXPECT_EQ(dst_u[r * uv_stride + x], UValue(src_row, x));
      EXPECT_EQ(dst_v[r * uv_stride + x], VValue(src_row, x));
    }
    for (int x = width; x < y_stride; ++x) {
      EXPECT_EQ(dst_y[r * y_stride + x],
                static_cast<uint16_t>(kPlaneSentinel | (kPlaneSentinel << 8)));
    }
    for (int x = halfwidth; x < uv_stride; ++x) {
      EXPECT_EQ(dst_u[r * uv_stride + x],
                static_cast<uint16_t>(kPlaneSentinel | (kPlaneSentinel << 8)));
      EXPECT_EQ(dst_v[r * uv_stride + x],
                static_cast<uint16_t>(kPlaneSentinel | (kPlaneSentinel << 8)));
    }
  }

  // The words produced by the library must also decode through the
  // independent unpacker back to the original samples.
  for (int r = 0; r < rows; ++r) {
    uint16_t check_y[64];
    uint16_t check_u[32];
    uint16_t check_v[32];
    const int src_row = height < 0 ? rows - 1 - r : r;
    RefUnpackRow(lib_packed + r * packed_stride, width, check_y, check_u,
                 check_v);
    for (int x = 0; x < width; ++x) {
      EXPECT_EQ(check_y[x], YValue(src_row, x));
    }
    for (int x = 0; x < halfwidth; ++x) {
      EXPECT_EQ(check_u[x], UValue(src_row, x));
      EXPECT_EQ(check_v[x], VValue(src_row, x));
    }
  }

  free(src_y);
  free(src_u);
  free(src_v);
  free(packed_storage);
  free(lib_packed_storage);
  free(dst_y);
  free(dst_u);
  free(dst_v);
}

// width=8 leaves a partial second group: one chroma pair and two luma only.
void TestPartialGroup() {
  const int width = 8;
  uint16_t y[8];
  uint16_t u[4];
  uint16_t v[4];
  uint16_t dy[8];
  uint16_t du[4];
  uint16_t dv[4];
  uint8_t packed[32];
  for (int x = 0; x < 8; ++x) {
    y[x] = static_cast<uint16_t>(200 + x);
  }
  for (int x = 0; x < 4; ++x) {
    u[x] = static_cast<uint16_t>(100 + x);
    v[x] = static_cast<uint16_t>(900 - x);
  }
  memset(packed, 0xFF, sizeof(packed));
  EXPECT_EQ(libyuv::I210ToV210(y, 8, u, 4, v, 4, packed, 32, width, 1), 0);
  // The full second group is written, with the slots past the row zeroed:
  // word2 holds V1/Y3/U2 and word3 holds Y4/V2/Y5; for width=8 only V1, Y3
  // (word2) and Y4, V2 (word3) may be nonzero.
  EXPECT_EQ(ReadLE32(packed + 16 + 8) & 0x3FF00000u, 0u);  // U2 slot
  EXPECT_EQ(ReadLE32(packed + 16 + 12) & 0x3FF00000u, 0u);  // Y5 slot
  memset(dy, 0x77, sizeof(dy));
  memset(du, 0x77, sizeof(du));
  memset(dv, 0x77, sizeof(dv));
  EXPECT_EQ(libyuv::V210ToI210(packed, 32, dy, 8, du, 4, dv, 4, width, 1), 0);
  for (int x = 0; x < 8; ++x) {
    EXPECT_EQ(dy[x], y[x]);
  }
  for (int x = 0; x < 4; ++x) {
    EXPECT_EQ(du[x], u[x]);
    EXPECT_EQ(dv[x], v[x]);
  }
  // Unused high bits in the source words must be ignored.
  WriteLE32(packed, ReadLE32(packed) | 0xC0000000u);
  EXPECT_EQ(libyuv::V210ToI210(packed, 32, dy, 8, du, 4, dv, 4, width, 1), 0);
  EXPECT_EQ(dy[0], y[0]);
  EXPECT_EQ(du[0], u[0]);
  EXPECT_EQ(dv[0], v[0]);
}

// Compare ConvertToI420 against the reference process: lossless unpack to
// I210, rectangular crop, then the existing 10 bit 4:2:2 to I420 path.
void TestConvertToI420(int src_width,
                       int src_height,
                       int crop_x,
                       int crop_y,
                       int crop_width,
                       int crop_height,
                       libyuv::RotationMode rotation) {
  const int packed_stride = 128 * ((src_width + 47) / 48);
  const int abs_src_height = src_height < 0 ? -src_height : src_height;
  const size_t frame_size =
      static_cast<size_t>(packed_stride) * abs_src_height;
  uint8_t* packed = static_cast<uint8_t*>(malloc(frame_size));
  const int y_stride = src_width;
  const int uv_stride = src_width / 2;
  uint16_t* i210_y =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * y_stride *
                                    abs_src_height));
  uint16_t* i210_u =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * uv_stride *
                                    abs_src_height));
  uint16_t* i210_v =
      static_cast<uint16_t*>(malloc(sizeof(uint16_t) * uv_stride *
                                    abs_src_height));
  for (int r = 0; r < abs_src_height; ++r) {
    for (int x = 0; x < src_width; ++x) {
      i210_y[r * y_stride + x] = YValue(r, x);
    }
    for (int x = 0; x < uv_stride; ++x) {
      i210_u[r * uv_stride + x] = UValue(r, x);
      i210_v[r * uv_stride + x] = VValue(r, x);
    }
  }
  for (int r = 0; r < abs_src_height; ++r) {
    RefPackRow(i210_y + r * y_stride, i210_u + r * uv_stride,
               i210_v + r * uv_stride, src_width,
               packed + r * packed_stride);
  }

  // Reference I420 of the crop, honoring the same vertical flip convention.
  // A negative crop height performs the row inversion itself, so the source
  // pointer always starts at the top row of the crop region.
  const int inv_crop_height =
      src_height < 0 ? -crop_height : crop_height;
  const int ref_start_row = crop_y;
  uint8_t* ref_y =
      static_cast<uint8_t*>(malloc(crop_width * crop_height));
  uint8_t* ref_u = static_cast<uint8_t*>(
      malloc((crop_width / 2) * ((crop_height + 1) / 2)));
  uint8_t* ref_v = static_cast<uint8_t*>(
      malloc((crop_width / 2) * ((crop_height + 1) / 2)));
  EXPECT_EQ(libyuv::I210ToI420(
                i210_y + ref_start_row * y_stride + crop_x, y_stride,
                i210_u + ref_start_row * uv_stride + crop_x / 2, uv_stride,
                i210_v + ref_start_row * uv_stride + crop_x / 2, uv_stride,
                ref_y, crop_width, ref_u, crop_width / 2, ref_v,
                crop_width / 2, crop_width, inv_crop_height),
            0);

  // Rotated reference for 90/180/270.
  const int rotated_width =
      (rotation == libyuv::kRotate90 || rotation == libyuv::kRotate270)
          ? crop_height
          : crop_width;
  const int rotated_height =
      (rotation == libyuv::kRotate90 || rotation == libyuv::kRotate270)
          ? crop_width
          : crop_height;
  uint8_t* expected_y =
      static_cast<uint8_t*>(malloc(rotated_width * rotated_height));
  const int expected_uv_width = rotated_width / 2;
  const int expected_uv_height = (rotated_height + 1) / 2;
  uint8_t* expected_u =
      static_cast<uint8_t*>(malloc(expected_uv_width * expected_uv_height));
  uint8_t* expected_v =
      static_cast<uint8_t*>(malloc(expected_uv_width * expected_uv_height));
  EXPECT_EQ(libyuv::I420Rotate(ref_y, crop_width, ref_u, crop_width / 2,
                               ref_v, crop_width / 2, expected_y,
                               rotated_width, expected_u, expected_uv_width,
                               expected_v, expected_uv_width, crop_width,
                               crop_height, rotation),
            0);

  uint8_t* dst_y =
      static_cast<uint8_t*>(malloc(rotated_width * rotated_height));
  uint8_t* dst_u =
      static_cast<uint8_t*>(malloc(expected_uv_width * expected_uv_height));
  uint8_t* dst_v =
      static_cast<uint8_t*>(malloc(expected_uv_width * expected_uv_height));
  EXPECT_EQ(libyuv::ConvertToI420(
                packed, frame_size, dst_y, rotated_width, dst_u,
                expected_uv_width, dst_v, expected_uv_width, crop_x, crop_y,
                src_width, src_height, crop_width, crop_height, rotation,
                libyuv::FOURCC_V210),
            0);
  EXPECT_EQ(memcmp(dst_y, expected_y, rotated_width * rotated_height), 0);
  EXPECT_EQ(memcmp(dst_u, expected_u,
                   expected_uv_width * expected_uv_height),
            0);
  EXPECT_EQ(memcmp(dst_v, expected_v,
                   expected_uv_width * expected_uv_height),
            0);

  free(packed);
  free(i210_y);
  free(i210_u);
  free(i210_v);
  free(ref_y);
  free(ref_u);
  free(ref_v);
  free(expected_y);
  free(expected_u);
  free(expected_v);
  free(dst_y);
  free(dst_u);
  free(dst_v);
}

// Small frame whose values exercise the lowest two 10 bit bits, compared
// against the real I210 -> I420 output.
void TestLowBitsConvertToI420() {
  const int width = 8;
  const int height = 2;
  const int packed_stride = 128;
  uint16_t y[16];
  uint16_t u[8];
  uint16_t v[8];
  for (int i = 0; i < 16; ++i) {
    y[i] = static_cast<uint16_t>(i & 3);  // 0,1,2,3 repeating.
  }
  for (int i = 0; i < 8; ++i) {
    u[i] = static_cast<uint16_t>((i + 1) & 3);
    v[i] = static_cast<uint16_t>((3 - i) & 3);
  }
  uint8_t packed[256];
  memset(packed, 0, sizeof(packed));
  for (int r = 0; r < height; ++r) {
    RefPackRow(y + r * width, u + r * (width / 2), v + r * (width / 2), width,
               packed + r * packed_stride);
  }
  uint8_t direct_y[16];
  uint8_t direct_u[8];
  uint8_t direct_v[8];
  EXPECT_EQ(libyuv::I210ToI420(y, width, u, width / 2, v, width / 2, direct_y,
                               width, direct_u, width / 2, direct_v,
                               width / 2, width, height),
            0);
  uint8_t got_y[16];
  uint8_t got_u[8];
  uint8_t got_v[8];
  EXPECT_EQ(libyuv::ConvertToI420(packed, sizeof(packed), got_y, width, got_u,
                                  width / 2, got_v, width / 2, 0, 0, width,
                                  height, width, height, libyuv::kRotate0,
                                  libyuv::FOURCC_V210),
            0);
  // Only the valid I420 cells are defined: width*height luma and
  // (width/2) * ceil(height/2) chroma samples.
  EXPECT_EQ(memcmp(got_y, direct_y, width * height), 0);
  const size_t uv_bytes =
      static_cast<size_t>(width / 2) * ((height + 1) / 2);
  EXPECT_EQ(memcmp(got_u, direct_u, uv_bytes), 0);
  EXPECT_EQ(memcmp(got_v, direct_v, uv_bytes), 0);
}

// Rejected calls must return -1 before touching any destination cell.
void TestRejections() {
  uint16_t y[16];
  uint16_t u[8];
  uint16_t v[8];
  uint8_t packed[128];
  uint8_t plane_guard[64];
  memset(y, 0x11, sizeof(y));
  memset(u, 0x22, sizeof(u));
  memset(v, 0x33, sizeof(v));
  memset(packed, 0x44, sizeof(packed));

  memset(plane_guard, 0x77, sizeof(plane_guard));
  EXPECT_EQ(libyuv::V210ToI210(nullptr, 16, y, 6, u, 3, v, 3, 6, 1), -1);
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, nullptr, 6, u, 3, v, 3, 6, 1), -1);
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, y, 6, u, 3, v, 3, 0, 1), -1);
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, y, 6, u, 3, v, 3, 1, 1), -1);
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, y, 6, u, 3, v, 3, 3, 1), -1);
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, y, 6, u, 3, v, 3, 6, 0), -1);
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, y, 6, u, 3, v, 3, 6, INT_MIN), -1);
  // Positive stride too short for the 16 byte group.
  EXPECT_EQ(libyuv::V210ToI210(packed, 8, y, 6, u, 3, v, 3, 6, 1), -1);
  // Plane stride too short for the row.
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, y, 5, u, 3, v, 3, 6, 1), -1);
  // Overflow in the plane span computation.
  EXPECT_EQ(libyuv::V210ToI210(packed, INT_MAX, y, 6, u, 3, v, 3, 6, 3), -1);
  EXPECT_EQ(libyuv::V210ToI210(packed, 16, y, INT_MAX, u, 3, v, 3, 6, 3), -1);
  // Huge even width rejected.
  EXPECT_EQ(libyuv::V210ToI210(packed, INT_MAX, y, INT_MAX, u, 3, v, 3,
                               INT_MAX - 1, 1),
            -1);

  EXPECT_EQ(libyuv::I210ToV210(nullptr, 6, u, 3, v, 3, packed, 16, 6, 1), -1);
  EXPECT_EQ(libyuv::I210ToV210(y, 6, u, 3, v, 3, nullptr, 16, 6, 1), -1);
  EXPECT_EQ(libyuv::I210ToV210(y, 6, u, 3, v, 3, packed, 16, 2, 0), -1);
  EXPECT_EQ(libyuv::I210ToV210(y, 6, u, 3, v, 3, packed, 16, 2, INT_MIN), -1);
  EXPECT_EQ(libyuv::I210ToV210(y, 6, u, 3, v, 3, packed, 8, 6, 1), -1);
  EXPECT_EQ(libyuv::I210ToV210(y, INT_MAX, u, 3, v, 3, packed, 16, 6, 3), -1);

  // A failed conversion must not modify the destination.
  memset(plane_guard, 0x77, sizeof(plane_guard));
  EXPECT_EQ(libyuv::V210ToI210(packed, 8,
                               reinterpret_cast<uint16_t*>(plane_guard), 6,
                               reinterpret_cast<uint16_t*>(plane_guard + 32),
                               3,
                               reinterpret_cast<uint16_t*>(plane_guard + 48),
                               3, 6, 1),
            -1);
  for (unsigned char i : plane_guard) {
    EXPECT_EQ(i, 0x77);
  }
  memset(plane_guard, 0x77, sizeof(plane_guard));
  EXPECT_EQ(libyuv::I210ToV210(y, 6, u, 3, v, 3, plane_guard, 8, 6, 1), -1);
  for (unsigned char i : plane_guard) {
    EXPECT_EQ(i, 0x77);
  }

  // ConvertToI420 dispatch rejections.  width=8, height=2, stride 128.
  const size_t frame_size = 256;
  uint8_t dst_y[16];
  uint8_t dst_u[8];
  uint8_t dst_v[8];
  memset(dst_y, 0x77, sizeof(dst_y));
  memset(dst_u, 0x77, sizeof(dst_u));
  memset(dst_v, 0x77, sizeof(dst_v));
  const uint32_t fourcc = libyuv::FOURCC_V210;
  // sample_size one byte short of the full frame.
  EXPECT_EQ(libyuv::ConvertToI420(packed, frame_size - 1, dst_y, 8, dst_u, 4,
                                  dst_v, 4, 0, 0, 8, 2, 8, 2,
                                  libyuv::kRotate0, fourcc),
            -1);
  // Odd source width.
  EXPECT_EQ(libyuv::ConvertToI420(packed, frame_size, dst_y, 8, dst_u, 4,
                                  dst_v, 4, 0, 0, 9, 2, 8, 2,
                                  libyuv::kRotate0, fourcc),
            -1);
  // Odd crop origin and odd crop width.
  EXPECT_EQ(libyuv::ConvertToI420(packed, frame_size, dst_y, 8, dst_u, 4,
                                  dst_v, 4, 1, 0, 8, 2, 6, 2,
                                  libyuv::kRotate0, fourcc),
            -1);
  EXPECT_EQ(libyuv::ConvertToI420(packed, frame_size, dst_y, 8, dst_u, 4,
                                  dst_v, 4, 0, 0, 8, 2, 7, 2,
                                  libyuv::kRotate0, fourcc),
            -1);
  for (uint8_t b : dst_y) {
    EXPECT_EQ(b, 0x77);
  }
  for (uint8_t b : dst_u) {
    EXPECT_EQ(b, 0x77);
  }
  for (uint8_t b : dst_v) {
    EXPECT_EQ(b, 0x77);
  }
}

}  // namespace

int main() {
  TestHandwrittenWords();
  static const int kWidths[] = {2, 6, 8, 50};
  for (int width : kWidths) {
    TestFrameRoundTrips(width, 3);
    TestFrameRoundTrips(width, -3);
  }
  TestPartialGroup();
  // Cross-group crop: crop_x=4 sits inside group 0 and crop_width=8 spans
  // parts of two groups.
  static const libyuv::RotationMode kRotations[] = {
      libyuv::kRotate0, libyuv::kRotate90, libyuv::kRotate180,
      libyuv::kRotate270};
  for (libyuv::RotationMode rotation : kRotations) {
    TestConvertToI420(50, 6, 4, 1, 8, 4, rotation);
    TestConvertToI420(50, -6, 4, 1, 8, 4, rotation);
  }
  // Full-width crop on the small 128 byte stride as well.
  for (libyuv::RotationMode rotation : kRotations) {
    TestConvertToI420(8, 4, 0, 0, 8, 4, rotation);
  }
  TestLowBitsConvertToI420();
  TestRejections();
  if (g_failures) {
    printf("v210_test: %d failure(s)\n", g_failures);
    return 1;
  }
  puts("v210_test: all checks passed");
  return 0;
}
