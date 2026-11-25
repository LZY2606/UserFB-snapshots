// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package rtcp

import (
	"testing"
	"time"

	"github.com/pion/rtcp/task_support/taskcheck"
)

// admitPair feeds the two consecutive packets that finish a fresh
// MIN_SEQUENTIAL trial, checking both ObserveRTP verdicts.
func admitPair(c *taskcheck.C, s *ReceptionSession, ssrc uint32, first uint16) {
	c.Check(!s.ObserveRTP(ssrc, first, uint32(first)*20, time.Millisecond),
		"ssrc %d: trial opener must be rejected", ssrc)
	c.Check(s.ObserveRTP(ssrc, first+1, uint32(first+1)*20, 2*time.Millisecond),
		"ssrc %d: second consecutive packet must be accepted", ssrc)
}

// decodeReport parses a compound report and returns its RR and SDES packets.
func decodeReport(c *taskcheck.C, data []byte) (*ReceiverReport, *SourceDescription) {
	var cp CompoundPacket
	err := cp.Unmarshal(data)
	c.Check(err == nil, "compound output must unmarshal: %v", err)
	c.Check(len(cp) == 2, "compound must hold RR+SDES, got %d packets", len(cp))

	rr, ok := cp[0].(*ReceiverReport)
	c.Check(ok, "first compound packet must be a ReceiverReport")
	sdes, ok := cp[1].(*SourceDescription)
	c.Check(ok, "second compound packet must be a SourceDescription")
	return rr, sdes
}

func findBlock(rr *ReceiverReport, ssrc uint32) (ReceptionReport, bool) {
	for _, block := range rr.Reports {
		if block.SSRC == ssrc {
			return block, true
		}
	}
	return ReceptionReport{}, false
}

func marshalPacket(c *taskcheck.C, pkt Packet) []byte {
	raw, err := pkt.Marshal()
	c.Check(err == nil, "marshal helper packet: %v", err)
	return raw
}

