/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_IMPL_VP9_SPATIAL_HPP
#define RTC_IMPL_VP9_SPATIAL_HPP

#if RTC_ENABLE_MEDIA

#include "frameinfo.hpp"

#include <cstddef>
#include <vector>

namespace rtc::impl {

/// Size in bytes of the non-flexible VP9 payload descriptor emitted for
/// spatial layers: first byte + 15-bit PictureID (2) + layer indices (1) +
/// TL0PICIDX (1).
inline constexpr size_t Vp9SpatialDescriptorSize = 5;

/// Parsed view of a VP9 payload descriptor (RFC 9628, section 4.2).
struct Vp9PayloadDescriptor {
	bool valid = false;
	bool hasPictureId = false;
	uint16_t pictureId = 0; // 15-bit
	bool interPicturePredicted = false;
	bool interLayerPredicted = false;
	bool startOfFrame = false;
	bool endOfFrame = false;
	uint8_t sid = 0;
	size_t headerSize = 0; // descriptor bytes; payload starts after this
};

/// Parses a VP9 payload descriptor from a packet payload.
Vp9PayloadDescriptor parseVp9PayloadDescriptor(const std::byte *data, size_t size);

/// Validates a spatial layer description against the concatenated frame.
/// Throws std::invalid_argument when the description is unusable.
void validateVp9SpatialLayers(const std::vector<VP9SpatialLayer> &layers, size_t frameSize,
                              size_t maxFragmentSize);

/// Builds a VP9 superframe index for the given frame sizes (VP9 bitstream
/// spec, section 7.5). Layout is `marker | sizes... | marker`, where the marker
/// packs the superframe marker (0b110), the size width minus one (bits 4..3)
/// and the frame count minus one (bits 2..0). The size width is the smallest
/// that fits the largest frame, and multi-byte fields are little endian.
binary buildVp9SuperframeIndex(const std::vector<size_t> &sizes);

} // namespace rtc::impl

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_IMPL_VP9_SPATIAL_HPP */
