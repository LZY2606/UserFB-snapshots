// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package opus

import (
	"math"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TOC configurations used below, with hand-computed 48 kHz frame counts:
//   - config 16: CELT-only NB, 2.5 ms -> 120 frames
//   - config 18: CELT-only NB, 10 ms  -> 480 frames
//   - config 19: CELT-only NB, 20 ms  -> 960 frames
//   - config 10: SILK-only WB, 40 ms  -> 1920 frames
//   - config 11: SILK-only NB, 60 ms  -> 2880 frames
//   - config 13: Hybrid SWB, 20 ms    -> 960 frames
//   - config 28: CELT-only FB, 2.5 ms -> 120 frames
//   - config 29: CELT-only FB, 5 ms   -> 240 frames
//   - config 30: CELT-only FB, 10 ms  -> 480 frames
//   - config 31: CELT-only FB, 20 ms  -> 960 frames
const (
	testCfgCELT2500us = 16
	testCfgCELT10ms   = 18
	testCfgCELT20ms   = 19
	testCfgSILK40ms   = 10
	testCfgSILK60ms   = 11
	testCfgHybrid20ms = 13
	testCfgFB2500us   = 28
	testCfgFB5ms      = 29
	testCfgFB10ms     = 30
	testCfgFB20ms     = 31
)

func code0Packet(config int) []byte {
	return []byte{byte(config << 3)} //nolint:gosec // G115: test constants are valid TOC configs.
}

func code3CBRPacket(config, frameCount int) []byte {
	return []byte{ //nolint:gosec // G115: test constants are valid TOC configs and frame counts.
		byte(config<<3) | byte(frameCodeArbitraryFrames),
		byte(frameCount),
	}
}

func stereoCode0Packet(config int) []byte {
	return []byte{byte(config<<3) | 0b00000100} //nolint:gosec // G115: test constants are valid TOC configs.
}

func repeatedCode0Packets(config, count int) [][]byte {
	packets := make([][]byte, count)
	for i := range count {
		packets[i] = code0Packet(config)
	}

	return packets
}

func TestTrimPlanWorkedExample(t *testing.T) {
	t.Parallel()

	// Ten packets of one 20 ms frame each: T = 10 * 960 = 9600 frames.
	packets := repeatedCode0Packets(testCfgCELT20ms, 10)

	plan, err := PlanTrim(packets, 312, 168, 5000, 6000)

	require.NoError(t, err)
	// R = 312 + 5000 = 5312; preroll bound = 5312 - 3840 = 1472, so the
	// latest packet start <= 1472 is packet 1 at 960 (not packet 5).
	// rawEnd = 6312 is first covered by packet 7 (start 6720 is the first
	// start beyond it), hence EndPacket = 7.
	assert.Equal(t, TrimPlan{
		FirstPacket: 1,
		EndPacket:   7,
		Discard:     5312 - 960, // 4352
		Keep:        1000,
	}, plan)
}

func TestTrimPlanUnequalPacketDurations(t *testing.T) {
	t.Parallel()

	t.Run("2.5ms through 120ms packets use duration-based preroll", func(t *testing.T) {
		t.Parallel()

		packets := [][]byte{
			code0Packet(testCfgFB2500us),       // p0: 120,  start 0
			code0Packet(testCfgFB2500us),       // p1: 120,  start 120
			code0Packet(testCfgFB5ms),          // p2: 240,  start 240
			code0Packet(testCfgFB10ms),         // p3: 480,  start 480
			code0Packet(testCfgFB20ms),         // p4: 960,  start 960
			code0Packet(testCfgCELT20ms),       // p5: 960,  start 1920
			{tocByte(frameCodeTwoEqualFrames)}, // p6: 2 * 960, start 2880
			// p7: code 3 CBR, 48 frames of 2.5 ms = 120 ms = 5760 frames.
			code3CBRPacket(testCfgFB2500us, 48),
		}
		// Hand-computed starts: 0, 120, 240, 480, 960, 1920, 2880, 3840.
		// T = 3840 + 5760 = 9600; audible = 9600 - 100 - 50 = 9450.
		plan, err := PlanTrim(packets, 100, 50, 6000, 8000)

		require.NoError(t, err)
		// R = 6100, preroll bound = 2260: packet 5 (start 1920) is the
		// latest start <= 2260, giving 4180 frames of context. A fixed
		// "four packets back" rule would pick packet 3, whose start 480
		// only covers 1620 frames before R, well short of the 3840
		// required.
		// rawEnd = 8100 falls inside packet 7, which ends at 9600.
		assert.Equal(t, TrimPlan{
			FirstPacket: 5,
			EndPacket:   8,
			Discard:     6100 - 1920, // 4180
			Keep:        2000,
		}, plan)
	})

	t.Run("120ms packets need fewer packets than a fixed packet-count rule", func(t *testing.T) {
		t.Parallel()

		// Four packets, each 48 frames of 2.5 ms = 5760 frames (120 ms).
		packets := make([][]byte, 4)
		for i := range packets {
			packets[i] = code3CBRPacket(testCfgFB2500us, 48)
		}
		// T = 4 * 5760 = 23040; starts: 0, 5760, 11520, 17280.
		plan, err := PlanTrim(packets, 0, 0, 10000, 12000)

		require.NoError(t, err)
		// R = 10000, preroll bound = 6160: packet 1 (start 5760) gives
		// 4240 frames of context; packet 0 would discard 10000 frames.
		// rawEnd = 12000 is inside packet 2 (11520..17280).
		assert.Equal(t, TrimPlan{
			FirstPacket: 1,
			EndPacket:   3,
			Discard:     10000 - 5760, // 4240
			Keep:        2000,
		}, plan)
	})

	t.Run("preroll bound landing exactly on a packet start", func(t *testing.T) {
		t.Parallel()

		// Ten 20 ms packets; R = 4800 so R-3840 = 960, exactly packet 1.
		packets := repeatedCode0Packets(testCfgCELT20ms, 10)
		plan, err := PlanTrim(packets, 0, 0, 4800, 5760)

		require.NoError(t, err)
		assert.Equal(t, TrimPlan{
			FirstPacket: 1,
			EndPacket:   6, // rawEnd 5760 is the start of packet 6.
			Discard:     3840,
			Keep:        960,
		}, plan)
	})

	t.Run("interval inside one 120ms packet", func(t *testing.T) {
		t.Parallel()

		packets := [][]byte{code3CBRPacket(testCfgFB2500us, 48)}
		plan, err := PlanTrim(packets, 0, 0, 100, 200)

		require.NoError(t, err)
		assert.Equal(t, TrimPlan{FirstPacket: 0, EndPacket: 1, Discard: 100, Keep: 100}, plan)
	})
}

func TestTrimPlanCrossPacketPreSkip(t *testing.T) {
	t.Parallel()

	t.Run("pre-skip spans the first two packets", func(t *testing.T) {
		t.Parallel()

		// Five 10 ms packets: T = 5 * 480 = 2400; pre-skip 700 crosses
		// packet 0 (ends at 480) into packet 1. Audible = 1600.
		packets := repeatedCode0Packets(testCfgCELT10ms, 5)
		plan, err := PlanTrim(packets, 700, 100, 200, 1000)

		require.NoError(t, err)
		// R = 900; the stream is shorter than 80 ms so preroll cannot be
		// satisfied and FirstPacket stays 0. rawEnd = 1700: packet 4
		// starts at 1920, the first start beyond 1700.
		assert.Equal(t, TrimPlan{
			FirstPacket: 0,
			EndPacket:   4,
			Discard:     900,
			Keep:        800,
		}, plan)
	})

	t.Run("pre-skip crosses three packets with enough preroll afterwards", func(t *testing.T) {
		t.Parallel()

		// Ten 10 ms packets: T = 4800; pre-skip 1000 crosses packets
		// 0..2; audible = 4800 - 1000 - 200 = 3600.
		packets := repeatedCode0Packets(testCfgCELT10ms, 10)
		plan, err := PlanTrim(packets, 1000, 200, 3500, 3600)

		require.NoError(t, err)
		// R = 4500, preroll bound = 660: packet 1 starts at 480, packet
		// 2 starts at 960 which is too late (only 3540 frames context).
		assert.Equal(t, TrimPlan{
			FirstPacket: 1,
			EndPacket:   10,         // rawEnd 4600 exceeds the last start 4320.
			Discard:     4500 - 480, // 4020
			Keep:        100,
		}, plan)
	})
}

func TestTrimPlanAudibleEOF(t *testing.T) {
	t.Parallel()

	// Same stream as the worked example: T = 9600, audible length 9120.
	packets := repeatedCode0Packets(testCfgCELT20ms, 10)

	t.Run("export ends at audible EOF", func(t *testing.T) {
		t.Parallel()

		plan, err := PlanTrim(packets, 312, 168, 8120, 9120)

		require.NoError(t, err)
		// R = 8432, preroll bound = 4592: packet 4 starts at 3840.
		// rawEnd = 9432 has no packet start beyond it (last is 8640),
		// so EndPacket is one past the final packet.
		assert.Equal(t, TrimPlan{
			FirstPacket: 4,
			EndPacket:   10,
			Discard:     8432 - 3840, // 4592
			Keep:        1000,
		}, plan)
	})

	t.Run("export ends exactly on a packet boundary", func(t *testing.T) {
		t.Parallel()

		plan, err := PlanTrim(packets, 312, 168, 8000, 8328)

		require.NoError(t, err)
		// rawEnd = 8640 is exactly the start of packet 9.
		assert.Equal(t, TrimPlan{
			FirstPacket: 4,
			EndPacket:   9,
			Discard:     8312 - 3840, // 4472
			Keep:        328,
		}, plan)
	})
}

func TestTrimPlanPrerollAtStreamStart(t *testing.T) {
	t.Parallel()

	// Two 20 ms packets cannot provide 80 ms of context; starting at the
	// raw origin must still select the first packet.
	packets := repeatedCode0Packets(testCfgCELT20ms, 2)

	plan, err := PlanTrim(packets, 0, 0, 0, 960)
	require.NoError(t, err)
	assert.Equal(t, TrimPlan{FirstPacket: 0, EndPacket: 1, Discard: 0, Keep: 960}, plan)

	plan, err = PlanTrim(packets, 0, 0, 0, 100)
	require.NoError(t, err)
	assert.Equal(t, TrimPlan{FirstPacket: 0, EndPacket: 1, Discard: 0, Keep: 100}, plan)
}

func TestTrimPlanEmptyInterval(t *testing.T) {
	t.Parallel()

	packets := repeatedCode0Packets(testCfgCELT20ms, 10)

	t.Run("zero-length interval inside the audible range", func(t *testing.T) {
		t.Parallel()

		plan, err := PlanTrim(packets, 312, 168, 5000, 5000)
		require.NoError(t, err)
		assert.Zero(t, plan)
	})

	t.Run("zero-length interval at the audible origin and EOF", func(t *testing.T) {
		t.Parallel()

		plan, err := PlanTrim(packets, 312, 168, 0, 0)
		require.NoError(t, err)
		assert.Zero(t, plan)

		plan, err = PlanTrim(packets, 312, 168, 9120, 9120)
		require.NoError(t, err)
		assert.Zero(t, plan)
	})

	t.Run("zero audible length only allows a zero interval", func(t *testing.T) {
		t.Parallel()

		// One 960-frame packet fully consumed by head and tail trim.
		onePacket := repeatedCode0Packets(testCfgCELT20ms, 1)
		plan, err := PlanTrim(onePacket, 480, 480, 0, 0)
		require.NoError(t, err)
		assert.Zero(t, plan)

		_, err = PlanTrim(onePacket, 480, 480, 0, 1)
		require.Error(t, err)
		assert.ErrorIs(t, err, errMalformedPacket)
	})
}

func TestTrimPlanEmptyPacketList(t *testing.T) {
	t.Parallel()

	t.Run("zero trims and zero interval is valid", func(t *testing.T) {
		t.Parallel()

		plan, err := PlanTrim(nil, 0, 0, 0, 0)
		require.NoError(t, err)
		assert.Zero(t, plan)

		plan, err = PlanTrim([][]byte{}, 0, 0, 0, 0)
		require.NoError(t, err)
		assert.Zero(t, plan)
	})

	t.Run("non-zero trim or interval is rejected", func(t *testing.T) {
		t.Parallel()

		_, err := PlanTrim(nil, 1, 0, 0, 0)
		require.Error(t, err)
		assert.ErrorIs(t, err, errMalformedPacket)

		_, err = PlanTrim(nil, 0, 1, 0, 0)
		require.Error(t, err)
		assert.ErrorIs(t, err, errMalformedPacket)

		_, err = PlanTrim(nil, 0, 0, 0, 1)
		require.Error(t, err)
		assert.ErrorIs(t, err, errMalformedPacket)

		_, err = PlanTrim(nil, 0, 0, 1, 1)
		require.Error(t, err)
		assert.ErrorIs(t, err, errMalformedPacket)
	})
}

func TestTrimPlanRejectsInvalidPackets(t *testing.T) {
	t.Parallel()

	// Every malformed corpus entry is placed at zero-based index 2 so the
	// error must name that index. Entries mirror
	// TestParsePacketFramesRFC6716MalformedRules.
	malformed := [][]byte{
		makePacket(tocByte(frameCodeOneFrame), maxOpusFrameSize+1),
		makePacket(tocByte(frameCodeTwoEqualFrames), 2*(maxOpusFrameSize+1)),
		{tocByte(frameCodeTwoEqualFrames), 0x00},
		{tocByte(frameCodeTwoDifferentFrames)},
		{tocByte(frameCodeTwoDifferentFrames), 3, 0xAA, 0xBB},
		{tocByte(frameCodeArbitraryFrames), 0x00},
		{tocByte(frameCodeArbitraryFrames), 7},
		{tocByte(frameCodeArbitraryFrames), 0b01000001, 5},
		{tocByte(frameCodeArbitraryFrames), 0b01000001, 255, 0},
		{tocByte(frameCodeArbitraryFrames), 2, 0xAA},
		{tocByte(frameCodeArbitraryFrames), 0b10000010, 3},
	}

	for _, bad := range malformed {
		packets := [][]byte{
			code0Packet(testCfgCELT20ms),
			code0Packet(testCfgCELT20ms),
			append([]byte(nil), bad...),
		}

		plan, err := PlanTrim(packets, 0, 0, 0, 1)
		assert.Error(t, err, "packet %v should be rejected", bad)
		assert.ErrorIs(t, err, errMalformedPacket)
		assert.ErrorContains(t, err, "packet 2")
		assert.Zero(t, plan)
	}

	t.Run("empty packet reports its index", func(t *testing.T) {
		t.Parallel()

		plan, err := PlanTrim([][]byte{code0Packet(testCfgCELT20ms), nil}, 0, 0, 0, 1)
		require.Error(t, err)
		assert.ErrorIs(t, err, errMalformedPacket)
		assert.ErrorIs(t, err, errTooShortForTableOfContentsHeader)
		assert.ErrorContains(t, err, "packet 1")
		assert.Zero(t, plan)
	})

	t.Run("over-120ms code 3 packet reports its index", func(t *testing.T) {
		t.Parallel()

		// Config 31 gives 960-frame frames; 7 frames = 6720 > 5760.
		packets := [][]byte{code3CBRPacket(testCfgFB20ms, 7)}
		plan, err := PlanTrim(packets, 0, 0, 0, 1)
		require.Error(t, err)
		assert.ErrorIs(t, err, errMalformedPacket)
		assert.ErrorContains(t, err, "packet 0")
		assert.Zero(t, plan)
	})

	t.Run("exactly 120ms packets are accepted", func(t *testing.T) {
		t.Parallel()

		// 48 frames of 2.5 ms and two 60 ms frames both total 5760.
		packets := [][]byte{
			code3CBRPacket(testCfgFB2500us, 48),
			{byte(testCfgSILK60ms<<3) | byte(frameCodeTwoEqualFrames)}, //nolint:gosec // G115
		}
		plan, err := PlanTrim(packets, 0, 0, 0, 2*5760)
		require.NoError(t, err)
		assert.Equal(t, TrimPlan{FirstPacket: 0, EndPacket: 2, Discard: 0, Keep: 2 * 5760}, plan)
	})
}

func TestTrimPlanRejectsOutOfRangeTrimsAndIntervals(t *testing.T) {
	t.Parallel()

	packets := repeatedCode0Packets(testCfgCELT20ms, 1) // T = 960

	tests := []struct {
		name                   string
		preSkip, endTrim, s, e uint64
	}{
		{"pre-skip exceeds T", 961, 0, 0, 0},
		{"end trim exceeds T", 0, 961, 0, 0},
		{"pre-skip plus end trim exceeds T", 900, 100, 0, 0},
		{"start beyond audible length", 0, 0, 961, 961},
		{"end beyond audible length", 0, 0, 0, 961},
		{"start greater than end", 0, 0, 500, 400},
		{"both trims consume T with non-empty interval", 960, 0, 0, 1},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()

			plan, err := PlanTrim(packets, tt.preSkip, tt.endTrim, tt.s, tt.e)
			require.Error(t, err)
			assert.ErrorIs(t, err, errMalformedPacket)
			assert.Zero(t, plan)
		})
	}
}

