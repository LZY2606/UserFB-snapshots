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

/// Description of one spatial layer inside a VP9 frame.
///
/// When a FrameInfo carries a non-empty vp9SpatialLayers vector, the frame data
/// is interpreted as the concatenation of the spatial layer frames in increasing
/// SID order (without a VP9 superframe index).
struct RTC_CPP_EXPORT VP9SpatialLayer {
	size_t size = 0;                     // Size of this spatial layer's frame data in bytes
	bool interPicturePredicted = false;  // P bit: predicted from a previous picture
	bool interLayerPredicted = false;    // D bit: predicted from a lower spatial layer
};

struct RTC_CPP_EXPORT FrameInfo {
	FrameInfo(uint32_t timestamp) : timestamp(timestamp) {};
	template<typename Period = std::ratio<1>> FrameInfo(std::chrono::duration<double, Period> timestamp) : timestampSeconds(timestamp) {}

	[[deprecated]] FrameInfo(uint8_t payloadType, uint32_t timestamp) : timestamp(timestamp), payloadType(payloadType) {};

	uint32_t timestamp = 0;
	uint8_t payloadType = 0;

	optional<std::chrono::duration<double>> timestampSeconds;

	bool isKeyFrame = false;	// Set by the application

	/// Absolute capture timestamp in NTP-format (high 32 bits seconds since
	/// the NTP epoch 1900-01-01 UTC; low 32 bits fractional seconds). When
	/// set alongside an RtpPacketizationConfig with absCaptureTimeId > 0,
	/// the RtpPacketizer writes the 8-byte form of the abs-capture-time RTP
	/// header extension on every packet of this frame. Receivers use this
	/// to recover the source's capture wallclock and compute glass-to-glass
	/// latency.
	/// https://webrtc.googlesource.com/src/+/refs/heads/main/docs/native-code/rtp-hdrext/abs-capture-time
	optional<uint64_t> absCaptureTimeNtp;

	/// VP9 spatial layers carried by this frame, in increasing SID order.
	/// When empty, the frame follows the legacy single-layer packetization path.
	/// When non-empty, sizes must be positive and sum to the frame data size.
	std::vector<VP9SpatialLayer> vp9SpatialLayers;
};

} // namespace rtc

#endif // RTC_FRAMEINFO_H
