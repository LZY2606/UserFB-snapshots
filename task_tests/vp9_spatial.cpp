/**
 * Regression tests for the VP9 spatial layer path.
 *
 * Covers the sender (payload descriptors, fragmentation bounds, picture
 * counters, recovery after a rejected frame) and the receiver (early delivery
 * without a higher-layer marker, superframe index construction, changing the
 * limit across calls, out-of-order and 16-bit sequence wrap).
 */

#include "../task_support/check.h"

#include "message.hpp"
#include "rtp.hpp"
#include "rtppacketizationconfig.hpp"
#include "vp9rtpdepacketizer.hpp"
#include "vp9rtppacketizer.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

using namespace rtc;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

binary makeFrame(std::initializer_list<uint8_t> bytes) {
	binary b;
	for (uint8_t v : bytes)
		b.push_back(byte(v));
	return b;
}

binary bytesOf(const message_ptr &m) { return *m; }

// A frame filled with a repeating tag so layers are easy to tell apart.
binary layerFrame(uint8_t tag, size_t size) {
	binary b(size);
	for (size_t i = 0; i < size; ++i)
		b[i] = byte(tag);
	return b;
}

struct SplitFrame {
	binary data;
	std::vector<VP9SpatialLayer> layers;

	void add(uint8_t tag, size_t size, bool p, bool d) {
		for (size_t i = 0; i < size; ++i)
			data.push_back(byte(tag));
		layers.push_back(VP9SpatialLayer{size, p, d});
	}
};

struct SentPacket {
	uint16_t seq = 0;
	uint32_t timestamp = 0;
	bool marker = false;
	binary payload; // RTP payload (descriptor + data)
};

// Packetizes one frame and records the packets the sender produced.
std::vector<SentPacket> sendFrame(VP9RtpPacketizer &packetizer, message_ptr frame) {
	message_vector out{frame};
	packetizer.outgoingChain(out, [](message_ptr) {});
	std::vector<SentPacket> result;
	for (const auto &m : out) {
		if (m->type == Message::Control) {
			SentPacket p;
			p.marker = false;
			p.seq = 0;
			p.payload = *m;
			result.push_back(p);
			continue;
		}
		auto *h = reinterpret_cast<const RtpHeader *>(m->data());
		size_t headerSize = h->getSize() + h->getExtensionHeaderSize();
		SentPacket p;
		p.seq = h->seqNumber();
		p.timestamp = h->timestamp();
		p.marker = h->marker() != 0;
		p.payload.assign(m->begin() + headerSize, m->end());
		result.push_back(p);
	}
	return result;
}

struct Descriptor {
	bool valid = false;
	bool i = false;
	bool p = false;
	bool l = false;
	bool f = false;
	bool b = false;
	bool e = false;
	uint16_t pictureId = 0;
	uint8_t tid = 0;
	uint8_t sid = 0;
	bool d = false;
	uint8_t tl0PicIdx = 0;
	size_t headerSize = 0;
	binary data;
};

Descriptor parsePayload(const binary &payload) {
	Descriptor d;
	if (payload.size() < 1)
		return d;
	uint8_t first = std::to_integer<uint8_t>(payload[0]);
	d.i = (first & 0x80) != 0;
	d.p = (first & 0x40) != 0;
	d.l = (first & 0x20) != 0;
	d.f = (first & 0x10) != 0;
	d.b = (first & 0x08) != 0;
	d.e = (first & 0x04) != 0;
	size_t offset = 1;
	if (d.i) {
		if (payload.size() < offset + 1)
			return d;
		uint8_t hi = std::to_integer<uint8_t>(payload[offset++]);
		if (hi & 0x80) {
			if (payload.size() < offset + 1)
				return d;
			d.pictureId = uint16_t((hi & 0x7F) << 8) | std::to_integer<uint8_t>(payload[offset++]);
		} else {
			d.pictureId = hi & 0x7F;
		}
	}
	if (d.l) {
		if (payload.size() < offset + 1)
			return d;
		uint8_t l = std::to_integer<uint8_t>(payload[offset++]);
		d.tid = (l >> 6) & 0x03;
		d.sid = (l >> 1) & 0x07;
		d.d = (l & 0x01) != 0;
		if (!d.f) {
			if (payload.size() < offset + 1)
				return d;
			d.tl0PicIdx = std::to_integer<uint8_t>(payload[offset++]);
		}
	}
	if (payload.size() < offset)
		return d;
	d.valid = true;
	d.headerSize = offset;
	d.data.assign(payload.begin() + offset, payload.end());
	return d;
}

// A hand-built RTP packet, independent of the production packetizer.
message_ptr rtpPacket(uint16_t seq, uint32_t timestamp, bool marker, const binary &payload,
                      uint8_t payloadType = 98) {
	message_ptr m = make_message(12 + payload.size());
	auto *h = reinterpret_cast<RtpHeader *>(m->data());
	std::memset(m->data(), 0, m->size());
	h->preparePacket();
	h->setSeqNumber(seq);
	h->setTimestamp(timestamp);
	h->setSsrc(123);
	h->setPayloadType(payloadType);
	h->setMarker(marker);
	std::memcpy(m->data() + 12, payload.data(), payload.size());
	return m;
}

// Builds a non-flexible VP9 payload descriptor + data.
binary vp9Payload(uint16_t pictureId, uint8_t sid, bool p, bool d, bool b, bool e,
                  const binary &data) {
	binary pld;
	pld.push_back(byte(0x80 | 0x20 | (p ? 0x40 : 0) | (b ? 0x08 : 0) | (e ? 0x04 : 0)));
	pld.push_back(byte(0x80 | ((pictureId >> 8) & 0x7F)));
	pld.push_back(byte(pictureId & 0xFF));
	pld.push_back(byte((sid << 1) | (d ? 1 : 0)));
	pld.push_back(byte(0)); // TL0PICIDX
	pld.insert(pld.end(), data.begin(), data.end());
	return pld;
}