func TestTrimPlanFrameDurationsByTOC(t *testing.T) {
	t.Parallel()

	// Each expectation is the hand-computed 48 kHz duration from the TOC
	// configuration and frame count; Keep must equal the full duration when
	// exporting the whole single-packet stream.
	tests := []struct {
		name        string
		packet      []byte
		wantKeep    uint64
		description string
	}{
		{"CELT 2.5ms one frame", code0Packet(testCfgCELT2500us), 120, "48000 * 2.5/1000"},
		{"CELT 10ms one frame", code0Packet(testCfgCELT10ms), 480, "48000 * 10/1000"},
		{"CELT 20ms one frame", code0Packet(testCfgCELT20ms), 960, "48000 * 20/1000"},
		{"SILK 40ms one frame", code0Packet(testCfgSILK40ms), 1920, "48000 * 40/1000"},
		{"SILK 60ms one frame", code0Packet(testCfgSILK60ms), 2880, "48000 * 60/1000"},
		{"Hybrid 20ms one frame", code0Packet(testCfgHybrid20ms), 960, "48000 * 20/1000"},
		{"CELT 20ms two equal frames", []byte{tocByte(frameCodeTwoEqualFrames)}, 2 * 960, "2 frames * 960"},
		{
			"CELT 20ms two zero-length different frames",
			[]byte{tocByte(frameCodeTwoDifferentFrames), 0},
			2 * 960,
			"2 frames * 960; zero payload bytes are legal",
		},
		{
			"CELT FB 2.5ms four arbitrary frames",
			code3CBRPacket(testCfgFB2500us, 4),
			4 * 120,
			"4 frames * 120",
		},
		{"stereo CELT 20ms one frame", stereoCode0Packet(testCfgFB20ms), 960, "channels do not change frame timing"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()

			plan, err := PlanTrim([][]byte{tt.packet}, 0, 0, 0, tt.wantKeep)
			require.NoError(t, err, tt.description)
			assert.Equal(t, TrimPlan{FirstPacket: 0, EndPacket: 1, Discard: 0, Keep: tt.wantKeep}, plan)
		})
	}
}