func TestReceptionSequence(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Constructor validation.
		_, err := NewReceptionSession(1, "x", 0)
		c.Check(err != nil, "zero clock rate must be rejected")
		_, err = NewReceptionSession(1, "", 90000)
		c.Check(err != nil, "empty CNAME must be rejected")
		_, err = NewReceptionSession(1, string(make([]byte, 256)), 90000)
		c.Check(err != nil, "256-byte CNAME must be rejected")
		session, err := NewReceptionSession(4242, string(make([]byte, 255)), 90000)
		c.Check(err == nil, "255-byte CNAME must be accepted: %v", err)
		c.Check(session != nil, "session must be constructed")

		s, err := NewReceptionSession(4242, "receiver", 1000)
		c.Check(err == nil, "session construction: %v", err)

		// Source 1: probation base, then a duplicate.
		admitPair(c, s, 1, 10)
		c.Check(s.ObserveRTP(1, 11, 220, 3*time.Millisecond),
			"duplicate of the highest sequence number must be accepted")
		block1, _ := func() (ReceptionReport, bool) {
			data, rerr := s.Report(10*time.Millisecond, 1<<16)
			c.Check(rerr == nil, "first report: %v", rerr)
			rr, _ := decodeReport(c, data)
			c.Check(data[0]&0xC0 == 0x80, "RTP version 2 in raw header, got %#x", data[0])
			c.Check(data[1] == byte(TypeReceiverReport), "raw packet type is RR, got %d", data[1])
			c.Check(int(data[0]&0x1F) == len(rr.Reports), "raw RC matches block count")
			c.Check(len(data) <= 1<<16, "output within budget")
			return findBlock(rr, 1)
		}()
		c.Check(block1.LastSequenceNumber == 11, "duplicate must not advance max seq, got %d", block1.LastSequenceNumber)

		// Source 2: a broken trial restarts.
		c.Check(!s.ObserveRTP(2, 20, 400, time.Millisecond), "trial opener rejected")
		c.Check(!s.ObserveRTP(2, 22, 440, 2*time.Millisecond), "non-consecutive packet breaks and restarts trial")
		c.Check(s.ObserveRTP(2, 23, 460, 3*time.Millisecond), "rebuilt trial completes on next consecutive packet")

		// Source 3: cycle wrap, then a late packet from the old cycle.
		c.Check(!s.ObserveRTP(3, 65534, 0, time.Millisecond), "wrap source opener rejected")
		c.Check(s.ObserveRTP(3, 65535, 20, 2*time.Millisecond), "wrap source base accepted")
		c.Check(s.ObserveRTP(3, 0, 40, 3*time.Millisecond), "seq 0 after wrap accepted")
		c.Check(s.ObserveRTP(3, 65535, 60, 4*time.Millisecond), "late duplicate from old cycle accepted")

		// Source 4: a large jump is rejected until the next sequence confirms
		// a restart; an SR recorded beforehand must be cleared.
		admitPair(c, s, 4, 100)
		sr := marshalPacket(c, &SenderReport{SSRC: 4, NTPTime: 0x1122334455667788})
		c.Check(s.ObserveRTCP(sr, 5*time.Millisecond) == nil, "SR before restart accepted")
		c.Check(!s.ObserveRTP(4, 5000, 1000, 6*time.Millisecond), "large jump rejected on sight")
		c.Check(!s.ObserveRTP(4, 5002, 1040, 7*time.Millisecond), "non-confirming packet stays rejected")
		c.Check(s.ObserveRTP(4, 5001, 1020, 8*time.Millisecond), "next sequence confirms the restart")

		data, err := s.Report(20*time.Millisecond, 1<<16)
		c.Check(err == nil, "report after restart: %v", err)
		rr, sdes := decodeReport(c, data)
		c.Check(rr.SSRC == 4242, "RR carries local SSRC")
		c.Check(len(rr.Reports) == 4, "four ready sources reported, got %d", len(rr.Reports))

		block2, ok := findBlock(rr, 2)
		c.Check(ok && block2.LastSequenceNumber == 23 && block2.TotalLost == 0,
			"restarted trial source block: %+v ok=%v", block2, ok)

		block3, ok := findBlock(rr, 3)
		c.Check(ok && block3.LastSequenceNumber == 65536,
			"wrap source extended max seq is 65536, got %d", block3.LastSequenceNumber)
		c.Check(block3.TotalLost == 0xFFFFFF,
			"negative cumulative lost uses low-24 two's complement, got %#x", block3.TotalLost)

		block4, ok := findBlock(rr, 4)
		c.Check(ok && block4.LastSequenceNumber == 5001,
			"restarted source resets cycles, ext seq %d", block4.LastSequenceNumber)
		c.Check(block4.Jitter == 0 && block4.LastSenderReport == 0 && block4.Delay == 0,
			"restart clears jitter and SR state: %+v", block4)

		cname := sdes.Chunks[0].Items[0].Text
		c.Check(cname == "receiver", "SDES carries local CNAME %q", cname)

		// Source 5: BYE removes it, then the same SSRC restarts as a fresh trial.
		admitPair(c, s, 5, 10)
		bye := marshalPacket(c, &Goodbye{Sources: []uint32{5}})
		c.Check(s.ObserveRTCP(bye, 9*time.Millisecond) == nil, "standalone reduced-size BYE accepted")
		data, err = s.Report(21*time.Millisecond, 1<<16)
		c.Check(err == nil, "report after BYE: %v", err)
		rr, _ = decodeReport(c, data)
		_, ok = findBlock(rr, 5)
		c.Check(!ok, "BYE source must be absent from the report")

		c.Check(!s.ObserveRTP(5, 999, 0, 22*time.Millisecond), "reborn SSRC goes through probation again")
		c.Check(s.ObserveRTP(5, 1000, 20, 23*time.Millisecond), "reborn SSRC finishes probation")
		data, err = s.Report(24*time.Millisecond, 1<<16)
		c.Check(err == nil, "report after rebirth: %v", err)
		rr, _ = decodeReport(c, data)
		block5, ok := findBlock(rr, 5)
		c.Check(ok && block5.LastSequenceNumber == 1000 && block5.TotalLost == 0,
			"reborn source reports fresh base: %+v ok=%v", block5, ok)
	})
}

