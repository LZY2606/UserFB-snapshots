/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "vp9rtpdepacketizer.hpp"
#include "rtp.hpp"

#include "impl/vp9spatial.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace rtc {

VP9RtpDepacketizer::VP9RtpDepacketizer() {}

VP9RtpDepacketizer::~VP9RtpDepacketizer() {}

void VP9RtpDepacketizer::setSpatialLayerLimit(optional<uint8_t> maxSid) {
	if (maxSid && *maxSid > 7)
		throw std::out_of_range("VP9 spatial layer limit must be between 0 and 7");

	if (mSpatialLayerLimit && !maxSid)
		mSpatial.reset();

	mSpatialLayerLimit = maxSid;
}

message_ptr VP9RtpDepacketizer::reassemble(message_buffer &buffer) {
	/*
	 * VP9 RTP payload format (RFC 9628)
	 * See https://datatracker.ietf.org/doc/html/rfc9628
	 */

	if (buffer.empty())
		return nullptr;

	if (mSpatialLayerLimit) {
		message_vector messages;
		messages.reserve(buffer.size());
		for (const auto &packet : buffer)
			messages.push_back(packet);

		auto frame = reassembleSpatial(messages);
		// The spatial path keeps its own picture state, so every packet it was
		// given has been handled: drop the base buffer.
		markBufferConsumed();
		return frame;
	}

	// Legacy path: reassemble when the marker arrives or the sequence is
	// continuous from a start-of-frame packet.
	auto first = *buffer.begin();
	auto firstRtpHeader = reinterpret_cast<const RtpHeader *>(first->data());
	uint8_t payloadType = firstRtpHeader->payloadType();
	uint32_t timestamp = firstRtpHeader->timestamp();
	uint16_t nextSeqNumber = firstRtpHeader->seqNumber();

	binary frame;
	std::vector<std::pair<const std::byte *, size_t>> payloads;
	bool continuousSequence = false;
	for (const auto &packet : buffer) {
		auto rtpHeader = reinterpret_cast<const rtc::RtpHeader *>(packet->data());
		if (rtpHeader->seqNumber() < nextSeqNumber) {
			// Skip
			continue;
		}
		if (rtpHeader->seqNumber() > nextSeqNumber) {
			// Missing packet(s)
			continuousSequence = false;
		}
		nextSeqNumber = rtpHeader->seqNumber() + 1;

		auto rtpHeaderSize = rtpHeader->getSize() + rtpHeader->getExtensionHeaderSize();
		auto paddingSize = 0;
		if (rtpHeader->padding())
			paddingSize = std::to_integer<uint8_t>(packet->back());

		if (packet->size() <= rtpHeaderSize + paddingSize)
			continue; // Empty payload

		const std::byte *payloadData = packet->data() + rtpHeaderSize;
		size_t payloadSize = packet->size() - rtpHeaderSize - paddingSize;

		if (payloadSize < 1)
			continue;

		// Parse VP9 payload descriptor (RFC 9628, Section 4.2)
		auto descriptor = impl::parseVp9PayloadDescriptor(payloadData, payloadSize);
		if (!descriptor.valid)
			continue;
		size_t descriptorSize = descriptor.headerSize;

		if (payloadSize < descriptorSize)
			continue;

		payloadData += descriptorSize;
		payloadSize -= descriptorSize;

		// Frame reassembly using B (start of frame) and marker bit
		if ((descriptor.startOfFrame) || rtpHeader->marker()) {
			if (continuousSequence) {
				for (auto [data, size] : payloads)
					frame.insert(frame.end(), data, data + size);

				if (rtpHeader->marker())
					frame.insert(frame.end(), payloadData, payloadData + payloadSize);
			}
			else if ((descriptor.startOfFrame) && rtpHeader->marker()) {
				// Single-packet frame at start of buffer
				frame.insert(frame.end(), payloadData, payloadData + payloadSize);
			}
			payloads.clear();
			continuousSequence = true;
		}

		if (!rtpHeader->marker())
			payloads.push_back(std::make_pair(payloadData, payloadSize));
	}

	if (frame.empty())
		return nullptr;

	return make_message(std::move(frame), createFrameInfo(timestamp, payloadType));
}

