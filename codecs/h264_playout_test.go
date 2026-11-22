// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package codecs

import (
	"bytes"
	"encoding/binary"
	"testing"
	"time"

	"github.com/pion/rtp"
	"github.com/pion/rtp/task_support/taskcheck"
)

const (
	playoutTestSSRC uint32 = 0x13572468
	playoutTestPT   uint8  = 96
)

var (
	testAUD   = []byte{0x09, 0xF0}
	testSPS   = []byte{0x67, 0x42, 0xC0, 0x1F, 0x10, 0x20}
	testPPS   = []byte{0x68, 0xCE, 0x3C, 0x80}
	testIDR   = []byte{0x65, 0x88, 0x84, 0x00, 0x21, 0x42, 0x63}
	testSlice = []byte{0x41, 0x9A, 0x00, 0x01, 0x02, 0x03}
)

// playoutAnnexB is an independent expectation builder: it never uses the
// production payloader or depacketizer.
func playoutAnnexB(nalus ...[]byte) []byte {
	var out []byte
	for _, nalu := range nalus {
		out = append(out, 0x00, 0x00, 0x00, 0x01)
		out = append(out, nalu...)
	}

	return out
}

func playoutAVC(nalus ...[]byte) []byte {
	var out []byte
	for _, nalu := range nalus {
		out = binary.BigEndian.AppendUint32(out, uint32(len(nalu))) //nolint:gosec // test data
		out = append(out, nalu...)
	}

	return out
}

func playoutStapA(nalus ...[]byte) []byte {
	out := []byte{0x78}
	for _, nalu := range nalus {
		out = binary.BigEndian.AppendUint16(out, uint16(len(nalu))) //nolint:gosec // test data
		out = append(out, nalu...)
	}

	return out
}

func playoutPacket(seq uint16, ts, ssrc uint32, pt uint8, marker bool, payload []byte) *rtp.Packet {
	return &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			Marker:         marker,
			PayloadType:    pt,
			SequenceNumber: seq,
			Timestamp:      ts,
			SSRC:           ssrc,
		},
		Payload: payload,
	}
}

// playoutRawRTP serialises the 12 byte RTP header by hand so the test does not
// depend on the production marshaller for its inputs.
func playoutRawRTP(seq uint16, ts, ssrc uint32, pt uint8, marker bool, payload []byte) []byte {
	raw := make([]byte, 12+len(payload))
	raw[0] = 0x80
	if marker {
		raw[1] |= 0x80
	}
	raw[1] |= pt & 0x7F
	binary.BigEndian.PutUint16(raw[2:4], seq)
	binary.BigEndian.PutUint32(raw[4:8], ts)
	binary.BigEndian.PutUint32(raw[8:12], ssrc)
	copy(raw[12:], payload)

	return raw
}

