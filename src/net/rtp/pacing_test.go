// Copyright (C) 2011 Werner Dittmann
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

package rtp

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"testing"
	"time"

	"github.com/wernerd/GoRTP/src/net/rtp/v2/task_support/taskcheck"
)

// pacedWrite records one WriteDataTo attempt of the memory transport.
type pacedWrite struct {
	addr Address
	data []byte
	n    int
	err  error
}

// memTransport is an in-memory TransportWrite/TransportRecv. It records every data
// write attempt (including failed and short ones) and every control write. No sockets,
// no goroutines.
type memTransport struct {
	writes     []pacedWrite
	ctrls      [][]byte
	calls      int
	failCalls  map[int]error // call index -> error to return
	shortCalls map[int]int   // call index -> short length to report
}

func newMemTransport() *memTransport {
	return &memTransport{
		failCalls:  make(map[int]error),
		shortCalls: make(map[int]int),
	}
}

func (m *memTransport) WriteDataTo(p *DataPacket, a *Address) (int, error) {
	call := m.calls
	m.calls++
	addr := *a
	if addr.IPAddr != nil {
		addr.IPAddr = append(net.IP(nil), addr.IPAddr...)
	}
	data := append([]byte(nil), p.Buffer()[:p.InUse()]...)
	n, err := p.InUse(), error(nil)
	if e, ok := m.failCalls[call]; ok {
		n, err = 0, e
	} else if s, ok := m.shortCalls[call]; ok {
		n = s
	}
	m.writes = append(m.writes, pacedWrite{addr: addr, data: data, n: n, err: err})
	return n, err
}

func (m *memTransport) WriteCtrlTo(p *CtrlPacket, a *Address) (int, error) {
	m.ctrls = append(m.ctrls, append([]byte(nil), p.Buffer()[:p.InUse()]...))
	return p.InUse(), nil
}

func (m *memTransport) SetToLower(lower TransportWrite)  {}
func (m *memTransport) CloseWrite()                      {}
func (m *memTransport) ListenOnTransports() error        { return nil }
func (m *memTransport) OnRecvData(p *DataPacket) bool    { return true }
func (m *memTransport) OnRecvCtrl(p *CtrlPacket) bool    { return true }
func (m *memTransport) SetCallUpper(upper TransportRecv) {}
func (m *memTransport) CloseRecv()                       {}
func (m *memTransport) SetEndChannel(ch TransportEnd)    {}

func pacedSession() (*Session, *memTransport) {
	mt := newMemTransport()
	return NewSession(mt, mt), mt
}

var pacedOwnAddr = &Address{IPAddr: net.IPv4(127, 0, 0, 1), DataPort: 5004, CtrlPort: 5005}

func pacedOutStream(c *taskcheck.C, rs *Session, ssrc uint32, seq uint16, pt byte) uint32 {
	idx, err := rs.NewSsrcStreamOut(pacedOwnAddr, ssrc, seq)
	c.Check(err == "", "output stream creation failed: %v", err)
	rs.SsrcStreamOutForIndex(idx).SetPayloadType(pt)
	return idx
}

// makePacedPacket builds a packet on the given stream with explicit wire timestamp,
// sequence number and payload.
func makePacedPacket(rs *Session, idx uint32, stamp uint32, seq uint16, payload ...byte) *DataPacket {
	rp := rs.NewDataPacketForStream(idx, 0)
	rp.SetTimestamp(stamp)
	rp.SetSequence(seq)
	rp.SetPayload(payload)
	return rp
}

func rtpSeq(b []byte) uint16   { return binary.BigEndian.Uint16(b[2:]) }
func rtpStamp(b []byte) uint32 { return binary.BigEndian.Uint32(b[4:]) }
func rtpSsrc(b []byte) uint32  { return binary.BigEndian.Uint32(b[8:]) }