void feed(VP9RtpDepacketizer &dep, const SentPacket &p, message_vector &out) {
	message_vector in{rtpPacket(p.seq, p.timestamp, p.marker, p.payload)};
	dep.incomingChain(in, [](message_ptr) {});
	for (auto &m : in)
		out.push_back(std::move(m));
}

message_ptr feedOne(VP9RtpDepacketizer &dep, const SentPacket &p) {
	message_vector in{rtpPacket(p.seq, p.timestamp, p.marker, p.payload)};
	dep.incomingChain(in, [](message_ptr) {});
	if (in.empty())
		return nullptr;
	return in.front();
}

// Reads a VP9 superframe index from the tail of a frame.
struct Superframe {
	bool valid = false;
	size_t width = 0;
	std::vector<size_t> sizes;
	size_t indexSize = 0;
};

Superframe readSuperframeIndex(const binary &frame) {
	Superframe sf;
	if (frame.empty())
		return sf;
	uint8_t marker = std::to_integer<uint8_t>(frame.back());
	if ((marker & 0xE0) != 0xC0)
		return sf;
	size_t frames = (marker & 0x07) + 1;
	size_t width = ((marker >> 3) & 0x03) + 1;
	size_t indexSize = 2 + frames * width;
	if (frame.size() < indexSize)
		return sf;
	size_t start = frame.size() - indexSize;
	if (std::to_integer<uint8_t>(frame[start]) != marker)
		return sf;
	size_t offset = start + 1;
	for (size_t i = 0; i < frames; ++i) {
		size_t size = 0;
		for (size_t j = 0; j < width; ++j)
			size |= size_t(std::to_integer<uint8_t>(frame[offset + j])) << (8 * j);
		offset += width;
		sf.sizes.push_back(size);
	}
	sf.valid = true;
	sf.width = width;
	sf.indexSize = indexSize;
	return sf;
}

// ---------------------------------------------------------------------------
// Sender tests
// ---------------------------------------------------------------------------

SplitFrame makeThreeLayerFrame() {
	SplitFrame f;
	f.add(0x11, 2, false, false); // SID 0, key
	f.add(0x22, 3, true, true);   // SID 1, depends on lower
	f.add(0x33, 4, true, true);   // SID 2, depends on lower
	return f;
}

int testSenderDescriptors() {
	auto cfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
	cfg->sequenceNumber = 100;
	cfg->timestamp = 0;
	VP9RtpPacketizer packetizer(cfg, 64);

	auto f = makeThreeLayerFrame();
	auto fi = std::make_shared<FrameInfo>(uint32_t(5000));
	fi->vp9SpatialLayers = f.layers;
	auto packets = sendFrame(packetizer, make_message(binary(f.data), fi));

	TASK_CHECK(packets.size() == 3);

	// Every packet carries a non-flexible descriptor with I, L, 15-bit
	// PictureID, TID 0, and the per-layer P/D bits.
	for (size_t i = 0; i < packets.size(); ++i) {
		auto d = parsePayload(packets[i].payload);
		TASK_CHECK(d.valid);
		TASK_CHECK(d.i);
		TASK_CHECK(d.l);
		TASK_CHECK(!d.f);
		TASK_CHECK(d.tid == 0);
		TASK_CHECK(d.sid == i);
		TASK_CHECK(d.pictureId == 0);
		TASK_CHECK(d.tl0PicIdx == 0);
		TASK_CHECK(d.p == (i != 0));
		TASK_CHECK(d.d == (i != 0));
		TASK_CHECK(d.b);
		TASK_CHECK(d.e);
		TASK_CHECK(d.data == layerFrame(uint8_t(0x11 + 0x11 * i), 2 + i));
		TASK_CHECK(packets[i].timestamp == 5000);
		TASK_CHECK(packets[i].marker == (i + 1 == packets.size()));
	}

	// Sequence numbers advance in order.
	TASK_CHECK(packets[0].seq == 100);
	TASK_CHECK(packets[1].seq == 101);
	TASK_CHECK(packets[2].seq == 102);

	return 0;
}

int testSenderCountersAndFragmentBounds() {
	auto cfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
	cfg->sequenceNumber = 7;
	cfg->timestamp = 0;
	// 5-byte descriptor + 3 data bytes per fragment.
	VP9RtpPacketizer packetizer(cfg, 8);

	SplitFrame f;
	f.add(0xAA, 7, false, false);
	f.add(0xBB, 5, true, true);
	auto fi = std::make_shared<FrameInfo>(uint32_t(100));
	fi->vp9SpatialLayers = f.layers;

	auto first = sendFrame(packetizer, make_message(binary(f.data), fi));

	// SID 0: 7 bytes -> 3 fragments (3+3+1). SID 1: 5 bytes -> 2 fragments.
	TASK_CHECK(first.size() == 5);
	for (const auto &p : first)
		TASK_CHECK(p.payload.size() <= 8);

	auto s0 = parsePayload(first[0].payload);
	auto s1 = parsePayload(first[1].payload);
	auto s2 = parsePayload(first[2].payload);
	auto s3 = parsePayload(first[3].payload);
	auto s4 = parsePayload(first[4].payload);
	TASK_CHECK(s0.sid == 0 && s0.b && !s0.e);
	TASK_CHECK(s1.sid == 0 && !s1.b && !s1.e);
	TASK_CHECK(s2.sid == 0 && !s2.b && s2.e);
	TASK_CHECK(s3.sid == 1 && s3.b && !s3.e);
	TASK_CHECK(s4.sid == 1 && !s4.b && s4.e);
	TASK_CHECK(s0.pictureId == 0);
	TASK_CHECK(s3.pictureId == 0);

	// Fragments never cross a layer boundary.
	TASK_CHECK(first[0].payload.size() == 8);
	TASK_CHECK(first[1].payload.size() == 8);
	TASK_CHECK(first[2].payload.size() == 6);
	TASK_CHECK(first[3].payload.size() == 8);
	TASK_CHECK(first[4].payload.size() == 7);

	// Only the last packet of the highest layer carries the marker.
	for (size_t i = 0; i + 1 < first.size(); ++i)
		TASK_CHECK(!first[i].marker);
	TASK_CHECK(first.back().marker);

	// The picture counted once: the next picture starts at PictureID 1, TL0 1.
	SplitFrame g;
	g.add(0xCC, 1, false, false);
	auto gi = std::make_shared<FrameInfo>(uint32_t(200));
	gi->vp9SpatialLayers = g.layers;
	auto second = sendFrame(packetizer, make_message(binary(g.data), gi));
	TASK_CHECK(second.size() == 1);
	auto d = parsePayload(second[0].payload);
	TASK_CHECK(d.pictureId == 1);
	TASK_CHECK(d.tl0PicIdx == 1);
	TASK_CHECK(second[0].seq == first.back().seq + 1);
	TASK_CHECK(second[0].timestamp == 200);

	return 0;
}

