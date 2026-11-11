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

// Independent reference helpers for the planar intake regression.
// Nothing in this file may call production copy, padding or bit depth
// conversion helpers: every expected sample is derived directly from the
// deterministic input pattern. The only xvc headers used are for enums and
// constants.

#ifndef TESTS_PLANAR_INTAKE_REF_H_
#define TESTS_PLANAR_INTAKE_REF_H_

#include <stdint.h>

#include <cstddef>
#include <string>
#include <vector>

#include "xvc_common_lib/common.h"

namespace planar_intake_ref {

enum ChromaFormat {
  kChroma420 = 1,
  kChroma444 = 3,
};

// Full description of one deterministic test sequence.
struct SequenceConfig {
  int width = 0;
  int height = 0;
  int input_bitdepth = 8;
  int internal_bitdepth = 8;
  ChromaFormat chroma_format = kChroma420;
  int sub_gop_length = 1;
  int num_frames = 17;
  int qp = 27;
  // Value written into stride padding and the front/back guard regions.
  // It must stay within [0, MaxPaddingValue()] so it can never collide
  // with an image pattern sample.
  int padding_value = 0;
};

// Largest legal padding value: one above every pattern sample in the given
// input bit depth, and representable in that bit depth.
int MaxPaddingValue(int input_bitdepth);

// One byte buffer per plane, each with guard bytes before and after the
// image area, a positive byte stride and uint16_t aligned start addresses.
struct StridedPlane {
  // uint16_t backing storage guarantees the image start is sample aligned
  // even for 10/12 bit input; data views the same memory as bytes.
  std::vector<uint16_t> aligned_storage;
  uint8_t *data = nullptr;       // First sample of the image area.
  ptrdiff_t stride_bytes = 0;    // Positive stride, unit is bytes.
  int width = 0;
  int height = 0;
};

// Size of the guard regions placed before and after each plane.
const int kGuardBytes = 4;

// Returns the chroma plane dimensions for a luma size.
int ScaleX(int size, ChromaFormat chroma_format);
int ScaleY(int size, ChromaFormat chroma_format);

// Valid (display) dimensions of any plane: luma keeps the full size.
int PlaneWidth(int luma_width, int plane, ChromaFormat chroma_format);
int PlaneHeight(int luma_height, int plane, ChromaFormat chroma_format);

// Deterministic input sample in the *input* bit depth for a sample of the
// given plane (0=Y, 1=U, 2=V). Every plane uses a distinct horizontal and
// vertical pattern and the U and V patterns are never equal.
int PatternSample(int plane, int x, int y, int frame_index,
                  int input_bitdepth);

// The same input sample expressed at internal bit depth, i.e. the value the
// encoder is expected to store internally (left shift by the bit depth
// difference).
int ExpectedInternalSample(int plane, int x, int y, int frame_index,
                           const SequenceConfig &config);

// Fills a complete strided plane: image area from the pattern, stride
// padding and guard regions with padding_value. High bit depth buffers are
// uint16_t aligned; odd byte alignment is intentionally not supported.
void FillPlane(int plane, int frame_index, const SequenceConfig &config,
               StridedPlane *out_plane);

// Builds all three planes of one frame.
std::vector<StridedPlane>
BuildStridedFrame(int frame_index, const SequenceConfig &config);

// Independently compacts the valid image area of one frame into one packed
// byte array, exactly the way the public encoder_encode entry point expects.
// This never calls the production ConvertFrom code.
std::vector<uint8_t>
BuildPackedFrame(int frame_index, const SequenceConfig &config);

// Overwrites the image area of every plane with a new pattern while leaving
// the padding/guard bytes untouched. Used to prove the encoder does not keep
// references to a consumed source buffer.
void RefillFrame(int frame_index, const SequenceConfig &config,
                 std::vector<StridedPlane> *planes);

// Expected internal sample on the padded encoder picture, including right
// and bottom extension (last valid column/row repeated). The padded picture
// dimensions are derived like the production segment header does.
int ExpectedPaddedSample(int plane, int padded_x, int padded_y,
                         int frame_index, const SequenceConfig &config);

// Description of the first differing sample, used by every assertion so a
// failure always reports the test case, plane and coordinates.
struct Mismatch {
  std::string case_name;
  int plane = -1;
  int x = -1;
  int y = -1;
  int frame_index = -1;
  int expected = -1;
  int actual = -1;
  std::string detail;
};

}  // namespace planar_intake_ref

#endif  // TESTS_PLANAR_INTAKE_REF_H_
