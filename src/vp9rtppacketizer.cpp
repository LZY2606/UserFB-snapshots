/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp9rtppacketizer.hpp"
#include "impl/vp9spatial.hpp"

#include <cstring>
#include <stdexcept>

namespace rtc {

VP9RtpPacketizer::VP9RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
		size_t maxFragmentSize)
	: RtpPacketizer(std::move(rtpConfig)), mMaxFragmentSize(maxFragmentSize) {}

void VP9RtpPacketizer::outgoing(message_vector &messages,
                                [[maybe_unused]] const message_callback &send) {
	message_vector result;
	for (const auto &message : messages) {
		// Control messages pass through untouched.
		if (message->type == Message::Control) {
			result.push_back(message);
			continue;
		}

		const auto &frameInfo = message->frameInfo;
		bool isSpatial = frameInfo && !frameInfo->vp9SpatialLayers.empty();
		uint32_t newTimestamp = rtpConfig->timestamp;
		if (frameInfo) {
			if (frameInfo->payloadType && frameInfo->payloadType != rtpConfig->payloadType)
				continue;

			if (frameInfo->timestampSeconds)
				newTimestamp =
				    rtpConfig->startTimestamp +
				    rtpConfig->secondsToTimestamp(
				        std::chrono::duration<double>(*frameInfo->timestampSeconds).count());
			else
				newTimestamp = frameInfo->timestamp;
		}

		// A spatial frame is validated before any state changes, so an invalid
		// description consumes no sequence number, timestamp or picture count.
		if (isSpatial)
			impl::validateVp9SpatialLayers(frameInfo->vp9SpatialLayers, message->size(),
			                               mMaxFragmentSize);

		rtpConfig->timestamp = newTimestamp;

		mCurrentFrameInfo = frameInfo;
		std::vector<binary> payloads;
		try {
			payloads = fragment(std::move(*message));
		} catch (...) {
			mCurrentFrameInfo.reset();
			throw;
		}
		mCurrentFrameInfo.reset();

		for (size_t i = 0; i < payloads.size(); i++) {
			if (rtpConfig->dependencyDescriptorContext.has_value()) {
				auto &ctx = *rtpConfig->dependencyDescriptorContext;
				ctx.descriptor.startOfFrame = i == 0;
				ctx.descriptor.endOfFrame = i == payloads.size() - 1;
			}
			bool mark = i == payloads.size() - 1;
			result.push_back(packetize(payloads[i], mark, frameInfo));
		}
	}

	messages.swap(result);
}

std::vector<binary> VP9RtpPacketizer::fragment(binary frame) {
	/*
	 * VP9 RTP payload descriptor (RFC 9628)
	 * See https://datatracker.ietf.org/doc/html/rfc9628
	 *
	 *       0 1 2 3 4 5 6 7
	 *      +-+-+-+-+-+-+-+-+
	 *      |I|P|L|F|B|E|V|Z| (REQUIRED)
	 *      +-+-+-+-+-+-+-+-+
	 *
	 * I: PictureID present
	 * P: Inter-picture predicted frame
	 * L: Layer indices present
	 * F: Flexible mode
	 * B: Start of a frame
	 * E: End of a frame
	 * V: Scalability Structure (SS) data present
	 * Z: Not a reference frame for upper spatial layers
	 */

	if (mCurrentFrameInfo && !mCurrentFrameInfo->vp9SpatialLayers.empty())
		return fragmentSpatialLayers(frame, mCurrentFrameInfo->vp9SpatialLayers);

	/*
	 * We use a minimal 1-byte descriptor with only B and E flags set.
	 * This is sufficient for single spatial/temporal layer streams.
	 */

	// First byte bit masks
	const uint8_t bitP = 0b01000000;
	const uint8_t bitB = 0b00001000;
	const uint8_t bitE = 0b00000100;

	/*
	 * VP9 uncompressed header (VP9 bitstream spec, Section 6.2):
	 * The first two bits of the first byte encode the frame marker (0b10),
	 * and the profile. Bit 3 (0x08) is the show_existing_frame flag.
	 * When show_existing_frame is 0, bit 2 (0x04) is 0 for a key frame
	 * and 1 for a non-key frame.
	 *
	 * The robust check: bit 3 (show_existing_frame) must be 0, and
	 * bit 2 is the frame_type: 0 = KEY_FRAME, 1 = NON_KEY_FRAME.
	 */
	const uint8_t showExistingFrame = 0b00001000; // bit 3 (0x08)
	const uint8_t frameTypeBit = 0b00000100;     // bit 2 (0x04)

	if (frame.empty())
		return {};

	bool isInterFrame = false;
	uint8_t firstFrameByte = std::to_integer<uint8_t>(frame[0]);
	if (!(firstFrameByte & showExistingFrame))
		isInterFrame = (firstFrameByte & frameTypeBit) != 0;

	const size_t descriptorSize = 1;
	if (mMaxFragmentSize <= descriptorSize)
		return {};

	std::vector<binary> payloads;
	size_t index = 0;
	while (index < frame.size()) {
		size_t remaining = frame.size() - index;
		size_t payloadSize = std::min(mMaxFragmentSize - descriptorSize, remaining);
		bool isFirst = (index == 0);
		bool isLast = (index + payloadSize >= frame.size());

		binary payload(descriptorSize + payloadSize);

		// Set 1-byte payload descriptor
		uint8_t descriptor = 0;
		if (isInterFrame)
			descriptor |= bitP;
		if (isFirst)
			descriptor |= bitB;
		if (isLast)
			descriptor |= bitE;
		payload[0] = std::byte(descriptor);

		// Copy frame data
		std::memcpy(payload.data() + descriptorSize, frame.data() + index, payloadSize);

		payloads.push_back(std::move(payload));
		index += payloadSize;
	}

	return payloads;
}