int testSenderInvalidDescriptions() {
	auto cfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
	cfg->sequenceNumber = 20;
	cfg->timestamp = 0;
	VP9RtpPacketizer packetizer(cfg, 64);

	auto expectThrow = [&](const std::vector<VP9SpatialLayer> &layers, const binary &data) {
		auto fi = std::make_shared<FrameInfo>(uint32_t(300));
		fi->vp9SpatialLayers = layers;
		message_vector out{make_message(binary(data), fi)};
		bool threw = false;
		try {
			packetizer.outgoingChain(out, [](message_ptr) {});
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		return threw && cfg->sequenceNumber == 20;
	};

	// Zero size.
	TASK_CHECK(expectThrow({VP9SpatialLayer{0, false, false}}, makeFrame({0x82})));
	// SID 0 declaring inter-layer dependency.
	TASK_CHECK(expectThrow({VP9SpatialLayer{1, false, true}}, makeFrame({0x82})));
	// Sizes not matching the frame.
	TASK_CHECK(expectThrow({VP9SpatialLayer{1, false, false}, VP9SpatialLayer{1, false, false}},
	                       makeFrame({0x82, 0x11, 0x22})));
	// More than eight layers.
	{
		std::vector<VP9SpatialLayer> many(9, VP9SpatialLayer{1, false, false});
		TASK_CHECK(expectThrow(many, makeFrame({0x82, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		                                       0x88})));
	}
	// Not enough room for the descriptor and one data byte (5 + 1 = 6).
	{
		auto smallCfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
		smallCfg->sequenceNumber = 20;
		VP9RtpPacketizer small(smallCfg, 5);
		auto fi = std::make_shared<FrameInfo>(uint32_t(300));
		fi->vp9SpatialLayers = {VP9SpatialLayer{1, false, false}};
		message_vector out{make_message(makeFrame({0x82}), fi)};
		bool threw = false;
		try {
			small.outgoingChain(out, [](message_ptr) {});
		} catch (const std::invalid_argument &) {
			threw = true;
		}
		TASK_CHECK(threw);
		TASK_CHECK(smallCfg->sequenceNumber == 20);
	}

	// Exactly descriptor + one byte fits.
	{
		auto exactCfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
		exactCfg->sequenceNumber = 20;
		VP9RtpPacketizer exact(exactCfg, 6);
		auto fi = std::make_shared<FrameInfo>(uint32_t(300));
		fi->vp9SpatialLayers = {VP9SpatialLayer{1, false, false}};
		message_vector out{make_message(makeFrame({0x82}), fi)};
		exact.outgoingChain(out, [](message_ptr) {});
		TASK_CHECK(out.size() == 1);
		auto *h = reinterpret_cast<const RtpHeader *>(out[0]->data());
		TASK_CHECK(h->getSize() + h->getExtensionHeaderSize() + 6 == out[0]->size());
	}

	// After the rejected frames a valid frame still goes out with the counters
	// and sequence number untouched by the failures.
	SplitFrame ok;
	ok.add(0x11, 2, false, false);
	auto fi = std::make_shared<FrameInfo>(uint32_t(300));
	fi->vp9SpatialLayers = ok.layers;
	auto packets = sendFrame(packetizer, make_message(binary(ok.data), fi));
	TASK_CHECK(packets.size() == 1);
	auto d = parsePayload(packets[0].payload);
	TASK_CHECK(d.valid);
	TASK_CHECK(d.pictureId == 0);
	TASK_CHECK(d.tl0PicIdx == 0);
	TASK_CHECK(packets[0].seq == 20);

	return 0;
}

int testSenderWrapsCounters() {
	auto cfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
	cfg->sequenceNumber = 0;
	cfg->timestamp = 0;
	VP9RtpPacketizer packetizer(cfg, 64);

	SplitFrame f;
	f.add(0x11, 1, false, false);

	// Advance 0x7FFF pictures so the 15-bit PictureID wraps to 0 on the next
	// frame; TL0PICIDX advances every picture and wraps modulo 256.
	uint16_t lastPictureId = 0;
	uint8_t lastTl0 = 0;
	for (int i = 0; i < 0x7FFF; ++i) {
		auto fi = std::make_shared<FrameInfo>(uint32_t(i));
		fi->vp9SpatialLayers = f.layers;
		auto packets = sendFrame(packetizer, make_message(binary(f.data), fi));
		if (packets.empty())
			return 1;
		auto d = parsePayload(packets[0].payload);
		lastPictureId = d.pictureId;
		lastTl0 = d.tl0PicIdx;
	}
	TASK_CHECK(lastPictureId == 0x7FFE);
	TASK_CHECK(lastTl0 == uint8_t(0x7FFE & 0xFF));

	auto fi = std::make_shared<FrameInfo>(uint32_t(0x7FFF));
	fi->vp9SpatialLayers = f.layers;
	auto packets = sendFrame(packetizer, make_message(binary(f.data), fi));
	TASK_CHECK(packets.size() == 1);
	auto d = parsePayload(packets[0].payload);
	TASK_CHECK(d.pictureId == 0x7FFF);
	lastTl0 = d.tl0PicIdx;
	TASK_CHECK(lastTl0 == uint8_t(0x7FFF & 0xFF));

	auto fi2 = std::make_shared<FrameInfo>(uint32_t(0x8000));
	fi2->vp9SpatialLayers = f.layers;
	auto packets2 = sendFrame(packetizer, make_message(binary(f.data), fi2));
	TASK_CHECK(packets2.size() == 1);
	auto d2 = parsePayload(packets2[0].payload);
	TASK_CHECK(d2.pictureId == 0); // 15-bit wrap
	TASK_CHECK(d2.tl0PicIdx == uint8_t((0x7FFF + 1) & 0xFF)); // 8-bit wrap to 0

	return 0;
}

int testSenderKeepsExtensions() {
	auto cfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
	cfg->sequenceNumber = 0;
	cfg->timestamp = 0;
	cfg->midId = 3;
	cfg->mid = std::string("m");
	VP9RtpPacketizer packetizer(cfg, 64);

	SplitFrame f;
	f.add(0x11, 2, false, false);
	f.add(0x22, 2, true, true);
	auto fi = std::make_shared<FrameInfo>(uint32_t(10));
	fi->vp9SpatialLayers = f.layers;

	message_vector out{make_message(binary(f.data), fi)};
	packetizer.outgoingChain(out, [](message_ptr) {});
	TASK_CHECK(out.size() == 2);
	for (const auto &m : out) {
		auto *h = reinterpret_cast<const RtpHeader *>(m->data());
		TASK_CHECK(h->extension());
		TASK_CHECK(h->getExtensionHeaderSize() > 0);
		auto *ext = h->getExtensionHeader();
		TASK_CHECK(ext->profileSpecificId() == 0xbede);
	}

	return 0;
}

// ---------------------------------------------------------------------------
// Receiver tests
// ---------------------------------------------------------------------------

int testReceiverEarlySingleLayer() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(0);

	std::vector<SentPacket> packets;
	// SID 0 is a single complete packet without the marker; SID 1 is a higher
	// layer that must not be needed.
	binary d0 = layerFrame(0x11, 3);
	SentPacket p0;
	p0.seq = 10;
	p0.timestamp = 9000;
	p0.marker = false;
	p0.payload = vp9Payload(7, 0, false, false, true, true, d0);
	packets.push_back(p0);

	auto out = feedOne(dep, packets[0]);
	TASK_CHECK(out != nullptr);
	TASK_CHECK(bytesOf(out) == d0);
	TASK_CHECK(out->frameInfo != nullptr);
	TASK_CHECK(out->frameInfo->timestamp == 9000);
	TASK_CHECK(out->frameInfo->payloadType == 98);

	// A late higher-layer packet of the same picture is not delivered again.
	SentPacket p1;
	p1.seq = 11;
	p1.timestamp = 9000;
	p1.marker = true;
	p1.payload = vp9Payload(7, 1, true, true, true, true, layerFrame(0x22, 2));
	auto nothing = feedOne(dep, p1);
	TASK_CHECK(nothing == nullptr);

	return 0;
}

