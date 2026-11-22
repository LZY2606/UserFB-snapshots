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

/*
 * This source file contains a manually pumped media send queue for the Session type.
 *
 * The pacing queue lets an application replay media (for example from a file) through
 * any TransportWrite implementation - including in-memory transports - without opening
 * sockets and without a timer goroutine. The application queues RTP packets, each gets
 * a deadline derived from its RTP timestamp, and drives the queue by calling PumpPaced
 * with the current time.
 */

import (
	"io"
	"net"
	"sort"
	"time"
)

// Limits for the paced send queue capacity.
const (
	pacedMinCapacity = 1
	pacedMaxCapacity = 256
)

// State of a frozen send target of a paced packet.
const (
	pacedTargetPending   = iota // not yet successfully written
	pacedTargetDone             // successfully written, never retried
	pacedTargetCancelled        // remote was removed, never accessed again
)

// pacedTarget stores a frozen copy of one remote destination of a paced packet.
type pacedTarget struct {
	index uint32 // remote index at freeze time
	addr  Address
	state int
}

// pacedPacket holds an independent copy of one queued RTP packet.
type pacedPacket struct {
	wire       []byte // frozen copy of the complete RTP packet
	payloadLen int    // payload length in bytes, for sender statistics
	due        int64  // send deadline, UnixNano
	targets    []pacedTarget
	frozen     bool // true once the target set was frozen
	counted    bool // true once sender statistics were updated
}

// pacedStream holds the pacing configuration and the send queue of one output stream.
type pacedStream struct {
	clockRate uint32
	baseStamp uint32
	baseTime  int64 // UnixNano
	capacity  int
	queue     []*pacedPacket
	lastSeq   uint16
	lastStamp uint32
	hasLast   bool
}

// ConfigurePacing enables the manually pumped send queue for an output stream.
//
// The output stream must exist and must be active. clockRate is the RTP media clock
// in Hertz and must be positive. capacity is the maximum number of queued packets and
// must be between 1 and 256.
//
// Each queued packet gets its send deadline from
//
//	baseTime + int64(int32(packet.Timestamp()-baseTimestamp)) * time.Second / clockRate
//
// thus the media clock may wrap at 2^32 as long as the used span stays below 2^31 ticks.
//
// A stream may be re-configured only while no packets are queued. A failed call has
// no side effects.
//
//   streamIndex   - the index of the output stream as returned by NewSsrcStreamOut
//   clockRate     - the RTP media clock rate in Hertz
//   baseTimestamp - the RTP timestamp that anchors the media timeline
//   baseTime      - the wall clock time that corresponds to baseTimestamp
//   capacity      - maximum number of queued packets, 1 to 256
//
func (rs *Session) ConfigurePacing(streamIndex uint32, clockRate uint32, baseTimestamp uint32, baseTime time.Time, capacity int) error {
	str := rs.streamsOut[streamIndex]
	if str == nil {
		return Error("ConfigurePacing: no such output stream.")
	}
	if str.streamStatus != active {
		return Error("ConfigurePacing: output stream is not active.")
	}
	if clockRate == 0 {
		return Error("ConfigurePacing: clock rate must be positive.")
	}
	if capacity < pacedMinCapacity || capacity > pacedMaxCapacity {
		return Error("ConfigurePacing: capacity out of range.")
	}
	if ps := rs.pacedStreams[streamIndex]; ps != nil && len(ps.queue) > 0 {
		return Error("ConfigurePacing: stream has queued packets.")
	}
	rs.pacedStreams[streamIndex] = &pacedStream{
		clockRate: clockRate,
		baseStamp: baseTimestamp,
		baseTime:  baseTime.UnixNano(),
		capacity:  capacity,
	}
	return nil
}

