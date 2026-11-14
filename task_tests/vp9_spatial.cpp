// VP9 spatial layer regression tests.
//
// Registered groups: early, layers, continuity.
// Every case checks real encoder output through the production entry points
// (VP9RtpPacketizer::outgoingChain and VP9RtpDepacketizer::incomingChain).

#include "../task_support/check.h"

#include "vp9rtpdepacketizer.hpp"
#include "vp9rtppacketizer.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace rtc;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

constexpr uint8_t kPayloadType = 98;
constexpr uint32_t kSsrc = 4242;

struct RtpView {
	uint16_t seq;
	uint32_t timestamp;
	uint8_t payloadType;
	bool marker;
	uint8_t firstDescriptor;
	bool b;
	bool e;
	bool p;
	bool l;
	bool i;
	uint16_t pictureId;
	uint8_t sid;
	uint8_t tid;
	uint8_t tl0PicIdx;
	bool d;
	size_t headerSize;
	binary data;
};

binary makeFrame(std::vector<binary> layers) {
	binary frame;
	for (const auto &layer : layers)
		frame.insert(frame.end(), layer.begin(), layer.end());
	return frame;
}

shared_ptr<FrameInfo> makeFrameInfo(uint32_t timestamp, std::vector<size_t> sizes,
                                    bool interPicture, bool interLayerOnUpper) {
	auto fi = std::make_shared<FrameInfo>(timestamp);
	for (size_t i = 0; i < sizes.size(); ++i) {
		VP9SpatialLayer layer;
		layer.size = sizes[i];
		layer.interPicturePredicted = interPicture;
		layer.interLayerPredicted = interLayerOnUpper && i > 0;
		fi->vp9SpatialLayers.push_back(layer);
	}
	return fi;
}

message_ptr makeInput(binary frame, shared_ptr<FrameInfo> fi) {
	return make_message(std::move(frame), std::move(fi));
}

shared_ptr<RtpPacketizationConfig> makeConfig(uint16_t seq = 100) {
	auto cfg = std::make_shared<RtpPacketizationConfig>(kSsrc, "vp9-spatial", kPayloadType, 90000);
	cfg->sequenceNumber = seq;
	return cfg;
}

uint16_t readBE16(const byte *p) { return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1])); }
uint16_t readLE16(const byte *p) { return uint16_t((uint16_t(p[1]) << 8) | uint16_t(p[0])); }

// Parses the RTP packet produced by the packetizer for non-flexible spatial layers.
RtpView parsePacket(const message_ptr &message) {
	RtpView view{};
	const auto *header = reinterpret_cast<const RtpHeader *>(message->data());
	view.seq = header->seqNumber();
	view.timestamp = header->timestamp();
	view.payloadType = header->payloadType();
	view.marker = header->marker() != 0;

	const size_t headerSize = header->getSize() + header->getExtensionHeaderSize();
	view.headerSize = headerSize;
	const byte *payload = message->data() + headerSize;
	const size_t payloadSize = message->size() - headerSize;

	view.firstDescriptor = std::to_integer<uint8_t>(payload[0]);
	view.i = (view.firstDescriptor & 0x80) != 0;
	view.p = (view.firstDescriptor & 0x40) != 0;
	view.l = (view.firstDescriptor & 0x20) != 0;
	view.b = (view.firstDescriptor & 0x08) != 0;
	view.e = (view.firstDescriptor & 0x04) != 0;

	// I=1: 15-bit extended PictureID
	size_t offset = 1;
	view.pictureId = uint16_t(((std::to_integer<uint8_t>(payload[offset]) & 0x7F) << 8) |
	                          std::to_integer<uint8_t>(payload[offset + 1]));
	offset += 2;

	// L=1: TID | U | SID | D
	const uint8_t layer = std::to_integer<uint8_t>(payload[offset]);
	view.tid = uint8_t((layer >> 5) & 0x07);
	view.sid = uint8_t((layer >> 1) & 0x07);
	view.d = (layer & 0x01) != 0;
	offset += 1;

	// TL0PICIDX in non-flexible mode
	view.tl0PicIdx = std::to_integer<uint8_t>(payload[offset]);
	offset += 1;

	view.data.assign(payload + offset, payload + payloadSize);
	return view;
}

std::vector<RtpView> runSender(VP9RtpPacketizer &packetizer, const binary &frame,
                               shared_ptr<FrameInfo> fi) {
	message_vector messages{makeInput(frame, std::move(fi))};
	packetizer.outgoingChain(messages, [](message_ptr) {});
	std::vector<RtpView> views;
	views.reserve(messages.size());
	for (const auto &message : messages)
		views.push_back(parsePacket(message));
	return views;
}