int testReceiverTwoLayerSuperframe() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(1);

	binary d0 = layerFrame(0x11, 4);
	binary d1 = layerFrame(0x22, 6);

	SentPacket p0;
	p0.seq = 100;
	p0.timestamp = 5000;
	p0.marker = false;
	p0.payload = vp9Payload(1, 0, false, false, true, true, d0);

	TASK_CHECK(feedOne(dep, p0) == nullptr);

	SentPacket p1;
	p1.seq = 101;
	p1.timestamp = 5000;
	p1.marker = true;
	p1.payload = vp9Payload(1, 1, true, true, true, true, d1);

	auto out = feedOne(dep, p1);
	TASK_CHECK(out != nullptr);
	auto data = bytesOf(out);
	TASK_CHECK(data.size() == d0.size() + d1.size() + 4); // two size bytes + 2 markers
	TASK_CHECK(std::equal(d0.begin(), d0.end(), data.begin()));
	TASK_CHECK(std::equal(d1.begin(), d1.end(), data.begin() + d0.size()));

	auto sf = readSuperframeIndex(data);
	TASK_CHECK(sf.valid);
	TASK_CHECK(sf.width == 1);
	TASK_CHECK(sf.sizes.size() == 2);
	TASK_CHECK(sf.sizes[0] == d0.size());
	TASK_CHECK(sf.sizes[1] == d1.size());
	// Marker byte: 0b110, width-1 = 0 (bits 4..3), frames-1 = 1 (bits 2..0).
	TASK_CHECK(std::to_integer<uint8_t>(data.back()) == 0xC1);

	return 0;
}

int testReceiverThreeLayerSuperframe() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(2);

	binary d0 = layerFrame(0x11, 1);
	binary d1 = layerFrame(0x22, 2);
	binary d2 = layerFrame(0x33, 3);

	SentPacket p0;
	p0.seq = 1;
	p0.timestamp = 1000;
	p0.marker = false;
	p0.payload = vp9Payload(4, 0, false, false, true, true, d0);

	SentPacket p1;
	p1.seq = 2;
	p1.timestamp = 1000;
	p1.marker = false;
	p1.payload = vp9Payload(4, 1, true, true, true, true, d1);

	SentPacket p2;
	p2.seq = 3;
	p2.timestamp = 1000;
	p2.marker = true;
	p2.payload = vp9Payload(4, 2, true, true, true, true, d2);

	TASK_CHECK(feedOne(dep, p0) == nullptr);
	TASK_CHECK(feedOne(dep, p1) == nullptr);
	auto out = feedOne(dep, p2);
	TASK_CHECK(out != nullptr);

	auto data = bytesOf(out);
	auto sf = readSuperframeIndex(data);
	TASK_CHECK(sf.valid);
	TASK_CHECK(sf.sizes.size() == 3);
	TASK_CHECK(sf.sizes[0] == 1);
	TASK_CHECK(sf.sizes[1] == 2);
	TASK_CHECK(sf.sizes[2] == 3);
	TASK_CHECK(std::to_integer<uint8_t>(data.back()) == 0xC2); // frames-1=2, width-1=0
	TASK_CHECK(data.size() == 6 + 5);

	return 0;
}

