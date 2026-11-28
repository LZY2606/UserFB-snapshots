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

#include <array>
#include <map>

namespace rtc {

/// RTP depacketization for VP9
class RTC_CPP_EXPORT VP9RtpDepacketizer final : public VideoRtpDepacketizer {
public:
	VP9RtpDepacketizer();
	~VP9RtpDepacketizer();

	/// Sets the highest spatial layer (SID) to deliver.
	///
	/// nullopt restores the legacy behavior, where a picture is delivered when
	/// its RTP marker (the end of the highest layer) arrives and the payload is
	/// passed through unchanged.
	///
	/// When set to a value in 0..7, a picture is delivered as soon as the target
	/// layer and every layer retained below it are complete, without waiting for
	/// the marker of a higher layer. The output is a plain frame for a single
	/// retained layer, or the concatenated layers followed by a VP9 superframe
	/// index otherwise.
	///
	/// The limit is sampled on the first valid packet of each picture, so
	/// changing it mid-picture only affects subsequent pictures.
	///
	/// @param maxSid Highest SID to retain, or nullopt for the legacy mode
	/// @throws std::out_of_range if maxSid has a value greater than 7
	void setSpatialLayerLimit(optional<uint8_t> maxSid);

private:
	message_ptr reassemble(message_buffer &buffer) override;

	/// A received packet of a spatial layer, keyed by its unwrapped sequence
	/// number.
	struct SpatialSegment {
		binary data;
		bool startOfFrame = false;
		bool endOfFrame = false;
	};

	struct SpatialLayer {
		bool used = false;
		bool sawStart = false;
		bool sawEnd = false;
		bool identityKnown = false;
		bool interPicturePredicted = false;
		bool interLayerPredicted = false;
		bool contradictory = false;
		std::map<int64_t, SpatialSegment> segments;
	};

	struct SpatialPicture {
		bool active = false;
		bool delivered = false;
		uint8_t limit = 0; // limit sampled at the start of the picture
		uint32_t timestamp = 0;
		uint8_t payloadType = 0;
		uint8_t highestSid = 0;
		bool hasPictureId = false;
		uint16_t pictureId = 0;
		bool markerSeen = false;
		uint8_t markerSid = 0;
		uint16_t lastSequence = 0;
		int64_t lastUnwrapped = 0;
		std::array<SpatialLayer, 8> layers;

		void reset() { *this = SpatialPicture(); }
	};

	/// Spatial layer reassembly. Returns a frame once the target layers of the
	/// current picture are complete.
	message_ptr reassembleSpatial(const message_vector &messages);

	/// Assembles a layer if it has a contiguous start..end run and returns true.
	static bool completeLayer(const SpatialLayer &layer, binary &out);

	SpatialPicture mSpatial;
	optional<uint8_t> mSpatialLayerLimit;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_VP9_RTP_DEPACKETIZER_H */