func TestPacedTimeline(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rs, mt := pacedSession()
		t0 := time.Date(2026, 1, 2, 3, 4, 5, 0, time.UTC)

		idxA := pacedOutStream(c, rs, 0x0A0A0A0A, 100, 0)  // audio, PCMU 8000 Hz
		idxB := pacedOutStream(c, rs, 0x0B0B0B0B, 5000, 26) // video, JPEG 90000 Hz

		// *** configuration validation
		c.Check(rs.ConfigurePacing(idxA+7, 8000, 1000, t0, 4) != nil, "configure on unknown stream must fail")
		c.Check(rs.ConfigurePacing(idxA, 0, 1000, t0, 4) != nil, "zero clock rate must fail")
		c.Check(rs.ConfigurePacing(idxA, 8000, 1000, t0, 0) != nil, "capacity below 1 must fail")
		c.Check(rs.ConfigurePacing(idxA, 8000, 1000, t0, 257) != nil, "capacity above 256 must fail")
		rs.streamsOut[idxB].streamStatus = idle
		c.Check(rs.ConfigurePacing(idxB, 90000, 5000, t0, 4) != nil, "inactive stream must fail")
		rs.streamsOut[idxB].streamStatus = active
		c.Check(rs.pacedStreams[idxA] == nil && rs.pacedStreams[idxB] == nil,
			"failed configures must not leave state behind")

		c.Check(rs.ConfigurePacing(idxA, 8000, 1000, t0, 8) == nil, "configure audio stream")
		c.Check(rs.ConfigurePacing(idxB, 90000, 5000, t0, 4) == nil, "configure video stream")

		queue := func(idx uint32, stamp uint32, seq uint16) error {
			return rs.QueuePaced(idx, makePacedPacket(rs, idx, stamp, seq, 0xAB))
		}

		// *** queue validation
		c.Check(rs.QueuePaced(idxA+7, makePacedPacket(rs, idxA, 1000, 1, 0x01)) != nil,
			"queue on unconfigured stream must fail")

		// audio: base 1000 at 8000 Hz -> 160 ticks = 20ms
		c.Check(queue(idxA, 1000, 100) == nil, "first packet on the media anchor")
		c.Check(queue(idxA, 1000, 101) == nil, "fragment with same timestamp")
		c.Check(queue(idxA, 1160, 102) == nil, "next audio frame")
		c.Check(queue(idxA, 1080, 103) != nil, "timestamp regression must fail")
		c.Check(queue(idxA, 1160, 103) == nil, "timestamp equal to last accepted is ok")
		c.Check(queue(idxA, 1320, 103) != nil, "duplicate sequence number must fail")
		c.Check(queue(idxA, 1320, 103+32768) != nil, "sequence step of 32768 must fail")
		c.Check(queue(idxA, 1320, 104) == nil, "accepted sequence and clock survive rejected packets")
		c.Check(queue(idxA, 999, 105) != nil, "negative media offset must fail")
		c.Check(queue(idxA, 1480, 105) == nil, "fill queue 1")
		c.Check(queue(idxA, 1640, 106) == nil, "fill queue 2")
		c.Check(queue(idxA, 1800, 107) == nil, "fill queue 3")
		c.Check(queue(idxA, 1960, 108) != nil, "full queue must reject")

		freed := makePacedPacket(rs, idxA, 1960, 108, 0x01)
		freed.FreePacket()
		c.Check(rs.QueuePaced(idxA, freed) != nil, "released packet must fail")

		badVersion := makePacedPacket(rs, idxA, 1960, 108, 0x01)
		badVersion.buffer[0] = 0x40 // RTP version 1
		c.Check(rs.QueuePaced(idxA, badVersion) != nil, "wrong RTP version must fail")
		badVersion.FreePacket()

		foreign := makePacedPacket(rs, idxB, 1160, 108, 0x01)
		c.Check(rs.QueuePaced(idxA, foreign) != nil, "SSRC mismatch must fail")
		foreign.FreePacket()

		c.Check(rs.ConfigurePacing(idxA, 8000, 2000, t0, 4) != nil,
			"reconfigure with queued packets must fail")

		// video: base 5000 at 90000 Hz -> 3000 ticks = 33.33ms
		c.Check(queue(idxB, 5000, 5000) == nil, "first video frame")
		c.Check(queue(idxB, 8000, 5001) == nil, "second video frame")

		remote := &Address{IPAddr: net.IPv4(10, 0, 0, 1), DataPort: 6000, CtrlPort: 6001}
		if _, err := rs.AddRemote(remote); err != nil {
			c.Check(false, "AddRemote failed: %v", err)
		}

		// *** nothing due yet
		retired, err := rs.PumpPaced(t0.Add(-time.Nanosecond))
		c.Check(err == nil && retired == 0, "pump before first deadline: retired=%d err=%v", retired, err)
		c.Check(len(mt.writes) == 0, "no writes before first deadline")

		// *** one late pump catches up everything, in media order across both streams
		retired, err = rs.PumpPaced(t0.Add(100 * time.Millisecond))
		c.Check(err == nil, "late pump error: %v", err)
		c.Check(retired == 10, "late pump must catch up all due packets, retired=%d", retired)

		want := []struct {
			ssrc  uint32
			seq   uint16
			stamp uint32
		}{
			{0x0A0A0A0A, 100, 1000},   // audio due t0
			{0x0A0A0A0A, 101, 1000},   // audio fragment due t0
			{0x0B0B0B0B, 5000, 5000},  // video due t0, loses tie to lower stream index
			{0x0A0A0A0A, 102, 1160},   // audio due t0+20ms
			{0x0A0A0A0A, 103, 1160},   // audio due t0+20ms
			{0x0B0B0B0B, 5001, 8000},  // video due t0+33.33ms
			{0x0A0A0A0A, 104, 1320},   // audio due t0+40ms
			{0x0A0A0A0A, 105, 1480},   // audio due t0+60ms
			{0x0A0A0A0A, 106, 1640},   // audio due t0+80ms
			{0x0A0A0A0A, 107, 1800},   // audio due t0+100ms
		}
		c.Check(len(mt.writes) == len(want), "wire packet count: got %d want %d", len(mt.writes), len(want))
		for i, w := range want {
			if i >= len(mt.writes) {
				break
			}
			got := mt.writes[i]
			c.Check(got.err == nil && got.n == len(got.data), "write %d completed", i)
			c.Check(got.data[0] == version2Bit, "write %d has RTP version 2", i)
			c.Check(rtpSsrc(got.data) == w.ssrc, "write %d SSRC: got %#x want %#x", i, rtpSsrc(got.data), w.ssrc)
			c.Check(rtpSeq(got.data) == w.seq, "write %d sequence: got %d want %d", i, rtpSeq(got.data), w.seq)
			c.Check(rtpStamp(got.data) == w.stamp, "write %d timestamp: got %d want %d", i, rtpStamp(got.data), w.stamp)
			c.Check(bytes.Equal(got.data[12:], []byte{0xAB}), "write %d payload bytes", i)
			c.Check(got.addr.DataPort == 6000 && got.addr.IPAddr.Equal(net.IPv4(10, 0, 0, 1)),
				"write %d destination address", i)
		}

		// *** queue-full rejection did not advance the accepted sequence or media clock
		c.Check(queue(idxA, 1960, 108) == nil, "next packet after full-queue rejection")
		c.Check(queue(idxA, 1960+2*8000, 109) == nil, "timestamp jump after media silence")
		retired, err = rs.PumpPaced(t0.Add(120 * time.Millisecond))
		c.Check(err == nil && retired == 1, "packet due at t0+120ms: retired=%d err=%v", retired, err)
		retired, err = rs.PumpPaced(t0.Add(3 * time.Second))
		c.Check(err == nil && retired == 1, "packet after silence gap: retired=%d err=%v", retired, err)

		// *** reconfigure on empty queue, then wrap timestamp and sequence number
		t1 := t0.Add(time.Hour)
		wrapBase := uint32(0xFFFFFFF0)
		c.Check(rs.ConfigurePacing(idxA, 8000, wrapBase, t1, 8) == nil, "reconfigure on empty queue")
		c.Check(queue(idxA, wrapBase, 65534) == nil, "base near timestamp wrap")
		c.Check(queue(idxA, wrapBase+0x81, 65535) == nil, "timestamp wraps past 2^32")
		c.Check(queue(idxA, wrapBase+0x101, 0) == nil, "sequence wraps past 2^16")
		c.Check(queue(idxA, wrapBase+0x181, 1) == nil, "after both wraps")

		retired, err = rs.PumpPaced(t1.Add(16 * time.Millisecond))
		c.Check(err == nil && retired == 1, "only first wrapped packet due (129 ticks = 16.125ms): retired=%d err=%v",
			retired, err)
		retired, err = rs.PumpPaced(t1.Add(time.Second))
		c.Check(err == nil && retired == 3, "remaining wrapped packets: retired=%d err=%v", retired, err)

		n := len(mt.writes)
		c.Check(n == 16, "total wire packets: got %d want 16", n)
		wantSeq := []uint16{65534, 65535, 0, 1}
		for i, s := range wantSeq {
			if n-4+i < 0 {
				break
			}
			got := rtpSeq(mt.writes[n-4+i].data)
			c.Check(got == s, "wrapped write %d sequence: got %d want %d", i, got, s)
		}
	})
}