int testReceiverLimitBelowActualLayer() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(1);

	binary d0 = layerFrame(0x11, 2);
	binary d1 = layerFrame(0x22, 3);
	binary d2 = layerFrame(0x33, 4);

	SentPacket p0;
	p0.seq = 5;
	p0.timestamp = 700;
	p0.marker = false;
	p0.payload = vp9Payload(9, 0, false, false, true, true, d0);
	SentPacket p1;
	p1.seq = 6;
	p1.timestamp = 700;
	p1.marker = false;
	p1.payload = vp9Payload(9, 1, true, true, true, true, d1);
	SentPacket p2;
	p2.seq = 7;
	p2.timestamp = 700;
	p2.marker = true;
	p2.payload = vp9Payload(9, 2, true, true, true, true, d2);

	TASK_CHECK(feedOne(dep, p0) == nullptr);
	auto out = feedOne(dep, p1);
	TASK_CHECK(out != nullptr);
	auto data = bytesOf(out);
	auto sf = readSuperframeIndex(data);
	TASK_CHECK(sf.valid);
	TASK_CHECK(sf.sizes.size() == 2);
	TASK_CHECK(sf.sizes[0] == d0.size());
	TASK_CHECK(sf.sizes[1] == d1.size());

	// The higher layer's marker is not delivered again.
	TASK_CHECK(feedOne(dep, p2) == nullptr);

	return 0;
}

int testReceiverActualHighestBelowLimit() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(5);

	binary d0 = layerFrame(0x11, 2);
	binary d1 = layerFrame(0x22, 3);

	SentPacket p0;
	p0.seq = 30;
	p0.timestamp = 400;
	p0.marker = false;
	p0.payload = vp9Payload(2, 0, false, false, true, true, d0);
	SentPacket p1;
	p1.seq = 31;
	p1.timestamp = 400;
	p1.marker = true;
	p1.payload = vp9Payload(2, 1, true, true, true, true, d1);

	// Layers 0 and 1 are complete, but the marker of the actual highest layer
	// has not arrived yet, so nothing is delivered early.
	TASK_CHECK(feedOne(dep, p0) == nullptr);
	auto out = feedOne(dep, p1);
	TASK_CHECK(out != nullptr);
	auto sf = readSuperframeIndex(bytesOf(out));
	TASK_CHECK(sf.valid);
	TASK_CHECK(sf.sizes.size() == 2);

	return 0;
}

int testReceiverLimitChangeBetweenPictures() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(2);

	auto sendPicture = [&](uint16_t seqBase, uint32_t ts, uint16_t pictureId) {
		std::vector<SentPacket> packets;
		binary data[3] = {layerFrame(0x11, 1), layerFrame(0x22, 2), layerFrame(0x33, 3)};
		for (uint8_t sid = 0; sid < 3; ++sid) {
			SentPacket p;
			p.seq = uint16_t(seqBase + sid);
			p.timestamp = ts;
			p.marker = sid == 2;
			p.payload = vp9Payload(pictureId, sid, sid != 0, sid != 0, true, true, data[sid]);
			packets.push_back(p);
		}
		message_vector out;
		for (const auto &p : packets)
			feed(dep, p, out);
		return out;
	};

	// First picture uses the limit set before it.
	auto first = sendPicture(10, 1000, 1);
	TASK_CHECK(first.size() == 1);
	auto sf1 = readSuperframeIndex(bytesOf(first[0]));
	TASK_CHECK(sf1.valid && sf1.sizes.size() == 3);

	// Lower the limit; it is sampled on the first packet of the next picture.
	dep.setSpatialLayerLimit(0);
	auto second = sendPicture(20, 2000, 2);
	TASK_CHECK(second.size() == 1);
	// Single retained layer: raw frame, no superframe index.
	TASK_CHECK(bytesOf(second[0]) == layerFrame(0x11, 1));

	// Raise it again for a third picture.
	dep.setSpatialLayerLimit(1);
	auto third = sendPicture(30, 3000, 3);
	TASK_CHECK(third.size() == 1);
	auto sf3 = readSuperframeIndex(bytesOf(third[0]));
	TASK_CHECK(sf3.valid && sf3.sizes.size() == 2);

	return 0;
}

int testReceiverOutOfOrder() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(1);

	binary d0a = layerFrame(0x11, 2);
	binary d0b = layerFrame(0x12, 3);
	binary d1 = layerFrame(0x22, 2);

	SentPacket a; // B, first fragment of SID 0
	a.seq = 200;
	a.timestamp = 900;
	a.marker = false;
	a.payload = vp9Payload(3, 0, false, false, true, false, d0a);

	SentPacket b; // E, last fragment of SID 0
	b.seq = 201;
	b.timestamp = 900;
	b.marker = false;
	b.payload = vp9Payload(3, 0, false, false, false, true, d0b);

	SentPacket c; // SID 1, marker
	c.seq = 202;
	c.timestamp = 900;
	c.marker = true;
	c.payload = vp9Payload(3, 1, true, true, true, true, d1);

	// Deliver E and the higher layer before the missing B fragment: neither
	// may emit a partial frame, and the B fragment completes the picture.
	TASK_CHECK(feedOne(dep, b) == nullptr);
	TASK_CHECK(feedOne(dep, c) == nullptr);
	auto out = feedOne(dep, a);
	TASK_CHECK(out != nullptr);
	auto data = bytesOf(out);
	auto sf = readSuperframeIndex(data);
	TASK_CHECK(sf.valid);
	TASK_CHECK(sf.sizes[0] == d0a.size() + d0b.size());
	TASK_CHECK(sf.sizes[1] == d1.size());
	TASK_CHECK(std::equal(d0a.begin(), d0a.end(), data.begin()));
	TASK_CHECK(std::equal(d0b.begin(), d0b.end(), data.begin() + d0a.size()));

	// A duplicate of the completed picture is not delivered again.
	TASK_CHECK(feedOne(dep, c) == nullptr);

	return 0;
}

