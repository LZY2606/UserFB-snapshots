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
 * This source file implements the RFC 3611 RTCP XR blocks required to measure
 * the round trip time on receivers that do not send RTP data themselves:
 * the Receiver Reference Time Report block (RRTR) and the DLRR report block.
 */

import (
	"encoding/binary"
	"math"
	"time"
)

// RTCP XR block types, see RFC 3611.
const (
	xrBlockRRTR = 4 // Receiver Reference Time Report block
	xrBlockDLRR = 5 // Delay since Last Receiver Reference Time Report block
)

// xrMaxRefAge is the maximum age of a sent reference timestamp that is still
// eligible to produce a RTT sample. Older references are discarded.
const xrMaxRefAge = time.Minute

// xrSentRef records one reference timestamp this session sent in a RRTR block.
type xrSentRef struct {
	lrr    uint32    // compact NTP form (middle 32 bits) of the sent reference
	sentAt time.Time // sampling instant taken from the injected XR clock
}

// xrPeerRef records the newest reference timestamp received from a peer and
// the local time this reference value arrived first.
type xrPeerRef struct {
	sec, frac uint32    // full NTP timestamp of the peer's reference
	arrival   time.Time // first arrival of this reference value (XR clock)
}

// lrr returns the compact (middle 32 bit) NTP form of the reference.
func (r *xrPeerRef) lrr() uint32 {
	return r.sec<<16 | r.frac>>16
}

// xrRTTKey identifies one RTT association: one of our local SSRCs and the
// SSRC of the peer that answered our reference timestamp.
type xrRTTKey struct {
	local, peer uint32
}

// xrRTTSample is the latest valid round trip time of an association.
type xrRTTSample struct {
	rtt       time.Duration
	refSentAt time.Time // send time of the reference that produced this sample
}

// xrDlrrSub is one decoded DLRR sub-block.
type xrDlrrSub struct {
	ssrc uint32 // receiver SSRC: the sender of the referenced timestamp
	lrr  uint32 // compact NTP of the reference being answered
	dlrr uint32 // delay since the reference arrived, in 1/65536 seconds
}

// xrPacketIn holds the validated XR content of one received XR packet. The
// content is committed to the session state only after every XR block of the
// datagram was validated.
type xrPacketIn struct {
	reporter uint32 // SSRC of the XR packet sender
	hasRef   bool
	refSec   uint32
	refFrac  uint32
	dlrrs    []xrDlrrSub
}

// EnableXR switches the RFC 3611 receiver reference time exchange on or off.
//
// Call this method before StartSession. If enabled, every periodic RTCP
// compound carries an XR packet with a RRTR block (and DLRR answers to
// references received from peers), so that pure receivers can measure the
// round trip time to their media peers. XR is disabled by default.
func (rs *Session) EnableXR(on bool) {
	rs.xrEnabled = on
}

// SetXRClock installs the clock used for XR reference timestamps and RTT
// durations. Call this method before StartSession. If not set, time.Now is
// used.
func (rs *Session) SetXRClock(clock func() time.Time) {
	rs.xrClock = clock
}

// XRRTT returns the latest round trip time measured for the association
// between one of the session's own (output) SSRCs and a peer SSRC. The
// second return value is false if no valid measurement exists.
func (rs *Session) XRRTT(localSSRC, peerSSRC uint32) (time.Duration, bool) {
	rs.xrMutex.Lock()
	defer rs.xrMutex.Unlock()
	sample, ok := rs.xrRTTs[xrRTTKey{localSSRC, peerSSRC}]
	if !ok {
		return 0, false
	}
	return sample.rtt, true
}

// xrNow returns the current time of the injected XR clock.
func (rs *Session) xrNow() time.Time {
	if rs.xrClock != nil {
		return rs.xrClock()
	}
	return time.Now()
}

// xrInitLocked creates the XR maps if necessary (sessions created via
// NewSession get them up front, this covers manually built sessions).
// Must be called with xrMutex held.
func (rs *Session) xrInitLocked() {
	if rs.xrSentRefs == nil {
		rs.xrSentRefs = make(map[uint32][]xrSentRef)
	}
	if rs.xrPeerRefs == nil {
		rs.xrPeerRefs = make(map[uint32]*xrPeerRef)
	}
	if rs.xrRTTs == nil {
		rs.xrRTTs = make(map[xrRTTKey]xrRTTSample)
	}
}

// xrCompact converts a full NTP timestamp into its compact middle 32 bit form.
func xrCompact(sec, frac uint32) uint32 {
	return sec<<16 | frac>>16
}

// xrDurationUnits converts a duration into units of 1/65536 seconds, rounding down.
func xrDurationUnits(d time.Duration) int64 {
	return d.Nanoseconds() * 65536 / 1e9
}