message_ptr makeRtpPacket(uint16_t seq, uint32_t timestamp, bool marker, uint8_t sid,
                          bool interLayerPredicted, bool b, bool e, uint16_t pictureId,
                          uint8_t tl0PicIdx, const binary &data, uint8_t payloadType = kPayloadType) {
	binary payload;
	uint8_t descriptor = 0x80 | 0x20; // I=1, L=1
	if (interLayerPredicted)
		descriptor |= 0x40; // P bit is not used by the depacketizer, harmless
	if (b)
		descriptor |= 0x08;
	if (e)
		descriptor |= 0x04;
	payload.push_back(byte(descriptor));
	payload.push_back(byte(0x80 | ((pictureId >> 8) & 0x7F)));
	payload.push_back(byte(pictureId & 0xFF));
	payload.push_back(byte((sid << 1) | (interLayerPredicted ? 1 : 0)));
	payload.push_back(byte(tl0PicIdx));
	payload.insert(payload.end(), data.begin(), data.end());

	auto message = make_message(12 + payload.size());
	auto *header = reinterpret_cast<RtpHeader *>(message->data());
	header->preparePacket();
	header->setSeqNumber(seq);
	header->setTimestamp(timestamp);
	header->setSsrc(kSsrc);
	header->setPayloadType(payloadType);
	header->setMarker(marker);
	std::memcpy(message->data() + 12, payload.data(), payload.size());
	return message;
}

std::vector<message_ptr> runReceiver(VP9RtpDepacketizer &depacketizer,
                                     const std::vector<message_ptr> &packets) {
	message_vector messages = packets;
	depacketizer.incomingChain(messages, [](message_ptr) {});
	return messages;
}

// Reads the VP9 superframe index appended to a frame built by the receiver.
bool parseSuperframeIndex(const binary &frame, size_t &bytesPerSize, size_t &numFrames,
                          std::vector<size_t> &sizes) {
	if (frame.size() < 3)
		return false;
	const uint8_t marker = std::to_integer<uint8_t>(frame.back());
	if ((marker & 0xE0) != 0xC0)
		return false;
	if (std::to_integer<uint8_t>(frame[frame.size() - 1]) != marker)
		return false;

	bytesPerSize = ((marker >> 3) & 0x03) + 1;
	numFrames = (marker & 0x07) + 1;
	const size_t indexSize = 1 + numFrames * bytesPerSize + 1;
	if (frame.size() < indexSize)
		return false;

	const size_t indexStart = frame.size() - indexSize;
	sizes.clear();
	for (size_t i = 0; i < numFrames; ++i) {
		size_t value = 0;
		for (size_t b = 0; b < bytesPerSize; ++b)
			value |= size_t(std::to_integer<uint8_t>(frame[indexStart + 1 + i * bytesPerSize + b]))
			         << (8 * b);
		sizes.push_back(value + 1);
	}
	return true;
}

// Builds a payload byte with the given value.
binary pattern(size_t size, uint8_t start) {
	binary out(size);
	for (size_t i = 0; i < size; ++i)
		out[i] = byte(uint8_t(start + i));
	return out;
}

// Adds a one-byte RTP header extension and RTP padding to a packet.
void addExtensionAndPadding(message_ptr &packet) {
	const size_t extensionBytes = 8; // one 1-byte header + 4 bytes of data, padded to 4
	const size_t paddingBytes = 3;
	const size_t oldSize = packet->size();

	// Rewrite the header with X=1 and P=1
	auto *header = reinterpret_cast<RtpHeader *>(packet->data());
	header->setExtension(true);
	header->preparePacket();

	binary rebuilt(oldSize);
	std::memcpy(rebuilt.data(), packet->data(), oldSize);

	// Extension header (profile 0xBEDE) with one-byte header id 1, length 4
	binary extension(extensionBytes);
	extension[0] = byte(0xBE);
	extension[1] = byte(0xDE);
	extension[2] = byte(0x00);
	extension[3] = byte(0x01); // one word
	extension[4] = byte(0x11); // id 1, length 4
	extension[5] = byte(0xAA);
	extension[6] = byte(0xBB);
	extension[7] = byte(0xCC);

	// The extension goes right after the fixed 12-byte RTP header
	rebuilt.insert(rebuilt.begin() + 12, extension.begin(), extension.end());
	// Padding bytes: optional garbage then the count
	for (size_t i = 0; i + 1 < paddingBytes; ++i)
		rebuilt.push_back(byte(0xEE));
	rebuilt.push_back(byte(paddingBytes));

	// Set P=1 in the first byte
	rebuilt[0] = byte(std::to_integer<uint8_t>(rebuilt[0]) | 0x20);

	packet = make_message(std::move(rebuilt));
}

// Reads the first payload byte of an extended packet, past the extension header.
uint8_t payloadOf(const message_ptr &packet, size_t &offset, size_t &size) {
	const auto *header = reinterpret_cast<const RtpHeader *>(packet->data());
	offset = header->getSize() + header->getExtensionHeaderSize();
	size = packet->size() - offset;
	return std::to_integer<uint8_t>((*packet)[offset]);
}

} // namespace

// ---------------------------------------------------------------------------
// Sender: descriptor, packet bounds, counters, recovery
// ---------------------------------------------------------------------------

