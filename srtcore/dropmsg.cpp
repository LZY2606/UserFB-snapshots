/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2024 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include "platform_sys.h"

#include <exception>
#include <typeinfo>

#include "api.h"
#include "core.h"
#include "buffer_snd.h"
#include "common.h"
#include "packet.h"
#include "queue.h"
#include "logging.h"
#include "srt.h"

using namespace std;
using namespace srt;
using namespace srt_logging;
using namespace srt::sync;

// Revocation of an already enqueued message from the sender side.
//
// The heavy lifting lives in CSndBuffer::dropMessage(), which only flags the
// blocks. This file wires the flag into the connection: validates the socket
// state and the message number, and emits the UMSG_DROPREQ control packet that
// lets the peer skip the resulting sequence gap.
//
// Design invariants (see also srt.h):
// - revocation is not an ACK: blocks stay in the sender buffer, window slots
//   stay occupied and no ACK statistics are produced. Real ACKs recycle the
//   flagged blocks through the normal CSndBuffer::ackData() path;
// - retransmission requests for flagged blocks are answered with DROPREQ
//   (CSndBuffer::readData -> READ_DROP -> CUDT::extractCleanRexmitPacket),
//   so the original payload is never sent again, even if this DROPREQ is lost;
// - only sequence numbers belonging to the revoked message are affected.

int srt::CUDT::dropmsg(int32_t msgno)
{
    // Only connected, message-API, single sockets support message revocation.
    if (m_bBroken || m_bClosing)
        throw CUDTException(MJ_CONNECTION, MN_CONNLOST, 0);
    else if (!m_bConnected || !m_CongCtl.ready())
        throw CUDTException(MJ_CONNECTION, MN_NOCONN, 0);

#if ENABLE_BONDING
    if (m_parent->m_GroupOf)
        throw CUDTException(MJ_NOTSUP, MN_INVAL, 0);
#endif

    if (!m_config.bMessageAPI)
        throw CUDTException(MJ_NOTSUP, MN_INVALBUFFERAPI, 0);

    // 0 is the control message number, -1 (and any negative value) is not a
    // valid user message number; the positive range is [1, MSGNO_SEQ_MAX].
    if (msgno < 1 || msgno > MSGNO_SEQ_MAX)
        throw CUDTException(MJ_NOTSUP, MN_INVAL, 0);

    int32_t seqlo = SRT_SEQNO_NONE;
    int32_t seqhi = SRT_SEQNO_NONE;
    int     bytes = 0;

    {
        ScopedLock ack_lock(m_RecvAckLock);

        const CSndBuffer::DropMsgResult drop = m_pSndBuffer->dropMessage(msgno);
        if (!drop.found)
        {
            // Nothing of this message remains in the sender buffer: it was
            // fully ACK-recycled (or the msgno is outside the live range).
            // This is a successful no-op, not an error.
            return 0;
        }

        bytes = drop.bytes;
        seqlo = drop.seqlo;
        seqhi = drop.seqhi;

        // Pending retransmission records are NOT removed here and no window
        // slot is released: revocation is not an ACK. When the sender thread
        // next services these sequences, CSndBuffer::readData reports
        // READ_DROP for the revoked range, extractCleanRexmitPacket() sends
        // another DROPREQ and trims the loss list exactly at the message
        // boundary, leaving adjacent messages' records intact. Sequences the
        // peer stops asking about are recycled by the next real ACK.
    }

    // Notify the peer over the wire. This is intentionally done outside the
    // ACK lock: sendCtrl only touches the send queue, and a blocking send must
    // not hold internal locks. If this packet is lost, a later NAK for the
    // range resolves through CSndBuffer::readData -> READ_DROP -> DROPREQ.
    int32_t seqpair[2] = { seqlo, seqhi };
    sendCtrl(UMSG_DROPREQ, &msgno, seqpair, sizeof(seqpair));

    // Re-schedule the sender so that not-yet-sent revoked blocks are skipped
    // promptly and following messages keep flowing.
    m_pSndQueue->m_pSndUList->update(this, CSndUList::DONT_RESCHEDULE);

    return bytes;
}

int srt::CUDT::dropmsg(SRTSOCKET u, int32_t msgno)
{
    try
    {
#if ENABLE_BONDING
        if (u & SRTGROUP_MASK)
        {
            // Message revocation is defined for single connected sockets only.
            return APIError(MJ_NOTSUP, MN_INVAL, 0);
        }
#endif

        return uglobal().locateSocket(u, CUDTUnited::ERH_THROW)->core().dropmsg(msgno);
    }
    catch (const CUDTException& e)
    {
        return APIError(e);
    }
    catch (bad_alloc&)
    {
        return APIError(MJ_SYSTEMRES, MN_MEMORY, 0);
    }
    catch (const std::exception& ee)
    {
        LOGC(aclog.Fatal, log << "dropmsg: UNEXPECTED EXCEPTION: " << typeid(ee).name() << ": " << ee.what());
        return APIError(MJ_UNKNOWN, MN_NONE, 0);
    }
}
