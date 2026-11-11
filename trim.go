// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package opus

import (
	"fmt"
	"math"
)

// Fixed preroll, in 48 kHz per-channel sample frames, that PlanTrim keeps in
// front of the target start so a fresh decoder can build up state. 3840
// frames is 80 ms of audio; the four 20 ms packets rule is only a special
// case of this duration-based preroll.
const trimPrerollFrames uint64 = 3840

// Samples per millisecond in the 48 kHz CELT time domain used for all trim
// accounting, independent of decoder output rate or channel count.
const samplesPerMillisecond = celtSampleRate / 1000

// RFC 6716 §3.2.5 [R5]: a packet holds at most 120 ms of audio, which is
// 5760 per-channel frames in the 48 kHz time domain.
const maxOpusPacketFrames = maxOpusPacketDurationNanosecond * celtSampleRate / 1000000000

// TrimPlan describes which complete Opus packets a caller must feed to a
// fresh 48 kHz decoder to export a sample interval, and how to trim the
// decoded audio. All time fields are counted in 48 kHz per-channel sample
// frames, whether the decoder outputs mono/stereo or at another API rate.
//
// Both packet indices and the decoded sample interval are half-open: feed
// packets[FirstPacket:EndPacket], discard Discard frames per channel, then
// keep Keep frames per channel.
type TrimPlan struct {
	// FirstPacket is the zero-based index of the first packet to decode.
	FirstPacket int
	// EndPacket is the exclusive zero-based index of the last packet to
	// decode, so the selected slice is packets[FirstPacket:EndPacket].
	EndPacket int
	// Discard is the number of decoded per-channel frames, measured from
	// the start of FirstPacket, to drop before the exported interval.
	Discard uint64
	// Keep is the number of decoded per-channel frames in the export.
	Keep uint64
}

// PlanTrim computes a TrimPlan for the audible interval [start, end) of
// packets already ordered by decode order. preSkip and endTrim are the
// container's pre-skip and end trimming in 48 kHz per-channel frames; they
// are applied once globally and never re-applied to individual packets.
//
// The packets must be raw Opus packets without OpusHead/OpusTags, container
// pages or RTP headers. PlanTrim only parses packet structure: it never
// reads files, decodes audio or re-encodes, and it never modifies packets,
// so a failed call can be retried with the same input. Any error is
// returned together with a zero TrimPlan.
func PlanTrim(packets [][]byte, preSkip, endTrim, start, end uint64) (TrimPlan, error) {
	durations, total, err := packetDurations(packets)
	if err != nil {
		return TrimPlan{}, err
	}

	audibleLength, err := audibleDuration(total, preSkip, endTrim)
	if err != nil {
		return TrimPlan{}, err
	}
	if start > audibleLength || end > audibleLength || start > end {
		return TrimPlan{}, fmt.Errorf(
			"%w: interval [%d,%d) is outside the audible length %d",
			errMalformedPacket, start, end, audibleLength,
		)
	}

	// An empty, in-range export still requires every packet and the global
	// head/tail trimming to have been validated above.
	if start == end {
		return TrimPlan{}, nil
	}

	return buildTrimPlan(durations, preSkip, start, end)
}

// buildTrimPlan maps the audible interval onto raw packet positions and
// selects the packet range once all inputs are known to be valid.
func buildTrimPlan(durations []uint64, preSkip, start, end uint64) (TrimPlan, error) {
	rawStart, overflow := saturatingAdd(preSkip, start)
	if overflow {
		return TrimPlan{}, fmt.Errorf("%w: raw start position overflows uint64", errMalformedPacket)
	}
	rawEnd, overflow := saturatingAdd(preSkip, end)
	if overflow {
		return TrimPlan{}, fmt.Errorf("%w: raw end position overflows uint64", errMalformedPacket)
	}

	prerollBound := uint64(0)
	if rawStart > trimPrerollFrames {
		prerollBound = rawStart - trimPrerollFrames
	}

	firstPacket, firstPacketStart, endPacket := selectPacketRange(durations, prerollBound, rawEnd)

	return TrimPlan{
		FirstPacket: firstPacket,
		EndPacket:   endPacket,
		Discard:     rawStart - firstPacketStart,
		Keep:        end - start,
	}, nil
}