// QueuePaced appends a copy of packet to the send queue of a paced output stream.
//
// The packet's SSRC must match the stream's SSRC, the RTP version must be correct and
// the packet must not be released. The media offset (packet timestamp minus the configured
// base timestamp, interpreted as signed 32bit) must not be negative. The timestamp must
// not move backwards compared to the last successfully queued packet; equal timestamps
// (fragments of one media instant) are allowed. The sequence number must strictly
// advance in 16bit modular order with a step smaller than 32768; gaps are allowed.
//
// The queue stores an independent copy: the caller may modify or free the packet
// immediately after this method returns. If the queue is full or any check fails the
// method returns an error and neither the accepted sequence number nor the media clock
// advance.
//
//   streamIndex - the index of the output stream as returned by NewSsrcStreamOut
//   packet      - the RTP packet to queue
//
func (rs *Session) QueuePaced(streamIndex uint32, packet *DataPacket) error {
	ps := rs.pacedStreams[streamIndex]
	if ps == nil {
		return Error("QueuePaced: pacing not configured for this stream.")
	}
	str := rs.streamsOut[streamIndex]
	if str == nil {
		return Error("QueuePaced: no such output stream.")
	}
	if packet == nil || packet.isFree {
		return Error("QueuePaced: packet already released.")
	}
	if packet.buffer[0]&version2Bit != version2Bit {
		return Error("QueuePaced: not a version 2 RTP packet.")
	}
	if packet.Ssrc() != str.ssrc {
		return Error("QueuePaced: SSRC does not match the stream.")
	}
	stamp := packet.Timestamp()
	if int32(stamp-ps.baseStamp) < 0 {
		return Error("QueuePaced: timestamp before the configured base timestamp.")
	}
	seq := packet.Sequence()
	if ps.hasLast {
		if int32(stamp-ps.lastStamp) < 0 {
			return Error("QueuePaced: timestamp moved backwards.")
		}
		if step := seq - ps.lastSeq; step == 0 || step >= 32768 {
			return Error("QueuePaced: sequence number does not strictly advance.")
		}
	}
	if len(ps.queue) >= ps.capacity {
		return Error("QueuePaced: send queue is full.")
	}
	wire := make([]byte, packet.inUse)
	copy(wire, packet.buffer[:packet.inUse])
	ps.queue = append(ps.queue, &pacedPacket{
		wire:       wire,
		payloadLen: len(packet.Payload()),
		due:        ps.baseTime + int64(int32(stamp-ps.baseStamp))*int64(time.Second)/int64(ps.clockRate),
	})
	ps.lastSeq = seq
	ps.lastStamp = stamp
	ps.hasLast = true
	return nil
}

// PumpPaced sends all queued packets whose deadline is not later than now.
//
// The caller guarantees that now never moves backwards between calls. PumpPaced never
// sleeps: if it is called late it catches up all due packets in one call, the media
// anchor (base time) stays untouched.
//
// Packets are selected by the deadline of each stream's head packet first, ties are
// broken by ascending stream index. Inside one stream the queue order is kept: a head
// packet that is not yet complete blocks the packets behind it, other streams are
// processed normally.
//
// The first time a head packet is really sent its target set is frozen from the
// current remotes in ascending remote index order; remotes added later only receive
// packets that have not started to send. A target removed with RemoveRemote counts as
// cancelled and is never accessed again. Each target is completed independently: only
// a write that returns no error and the full packet length counts as success, failed
// or short writes are retried on the next PumpPaced call. During one PumpPaced call
// each incomplete target of a packet is tried at most once.
//
// Sender statistics (SenderPacketCnt, SenderOctectCnt, sender/weSent, lastPacketTime)
// are updated exactly once per packet: when the first target write succeeds, using
// this call's now for lastPacketTime. Packets whose targets were all cancelled before
// any successful write are not counted. Packets with an empty target set are removed
// from the queue without sending.
//
// The method returns the number of logical packets removed from the queues during this
// call (completed, cancelled, or empty target set) and the first error encountered in
// attempt order; short writes report io.ErrShortWrite.
//
func (rs *Session) PumpPaced(now time.Time) (retired int, err error) {
	nowNano := now.UnixNano()
	blocked := make(map[uint32]bool, len(rs.pacedStreams))
	for {
		// Arbitrate: earliest head deadline first, then ascending stream index.
		var bestIdx uint32
		var bestDue int64
		found := false
		for idx, ps := range rs.pacedStreams {
			if blocked[idx] || len(ps.queue) == 0 {
				continue
			}
			if due := ps.queue[0].due; due <= nowNano {
				if !found || due < bestDue || (due == bestDue && idx < bestIdx) {
					found, bestIdx, bestDue = true, idx, due
				}
			}
		}
		if !found {
			return retired, err
		}
		ps := rs.pacedStreams[bestIdx]
		pp := ps.queue[0]
		if !pp.frozen {
			pp.freezeTargets(rs)
		}
		if len(pp.targets) == 0 {
			// No destinations: remove without sending, not counted as sent.
			ps.queue = ps.queue[1:]
			retired++
			continue
		}
		var pktErr error
		pending := 0
		for i := range pp.targets {
			t := &pp.targets[i]
			if t.state != pacedTargetPending {
				continue
			}
			if _, ok := rs.remotes[t.index]; !ok {
				t.state = pacedTargetCancelled // removed remote: cancel, never touch again
				continue
			}
			addr := t.addr // work on a copy: the frozen address stays untouched
			rp := &DataPacket{RawPacket: RawPacket{buffer: pp.wire, inUse: len(pp.wire)}}
			n, werr := rs.transportWrite.WriteDataTo(rp, &addr)
			if werr == nil && n != len(pp.wire) {
				werr = io.ErrShortWrite
			}
			if werr != nil {
				if pktErr == nil {
					pktErr = werr
				}
				pending++
				continue
			}
			t.state = pacedTargetDone
			if !pp.counted {
				pp.counted = true
				rs.countPacedSent(bestIdx, pp.payloadLen, nowNano)
			}
		}
		if pktErr != nil && err == nil {
			err = pktErr
		}
		if pending > 0 {
			blocked[bestIdx] = true // head not complete: blocks this stream until next call
			continue
		}
		ps.queue = ps.queue[1:]
		retired++
	}
}

