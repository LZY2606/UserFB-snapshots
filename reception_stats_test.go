// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"math"
	"testing"
	"time"

	"github.com/pion/rtcp/task_support/taskcheck"
)

const (
	testLocalSSRC = 8
	testClockRate = 90000
)

func testSession(c *taskcheck.C, cname string, clockRate uint32) *ReceptionSession {
	session, err := NewReceptionSession(testLocalSSRC, cname, clockRate)
	c.Check(err == nil, "NewReceptionSession: %v", err)

	return session
}

// decodeCompound parses the emitted wire bytes back and returns the RR and
// the CNAME carried by the SourceDescription packet.
func decodeCompound(c *taskcheck.C, raw []byte) (ReceiverReport, string) {
	var compound CompoundPacket
	err := compound.Unmarshal(raw)
	c.Check(err == nil, "compound unmarshal: %v", err)
	c.Check(len(compound) == 2, "compound must hold RR and SDES, got %d packets", len(compound))

	rr, ok := compound[0].(*ReceiverReport)
	c.Check(ok, "first compound packet is a ReceiverReport")
	_, ok = compound[1].(*SourceDescription)
	c.Check(ok, "second compound packet is a SourceDescription, got %T", compound[1])
	cname, err := compound.CNAME()
	c.Check(err == nil, "compound CNAME: %v", err)
	if rr == nil {
		return ReceiverReport{}, cname
	}
	c.Check(rr.SSRC == testLocalSSRC, "RR sender SSRC = %d", rr.SSRC)

	return *rr, cname
}

func findBlock(rr ReceiverReport, ssrc uint32) (ReceptionReport, bool) {
	for _, block := range rr.Reports {
		if block.SSRC == ssrc {
			return block, true
		}
	}

	return ReceptionReport{}, false
}

func senderReportRaw(c *taskcheck.C, ssrc uint32, ntp uint64) []byte {
	raw, err := Marshal([]Packet{&SenderReport{SSRC: ssrc, NTPTime: ntp}})
	c.Check(err == nil, "marshal SR: %v", err)

	return raw
}

func ms(value int64) time.Duration {
	return time.Duration(value) * time.Millisecond
}