static int sender_descriptor_and_bounds() {
	auto cfg = makeConfig(100);
	VP9RtpPacketizer packetizer(cfg, 8);
	const auto frame = makeFrame({pattern(3, 0x10), pattern(2, 0x20)});
	auto fi = makeFrameInfo(20000, {3, 2}, /*interPicture=*/true, /*interLayerOnUpper=*/true);

	auto views = runSender(packetizer, frame, fi);
	TASK_CHECK(!views.empty());

	// Two layers, each fits in a single fragment (3 and 2 payload bytes with an 8-byte budget).
	TASK_CHECK(views.size() == 2);
	TASK_CHECK(views[0].marker == false);
	TASK_CHECK(views[1].marker == true);
	TASK_CHECK(views[0].sid == 0 && views[1].sid == 1);
	TASK_CHECK(views[0].i && views[0].l);
	TASK_CHECK(views[0].b == true && views[0].e == true);
	TASK_CHECK(views[1].b == true && views[1].e == true);
	TASK_CHECK(views[0].p == true && views[1].p == true);
	TASK_CHECK(views[0].d == false && views[1].d == true);
	TASK_CHECK(views[0].tid == 0 && views[1].tid == 0);
	TASK_CHECK(views[0].timestamp == 20000 && views[1].timestamp == 20000);
	TASK_CHECK(views[0].pictureId == 0 && views[1].pictureId == 0);
	TASK_CHECK(views[0].tl0PicIdx == 0 && views[1].tl0PicIdx == 0);
	TASK_CHECK(views[0].payloadType == kPayloadType);
	TASK_CHECK(views[0].data == pattern(3, 0x10));
	TASK_CHECK(views[1].data == pattern(2, 0x20));

	// maxFragmentSize includes the descriptor: 5-byte descriptor + up to 3 data bytes.
	for (const auto &view : views)
		TASK_CHECK(5 + view.data.size() <= 8);

	// Counters advance once per picture
	auto fi2 = makeFrameInfo(21000, {3, 2}, false, false);
	auto views2 = runSender(packetizer, frame, fi2);
	TASK_CHECK(views2.size() == 2);
	TASK_CHECK(views2[0].pictureId == 1 && views2[1].pictureId == 1);
	TASK_CHECK(views2[0].tl0PicIdx == 1 && views2[1].tl0PicIdx == 1);
	TASK_CHECK(views2[0].timestamp == 21000);

	return 0;
}

static int sender_packet_boundaries() {
	auto cfg = makeConfig(50);
	VP9RtpPacketizer packetizer(cfg, 8); // 5-byte descriptor leaves 3 data bytes per packet
	const auto frame = makeFrame({pattern(7, 0x30), pattern(4, 0x50)});
	auto fi = makeFrameInfo(30000, {7, 4}, true, true);

	auto views = runSender(packetizer, frame, fi);
	// Layer 0: 7 bytes -> 3 fragments; layer 1: 4 bytes -> 2 fragments.
	TASK_CHECK(views.size() == 5);
	TASK_CHECK(views[0].sid == 0 && views[0].b && !views[0].e);
	TASK_CHECK(views[1].sid == 0 && !views[1].b && !views[1].e);
	TASK_CHECK(views[2].sid == 0 && !views[2].b && views[2].e);
	TASK_CHECK(views[3].sid == 1 && views[3].b && !views[3].e);
	TASK_CHECK(views[4].sid == 1 && !views[4].b && views[4].e);
	TASK_CHECK(views[4].marker == true);
	for (size_t i = 0; i + 1 < views.size(); ++i)
		TASK_CHECK(views[i].marker == false);
	// No packet straddles two layers
	for (const auto &view : views)
		TASK_CHECK(view.data.size() <= 3 && !view.data.empty());

	// Picture data reassembles exactly, layer by layer
	binary layer0, layer1;
	for (const auto &view : views) {
		if (view.sid == 0)
			layer0.insert(layer0.end(), view.data.begin(), view.data.end());
		else
			layer1.insert(layer1.end(), view.data.begin(), view.data.end());
	}
	TASK_CHECK(layer0 == pattern(7, 0x30));
	TASK_CHECK(layer1 == pattern(4, 0x50));
	TASK_CHECK(views[3].pictureId == views[0].pictureId);

	return 0;
}

