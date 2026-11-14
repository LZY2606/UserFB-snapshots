/**
 * Copyright (c) 2026 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_VP9_RTP_DEPACKETIZER_H
#define RTC_VP9_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "message.hpp"
#include "rtpdepacketizer.hpp"

#include <memory>
#include <optional>

namespace rtc {

/// RTP depacketization for VP9
class RTC_CPP_EXPORT VP9RtpDepacketizer final : public VideoRtpDepacketizer {
public:
	VP9RtpDepacketizer();
	~VP9RtpDepacketizer() override;

	/// Selects the highest spatial layer to reassemble and deliver.
	/// @param maxSid maximum SID to retain (0-7), or nullopt for legacy single-layer mode.
	/// @throws std::out_of_range if maxSid is greater than 7.
	void setSpatialLayerLimit(std::optional<uint8_t> maxSid);

	void incoming(message_vector &messages, const message_callback &send) override;

private:
	message_ptr reassemble(message_buffer &buffer) override;

	struct Impl;
	std::unique_ptr<Impl> mImpl;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_VP9_RTP_DEPACKETIZER_H */