// addXr appends an RTCP XR packet to the compound control packet.
//
// The XR packet always carries a RRTR block with a fresh reference timestamp
// taken from the XR clock and, if references from peers are known, a DLRR
// block answering them. The sent reference is recorded so that matching DLRR
// answers can be turned into RTT samples later.
func (rs *Session) addXr(strOut *SsrcStream, rc *CtrlPacket) {
	now := rs.xrNow()
	sec, frac := toNtpStamp(now.UnixNano())
	local := strOut.Ssrc()

	rs.xrMutex.Lock()
	rs.xrInitLocked()

	// Record the reference we are about to send; drop references that ran
	// out of the eligibility window.
	refs := rs.xrSentRefs[local]
	kept := refs[:0]
	for _, r := range refs {
		if now.Sub(r.sentAt) <= xrMaxRefAge {
			kept = append(kept, r)
		}
	}
	refs = append(kept, xrSentRef{xrCompact(sec, frac), now})
	rs.xrSentRefs[local] = refs

	// Answer the newest reference of every known peer.
	subs := make([]xrDlrrSub, 0, len(rs.xrPeerRefs))
	for peer, ref := range rs.xrPeerRefs {
		wait := now.Sub(ref.arrival)
		if wait < 0 {
			wait = 0
		}
		units := xrDurationUnits(wait)
		if units > math.MaxUint32 {
			units = math.MaxUint32
		}
		subs = append(subs, xrDlrrSub{peer, ref.lrr(), uint32(units)})
	}
	rs.xrMutex.Unlock()

	need := 8 + 12 // XR header plus SSRC, RRTR block
	if len(subs) > 0 {
		need += 4 + 12*len(subs)
	}
	offset := rc.InUse()
	if offset+need > len(rc.Buffer()) {
		return
	}
	rc.addHeaderCtrl(offset) // V=2, P=0, reserved count zero
	rc.SetType(offset, RtcpXr)
	rc.addHeaderSsrc(offset+rtcpHeaderLength, local)

	blk := offset + rtcpHeaderLength + rtcpSsrcLength
	rc.buffer[blk] = xrBlockRRTR
	rc.buffer[blk+1] = 0
	binary.BigEndian.PutUint16(rc.buffer[blk+2:], 2)
	binary.BigEndian.PutUint32(rc.buffer[blk+4:], sec)
	binary.BigEndian.PutUint32(rc.buffer[blk+8:], frac)
	rc.inUse += 12

	if len(subs) > 0 {
		blk += 12
		rc.buffer[blk] = xrBlockDLRR
		rc.buffer[blk+1] = 0
		binary.BigEndian.PutUint16(rc.buffer[blk+2:], uint16(3*len(subs)))
		pos := blk + 4
		for _, sub := range subs {
			binary.BigEndian.PutUint32(rc.buffer[pos:], sub.ssrc)
			binary.BigEndian.PutUint32(rc.buffer[pos+4:], sub.lrr)
			binary.BigEndian.PutUint32(rc.buffer[pos+8:], sub.dlrr)
			pos += 12
		}
		rc.inUse += 4 + 12*len(subs)
	}
	rc.SetLength(offset, uint16((rc.InUse()-offset)/4-1))
}

// parseXrPacket validates the XR packet starting at offset and returns its
// decoded content. It returns false if the packet or any of its blocks is
// corrupt; in that case none of the datagram's XR state must be committed.
func (rs *Session) parseXrPacket(rp *CtrlPacket, offset, pktLen int) (*xrPacketIn, bool) {
	if pktLen < rtcpHeaderLength+rtcpSsrcLength || offset+pktLen > rp.inUse {
		return nil, false
	}
	limit := offset + pktLen
	if rp.buffer[offset]&paddingBit != 0 {
		// Normal RTCP padding is only valid on the last packet of the
		// compound and its count must fit into this packet.
		if offset+pktLen != rp.inUse {
			return nil, false
		}
		pad := int(rp.buffer[rp.inUse-1])
		if pad == 0 || pad > pktLen-rtcpHeaderLength {
			return nil, false
		}
		limit -= pad
	}
	if limit < offset+rtcpHeaderLength+rtcpSsrcLength {
		return nil, false
	}
	out := &xrPacketIn{reporter: rp.Ssrc(offset)}
	for blk := offset + rtcpHeaderLength + rtcpSsrcLength; blk < limit; {
		if blk+4 > limit {
			return nil, false
		}
		bt := rp.buffer[blk]
		blkLen := int(binary.BigEndian.Uint16(rp.buffer[blk+2:])) * 4
		if blk+4+blkLen > limit {
			return nil, false
		}
		body := blk + 4
		switch bt {
		case xrBlockRRTR:
			if blkLen != 8 {
				return nil, false
			}
			out.hasRef = true
			out.refSec = binary.BigEndian.Uint32(rp.buffer[body:])
			out.refFrac = binary.BigEndian.Uint32(rp.buffer[body+4:])
		case xrBlockDLRR:
			if blkLen%12 != 0 {
				return nil, false
			}
			for pos := body; pos < body+blkLen; pos += 12 {
				out.dlrrs = append(out.dlrrs, xrDlrrSub{
					ssrc: binary.BigEndian.Uint32(rp.buffer[pos:]),
					lrr:  binary.BigEndian.Uint32(rp.buffer[pos+4:]),
					dlrr: binary.BigEndian.Uint32(rp.buffer[pos+8:]),
				})
			}
		default:
			// Unknown block types are skipped using their declared length.
		}
		blk += 4 + blkLen
	}
	return out, true
}