// CancelPaced discards all queued packet copies of the stream and revokes its pacing
// configuration. Sender statistics gathered so far stay unchanged. Packets that have
// not started to send never become "sent" because of a cancel.
//
//   streamIndex - the index of the output stream as returned by NewSsrcStreamOut
//
func (rs *Session) CancelPaced(streamIndex uint32) {
	delete(rs.pacedStreams, streamIndex)
}

// cancelAllPaced releases all paced queues of the session. Called during CloseSession,
// also if the RTCP service was never started.
func (rs *Session) cancelAllPaced() {
	for idx := range rs.pacedStreams {
		delete(rs.pacedStreams, idx)
	}
}

// freezeTargets snapshots the current remotes in ascending index order. Called once
// when the packet is really sent for the first time.
func (pp *pacedPacket) freezeTargets(rs *Session) {
	pp.frozen = true
	if len(rs.remotes) == 0 {
		return
	}
	idxs := make([]uint32, 0, len(rs.remotes))
	for idx := range rs.remotes {
		idxs = append(idxs, idx)
	}
	sort.Slice(idxs, func(i, j int) bool { return idxs[i] < idxs[j] })
	pp.targets = make([]pacedTarget, 0, len(idxs))
	for _, idx := range idxs {
		addr := *rs.remotes[idx]
		if addr.IPAddr != nil {
			addr.IPAddr = append(net.IP(nil), addr.IPAddr...)
		}
		pp.targets = append(pp.targets, pacedTarget{index: idx, addr: addr})
	}
}

// countPacedSent updates the sender statistics of the output stream once per packet,
// when the first target write of the packet succeeds.
func (rs *Session) countPacedSent(streamIndex uint32, payloadLen int, nowNano int64) {
	strOut := rs.streamsOut[streamIndex]
	if strOut == nil {
		return
	}
	strOut.SenderPacketCnt++
	strOut.SenderOctectCnt += uint32(payloadLen)

	strOut.streamMutex.Lock()
	if !strOut.sender && rs.rtcpCtrlChan != nil {
		// Non-blocking: must not stall if no RTCP service consumes the channel.
		select {
		case rs.rtcpCtrlChan <- rtcpIncrementSender:
		default:
		}
		strOut.sender = true
	}
	strOut.statistics.lastPacketTime = nowNano
	strOut.streamMutex.Unlock()
	rs.weSent = true
}