bool VP9RtpDepacketizer::completeLayer(const SpatialLayer &layer, binary &out) {
	if (!layer.sawStart || !layer.sawEnd)
		return false;

	// A layer's start and end must belong to a single coherent picture; a
	// second start or end would mean two frames were mixed together.
	unsigned int startCount = 0;
	unsigned int endCount = 0;
	for (const auto &entry : layer.segments) {
		startCount += entry.second.startOfFrame ? 1 : 0;
		endCount += entry.second.endOfFrame ? 1 : 0;
	}
	if (startCount != 1 || endCount != 1)
		return false;

	// Find the segment carrying B, then walk contiguous sequence numbers until
	// the segment carrying E. Anything out of order is ignored until the gap
	// is filled.
	auto it = layer.segments.begin();
	while (it != layer.segments.end() && !it->second.startOfFrame)
		++it;
	if (it == layer.segments.end())
		return false;

	binary assembled;
	int64_t key = it->first;
	bool ended = false;
	while (true) {
		auto seg = layer.segments.find(key);
		if (seg == layer.segments.end())
			break; // gap
		assembled.insert(assembled.end(), seg->second.data.begin(), seg->second.data.end());
		if (seg->second.endOfFrame) {
			ended = true;
			break;
		}
		++key;
	}

	if (!ended)
		return false;

	out = std::move(assembled);
	return true;
}

