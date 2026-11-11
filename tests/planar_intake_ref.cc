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

// Independent reference implementation, see planar_intake_ref.h.
// All expected values are recomputed from the pattern formulas below; no
// production resampler, padding or bit shift helper is used.

#include "planar_intake_ref.h"

#include <assert.h>

#include <algorithm>

namespace planar_intake_ref {

namespace {

// Image samples stay below the input code maximum so that the padding value
// (the full-scale value) can never collide with an image sample.
int InputRange(int input_bitdepth) {
  return (1 << input_bitdepth) - 16;
}

int SampleSize(int bitdepth) {
  return bitdepth > 8 ? 2 : 1;
}

}  // namespace

int ScaleX(int size, ChromaFormat chroma_format) {
  return chroma_format == kChroma444 ? size : (size >> 1);
}

int ScaleY(int size, ChromaFormat chroma_format) {
  return chroma_format == kChroma444 ? size : (size >> 1);
}

int MaxPaddingValue(int input_bitdepth) {
  return (1 << input_bitdepth) - 15;
}

int PlaneWidth(int luma_width, int plane, ChromaFormat chroma_format) {
  return plane == 0 ? luma_width : ScaleX(luma_width, chroma_format);
}

int PlaneHeight(int luma_height, int plane, ChromaFormat chroma_format) {
  return plane == 0 ? luma_height : ScaleY(luma_height, chroma_format);
}

int PatternSample(int plane, int x, int y, int frame_index,
                  int input_bitdepth) {
  const int range = InputRange(input_bitdepth);
  int value = 0;
  switch (plane) {
    case 0:  // Y: strong horizontal component
      value = 17 * x + 31 * y + 7 * frame_index + 13;
      break;
    case 1:  // U: different horizontal and vertical factors
      value = 23 * x + 11 * y + 5 * frame_index + 29;
      break;
    default:  // V: yet another independent pattern
      value = 29 * x + 37 * y + 13 * frame_index + 41;
      break;
  }
  value %= range;
  if (value < 0) {
    value += range;
  }
  return value;
}

int ExpectedInternalSample(int plane, int x, int y, int frame_index,
                           const SequenceConfig &config) {
  const int input_sample =
    PatternSample(plane, x, y, frame_index, config.input_bitdepth);
  return input_sample <<
    (config.internal_bitdepth - config.input_bitdepth);
}

void FillPlane(int plane, int frame_index, const SequenceConfig &config,
               StridedPlane *out_plane) {
  assert(config.padding_value >= 0 &&
         config.padding_value <= MaxPaddingValue(config.input_bitdepth));
  const int plane_width =
    PlaneWidth(config.width, plane, config.chroma_format);
  const int plane_height =
    PlaneHeight(config.height, plane, config.chroma_format);
  const int sample_size = SampleSize(config.input_bitdepth);
  // A different positive padding per plane keeps a stride bug from being
  // masked by identical neighboring rows. The extra width is at least one
  // full sample and always keeps the stride sample aligned.
  const int extra_samples = 3 + plane;
  const ptrdiff_t stride =
    static_cast<ptrdiff_t>((plane_width + extra_samples) * sample_size);
  assert(kGuardBytes % sample_size == 0);
  const size_t total_bytes =
    static_cast<size_t>(kGuardBytes) +
    static_cast<size_t>(stride * plane_height) + kGuardBytes;

  // Back with uint16_t elements so the image start can be sample aligned.
  out_plane->aligned_storage.assign(total_bytes / sizeof(uint16_t) + 1, 0);
  uint8_t *raw_bytes =
    reinterpret_cast<uint8_t*>(out_plane->aligned_storage.data());
  const size_t raw_size =
    out_plane->aligned_storage.size() * sizeof(uint16_t);
  out_plane->data = raw_bytes + kGuardBytes;
  out_plane->stride_bytes = stride;
  out_plane->width = plane_width;
  out_plane->height = plane_height;
  assert(reinterpret_cast<uintptr_t>(out_plane->data) % alignof(uint16_t) == 0);

  // Fill every byte (guards and row-end padding) with the padding value.
  if (sample_size == 1) {
    std::fill(raw_bytes, raw_bytes + raw_size,
              static_cast<uint8_t>(config.padding_value));
  } else {
    for (size_t i = 0; i + 1 < raw_size; i += 2) {
      raw_bytes[i] =
        static_cast<uint8_t>(config.padding_value & 0xff);
      raw_bytes[i + 1] =
        static_cast<uint8_t>((config.padding_value >> 8) & 0xff);
    }
  }

  // Write only the valid image area from the independent pattern.
  for (int y = 0; y < plane_height; y++) {
    for (int x = 0; x < plane_width; x++) {
      const int sample =
        PatternSample(plane, x, y, frame_index, config.input_bitdepth);
      uint8_t *dst = out_plane->data + y * stride + x * sample_size;
      if (sample_size == 1) {
        dst[0] = static_cast<uint8_t>(sample);
      } else {
        uint16_t *dst16 = reinterpret_cast<uint16_t*>(dst);
        *dst16 = static_cast<uint16_t>(sample);
      }
    }
  }
}

std::vector<StridedPlane>
BuildStridedFrame(int frame_index, const SequenceConfig &config) {
  std::vector<StridedPlane> planes(3);
  for (int plane = 0; plane < 3; plane++) {
    FillPlane(plane, frame_index, config, &planes[plane]);
  }
  return planes;
}

std::vector<uint8_t>
BuildPackedFrame(int frame_index, const SequenceConfig &config) {
  const int sample_size = SampleSize(config.input_bitdepth);
  std::vector<uint8_t> packed;
  for (int plane = 0; plane < 3; plane++) {
    const int plane_width =
      PlaneWidth(config.width, plane, config.chroma_format);
    const int plane_height =
      PlaneHeight(config.height, plane, config.chroma_format);
    packed.resize(packed.size() +
                  plane_width * plane_height * sample_size);
    uint8_t *dst = &packed[packed.size() -
                           plane_width * plane_height * sample_size];
    for (int y = 0; y < plane_height; y++) {
      for (int x = 0; x < plane_width; x++) {
        const int sample =
          PatternSample(plane, x, y, frame_index, config.input_bitdepth);
        if (sample_size == 1) {
          dst[(y * plane_width + x)] = static_cast<uint8_t>(sample);
        } else {
          uint16_t *dst16 = reinterpret_cast<uint16_t*>(dst);
          dst16[y * plane_width + x] = static_cast<uint16_t>(sample);
        }
      }
    }
  }
  return packed;
}

void RefillFrame(int frame_index, const SequenceConfig &config,
                 std::vector<StridedPlane> *planes) {
  assert(planes->size() == 3);
  for (int plane = 0; plane < 3; plane++) {
    StridedPlane &strided = (*planes)[plane];
    const int sample_size = SampleSize(config.input_bitdepth);
    assert(strided.width ==
           PlaneWidth(config.width, plane, config.chroma_format));
    assert(strided.height ==
           PlaneHeight(config.height, plane, config.chroma_format));
    for (int y = 0; y < strided.height; y++) {
      for (int x = 0; x < strided.width; x++) {
        const int sample =
          PatternSample(plane, x, y, frame_index, config.input_bitdepth);
        uint8_t *dst = strided.data + y * strided.stride_bytes +
          x * sample_size;
        if (sample_size == 1) {
          dst[0] = static_cast<uint8_t>(sample);
        } else {
          *reinterpret_cast<uint16_t*>(dst) = static_cast<uint16_t>(sample);
        }
      }
    }
  }
}

int ExpectedPaddedSample(int plane, int padded_x, int padded_y,
                         int frame_index, const SequenceConfig &config) {
  const int padded_width =
    (config.width + 7) / 8 * 8;
  const int padded_height =
    (config.height + 7) / 8 * 8;
  // The luma plane keeps the full picture dimensions; only U and V are
  // scaled by the chroma format.
  const int valid_width =
    PlaneWidth(config.width, plane, config.chroma_format);
  const int valid_height =
    PlaneHeight(config.height, plane, config.chroma_format);
  const int plane_padded_width =
    PlaneWidth(padded_width, plane, config.chroma_format);
  const int plane_padded_height =
    PlaneHeight(padded_height, plane, config.chroma_format);
  assert(padded_x >= 0 && padded_x < plane_padded_width);
  assert(padded_y >= 0 && padded_y < plane_padded_height);
  // Right and bottom padding extends the last valid column/row.
  const int valid_x = std::min(padded_x, valid_width - 1);
  const int valid_y = std::min(padded_y, valid_height - 1);
  return ExpectedInternalSample(plane, valid_x, valid_y, frame_index, config);
}

}  // namespace planar_intake_ref
