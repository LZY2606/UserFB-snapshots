/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2020 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

//
// Live message mode extensions for the receiver buffer:
// message-aware too-late packet drop (TLPKTDROP).
//
// In live mode a single message (e.g. one compressed video frame) may consist
// of several packets. When the play time of an incomplete head message has
// passed, the whole message has to be skipped as a unit - dropping only a part
// of it would leave a tail of continuation packets blocking the head of the
// buffer, and must never swallow packets of the following messages either.
//

#include "buffer_rcv.h"

using namespace srt::sync;

namespace srt {

int32_t CRcvBuffer::getOverdueMsgDropSeqNo(time_point time_now) const
{
    // This function makes sense only for the live message mode, where every
    // message keeps its boundary flags and all packets of one message share
    // the same message number and the same play time.
    SRT_ASSERT(m_bMessageAPI && m_tsbpd.isEnabled());

    if (m_iMaxPosOff == 0)
        return SRT_SEQNO_NONE; // The buffer is empty.

    if (hasReadableInorderPkts())
        return SRT_SEQNO_NONE; // The head message is complete - it is to be delivered, not dropped.

    // Find the first existing packet in the buffer. Packets missing before it
    // belong to the head message(s) and are covered by the resulting drop range.
    const int end_pos = incPos(m_iStartPos, m_iMaxPosOff);
    int       first   = m_iStartPos;
    for (; first != end_pos; first = incPos(first))
    {
        if (m_entries[first].pUnit)
            break;
    }

    if (first == end_pos)
        return SRT_SEQNO_NONE; // No packets in the buffer at all.

    const CPacket& firstpkt = packetAt(first);

    // The play time of the head message has not come yet - it may still get
    // completed by retransmission, so don't drop anything now.
    if (getPktTsbPdTime(firstpkt.getMsgTimeStamp()) > time_now)
        return SRT_SEQNO_NONE;

    // If the first existing packet starts a fresh message and there is a gap
    // before it, the missing head packets belong to earlier message(s), which
    // can be dropped up to this packet. The message starting here is then
    // re-evaluated by the caller.
    if (first != m_iStartPos && (firstpkt.getMsgBoundary() & PB_FIRST))
        return firstpkt.getSeqNo();

    // The head message is incomplete (its FIRST packet may be missing as well).
    // Find its end: either its LAST packet, or the first packet of a following
    // message - packets of one message occupy a contiguous sequence range, so
    // any packet with a different message number bounds the head message.
    const int32_t msgno = firstpkt.getMsgSeq(m_bPeerRexmitFlag);
    for (int i = first; i != end_pos; i = incPos(i))
    {
        if (!m_entries[i].pUnit)
            continue;

        const CPacket& pkt = packetAt(i);
        if (pkt.getMsgSeq(m_bPeerRexmitFlag) != msgno)
        {
            // A following message has already started: drop everything before it.
            return pkt.getSeqNo();
        }

        if (pkt.getMsgBoundary() & PB_LAST)
        {
            // The LAST packet of the head message is known: drop the whole message.
            return CSeqNo::incseq(pkt.getSeqNo());
        }
    }

    // The end of the head message cannot be determined from the packets
    // currently held. Wait for new packets to provide the boundary instead of
    // spinning around the already passed deadline.
    return SRT_SEQNO_NONE;
}

} // namespace srt
