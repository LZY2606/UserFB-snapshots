// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package gcc

import (
	"errors"
	"fmt"
	"io"
	"strings"
	"testing"
	"time"

	"github.com/pion/interceptor"
	"github.com/pion/interceptor/pkg/twcc"
	"github.com/pion/interceptor/task_support/taskcheck"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
)

// probeClock is a replaceable monotonic clock for manual probe scheduling.
type probeClock struct {
	now time.Time
}

func (p *probeClock) Now() time.Time { return p.now }

// probeRecord is one writer call captured by probeRecorder.
type probeRecord struct {
	header rtp.Header
	wire   []byte
	n      int
	err    error
}

// probeRecorder is an in-memory interceptor.RTPWriter that records the real
// RTP wire bytes of every call.
type probeRecorder struct {
	tag     string
	order   *[]string
	records []probeRecord
	failAt  map[int]error
	shortAt map[int]int
	onWrite func(call int)
}

func (w *probeRecorder) Write(header *rtp.Header, payload []byte, _ interceptor.Attributes) (int, error) {
	call := len(w.records)
	wire, err := (&rtp.Packet{Header: *header, Payload: payload}).Marshal()
	if err != nil {
		return 0, err
	}
	if w.order != nil {
		*w.order = append(*w.order, w.tag)
	}
	if w.onWrite != nil {
		w.onWrite(call)
	}
	record := probeRecord{header: *header, wire: wire, n: len(wire)}
	if werr, ok := w.failAt[call]; ok {
		record.n, record.err = 0, werr
		w.records = append(w.records, record)

		return 0, werr
	}
	if short, ok := w.shortAt[call]; ok {
		record.n = short
	}
	w.records = append(w.records, record)

	return record.n, nil
}

// probePacket unmarshals recorded wire bytes.
func probePacket(c *taskcheck.C, wire []byte) rtp.Packet {
	var pkt rtp.Packet
	err := pkt.Unmarshal(wire)
	c.Check(err == nil, "unmarshal recorded packet: %v", err)

	return pkt
}

// probeTSeq reads the transport-wide sequence number of a recorded packet.
func probeTSeq(c *taskcheck.C, wire []byte, extID uint8) uint16 {
	pkt := probePacket(c, wire)
	var ext rtp.TransportCCExtension
	err := ext.Unmarshal(pkt.GetExtension(extID))
	c.Check(err == nil, "unmarshal twcc extension: %v", err)

	return ext.TransportSequence
}

// probeRate mirrors the documented cluster rate formula.
func probeRate(totalBytes, lastBytes int, span time.Duration) int {
	return int(8 * int64(totalBytes-lastBytes) * int64(time.Second) / span.Nanoseconds())
}

// probeAverageLoss reads the loss controller state that WriteRTCP updates
// synchronously (GetStats only refreshes via the async delay pipeline).
func probeAverageLoss(bwe *SendSideBWE) float64 {
	bwe.lossController.lock.Lock()
	defer bwe.lossController.lock.Unlock()

	return bwe.lossController.averageLoss
}

// probeRunFeedback builds a feedback reporting count consecutive received
// packets starting at base, each delta microseconds apart.
func probeRunFeedback(base, count uint16, refTime uint32, delta int64) *rtcp.TransportLayerCC {
	deltas := make([]*rtcp.RecvDelta, 0, count)
	for i := uint16(0); i < count; i++ {
		deltas = append(deltas, &rtcp.RecvDelta{Type: rtcp.TypeTCCPacketReceivedSmallDelta, Delta: delta})
	}

	return &rtcp.TransportLayerCC{
		SenderSSRC:         99,
		MediaSSRC:          200,
		BaseSequenceNumber: base,
		PacketStatusCount:  count,
		ReferenceTime:      refTime,
		PacketChunks: []rtcp.PacketStatusChunk{&rtcp.RunLengthChunk{
			Type:               rtcp.TypeTCCRunLengthChunk,
			PacketStatusSymbol: rtcp.TypeTCCPacketReceivedSmallDelta,
			RunLength:          count,
		}},
		RecvDeltas: deltas,
	}
}

// probeVectorFeedback builds a feedback with an explicit per-packet
// received(1)/not-received(0) symbol list starting at base.
func probeVectorFeedback(base uint16, refTime uint32, symbols []uint16, delta int64) *rtcp.TransportLayerCC {
	deltas := make([]*rtcp.RecvDelta, 0, len(symbols))
	for _, s := range symbols {
		if s == rtcp.TypeTCCPacketReceivedSmallDelta {
			deltas = append(deltas, &rtcp.RecvDelta{Type: rtcp.TypeTCCPacketReceivedSmallDelta, Delta: delta})
		}
	}

	return &rtcp.TransportLayerCC{
		SenderSSRC:         99,
		MediaSSRC:          200,
		BaseSequenceNumber: base,
		PacketStatusCount:  uint16(len(symbols)), //nolint:gosec // G115
		ReferenceTime:      refTime,
		PacketChunks: []rtcp.PacketStatusChunk{&rtcp.StatusVectorChunk{
			Type:       rtcp.TypeTCCStatusVectorChunk,
			SymbolSize: rtcp.TypeTCCSymbolSizeOneBit,
			SymbolList: symbols,
		}},
		RecvDeltas: deltas,
	}
}