func TestTrimPlanZeroLengthFramesDoNotChangeDuration(t *testing.T) {
	t.Parallel()

	// Code 2 packet with two zero-length frames and a code 3 CBR packet
	// with one zero-length frame are both valid per RFC 6716 and still
	// occupy their full audio duration.
	packets := [][]byte{
		{tocByte(frameCodeTwoDifferentFrames), 0}, // 2 * 960 = 1920
		{tocByte(frameCodeArbitraryFrames), 1},    // 1 * 960 = 960
	}
	// T = 2880.
	plan, err := PlanTrim(packets, 0, 0, 100, 2880)

	require.NoError(t, err)
	assert.Equal(t, TrimPlan{FirstPacket: 0, EndPacket: 2, Discard: 100, Keep: 2780}, plan)
}

func TestTrimPlanInputNotModifiedAndRetryable(t *testing.T) {
	t.Parallel()

	good := repeatedCode0Packets(testCfgCELT20ms, 4)
	packets := [][]byte{
		good[0],
		good[1],
		code3CBRPacket(testCfgCELT20ms, 7), // malformed: 7 * 960 > 5760
		good[3],
	}
	snapshot := append([][]byte(nil), packets...)
	for i, packet := range snapshot {
		snapshot[i] = append([]byte(nil), packet...)
	}

	for range 2 {
		plan, err := PlanTrim(packets, 0, 0, 0, 100)
		require.Error(t, err)
		assert.ErrorContains(t, err, "packet 2")
		assert.Zero(t, plan)
	}

	for i := range packets {
		assert.Equal(t, snapshot[i], packets[i], "packet %d was modified", i)
	}

	// The same slice layout with a valid third packet is accepted.
	packets[2] = code0Packet(testCfgCELT20ms)
	plan, err := PlanTrim(packets, 0, 0, 0, 4*960)
	require.NoError(t, err)
	assert.Equal(t, TrimPlan{FirstPacket: 0, EndPacket: 4, Discard: 0, Keep: 4 * 960}, plan)
}

func TestTrimPlanSaturatingArithmetic(t *testing.T) {
	t.Parallel()

	sum, overflow := saturatingAdd(math.MaxUint64-5, 6)
	assert.True(t, overflow)
	assert.Equal(t, uint64(math.MaxUint64), sum)

	sum, overflow = saturatingAdd(math.MaxUint64-5, 5)
	assert.False(t, overflow)
	assert.Equal(t, uint64(math.MaxUint64), sum)

	product, overflow := saturatingMul(math.MaxUint64, 2)
	assert.True(t, overflow)
	assert.Equal(t, uint64(math.MaxUint64), product)

	product, overflow = saturatingMul(math.MaxUint64/3840, 3840)
	assert.False(t, overflow)
	assert.Equal(t, uint64(math.MaxUint64/3840*3840), product)

	product, overflow = saturatingMul(0, math.MaxUint64)
	assert.False(t, overflow)
	assert.Equal(t, uint64(0), product)
}
