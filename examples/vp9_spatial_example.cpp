/**
 * VP9 spatial layer round-trip example.
 *
 * Shows the whole path that was previously broken:
 *
 *   1. The encoder hands over one picture made of several spatial layers,
 *      concatenated in increasing SID order, described by FrameInfo.
 *   2. VP9RtpPacketizer emits a non-flexible RFC 9628 stream: one PictureID
 *      per picture, B/E on every layer, and the RTP marker only on the last
 *      packet of the highest layer.
 *   3. VP9RtpDepacketizer is limited to a lower SID, so it delivers the low
 *      layers as soon as they are complete instead of waiting for the marker.
 *
 * Build (from the repository root):
 *
 *   c++ -std=c++17 -O1 -pthread -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=0 \
 *       -DRTC_STATIC -I. -Isrc -Iinclude/rtc -Ideps/plog/include \
 *       examples/vp9_spatial_example.cpp $(find src -name '*.cpp') -o vp9_example
 */

#include "message.hpp"
#include "rtp.hpp"
#include "rtppacketizationconfig.hpp"
#include "vp9rtpdepacketizer.hpp"
#include "vp9rtppacketizer.hpp"

#include <cstring>
#include <iostream>
#include <vector>

using namespace rtc;

namespace {

binary layer(uint8_t tag, size_t size) {
	binary b(size);
	for (size_t i = 0; i < size; ++i)
		b[i] = byte(tag);
	return b;
}

// Reads the VP9 superframe index at the tail of a frame, enough to print the
// per-layer sizes.
void printFrame(const std::string &label, const binary &frame) {
	if (frame.empty()) {
		std::cout << label << ": <empty>" << std::endl;
		return;
	}

	uint8_t marker = std::to_integer<uint8_t>(frame.back());
	if ((marker & 0xE0) != 0xC0) {
		std::cout << label << ": raw frame, " << frame.size() << " bytes" << std::endl;
		return;
	}

	size_t frames = (marker & 0x07) + 1;
	size_t width = ((marker >> 3) & 0x03) + 1;
	size_t indexSize = 2 + frames * width;
	if (frame.size() < indexSize) {
		std::cout << label << ": malformed superframe index" << std::endl;
		return;
	}

	size_t offset = frame.size() - indexSize + 1;
	std::cout << label << ": superframe of " << frames << " layers (";
	for (size_t i = 0; i < frames; ++i) {
		size_t size = 0;
		for (size_t j = 0; j < width; ++j)
			size |= size_t(std::to_integer<uint8_t>(frame[offset + j])) << (8 * j);
		offset += width;
		if (i)
			std::cout << ", ";
		std::cout << size;
	}
	std::cout << " bytes), total " << frame.size() << " bytes" << std::endl;
}

} // namespace

int main() {
	// --- Sender -----------------------------------------------------------
	auto config = std::make_shared<RtpPacketizationConfig>(/*ssrc*/ 1234, /*cname*/ "vp9-example",
	                                                       /*payloadType*/ 98, /*clockRate*/ 90000);
	config->sequenceNumber = 100;
	config->timestamp = 0;

	VP9RtpPacketizer packetizer(config, /*maxFragmentSize*/ 1200);

	// One picture: three spatial layers concatenated in SID order.
	binary pictureData;
	std::vector<VP9SpatialLayer> layers;
	auto addLayer = [&](uint8_t tag, size_t size, bool interPicture, bool interLayer) {
		auto data = layer(tag, size);
		pictureData.insert(pictureData.end(), data.begin(), data.end());
		layers.push_back(VP9SpatialLayer{size, interPicture, interLayer});
	};
	addLayer(0x11, 12, /*interPicturePredicted=*/false, /*interLayerPredicted=*/false); // SID 0
	addLayer(0x22, 20, true, true);                                                     // SID 1
	addLayer(0x33, 16, true, true);                                                     // SID 2

	auto frameInfo = std::make_shared<FrameInfo>(uint32_t(9000));
	frameInfo->vp9SpatialLayers = layers;

	message_vector outgoing{make_message(binary(pictureData), frameInfo)};
	packetizer.outgoingChain(outgoing, [](message_ptr) {});
	std::cout << "Sender produced " << outgoing.size() << " RTP packets" << std::endl;
	for (size_t i = 0; i < outgoing.size(); ++i) {
		auto *h = reinterpret_cast<const RtpHeader *>(outgoing[i]->data());
		std::cout << "  packet " << i + 1 << ": seq " << h->seqNumber() << ", timestamp "
		          << h->timestamp() << (h->marker() ? ", marker" : "") << std::endl;
	}

	// --- Receiver ---------------------------------------------------------
	// Ask for SID 0 and 1 only: the receiver should deliver a two-layer
	// superframe as soon as both are complete, without the SID 2 marker.
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(1);

	message_vector received;
	for (size_t i = 0; i < outgoing.size(); ++i) {
		message_vector in{outgoing[i]};
		depacketizer.incomingChain(in, [](message_ptr) {});
		for (auto &m : in) {
			if (m->type == Message::Control)
				continue;
			std::cout << "  after packet " << i + 1 << ": delivered " << m->size()
			          << " bytes (timestamp " << m->frameInfo->timestamp << ")" << std::endl;
			received.push_back(std::move(m));
		}
	}

	if (received.empty()) {
		std::cerr << "Receiver delivered nothing" << std::endl;
		return 1;
	}

	printFrame("Delivered", *received.front());

	// The decoder would now feed the superframe to its VP9 decoder, or read the
	// index and decode only the layers it wants.
	bool hasLowLayers =
	    received.front()->size() > 12 + 20; // the two retained layers plus the index
	if (!hasLowLayers) {
		std::cerr << "Delivered frame does not contain the retained layers" << std::endl;
		return 1;
	}

	std::cout << "OK" << std::endl;
	return 0;
}
