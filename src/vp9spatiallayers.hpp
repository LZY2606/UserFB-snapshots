/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_VP9_SPATIAL_LAYERS_HPP
#define RTC_VP9_SPATIAL_LAYERS_HPP

#if RTC_ENABLE_MEDIA

#include "frameinfo.hpp"

#include <vector>

namespace rtc::vp9spatial {

// Parsed VP9 RTP payload descriptor (RFC 9628) for non-flexible layered streams.
struct ParsedDescriptor {
	bool interPicturePredicted = false;
	bool interLayerPredicted = false;
	bool startOfFrame = false;
	bool endOfFrame = false;
	bool flexible = false;
	bool pictureIdPresent = false;
	bool layerIndicesPresent = false;
	uint8_t tid = 0;
	uint8_t sid = 0;
	uint16_t pictureId = 0;
	uint8_t tl0PicIdx = 0;
	size_t headerSize = 0;
};

// Parses the VP9 payload descriptor. Returns false if truncated or if the
// descriptor does not carry both a PictureID and layer indices.
bool parseDescriptor(const byte *payload, size_t size, ParsedDescriptor &out);

// Confirms a spatial layer description is well-formed and fits the packet budget.
// Throws std::invalid_argument when the layer information is invalid or when a
// packet cannot hold the descriptor plus at least one data byte.
void validateSpatialLayers(const std::vector<VP9SpatialLayer> &layers, size_t frameSize,
                           size_t maxFragmentSize);

// Fragments a frame made of concatenated spatial layer frames into RTP payloads.
// Each payload starts with a 5-byte non-flexible VP9 descriptor.
// Throws std::invalid_argument if the layer description is invalid or if a
// packet cannot hold the descriptor plus at least one byte of data.
std::vector<binary> fragmentSpatialLayers(const std::vector<VP9SpatialLayer> &layers,
                                          const binary &frame, size_t maxFragmentSize,
                                          uint16_t pictureId, uint8_t tl0PicIdx);

// Builds a VP9 superframe (frames concatenated, followed by a little-endian
// superframe index) from the given spatial layer frames. A single frame is
// returned unchanged.
binary buildSuperframe(const std::vector<binary> &layers);

} // namespace rtc::vp9spatial

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_VP9_SPATIAL_LAYERS_HPP */