int testReceiverSequenceWrap() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(0);

	// Two fragments spanning the 16-bit sequence wrap.
	binary f0 = layerFrame(0xAA, 4);
	binary f1 = layerFrame(0xBB, 4);
	binary f2 = layerFrame(0xCC, 2);

	SentPacket p0;
	p0.seq = 0xFFFE;
	p0.timestamp = 1500;
	p0.marker = false;
	p0.payload = vp9Payload(11, 0, false, false, true, false, f0);
	SentPacket p1;
	p1.seq = 0xFFFF;
	p1.timestamp = 1500;
	p1.marker = false;
	p1.payload = vp9Payload(11, 0, false, false, false, false, f1);
	SentPacket p2;
	p2.seq = 0x0000;
	p2.timestamp = 1500;
	p2.marker = false;
	p2.payload = vp9Payload(11, 0, false, false, false, true, f2);

	TASK_CHECK(feedOne(dep, p0) == nullptr);
	TASK_CHECK(feedOne(dep, p2) == nullptr); // E arrives out of order
	auto out = feedOne(dep, p1);
	TASK_CHECK(out != nullptr);

	binary expected = f0;
	expected.insert(expected.end(), f1.begin(), f1.end());
	expected.insert(expected.end(), f2.begin(), f2.end());
	TASK_CHECK(bytesOf(out) == expected);

	return 0;
}

int testReceiverIncompletePictureDropped() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(0);

	// Picture 1: SID 0 B fragment, then its E fragment is lost.
	SentPacket incomplete;
	incomplete.seq = 50;
	incomplete.timestamp = 100;
	incomplete.marker = false;
	incomplete.payload = vp9Payload(1, 0, false, false, true, false, layerFrame(0x11, 2));
	TASK_CHECK(feedOne(dep, incomplete) == nullptr);

	// Picture 2 is complete and must be delivered without the missing piece.
	binary d = layerFrame(0x22, 3);
	SentPacket complete;
	complete.seq = 52;
	complete.timestamp = 200;
	complete.marker = false;
	complete.payload = vp9Payload(2, 0, false, false, true, true, d);

	auto out = feedOne(dep, complete);
	TASK_CHECK(out != nullptr);
	TASK_CHECK(bytesOf(out) == d);
	TASK_CHECK(out->frameInfo && out->frameInfo->timestamp == 200);

	return 0;
}

int testReceiverTruncatedDescriptor() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(0);

	// A payload that claims I and L but is too short for the descriptor.
	binary truncated{byte(0x80 | 0x20 | 0x08 | 0x04), byte(0x80)};
	message_vector in{rtpPacket(7, 50, true, truncated)};
	dep.incomingChain(in, [](message_ptr) {});
	TASK_CHECK(in.empty());

	// The next valid picture still works.
	binary d = layerFrame(0x33, 2);
	SentPacket good;
	good.seq = 8;
	good.timestamp = 60;
	good.marker = false;
	good.payload = vp9Payload(5, 0, false, false, true, true, d);
	auto out = feedOne(dep, good);
	TASK_CHECK(out != nullptr);
	TASK_CHECK(bytesOf(out) == d);

	return 0;
}

int testReceiverWideSuperframeSizes() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(1);

	// A layer larger than 255 bytes forces a two-byte size field, which must be
	// written little endian with the width encoded in bits 4..3.
	binary d0 = layerFrame(0x11, 300);
	binary d1 = layerFrame(0x22, 2);

	SentPacket p0;
	p0.seq = 400;
	p0.timestamp = 8000;
	p0.marker = false;
	p0.payload = vp9Payload(12, 0, false, false, true, true, d0);
	SentPacket p1;
	p1.seq = 401;
	p1.timestamp = 8000;
	p1.marker = true;
	p1.payload = vp9Payload(12, 1, true, true, true, true, d1);

	TASK_CHECK(feedOne(dep, p0) == nullptr);
	auto out = feedOne(dep, p1);
	TASK_CHECK(out != nullptr);

	auto data = bytesOf(out);
	auto sf = readSuperframeIndex(data);
	TASK_CHECK(sf.valid);
	TASK_CHECK(sf.width == 2);
	TASK_CHECK(sf.sizes.size() == 2);
	TASK_CHECK(sf.sizes[0] == 300);
	TASK_CHECK(sf.sizes[1] == 2);

	// Marker: 0b110, width-1 = 1 (bits 4..3), frames-1 = 1 (bits 2..0).
	TASK_CHECK(std::to_integer<uint8_t>(data.back()) == 0xC9);
	// The 300 is stored little endian as 0x2C 0x01.
	size_t start = data.size() - sf.indexSize;
	TASK_CHECK(std::to_integer<uint8_t>(data[start + 1]) == 0x2C);
	TASK_CHECK(std::to_integer<uint8_t>(data[start + 2]) == 0x01);
	TASK_CHECK(std::to_integer<uint8_t>(data[start + 3]) == 0x02);
	TASK_CHECK(std::to_integer<uint8_t>(data[start + 4]) == 0x00);

	return 0;
}

int testReceiverContradictoryIdentity() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(0);

	// Two fragments of SID 0 disagree on the P flag: the descriptor identity is
	// self-contradictory, so no partial frame is emitted.
	SentPacket a;
	a.seq = 60;
	a.timestamp = 20;
	a.marker = false;
	a.payload = vp9Payload(1, 0, false, false, true, false, layerFrame(0x11, 2));
	SentPacket b;
	b.seq = 61;
	b.timestamp = 20;
	b.marker = false;
	b.payload = vp9Payload(1, 0, true, false, false, true, layerFrame(0x22, 2));

	TASK_CHECK(feedOne(dep, a) == nullptr);
	TASK_CHECK(feedOne(dep, b) == nullptr);

	// A new picture with a consistent descriptor works again.
	binary d = layerFrame(0x33, 3);
	SentPacket good;
	good.seq = 62;
	good.timestamp = 30;
	good.marker = false;
	good.payload = vp9Payload(2, 0, false, false, true, true, d);
	auto out = feedOne(dep, good);
	TASK_CHECK(out != nullptr);
	TASK_CHECK(bytesOf(out) == d);

	return 0;
}

