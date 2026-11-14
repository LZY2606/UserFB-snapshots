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
#include "vp9spatiallayers.hpp"

#include <map>
#include <stdexcept>

namespace rtc {

namespace {

constexpr uint8_t bitI = 0b10000000;
constexpr uint8_t bitP = 0b01000000;
constexpr uint8_t bitL = 0b00100000;
constexpr uint8_t bitF = 0b00010000;
constexpr uint8_t bitB = 0b00001000;
constexpr uint8_t bitE = 0b00000100;
constexpr uint8_t bitV = 0b00000010;

// Maximum number of spatial layers supported by the superframe index
constexpr uint8_t MaxSpatialLayers = 8;

struct Fragment {
	binary data;
	bool startOfFrame = false;
	bool endOfFrame = false;
};

struct LayerState {
	// Fragments keyed by sequence number relative to the picture anchor
	std::map<int16_t, Fragment> fragments;
	std::optional<int16_t> start; // relative sequence of the B fragment
	std::optional<int16_t> end;   // relative sequence of the E fragment

	bool complete() const {
		if (!start.has_value() || !end.has_value() || *end < *start)
			return false;

		// Every sequence number between the start and end fragments must be present
		for (int rel = int(*start); rel <= int(*end); ++rel) {
			if (fragments.find(int16_t(rel)) == fragments.end())
				return false;
		}
		return true;
	}