// selectPacketRange returns the first and exclusive last packet indices for
// the plan. FirstPacket is the latest packet whose start is no later than
// prerollBound, guaranteeing at least 80 ms of decoder context whenever the
// stream contains that much audio; EndPacket is the smallest exclusive
// index whose packet start is at or beyond rawEnd.
func selectPacketRange(durations []uint64, prerollBound, rawEnd uint64) (int, uint64, int) {
	firstPacket := 0
	firstPacketStart := uint64(0)
	endPacket := 0

	var position uint64
	for i := range durations {
		if endPacket == 0 && position >= rawEnd {
			endPacket = i
		}
		if position <= prerollBound {
			firstPacket = i
			firstPacketStart = position
		}
		position += durations[i]
	}

	// rawEnd lies in the final packet (or at the stream end), so the
	// exclusive index is one past the last packet.
	if endPacket == 0 {
		endPacket = len(durations)
	}

	return firstPacket, firstPacketStart, endPacket
}

// audibleDuration returns T-preSkip-endTrim after checking that the global
// head and tail trimming does not exceed the total packet duration T.
func audibleDuration(total, preSkip, endTrim uint64) (uint64, error) {
	if preSkip > total || endTrim > total || preSkip > total-endTrim {
		return 0, fmt.Errorf(
			"%w: pre-skip %d plus end trim %d exceed total packet duration %d",
			errMalformedPacket, preSkip, endTrim, total,
		)
	}

	return total - preSkip - endTrim, nil
}

// packetDurations validates every packet with the same structure checks the
// decoder uses and returns each packet's raw audio duration in 48 kHz
// per-channel frames. Padding contributes no audio time, and zero-length
// frames are legal: payloads are never decoded to judge their content.
func packetDurations(packets [][]byte) ([]uint64, uint64, error) {
	durations := make([]uint64, len(packets))

	var total uint64
	for i, packet := range packets {
		if len(packet) < 1 {
			return nil, 0, fmt.Errorf(
				"packet %d: %w: %w", i, errMalformedPacket, errTooShortForTableOfContentsHeader,
			)
		}

		tocHeader := tableOfContentsHeader(packet[0])
		frames, err := parsePacketFrames(packet, tocHeader)
		if err != nil {
			return nil, 0, fmt.Errorf("packet %d: %w", i, err)
		}

		frameFrames := frameDurationInSamples(tocHeader.configuration())
		if frameFrames == 0 {
			return nil, 0, fmt.Errorf(
				"%w: packet %d uses unsupported configuration %d",
				errMalformedPacket, i, tocHeader.configuration(),
			)
		}

		// RFC 6716 §3.2.5 [R5]: a packet must contain no more than 120 ms
		// (5760 frames at 48 kHz) of audio. parsePacketFrames already
		// rejects code 3 packets past this limit; enforce it for every
		// frame code as well.
		frameCount := uint64(len(frames)) //nolint:gosec // G115: frame count is bounded by the validated packet.
		duration, overflow := saturatingMul(frameCount, frameFrames)
		if overflow || duration > maxOpusPacketFrames {
			return nil, 0, fmt.Errorf(
				"%w: packet %d duration %d frames exceeds 120 ms",
				errMalformedPacket, i, duration,
			)
		}

		total, overflow = saturatingAdd(total, duration)
		if overflow {
			return nil, 0, fmt.Errorf("%w: total packet duration overflows uint64", errMalformedPacket)
		}
		durations[i] = duration
	}

	return durations, total, nil
}

// frameDurationInSamples maps a TOC configuration to its per-frame duration
// in 48 kHz per-channel sample frames. Opus frame lengths are 2.5, 5, 10,
// 20, 40 or 60 ms, i.e. 120, 240, 480, 960, 1920 or 2880 frames.
func frameDurationInSamples(cfg Configuration) uint64 {
	switch cfg.frameDuration() {
	case frameDuration2500us:
		return samplesPerMillisecond * 5 / 2
	case frameDuration5ms:
		return samplesPerMillisecond * 5
	case frameDuration10ms:
		return samplesPerMillisecond * 10
	case frameDuration20ms:
		return samplesPerMillisecond * 20
	case frameDuration40ms:
		return samplesPerMillisecond * 40
	case frameDuration60ms:
		return samplesPerMillisecond * 60
	default:
		return 0
	}
}

func saturatingAdd(augend, addend uint64) (uint64, bool) {
	if augend > math.MaxUint64-addend {
		return math.MaxUint64, true
	}

	return augend + addend, false
}

func saturatingMul(multiplicand, multiplier uint64) (uint64, bool) {
	if multiplier != 0 && multiplicand > math.MaxUint64/multiplier {
		return math.MaxUint64, true
	}

	return multiplicand * multiplier, false
}