std::vector<binary> VP9RtpPacketizer::fragmentSpatialLayers(
		const binary &frame, const std::vector<VP9SpatialLayer> &layers) {
	/*
	 * Non-flexible mode VP9 descriptor (RFC 9628, Section 4.2):
	 *
	 *       0 1 2 3 4 5 6 7
	 *      +-+-+-+-+-+-+-+-+
	 *      |I|P|L|F|B|E|V|Z| (REQUIRED)
	 *      +-+-+-+-+-+-+-+-+
	 *   I: |M| PICTURE ID  | (15-bit)
	 *      +-+-+-+-+-+-+-+-+
	 *      | EXTENDED PID  |
	 *      +-+-+-+-+-+-+-+-+
	 *   L: | TID |U| SID |D|
	 *      +-+-+-+-+-+-+-+-+
	 *      |   TL0PICIDX   |
	 *      +-+-+-+-+-+-+-+-+
	 *
	 * I=1, L=1, F=0, TID=0. P and D come from the layer metadata, B/E mark
	 * the first and last packet of each layer, and the RTP marker is set
	 * only on the last packet of the highest layer.
	 */

	const uint8_t bitI = 0b10000000;
	const uint8_t bitP = 0b01000000;
	const uint8_t bitL = 0b00100000;
	const uint8_t bitB = 0b00001000;
	const uint8_t bitE = 0b00000100;

	const uint8_t bitM = 0b10000000;

	const size_t descriptorSize = impl::Vp9SpatialDescriptorSize;

	// Validate the whole description before touching any state or producing
	// any packet, so a bad frame consumes no sequence number or counter.
	impl::validateVp9SpatialLayers(layers, frame.size(), mMaxFragmentSize);

	const uint16_t pictureId = mPictureId;
	const uint8_t tl0PicIdx = mTl0PicIdx;
	const uint8_t pictureIdHi = static_cast<uint8_t>(bitM | ((pictureId >> 8) & 0x7F));
	const uint8_t pictureIdLo = static_cast<uint8_t>(pictureId & 0xFF);

	const size_t maxPayload = mMaxFragmentSize - descriptorSize;

	std::vector<binary> payloads;
	size_t index = 0;
	for (size_t sid = 0; sid < layers.size(); ++sid) {
		const auto &layer = layers[sid];

		size_t layerOffset = 0;
		while (layerOffset < layer.size) {
			const size_t payloadSize = std::min(maxPayload, layer.size - layerOffset);
			const bool isFirst = (layerOffset == 0);
			const bool isLast = (layerOffset + payloadSize >= layer.size);

			binary payload(descriptorSize + payloadSize);

			uint8_t descriptor = bitI | bitL;
			if (layer.interPicturePredicted)
				descriptor |= bitP;
			if (isFirst)
				descriptor |= bitB;
			if (isLast)
				descriptor |= bitE;

			// TID (top 2 bits) = 0, U = 0, SID, D.
			uint8_t layerByte = static_cast<uint8_t>((sid & 0x07) << 1);
			if (layer.interLayerPredicted)
				layerByte |= 0x01;

			payload[0] = std::byte(descriptor);
			payload[1] = std::byte(pictureIdHi);
			payload[2] = std::byte(pictureIdLo);
			payload[3] = std::byte(layerByte);
			payload[4] = std::byte(tl0PicIdx);

			std::memcpy(payload.data() + descriptorSize, frame.data() + index, payloadSize);

			payloads.push_back(std::move(payload));
			index += payloadSize;
			layerOffset += payloadSize;
		}
	}

	// The picture is fully packetized; commit the counters.
	mPictureId = static_cast<uint16_t>((pictureId + 1) & 0x7FFF);
	mTl0PicIdx = static_cast<uint8_t>(tl0PicIdx + 1);

	return payloads;
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