static int sender_invalid_layers_recovery() {
	auto cfg = makeConfig(7);
	const uint32_t initialTimestamp = cfg->timestamp;
	VP9RtpPacketizer packetizer(cfg, 8);
	const auto frame = makeFrame({pattern(4, 0x60), pattern(4, 0x70)});

	// Each malformed description throws std::invalid_argument
	auto expectThrow = [&](std::vector<size_t> sizes, bool sidZeroInterLayer, size_t maxFragment) {
		auto fi = std::make_shared<FrameInfo>(uint32_t(40000));
		for (size_t i = 0; i < sizes.size(); ++i) {
			VP9SpatialLayer layer;
			layer.size = sizes[i];
			layer.interPicturePredicted = true;
			layer.interLayerPredicted = sidZeroInterLayer && i == 0;
			fi->vp9SpatialLayers.push_back(layer);
		}
		auto boxed = std::make_shared<VP9RtpPacketizer>(makeConfig(7), maxFragment);
		message_vector messages{makeInput(frame, fi)};
		bool threw = false;
		try {
			boxed->outgoingChain(messages, [](message_ptr) {});
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		return threw && messages.size() == 1; // Nothing was sent
	};

	// Sum does not match
	TASK_CHECK(expectThrow({4, 5}, false, 8));
	// Zero-sized layer
	TASK_CHECK(expectThrow({0, 8}, false, 8));
	// SID 0 declaring inter-layer prediction
	TASK_CHECK(expectThrow({4, 4}, true, 8));
	// Descriptor + one data byte does not fit
	TASK_CHECK(expectThrow({4, 4}, false, 5));
	// Too many layers
	TASK_CHECK(expectThrow({1, 1, 1, 1, 1, 1, 1, 1, 1}, false, 8));

	// Sequence number, timestamp and picture counters were not consumed
	TASK_CHECK(cfg->sequenceNumber == 7);
	TASK_CHECK(cfg->timestamp == initialTimestamp); // untouched (configured by user)

	// The next valid picture is sent with picture 0, TL0PICIDX 0, sequence 7
	auto fi = makeFrameInfo(41000, {4, 4}, false, false);
	auto views = runSender(packetizer, frame, fi);
	// Each 4-byte layer needs two fragments (3 data bytes per packet)
	TASK_CHECK(views.size() == 4);
	TASK_CHECK(views[0].seq == 7);
	TASK_CHECK(views[0].pictureId == 0 && views[0].tl0PicIdx == 0);
	TASK_CHECK(views[0].timestamp == 41000);
	TASK_CHECK(views[0].sid == 0 && views[2].sid == 1);
	TASK_CHECK(views[3].marker == true);

	return 0;
}

static int sender_counter_wraparound() {
	auto cfg = makeConfig(1);
	VP9RtpPacketizer packetizer(cfg, 64);
	const auto frame = makeFrame({pattern(2, 0x11), pattern(2, 0x22)});

	uint16_t expectedPictureId = 0;
	uint8_t expectedTl0 = 0;
	for (int i = 0; i < 33000; ++i) {
		auto fi = makeFrameInfo(uint32_t(1000 + i), {2, 2}, true, true);
		message_vector messages{makeInput(frame, fi)};
		packetizer.outgoingChain(messages, [](message_ptr) {});
		if (i == 0 || i == 1 || i == 32767 || i == 32768 || i == 255 || i == 256) {
			TASK_CHECK(!messages.empty());
			auto view = parsePacket(messages.front());
			TASK_CHECK(view.pictureId == expectedPictureId);
			TASK_CHECK(view.tl0PicIdx == expectedTl0);
		}
		expectedPictureId = uint16_t((expectedPictureId + 1) & 0x7FFF);
		expectedTl0 = uint8_t(expectedTl0 + 1);
	}
	return 0;
}

static int sender_legacy_compatibility() {
	auto cfg = makeConfig(10);
	VP9RtpPacketizer packetizer(cfg, 8);
	auto fi = std::make_shared<FrameInfo>(uint32_t(5555));
	const auto frame = pattern(20, 0x40);
	message_vector messages{makeInput(frame, fi)};
	packetizer.outgoingChain(messages, [](message_ptr) {});
	TASK_CHECK(messages.size() == 3);
	for (size_t i = 0; i < messages.size(); ++i) {
		const auto *header = reinterpret_cast<const RtpHeader *>(messages[i]->data());
		TASK_CHECK((header->marker() != 0) == (i + 1 == messages.size()));
		// Legacy 1-byte descriptor: B on the first packet, E on the last
		const bool expectedB = i == 0;
		const bool expectedE = i + 1 == messages.size();
		TASK_CHECK(std::to_integer<uint8_t>((*messages[i])[12]) ==
		           uint8_t((expectedB ? 0x08 : 0x00) | (expectedE ? 0x04 : 0x00)));
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Receiver: early output and superframe construction
// ---------------------------------------------------------------------------

static int receiver_early_output_without_marker() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1));

	auto layer0 = pattern(3, 0x80);
	auto layer1 = pattern(4, 0x90);

	// Layer 0 complete (no marker), layer 1 complete. No marker seen yet.
	auto out = runReceiver(depacketizer,
	                       {makeRtpPacket(1, 9000, false, 0, false, true, true, 5, 9, layer0),
	                        makeRtpPacket(2, 9000, false, 1, true, true, true, 5, 9, layer1)});
	TASK_CHECK(out.size() == 1);
	TASK_CHECK(out[0]->frameInfo && out[0]->frameInfo->timestamp == 9000);
	TASK_CHECK(out[0]->frameInfo->payloadType == kPayloadType);

	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*out[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2);
	TASK_CHECK(sizes.size() == 2);
	TASK_CHECK(sizes[0] == layer0.size() && sizes[1] == layer1.size());
	TASK_CHECK(bytesPerSize == 1); // largest layer is 4 bytes
	// The two frames are concatenated in SID order before the index
	TASK_CHECK((*out[0])[0] == layer0[0]);
	TASK_CHECK((*out[0])[3] == layer1[0]);

	// The marker that follows must not deliver the picture a second time
	auto again = runReceiver(depacketizer,
	                         {makeRtpPacket(2, 9000, true, 1, true, false, true, 5, 9, layer1)});
	TASK_CHECK(again.empty());

	// Late duplicate of a lower layer is dropped too
	auto dup = runReceiver(depacketizer,
	                       {makeRtpPacket(1, 9000, false, 0, false, true, true, 5, 9, layer0)});
	TASK_CHECK(dup.empty());
	return 0;
}