// probeLostFeedback builds a feedback reporting count not-received packets.
func probeLostFeedback(base, count uint16, refTime uint32) *rtcp.TransportLayerCC {
	return &rtcp.TransportLayerCC{
		SenderSSRC:         99,
		MediaSSRC:          200,
		BaseSequenceNumber: base,
		PacketStatusCount:  count,
		ReferenceTime:      refTime,
		PacketChunks: []rtcp.PacketStatusChunk{&rtcp.RunLengthChunk{
			Type:               rtcp.TypeTCCRunLengthChunk,
			PacketStatusSymbol: rtcp.TypeTCCPacketNotReceived,
			RunLength:          count,
		}},
	}
}

// probeWriteFeedback round-trips the feedback through the wire format and
// hands the decoded TransportLayerCC to WriteRTCP.
func probeWriteFeedback(c *taskcheck.C, bwe *SendSideBWE, fb *rtcp.TransportLayerCC) {
	fb.Header = rtcp.Header{Type: rtcp.TypeTransportSpecificFeedback, Count: rtcp.FormatTCC}
	fb.Header.Length = uint16(fb.MarshalSize()/4 - 1) //nolint:gosec // G115
	wire, err := fb.Marshal()
	c.Check(err == nil, "marshal feedback: %v", err)
	pkts, err := rtcp.Unmarshal(wire)
	c.Check(err == nil && len(pkts) == 1, "unmarshal feedback: %v (packets %d)", err, len(pkts))
	err = bwe.WriteRTCP(pkts, nil)
	c.Check(err == nil, "WriteRTCP: %v", err)
}

func probeMediaInfo() *interceptor.StreamInfo {
	return &interceptor.StreamInfo{
		SSRC:                100,
		PayloadType:         96,
		ClockRate:           90000,
		RTPHeaderExtensions: []interceptor.RTPHeaderExtension{{URI: transportCCURI, ID: 1}},
	}
}