int testReceiverNewPictureIdSameTimestamp() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(0);

	// Picture 1 is delivered early from SID 0 without a marker.
	binary d1 = layerFrame(0x11, 2);
	SentPacket p1;
	p1.seq = 70;
	p1.timestamp = 55;
	p1.marker = false;
	p1.payload = vp9Payload(1, 0, false, false, true, true, d1);
	auto out1 = feedOne(dep, p1);
	TASK_CHECK(out1 != nullptr);
	TASK_CHECK(bytesOf(out1) == d1);

	// A higher layer of picture 1 arriving later is dropped.
	SentPacket late;
	late.seq = 71;
	late.timestamp = 55;
	late.marker = true;
	late.payload = vp9Payload(1, 1, true, true, true, true, layerFrame(0x22, 2));
	TASK_CHECK(feedOne(dep, late) == nullptr);

	// Picture 2 reuses the timestamp but has a new PictureID: it must be
	// treated as a new picture and delivered.
	binary d2 = layerFrame(0x33, 3);
	SentPacket p2;
	p2.seq = 72;
	p2.timestamp = 55;
	p2.marker = false;
	p2.payload = vp9Payload(2, 0, false, false, true, true, d2);
	auto out2 = feedOne(dep, p2);
	TASK_CHECK(out2 != nullptr);
	TASK_CHECK(bytesOf(out2) == d2);

	return 0;
}

int testReceiverControlPassthrough() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(1);

	message_vector in{make_message(binary{byte(0x90), byte(0x00), byte(0x01), byte(0x02)},
	                               Message::Control)};
	dep.incomingChain(in, [](message_ptr) {});
	TASK_CHECK(in.size() == 1);
	TASK_CHECK(in[0]->type == Message::Control);

	return 0;
}

int testReceiverPaddingAndExtensionsExcluded() {
	VP9RtpDepacketizer dep;
	dep.setSpatialLayerLimit(0);

	binary d = layerFrame(0x44, 3);
	binary payload = vp9Payload(6, 0, false, false, true, true, d);

	// Build a packet with a one-byte RTP header extension and trailing padding.
	// Extension: profile 0xbede, length 1 word: one-byte header id=1 size=4.
	message_ptr m = make_message(12 + 8 + payload.size() + 3);
	std::memset(m->data(), 0, m->size());
	auto *h = reinterpret_cast<RtpHeader *>(m->data());
	h->preparePacket();
	h->setSeqNumber(1);
	h->setTimestamp(70);
	h->setSsrc(123);
	h->setPayloadType(98);
	h->setMarker(false);
	h->setExtension(true);
	auto *ext = h->getExtensionHeader();
	ext->setProfileSpecificId(0xbede);
	ext->setHeaderLength(1);
	ext->clearBody();
	ext->writeOneByteHeader(0, 1, reinterpret_cast<const byte *>("abcd"), 4);
	std::memcpy(m->data() + 20, payload.data(), payload.size());
	// Padding: last byte is the padding count.
	std::memset(m->data() + 20 + payload.size(), 0, 2);
	m->data()[m->size() - 1] = byte(3);
	h->_first |= 0x20; // set padding bit

	message_vector in{m};
	dep.incomingChain(in, [](message_ptr) {});
	TASK_CHECK(in.size() == 1);
	TASK_CHECK(bytesOf(in[0]) == d);

	return 0;
}

int testReceiverLimitValidation() {
	VP9RtpDepacketizer dep;
	bool threw = false;
	try {
		dep.setSpatialLayerLimit(8);
	} catch (const std::out_of_range &) {
		threw = true;
	}
	TASK_CHECK(threw);

	// Valid boundary values are accepted.
	dep.setSpatialLayerLimit(0);
	dep.setSpatialLayerLimit(7);
	dep.setSpatialLayerLimit(optional<uint8_t>());

	return 0;
}

// ---------------------------------------------------------------------------
// End-to-end: real sender and receiver
// ---------------------------------------------------------------------------

int testEndToEndSenderReceiver() {
	auto cfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
	cfg->sequenceNumber = 65530; // exercise 16-bit wrap across the picture
	cfg->timestamp = 0;
	VP9RtpPacketizer sender(cfg, 12);
	VP9RtpDepacketizer receiver;
	receiver.setSpatialLayerLimit(1);

	// Picture 1: three layers, fragmented. The receiver has no access to the
	// sender's packets directly, so drive the real packetizer and feed the
	// resulting RTP messages into the real depacketizer.
	SplitFrame f1;
	f1.add(0x11, 5, false, false);
	f1.add(0x22, 7, true, true);
	f1.add(0x33, 6, true, true);

	auto fi1 = std::make_shared<FrameInfo>(uint32_t(2000));
	fi1->vp9SpatialLayers = f1.layers;
	message_vector tx1{make_message(binary(f1.data), fi1)};
	sender.outgoingChain(tx1, [](message_ptr) {});
	TASK_CHECK(tx1.size() == 3);

	message_vector rx1;
	for (const auto &m : tx1) {
		message_vector in{m};
		receiver.incomingChain(in, [](message_ptr) {});
		for (auto &r : in)
			rx1.push_back(std::move(r));
	}

	// The limit is 1, so layers 0 and 1 are delivered as a superframe before
	// the highest layer's marker.
	TASK_CHECK(rx1.size() == 1);
	auto sf = readSuperframeIndex(bytesOf(rx1[0]));
	TASK_CHECK(sf.valid);
	TASK_CHECK(sf.sizes.size() == 2);
	TASK_CHECK(sf.sizes[0] == 5);
	TASK_CHECK(sf.sizes[1] == 7);
	TASK_CHECK(rx1[0]->frameInfo && rx1[0]->frameInfo->timestamp == 2000);

	// Picture 2: a single layer (below the limit) is delivered on its marker.
	SplitFrame f2;
	f2.add(0x44, 4, false, false);
	auto fi2 = std::make_shared<FrameInfo>(uint32_t(3000));
	fi2->vp9SpatialLayers = f2.layers;
	message_vector tx2{make_message(binary(f2.data), fi2)};
	sender.outgoingChain(tx2, [](message_ptr) {});

	message_vector rx2;
	for (const auto &m : tx2) {
		message_vector in{m};
		receiver.incomingChain(in, [](message_ptr) {});
		for (auto &r : in)
			rx2.push_back(std::move(r));
	}
	TASK_CHECK(rx2.size() == 1);
	TASK_CHECK(bytesOf(rx2[0]) == layerFrame(0x44, 4));

	return 0;
}