static int receiver_two_and_three_layers() {
	// Two retained layers, delivered on the marker of the highest layer
	{
		VP9RtpDepacketizer depacketizer;
		depacketizer.setSpatialLayerLimit(uint8_t(2));
		auto l0 = pattern(2, 0xA0);
		auto l1 = pattern(8, 0xB0);
		auto l2 = pattern(3, 0xC0);

		std::vector<message_ptr> packets;
		packets.push_back(makeRtpPacket(10, 7000, false, 0, false, true, true, 1, 2, l0));
		packets.push_back(makeRtpPacket(11, 7000, false, 1, true, true, true, 1, 2, l1));
		TASK_CHECK(runReceiver(depacketizer, packets).empty());

		auto marker = runReceiver(
		    depacketizer, {makeRtpPacket(12, 7000, true, 2, true, true, true, 1, 2, l2)});
		TASK_CHECK(marker.size() == 1);
		size_t bytesPerSize = 0, numFrames = 0;
		std::vector<size_t> sizes;
		TASK_CHECK(parseSuperframeIndex(*marker[0], bytesPerSize, numFrames, sizes));
		TASK_CHECK(numFrames == 3);
		TASK_CHECK(bytesPerSize == 1);
		TASK_CHECK(sizes[0] == 2 && sizes[1] == 8 && sizes[2] == 3);
	}

	// Limit 1 with three layers sent: only SID 0 and 1 are retained
	{
		VP9RtpDepacketizer depacketizer;
		depacketizer.setSpatialLayerLimit(uint8_t(1));
		auto l0 = pattern(5, 0xD0);
		auto l1 = pattern(2, 0xE0);
		auto l2 = pattern(9, 0xF0);

		std::vector<message_ptr> packets;
		packets.push_back(makeRtpPacket(20, 8000, false, 0, false, true, true, 3, 4, l0));
		packets.push_back(makeRtpPacket(21, 8000, false, 1, true, true, true, 3, 4, l1));
		// Complete immediately after SID 1
		auto out = runReceiver(depacketizer, packets);
		TASK_CHECK(out.size() == 1);
		size_t bytesPerSize = 0, numFrames = 0;
		std::vector<size_t> sizes;
		TASK_CHECK(parseSuperframeIndex(*out[0], bytesPerSize, numFrames, sizes));
		TASK_CHECK(numFrames == 2);
		TASK_CHECK(sizes[0] == 5 && sizes[1] == 2);

		// The higher-layer marker must not produce another frame
		auto higher = runReceiver(
		    depacketizer, {makeRtpPacket(22, 8000, true, 2, true, true, true, 3, 4, l2)});
		TASK_CHECK(higher.empty());
	}

	// A single retained layer is delivered as the raw frame
	{
		VP9RtpDepacketizer depacketizer;
		depacketizer.setSpatialLayerLimit(uint8_t(0));
		auto l0 = pattern(6, 0x55);
		auto out = runReceiver(
		    depacketizer, {makeRtpPacket(30, 8100, true, 0, false, true, true, 7, 8, l0)});
		TASK_CHECK(out.size() == 1);
		TASK_CHECK(*out[0] == l0);
	}
	return 0;
}

static int receiver_limit_validation() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(7));
	depacketizer.setSpatialLayerLimit(std::nullopt);

	bool threw = false;
	try {
		depacketizer.setSpatialLayerLimit(uint8_t(8));
	} catch (const std::out_of_range &) {
		threw = true;
	}
	TASK_CHECK(threw);

	// Values 0 through 7 are accepted
	for (uint8_t sid = 0; sid <= 7; ++sid)
		depacketizer.setSpatialLayerLimit(sid);

	// nullopt restores the legacy mode: no early delivery, marker-based output
	depacketizer.setSpatialLayerLimit(std::nullopt);
	auto l0 = pattern(3, 0x01);
	auto l1 = pattern(2, 0x02);
	auto early = runReceiver(depacketizer,
	                         {makeRtpPacket(90, 43000, false, 0, false, true, true, 1, 2, l0),
	                          makeRtpPacket(91, 43000, false, 1, true, true, true, 1, 2, l1)});
	TASK_CHECK(early.empty());
	auto marked = runReceiver(
	    depacketizer, {makeRtpPacket(92, 43000, true, 1, true, false, true, 1, 2, pattern(1, 0x03))});
	TASK_CHECK(marked.size() == 1);
	return 0;
}

// ---------------------------------------------------------------------------
// Continuity: out-of-order, wraparound, limit changes, no duplicates
// ---------------------------------------------------------------------------

