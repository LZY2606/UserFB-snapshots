// Copyright (c) 2026 The WebM project authors. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS.  All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
#ifndef LIBWEBM_COMMON_CLUSTER_CRC_H_
#define LIBWEBM_COMMON_CLUSTER_CRC_H_

#include <stdint.h>

namespace libwebm {

// Matroska Cluster CRC-32 element (Matroska ID 0xBF). Its payload is always
// exactly four bytes holding a little endian IEEE CRC-32 value computed with
// the reflected polynomial 0xEDB88320 and an initial/final XOR of 0xFFFFFFFF.
const uint64_t kMkvCRC32 = 0xBF;
const int kClusterCRC32PayloadSize = 4;
// One ID byte, one size byte (0x84), and four payload bytes.
const int kClusterCRC32ElementSize = 6;

// Feeds |length| bytes from |data| into the running CRC-32 held in |crc|. The
// caller must initialize |crc| to 0xFFFFFFFF before the first update and may
// resume a long stream across any number of calls.
void ClusterCRC32Update(uint32_t& crc, const uint8_t* data, uint64_t length);

// Finalizes a CRC-32 accumulated with ClusterCRC32Update().
uint32_t ClusterCRC32Finalize(uint32_t crc);

// Computes the CRC-32 of |length| bytes from |data| in one shot.
uint32_t ClusterCRC32(const uint8_t* data, uint64_t length);

}  // namespace libwebm

#endif  // LIBWEBM_COMMON_CLUSTER_CRC_H_