func TestPacedDelivery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rs, mt := pacedSession()
		t0 := time.Date(2026, 2, 3, 4, 5, 6, 0, time.UTC)

		idxA := pacedOutStream(c, rs, 0xAAAA0001, 1, 0)
		idxB := pacedOutStream(c, rs, 0xBBBB0002, 7000, 26)
		c.Check(rs.ConfigurePacing(idxA, 8000, 0, t0, 8) == nil, "configure stream A")
		c.Check(rs.ConfigurePacing(idxB, 8000, 0, t0, 8) == nil, "configure stream B")

		addr1 := &Address{IPAddr: net.IPv4(10, 0, 0, 1), DataPort: 6000, CtrlPort: 6001}
		addr2 := &Address{IPAddr: net.IPv4(10, 0, 0, 2), DataPort: 6002, CtrlPort: 6003}
		addr3 := &Address{IPAddr: net.IPv4(10, 0, 0, 3), DataPort: 6004, CtrlPort: 6005}
		r1, _ := rs.AddRemote(addr1)
		if _, err := rs.AddRemote(addr2); err != nil {
			c.Check(false, "AddRemote addr2 failed: %v", err)
		}
		errBoom := errors.New("simulated send failure")
		strA := rs.SsrcStreamOutForIndex(idxA)

		queue := func(idx uint32, stamp uint32, seq uint16, payload ...byte) {
			c.Check(rs.QueuePaced(idx, makePacedPacket(rs, idx, stamp, seq, payload...)) == nil,
				"queue stamp=%d seq=%d", stamp, seq)
		}

		// *** target set freezes at first send; partial failure retries only the failed target
		queue(idxA, 0, 1, 0x01)   // due t0
		queue(idxA, 160, 2, 0x02) // due t0+20ms
		mt.failCalls[mt.calls] = errBoom // first attempt of the next pump fails
		retired, err := rs.PumpPaced(t0.Add(10 * time.Millisecond))
		c.Check(retired == 0 && err == errBoom, "first pump: retired=%d err=%v", retired, err)
		c.Check(len(mt.writes) == 2, "each incomplete target tried exactly once per pump, writes=%d", len(mt.writes))

		if _, err := rs.AddRemote(addr3); err != nil { // joins after P1 started to send
			c.Check(false, "AddRemote addr3 failed: %v", err)
		}
		retired, err = rs.PumpPaced(t0.Add(20 * time.Millisecond))
		c.Check(retired == 2 && err == nil, "recovery pump: retired=%d err=%v", retired, err)
		c.Check(len(mt.writes) == 6, "retry plus frozen sets: writes=%d", len(mt.writes))
		c.Check(mt.writes[0].addr.DataPort == 6000 && mt.writes[0].err == errBoom, "P1 first target failed")
		c.Check(mt.writes[1].addr.DataPort == 6002 && mt.writes[1].err == nil, "P1 second target ok")
		c.Check(mt.writes[2].addr.DataPort == 6000 && mt.writes[2].err == nil, "P1 failed target retried")
		c.Check(mt.writes[3].addr.DataPort == 6000, "P2 to first remote")
		c.Check(mt.writes[4].addr.DataPort == 6002, "P2 to second remote")
		c.Check(mt.writes[5].addr.DataPort == 6004, "P2 to remote added after P1 froze")
		c.Check(strA.SenderPacketCnt == 2, "each packet counted exactly once: %d", strA.SenderPacketCnt)

		// *** short write reports io.ErrShortWrite and is retried
		queue(idxA, 320, 3, 0x03) // due t0+40ms
		mt.shortCalls[mt.calls] = 5
		firstSuccess := t0.Add(40 * time.Millisecond)
		retired, err = rs.PumpPaced(firstSuccess)
		c.Check(retired == 0 && err == io.ErrShortWrite, "short write: retired=%d err=%v", retired, err)
		c.Check(strA.SenderPacketCnt == 3, "counted once on first successful target: %d", strA.SenderPacketCnt)
		c.Check(strA.statistics.lastPacketTime == firstSuccess.UnixNano(), "lastPacketTime from first success")
		writesBefore := len(mt.writes)
		retired, err = rs.PumpPaced(t0.Add(50 * time.Millisecond))
		c.Check(retired == 1 && err == nil, "short-written target retried: retired=%d err=%v", retired, err)
		c.Check(len(mt.writes) == writesBefore+1, "only the incomplete target retried")
		c.Check(mt.writes[len(mt.writes)-1].addr.DataPort == 6000, "retried target is the short-written one")
		c.Check(strA.SenderPacketCnt == 3, "retry does not double count: %d", strA.SenderPacketCnt)
		c.Check(strA.statistics.lastPacketTime == firstSuccess.UnixNano(), "retry does not move lastPacketTime")

		// *** frozen address survives caller mutation of the original Address
		queue(idxA, 480, 4, 0x04) // due t0+60ms
		mt.failCalls[mt.calls] = errBoom
		retired, err = rs.PumpPaced(t0.Add(60 * time.Millisecond))
		c.Check(retired == 0 && err == errBoom, "freeze pump: retired=%d err=%v", retired, err)
		addr1.DataPort = 7778 // caller rewrites the original Address after the freeze
		addr1.IPAddr = net.IPv4(192, 168, 0, 1)
		retired, err = rs.PumpPaced(t0.Add(70 * time.Millisecond))
		c.Check(retired == 1 && err == nil, "retry after address mutation: retired=%d err=%v", retired, err)
		last := mt.writes[len(mt.writes)-1]
		c.Check(last.addr.DataPort == 6000 && last.addr.IPAddr.Equal(net.IPv4(10, 0, 0, 1)),
			"retry uses frozen address, got port %d ip %v", last.addr.DataPort, last.addr.IPAddr)

		// *** removed remote counts as cancelled and is never accessed again
		queue(idxA, 640, 5, 0x05) // due t0+80ms
		mt.failCalls[mt.calls] = errBoom
		retired, err = rs.PumpPaced(t0.Add(80 * time.Millisecond))
		c.Check(retired == 0 && err == errBoom, "one target fails before removal: retired=%d err=%v", retired, err)
		rs.RemoveRemote(r1)
		writesBefore = len(mt.writes)
		retired, err = rs.PumpPaced(t0.Add(90 * time.Millisecond))
		c.Check(retired == 1 && err == nil, "removed remote cancels its target: retired=%d err=%v", retired, err)
		c.Check(len(mt.writes) == writesBefore, "cancelled target never accessed again")

		// *** re-added remote is a new index, only new packets see it
		addr4 := &Address{IPAddr: net.IPv4(10, 0, 0, 4), DataPort: 6006, CtrlPort: 6007}
		if _, err := rs.AddRemote(addr4); err != nil {
			c.Check(false, "AddRemote addr4 failed: %v", err)
		}
		queue(idxA, 800, 6, 0x06) // due t0+100ms
		writesBefore = len(mt.writes)
		retired, err = rs.PumpPaced(t0.Add(100 * time.Millisecond))
		c.Check(retired == 1 && err == nil, "packet after re-add: retired=%d err=%v", retired, err)
		newWrites := mt.writes[writesBefore:]
		c.Check(len(newWrites) == 3, "removed remote not in new frozen set: writes=%d", len(newWrites))
		wantPorts := []int{6002, 6004, 6006}
		for i, w := range newWrites {
			c.Check(w.addr.DataPort == wantPorts[i], "frozen target %d port: got %d want %d",
				i, w.addr.DataPort, wantPorts[i])
		}

		// *** queue holds an independent copy of the media bytes
		p := makePacedPacket(rs, idxA, 960, 7, 1, 2, 3) // due t0+120ms
		c.Check(rs.QueuePaced(idxA, p) == nil, "queue packet later mutated by caller")
		p.SetPayload([]byte{9, 9, 9, 9, 9})
		p.FreePacket()
		writesBefore = len(mt.writes)
		retired, err = rs.PumpPaced(t0.Add(120 * time.Millisecond))
		c.Check(retired == 1 && err == nil, "pump of caller-mutated packet: retired=%d err=%v", retired, err)
		for _, w := range mt.writes[writesBefore:] {
			c.Check(bytes.Equal(w.data[12:], []byte{1, 2, 3}), "queued copy keeps original payload")
		}

		// *** a blocked head stalls only its own stream
		queue(idxA, 1120, 8, 0x08) // due t0+140ms
		queue(idxB, 160, 7000, 0x0B) // stream B first packet, due t0+20ms
		base := mt.calls
		for i := 3; i < 6; i++ { // stream B sends first (earlier due), then A's three targets fail
			mt.failCalls[base+i] = errBoom
		}
		retired, err = rs.PumpPaced(t0.Add(200 * time.Millisecond))
		c.Check(retired == 1 && err == errBoom, "stream B proceeds while A blocked: retired=%d err=%v", retired, err)
		c.Check(rtpSsrc(mt.writes[base].data) == 0xBBBB0002, "earlier deadline of stream B sent first")
		retired, err = rs.PumpPaced(t0.Add(210 * time.Millisecond))
		c.Check(retired == 1 && err == nil, "stream A recovers once failures cleared: retired=%d err=%v", retired, err)

		// *** empty target set retires without sending and without counting
		rs2, mt2 := pacedSession()
		idx2 := pacedOutStream(c, rs2, 0x0E0E0E0E, 9, 0)
		c.Check(rs2.ConfigurePacing(idx2, 8000, 0, t0, 4) == nil, "configure remote-less stream")
		c.Check(rs2.QueuePaced(idx2, makePacedPacket(rs2, idx2, 0, 1, 0x77)) == nil, "queue without remotes")
		retired, err = rs2.PumpPaced(t0)
		c.Check(retired == 1 && err == nil, "empty target set retires the packet: retired=%d err=%v", retired, err)
		c.Check(len(mt2.writes) == 0, "nothing sent without remotes")
		c.Check(rs2.SsrcStreamOutForIndex(idx2).SenderPacketCnt == 0, "empty target set is not counted as sent")
	})
}

