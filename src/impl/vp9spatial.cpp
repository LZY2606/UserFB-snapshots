/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp9spatial.hpp"

#include <algorithm>
#include <stdexcept>

namespace rtc::impl {

namespace {

// VP9 payload descriptor first-byte flags (RFC 9628).
constexpr uint8_t bitI = 0b10000000; // PictureID present
constexpr uint8_t bitP = 0b01000000; // Inter-picture predicted
constexpr uint8_t bitL = 0b00100000; // Layer indices present
constexpr uint8_t bitF = 0b00010000; // Flexible mode
constexpr uint8_t bitB = 0b00001000; // Start of frame
constexpr uint8_t bitE = 0b00000100; // End of frame
constexpr uint8_t bitV = 0b00000010; // Scalability structure present

constexpr uint8_t bitM = 0b10000000; // Extended PictureID
constexpr uint8_t bitN = 0b00000001; // Next reference index follows
constexpr uint8_t bitD = 0b00000001; // Inter-layer predicted

constexpr size_t maxRefPics = 3;

} // namespace

Vp9PayloadDescriptor parseVp9PayloadDescriptor(const std::byte *data, size_t size) {
	Vp9PayloadDescriptor d;
	if (size < 1)
		return d;

	const uint8_t firstByte = std::to_integer<uint8_t>(data[0]);
	size_t offset = 1;

	d.interPicturePredicted = (firstByte & bitP) != 0;
	d.startOfFrame = (firstByte & bitB) != 0;
	d.endOfFrame = (firstByte & bitE) != 0;

	if (firstByte & bitI) {
		if (size < offset + 1)
			return d;
		const uint8_t pictureIdByte = std::to_integer<uint8_t>(data[offset]);
		++offset;
		d.hasPictureId = true;
		d.pictureId = static_cast<uint16_t>((pictureIdByte & 0x7F) << 8);
		if (pictureIdByte & bitM) { // 15-bit extended PictureID
			if (size < offset + 1)
				return d;
			d.pictureId |= std::to_integer<uint8_t>(data[offset]);
			++offset;
		}
	}

	if (firstByte & bitL) {
		if (size < offset + 1)
			return d;
		const uint8_t layerByte = std::to_integer<uint8_t>(data[offset]);
		++offset;
		d.sid = static_cast<uint8_t>((layerByte >> 1) & 0x07);
		d.interLayerPredicted = (layerByte & bitD) != 0;

		// TL0PICIDX is present only in non-flexible mode.
		if (!(firstByte & bitF)) {
			if (size < offset + 1)
				return d;
			++offset;
		}
	}

	// Reference indices (flexible mode only).
	if ((firstByte & bitP) && (firstByte & bitF)) {
		for (size_t i = 0; i < maxRefPics; ++i) {
			if (size < offset + 1)
				return d;
			const uint8_t refByte = std::to_integer<uint8_t>(data[offset]);
			++offset;
			if (!(refByte & bitN))
				break;
		}
	}

	if (firstByte & bitV) {
		if (size < offset + 1)
			return d;
		const uint8_t ssByte = std::to_integer<uint8_t>(data[offset]);
		++offset;

		const unsigned int numSpatialLayers = (ssByte >> 5) + 1; // N_S + 1
		const bool resolutionPresent = (ssByte >> 4) & 0x01;
		const bool pgPresent = (ssByte >> 3) & 0x01;

		if (resolutionPresent) {
			const size_t resolutionSize = 4 * static_cast<size_t>(numSpatialLayers);
			if (size < offset + resolutionSize)
				return d;
			offset += resolutionSize;
		}

		if (pgPresent) {
			if (size < offset + 1)
				return d;
			const unsigned int numPictureGroups = std::to_integer<uint8_t>(data[offset]);
			++offset;

			for (unsigned int i = 0; i < numPictureGroups; ++i) {
				if (size < offset + 1)
					return d;
				const uint8_t pgByte = std::to_integer<uint8_t>(data[offset]);
				++offset;
				const unsigned int numRefs = (pgByte >> 2) & 0x03;
				if (size < offset + numRefs)
					return d;
				offset += numRefs;
			}
		}
	}

	if (size < offset)
		return d;

	d.valid = true;
	d.headerSize = offset;
	return d;
}

void validateVp9SpatialLayers(const std::vector<VP9SpatialLayer> &layers, size_t frameSize,
                              size_t maxFragmentSize) {
	if (layers.empty() || layers.size() > 8)
		throw std::invalid_argument("VP9 spatial layer count must be between 1 and 8");

	if (maxFragmentSize < Vp9SpatialDescriptorSize + 1)
		throw std::invalid_argument(
		    "VP9 max fragment size is too small for the descriptor and one data byte");

	size_t total = 0;
	for (size_t sid = 0; sid < layers.size(); ++sid) {
		const auto &layer = layers[sid];
		if (layer.size == 0)
			throw std::invalid_argument("VP9 spatial layer size must be positive");
		if (sid == 0 && layer.interLayerPredicted)
			throw std::invalid_argument("VP9 SID 0 must not depend on a lower layer");
		total += layer.size;
	}

	if (total != frameSize)
		throw std::invalid_argument("VP9 spatial layer sizes do not match the frame length");
}

// Builds a VP9 superframe index for the given frame sizes (VP9 bitstream spec,
// section 7.5; see libvpx parse_superframe_index). The index is appended after
// the frames. Layout:
//
//   marker | sizes... | marker
//
// where the marker byte is
//
//   superframe_marker (3 bits, 0b110) | magnitude_minus_1 (2 bits) |
//   frames_minus_1 (3 bits)
//
// so the size width occupies bits 4..3 and the frame count bits 2..0. The size
// width is the smallest that fits the largest frame. The marker is repeated at
// the end and sizes are little endian.
binary buildVp9SuperframeIndex(const std::vector<size_t> &sizes) {
	const size_t numFrames = sizes.size();

	size_t maxSize = 0;
	for (size_t size : sizes)
		maxSize = std::max(maxSize, size);

	size_t magnitude = 1; // width in bytes of each frame size
	size_t capacity = 0xFF;
	while (capacity < maxSize && magnitude < 4) {
		++magnitude;
		capacity = (capacity << 8) | 0xFF;
	}

	const uint8_t marker =
	    static_cast<uint8_t>(0xC0 | ((magnitude - 1) << 3) | (numFrames - 1));

	binary index;
	index.reserve(2 + magnitude * numFrames);
	index.push_back(std::byte(marker));
	for (size_t size : sizes) {
		for (size_t i = 0; i < magnitude; ++i)
			index.push_back(std::byte(static_cast<uint8_t>((size >> (8 * i)) & 0xFF)));
	}
	index.push_back(std::byte(marker));

	return index;
}

} // namespace rtc::impl

#endif /* RTC_ENABLE_MEDIA */