func TestReceptionIntervals(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Appendix A.3 loss progression on a single source.
		s, err := NewReceptionSession(8, "int", 1000)
		c.Check(err == nil, "session: %v", err)
		admitPair(c, s, 7, 0) // base seq 1, received 1, extended max 1
		c.Check(s.ObserveRTP(7, 3, 60, 3*time.Millisecond), "seq 3 accepted with seq 2 lost")

		data, err := s.Report(time.Second, 1<<16)
		c.Check(err == nil, "first report: %v", err)
		rr, _ := decodeReport(c, data)
		b, ok := findBlock(rr, 7)
		c.Check(ok && b.FractionLost == 0 && b.TotalLost == 1,
			"first interval reports no fraction, one total lost: %+v", b)

		c.Check(s.ObserveRTP(7, 4, 80, 4*time.Millisecond), "seq 4")
		c.Check(s.ObserveRTP(7, 5, 100, 5*time.Millisecond), "seq 5")
		data, err = s.Report(time.Second, 1<<16)
		c.Check(err == nil, "second report: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 7)
		c.Check(b.FractionLost == 0 && b.TotalLost == 1,
			"loss-free interval keeps fraction zero: %+v", b)

		c.Check(s.ObserveRTP(7, 8, 160, 6*time.Millisecond), "seq 8 with 6 and 7 lost")
		data, err = s.Report(time.Second, 1<<16)
		c.Check(err == nil, "third report: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 7)
		c.Check(b.FractionLost == 170 && b.TotalLost == 3,
			"2 lost of 3 expected gives fraction 170 and total 3: %+v", b)

		data, err = s.Report(time.Second, 1<<16)
		c.Check(err == nil, "fourth report: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 7)
		c.Check(b.FractionLost == 0 && b.TotalLost == 3,
			"quiet interval reports zero fraction: %+v", b)

		// A failing report must not advance the interval baseline.
		s2, err := NewReceptionSession(8, "retry", 1000)
		c.Check(err == nil, "session: %v", err)
		admitPair(c, s2, 7, 0)
		_, err = s2.Report(time.Second, 1<<16)
		c.Check(err == nil, "baseline report succeeds")
		c.Check(s2.ObserveRTP(7, 3, 60, 3*time.Millisecond), "loss introduced after baseline")
		_, err = s2.Report(time.Second, 8) // below even the empty compound size
		c.Check(err != nil, "undersized budget must error")
		data, err = s2.Report(time.Second, 1<<16)
		c.Check(err == nil, "retry report succeeds: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 7)
		c.Check(b.FractionLost == 128,
			"failed report kept the baseline; fraction is 128, got %d", b.FractionLost)

		// Round-robin across more than the 31-block RR limit.
		s3, err := NewReceptionSession(8, "rotate", 1000)
		c.Check(err == nil, "session: %v", err)
		const sourceCount = 33
		for ssrc := uint32(1); ssrc <= sourceCount; ssrc++ {
			admitPair(c, s3, ssrc, 0)
		}
		// "rotate" is 6 bytes: derive the empty-compound base size from the
		// real marshal sizes so the padded SDES alignment is accounted for.
		baseSize := (&ReceiverReport{SSRC: 8}).MarshalSize() +
			NewCNAMESourceDescription(8, "rotate").MarshalSize()
		twoBlockBudget := baseSize + 2*receptionReportLength
		data, err = s3.Report(time.Second, 1<<16)
		c.Check(err == nil, "rotation report 1: %v", err)
		rr, _ = decodeReport(c, data)
		c.Check(len(rr.Reports) == 31, "first RR carries 31 blocks, got %d", len(rr.Reports))
		for i, block := range rr.Reports {
			c.Check(block.SSRC == uint32(i+1), "first round selects ascending SSRCs, pos %d got %d", i, block.SSRC)
		}

		data, err = s3.Report(time.Second, twoBlockBudget)
		c.Check(err == nil, "rotation report 2: %v", err)
		rr, _ = decodeReport(c, data)
		c.Check(len(rr.Reports) == 2, "budget-limited RR carries two blocks, got %d", len(rr.Reports))
		c.Check(rr.Reports[0].SSRC == 32 && rr.Reports[1].SSRC == 33,
			"second round continues strictly after the last cursor: %d %d",
			rr.Reports[0].SSRC, rr.Reports[1].SSRC)
		c.Check(len(data) <= twoBlockBudget, "limited output stays within budget")

		data, err = s3.Report(time.Second, 1<<16)
		c.Check(err == nil, "rotation report 3: %v", err)
		rr, _ = decodeReport(c, data)
		c.Check(len(rr.Reports) == 31 && rr.Reports[0].SSRC == 1 && rr.Reports[30].SSRC == 31,
			"third round cycles back to the lowest SSRC")

		// CNAME alignment consumes budget; sources skipped by budget keep
		// their interval baseline unset.
		s4, err := NewReceptionSession(8, "abc", 1000) // 3-byte CNAME -> padded SDES chunk
		c.Check(err == nil, "session: %v", err)
		sdesSize := NewCNAMESourceDescription(8, "abc").MarshalSize()
		c.Check(sdesSize == 16, "SDES with 3-byte CNAME pads to 16 bytes, got %d", sdesSize)
		admitPair(c, s4, 10, 0)
		admitPair(c, s4, 20, 0)
		// base = empty RR (8) + SDES (16) = 24; one 24-byte block needs 48.
		_, err = s4.Report(time.Second, 47)
		c.Check(err != nil, "ready source but no block fits: budget must error")
		data, err = s4.Report(time.Second, 48)
		c.Check(err == nil, "exact one-block budget succeeds: %v", err)
		c.Check(len(data) == 48, "compound is exactly 48 bytes, got %d", len(data))
		rr, _ = decodeReport(c, data)
		c.Check(len(rr.Reports) == 1 && rr.Reports[0].SSRC == 10,
			"only the first source fits the budget")

		c.Check(s4.ObserveRTP(10, 3, 60, 3*time.Millisecond), "loss on selected source")
		c.Check(s4.ObserveRTP(20, 3, 60, 3*time.Millisecond), "loss on unselected source")
		data, err = s4.Report(time.Second, 1<<16)
		c.Check(err == nil, "full-budget report: %v", err)
		rr, _ = decodeReport(c, data)
		b10, ok := findBlock(rr, 10)
		c.Check(ok && b10.FractionLost == 128, "reported source keeps its interval: %+v", b10)
		b20, ok := findBlock(rr, 20)
		c.Check(ok && b20.FractionLost == 0 && b20.TotalLost == 1,
			"skipped source has no interval baseline yet: %+v", b20)

		// No ready sources: an empty RR is allowed; below the base size it errors.
		s5, err := NewReceptionSession(8, "empty", 1000)
		c.Check(err == nil, "session: %v", err)
		emptySDES := NewCNAMESourceDescription(8, "empty").MarshalSize()
		data, err = s5.Report(time.Second, 8+emptySDES)
		c.Check(err == nil, "empty RR report: %v", err)
		rr, sdes := decodeReport(c, data)
		c.Check(len(rr.Reports) == 0, "empty RR has zero blocks")
		c.Check(len(sdes.Chunks) == 1, "SDES still attached to empty RR")
		_, err = s5.Report(time.Second, 7)
		c.Check(err != nil, "budget below base size must error even with no sources")
	})
}

func TestReceptionClock(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		s, err := NewReceptionSession(99, "clock", 1000) // one RTP tick per millisecond
		c.Check(err == nil, "session: %v", err)

		// Jitter: perfect stream, one 50 ms surge, recovery, then a timestamp
		// wrap that must not look like a jump.
		c.Check(!s.ObserveRTP(1, 0, 0xFFFFFFA0, 20*time.Millisecond), "opener")
		c.Check(s.ObserveRTP(1, 1, 0xFFFFFFB4, 40*time.Millisecond), "base packet")
		c.Check(s.ObserveRTP(1, 2, 0xFFFFFFC8, 70*time.Millisecond), "10 ms deviation")
		c.Check(s.ObserveRTP(1, 3, 0xFFFFFFDC, 130*time.Millisecond), "50 ms deviation")
		c.Check(s.ObserveRTP(1, 4, 0xFFFFFFF0, 150*time.Millisecond), "recovery packet")
		c.Check(s.ObserveRTP(1, 5, 0x00000004, 170*time.Millisecond), "timestamp wraps 2^32")

		data, err := s.Report(200*time.Millisecond, 1<<16)
		c.Check(err == nil, "jitter report: %v", err)
		rr, _ := decodeReport(c, data)
		b, ok := findBlock(rr, 1)
		c.Check(ok && b.Jitter == 2, "wrap-safe Q4 jitter estimate is 2, got %d", b.Jitter)
		c.Check(b.LastSenderReport == 0 && b.Delay == 0, "no SR means zero LSR/DLSR")

		// SR timing: LSR is the middle 32 NTP bits; DLSR floors 1/65536 s units.
		admitPair(c, s, 2, 0)
		sr := marshalPacket(c, &SenderReport{SSRC: 2, NTPTime: 0x1122334455667788})
		c.Check(s.ObserveRTCP(sr, time.Second) == nil, "SR observed")
		data, err = s.Report(2*time.Second, 1<<16)
		c.Check(err == nil, "SR report: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 2)
		c.Check(b.LastSenderReport == 0x33445566, "LSR takes NTP middle 32 bits, got %#x", b.LastSenderReport)
		c.Check(b.Delay == 65536, "one second delay scales to 65536, got %d", b.Delay)

		// A later report floors fractional DLSR (10 ms -> 655.36 -> 655).
		data, err = s.Report(time.Second+10*time.Millisecond, 1<<16)
		c.Check(err == nil, "fractional DLSR report: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 2)
		c.Check(b.Delay == 655, "DLSR floors toward zero, got %d", b.Delay)

		// An SR whose middle 32 bits happen to be zero still times DLSR.
		zeroSR := marshalPacket(c, &SenderReport{SSRC: 2, NTPTime: 0x0000000000001000})
		c.Check(s.ObserveRTCP(zeroSR, 3*time.Second) == nil, "zero-LSR SR observed")
		data, err = s.Report(3*time.Second+500*time.Millisecond, 1<<16)
		c.Check(err == nil, "zero-LSR report: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 2)
		c.Check(b.LastSenderReport == 0 && b.Delay == 32768,
			"zero LSR still produces a DLSR: %+v", b)

		// DLSR saturates at uint32 max (a full day exceeds the field range).
		admitPair(c, s, 3, 0)
		daySR := marshalPacket(c, &SenderReport{SSRC: 3, NTPTime: 0xAABBCCDD00000000})
		c.Check(s.ObserveRTCP(daySR, 0) == nil, "SR at time zero")
		data, err = s.Report(24*time.Hour, 1<<16)
		c.Check(err == nil, "saturation report: %v", err)
		rr, _ = decodeReport(c, data)
		b, _ = findBlock(rr, 3)
		c.Check(b.Delay == ^uint32(0), "DLSR saturates to uint32 max, got %d", b.Delay)

		// SRs never create state for unknown or unprobated sources.
		unknownSR := marshalPacket(c, &SenderReport{SSRC: 999, NTPTime: 0x1122334455667788})
		c.Check(s.ObserveRTCP(unknownSR, time.Second) == nil, "unknown SR ignored without error")
		c.Check(!s.ObserveRTP(4, 7, 0, time.Millisecond), "source 4 stuck in probation")
		probSR := marshalPacket(c, &SenderReport{SSRC: 4, NTPTime: 0x1122334455667788})
		c.Check(s.ObserveRTCP(probSR, time.Second) == nil, "SR during probation ignored")
		data, err = s.Report(24*time.Hour, 1<<16)
		c.Check(err == nil, "report after ignored SRs: %v", err)
		rr, _ = decodeReport(c, data)
		_, ok = findBlock(rr, 999)
		c.Check(!ok, "unknown SR source must not appear")
		_, ok = findBlock(rr, 4)
		c.Check(!ok, "probation source must not appear")

		// SR then BYE in one datagram: ordered processing removes the source,
		// so its SR never reaches a report.
		admitPair(c, s, 5, 0)
		ordered, err := Marshal([]Packet{
			&SenderReport{SSRC: 5, NTPTime: 0x1122334455667788},
			&Goodbye{Sources: []uint32{5}},
		})
		c.Check(err == nil, "marshal SR+BYE compound: %v", err)
		c.Check(s.ObserveRTCP(ordered, time.Second) == nil, "SR+BYE compound accepted")
		data, err = s.Report(2*time.Second, 1<<16)
		c.Check(err == nil, "report after SR+BYE: %v", err)
		rr, _ = decodeReport(c, data)
		_, ok = findBlock(rr, 5)
		c.Check(!ok, "BYE after SR still removes the source")

		// A bad tail packet invalidates the whole datagram: the leading SR is
		// not partially committed.
		admitPair(c, s, 6, 0)
		goodSR := marshalPacket(c, &SenderReport{SSRC: 6, NTPTime: 0x1122334455667788})
		badTail := append(goodSR, 0x80, byte(TypeReceiverReport), 0x00, 0x10) // claims 68 trailing bytes
		c.Check(s.ObserveRTCP(badTail, time.Second) != nil, "truncated tail must fail parsing")
		c.Check(s.ObserveRTCP([]byte{0x00}, time.Second) != nil, "garbage datagram must fail parsing")
		data, err = s.Report(2*time.Second, 1<<16)
		c.Check(err == nil, "report after failed datagrams: %v", err)
		rr, _ = decodeReport(c, data)
		b, ok = findBlock(rr, 6)
		c.Check(ok && b.LastSenderReport == 0 && b.Delay == 0,
			"failed datagram committed no SR state: %+v", b)
	})
}