func TestPacedReports(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		rs, mt := pacedSession()
		t0 := time.Date(2026, 3, 4, 5, 6, 7, 0, time.UTC)

		idxA := pacedOutStream(c, rs, 0x00C0FFEE, 1, 0)
		c.Check(rs.ConfigurePacing(idxA, 8000, 0, t0, 8) == nil, "configure stream A")
		addr1 := &Address{IPAddr: net.IPv4(10, 1, 0, 1), DataPort: 6100, CtrlPort: 6101}
		if _, err := rs.AddRemote(addr1); err != nil {
			c.Check(false, "AddRemote failed: %v", err)
		}
		errBoom := errors.New("simulated send failure")
		strA := rs.SsrcStreamOutForIndex(idxA)

		c.Check(rs.QueuePaced(idxA, makePacedPacket(rs, idxA, 0, 1, make([]byte, 100)...)) == nil, "queue P1")
		c.Check(rs.QueuePaced(idxA, makePacedPacket(rs, idxA, 160, 2, make([]byte, 200)...)) == nil, "queue P2")

		// *** no success yet: no statistics
		mt.failCalls[mt.calls] = errBoom
		retired, err := rs.PumpPaced(t0)
		c.Check(retired == 0 && err == errBoom, "failing first pump: retired=%d err=%v", retired, err)
		c.Check(strA.SenderPacketCnt == 0 && strA.SenderOctectCnt == 0, "no counters before first success")
		c.Check(!strA.sender && !rs.weSent, "sender flags untouched before first success")
		c.Check(strA.statistics.lastPacketTime == 0, "lastPacketTime untouched before first success")

		// *** first successful target write counts the packet once
		t1 := t0.Add(10 * time.Millisecond)
		retired, err = rs.PumpPaced(t1)
		c.Check(retired == 1 && err == nil, "recovering pump: retired=%d err=%v", retired, err)
		c.Check(strA.SenderPacketCnt == 1 && strA.SenderOctectCnt == 100,
			"first success counts once: packets=%d octets=%d", strA.SenderPacketCnt, strA.SenderOctectCnt)
		c.Check(strA.sender && rs.weSent, "sender flags set on first success")
		c.Check(strA.statistics.lastPacketTime == t1.UnixNano(), "lastPacketTime is the first success now")

		// *** failed retry of the next packet changes nothing
		mt.failCalls[mt.calls] = errBoom
		retired, err = rs.PumpPaced(t0.Add(20 * time.Millisecond))
		c.Check(retired == 0 && err == errBoom, "failing pump for P2: retired=%d err=%v", retired, err)
		c.Check(strA.SenderPacketCnt == 1 && strA.SenderOctectCnt == 100, "failed retry does not count")
		c.Check(strA.statistics.lastPacketTime == t1.UnixNano(), "failed retry keeps lastPacketTime")

		t3 := t0.Add(30 * time.Millisecond)
		retired, err = rs.PumpPaced(t3)
		c.Check(retired == 1 && err == nil, "P2 recovery: retired=%d err=%v", retired, err)
		c.Check(strA.SenderPacketCnt == 2 && strA.SenderOctectCnt == 300,
			"second packet counted: packets=%d octets=%d", strA.SenderPacketCnt, strA.SenderOctectCnt)
		c.Check(strA.statistics.lastPacketTime == t3.UnixNano(), "lastPacketTime updated on P2 first success")

		// *** a packet whose targets are all cancelled never counts
		c.Check(rs.QueuePaced(idxA, makePacedPacket(rs, idxA, 320, 3, make([]byte, 50)...)) == nil, "queue P3")
		mt.failCalls[mt.calls] = errBoom
		retired, err = rs.PumpPaced(t0.Add(40 * time.Millisecond))
		c.Check(retired == 0 && err == errBoom, "P3 target fails: retired=%d err=%v", retired, err)
		rs.RemoveRemote(0)
		writesBefore := len(mt.writes)
		retired, err = rs.PumpPaced(t0.Add(50 * time.Millisecond))
		c.Check(retired == 1 && err == nil, "all targets cancelled retires P3: retired=%d err=%v", retired, err)
		c.Check(len(mt.writes) == writesBefore, "cancelled target not accessed")
		c.Check(strA.SenderPacketCnt == 2 && strA.SenderOctectCnt == 300, "never-written packet not counted")
		c.Check(strA.statistics.lastPacketTime == t3.UnixNano(), "cancel keeps lastPacketTime")

		// *** the RTCP sender report reflects the same counters
		if _, err := rs.AddRemote(addr1); err != nil { // session needs a remote for WriteCtrl
			c.Check(false, "re-AddRemote failed: %v", err)
		}
		rc := rs.buildRtcpPkt(strA, 0)
		c.Check(rc.Type(0) == RtcpSR, "sending stream produces a sender report")
		if _, err := rs.WriteCtrl(rc); err != nil {
			c.Check(false, "WriteCtrl failed: %v", err)
		}
		c.Check(len(mt.ctrls) == 1, "sender report reached the transport")
		if len(mt.ctrls) == 1 {
			sr := mt.ctrls[0]
			c.Check(sr[0] == version2Bit && sr[1] == RtcpSR, "SR header: %#02x %#02x", sr[0], sr[1])
			c.Check(binary.BigEndian.Uint32(sr[4:]) == 0x00C0FFEE, "SR SSRC")
			c.Check(binary.BigEndian.Uint32(sr[8:]) != 0, "SR NTP seconds set")
			c.Check(binary.BigEndian.Uint32(sr[20:]) == 2, "SR sender packet count: got %d",
				binary.BigEndian.Uint32(sr[20:]))
			c.Check(binary.BigEndian.Uint32(sr[24:]) == 300, "SR sender octet count: got %d",
				binary.BigEndian.Uint32(sr[24:]))
		}
		rc.FreePacket()

		// *** CancelPaced drops queued copies, keeps statistics
		c.Check(rs.QueuePaced(idxA, makePacedPacket(rs, idxA, 480, 4, 0x04)) == nil, "queue P4")
		rs.CancelPaced(idxA)
		c.Check(rs.QueuePaced(idxA, makePacedPacket(rs, idxA, 640, 5, 0x05)) != nil, "queue after cancel fails")
		c.Check(strA.SenderPacketCnt == 2 && strA.SenderOctectCnt == 300, "cancel keeps statistics")
		retired, err = rs.PumpPaced(t0.Add(time.Hour))
		c.Check(retired == 0 && err == nil, "cancelled stream has nothing to pump: retired=%d err=%v", retired, err)

		// *** reconfigure after cancel starts with fresh sequence/timestamp state
		c.Check(rs.ConfigurePacing(idxA, 8000, 0, t0, 4) == nil, "reconfigure after cancel")
		c.Check(rs.QueuePaced(idxA, makePacedPacket(rs, idxA, 0, 42, 0x2A)) == nil, "fresh state after reconfigure")

		// *** closing a configured stream releases its queued copies
		rs.SsrcStreamCloseForIndex(idxA)
		c.Check(rs.pacedStreams[idxA] == nil, "stream close releases queued copies")
		c.Check(strA.SenderPacketCnt == 2 && strA.SenderOctectCnt == 300,
			"unstarted packet not turned into sent by stream close")

		// *** closing the session releases all queues, even without a running RTCP service
		idxB := pacedOutStream(c, rs, 0x00B0B0B0, 1, 26)
		c.Check(rs.ConfigurePacing(idxB, 90000, 0, t0, 4) == nil, "configure stream B")
		c.Check(rs.QueuePaced(idxB, makePacedPacket(rs, idxB, 0, 1, 0x0B)) == nil, "queue on stream B")
		c.Check(rs.QueuePaced(idxB, makePacedPacket(rs, idxB, 3000, 2, 0x0B)) == nil, "queue second on stream B")
		rs.CloseSession()
		c.Check(len(rs.pacedStreams) == 0, "session close releases all paced queues")

		// *** two sending streams pump fine without a running RTCP service
		rs3, mt3 := pacedSession()
		idxX := pacedOutStream(c, rs3, 0x0AA00001, 1, 0)
		idxY := pacedOutStream(c, rs3, 0x0BB00002, 1, 0)
		c.Check(rs3.ConfigurePacing(idxX, 8000, 0, t0, 4) == nil, "configure stream X")
		c.Check(rs3.ConfigurePacing(idxY, 8000, 0, t0, 4) == nil, "configure stream Y")
		if _, err := rs3.AddRemote(addr1); err != nil {
			c.Check(false, "AddRemote on third session failed: %v", err)
		}
		c.Check(rs3.QueuePaced(idxX, makePacedPacket(rs3, idxX, 0, 1, 0x01)) == nil, "queue on stream X")
		c.Check(rs3.QueuePaced(idxY, makePacedPacket(rs3, idxY, 0, 1, 0x02)) == nil, "queue on stream Y")
		retired, err = rs3.PumpPaced(t0.Add(time.Second))
		c.Check(retired == 2 && err == nil, "two-stream pump returns without RTCP service: retired=%d err=%v",
			retired, err)
		c.Check(len(mt3.writes) == 2, "both streams sent: writes=%d", len(mt3.writes))
		c.Check(rs3.SsrcStreamOutForIndex(idxX).sender && rs3.SsrcStreamOutForIndex(idxY).sender,
			"both streams became senders")
	})
}
