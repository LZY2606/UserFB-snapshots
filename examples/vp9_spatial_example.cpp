// VP9 spatial layer usage example.
//
// Build and run from the repository root:
//   bash task_support/run.sh example
//
// The example shows the full chain:
//   1. The encoder reports one picture with three spatial layers (SID 0, 1, 2)
//      through FrameInfo::vp9SpatialLayers. The input bytes are the three
//      layer frames concatenated in SID order, without a superframe index.
//   2. VP9RtpPacketizer fragments the picture in non-flexible mode: each layer
//      gets its own VP9 payload descriptor, and the RTP marker is only set on
//      the last packet of the highest layer.
//   3. VP9RtpDepacketizer is configured to retain SID 0 and 1. It delivers a
//      legal VP9 superframe as soon as the retained layers are complete,
//      without waiting for the SID 2 marker.
//   4. Remaining packets of the same picture (the SID 2 tail and marker) are
//      not delivered a second time.

#include "vp9rtpdepacketizer.hpp"
#include "vp9rtppacketizer.hpp"

#include <cstring>
#include <iostream>

using namespace rtc;

namespace {

binary pattern(size_t size, uint8_t start) {
	binary out(size);
	for (size_t i = 0; i < size; ++i)
		out[i] = byte(uint8_t(start + i));
	return out;
}

void printFrameInfo(const char *label, const message_ptr &frame) {
	const auto &info = frame->frameInfo;
	std::cout << label << ": size=" << frame->size() << " timestamp=" << info->timestamp
	          << " payloadType=" << int(info->payloadType) << " prefix=";
	const size_t shown = std::min<size_t>(frame->size(), 8);
	for (size_t i = 0; i < shown; ++i)
		std::cout << std::hex << int(std::to_integer<uint8_t>((*frame)[i])) << ' ';
	std::cout << std::dec << "...\n";
}

} // namespace

int main() {
	// RTP configuration shared by the sender
	auto config = std::make_shared<RtpPacketizationConfig>(/*ssrc=*/1001, "vp9-spatial-example",
	                                                       /*payloadType=*/98, 90000);
	config->sequenceNumber = 100;

	// Max 1200 bytes per RTP payload, descriptor included
	VP9RtpPacketizer packetizer(config, 1200);
	VP9RtpDepacketizer depacketizer;
	depacketizer.setSpatialLayerLimit(uint8_t(1)); // retain SID 0 and SID 1

	// One picture made of three spatial layers (concatenated without index)
	const binary layer0 = pattern(900, 0x10);
	const binary layer1 = pattern(600, 0x20);
	const binary layer2 = pattern(1500, 0x30);

	binary picture;
	picture.insert(picture.end(), layer0.begin(), layer0.end());
	picture.insert(picture.end(), layer1.begin(), layer1.end());
	picture.insert(picture.end(), layer2.begin(), layer2.end());

	auto frameInfo = std::make_shared<FrameInfo>(uint32_t(90000)); // 1 second
	frameInfo->vp9SpatialLayers = {
	    VP9SpatialLayer{layer0.size(), /*interPicturePredicted=*/false, /*interLayerPredicted=*/false},
	    VP9SpatialLayer{layer1.size(), /*interPicturePredicted=*/false, /*interLayerPredicted=*/true},
	    VP9SpatialLayer{layer2.size(), /*interPicturePredicted=*/false, /*interLayerPredicted=*/true},
	};

	// --- Sender side -------------------------------------------------------
	message_vector outgoing{make_message(std::move(picture), frameInfo)};
	packetizer.outgoingChain(outgoing, [](message_ptr) {});
	std::cout << "Sender produced " << outgoing.size() << " RTP packets\n";
	for (size_t i = 0; i < outgoing.size(); ++i) {
		const auto *header = reinterpret_cast<const RtpHeader *>(outgoing[i]->data());
		std::cout << "  packet " << i << ": seq=" << header->seqNumber()
		          << " marker=" << (header->marker() ? "yes" : "no")
		          << " timestamp=" << header->timestamp() << '\n';
	}

	// --- Receiver side -----------------------------------------------------
	// Feed the packets that make up SID 0 and SID 1 (the first four packets).
	message_vector firstBatch;
	for (size_t i = 0; i < 4 && i < outgoing.size(); ++i)
		firstBatch.push_back(outgoing[i]);
	depacketizer.incomingChain(firstBatch, [](message_ptr) {});

	if (firstBatch.size() != 1) {
		std::cerr << "Expected an early delivery after the retained layers\n";
		return 1;
	}
	printFrameInfo("Early delivery (no marker needed)", firstBatch.front());

	// Feed the rest of the picture: the SID 2 tail including its marker.
	message_vector secondBatch;
	for (size_t i = 4; i < outgoing.size(); ++i)
		secondBatch.push_back(outgoing[i]);
	depacketizer.incomingChain(secondBatch, [](message_ptr) {});
	if (!secondBatch.empty()) {
		std::cerr << "Higher layer packets must not be delivered again\n";
		return 1;
	}
	std::cout << "Higher layer packets ignored: " << secondBatch.size() << " more deliveries\n";

	// --- Next picture ------------------------------------------------------
	auto nextInfo = std::make_shared<FrameInfo>(uint32_t(180000)); // 2 seconds
	nextInfo->vp9SpatialLayers = {
	    VP9SpatialLayer{layer0.size(), true, false},
	    VP9SpatialLayer{layer1.size(), true, true},
	    VP9SpatialLayer{layer2.size(), true, true},
	};
	binary nextPicture;
	nextPicture.insert(nextPicture.end(), layer0.begin(), layer0.end());
	nextPicture.insert(nextPicture.end(), layer1.begin(), layer1.end());
	nextPicture.insert(nextPicture.end(), layer2.begin(), layer2.end());

	message_vector nextOutgoing{make_message(std::move(nextPicture), nextInfo)};
	packetizer.outgoingChain(nextOutgoing, [](message_ptr) {});
	message_vector nextIncoming = nextOutgoing;
	depacketizer.incomingChain(nextIncoming, [](message_ptr) {});
	if (nextIncoming.size() != 1) {
		std::cerr << "Expected the next picture to be delivered\n";
		return 1;
	}
	printFrameInfo("Next picture", nextIncoming.front());

	std::cout << "VP9_SPATIAL_EXAMPLE_OK\n";
	return 0;
}