func TestProbeSchedule(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t0 := time.Unix(1000, 0)
		clock := &probeClock{now: t0}
		hdrExt := &twcc.HeaderExtensionInterceptor{}
		pacer := NewLeakyBucketPacer(1_000_000, LeakyBucketPacerManualTicks())
		bwe, err := NewSendSideBWE(SendSideBWEPacer(pacer))
		c.Check(err == nil, "NewSendSideBWE: %v", err)
		defer func() { _ = bwe.Close() }()

		order := []string{}
		mediaSink := &probeRecorder{tag: "media", order: &order}
		probeSink := &probeRecorder{tag: "probe", order: &order}
		mediaWriter := hdrExt.BindLocalStream(probeMediaInfo(), bwe.AddStream(probeMediaInfo(), mediaSink))

		err = bwe.EnableProbeStream(ProbeStreamConfig{
			SSRC: 200, PayloadType: 127, ClockRate: 90000,
			InitialSequenceNumber: 60000,
			RTPTimeBase:           0xfffffff0,
			LocalTimeBase:         t0,
			TWCCExtensionID:       1,
			HeaderExtension:       hdrExt,
			Writer:                probeSink,
			Now:                   clock.Now,
		})
		c.Check(err == nil, "EnableProbeStream: %v", err)

		at := func(tm time.Time) { clock.now = tm; pacer.Tick(tm) }

		// wire = 20 + 100 = 120 bytes, interval = ceil(8*120*1e9/960000) = 1ms.
		clusterID, err := bwe.StartProbe(ProbeConfig{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 4, Timeout: time.Second})
		c.Check(err == nil, "StartProbe: %v", err)
		c.Check(clusterID == 1, "first cluster ID = %d, want 1", clusterID)

		for i := 0; i < 2; i++ {
			_, werr := mediaWriter.Write(&rtp.Header{
				Version: 2, PayloadType: 96, SSRC: 100,
				SequenceNumber: uint16(i), Timestamp: uint32(i * 900), //nolint:gosec // G115
			}, []byte{0x61, byte(i)}, nil)
			c.Check(werr == nil, "media write %d: %v", i, werr)
		}

		// The first probe packet is due right at the start; the first manual
		// tick carries no media budget yet.
		at(t0)
		c.Check(len(probeSink.records) == 1, "probe packets at start = %d, want 1", len(probeSink.records))
		c.Check(len(mediaSink.records) == 0, "media drained without budget = %d, want 0", len(mediaSink.records))

		// Same tick: media budget first, then one due probe packet.
		at(t0.Add(10 * time.Millisecond))
		c.Check(len(mediaSink.records) == 2, "media drained = %d, want 2", len(mediaSink.records))
		c.Check(len(probeSink.records) == 2, "probe packets = %d, want 2", len(probeSink.records))
		c.Check(strings.Join(order, ",") == "probe,media,media,probe", "tick order = %v", order)

		// Media keeps FIFO order and its own timestamps; media and probe share
		// one transport sequence space.
		c.Check(mediaSink.records[0].header.SequenceNumber == 0, "media seq = %d", mediaSink.records[0].header.SequenceNumber)
		c.Check(mediaSink.records[1].header.SequenceNumber == 1, "media seq = %d", mediaSink.records[1].header.SequenceNumber)
		c.Check(mediaSink.records[0].header.Timestamp == 0, "media ts = %d", mediaSink.records[0].header.Timestamp)
		c.Check(mediaSink.records[1].header.Timestamp == 900, "media ts = %d", mediaSink.records[1].header.Timestamp)
		c.Check(probeTSeq(c, mediaSink.records[0].wire, 1) == 0, "media tseq = %d", probeTSeq(c, mediaSink.records[0].wire, 1))
		c.Check(probeTSeq(c, mediaSink.records[1].wire, 1) == 1, "media tseq = %d", probeTSeq(c, mediaSink.records[1].wire, 1))
		c.Check(probeTSeq(c, probeSink.records[0].wire, 1) == 2, "probe tseq = %d", probeTSeq(c, probeSink.records[0].wire, 1))
		c.Check(probeTSeq(c, probeSink.records[1].wire, 1) == 3, "probe tseq = %d", probeTSeq(c, probeSink.records[1].wire, 1))

		// Probe wire format: RTP v2, marker false, TWCC extension and padding
		// only, padding length byte at the end, 20 + padding wire bytes.
		wire0 := probeSink.records[0].wire
		pkt0 := probePacket(c, wire0)
		c.Check(len(wire0) == 120, "wire length = %d, want 120", len(wire0))
		c.Check(wire0[len(wire0)-1] == 100, "padding length byte = %d, want 100", wire0[len(wire0)-1])
		c.Check(pkt0.Header.Version == 2, "version = %d", pkt0.Header.Version)
		c.Check(!pkt0.Header.Marker, "marker must be false")
		c.Check(pkt0.Header.Padding, "padding bit must be set")
		c.Check(pkt0.Header.PayloadType == 127, "payload type = %d", pkt0.Header.PayloadType)
		c.Check(pkt0.Header.SSRC == 200, "ssrc = %d", pkt0.Header.SSRC)
		c.Check(pkt0.Header.SequenceNumber == 60000, "rtp seq = %d", pkt0.Header.SequenceNumber)
		c.Check(pkt0.PaddingSize == 100 && len(pkt0.Payload) == 0, "padding only: size=%d payload=%d", pkt0.PaddingSize, len(pkt0.Payload))
		c.Check(pkt0.Header.Timestamp == 0xfffffff0, "first ts = %#x", pkt0.Header.Timestamp)

		// RTP timestamps follow the base plus the local clock, mod 2^32.
		wantTS := func(d time.Duration) uint32 {
			return uint32(uint64(0xfffffff0) + uint64(d.Nanoseconds())*90000/uint64(time.Second)) //nolint:gosec // G115
		}
		pkt1 := probePacket(c, probeSink.records[1].wire)
		c.Check(pkt1.Header.Timestamp == wantTS(10*time.Millisecond), "wrapped ts = %#x, want %#x", pkt1.Header.Timestamp, wantTS(10*time.Millisecond))
		c.Check(pkt1.Header.SequenceNumber == 60001, "rtp seq = %d", pkt1.Header.SequenceNumber)

		// Not yet due: the next attempt is one interval after the actual one.
		at(t0.Add(10*time.Millisecond + 500*time.Microsecond))
		c.Check(len(probeSink.records) == 2, "early tick sent = %d, want 2", len(probeSink.records))

		// Late wakeup: exactly one packet, no catch-up burst; the interval
		// restarts from this actual attempt.
		at(t0.Add(16*time.Millisecond + 500*time.Microsecond))
		c.Check(len(probeSink.records) == 3, "late wakeup sent = %d, want 3", len(probeSink.records))
		at(t0.Add(17*time.Millisecond + 400*time.Microsecond))
		c.Check(len(probeSink.records) == 3, "not due yet = %d, want 3", len(probeSink.records))
		at(t0.Add(17*time.Millisecond + 500*time.Microsecond))
		c.Check(len(probeSink.records) == 4, "due = %d, want 4", len(probeSink.records))
		pkt2 := probePacket(c, probeSink.records[2].wire)
		c.Check(pkt2.Header.Timestamp == wantTS(16*time.Millisecond+500*time.Microsecond), "ts = %#x", pkt2.Header.Timestamp)
		c.Check(pkt2.Header.SequenceNumber == 60002, "rtp seq = %d", pkt2.Header.SequenceNumber)

		// All planned attempts done: no more packets, no result yet.
		at(t0.Add(20 * time.Millisecond))
		c.Check(len(probeSink.records) == 4, "after last attempt = %d, want 4", len(probeSink.records))
		_, ok := bwe.LatestProbeResult()
		c.Check(!ok, "no result before the cluster ends")

		// The timeout boundary ends the cluster.
		at(t0.Add(time.Second))
		result, ok := bwe.LatestProbeResult()
		c.Check(ok && result.Status == ProbeStatusTimeout, "timeout result = %+v (ok=%v)", result, ok)
		c.Check(result.ClusterID == 1 && result.Attempts == 4 && result.Sent == 4 && result.Received == 0, "counts = %+v", result)

		// Failed and short writes consume RTP sequence numbers but are never
		// recorded as sent.
		errWrite := errors.New("probe write failed")
		probeSink.failAt = map[int]error{5: errWrite}
		probeSink.shortAt = map[int]int{6: 119}

		s0 := t0.Add(time.Second)
		clock.now = s0
		clusterID, err = bwe.StartProbe(ProbeConfig{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 6, Timeout: 500 * time.Millisecond})
		c.Check(err == nil && clusterID == 2, "StartProbe 2: %v id=%d", err, clusterID)
		for k := 0; k < 6; k++ {
			at(s0.Add(time.Duration(k) * time.Millisecond))
		}
		c.Check(len(probeSink.records) == 10, "attempts = %d, want 10", len(probeSink.records))
		sent := 0
		seqs := []uint16{}
		for _, r := range probeSink.records[4:] {
			seqs = append(seqs, r.header.SequenceNumber)
			if r.err == nil && r.n >= 120 {
				sent++
			}
		}
		c.Check(sent == 4, "successful writes = %d, want 4", sent)
		c.Check(fmt.Sprint(seqs) == "[60004 60005 60006 60007 60008 60009]", "failed sequences not rolled back: %v", seqs)

		// Only the four successfully written packets can be reported received;
		// four is below the threshold of five for six planned packets.
		clock.now = s0.Add(6 * time.Millisecond)
		probeWriteFeedback(c, bwe, probeVectorFeedback(6, 100, []uint16{1, 0, 0, 1, 1, 1}, 250))
		result, _ = bwe.LatestProbeResult()
		c.Check(result.ClusterID == 1, "cluster 2 must not succeed below threshold, latest = %+v", result)

		at(s0.Add(500 * time.Millisecond))
		result, ok = bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 2 && result.Status == ProbeStatusTimeout, "result = %+v", result)
		c.Check(result.Attempts == 6 && result.Sent == 4 && result.Received == 4, "counts = %+v", result)
	})
}