static int continuity_out_of_order_across_zero() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1));

	auto l0a = pattern(2, 0x01);
	auto l0b = pattern(2, 0x02);
	auto l1 = pattern(3, 0x03);

	// Picture starts just before the 16-bit sequence number wraps
	// SID 0 split over seq 65535 (B) and seq 1 (E); seq 0 arrives after seq 1.
	auto first = runReceiver(
	    depacketizer, {makeRtpPacket(65535, 12000, false, 0, false, true, false, 2, 3, l0a)});
	TASK_CHECK(first.empty());

	auto third = runReceiver(
	    depacketizer, {makeRtpPacket(1, 12000, false, 0, false, false, true, 2, 3, l0b)});
	TASK_CHECK(third.empty()); // seq 0 still missing

	// Marker of SID 1 arrives, but SID 0 is still incomplete
	auto marker = runReceiver(
	    depacketizer, {makeRtpPacket(2, 12000, true, 1, true, true, true, 2, 3, l1)});
	TASK_CHECK(marker.empty());

	// The missing seq 0 fills the gap and this call emits the picture
	auto filled = runReceiver(
	    depacketizer, {makeRtpPacket(0, 12000, false, 0, false, false, false, 2, 3,
	                                 pattern(1, 0x04))});
	TASK_CHECK(filled.size() == 1);

	binary expectedLayer0 = l0a;
	expectedLayer0.push_back(byte(0x04));
	expectedLayer0.insert(expectedLayer0.end(), l0b.begin(), l0b.end());

	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*filled[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2);
	TASK_CHECK(sizes[0] == expectedLayer0.size() && sizes[1] == l1.size());
	TASK_CHECK((*filled[0])[0] == expectedLayer0[0] &&
	           (*filled[0])[expectedLayer0.size() - 1] == expectedLayer0.back());
	return 0;
}

static int continuity_limit_change_between_pictures() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(0));

	auto l0 = pattern(2, 0x10);
	auto l1 = pattern(4, 0x20);
	auto out0 = runReceiver(
	    depacketizer, {makeRtpPacket(100, 20000, true, 0, false, true, true, 10, 11, l0)});
	TASK_CHECK(out0.size() == 1);
	TASK_CHECK(*out0[0] == l0);

	// Raise the limit; it applies from the next picture onwards
	depacketizer.setSpatialLayerLimit(uint8_t(1));
	auto out1 = runReceiver(depacketizer,
	                        {makeRtpPacket(101, 21000, false, 0, false, true, true, 11, 12, l0),
	                         makeRtpPacket(102, 21000, false, 1, true, true, true, 11, 12, l1)});
	TASK_CHECK(out1.size() == 1);
	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*out1[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2);

	// Lower it again while a picture is in flight: the in-flight picture keeps
	// its pinned limit, the following picture uses the new one.
	depacketizer.setSpatialLayerLimit(uint8_t(1));
	auto started = runReceiver(
	    depacketizer, {makeRtpPacket(103, 22000, false, 0, false, true, true, 12, 13, l0)});
	TASK_CHECK(started.empty());
	depacketizer.setSpatialLayerLimit(uint8_t(0));
	auto finished = runReceiver(
	    depacketizer, {makeRtpPacket(104, 22000, true, 1, true, true, true, 12, 13, l1)});
	// The pinned limit was 1, so the picture completes with two layers
	TASK_CHECK(finished.size() == 1);
	TASK_CHECK(parseSuperframeIndex(*finished[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2);

	// The next picture honours the new limit of 0
	auto next = runReceiver(
	    depacketizer, {makeRtpPacket(105, 23000, true, 0, false, true, true, 13, 14, l0)});
	TASK_CHECK(next.size() == 1);
	TASK_CHECK(*next[0] == l0);
	return 0;
}

static int continuity_descriptor_conflicts() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1));
	auto l0 = pattern(3, 0x31);
	auto l1 = pattern(2, 0x32);

	// Two different B fragments for the same layer: no partial frame
	auto bad = runReceiver(
	    depacketizer, {makeRtpPacket(200, 30000, false, 0, false, true, false, 20, 21, l0),
	                   makeRtpPacket(201, 30000, false, 0, false, true, false, 20, 21, l0)});
	TASK_CHECK(bad.empty());

	// A new picture replaces the broken one and is delivered normally
	auto good = runReceiver(depacketizer,
	                        {makeRtpPacket(300, 31000, false, 0, false, true, true, 21, 22, l0),
	                         makeRtpPacket(301, 31000, true, 1, true, true, true, 21, 22, l1)});
	TASK_CHECK(good.size() == 1);
	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*good[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2 && sizes[0] == 3 && sizes[1] == 2);

	// Conflicting PictureID inside one picture: nothing is delivered
	{
		VP9RtpDepacketizer conflicted;
		conflicted.setSpatialLayerLimit(uint8_t(1));
		auto bad = runReceiver(
		    conflicted, {makeRtpPacket(400, 32000, false, 0, false, true, true, 30, 31, l0),
		                 makeRtpPacket(401, 32000, true, 1, true, true, true, 31, 31, l1)});
		TASK_CHECK(bad.empty());
		// The following picture is unaffected
		auto after = runReceiver(
		    conflicted, {makeRtpPacket(500, 33000, false, 0, false, true, true, 32, 33, l0),
		                 makeRtpPacket(501, 33000, true, 1, true, true, true, 32, 33, l1)});
		TASK_CHECK(after.size() == 1);
	}

	// Conflicting TL0PICIDX inside one picture: nothing is delivered
	{
		VP9RtpDepacketizer conflicted;
		conflicted.setSpatialLayerLimit(uint8_t(1));
		auto bad = runReceiver(
		    conflicted, {makeRtpPacket(600, 34000, false, 0, false, true, true, 40, 41, l0),
		                 makeRtpPacket(601, 34000, true, 1, true, true, true, 40, 42, l1)});
		TASK_CHECK(bad.empty());
	}

	// Truncated descriptor (no room for the TL0PICIDX byte) is ignored
	{
		VP9RtpDepacketizer truncated;
		truncated.setSpatialLayerLimit(uint8_t(0));
		auto packet = makeRtpPacket(700, 35000, true, 0, false, true, true, 50, 51, l0);
		// Shrink the payload to just the first descriptor byte
		packet->resize(12 + 1);
		auto out = runReceiver(truncated, {packet});
		TASK_CHECK(out.empty());
		// A valid picture still goes through afterwards
		auto after = runReceiver(
		    truncated, {makeRtpPacket(701, 36000, true, 0, false, true, true, 51, 52, l0)});
		TASK_CHECK(after.size() == 1 && *after[0] == l0);
	}
	return 0;
}