func TestH264Assembly(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- Batch API: multiple NAL types, shuffled order and duplicate. ---
		stapA := playoutStapA(testSPS, testPPS)

		// FU-A carrying testIDR, hand written: three fragments.
		fuaStart := []byte{0x7C, 0x85, 0x88, 0x84}
		fuaMiddle := []byte{0x7C, 0x05, 0x00, 0x21}
		fuaEnd := []byte{0x7C, 0x45, 0x42, 0x63}

		batch := []*rtp.Packet{
			playoutPacket(11, 90000, playoutTestSSRC, playoutTestPT, false, stapA),
			playoutPacket(10, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(15, 90000, playoutTestSSRC, playoutTestPT, true, testSlice),
			playoutPacket(12, 90000, playoutTestSSRC, playoutTestPT, false, fuaStart),
			playoutPacket(13, 90000, playoutTestSSRC, playoutTestPT, false, fuaMiddle),
			// duplicate of the middle fragment: must be tolerated.
			playoutPacket(13, 90000, playoutTestSSRC, playoutTestPT, false, fuaMiddle),
		}
		// The end fragment arrives last on purpose, still out of sequence.
		batch = append(batch, playoutPacket(14, 90000, playoutTestSSRC, playoutTestPT, false, fuaEnd))

		got, err := (&H264Packet{}).UnmarshalAccessUnit(batch)
		c.Check(err == nil, "batch AU should assemble, got %v", err)
		wantAnnexB := playoutAnnexB(testAUD, testSPS, testPPS, testIDR, testSlice)
		c.Check(bytes.Equal(got, wantAnnexB), "batch AU AnnexB mismatch\n got %x\nwant %x", got, wantAnnexB)

		avcGot, err := (&H264Packet{IsAVC: true}).UnmarshalAccessUnit(batch)
		c.Check(err == nil, "batch AU with IsAVC should assemble, got %v", err)
		wantAVC := playoutAVC(testAUD, testSPS, testPPS, testIDR, testSlice)
		c.Check(bytes.Equal(avcGot, wantAVC), "batch AU AVC mismatch\n got %x\nwant %x", avcGot, wantAVC)

		// --- Batch API rejects malformed or incomplete access units. ---
		valid := []*rtp.Packet{
			playoutPacket(20, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(21, 90000, playoutTestSSRC, playoutTestPT, true, testIDR),
		}
		ok, err := (&H264Packet{}).UnmarshalAccessUnit(valid)
		c.Check(err == nil, "valid AU rejected: %v", err)
		c.Check(bytes.Equal(ok, playoutAnnexB(testAUD, testIDR)), "valid AU bytes mismatch: %x", ok)

		reject := func(name string, packets []*rtp.Packet) {
			out, err := (&H264Packet{}).UnmarshalAccessUnit(packets)
			c.Check(err != nil, "%s: expected error, got %x", name, out)
			c.Check(out == nil, "%s: expected nil output on error, got %x", name, out)
		}

		reject("empty batch", nil)
		reject("no AUD", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, true, testIDR),
		})
		reject("AUD after slice", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testIDR),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, testAUD),
		})
		reject("two AUDs", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, testAUD),
		})
		reject("marker before end", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, testIDR),
			playoutPacket(32, 90000, playoutTestSSRC, playoutTestPT, false, testSlice),
		})
		reject("missing marker", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, false, testIDR),
		})
		reject("sequence gap", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(32, 90000, playoutTestSSRC, playoutTestPT, true, testIDR),
		})
		reject("FU-A continuation without start", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, fuaMiddle),
		})
		reject("FU-A type change", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, false, fuaStart),
			playoutPacket(32, 90000, playoutTestSSRC, playoutTestPT, false,
				[]byte{0x7C, 0x01, 0x00, 0x00}),
			playoutPacket(33, 90000, playoutTestSSRC, playoutTestPT, true, fuaEnd),
		})
		reject("unterminated FU-A", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, false, fuaStart),
			playoutPacket(32, 90000, playoutTestSSRC, playoutTestPT, true, fuaMiddle),
		})
		reject("conflicting duplicate", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, testIDR),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, testSlice),
		})
		reject("inconsistent timestamp", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 93000, playoutTestSSRC, playoutTestPT, true, testIDR),
		})
		reject("inconsistent SSRC", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC+1, playoutTestPT, true, testIDR),
		})
		reject("STAP-A with AUD member", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, playoutStapA(testAUD)),
		})
		reject("STAP-A truncated", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true,
				[]byte{0x78, 0x00, 0x10, 0x67, 0x42}),
		})
		reject("FU-A too short", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, []byte{0x7C}),
		})
		reject("FU-A carrying AUD", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, []byte{0x7C, 0x49, 0x00}),
		})
		reject("empty payload", []*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(31, 90000, playoutTestSSRC, playoutTestPT, true, nil),
		})

		// --- A failed batch must not disturb the streaming FU-A buffer. ---
		streaming := &H264Packet{}
		first, err := streaming.Unmarshal([]byte{0x7C, 0x85, 0x88, 0x84})
		c.Check(err == nil, "streaming FU start should buffer, got %v", err)
		c.Check(len(first) == 0, "streaming FU start should emit nothing, got %x", first)

		_, err = streaming.UnmarshalAccessUnit([]*rtp.Packet{
			playoutPacket(30, 90000, playoutTestSSRC, playoutTestPT, false, fuaStart),
			playoutPacket(32, 90000, playoutTestSSRC, playoutTestPT, true, fuaEnd),
		})
		c.Check(err != nil, "malformed batch should fail")

		middle, err := streaming.Unmarshal(fuaMiddle)
		c.Check(err == nil, "streaming FU middle should buffer, got %v", err)
		c.Check(len(middle) == 0, "streaming FU middle should emit nothing, got %x", middle)

		last, err := streaming.Unmarshal(fuaEnd)
		c.Check(err == nil, "streaming FU end should complete, got %v", err)
		c.Check(bytes.Equal(last, playoutAnnexB(testIDR)),
			"streaming FU-A buffer was corrupted by failed batch: %x", last)

		// --- Push validation must leave buffered media and state untouched. ---
		guarded, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 8)
		c.Check(err == nil, "guarded playout creation failed: %v", err)
		c.Check(guarded.Push(playoutRawRTP(80, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"guarded head should buffer")
		c.Check(guarded.Push(playoutRawRTP(81, 0, playoutTestSSRC, playoutTestPT, true, testIDR), 0) == nil,
			"guarded tail should buffer")

		// Wrong SSRC, wrong PT, wrong RTP version, broken framing, broken STAP-A
		// and NSLog. None may alter the buffered access unit.
		c.Check(guarded.Push(playoutRawRTP(82, 0, playoutTestSSRC+1, playoutTestPT, false, testSlice), 0) != nil,
			"wrong SSRC must be rejected")
		c.Check(guarded.Push(playoutRawRTP(82, 0, playoutTestSSRC, playoutTestPT+1, false, testSlice), 0) != nil,
			"wrong payload type must be rejected")
		badVersion := playoutRawRTP(82, 0, playoutTestSSRC, playoutTestPT, false, testSlice)
		badVersion[0] = 0x40 // version 1
		c.Check(guarded.Push(badVersion, 0) != nil, "wrong RTP version must be rejected")
		c.Check(guarded.Push([]byte{0x80, 0x60}, 0) != nil, "truncated RTP header must be rejected")
		c.Check(guarded.Push(playoutRawRTP(82, 0, playoutTestSSRC, playoutTestPT, false, []byte{0x78, 0x00, 0x09}), 0) != nil,
			"truncated STAP-A must be rejected")
		c.Check(guarded.Push(playoutRawRTP(82, 0, playoutTestSSRC, playoutTestPT, false, []byte{0x7C, 0x09, 0x00}), 0) != nil,
			"FU-A carrying AUD must be rejected")

		// A conflicting duplicate is rejected; an identical duplicate is nil.
		c.Check(guarded.Push(playoutRawRTP(81, 0, playoutTestSSRC, playoutTestPT, true, testIDR), 0) == nil,
			"identical duplicate must return nil")
		c.Check(guarded.Push(playoutRawRTP(81, 0, playoutTestSSRC, playoutTestPT, true, testSlice), 0) != nil,
			"conflicting duplicate must be rejected")

		guardedFrames := guarded.Advance(0)
		c.Check(len(guardedFrames) == 1, "rejections must not disturb the buffered AU, got %d", len(guardedFrames))
		if len(guardedFrames) == 1 {
			c.Check(bytes.Equal(guardedFrames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"rejections altered buffered bytes: %x", guardedFrames[0].AnnexB)
		}

		// --- Non-VCL access units are consumed but never delivered. ---
		pictures, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 8)
		c.Check(err == nil, "pictures playout creation failed: %v", err)
		c.Check(pictures.Push(playoutRawRTP(90, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"non-VCL head should buffer")
		c.Check(pictures.Push(playoutRawRTP(91, 0, playoutTestSSRC, playoutTestPT, true, testSPS), 0) == nil,
			"non-VCL body should buffer")
		c.Check(len(pictures.Advance(0)) == 0, "non-VCL AU must not be delivered")

		// --- An entire AU pushed end-first still assembles. ---
		backwards, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 8)
		c.Check(err == nil, "backwards playout creation failed: %v", err)
		c.Check(backwards.Push(playoutRawRTP(101, 0, playoutTestSSRC, playoutTestPT, true, testIDR), 0) == nil,
			"end-first tail should buffer")
		c.Check(backwards.Push(playoutRawRTP(100, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"end-first head should buffer")
		backwardsFrames := backwards.Advance(0)
		c.Check(len(backwardsFrames) == 1, "end-first AU should assemble, got %d", len(backwardsFrames))
		if len(backwardsFrames) == 1 {
			c.Check(bytes.Equal(backwardsFrames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"end-first bytes mismatch: %x", backwardsFrames[0].AnnexB)
		}

		// --- A late end-before-start FU pair cannot leak the old FU buffer. ---
		ordering, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 16)
		c.Check(err == nil, "ordering playout creation failed: %v", err)
		// AU at ts 0 completes normally so its FU data is used. It starts with
		// an IDR so the playout leaves its wait-for-IDR state.
		c.Check(ordering.Push(playoutRawRTP(110, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"ordering AU head should buffer")
		c.Check(ordering.Push(playoutRawRTP(111, 0, playoutTestSSRC, playoutTestPT, false, fuaStart), 0) == nil,
			"ordering FU start should buffer")
		c.Check(ordering.Push(playoutRawRTP(112, 0, playoutTestSSRC, playoutTestPT, false, fuaMiddle), 0) == nil,
			"ordering FU middle should buffer")
		c.Check(ordering.Push(playoutRawRTP(113, 0, playoutTestSSRC, playoutTestPT, true, fuaEnd), 0) == nil,
			"ordering FU end should buffer")
		orderingFrames := ordering.Advance(0)
		c.Check(len(orderingFrames) == 1, "first ordering AU should play, got %d", len(orderingFrames))
		if len(orderingFrames) == 1 {
			c.Check(bytes.Equal(orderingFrames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"first ordering bytes mismatch: %x", orderingFrames[0].AnnexB)
		}

		// Second AU receives its FU end before its FU start. Even vector order
		// must not splice the previous AU's fragments into this one.
		nextTimestamp := uint32(3000)
		endFirst := []*rtp.Packet{
			playoutPacket(121, nextTimestamp, playoutTestSSRC, playoutTestPT, true, []byte{0x7C, 0x45, 0x99, 0x98}),
			playoutPacket(120, nextTimestamp, playoutTestSSRC, playoutTestPT, false, testAUD),
			playoutPacket(121, nextTimestamp, playoutTestSSRC, playoutTestPT, true, []byte{0x7C, 0x45, 0x99, 0x98}),
		}
		// Missing start: must fail rather than reuse state.
		spliced, err := (&H264Packet{}).UnmarshalAccessUnit(endFirst)
		c.Check(err != nil, "batch with an FU end but no start must fail")
		c.Check(spliced == nil, "failure must return nil, got %x", spliced)

		c.Check(ordering.Push(playoutRawRTP(120, nextTimestamp, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"second AU head should buffer")
		c.Check(ordering.Push(playoutRawRTP(121, nextTimestamp, playoutTestSSRC, playoutTestPT, true,
			[]byte{0x7C, 0x45, 0x99, 0x98}), 0) == nil,
			"end-first FU fragment should buffer")
		c.Check(len(ordering.Advance(time.Duration(nextTimestamp)*time.Second/90000)) == 0,
			"incomplete end-before-start AU must not play")

		// --- End to end playout using STAP-A, FU-A, reordering and duplicates. ---
		playout, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 90000, time.Second, 100*time.Millisecond, 16)
		c.Check(err == nil, "playout should be created, got %v", err)

		au0 := [][]byte{testAUD, stapA, fuaStart, fuaMiddle, fuaEnd}
		pushOrder := []int{4, 2, 0, 3, 1, 3} // end first, duplicate middle
		for _, index := range pushOrder {
			marker := index == len(au0)-1
			raw := playoutRawRTP(uint16(40+index), 90000, playoutTestSSRC, playoutTestPT, marker, au0[index]) //nolint:gosec // bounded
			c.Check(playout.Push(raw, 100*time.Millisecond) == nil,
				"AU0 packet %d should be accepted", index)
		}

		frames := playout.Advance(1100 * time.Millisecond)
		c.Check(len(frames) == 1, "due AU0 should produce one frame, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(frames[0].Timestamp == 90000, "AU0 timestamp = %d", frames[0].Timestamp)
			c.Check(frames[0].Due == 1100*time.Millisecond, "AU0 due = %s", frames[0].Due)
			wantAU0 := playoutAnnexB(testAUD, testSPS, testPPS, testIDR)
			c.Check(bytes.Equal(frames[0].AnnexB, wantAU0),
				"AU0 bytes mismatch\n got %x\nwant %x", frames[0].AnnexB, wantAU0)
		}

		// AU1: plain single NAL units, due 1/30s after AU0.
		au1 := [][]byte{testAUD, testSlice}
		for index, payload := range au1 {
			raw := playoutRawRTP(uint16(45+index), 93000, playoutTestSSRC, playoutTestPT, //nolint:gosec // bounded
				index == len(au1)-1, payload)
			c.Check(playout.Push(raw, 1100*time.Millisecond) == nil,
				"AU1 packet %d should be accepted", index)
		}

		au1Due := 1100*time.Millisecond + time.Duration(3000)*time.Second/90000
		c.Check(au1Due == 1133333333*time.Nanosecond, "unexpected AU1 deadline %s", au1Due)
		frames = playout.Advance(au1Due)
		c.Check(len(frames) == 1, "due AU1 should produce one frame, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(frames[0].Timestamp == 93000, "AU1 timestamp = %d", frames[0].Timestamp)
			c.Check(bytes.Equal(frames[0].AnnexB, playoutAnnexB(testAUD, testSlice)),
				"AU1 bytes mismatch: %x", frames[0].AnnexB)
		}
	})
}

func TestH264Deadline(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- Constructor validation. ---
		invalid := []struct {
			name  string
			pt    uint8
			base  time.Duration
			delay time.Duration
			win   int
		}{
			{"pt out of range", 128, 0, 0, 8},
			{"negative baseTime", 96, -1, 0, 8},
			{"negative delay", 96, 0, -1, 8},
			{"window too small", 96, 0, 0, 1},
			{"window too large", 96, 0, 0, 513},
		}
		for _, tc := range invalid {
			playout, err := NewH264Playout(1, tc.pt, 0, tc.base, tc.delay, tc.win)
			c.Check(err != nil, "%s: expected error", tc.name)
			c.Check(playout == nil, "%s: expected nil playout", tc.name)
		}
		for _, window := range []int{2, 512} {
			playout, err := NewH264Playout(1, 127, 0, 0, 0, window)
			c.Check(err == nil, "window %d should be accepted, got %v", window, err)
			c.Check(playout != nil, "window %d should produce a playout", window)
		}

		// --- Deadline formula, exact boundary behaviour and media offset. ---
		playout, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 90000, 2*time.Second, 150*time.Millisecond, 16)
		c.Check(err == nil, "playout creation failed: %v", err)

		due := 2150 * time.Millisecond // baseTime + delay + 0 ticks
		late := playoutRawRTP(1, 90000, playoutTestSSRC, playoutTestPT, false, testAUD)
		c.Check(playout.Push(late, due+1) != nil, "packet after its deadline must be rejected")

		negative := playoutRawRTP(1, 89999, playoutTestSSRC, playoutTestPT, false, testAUD)
		negativeErr := playout.Push(negative, 0)
		c.Check(negativeErr != nil, "negative media offset must be rejected")
		c.Check(negativeErr != nil && bytes.Contains([]byte(negativeErr.Error()), []byte("negative")),
			"negative media offset error should say so, got %v", negativeErr)

		// Missing final packet until the exact deadline: it may still be filled.
		head := playoutRawRTP(1, 90000, playoutTestSSRC, playoutTestPT, false, testAUD)
		body := playoutRawRTP(2, 90000, playoutTestSSRC, playoutTestPT, true, testIDR)
		c.Check(playout.Push(head, 0) == nil, "AU head before deadline should be accepted")
		c.Check(playout.Push(body, due) == nil, "AU tail at the exact deadline should be accepted")

		frames := playout.Advance(due)
		c.Check(len(frames) == 1, "AU completed at the deadline should play, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(frames[0].Due == due, "frame due = %s, want %s", frames[0].Due, due)
			c.Check(bytes.Equal(frames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"deadline frame bytes mismatch: %x", frames[0].AnnexB)
		}

		// Already consumed at the deadline; the same timestamp must not revive.
		revive := playoutRawRTP(3, 90000, playoutTestSSRC, playoutTestPT, true, testSlice)
		c.Check(playout.Push(revive, due) != nil, "consumed AU must not accept packets at the deadline")
		c.Check(playout.Push(revive, due+time.Nanosecond) != nil, "consumed AU must stay closed after the deadline")

		// --- Out of order arrival plays in media time order. ---
		playout2, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 16)
		c.Check(err == nil, "playout2 creation failed: %v", err)

		// Later AU pushed first, earlier IDR pushed second.
		laterDue := time.Duration(3000) * time.Second / 90000
		c.Check(playout2.Push(playoutRawRTP(12, 3000, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"later AU head should buffer")
		c.Check(playout2.Push(playoutRawRTP(13, 3000, playoutTestSSRC, playoutTestPT, true, testSlice), 0) == nil,
			"later AU tail should buffer")
		c.Check(playout2.Push(playoutRawRTP(10, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"earlier AU head should buffer")
		c.Check(playout2.Push(playoutRawRTP(11, 0, playoutTestSSRC, playoutTestPT, true, testIDR), 0) == nil,
			"earlier AU tail should buffer")

		frames = playout2.Advance(laterDue)
		c.Check(len(frames) == 2, "both due access units should play, got %d", len(frames))
		if len(frames) == 2 {
			c.Check(frames[0].Timestamp == 0, "earlier AU must play first, got ts %d", frames[0].Timestamp)
			c.Check(frames[1].Timestamp == 3000, "later AU must play second, got ts %d", frames[1].Timestamp)
			c.Check(bytes.Equal(frames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"earlier AU bytes mismatch: %x", frames[0].AnnexB)
			c.Check(bytes.Equal(frames[1].AnnexB, playoutAnnexB(testAUD, testSlice)),
				"later AU bytes mismatch: %x", frames[1].AnnexB)
		}

		// --- Window limit: reject without evicting, resume after Advance. ---
		playout3, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 2)
		c.Check(err == nil, "playout3 creation failed: %v", err)
		c.Check(playout3.Push(playoutRawRTP(20, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"first buffered packet should be accepted")
		c.Check(playout3.Push(playoutRawRTP(21, 0, playoutTestSSRC, playoutTestPT, true, testIDR), 0) == nil,
			"second buffered packet should be accepted")
		tooMany := playout3.Push(playoutRawRTP(22, 3000, playoutTestSSRC, playoutTestPT, false, testAUD), 0)
		c.Check(tooMany != nil, "third packet must be rejected by the window")

		frames = playout3.Advance(0)
		c.Check(len(frames) == 1, "buffered AU must survive a rejected packet, got %d frames", len(frames))
		if len(frames) == 1 {
			c.Check(bytes.Equal(frames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"window rejection must not evict buffered bytes: %x", frames[0].AnnexB)
		}
		c.Check(playout3.Push(playoutRawRTP(22, 3000, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"window should accept packets again after Advance")
		c.Check(playout3.Push(playoutRawRTP(23, 3000, playoutTestSSRC, playoutTestPT, true, testSlice), 0) == nil,
			"second packet after Advance should be accepted")
		frames = playout3.Advance(laterDue)
		c.Check(len(frames) == 1, "non-IDR after a delivered IDR should play, got %d", len(frames))

		// --- Oversized rebuild is rejected and does not evict. ---
		playout4, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 8)
		c.Check(err == nil, "playout4 creation failed: %v", err)
		c.Check(playout4.Push(playoutRawRTP(30, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"small AU head should buffer")
		c.Check(playout4.Push(playoutRawRTP(31, 0, playoutTestSSRC, playoutTestPT, true, testIDR), 0) == nil,
			"small AU tail should buffer")
		huge := make([]byte, maxH264AccessUnitSize)
		huge[0] = 0x65
		c.Check(playout4.Push(playoutRawRTP(32, 90000, playoutTestSSRC, playoutTestPT, true, huge), 0) != nil,
			"oversized access unit must be rejected")
		// A large packet that fits on its own but overflows an already-buffered
		// AU must also be rejected, leaving the AU intact.
		largeButFits := make([]byte, maxH264AccessUnitSize-16)
		largeButFits[0] = 0x41
		c.Check(playout4.Push(playoutRawRTP(33, 3000, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"growing AU head should buffer")
		c.Check(playout4.Push(playoutRawRTP(34, 3000, playoutTestSSRC, playoutTestPT, false, largeButFits), 0) == nil,
			"large packet fitting the AU limit should buffer")
		c.Check(playout4.Push(playoutRawRTP(35, 3000, playoutTestSSRC, playoutTestPT, true, testSlice), 0) != nil,
			"packet overflowing an existing AU must be rejected")
		c.Check(len(playout4.Advance(0)) == 1, "existing AU should survive overflow rejection")

		// --- Input ownership: mutating raw after Push must not affect output. ---
		playout5, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 8)
		c.Check(err == nil, "playout5 creation failed: %v", err)
		raw := playoutRawRTP(40, 0, playoutTestSSRC, playoutTestPT, false, testAUD)
		c.Check(playout5.Push(raw, 0) == nil, "owned head should buffer")
		tail := playoutRawRTP(41, 0, playoutTestSSRC, playoutTestPT, true, testIDR)
		c.Check(playout5.Push(tail, 0) == nil, "owned tail should buffer")
		for i := range raw {
			raw[i] = 0xFF
		}
		for i := range tail {
			tail[i] = 0xFF
		}
		frames = playout5.Advance(0)
		c.Check(len(frames) == 1, "owned AU should still play, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(bytes.Equal(frames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"push must copy the datagram, got %x", frames[0].AnnexB)
		}

		// --- Missing marker at the deadline consumes and discards the AU. ---
		playout6, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 8)
		c.Check(err == nil, "playout6 creation failed: %v", err)
		c.Check(playout6.Push(playoutRawRTP(50, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"unterminated AU head should buffer")
		c.Check(playout6.Push(playoutRawRTP(51, 0, playoutTestSSRC, playoutTestPT, false, testIDR), 0) == nil,
			"unterminated AU body should buffer")
		frames = playout6.Advance(0)
		c.Check(len(frames) == 0, "AU without marker must not play, got %d", len(frames))
		c.Check(playout6.Push(playoutRawRTP(52, 0, playoutTestSSRC, playoutTestPT, true, testSlice), 0) != nil,
			"consumed AU must not accept the marker afterwards")

		// --- Reset clears buffers and reference state, keeps parameters. ---
		playout7, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 8)
		c.Check(err == nil, "playout7 creation failed: %v", err)
		c.Check(playout7.Push(playoutRawRTP(60, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"pre-reset head should buffer")
		c.Check(playout7.Push(playoutRawRTP(61, 0, playoutTestSSRC, playoutTestPT, true, testSlice), 0) == nil,
			"pre-reset tail should buffer")
		playout7.Reset()
		c.Check(len(playout7.Advance(time.Hour)) == 0, "Reset must drop buffered packets")
		// Reset also restores the wait-for-IDR state: a non-IDR AU is consumed.
		c.Check(playout7.Push(playoutRawRTP(62, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"post-reset head should buffer")
		c.Check(playout7.Push(playoutRawRTP(63, 0, playoutTestSSRC, playoutTestPT, true, testSlice), 0) == nil,
			"post-reset tail should buffer")
		c.Check(len(playout7.Advance(0)) == 0, "post-reset non-IDR must not play while waiting for IDR")
		// Constructor parameters are retained: deadlines still use baseTime/delay.
		playout8, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 5*time.Second, time.Second, 8)
		c.Check(err == nil, "playout8 creation failed: %v", err)
		playout8.Reset()
		c.Check(playout8.Push(playoutRawRTP(70, 0, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"post-reset AU should buffer")
		c.Check(playout8.Push(playoutRawRTP(71, 0, playoutTestSSRC, playoutTestPT, true, testIDR), 0) == nil,
			"post-reset IDR should buffer")
		c.Check(len(playout8.Advance(5999*time.Millisecond)) == 0, "deadline parameters must survive Reset")
		frames = playout8.Advance(6 * time.Second)
		c.Check(len(frames) == 1, "AU should play at baseTime+delay after Reset, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(frames[0].Due == 6*time.Second, "post-reset due = %s", frames[0].Due)
		}
	})
}

func TestH264Recovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- The reference state starts out waiting for an IDR. ---
		playout, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0, 0, 0, 32)
		c.Check(err == nil, "playout creation failed: %v", err)

		pushAU := func(firstSeq uint16, timestamp uint32, marker bool, payloads ...[]byte) {
			for index, payload := range payloads {
				last := index == len(payloads)-1
				seq := firstSeq + uint16(index) //nolint:gosec // test data
				raw := playoutRawRTP(seq, timestamp, playoutTestSSRC, playoutTestPT, last && marker, payload)
				c.Check(playout.Push(raw, 0) == nil, "seq %d should be accepted", seq)
			}
		}

		// Complete non-IDR AU is consumed but not delivered.
		pushAU(10, 0, true, testAUD, testSlice)
		c.Check(len(playout.Advance(0)) == 0, "non-IDR while waiting must not play")

		// Complete IDR AU restores delivery.
		pushAU(12, 3000, true, testAUD, testIDR)
		frames := playout.Advance(time.Duration(3000) * time.Second / 90000)
		c.Check(len(frames) == 1, "IDR should restore delivery, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(bytes.Equal(frames[0].AnnexB, playoutAnnexB(testAUD, testIDR)),
				"IDR bytes mismatch: %x", frames[0].AnnexB)
		}

		// Complete non-IDR continues afterwards.
		pushAU(14, 6000, true, testAUD, testSlice)
		c.Check(len(playout.Advance(2*time.Duration(3000)*time.Second/90000)) == 1,
			"non-IDR after IDR should play")

		// --- An incomplete AU drops the reference state, and no gap is needed. ---
		// AU with a hole: AUD and marker slice with a missing middle packet.
		c.Check(playout.Push(playoutRawRTP(16, 9000, playoutTestSSRC, playoutTestPT, false, testAUD), 0) == nil,
			"incomplete AU head should buffer")
		c.Check(playout.Push(playoutRawRTP(18, 9000, playoutTestSSRC, playoutTestPT, true, testSlice), 0) == nil,
			"incomplete AU tail should buffer")
		c.Check(len(playout.Advance(3*time.Duration(3000)*time.Second/90000)) == 0,
			"incomplete AU must not play")

		// While waiting again, complete non-IDR is consumed.
		pushAU(19, 12000, true, testAUD, testSlice)
		c.Check(len(playout.Advance(4*time.Duration(3000)*time.Second/90000)) == 0,
			"non-IDR after a dropped AU must not play")

		// IDR restores delivery again.
		pushAU(21, 15000, true, testAUD, testIDR)
		c.Check(len(playout.Advance(5*time.Duration(3000)*time.Second/90000)) == 1,
			"IDR after a dropped AU should restore delivery")

		// --- A whole missing AU is detected through the sequence gap. ---
		pushAU(23, 18000, true, testAUD, testSlice)
		c.Check(len(playout.Advance(6*time.Duration(3000)*time.Second/90000)) == 1,
			"contiguous non-IDR should play before the gap")

		// Skip one AU entirely: previous AU ended at 24, the next starts at 26.
		pushAU(26, 21000, true, testAUD, testSlice)
		c.Check(len(playout.Advance(7*time.Duration(3000)*time.Second/90000)) == 0,
			"sequence gap must disable delivery until an IDR arrives")

		pushAU(28, 24000, true, testAUD, testSlice)
		c.Check(len(playout.Advance(8*time.Duration(3000)*time.Second/90000)) == 0,
			"non-IDR after a sequence gap must not play")

		pushAU(30, 27000, true, testAUD, testIDR)
		c.Check(len(playout.Advance(9*time.Duration(3000)*time.Second/90000)) == 1,
			"IDR after a sequence gap should restore delivery")

		// --- Late fragments must not roll the reference state back. ---
		stale := playoutRawRTP(31, 27000, playoutTestSSRC, playoutTestPT, true, testSlice)
		c.Check(playout.Push(stale, 9*time.Duration(3000)*time.Second/90000) != nil,
			"late fragment for a consumed AU must be rejected")
		pushAU(32, 30000, true, testAUD, testSlice)
		c.Check(len(playout.Advance(10*time.Duration(3000)*time.Second/90000)) == 1,
			"reference state must not roll back after a late fragment")

		// --- Sequence number and timestamp wrap together. ---
		wrap, err := NewH264Playout(playoutTestSSRC, playoutTestPT, 0xFFFFFF00, 0, 0, 32)
		c.Check(err == nil, "wrap playout creation failed: %v", err)

		pushWrap := func(firstSeq uint16, timestamp uint32, payloads ...[]byte) {
			for index, payload := range payloads {
				seq := firstSeq + uint16(index) //nolint:gosec // wraps on purpose
				raw := playoutRawRTP(seq, timestamp, playoutTestSSRC, playoutTestPT,
					index == len(payloads)-1, payload)
				c.Check(wrap.Push(raw, 0) == nil, "wrap seq %d should be accepted", seq)
			}
		}

		wrapDue := func(offset int64) time.Duration {
			return time.Duration(offset) * time.Second / 90000
		}

		// IDR at the very end of the sequence and timestamp spaces.
		pushWrap(65534, 0xFFFFFFF0, testAUD, testIDR)
		frames = wrap.Advance(wrapDue(0xF0))
		c.Check(len(frames) == 1, "IDR before wrap should play, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(frames[0].Timestamp == 0xFFFFFFF0, "pre-wrap timestamp = %x", frames[0].Timestamp)
		}

		// Sequence wraps 65535 -> 0 and timestamp wraps ffffffff -> 0.
		pushWrap(0, 0x00000010, testAUD, testSlice)
		frames = wrap.Advance(wrapDue(0x110))
		c.Check(len(frames) == 1, "AU after both wraps should play, got %d", len(frames))
		if len(frames) == 1 {
			c.Check(frames[0].Timestamp == 0x10, "post-wrap timestamp = %x", frames[0].Timestamp)
			c.Check(frames[0].Due == wrapDue(0x110), "post-wrap due = %s", frames[0].Due)
			c.Check(bytes.Equal(frames[0].AnnexB, playoutAnnexB(testAUD, testSlice)),
				"post-wrap bytes mismatch: %x", frames[0].AnnexB)
		}

		// The wrap must not look like a sequence gap: delivery keeps working,
		// and the next contiguous AU is played without another IDR.
		pushWrap(2, 0x00000030, testAUD, testSlice)
		c.Check(len(wrap.Advance(wrapDue(0x130))) == 1,
			"contiguous AU after wrap should keep playing")

		// A gap across the wrap boundary must still drop the reference state.
		pushWrap(10, 0x00000050, testAUD, testSlice)
		c.Check(len(wrap.Advance(wrapDue(0x150))) == 0,
			"gap across the wrap boundary must disable delivery")
		pushWrap(12, 0x00000070, testAUD, testIDR)
		c.Check(len(wrap.Advance(wrapDue(0x170))) == 1,
			"IDR after a gap across the wrap should restore delivery")
	})
}