func TestProbeFeedback(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t0 := time.Unix(2000, 0)
		clock := &probeClock{now: t0}
		hdrExt := &twcc.HeaderExtensionInterceptor{}
		pacer := NewLeakyBucketPacer(1_000_000, LeakyBucketPacerManualTicks())
		bwe, err := NewSendSideBWE(SendSideBWEPacer(pacer))
		c.Check(err == nil, "NewSendSideBWE: %v", err)
		defer func() { _ = bwe.Close() }()

		mediaSink := &probeRecorder{tag: "media"}
		probeSink := &probeRecorder{tag: "probe"}
		mediaWriter := hdrExt.BindLocalStream(probeMediaInfo(), bwe.AddStream(probeMediaInfo(), mediaSink))
		err = bwe.EnableProbeStream(ProbeStreamConfig{
			SSRC: 200, PayloadType: 127, ClockRate: 90000,
			InitialSequenceNumber: 1000,
			RTPTimeBase:           0,
			LocalTimeBase:         t0,
			TWCCExtensionID:       1,
			HeaderExtension:       hdrExt,
			Writer:                probeSink,
			Now:                   clock.Now,
		})
		c.Check(err == nil, "EnableProbeStream: %v", err)

		run := func(base time.Time, n int, step time.Duration) {
			for k := 0; k < n; k++ {
				clock.now = base.Add(time.Duration(k) * step)
				pacer.Tick(clock.now)
			}
		}

		// Cluster 1: full success. wire = 120 bytes, interval = 1ms.
		clusterID, err := bwe.StartProbe(ProbeConfig{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 8, Timeout: 500 * time.Millisecond})
		c.Check(err == nil && clusterID == 1, "StartProbe: %v id=%d", err, clusterID)
		run(t0, 8, time.Millisecond) // transport sequences 0..7
		c.Check(len(probeSink.records) == 8, "probe packets = %d, want 8", len(probeSink.records))
		probeWriteFeedback(c, bwe, probeRunFeedback(0, 8, 100, 250))
		result, ok := bwe.LatestProbeResult()
		c.Check(ok && result.Status == ProbeStatusSuccess, "result = %+v (ok=%v)", result, ok)
		c.Check(result.ClusterID == 1 && result.Attempts == 8 && result.Sent == 8 && result.Received == 8, "counts = %+v", result)
		sendRate := probeRate(8*120, 120, 7*time.Millisecond)
		recvRate := probeRate(8*120, 120, 7*250*time.Microsecond)
		c.Check(sendRate == 960_000, "sendRate = %d, want 960000", sendRate)
		c.Check(result.Bitrate == min(sendRate, recvRate), "bitrate = %d, want %d", result.Bitrate, min(sendRate, recvRate))
		c.Check(result.Bitrate == 960_000, "bitrate = %d, want 960000", result.Bitrate)
		stats := bwe.GetStats()
		got, ok := stats["probeResult"].(ProbeResult)
		c.Check(ok && got.ClusterID == 1 && got.Bitrate == 960_000, "GetStats probeResult = %v", stats["probeResult"])
		c.Check(probeAverageLoss(bwe) == 0, "probe ACKs must not enter media loss samples: %v", probeAverageLoss(bwe))

		// Cluster 2: one reception below the threshold (7 of 10, threshold 8).
		t1 := t0.Add(100 * time.Millisecond)
		clock.now = t1
		clusterID, err = bwe.StartProbe(ProbeConfig{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 10, Timeout: 500 * time.Millisecond})
		c.Check(err == nil && clusterID == 2, "StartProbe 2: %v id=%d", err, clusterID)
		run(t1, 10, time.Millisecond) // transport sequences 8..17
		probeWriteFeedback(c, bwe, probeVectorFeedback(8, 200, []uint16{1, 1, 1, 1, 1, 1, 1, 0, 0, 0}, 250))
		result, _ = bwe.LatestProbeResult()
		c.Check(result.ClusterID == 1, "cluster 2 must not succeed below threshold, latest = %+v", result)
		clock.now = t1.Add(500 * time.Millisecond)
		pacer.Tick(clock.now)
		result, ok = bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 2 && result.Status == ProbeStatusTimeout, "result = %+v", result)
		c.Check(result.Attempts == 10 && result.Sent == 10 && result.Received == 7, "counts = %+v", result)

		// Cluster 3: split and overlapping feedback, duplicate reports, a loss
		// report later filled, and a loss report trying to revoke a reception.
		t2 := t1.Add(600 * time.Millisecond)
		clock.now = t2
		clusterID, err = bwe.StartProbe(ProbeConfig{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 10, Timeout: 500 * time.Millisecond})
		c.Check(err == nil && clusterID == 3, "StartProbe 3: %v id=%d", err, clusterID)
		run(t2, 10, time.Millisecond) // transport sequences 18..27
		probeWriteFeedback(c, bwe, probeVectorFeedback(18, 300, []uint16{1, 1, 0, 1, 1}, 250))
		probeWriteFeedback(c, bwe, probeVectorFeedback(20, 300, []uint16{1, 1, 0, 1, 1, 1, 1, 1}, 250))
		result, ok = bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 3 && result.Status == ProbeStatusSuccess, "result = %+v", result)
		c.Check(result.Received == 10, "received = %d, want 10 (duplicates ignored, loss filled, revoke ignored)", result.Received)
		sendRate = probeRate(10*120, 120, 9*time.Millisecond)
		recvRate = probeRate(10*120, 120, 1500*time.Microsecond)
		c.Check(result.Bitrate == min(sendRate, recvRate), "bitrate = %d, want %d", result.Bitrate, min(sendRate, recvRate))

		// Media ACKs are still processed by the loss controller.
		_, werr := mediaWriter.Write(&rtp.Header{
			Version: 2, PayloadType: 96, SSRC: 100, SequenceNumber: 500, Timestamp: 12345,
		}, []byte{0x61}, nil)
		c.Check(werr == nil, "media write: %v", werr)
		clock.now = t2.Add(20 * time.Millisecond)
		pacer.Tick(clock.now) // drains the media packet, transport sequence 28
		c.Check(len(mediaSink.records) == 1, "media drained = %d, want 1", len(mediaSink.records))
		probeWriteFeedback(c, bwe, probeLostFeedback(28, 1, 400))
		c.Check(probeAverageLoss(bwe) > 0, "media ACKs must still be processed: %v", probeAverageLoss(bwe))

		// Transport sequence numbers wrap across 65535/0.
		clock2 := &probeClock{now: t0}
		hdrExt2 := &twcc.HeaderExtensionInterceptor{}
		pacer2 := NewLeakyBucketPacer(1_000_000, LeakyBucketPacerManualTicks())
		bwe2, err := NewSendSideBWE(SendSideBWEPacer(pacer2))
		c.Check(err == nil, "NewSendSideBWE 2: %v", err)
		defer func() { _ = bwe2.Close() }()
		probeSink2 := &probeRecorder{tag: "probe2"}
		discardInfo := &interceptor.StreamInfo{
			SSRC:                300,
			RTPHeaderExtensions: []interceptor.RTPHeaderExtension{{URI: transportCCURI, ID: 1}},
		}
		discard := hdrExt2.BindLocalStream(discardInfo, interceptor.RTPWriterFunc(
			func(h *rtp.Header, p []byte, _ interceptor.Attributes) (int, error) {
				return h.MarshalSize() + len(p), nil
			},
		))
		var derr error
		for i := 0; i < 65535; i++ {
			if _, derr = discard.Write(&rtp.Header{Version: 2, SSRC: 300}, nil, nil); derr != nil {
				break
			}
		}
		c.Check(derr == nil, "advancing transport sequence space: %v", derr)
		err = bwe2.EnableProbeStream(ProbeStreamConfig{
			SSRC: 200, PayloadType: 127, ClockRate: 90000,
			InitialSequenceNumber: 10,
			RTPTimeBase:           0,
			LocalTimeBase:         t0,
			TWCCExtensionID:       1,
			HeaderExtension:       hdrExt2,
			Writer:                probeSink2,
			Now:                   clock2.Now,
		})
		c.Check(err == nil, "EnableProbeStream 2: %v", err)
		// wire = 30 bytes, interval = ceil(8*30*1e9/240000000) = 1us.
		clusterID, err = bwe2.StartProbe(ProbeConfig{TargetBitrate: 240_000_000, PaddingBytes: 10, Packets: 4, Timeout: time.Second})
		c.Check(err == nil && clusterID == 1, "StartProbe wrap: %v id=%d", err, clusterID)
		for k := 0; k < 4; k++ {
			clock2.now = t0.Add(time.Duration(k) * time.Microsecond)
			pacer2.Tick(clock2.now)
		}
		c.Check(len(probeSink2.records) == 4, "wrap probe packets = %d, want 4", len(probeSink2.records))
		wantTSeq := []uint16{65535, 0, 1, 2}
		for k, want := range wantTSeq {
			gotTSeq := probeTSeq(c, probeSink2.records[k].wire, 1)
			c.Check(gotTSeq == want, "tseq %d = %d, want %d", k, gotTSeq, want)
		}
		probeWriteFeedback(c, bwe2, probeRunFeedback(65535, 4, 100, 500))
		result, ok = bwe2.LatestProbeResult()
		c.Check(ok && result.Status == ProbeStatusSuccess && result.Received == 4, "wrap result = %+v", result)
		wrapSendRate := probeRate(4*30, 30, 3*time.Microsecond)
		wrapRecvRate := probeRate(4*30, 30, 3*500*time.Microsecond)
		c.Check(wrapSendRate == 240_000_000 && wrapRecvRate == 480_000, "wrap rates = %d/%d", wrapSendRate, wrapRecvRate)
		c.Check(result.Bitrate == 480_000, "wrap bitrate = %d, want 480000", result.Bitrate)
	})
}

func TestProbeLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t0 := time.Unix(3000, 0)
		clock := &probeClock{now: t0}
		hdrExt := &twcc.HeaderExtensionInterceptor{}
		pacer := NewLeakyBucketPacer(1_000_000, LeakyBucketPacerManualTicks())
		bwe, err := NewSendSideBWE(SendSideBWEPacer(pacer))
		c.Check(err == nil, "NewSendSideBWE: %v", err)

		mediaSink := &probeRecorder{tag: "media"}
		probeSink := &probeRecorder{tag: "probe"}
		mediaWriter := hdrExt.BindLocalStream(probeMediaInfo(), bwe.AddStream(probeMediaInfo(), mediaSink))

		validCluster := ProbeConfig{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 4, Timeout: time.Second}
		validStream := ProbeStreamConfig{
			SSRC: 200, PayloadType: 127, ClockRate: 90000,
			InitialSequenceNumber: 60000,
			RTPTimeBase:           0,
			LocalTimeBase:         t0,
			TWCCExtensionID:       1,
			HeaderExtension:       hdrExt,
			Writer:                probeSink,
			Now:                   clock.Now,
		}

		// Probing is disabled by default.
		_, err = bwe.StartProbe(validCluster)
		c.Check(errors.Is(err, ErrProbeNotEnabled), "start while disabled = %v", err)
		c.Check(errors.Is(bwe.CancelProbe(), ErrProbeNotEnabled), "cancel while disabled = %v", bwe.CancelProbe())
		_, ok := bwe.LatestProbeResult()
		c.Check(!ok, "no result without any cluster")

		// A custom pacer without probe support fails with a clear error while
		// still satisfying the Pacer interface.
		bweNoOp, err := NewSendSideBWE(SendSideBWEPacer(NewNoOpPacer()))
		c.Check(err == nil, "NewSendSideBWE noop: %v", err)
		defer func() { _ = bweNoOp.Close() }()
		err = bweNoOp.EnableProbeStream(validStream)
		c.Check(errors.Is(err, ErrProbePacerUnsupported), "custom pacer = %v", err)

		// Invalid stream configurations.
		bad := validStream
		bad.ClockRate = 0
		c.Check(errors.Is(bwe.EnableProbeStream(bad), ErrProbeStreamInvalid), "clock rate 0 = %v", bwe.EnableProbeStream(bad))
		bad = validStream
		bad.TWCCExtensionID = 0
		c.Check(errors.Is(bwe.EnableProbeStream(bad), ErrProbeStreamInvalid), "twcc id 0 = %v", bwe.EnableProbeStream(bad))
		bad = validStream
		bad.TWCCExtensionID = 15
		c.Check(errors.Is(bwe.EnableProbeStream(bad), ErrProbeStreamInvalid), "twcc id 15 = %v", bwe.EnableProbeStream(bad))
		bad = validStream
		bad.SSRC = 100
		c.Check(errors.Is(bwe.EnableProbeStream(bad), ErrProbeSSRCConflict), "ssrc conflict = %v", bwe.EnableProbeStream(bad))
		bad = validStream
		bad.HeaderExtension = nil
		c.Check(errors.Is(bwe.EnableProbeStream(bad), ErrProbeStreamInvalid), "nil header extension = %v", bwe.EnableProbeStream(bad))
		bad = validStream
		bad.Writer = nil
		c.Check(errors.Is(bwe.EnableProbeStream(bad), ErrProbeStreamInvalid), "nil writer = %v", bwe.EnableProbeStream(bad))
		c.Check(bwe.EnableProbeStream(validStream) == nil, "enable valid stream")

		// The result callback never runs under the cluster lock and fires once
		// per finished cluster.
		callbackResults := []ProbeResult{}
		bwe.OnProbeResult(func(r ProbeResult) { callbackResults = append(callbackResults, r) })

		// Invalid cluster configurations fail and preserve the old state.
		badClusters := []ProbeConfig{
			{TargetBitrate: 0, PaddingBytes: 100, Packets: 4, Timeout: time.Second},
			{TargetBitrate: -1, PaddingBytes: 100, Packets: 4, Timeout: time.Second},
			{TargetBitrate: 960_000, PaddingBytes: 0, Packets: 4, Timeout: time.Second},
			{TargetBitrate: 960_000, PaddingBytes: 256, Packets: 4, Timeout: time.Second},
			{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 3, Timeout: time.Second},
			{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 65, Timeout: time.Second},
			{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 4, Timeout: 0},
			{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 4, Timeout: -time.Second},
		}
		for i, bc := range badClusters {
			_, serr := bwe.StartProbe(bc)
			c.Check(errors.Is(serr, ErrProbeConfigInvalid), "bad cluster %d = %v", i, serr)
		}
		clusterID, err := bwe.StartProbe(validCluster)
		c.Check(err == nil && clusterID == 1, "start after invalid configs: %v id=%d", err, clusterID)
		_, err = bwe.StartProbe(validCluster)
		c.Check(errors.Is(err, ErrProbeActive), "double start = %v", err)
		c.Check(errors.Is(bwe.EnableProbeStream(validStream), ErrProbeActive), "enable while active = %v", bwe.EnableProbeStream(validStream))

		at := func(tm time.Time) { clock.now = tm; pacer.Tick(tm) }

		// The in-flight cluster survived the failed starts.
		at(t0)
		c.Check(len(probeSink.records) == 1 && probeSink.records[0].header.SequenceNumber == 60000,
			"old cluster preserved: records=%d", len(probeSink.records))
		at(t0.Add(time.Millisecond))
		c.Check(len(probeSink.records) == 2, "records = %d, want 2", len(probeSink.records))

		// Cancel publishes a canceled result and frees the estimator at once.
		c.Check(bwe.CancelProbe() == nil, "cancel")
		result, ok := bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 1 && result.Status == ProbeStatusCanceled, "result = %+v", result)
		c.Check(result.Attempts == 2 && result.Sent == 2 && result.Received == 0, "counts = %+v", result)
		c.Check(len(callbackResults) == 1 && callbackResults[0].ClusterID == 1, "callback results = %+v", callbackResults)
		err = bwe.CancelProbe()
		c.Check(errors.Is(err, ErrProbeNotActive), "double cancel = %v", err)

		// A new cluster starts immediately; late feedback of the canceled
		// cluster must not join it.
		t1 := t0.Add(10 * time.Millisecond)
		clock.now = t1
		clusterID, err = bwe.StartProbe(validCluster)
		c.Check(err == nil && clusterID == 2, "restart: %v id=%d", err, clusterID)
		probeWriteFeedback(c, bwe, probeRunFeedback(0, 2, 100, 250)) // late, cluster 1
		for k := 0; k < 4; k++ {
			at(t1.Add(time.Duration(k) * time.Millisecond))
		}
		c.Check(len(probeSink.records) == 6, "records = %d, want 6", len(probeSink.records))
		probeWriteFeedback(c, bwe, probeRunFeedback(2, 4, 100, 250)) // cluster 2
		result, ok = bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 2 && result.Status == ProbeStatusSuccess, "result = %+v", result)
		c.Check(result.Received == 4, "old feedback must not join the new cluster, received = %d", result.Received)
		c.Check(result.Bitrate == 960_000, "bitrate = %d, want 960000", result.Bitrate)
		c.Check(probeAverageLoss(bwe) == 0, "probe ACKs stay out of media loss: %v", probeAverageLoss(bwe))

		// The timeout boundary is now reaching start+timeout: no more sends,
		// no more success.
		t2 := t1.Add(100 * time.Millisecond)
		clock.now = t2
		clusterID, err = bwe.StartProbe(ProbeConfig{TargetBitrate: 960_000, PaddingBytes: 100, Packets: 6, Timeout: 3500 * time.Microsecond})
		c.Check(err == nil && clusterID == 3, "StartProbe 3: %v id=%d", err, clusterID)
		for k := 0; k < 4; k++ {
			at(t2.Add(time.Duration(k) * time.Millisecond))
		}
		c.Check(len(probeSink.records) == 10, "records = %d, want 10", len(probeSink.records))
		at(t2.Add(3500 * time.Microsecond)) // exactly at the deadline
		c.Check(len(probeSink.records) == 10, "no send at the deadline = %d", len(probeSink.records))
		result, ok = bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 3 && result.Status == ProbeStatusTimeout, "result = %+v", result)
		c.Check(result.Attempts == 4 && result.Sent == 4, "counts = %+v", result)
		at(t2.Add(10 * time.Millisecond))
		c.Check(len(probeSink.records) == 10, "no sends after timeout = %d", len(probeSink.records))
		probeWriteFeedback(c, bwe, probeRunFeedback(6, 4, 100, 250)) // late, cluster 3
		result, _ = bwe.LatestProbeResult()
		c.Check(result.ClusterID == 3 && result.Status == ProbeStatusTimeout && result.Received == 0,
			"late feedback must not modify a finished result = %+v", result)

		// Cancelling from inside the writer callback must not deadlock; the
		// late write completion must not modify the canceled or a new result.
		t3 := t2.Add(100 * time.Millisecond)
		clock.now = t3
		cancelCalled := make(chan struct{})
		var cancelErr error
		probeSink.onWrite = func(int) {
			cancelErr = bwe.CancelProbe()
			close(cancelCalled)
			probeSink.onWrite = nil
		}
		clusterID, err = bwe.StartProbe(validCluster)
		c.Check(err == nil && clusterID == 4, "StartProbe 4: %v id=%d", err, clusterID)
		at(t3)
		<-cancelCalled // explicit signal: CancelProbe returned inside the writer callback
		c.Check(cancelErr == nil, "cancel inside writer = %v", cancelErr)
		result, ok = bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 4 && result.Status == ProbeStatusCanceled, "result = %+v", result)
		c.Check(result.Attempts == 1 && result.Sent == 0, "late completion leaked into canceled result = %+v", result)
		c.Check(len(probeSink.records) == 11, "the write itself is not withdrawn = %d", len(probeSink.records))

		// The next cluster is unaffected by the previous late completion.
		clock.now = t3.Add(time.Millisecond)
		clusterID, err = bwe.StartProbe(validCluster)
		c.Check(err == nil && clusterID == 5, "StartProbe 5: %v id=%d", err, clusterID)
		at(t3.Add(time.Millisecond))
		c.Check(bwe.CancelProbe() == nil, "cancel 5")
		result, ok = bwe.LatestProbeResult()
		c.Check(ok && result.ClusterID == 5 && result.Attempts == 1 && result.Sent == 1, "cluster 5 counts = %+v", result)
		c.Check(len(callbackResults) == 5, "callback fired once per cluster: %+v", callbackResults)

		// Close releases timers and in-flight records and keeps the media
		// close convention.
		c.Check(bwe.Close() == nil, "close")
		_, werr := mediaWriter.Write(&rtp.Header{Version: 2, SSRC: 100}, []byte{0x61}, nil)
		c.Check(errors.Is(werr, io.ErrClosedPipe), "media write after close = %v", werr)
		c.Check(errors.Is(bwe.WriteRTCP(nil, nil), ErrSendSideBWEClosed), "WriteRTCP after close = %v", bwe.WriteRTCP(nil, nil))
		_, ok = bwe.LatestProbeResult()
		c.Check(ok, "latest result is kept after close")
	})
}