static int continuity_end_to_end_no_duplicates() {
	auto cfg = makeConfig(60000); // will wrap during the test
	VP9RtpPacketizer packetizer(cfg, 9); // 5-byte descriptor leaves 4 data bytes
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1)); // delivers before the SID 2 marker

	// Picture 1: three layers, the largest split over two fragments
	auto fi1 = makeFrameInfo(50000, {5, 4, 6}, true, true);
	const auto frame1 = makeFrame({pattern(5, 0x01), pattern(4, 0x11), pattern(6, 0x21)});
	std::vector<message_ptr> packets;
	{
		message_vector messages{makeInput(frame1, fi1)};
		packetizer.outgoingChain(messages, [](message_ptr) {});
		for (auto &message : messages)
			packets.push_back(message);
	}
	// 5-byte layer 0 -> 2 packets, 4-byte layer 1 -> 1 packet, 6-byte layer 2 -> 2 packets
	TASK_CHECK(packets.size() == 5);
	TASK_CHECK(reinterpret_cast<const RtpHeader *>(packets.back()->data())->marker());

	// Feed the retained layers (through the end of SID 1). Delivery happens
	// without waiting for the SID 2 marker.
	message_vector early;
	for (size_t i = 0; i < 3; ++i)
		early.push_back(packets[i]);
	depacketizer.incomingChain(early, [](message_ptr) {});
	TASK_CHECK(early.size() == 1);
	TASK_CHECK(early[0]->frameInfo->timestamp == 50000);

	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*early[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2 && sizes[0] == 5 && sizes[1] == 4);

	// Now feed the higher-layer tail (its E and marker), then the next picture.
	// Neither may produce a duplicate delivery, and the limit stays at 1.
	message_vector tail;
	for (size_t i = 3; i < packets.size(); ++i)
		tail.push_back(packets[i]);
	depacketizer.incomingChain(tail, [](message_ptr) {});
	TASK_CHECK(tail.empty());

	auto fi2 = makeFrameInfo(51000, {4, 4, 4}, true, true);
	const auto frame2 = makeFrame({pattern(4, 0x41), pattern(4, 0x51), pattern(4, 0x61)});
	message_vector next;
	{
		message_vector messages{makeInput(frame2, fi2)};
		packetizer.outgoingChain(messages, [](message_ptr) {});
		for (auto &message : messages)
			next.push_back(message);
	}
	depacketizer.incomingChain(next, [](message_ptr) {});
	TASK_CHECK(next.size() == 1);
	TASK_CHECK(next[0]->frameInfo->timestamp == 51000);

	TASK_CHECK(parseSuperframeIndex(*next[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2 && sizes[0] == 4 && sizes[1] == 4);
	TASK_CHECK((*next[0])[0] == byte(0x41) && (*next[0])[4] == byte(0x51));
	return 0;
}

static int continuity_control_messages_pass_through() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1));
	auto control = make_message(binary{byte(0x01), byte(0x02)}, Message::Control);
	auto out = runReceiver(depacketizer, {control});
	TASK_CHECK(out.size() == 1);
	TASK_CHECK(out[0]->type == Message::Control);
	TASK_CHECK(*out[0] == binary({byte(0x01), byte(0x02)}));
	return 0;
}

static int layers_superframe_width() {
	// The index width follows the largest layer; 300 bytes needs two bytes per size
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1));
	auto l0 = pattern(300, 0x10);
	auto l1 = pattern(7, 0x20);
	auto out = runReceiver(
	    depacketizer, {makeRtpPacket(50, 6000, false, 0, false, true, true, 4, 5, l0),
	                   makeRtpPacket(51, 6000, true, 1, true, true, true, 4, 5, l1)});
	TASK_CHECK(out.size() == 1);

	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*out[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(bytesPerSize == 2);
	TASK_CHECK(numFrames == 2);
	TASK_CHECK(sizes[0] == 300 && sizes[1] == 7);
	TASK_CHECK(out[0]->size() == 300 + 7 + 1 + 2 * 2 + 1);
	// Mask bits and little-endian sizes
	TASK_CHECK((std::to_integer<uint8_t>(out[0]->back()) & 0xE0) == 0xC0);
	return 0;
}