int testEndToEndNoDuplicateAfterEarlyDelivery() {
	auto cfg = std::make_shared<RtpPacketizationConfig>(123, "vp9", 98, 90000);
	cfg->sequenceNumber = 1000;
	cfg->timestamp = 0;
	VP9RtpPacketizer sender(cfg, 100);
	VP9RtpDepacketizer receiver;
	receiver.setSpatialLayerLimit(1);

	// Picture 1: layers 0..2, each a single packet.
	SplitFrame f1;
	f1.add(0x11, 2, false, false);
	f1.add(0x22, 3, true, true);
	f1.add(0x33, 4, true, true);
	auto fi1 = std::make_shared<FrameInfo>(uint32_t(500));
	fi1->vp9SpatialLayers = f1.layers;
	message_vector tx1{make_message(binary(f1.data), fi1)};
	sender.outgoingChain(tx1, [](message_ptr) {});
	TASK_CHECK(tx1.size() == 3);

	// Feed the low layers first: the receiver delivers the superframe before the
	// highest layer's marker arrives.
	message_vector rx;
	{
		message_vector in{tx1[0], tx1[1]};
		receiver.incomingChain(in, [](message_ptr) {});
		TASK_CHECK(in.size() == 1);
		auto sf = readSuperframeIndex(bytesOf(in[0]));
		TASK_CHECK(sf.valid && sf.sizes.size() == 2);
	}

	// Now feed the highest layer's tail packet: it must not be delivered again.
	{
		message_vector in{tx1[2]};
		receiver.incomingChain(in, [](message_ptr) {});
		TASK_CHECK(in.empty());
	}

	// Feed a duplicate of a low-layer packet: still nothing.
	{
		message_vector in{tx1[0]};
		receiver.incomingChain(in, [](message_ptr) {});
		TASK_CHECK(in.empty());
	}

	// The next picture delivers normally.
	SplitFrame f2;
	f2.add(0x55, 2, true, false);
	f2.add(0x66, 3, true, true);
	f2.add(0x77, 4, true, true);
	auto fi2 = std::make_shared<FrameInfo>(uint32_t(600));
	fi2->vp9SpatialLayers = f2.layers;
	message_vector tx2{make_message(binary(f2.data), fi2)};
	sender.outgoingChain(tx2, [](message_ptr) {});

	message_vector rx2;
	for (const auto &m : tx2) {
		message_vector in{m};
		receiver.incomingChain(in, [](message_ptr) {});
		for (auto &r : in)
			rx2.push_back(std::move(r));
	}
	TASK_CHECK(rx2.size() == 1);
	auto sf2 = readSuperframeIndex(bytesOf(rx2[0]));
	TASK_CHECK(sf2.valid && sf2.sizes.size() == 2);

	return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

} // namespace

const struct task_case task_cases[] = {
    {"early", "sender_descriptors", testSenderDescriptors},
    {"early", "sender_counters_and_fragments", testSenderCountersAndFragmentBounds},
    {"early", "sender_invalid_descriptions", testSenderInvalidDescriptions},
    {"early", "receiver_early_single_layer", testReceiverEarlySingleLayer},
    {"early", "receiver_limit_validation", testReceiverLimitValidation},

    {"layers", "sender_counter_wrap", testSenderWrapsCounters},
    {"layers", "sender_keeps_extensions", testSenderKeepsExtensions},
    {"layers", "receiver_two_layer_superframe", testReceiverTwoLayerSuperframe},
    {"layers", "receiver_three_layer_superframe", testReceiverThreeLayerSuperframe},
    {"layers", "receiver_limit_below_actual", testReceiverLimitBelowActualLayer},
    {"layers", "receiver_actual_highest_below_limit", testReceiverActualHighestBelowLimit},
    {"layers", "receiver_limit_change", testReceiverLimitChangeBetweenPictures},
    {"layers", "receiver_padding_and_extensions", testReceiverPaddingAndExtensionsExcluded},

    {"continuity", "receiver_out_of_order", testReceiverOutOfOrder},
    {"continuity", "receiver_sequence_wrap", testReceiverSequenceWrap},
    {"continuity", "receiver_incomplete_dropped", testReceiverIncompletePictureDropped},
    {"continuity", "receiver_truncated_descriptor", testReceiverTruncatedDescriptor},
    {"continuity", "receiver_wide_sizes", testReceiverWideSuperframeSizes},
    {"continuity", "receiver_contradictory_identity", testReceiverContradictoryIdentity},
    {"continuity", "receiver_new_picture_id_same_timestamp", testReceiverNewPictureIdSameTimestamp},
    {"continuity", "receiver_control_passthrough", testReceiverControlPassthrough},
    {"continuity", "end_to_end", testEndToEndSenderReceiver},
    {"continuity", "end_to_end_no_duplicate", testEndToEndNoDuplicateAfterEarlyDelivery},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