// TestReceptionSequence exercises appendix A.1 validation: probation base
// points, 16-bit wrap with late packets and duplicates, broken trials,
// dropout/misorder boundaries, restart confirmation and BYE rebirth.
func TestReceptionSequence(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		session := testSession(c, "rx", testClockRate)

		// A source needs two consecutive sequence numbers; the second packet
		// is the statistics base point and the first one counted.
		c.Check(!session.ObserveRTP(1000, 10, 100, ms(1)), "first probation packet must be rejected")
		c.Check(session.ObserveRTP(1000, 11, 200, ms(2)), "second consecutive packet is accepted")

		raw, err := session.Report(ms(3), 1500)
		c.Check(err == nil, "first report: %v", err)
		rr, cname := decodeCompound(c, raw)
		c.Check(cname == "rx", "cname round trip = %q", cname)
		c.Check(len(rr.Reports) == 1, "one report block, got %d", len(rr.Reports))
		block, ok := findBlock(rr, 1000)
		c.Check(ok, "block for SSRC 1000 present")
		c.Check(block.LastSequenceNumber == 11, "base extended sequence = %d", block.LastSequenceNumber)
		c.Check(block.TotalLost == 0, "no loss at base point, got %d", block.TotalLost)

		// Cross-zero progression, then late and duplicate packets from the old
		// cycle: they still count as received but never move the high watermark.
		c.Check(!session.ObserveRTP(2000, 65534, 0, ms(4)), "wrap source starts probation")
		c.Check(session.ObserveRTP(2000, 65535, 0, ms(5)), "wrap source confirmed at 65535")
		c.Check(session.ObserveRTP(2000, 0, 0, ms(6)), "sequence zero advances the cycle")
		c.Check(session.ObserveRTP(2000, 65535, 0, ms(7)), "late packet from old cycle accepted")
		c.Check(session.ObserveRTP(2000, 0, 0, ms(8)), "duplicate packet accepted")

		raw, err = session.Report(ms(9), 1500)
		c.Check(err == nil, "wrap report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, ok = findBlock(rr, 2000)
		c.Check(ok, "block for wrap source present")
		c.Check(block.LastSequenceNumber == 1<<16, "extended sequence crossed zero, got %d", block.LastSequenceNumber)
		// Two more packets arrived than the sequence space expects: the
		// signed 24-bit loss count is negative, encoded as two's complement.
		c.Check(block.TotalLost == 0xFFFFFE, "negative loss encoded on 24 bits, got %#x", block.TotalLost)

		// A broken trial restarts from the offending packet.
		c.Check(!session.ObserveRTP(3000, 5, 0, ms(10)), "trial packet one")
		c.Check(!session.ObserveRTP(3000, 7, 0, ms(11)), "non-consecutive packet breaks and restarts trial")
		c.Check(session.ObserveRTP(3000, 8, 0, ms(12)), "restarted trial confirms on next sequence")

		// MAX_DROPOUT boundary: exactly 3000 forward is accepted, 3001 is not.
		c.Check(!session.ObserveRTP(5000, 1, 0, ms(13)), "dropout source probation one")
		c.Check(session.ObserveRTP(5000, 2, 0, ms(14)), "dropout source confirmed")
		c.Check(session.ObserveRTP(5000, 3002, 0, ms(15)), "jump of MAX_DROPOUT accepted")
		c.Check(!session.ObserveRTP(5000, 6003, 0, ms(16)), "jump beyond MAX_DROPOUT rejected")

		// MAX_MISORDER boundary: a delta of -100 is still reordering, -101
		// reopens the trial and the following packet confirms a restart.
		c.Check(!session.ObserveRTP(6000, 1, 0, ms(17)), "misorder source probation one")
		c.Check(session.ObserveRTP(6000, 2, 0, ms(18)), "misorder source confirmed")
		c.Check(session.ObserveRTP(6000, 102, 0, ms(19)), "forward jump to sequence 102 accepted")
		c.Check(session.ObserveRTP(6000, 2, 0, ms(20)), "late packet exactly MAX_MISORDER behind accepted")
		c.Check(!session.ObserveRTP(6000, 1, 0, ms(21)), "packet beyond MAX_MISORDER rejected")
		c.Check(session.ObserveRTP(6000, 2, 0, ms(22)), "next sequence confirms restart")

		// Oversized jump: rejection leaves the old statistics alive until the
		// successor sequence confirms the restart, at which point everything
		// (including the earlier SR timing) is cleared.
		c.Check(!session.ObserveRTP(4000, 1, 0, ms(23)), "jump source probation one")
		c.Check(session.ObserveRTP(4000, 2, 0, ms(24)), "jump source confirmed")
		for seq := uint16(3); seq <= 12; seq++ {
			c.Check(session.ObserveRTP(4000, seq, 0, ms(25)), "in-order packet %d accepted", seq)
		}
		srRaw := senderReportRaw(c, 4000, 0x1122334455667788)
		c.Check(session.ObserveRTCP(srRaw, ms(26)) == nil, "SR before jump accepted")
		c.Check(!session.ObserveRTP(4000, 3013, 0, ms(27)), "jump of 3001 rejected")
		c.Check(!session.ObserveRTP(4000, 3013, 0, ms(28)), "repeated jump packet restarts trial")
		c.Check(session.ObserveRTP(4000, 3014, 0, ms(29)), "successor confirms the restart")

		raw, err = session.Report(ms(30), 1500)
		c.Check(err == nil, "post-restart report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, ok = findBlock(rr, 4000)
		c.Check(ok, "restarted source still reported")
		c.Check(block.LastSequenceNumber == 3014, "restart base sequence = %d", block.LastSequenceNumber)
		c.Check(block.TotalLost == 0, "restart cleared cumulative loss, got %d", block.TotalLost)
		c.Check(block.Jitter == 0, "restart cleared jitter, got %d", block.Jitter)
		c.Check(block.LastSenderReport == 0, "restart cleared SR timing, got %#x", block.LastSenderReport)
		block, ok = findBlock(rr, 3000)
		c.Check(ok && block.LastSequenceNumber == 8, "broken-trial source reports base 8")
		block, ok = findBlock(rr, 6000)
		c.Check(ok && block.LastSequenceNumber == 2, "misorder restart reports base 2, got ok=%v", ok)

		// BYE removes the source; the same SSRC later starts a fresh trial.
		byeRaw, err := Marshal([]Packet{&Goodbye{Sources: []uint32{7000}}})
		c.Check(err == nil, "marshal BYE: %v", err)
		c.Check(!session.ObserveRTP(7000, 1, 0, ms(31)), "bye source probation one")
		c.Check(session.ObserveRTP(7000, 2, 0, ms(32)), "bye source confirmed")
		c.Check(session.ObserveRTCP(byeRaw, ms(33)) == nil, "BYE processed")
		raw, err = session.Report(ms(34), 1500)
		c.Check(err == nil, "post-bye report: %v", err)
		rr, _ = decodeCompound(c, raw)
		_, ok = findBlock(rr, 7000)
		c.Check(!ok, "BYE source absent from report")
		c.Check(!session.ObserveRTP(7000, 5, 0, ms(35)), "reborn source must re-enter probation")
		c.Check(session.ObserveRTP(7000, 6, 0, ms(36)), "reborn source confirms again")
		raw, err = session.Report(ms(37), 1500)
		c.Check(err == nil, "rebirth report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, ok = findBlock(rr, 7000)
		c.Check(ok && block.LastSequenceNumber == 6, "reborn source reports new base 6")
	})
}

// TestReceptionIntervals covers appendix A.3 loss accounting, report rotation
// across more than 31 sources, byte budgeting and retry semantics.
func TestReceptionIntervals(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Constructor validation: a non-zero clock rate and a 1-255 byte CNAME
		// are mandatory.
		_, err := NewReceptionSession(testLocalSSRC, "rx", 0)
		c.Check(err != nil, "zero clock rate must be rejected")
		_, err = NewReceptionSession(testLocalSSRC, "", testClockRate)
		c.Check(err != nil, "empty CNAME must be rejected")
		_, err = NewReceptionSession(testLocalSSRC, string(make([]byte, 256)), testClockRate)
		c.Check(err != nil, "256-byte CNAME must be rejected")
		_, err = NewReceptionSession(testLocalSSRC, string(make([]byte, 255)), testClockRate)
		c.Check(err == nil, "255-byte CNAME accepted: %v", err)

		session := testSession(c, "rx", testClockRate)

		// Fraction lost: two of three expected interval packets were lost.
		c.Check(!session.ObserveRTP(11, 1, 0, ms(1)), "loss source probation one")
		c.Check(session.ObserveRTP(11, 2, 0, ms(2)), "loss source confirmed")
		c.Check(session.ObserveRTP(11, 5, 0, ms(3)), "packet after a two-packet gap")
		raw, err := session.Report(ms(4), 1500)
		c.Check(err == nil, "loss report: %v", err)
		rr, _ := decodeCompound(c, raw)
		block, ok := findBlock(rr, 11)
		c.Check(ok, "loss source reported")
		c.Check(block.TotalLost == 2, "cumulative loss = %d", block.TotalLost)
		c.Check(block.FractionLost == 170, "fraction lost 2/3 = 170, got %d", block.FractionLost)

		// After a successful report the baseline moves: a clean interval and
		// an empty interval both report zero fraction while cumulative loss
		// stays put.
		c.Check(session.ObserveRTP(11, 6, 0, ms(5)), "clean packet accepted")
		raw, err = session.Report(ms(6), 1500)
		c.Check(err == nil, "clean interval report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 11)
		c.Check(block.FractionLost == 0, "clean interval fraction = %d", block.FractionLost)
		c.Check(block.TotalLost == 2, "cumulative loss preserved = %d", block.TotalLost)
		raw, err = session.Report(ms(7), 1500)
		c.Check(err == nil, "empty interval report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 11)
		c.Check(block.FractionLost == 0, "empty interval fraction = %d", block.FractionLost)

		// Heavy loss drives the rounded fraction up to its 255 ceiling clamp.
		c.Check(session.ObserveRTP(11, 261, 0, ms(8)), "large gap accepted")
		raw, err = session.Report(ms(9), 1500)
		c.Check(err == nil, "heavy loss report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 11)
		c.Check(block.FractionLost == 254, "254/255 fraction = %d", block.FractionLost)
		c.Check(block.TotalLost == 256, "cumulative loss = %d", block.TotalLost)

		// 33 ready sources: selection rotates in strict numeric successor
		// order without ever repeating a source inside one report.
		rotate := testSession(c, "rotate", testClockRate)
		for ssrc := uint32(1); ssrc <= 33; ssrc++ {
			c.Check(!rotate.ObserveRTP(ssrc, 1, 0, 0), "source %d probation", ssrc)
			c.Check(rotate.ObserveRTP(ssrc, 2, 0, 0), "source %d confirmed", ssrc)
		}
		raw, err = rotate.Report(0, 65536)
		c.Check(err == nil, "first rotation report: %v", err)
		rr, _ = decodeCompound(c, raw)
		c.Check(len(rr.Reports) == maxReportBlocks, "first report holds 31 blocks, got %d", len(rr.Reports))
		for i, block := range rr.Reports {
			c.Check(block.SSRC == uint32(i+1), "first report block %d SSRC = %d", i, block.SSRC)
		}
		raw, err = rotate.Report(0, 65536)
		c.Check(err == nil, "second rotation report: %v", err)
		rr, _ = decodeCompound(c, raw)
		c.Check(len(rr.Reports) == maxReportBlocks, "second report holds 31 blocks")
		want := []uint32{32, 33}
		for ssrc := uint32(1); ssrc <= 29; ssrc++ {
			want = append(want, ssrc)
		}
		for i, block := range rr.Reports {
			c.Check(block.SSRC == want[i], "second report block %d SSRC = %d want %d", i, block.SSRC, want[i])
		}
		raw, err = rotate.Report(0, 65536)
		c.Check(err == nil, "third rotation report: %v", err)
		rr, _ = decodeCompound(c, raw)
		c.Check(rr.Reports[0].SSRC == 30, "third report resumes after cursor 29, got %d", rr.Reports[0].SSRC)
		c.Check(rr.Reports[len(rr.Reports)-1].SSRC == 27, "third report ends on SSRC 27")

		// Source deletion and addition keep the numeric successor rule.
		byeRaw, err := Marshal([]Packet{&Goodbye{Sources: []uint32{33}}})
		c.Check(err == nil, "marshal rotation BYE: %v", err)
		c.Check(rotate.ObserveRTCP(byeRaw, 0) == nil, "rotation BYE applied")
		c.Check(!rotate.ObserveRTP(100, 1, 0, 0), "new source probation")
		c.Check(rotate.ObserveRTP(100, 2, 0, 0), "new source confirmed")
		raw, err = rotate.Report(0, 65536)
		c.Check(err == nil, "fourth rotation report: %v", err)
		rr, _ = decodeCompound(c, raw)
		c.Check(rr.Reports[0].SSRC == 28, "successor of cursor 27 is 28, got %d", rr.Reports[0].SSRC)
		c.Check(rr.Reports[5].SSRC == 100, "new source 100 takes its numeric slot")
		c.Check(rr.Reports[6].SSRC == 1, "rotation wraps back to SSRC 1")

		// CNAME alignment consumes budget: when no block fits the call must
		// fail rather than emit an empty RR, and a failed call must not move
		// the cursor.
		budget := testSession(c, string(make([]byte, 200)), testClockRate)
		sdes := NewCNAMESourceDescription(testLocalSSRC, string(make([]byte, 200)))
		baseSize := (&ReceiverReport{SSRC: testLocalSSRC}).MarshalSize() + sdes.MarshalSize()
		for _, ssrc := range []uint32{1, 2} {
			c.Check(!budget.ObserveRTP(ssrc, 1, 0, 0), "budget source %d probation", ssrc)
			c.Check(budget.ObserveRTP(ssrc, 2, 0, 0), "budget source %d confirmed", ssrc)
		}
		_, err = budget.Report(0, baseSize)
		c.Check(err != nil, "budget without room for one block must error")
		raw, err = budget.Report(0, baseSize+receptionReportLength)
		c.Check(err == nil, "exact one-block budget: %v", err)
		c.Check(len(raw) == baseSize+receptionReportLength, "wire length = %d", len(raw))
		rr, cname := decodeCompound(c, raw)
		c.Check(len(cname) == 200, "200-byte CNAME round trips, got %d", len(cname))
		c.Check(len(rr.Reports) == 1 && rr.Reports[0].SSRC == 1, "cursor did not advance after failed report")

		// Sources skipped for budget reasons keep their interval baseline.
		skip := testSession(c, "skip", testClockRate)
		skipBase := (&ReceiverReport{SSRC: testLocalSSRC}).MarshalSize() +
			NewCNAMESourceDescription(testLocalSSRC, "skip").MarshalSize()
		for _, ssrc := range []uint32{1, 2} {
			c.Check(!skip.ObserveRTP(ssrc, 1, 0, 0), "skip source %d probation", ssrc)
			c.Check(skip.ObserveRTP(ssrc, 2, 0, 0), "skip source %d confirmed", ssrc)
			c.Check(skip.ObserveRTP(ssrc, 5, 0, 0), "skip source %d gap packet", ssrc)
		}
		raw, err = skip.Report(0, skipBase+receptionReportLength)
		c.Check(err == nil, "one-block report: %v", err)
		rr, _ = decodeCompound(c, raw)
		c.Check(len(rr.Reports) == 1 && rr.Reports[0].SSRC == 1, "only the first source fits")
		c.Check(rr.Reports[0].FractionLost == 170, "first source reports its loss")
		raw, err = skip.Report(0, 65536)
		c.Check(err == nil, "full budget retry: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, ok = findBlock(rr, 1)
		c.Check(ok && block.FractionLost == 0, "reported source baseline advanced")
		block, ok = findBlock(rr, 2)
		c.Check(ok && block.FractionLost == 170, "skipped source kept its interval loss, got %d", block.FractionLost)

		// With no validated source an empty RR plus CNAME is still valid.
		empty := testSession(c, "empty", testClockRate)
		emptyBase := (&ReceiverReport{SSRC: testLocalSSRC}).MarshalSize() +
			NewCNAMESourceDescription(testLocalSSRC, "empty").MarshalSize()
		raw, err = empty.Report(0, 65536)
		c.Check(err == nil, "empty report: %v", err)
		rr, cname = decodeCompound(c, raw)
		c.Check(len(rr.Reports) == 0, "empty RR carries no blocks")
		c.Check(cname == "empty", "empty RR still carries the CNAME")
		c.Check(len(raw) == emptyBase, "empty compound wire length = %d want %d", len(raw), emptyBase)
	})
}

// TestReceptionClock checks timestamp-wrap-safe jitter, SR timing fields and
// atomic application of parsed RTCP datagrams.
func TestReceptionClock(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// 1000 ticks per second makes the jitter arithmetic easy to follow.
		session := testSession(c, "clock", 1000)

		// RTP timestamps wrapping through 2^32 must not create a phantom jump.
		c.Check(!session.ObserveRTP(1, 10, 0xFFFFFF00, ms(1000)), "jitter source probation one")
		c.Check(session.ObserveRTP(1, 11, 0xFFFFFF00, ms(1000)), "jitter source confirmed, phase established")
		c.Check(session.ObserveRTP(1, 12, 0x00000096, ms(2000)), "packet crossing the timestamp wrap")
		raw, err := session.Report(ms(2100), 1500)
		c.Check(err == nil, "jitter report one: %v", err)
		rr, _ := decodeCompound(c, raw)
		block, ok := findBlock(rr, 1)
		c.Check(ok, "jitter source reported")
		c.Check(block.Jitter == 37, "wrap-safe jitter after D=594 = 37, got %d", block.Jitter)

		c.Check(session.ObserveRTP(1, 13, 0x0000012A, ms(3000)), "second jitter sample")
		raw, err = session.Report(ms(3100), 1500)
		c.Check(err == nil, "jitter report two: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 1)
		c.Check(block.Jitter == 87, "jitter after D=852 = 87, got %d", block.Jitter)

		// LSR is the middle 32 bits of the SR NTP timestamp; DLSR counts
		// 1/65536 seconds since the SR arrival.
		c.Check(!session.ObserveRTP(2, 1, 0, ms(900)), "lsr source probation one")
		c.Check(session.ObserveRTP(2, 2, 0, ms(950)), "lsr source confirmed")
		c.Check(session.ObserveRTCP(senderReportRaw(c, 2, 0x1122334455667788), time.Second) == nil, "SR received")
		raw, err = session.Report(ms(1100), 1500)
		c.Check(err == nil, "lsr report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 2)
		c.Check(block.LastSenderReport == 0x33445566, "LSR = %#x", block.LastSenderReport)
		c.Check(block.Delay == 6553, "DLSR for 100ms floors to 6553, got %d", block.Delay)

		// Reporting again recomputes the delay instead of clearing the SR.
		raw, err = session.Report(ms(1200), 1500)
		c.Check(err == nil, "second lsr report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 2)
		c.Check(block.LastSenderReport == 0x33445566, "LSR survives reporting, got %#x", block.LastSenderReport)
		c.Check(block.Delay == 13107, "DLSR for 200ms = 13107, got %d", block.Delay)

		// An SR whose middle 32 bits happen to be zero is still timed.
		c.Check(!session.ObserveRTP(3, 1, 0, ms(1900)), "zero-lsr source probation one")
		c.Check(session.ObserveRTP(3, 2, 0, ms(1950)), "zero-lsr source confirmed")
		c.Check(session.ObserveRTCP(senderReportRaw(c, 3, 0xAAAA00000000BBBB), 2*time.Second) == nil, "zero-LSR SR received")
		raw, err = session.Report(ms(2100), 1500)
		c.Check(err == nil, "zero-lsr report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 3)
		c.Check(block.LastSenderReport == 0, "zero LSR encoded as zero")
		c.Check(block.Delay == 6553, "zero LSR still produces a DLSR, got %d", block.Delay)

		// A source that never got an SR reports both fields as zero.
		c.Check(!session.ObserveRTP(4, 1, 0, ms(2200)), "no-sr source probation one")
		c.Check(session.ObserveRTP(4, 2, 0, ms(2250)), "no-sr source confirmed")
		raw, err = session.Report(ms(2300), 1500)
		c.Check(err == nil, "no-sr report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 4)
		c.Check(block.LastSenderReport == 0 && block.Delay == 0, "no SR means zero LSR/DLSR")

		// SRs for unknown sources never create state, and SRs arriving while a
		// source is still on trial are discarded.
		c.Check(session.ObserveRTCP(senderReportRaw(c, 999, 0x1122334455667788), ms(2400)) == nil, "unknown SR ignored")
		c.Check(!session.ObserveRTP(999, 1, 0, ms(2500)), "formerly unknown source on probation")
		c.Check(session.ObserveRTP(999, 2, 0, ms(2600)), "formerly unknown source confirmed")
		c.Check(!session.ObserveRTP(7, 1, 0, ms(2700)), "trial source probation one")
		c.Check(session.ObserveRTCP(senderReportRaw(c, 7, 0x1122334455667788), ms(2800)) == nil, "SR during probation ignored")
		c.Check(session.ObserveRTP(7, 2, 0, ms(2900)), "trial source confirmed")
		raw, err = session.Report(ms(3000), 1500)
		c.Check(err == nil, "ignored-sr report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, ok = findBlock(rr, 999)
		c.Check(ok && block.LastSenderReport == 0, "unknown SR was not stored")
		block, ok = findBlock(rr, 7)
		c.Check(ok && block.LastSenderReport == 0, "probation SR was not stored")

		// SR then BYE for the same SSRC in one datagram ends with it gone.
		c.Check(!session.ObserveRTP(8, 1, 0, ms(3100)), "sr-bye source probation one")
		c.Check(session.ObserveRTP(8, 2, 0, ms(3200)), "sr-bye source confirmed")
		srBye, err := Marshal([]Packet{
			&SenderReport{SSRC: 8, NTPTime: 0x1122334455667788},
			&Goodbye{Sources: []uint32{8}},
		})
		c.Check(err == nil, "marshal SR+BYE: %v", err)
		c.Check(session.ObserveRTCP(srBye, ms(3300)) == nil, "SR then BYE applied")
		// BYE then SR: the SR finds no source and the SSRC stays removed.
		c.Check(!session.ObserveRTP(9, 1, 0, ms(3400)), "bye-sr source probation one")
		c.Check(session.ObserveRTP(9, 2, 0, ms(3500)), "bye-sr source confirmed")
		byeSr, err := Marshal([]Packet{
			&Goodbye{Sources: []uint32{9}},
			&SenderReport{SSRC: 9, NTPTime: 0x1122334455667788},
		})
		c.Check(err == nil, "marshal BYE+SR: %v", err)
		c.Check(session.ObserveRTCP(byeSr, ms(3600)) == nil, "BYE then SR applied")
		raw, err = session.Report(ms(3700), 1500)
		c.Check(err == nil, "order report: %v", err)
		rr, _ = decodeCompound(c, raw)
		_, ok = findBlock(rr, 8)
		c.Check(!ok, "SR+BYE removed the source")
		_, ok = findBlock(rr, 9)
		c.Check(!ok, "BYE+SR left the source removed")

		// A malformed trailing packet invalidates the whole datagram: the
		// valid SR ahead of it must not have been applied.
		goodSR := senderReportRaw(c, 2, 0xAABBCCDDEEFF0011)
		badTail := make([]byte, len(goodSR)+4)
		copy(badTail, goodSR)
		copy(badTail[len(goodSR):], []byte{0x80, 0xc8, 0xff, 0xff})
		c.Check(session.ObserveRTCP(badTail, ms(3800)) != nil, "datagram with bad tail rejected")
		c.Check(session.ObserveRTCP([]byte{0x80, 0xc8, 0xff, 0xff}, ms(3900)) != nil, "lone malformed packet rejected")

		// A reduced-size RTCP datagram (a single feedback packet, no RR/SDES)
		// parses successfully and is simply ignored.
		pli, err := Marshal([]Packet{&PictureLossIndication{SenderSSRC: 50, MediaSSRC: 51}})
		c.Check(err == nil, "marshal PLI: %v", err)
		c.Check(session.ObserveRTCP(pli, ms(3950)) == nil, "reduced-size feedback accepted")
		raw, err = session.Report(ms(4000), 1500)
		c.Check(err == nil, "post-error report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, ok = findBlock(rr, 2)
		c.Check(ok && block.LastSenderReport == 0x33445566, "failed datagram did not partially commit, got %#x", block.LastSenderReport)

		// DLSR saturates once the scaled delay would overflow uint32.
		sat := testSession(c, "sat", 1000)
		c.Check(!sat.ObserveRTP(1, 1, 0, 0), "saturation source probation one")
		c.Check(sat.ObserveRTP(1, 2, 0, 0), "saturation source confirmed")
		c.Check(sat.ObserveRTCP(senderReportRaw(c, 1, 0x1122334455667788), 0) == nil, "SR at time zero")
		raw, err = sat.Report(70000*time.Second, 65536)
		c.Check(err == nil, "saturation report: %v", err)
		rr, _ = decodeCompound(c, raw)
		block, _ = findBlock(rr, 1)
		c.Check(block.Delay == math.MaxUint32, "DLSR saturates at uint32 max, got %d", block.Delay)
	})
}
