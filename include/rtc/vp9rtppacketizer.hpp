/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_VP9_RTP_PACKETIZER_H
#define RTC_VP9_RTP_PACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "rtppacketizer.hpp"

namespace rtc {

/// RTP packetization for VP9
class RTC_CPP_EXPORT VP9RtpPacketizer final : public RtpPacketizer {
public:
	inline static const uint32_t ClockRate = VideoClockRate;

	/// Constructs VP9 payload packetizer with given RTP configuration.
	/// @note RTP configuration is used in packetization process which may change some configuration
	/// properties such as sequence number.
	/// @param rtpConfig RTP configuration
	/// @param maxFragmentSize maximum size of one packet payload
	VP9RtpPacketizer(shared_ptr<RtpPacketizationConfig> rtpConfig,
	                 size_t maxFragmentSize = DefaultMaxFragmentSize);

	virtual void outgoing(message_vector &messages, const message_callback &send) override;

private:
	std::vector<binary> fragment(binary frame) override;

	/// Packetizes a frame carrying multiple VP9 spatial layers.
	/// Input bytes are the layer frames concatenated in increasing SID order.
	/// Throws std::invalid_argument if the layer description is invalid.
	/// @note does not touch sequence numbers, timestamps or picture counters on failure.
	std::vector<binary> fragmentSpatialLayers(const binary &frame,
	                                          const std::vector<VP9SpatialLayer> &layers);

	const size_t mMaxFragmentSize;

	// FrameInfo of the frame currently being fragmented, set by the override
	// of RtpPacketizer::outgoing() around the call to fragment().
	shared_ptr<FrameInfo> mCurrentFrameInfo;

	// Non-flexible spatial layer state, per RFC 9628 section 4.2.
	// Starts at 0 and advances once per successfully packetized spatial picture.
	uint16_t mPictureId = 0;   // 15-bit, wraps
	uint8_t mTl0PicIdx = 0;    // 8-bit, wraps
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_VP9_RTP_PACKETIZER_H */