message_ptr VP9RtpDepacketizer::reassembleSpatial(const message_vector &messages) {
	for (const auto &message : messages) {
		if (message->type == Message::Control)
			continue;

		if (message->size() < sizeof(RtpHeader))
			continue;

		auto header = reinterpret_cast<const RtpHeader *>(message->data());
		const size_t rtpHeaderSize = header->getSize() + header->getExtensionHeaderSize();
		if (message->size() < rtpHeaderSize)
			continue;

		size_t paddingSize = 0;
		if (header->padding())
			paddingSize = std::to_integer<uint8_t>(message->back());
		if (message->size() < rtpHeaderSize + paddingSize)
			continue;

		const std::byte *payload = message->data() + rtpHeaderSize;
		const size_t payloadSize = message->size() - rtpHeaderSize - paddingSize;

		auto descriptor = impl::parseVp9PayloadDescriptor(payload, payloadSize);
		if (!descriptor.valid || payloadSize < descriptor.headerSize)
			continue; // truncated or contradictory descriptor: never emit partial

		const uint32_t timestamp = header->timestamp();
		const uint16_t seqNumber = header->seqNumber();

		// Decide whether this packet belongs to the current picture. A new
		// PictureID at the same timestamp still marks a new picture, so it is
		// checked before treating the packet as a duplicate of a delivered one.
		const bool newPictureId = descriptor.hasPictureId && mSpatial.hasPictureId &&
		                          descriptor.pictureId != mSpatial.pictureId;

		bool startNewPicture = false;
		if (!mSpatial.active) {
			startNewPicture = true;
		} else if (timestamp != mSpatial.timestamp) {
			startNewPicture = true;
		} else if (newPictureId) {
			startNewPicture = true;
		} else if (mSpatial.delivered) {
			// A packet of an already delivered picture (late higher layer,
			// marker, or duplicate low layer) is ignored.
			continue;
		}

		if (startNewPicture) {
			// The limit is sampled on the first valid packet of each picture.
			mSpatial.reset();
			mSpatial.active = true;
			mSpatial.limit = *mSpatialLayerLimit;
			mSpatial.timestamp = timestamp;
			mSpatial.payloadType = header->payloadType();
			mSpatial.hasPictureId = descriptor.hasPictureId;
			mSpatial.pictureId = descriptor.pictureId;
			mSpatial.lastSequence = seqNumber;
			mSpatial.lastUnwrapped = seqNumber;
		} else if (descriptor.hasPictureId && !mSpatial.hasPictureId) {
			// Adopt the PictureID from a later packet if the first one lacked it.
			mSpatial.hasPictureId = true;
			mSpatial.pictureId = descriptor.pictureId;
		}

		// Unwrap the 16-bit sequence number, tolerating reordering and wrap.
		int64_t unwrapped;
		{
			const int16_t delta =
			    static_cast<int16_t>(static_cast<uint16_t>(seqNumber - mSpatial.lastSequence));
			unwrapped = mSpatial.lastUnwrapped + delta;
			if (unwrapped > mSpatial.lastUnwrapped) {
				mSpatial.lastUnwrapped = unwrapped;
				mSpatial.lastSequence = seqNumber;
			}
		}

		auto &layer = mSpatial.layers[descriptor.sid];
		layer.used = true;
		layer.sawStart |= descriptor.startOfFrame;
		layer.sawEnd |= descriptor.endOfFrame;

		if (!layer.segments.count(unwrapped)) {
			SpatialSegment segment;
			segment.data.assign(payload + descriptor.headerSize,
			                    payload + payloadSize);
			segment.startOfFrame = descriptor.startOfFrame;
			segment.endOfFrame = descriptor.endOfFrame;
			layer.segments.emplace(unwrapped, std::move(segment));
		}

		// A layer's descriptor identity must stay consistent across its
		// packets; a change in either prediction flag is contradictory.
		if (!layer.identityKnown) {
			layer.interPicturePredicted = descriptor.interPicturePredicted;
			layer.interLayerPredicted = descriptor.interLayerPredicted;
			layer.identityKnown = true;
		} else if (layer.interPicturePredicted != descriptor.interPicturePredicted ||
		           layer.interLayerPredicted != descriptor.interLayerPredicted) {
			layer.contradictory = true;
		}

		if (descriptor.sid > mSpatial.highestSid)
			mSpatial.highestSid = descriptor.sid;

		if (header->marker()) {
			mSpatial.markerSeen = true;
			mSpatial.markerSid = descriptor.sid;
		}
	}

	if (!mSpatial.active || mSpatial.delivered)
		return nullptr;

	// Determine the target: the configured limit, or the actual highest layer
	// when the picture has fewer layers than the limit.
	const bool reachedLimit = mSpatial.highestSid >= mSpatial.limit;
	const uint8_t target = reachedLimit ? mSpatial.limit : mSpatial.highestSid;

	if (!reachedLimit) {
		// The highest actual layer ends the picture with its marker; only then
		// is a picture below the limit considered complete.
		if (!(mSpatial.markerSeen && mSpatial.markerSid == mSpatial.highestSid))
			return nullptr;
	}

	// Every retained layer, up to and including the target, must be complete.
	// A layer's descriptor identity must be consistent, a layer that depends on
	// the lower one must find it declared complete below it, and SID 0 must not
	// claim a lower-layer dependency.
	std::vector<binary> layerData(static_cast<size_t>(target) + 1);
	for (uint8_t sid = 0; sid <= target; ++sid) {
		auto &layer = mSpatial.layers[sid];
		if (!layer.used || layer.contradictory)
			return nullptr;
		if (sid == 0 && layer.interLayerPredicted)
			return nullptr;
		if (sid > 0 && layer.interLayerPredicted) {
			auto &below = mSpatial.layers[sid - 1];
			binary ignored;
			if (!below.used || below.contradictory || !completeLayer(below, ignored))
				return nullptr;
		}
		if (!completeLayer(layer, layerData[sid]))
			return nullptr;
	}

	binary frame;
	if (target == 0) {
		frame = std::move(layerData[0]);
	} else {
		std::vector<size_t> sizes;
		sizes.reserve(target + 1);
		for (uint8_t sid = 0; sid <= target; ++sid) {
			sizes.push_back(layerData[sid].size());
			frame.insert(frame.end(), layerData[sid].begin(), layerData[sid].end());
		}
		auto index = impl::buildVp9SuperframeIndex(sizes);
		frame.insert(frame.end(), index.begin(), index.end());
	}

	mSpatial.delivered = true;

	return make_message(std::move(frame),
	                    createFrameInfo(mSpatial.timestamp, mSpatial.payloadType));
}

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
