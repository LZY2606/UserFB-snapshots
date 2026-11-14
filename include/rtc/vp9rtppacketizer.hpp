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

	void outgoing(message_vector &messages, const message_callback &send) override;

private:
	std::vector<binary> fragment(binary frame) override;

	const size_t mMaxFragmentSize;

	// Counters for non-flexible spatial layer packetization
	uint16_t mPictureId = 0; // 15 bits
	uint8_t mTl0PicIdx = 0;
};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_VP9_RTP_PACKETIZER_H */