// xrCommit applies the validated XR content of one received datagram to the
// session state. arrival is the datagram's arrival instant on the XR clock.
func (rs *Session) xrCommit(pending []xrPacketIn, arrival time.Time) {
	if len(pending) == 0 {
		return
	}
	rs.xrMutex.Lock()
	defer rs.xrMutex.Unlock()
	rs.xrInitLocked()
	for i := range pending {
		pkt := &pending[i]
		if pkt.hasRef {
			lrr := xrCompact(pkt.refSec, pkt.refFrac)
			cur, ok := rs.xrPeerRefs[pkt.reporter]
			switch {
			case !ok:
				rs.xrPeerRefs[pkt.reporter] = &xrPeerRef{pkt.refSec, pkt.refFrac, arrival}
			case cur.lrr() == lrr:
				// Same reference value again: keep the first arrival,
				// the wait interval must not be restarted.
			case pkt.refSec > cur.sec || (pkt.refSec == cur.sec && pkt.refFrac > cur.frac):
				*cur = xrPeerRef{pkt.refSec, pkt.refFrac, arrival}
			}
			// References older than the recorded one are ignored.
		}
		for _, sub := range pkt.dlrrs {
			rs.xrMatchDlrr(pkt.reporter, sub, arrival)
		}
	}
}

// xrMatchDlrr matches one received DLRR sub-block against the references this
// session sent and, on a valid match, records a RTT sample. Must be called
// with xrMutex held.
func (rs *Session) xrMatchDlrr(peer uint32, sub xrDlrrSub, now time.Time) {
	refs := rs.xrSentRefs[sub.ssrc]
	for i := len(refs) - 1; i >= 0; i-- {
		ref := refs[i]
		if ref.lrr != sub.lrr {
			continue
		}
		// Found the reference this answer belongs to. Only references sent
		// within the last minute are eligible.
		elapsed := now.Sub(ref.sentAt)
		if elapsed < 0 || elapsed > xrMaxRefAge {
			return
		}
		// RTT at wire resolution: local elapsed time minus the peer's wait.
		rttUnits := xrDurationUnits(elapsed) - int64(sub.dlrr)
		if rttUnits < 0 {
			return
		}
		key := xrRTTKey{sub.ssrc, peer}
		if cur, ok := rs.xrRTTs[key]; ok && !ref.sentAt.After(cur.refSentAt) {
			// A sample from this or a newer reference exists already; a
			// late answer to an old reference must not overwrite it.
			return
		}
		rs.xrRTTs[key] = xrRTTSample{
			rtt:       time.Duration(rttUnits * 1e9 / 65536),
			refSentAt: ref.sentAt,
		}
		return
	}
}

// xrPeerGone drops all XR state associated with a peer SSRC, for example
// after a BYE was received from that peer.
func (rs *Session) xrPeerGone(peer uint32) {
	rs.xrMutex.Lock()
	defer rs.xrMutex.Unlock()
	delete(rs.xrPeerRefs, peer)
	for key := range rs.xrRTTs {
		if key.peer == peer {
			delete(rs.xrRTTs, key)
		}
	}
}

// xrLocalGone drops all XR state associated with a local (output) SSRC, for
// example after the stream was closed. A stream reusing the SSRC later must
// not inherit old RTT samples.
func (rs *Session) xrLocalGone(local uint32) {
	rs.xrMutex.Lock()
	defer rs.xrMutex.Unlock()
	delete(rs.xrSentRefs, local)
	for key := range rs.xrRTTs {
		if key.local == local {
			delete(rs.xrRTTs, key)
		}
	}
}

// xrReset drops all XR state, used when the session is closed.
func (rs *Session) xrReset() {
	rs.xrMutex.Lock()
	defer rs.xrMutex.Unlock()
	rs.xrSentRefs = make(map[uint32][]xrSentRef)
	rs.xrPeerRefs = make(map[uint32]*xrPeerRef)
	rs.xrRTTs = make(map[xrRTTKey]xrRTTSample)
}
