/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2026 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "platform_sys.h"

#include "api.h"
#include "core.h"
#include "logging.h"

namespace srt
{

using sync::ScopedLock;

int CUDT::dropmsg(SRTSOCKET u, int32_t msgno)
{
    try
    {
        // Bonding groups have their own message ownership and sender buffers.
        // This operation is intentionally supported on ordinary sockets only.
        if (u != SRT_INVALID_SOCK && (u & SRTGROUP_MASK))
            return APIError(MJ_NOTSUP, MN_INVAL, 0);

        return CUDT::uglobal().locateSocket(u, CUDTUnited::ERH_THROW)->core().dropmsg(msgno);
    }
    catch (const CUDTException& e)
    {
        return APIError(e);
    }
    catch (const std::exception& ee)
    {
        LOGC(aclog.Fatal, log << "dropmsg: UNEXPECTED EXCEPTION: " << typeid(ee).name() << ": " << ee.what());
        return APIError(MJ_UNKNOWN, MN_NONE, 0);
    }
}

int CUDT::dropmsg(int32_t msgno)
{
    // 0 is the protocol/control message number; all application message
    // numbers are in [1, MSGNO_SEQ_MAX].
    if (msgno < 1 || msgno > MSGNO_SEQ_MAX)
    {
        LOGC(aslog.Error, log << CONID() << "INVALID msgno " << msgno << " for srt_dropmsg");
        throw CUDTException(MJ_NOTSUP, MN_INVAL, 0);
    }

    if (m_bBroken || m_bClosing)
        throw CUDTException(MJ_CONNECTION, MN_CONNLOST, 0);
    if (!m_bConnected || !m_CongCtl.ready())
        throw CUDTException(MJ_CONNECTION, MN_NOCONN, 0);

    if (!m_config.bMessageAPI)
    {
        LOGC(aslog.Error, log << CONID() << "srt_dropmsg cannot be used with the byte-stream Buffer API");
        throw CUDTException(MJ_NOTSUP, MN_INVALBUFFERAPI, 0);
    }

    CSndBuffer::DropRange range;
    int dropped_bytes = 0;

    {
        // This is the same lock used by ACK processing, original scheduling and
        // loss-list processing. It prevents an ACK from reclaiming the blocks
        // while the DROPREQ range is being prepared. The blocks are not removed
        // here: the peer's ACK after processing DROPREQ performs that reclaim.
        ScopedLock ack_lock(m_RecvAckLock);
        dropped_bytes = m_pSndBuffer->dropMessage(msgno, (range));

        if (dropped_bytes > 0)
        {
            HLOGC(qslog.Debug,
                  log << CONID() << "dropmsg: #" << msgno << " %(" << range.seqno[CSndBuffer::DropRange::BEGIN]
                      << " - " << range.seqno[CSndBuffer::DropRange::END] << "), " << dropped_bytes
                      << " payload bytes newly revoked");
            sendCtrl(UMSG_DROPREQ, &range.msgno, range.seqno, sizeof(range.seqno));
        }
    }

    if (dropped_bytes > 0 && m_pSndQueue && m_pSndQueue->m_pSndUList)
    {
        // Let the sender thread promptly skip any fragments that had not yet
        // been originally extracted. Skipped sequence numbers still remain
        // outstanding until a real ACK advances the window.
        m_pSndQueue->m_pSndUList->update(this, CSndUList::DONT_RESCHEDULE);
    }

    return dropped_bytes;
}

} // namespace srt