static int layers_extensions_and_padding_excluded() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(0));
	auto l0 = pattern(4, 0x60);
	auto packet = makeRtpPacket(60, 6100, true, 0, false, true, true, 6, 7, l0);
	addExtensionAndPadding(packet);

	size_t offset = 0, size = 0;
	TASK_CHECK(payloadOf(packet, offset, size) == 0xAC); // I=1, L=1, B=1, E=1
	auto out = runReceiver(depacketizer, {packet});
	TASK_CHECK(out.size() == 1);
	// The extension bytes and padding are not part of the delivered frame
	TASK_CHECK(*out[0] == l0);
	TASK_CHECK(out[0]->frameInfo->timestamp == 6100);
	TASK_CHECK(out[0]->frameInfo->payloadType == kPayloadType);
	return 0;
}

static int continuity_new_picture_discards_incomplete() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1));
	auto l0 = pattern(3, 0x71);
	auto l1 = pattern(2, 0x72);

	// Picture A: SID 0 complete, SID 1 missing
	auto partial = runReceiver(
	    depacketizer, {makeRtpPacket(70, 40000, false, 0, false, true, true, 8, 9, l0)});
	TASK_CHECK(partial.empty());

	// Picture B begins; A is dropped without producing a frame
	auto next = runReceiver(
	    depacketizer, {makeRtpPacket(71, 41000, false, 0, false, true, true, 9, 10, l0),
	                   makeRtpPacket(72, 41000, true, 1, true, true, true, 9, 10, l1)});
	TASK_CHECK(next.size() == 1);
	TASK_CHECK(next[0]->frameInfo->timestamp == 41000);
	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*next[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2 && sizes[0] == 3 && sizes[1] == 2);
	return 0;
}

static int early_actual_top_below_limit() {
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(2));

	// A two-layer picture is sent even though the limit is 2. Its marker is
	// on SID 1, so SID 2 never arrives: the marker must end the picture as
	// soon as SID 0 and SID 1 are complete, without waiting for SID 2.
	auto l0 = pattern(3, 0x11);
	auto l1 = pattern(5, 0x22);

	std::vector<message_ptr> packets{
	    makeRtpPacket(80, 42000, false, 1, true, true, false, 9, 10, pattern(3, 0x22)),
	    makeRtpPacket(81, 42000, true, 1, true, false, true, 9, 10, pattern(2, 0x25)),
	};
	// SID 0 missing: nothing is delivered yet
	TASK_CHECK(runReceiver(depacketizer, packets).empty());

	auto out = runReceiver(
	    depacketizer, {makeRtpPacket(82, 42000, false, 0, false, true, true, 9, 10, l0)});
	TASK_CHECK(out.size() == 1);
	TASK_CHECK(out[0]->frameInfo->timestamp == 42000);

	size_t bytesPerSize = 0, numFrames = 0;
	std::vector<size_t> sizes;
	TASK_CHECK(parseSuperframeIndex(*out[0], bytesPerSize, numFrames, sizes));
	TASK_CHECK(numFrames == 2);
	TASK_CHECK(bytesPerSize == 1); // largest layer is 5 bytes
	TASK_CHECK(sizes[0] == 3 && sizes[1] == 5);
	return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

#define CASE(group, name, fn) {group, name, fn}

const struct task_case task_cases[] = {
    CASE("early", "sender_descriptor_and_bounds", sender_descriptor_and_bounds),
    CASE("early", "sender_packet_boundaries", sender_packet_boundaries),
    CASE("early", "sender_invalid_layers_recovery", sender_invalid_layers_recovery),
    CASE("early", "sender_counter_wraparound", sender_counter_wraparound),
    CASE("early", "sender_legacy_compatibility", sender_legacy_compatibility),
    CASE("layers", "receiver_two_and_three_layers", receiver_two_and_three_layers),
    CASE("layers", "receiver_limit_validation", receiver_limit_validation),
    CASE("layers", "layers_superframe_width", layers_superframe_width),
    CASE("layers", "layers_extensions_and_padding_excluded", layers_extensions_and_padding_excluded),
    CASE("layers", "continuity_limit_change_between_pictures",
         continuity_limit_change_between_pictures),
    CASE("continuity", "continuity_new_picture_discards_incomplete",
         continuity_new_picture_discards_incomplete),
    CASE("early", "early_actual_top_below_limit", early_actual_top_below_limit),
    CASE("continuity", "receiver_early_output_without_marker", receiver_early_output_without_marker),
    CASE("continuity", "continuity_out_of_order_across_zero", continuity_out_of_order_across_zero),
    CASE("continuity", "continuity_descriptor_conflicts", continuity_descriptor_conflicts),
    CASE("continuity", "continuity_end_to_end_no_duplicates", continuity_end_to_end_no_duplicates),
    CASE("continuity", "continuity_control_messages_pass_through",
         continuity_control_messages_pass_through),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
