/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp9spatiallayers.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rtc::vp9spatial {

namespace {

constexpr uint8_t bitI = 0b10000000;
constexpr uint8_t bitP = 0b01000000;
constexpr uint8_t bitL = 0b00100000;
constexpr uint8_t bitF = 0b00010000;
constexpr uint8_t bitB = 0b00001000;
constexpr uint8_t bitE = 0b00000100;
constexpr uint8_t bitV = 0b00000010;
constexpr uint8_t bitM = 0b10000000;
constexpr uint8_t bitN = 0b00000001;

constexpr size_t LayerDescriptorSize = 5;

} // namespace

bool parseDescriptor(const byte *payload, size_t size, ParsedDescriptor &out) {
	out = ParsedDescriptor{};

	if (payload == nullptr || size < 1)
		return false;

	const uint8_t first = std::to_integer<uint8_t>(payload[0]);
	out.flexible = (first & bitF) != 0;
	out.interPicturePredicted = (first & bitP) != 0;
	out.startOfFrame = (first & bitB) != 0;
	out.endOfFrame = (first & bitE) != 0;

	size_t offset = 1;

	// PictureID (I=1)
	if (first & bitI) {
		out.pictureIdPresent = true;
		if (offset + 1 > size)
			return false;
		const uint8_t pid = std::to_integer<uint8_t>(payload[offset++]);
		if (pid & bitM) { // 15-bit extended PictureID
			if (offset + 1 > size)
				return false;
			out.pictureId = uint16_t((uint16_t(pid & 0x7F) << 8) |
			                         std::to_integer<uint8_t>(payload[offset++]));
		} else {
			out.pictureId = pid & 0x7F;
		}
	}

	// Layer indices (L=1)
	if (first & bitL) {
		out.layerIndicesPresent = true;
		if (offset + 1 > size)
			return false;
		const uint8_t layer = std::to_integer<uint8_t>(payload[offset++]);
		out.tid = uint8_t((layer >> 5) & 0x07);
		out.sid = uint8_t((layer >> 1) & 0x07);
		out.interLayerPredicted = (layer & 0x01) != 0;

		// TL0PICIDX is present only in non-flexible mode (F=0)
		if (!out.flexible) {
			if (offset + 1 > size)
				return false;
			out.tl0PicIdx = std::to_integer<uint8_t>(payload[offset++]);
		}
	}

	// Reference indices (P=1 and F=1, flexible mode only)
	if ((first & bitP) && (first & bitF)) {
		for (int i = 0; i < 3; ++i) {
			if (offset + 1 > size)
				return false;
			const uint8_t ref = std::to_integer<uint8_t>(payload[offset++]);
			if (!(ref & bitN))
				break;
		}
	}

	// Scalability Structure (V=1)
	if (first & bitV) {
		if (offset + 1 > size)
			return false;
		const uint8_t ss = std::to_integer<uint8_t>(payload[offset++]);
		const size_t numSpatialLayers = size_t((ss >> 5) & 0x07) + 1;
		const bool resolutionPresent = (ss >> 4) & 0x01;
		const bool pictureGroupPresent = (ss >> 3) & 0x01;

		if (resolutionPresent) {
			const size_t resolutionSize = 4 * numSpatialLayers;
			if (offset + resolutionSize > size)
				return false;
			offset += resolutionSize;
		}

		if (pictureGroupPresent) {
			if (offset + 1 > size)
				return false;
			const size_t numPictureGroups = std::to_integer<uint8_t>(payload[offset++]);
			for (size_t i = 0; i < numPictureGroups; ++i) {
				if (offset + 1 > size)
					return false;
				const uint8_t pg = std::to_integer<uint8_t>(payload[offset++]);
				const size_t numRefs = (pg >> 2) & 0x03;
				if (offset + numRefs > size)
					return false;
				offset += numRefs;
			}
		}
	}

	out.headerSize = offset;
	return offset <= size;
}

void validateSpatialLayers(const std::vector<VP9SpatialLayer> &layers, size_t frameSize,
                           size_t maxFragmentSize) {
	if (layers.empty() || layers.size() > 8)
		throw std::invalid_argument("VP9 spatial layer count must be between 1 and 8");

	if (maxFragmentSize < LayerDescriptorSize + 1)
		throw std::invalid_argument("VP9 max fragment size is too small for the payload descriptor");

	size_t total = 0;
	for (const auto &layer : layers) {
		if (layer.size == 0)
			throw std::invalid_argument("VP9 spatial layer size must be positive");
		if (total > std::numeric_limits<size_t>::max() - layer.size)
			throw std::invalid_argument("VP9 spatial layer sizes overflow");
		total += layer.size;
	}

	if (total != frameSize)
		throw std::invalid_argument("VP9 spatial layer sizes do not match the frame size");

	// SID 0 cannot depend on a lower spatial layer
	if (layers.front().interLayerPredicted)
		throw std::invalid_argument("VP9 spatial layer 0 cannot be inter-layer predicted");
}

std::vector<binary> fragmentSpatialLayers(const std::vector<VP9SpatialLayer> &layers,
                                          const binary &frame, size_t maxFragmentSize,
                                          uint16_t pictureId, uint8_t tl0PicIdx) {
	validateSpatialLayers(layers, frame.size(), maxFragmentSize);

	pictureId &= 0x7FFF;

	const size_t maxDataSize = maxFragmentSize - LayerDescriptorSize;

	std::vector<binary> payloads;
	size_t offset = 0;
	uint8_t sid = 0;
	for (const auto &layer : layers) {
		size_t layerOffset = 0;
		while (layerOffset < layer.size) {
			const size_t chunk = std::min(maxDataSize, layer.size - layerOffset);
			const bool isFirst = layerOffset == 0;
			const bool isLast = layerOffset + chunk >= layer.size;

			binary payload(LayerDescriptorSize + chunk);

			// I=1, P from metadata, L=1, F=0, B/E per fragment, V=0, Z=0
			uint8_t descriptor = bitI | bitL;
			if (layer.interPicturePredicted)
				descriptor |= bitP;
			if (isFirst)
				descriptor |= bitB;
			if (isLast)
				descriptor |= bitE;

			payload[0] = std::byte(descriptor);
			payload[1] = std::byte(0x80 | ((pictureId >> 8) & 0x7F)); // M=1
			payload[2] = std::byte(pictureId & 0xFF);
			// TID=0, U=0, SID, D from metadata
			payload[3] = std::byte((sid << 1) | (layer.interLayerPredicted ? 1 : 0));
			payload[4] = std::byte(tl0PicIdx);

			std::memcpy(payload.data() + LayerDescriptorSize, frame.data() + offset + layerOffset,
			            chunk);
			payloads.push_back(std::move(payload));

			layerOffset += chunk;
		}
		offset += layer.size;
		++sid;
	}

	return payloads;
}

binary buildSuperframe(const std::vector<binary> &layers) {
	if (layers.empty())
		throw std::invalid_argument("VP9 superframe requires at least one spatial layer");

	if (layers.size() > 8)
		throw std::invalid_argument("VP9 superframe supports at most 8 spatial layers");

	if (layers.size() == 1)
		return layers.front();

	size_t maxSize = 0;
	for (const auto &layer : layers)
		maxSize = std::max(maxSize, layer.size());

	// Smallest number of bytes able to represent the largest layer length.
	// The stored value is the frame size minus one, but the width is derived
	// from the size itself, matching libvpx and the VP9 superframe format.
	size_t bytesPerSize = 1;
	while (bytesPerSize < 4 && uint64_t(maxSize) >= (uint64_t(1) << (bytesPerSize * 8)))
		++bytesPerSize;

	if (uint64_t(maxSize) >= (uint64_t(1) << 32))
		throw std::invalid_argument("VP9 spatial layer is too large for a superframe index");

	size_t total = 0;
	for (const auto &layer : layers)
		total += layer.size();

	const size_t indexSize = 1 + layers.size() * bytesPerSize + 1;
	binary out;
	out.reserve(total + indexSize);

	for (const auto &layer : layers)
		out.insert(out.end(), layer.begin(), layer.end());

	const uint8_t marker = uint8_t(0xC0 | ((bytesPerSize - 1) << 3) | (layers.size() - 1));
	out.push_back(std::byte(marker));
	for (const auto &layer : layers) {
		// Frame sizes are stored minus one, little-endian
		size_t value = layer.size() - 1;
		for (size_t b = 0; b < bytesPerSize; ++b)
			out.push_back(std::byte((value >> (8 * b)) & 0xFF));
	}
	out.push_back(std::byte(marker)); // duplicated trailing marker

	return out;
}

} // namespace rtc::vp9spatial

#endif /* RTC_ENABLE_MEDIA */
