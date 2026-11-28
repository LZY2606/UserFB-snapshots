/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_FRAMEINFO_H
#define RTC_FRAMEINFO_H

#include "common.hpp"

#include <chrono>

namespace rtc {

/// Description of one spatial layer of a VP9 picture.
///
/// When a frame carries spatial layers, the input bytes are the spatial
/// layer frames concatenated in increasing SID order (SID 0 first), without
/// a superframe index. Each entry describes one of those frames.
struct RTC_CPP_EXPORT VP9SpatialLayer {
	/// Size in bytes of this spatial layer's frame inside the input.
	size_t size = 0;
	/// P bit of the VP9 payload descriptor: inter-picture predicted.
	bool interPicturePredicted = false;
	/// D bit of the VP9 payload descriptor: inter-layer predicted.
	/// Must be false for SID 0.
	bool interLayerPredicted = false;
};

struct RTC_CPP_EXPORT FrameInfo {
	FrameInfo(uint32_t timestamp) : timestamp(timestamp) {};
	template<typename Period = std::ratio<1>> FrameInfo(std::chrono::duration<double, Period> timestamp) : timestampSeconds(timestamp) {}

	[[deprecated]] FrameInfo(uint8_t payloadType, uint32_t timestamp) : timestamp(timestamp), payloadType(payloadType) {};

	uint32_t timestamp = 0;
	uint8_t payloadType = 0;

	optional<std::chrono::duration<double>> timestampSeconds;

	bool isKeyFrame = false;	// Set by the application

	/// Optional spatial layer description for VP9.
	/// Empty means the frame is a single spatial layer and the legacy
	/// packetization path applies.
	std::vector<VP9SpatialLayer> vp9SpatialLayers;

	/// Absolute capture timestamp in NTP-format (high 32 bits seconds since
	/// the NTP epoch 1900-01-01 UTC; low 32 bits fractional seconds). When
	/// set alongside an RtpPacketizationConfig with absCaptureTimeId > 0,
	/// the RtpPacketizer writes the 8-byte form of the abs-capture-time RTP
	/// header extension on every packet of this frame. Receivers use this
	/// to recover the source's capture wallclock and compute glass-to-glass
	/// latency.
	/// https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/rtp-hdrext/abs-capture-time
	optional<uint64_t> absCaptureTimeNtp;
};

} // namespace rtc

#endif // RTC_FRAMEINFO_H