	binary assemble() const {
		binary out;
		for (int rel = int(*start); rel <= int(*end); ++rel) {
			const auto it = fragments.find(int16_t(rel));
			if (it == fragments.end())
				break;
			out.insert(out.end(), it->second.data.begin(), it->second.data.end());
		}
		return out;
	}
};

struct PictureState {
	bool active = false;
	uint32_t timestamp = 0;
	uint8_t payloadType = 0;
	uint16_t anchorSeq = 0;
	uint16_t pictureId = 0;
	uint8_t tl0PicIdx = 0;
	bool hasIdentity = false;
	uint8_t limit = 0; // pinned at the first valid packet of this picture
	bool emitted = false;
	bool contradictory = false;
	std::optional<uint8_t> endingSid; // marker seen on a layer at or below the limit
	std::map<uint8_t, LayerState> layers;
};

} // namespace

struct VP9RtpDepacketizer::Impl {
	optional<uint8_t> limit;
	PictureState picture;
};

VP9RtpDepacketizer::VP9RtpDepacketizer() : mImpl(std::make_unique<Impl>()) {}

VP9RtpDepacketizer::~VP9RtpDepacketizer() = default;

void VP9RtpDepacketizer::setSpatialLayerLimit(std::optional<uint8_t> maxSid) {
	if (maxSid.has_value() && *maxSid > 7)
		throw std::out_of_range("VP9 spatial layer limit must be in the range 0-7");

	mImpl->limit = maxSid;
}

void VP9RtpDepacketizer::incoming(message_vector &messages,
                                  [[maybe_unused]] const message_callback &send) {
	if (!mImpl->limit.has_value()) {
		// Legacy single-layer mode, identical to the base implementation
		VideoRtpDepacketizer::incoming(messages, send);
		return;
	}

	message_vector result;
	auto &picture = mImpl->picture;

	for (auto &message : messages) {
		if (message->type == Message::Control) {
			result.push_back(std::move(message));
			continue;
		}

		if (message->size() < sizeof(RtpHeader))
			continue;

		auto header = reinterpret_cast<const RtpHeader *>(message->data());
		const size_t headerSize = header->getSize();
		if (message->size() < headerSize)
			continue;

		const size_t totalHeaderSize = headerSize + header->getExtensionHeaderSize();
		if (message->size() < totalHeaderSize)
			continue;

		size_t paddingSize = 0;
		if (header->padding() && message->size() > totalHeaderSize) {
			paddingSize = std::to_integer<uint8_t>(message->back());
			if (paddingSize > message->size() - totalHeaderSize)
				continue;
		}

		const byte *payload = message->data() + totalHeaderSize;
		const size_t payloadSize = message->size() - totalHeaderSize - paddingSize;
		if (payloadSize == 0)
			continue;

		vp9spatial::ParsedDescriptor desc;
		if (!vp9spatial::parseDescriptor(payload, payloadSize, desc))
			continue; // Truncated descriptor

		// Only non-flexible spatial layer streams are supported here
		if (desc.flexible || !desc.pictureIdPresent || !desc.layerIndicesPresent)
			continue;

		if (desc.sid >= MaxSpatialLayers)
			continue;

		// SID 0 cannot depend on a lower spatial layer
		if (desc.sid == 0 && desc.interLayerPredicted)
			continue;

		if (desc.headerSize > payloadSize)
			continue;

		const uint32_t timestamp = header->timestamp();
		const uint16_t seq = header->seqNumber();

		if (picture.active && picture.timestamp != timestamp) {
			// A new picture discards the previous incomplete one
			picture = PictureState{};
		} else if (picture.active && picture.hasIdentity && picture.emitted &&
		           picture.pictureId != desc.pictureId) {
			// A different PictureID after delivery belongs to the next picture,
			// even if the timestamp happens to repeat.
			picture = PictureState{};
		}

		if (!picture.active) {
			picture.active = true;
			picture.timestamp = timestamp;
			picture.payloadType = header->payloadType();
			picture.anchorSeq = seq;
			picture.limit = *mImpl->limit; // pinned for the whole picture
		}

		if (picture.emitted || picture.contradictory) {
			// Late packets of an already delivered picture (or a broken one) are ignored
			continue;
		}

		// The descriptor identity must be consistent within a picture. A
		// mismatching PictureID or TL0PICIDX makes the picture undeliverable:
		// no partial frame is emitted and the next picture starts cleanly.
		if (!picture.hasIdentity) {
			picture.pictureId = desc.pictureId;
			picture.tl0PicIdx = desc.tl0PicIdx;
			picture.hasIdentity = true;
		} else if (picture.pictureId != desc.pictureId || picture.tl0PicIdx != desc.tl0PicIdx) {
			picture.contradictory = true;
			continue;
		}

		const int16_t rel = int16_t(seq - picture.anchorSeq);
		const size_t dataOffset = desc.headerSize;
		const size_t dataSize = payloadSize - dataOffset;

		auto &layer = picture.layers[desc.sid];
		auto [it, inserted] = layer.fragments.emplace(rel, Fragment{});
		if (!inserted)
			continue; // Duplicate packet

		Fragment &fragment = it->second;
		fragment.data.assign(payload + dataOffset, payload + dataOffset + dataSize);
		fragment.startOfFrame = desc.startOfFrame;
		fragment.endOfFrame = desc.endOfFrame;

		if (desc.startOfFrame) {
			if (layer.start.has_value() && *layer.start != rel) {
				picture.contradictory = true;
				continue;
			}
			layer.start = rel;
		}
		if (desc.endOfFrame) {
			if (layer.end.has_value() && *layer.end != rel) {
				picture.contradictory = true;
				continue;
			}
			layer.end = rel;
		}

		// The end of frame fragment must come after the start of frame fragment
		if (layer.start.has_value() && layer.end.has_value() && *layer.start > *layer.end) {
			picture.contradictory = true;
			continue;
		}

		// The marker is set on the last packet of the actual highest sent layer.
		// When that layer is at or below the limit, its end of frame fragment
		// ends the picture even if higher layers are missing. The marker may
		// arrive before the retained layers are complete, so remember its SID.
		if (header->marker()) {
			if (desc.sid > picture.limit)
				continue; // Marker above the limit: the picture continues
			picture.endingSid = desc.sid;
		}

		const std::optional<uint8_t> top =
		    picture.endingSid.has_value()
		        ? picture.endingSid
		        : optional<uint8_t>(picture.limit); // early delivery at the limit

		// The picture cannot be emitted while the top layer's end of frame
		// fragment is waiting for a missing start of frame fragment.
		bool complete = true;
		for (uint8_t sid = 0; sid <= *top; ++sid) {
			auto layerIt = picture.layers.find(sid);
			if (layerIt == picture.layers.end() || !layerIt->second.complete()) {
				complete = false;
				break;
			}
		}

		if (!complete)
			continue; // Wait for the missing fragments

		std::vector<binary> frames;
		frames.reserve(size_t(*top) + 1);
		for (uint8_t sid = 0; sid <= *top; ++sid)
			frames.push_back(picture.layers[sid].assemble());

		picture.emitted = true;
		picture.layers.clear();

		auto frame = vp9spatial::buildSuperframe(frames);
		result.push_back(make_message(std::move(frame),
		                              createFrameInfo(picture.timestamp, picture.payloadType)));
	}

	messages.swap(result);
}

message_ptr VP9RtpDepacketizer::reassemble(message_buffer &buffer) {
	/*
	 * VP9 RTP payload format (RFC 9628)
	 * See https://datatracker.ietf.org/doc/html/rfc9628
	 *
	 * Payload descriptor:
	 *
	 *       0 1 2 3 4 5 6 7
	 *      +-+-+-+-+-+-+-+-+
	 *      |I|P|L|F|B|E|V|Z| (REQUIRED)
	 *      +-+-+-+-+-+-+-+-+
	 *   I: |M| PICTURE ID  | (RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *   M: | EXTENDED PID  | (RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *   L: | TID |U| SID |D| (CONDITIONALLY RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *      |   TL0PICIDX   | (CONDITIONALLY REQUIRED, only if L=1 and F=0)
	 *      +-+-+-+-+-+-+-+-+
	 * P,F: | P_DIFF    |N  | (CONDITIONALLY REQUIRED, up to 3 times)
	 *      +-+-+-+-+-+-+-+-+
	 *   V: | N_S |Y|G|-|-|-| (CONDITIONALLY RECOMMENDED)
	 *      +-+-+-+-+-+-+-+-+
	 *   Y: |    WIDTH (16)  | (OPTIONAL, per spatial layer)
	 *      +-+-+-+-+-+-+-+-+
	 *      |    HEIGHT (16) | (OPTIONAL, per spatial layer)
	 *      +-+-+-+-+-+-+-+-+
	 *   G: |     N_G       | (OPTIONAL)
	 *      +-+-+-+-+-+-+-+-+
	 * N_G: | TID |U| R |-|-| (OPTIONAL, per picture group)
	 *      +-+-+-+-+-+-+-+-+
	 *      |   P_DIFF      | (OPTIONAL, R times per picture group)
	 *      +-+-+-+-+-+-+-+-+
	 *
	 * I: PictureID present
	 * P: Inter-picture predicted frame
	 * L: Layer indices present
	 * F: Flexible mode (F=1 implies I=1)
	 * B: Start of a frame
	 * E: End of a frame
	 * V: Scalability Structure (SS) data present
	 * Z: Not a reference frame for upper spatial layers
	 * M: PictureID is 15 bits (extended)
	 */

	// First byte bit masks
	const uint8_t bitN = 0b00000001;

	// Maximum number of reference indices in flexible mode
	const int maxRefPics = 3;

	if (buffer.empty())
		return nullptr;

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
		size_t descriptorSize = 1;
		uint8_t firstByte = std::to_integer<uint8_t>(payloadData[0]);

		// PictureID (conditional: I=1)
		if (firstByte & bitI) {
			if (payloadSize < descriptorSize + 1)
				continue;
			uint8_t pictureIdByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
			descriptorSize++;
			if (pictureIdByte & 0x80) { // 15-bit extended PictureID
				if (payloadSize < descriptorSize + 1)
					continue;
				descriptorSize++;
			}
		}

		// Layer indices (conditional: L=1)
		if (firstByte & bitL) {
			if (payloadSize < descriptorSize + 1)
				continue;
			descriptorSize++; // TID | U | SID | D

			// TL0PICIDX present only in non-flexible mode (F=0)
			if (!(firstByte & bitF)) {
				if (payloadSize < descriptorSize + 1)
					continue;
				descriptorSize++;
			}
		}

		// Reference indices (conditional: P=1 and F=1, flexible mode)
		if ((firstByte & bitP) && (firstByte & bitF)) {
			for (int i = 0; i < maxRefPics; i++) {
				if (payloadSize < descriptorSize + 1)
					break;
				uint8_t refByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
				descriptorSize++;
				if (!(refByte & bitN)) // N=0 means last reference index
					break;
			}
		}

		// Scalability Structure (conditional: V=1)
		if (firstByte & bitV) {
			if (payloadSize < descriptorSize + 1)
				continue;
			uint8_t ssByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
			descriptorSize++;

			int numSpatialLayers = (ssByte >> 5) + 1; // N_S + 1
			bool resolutionPresent = (ssByte >> 4) & 0x01; // Y bit
			bool pgPresent = (ssByte >> 3) & 0x01; // G bit

			// Resolution data: WIDTH (2 bytes) + HEIGHT (2 bytes) per spatial layer
			if (resolutionPresent) {
				size_t resolutionSize = 4 * static_cast<size_t>(numSpatialLayers);
				if (payloadSize < descriptorSize + resolutionSize)
					continue;
				descriptorSize += resolutionSize;
			}

			// Picture Group description
			if (pgPresent) {
				if (payloadSize < descriptorSize + 1)
					continue;
				uint8_t numPictureGroups = std::to_integer<uint8_t>(payloadData[descriptorSize]);
				descriptorSize++;

				bool ssTruncated = false;
				for (int i = 0; i < numPictureGroups; i++) {
					if (payloadSize < descriptorSize + 1) {
						ssTruncated = true;
						break;
					}
					uint8_t pgByte = std::to_integer<uint8_t>(payloadData[descriptorSize]);
					descriptorSize++;
					int numRefs = (pgByte >> 2) & 0x03; // R field
					if (payloadSize < descriptorSize + static_cast<size_t>(numRefs)) {
						ssTruncated = true;
						break;
					}
					descriptorSize += numRefs;
				}
				if (ssTruncated)
					continue;
			}
		}

		if (payloadSize < descriptorSize)
			continue;

		payloadData += descriptorSize;
		payloadSize -= descriptorSize;

		// Frame reassembly using B (start of frame) and marker bit
		if ((firstByte & bitB) || rtpHeader->marker()) {
			if (continuousSequence) {
				for (auto [data, size] : payloads)
					frame.insert(frame.end(), data, data + size);

				if (rtpHeader->marker())
					frame.insert(frame.end(), payloadData, payloadData + payloadSize);
			}
			else if ((firstByte & bitB) && rtpHeader->marker()) {
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

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */
