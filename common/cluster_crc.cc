// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#include "common/cluster_crc.h"

namespace libwebm {

void ClusterCRC32Update(uint32_t& crc, const uint8_t* data, uint64_t length) {
  if (!data)
    return;

  for (uint64_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (crc & 1) ? 0xFFFFFFFFU : 0U;
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
}

uint32_t ClusterCRC32Finalize(uint32_t crc) {
  return crc ^ 0xFFFFFFFFU;
}

uint32_t ClusterCRC32(const uint8_t* data, uint64_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  ClusterCRC32Update(crc, data, length);
  return ClusterCRC32Finalize(crc);
}

}  // namespace libwebm
