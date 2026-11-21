/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2023 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include "platform_sys.h"

#include "list.h"
#include "core.h"
#include "packet.h"
#include "logging.h"

// Cyclic paging of the receiver's periodic NAK LOSSREPORT. Everything in this
// file belongs to the periodic feedback path: immediate loss detection reports
// and the belated FreshLoss reports keep using the legacy code in core.cpp.

using namespace srt;
using namespace srt::sync;
using namespace srt_logging;

// ---------------------------------------------------------------------------
// CRcvLossList: cyclic report cursor and prepare/commit page tokens
// ---------------------------------------------------------------------------

int srt::CRcvLossList::findNodeForSeq(int32_t seqno) const
{
    // Same modular-slot lookup as in remove(): the node start maps to its
    // slot directly; a sequence covered by a range maps to the slot right
    // after the covered seqno, so the owning node is the nearest used one
    // walking backwards from that slot.
    const int offset = CSeqNo::seqoff(m_caSeq[m_iHead].seqstart, seqno);
    if (offset < 0)
        return -1;

    int loc = (m_iHead + offset) % m_iSize;
    if (m_caSeq[loc].seqstart != SRT_SEQNO_NONE && m_caSeq[loc].seqstart == seqno)
        return loc;

    int i = (loc - 1 + m_iSize) % m_iSize;
    while (SRT_SEQNO_NONE == m_caSeq[i].seqstart)
        i = (i - 1 + m_iSize) % m_iSize;

    if (m_caSeq[i].seqend == SRT_SEQNO_NONE || CSeqNo::seqcmp(seqno, m_caSeq[i].seqend) > 0
        || CSeqNo::seqcmp(seqno, m_caSeq[i].seqstart) < 0)
    {
        return -1;
    }
    return i;
}

void srt::CRcvLossList::reanchorReportCursor()
{
    if (m_iLength == 0)
    {
        m_iReportCursor = SRT_SEQNO_NONE;
        return;
    }

    if (m_iReportCursor == SRT_SEQNO_NONE)
    {
        // Fresh start (the list was emptied and has been filled again).
        m_iReportCursor = m_caSeq[m_iHead].seqstart;
        return;
    }

    // Keep the cursor if it still names a lost packet; otherwise advance to
    // the first still-existing loss at or after it in modular order.
    if (findNodeForSeq(m_iReportCursor) != -1)
        return;

    const int32_t saved = m_iReportCursor;
    int p = m_iHead;
    while (p != -1 && CSeqNo::seqoff(saved, m_caSeq[p].seqstart) < 0)
        p = m_caSeq[p].inext;

    // No successor in this round: wrap back to the head.
    m_iReportCursor = (p != -1) ? m_caSeq[p].seqstart : m_caSeq[m_iHead].seqstart;
}

srt::CRcvLossList::LossReportPage srt::CRcvLossList::prepareLossReport(size_t max_words) const
{
    LossReportPage page;
    page.m_owner      = this;
    page.m_generation = m_ullReportGeneration;
    page.m_committed  = false;
    page.m_lastSeq    = SRT_SEQNO_NONE;

    if (max_words == 0 || m_iLength == 0)
        return page;

    // The traversal starts at the cursor. An unset cursor (or a cursor whose
    // packet got recovered) is anchored at the head for this preparation.
    int32_t start = m_iReportCursor;
    if (start == SRT_SEQNO_NONE || findNodeForSeq(start) == -1)
        start = m_caSeq[m_iHead].seqstart;

    int loc = findNodeForSeq(start);
    SRT_ASSERT(loc != -1);

    // First seqno to cover inside the current node (the cursor may sit in the
    // middle of a range node).
    int32_t from    = start;
    bool    wrapped = false;
    size_t  budget  = max_words;
    int32_t last_represented = SRT_SEQNO_NONE;

    while (loc != -1 && budget > 0)
    {
        const int32_t node_start = m_caSeq[loc].seqstart;
        const int32_t node_full_end = (m_caSeq[loc].seqend != SRT_SEQNO_NONE)
                                          ? m_caSeq[loc].seqend
                                          : node_start;

        if (wrapped)
        {
            // After one wrap only seqnos modularly older than 'start' may
            // still be covered. A node starting at/after 'start' ends the
            // round (the starting node itself begins before 'start').
            if (CSeqNo::seqoff(start, node_start) >= 0)
                break;
        }

        // On the wrapped lap the starting node must only be covered up to the
        // seqno immediately before the starting seqno.
        int32_t limit = node_full_end;
        if (wrapped && CSeqNo::seqcmp(node_start, start) <= 0
            && CSeqNo::seqcmp(node_full_end, start) >= 0)
        {
            limit = CSeqNo::decseq(start);
        }

        if (budget == 1)
        {
            // One word left: report the segment head alone. When the segment
            // continues, the next page resumes right inside it.
            page.m_words.push_back(from);
            --budget;
            last_represented = from;

            if (CSeqNo::seqcmp(from, limit) >= 0)
            {
                loc = m_caSeq[loc].inext;
                if (loc == -1 && !wrapped)
                {
                    wrapped = true;
                    loc = m_iHead;
                }
                if (loc != -1)
                    from = m_caSeq[loc].seqstart;
            }
            else
            {
                break; // budget exhausted in the middle of the segment
            }
            continue;
        }

        // At least two words remain: emit the current continuous segment as a
        // range when it covers more than one seqno, otherwise as a single word,
        // greedily merging following adjacent segments. The range encoding
        // costs two words regardless of how far it reaches.
        int     run_loc  = loc;
        int32_t run_end  = limit;
        for (;;)
        {
            const int next = m_caSeq[run_loc].inext;
            if (next == -1)
                break;

            const int32_t next_start = m_caSeq[next].seqstart;
            if (CSeqNo::incseq(run_end) != next_start)
                break;

            const int32_t next_end = (m_caSeq[next].seqend != SRT_SEQNO_NONE)
                                         ? m_caSeq[next].seqend
                                         : next_start;

            // On the wrapped lap never swallow the starting seqno again.
            if (wrapped && CSeqNo::seqcmp(next_start, start) >= 0
                && CSeqNo::seqoff(start, next_start) >= 0)
                break;
            if (wrapped && CSeqNo::seqcmp(next_start, start) <= 0
                && CSeqNo::seqcmp(next_end, start) >= 0)
            {
                run_end = CSeqNo::decseq(start);
                run_loc = next;
                break;
            }

            run_end = next_end;
            run_loc = next;
        }

        if (CSeqNo::seqcmp(run_end, from) == 0)
        {
            page.m_words.push_back(from);
            --budget;
        }
        else
        {
            page.m_words.push_back(from | LOSSDATA_SEQNO_RANGE_FIRST);
            page.m_words.push_back(run_end);
            budget -= 2;
        }
        last_represented = run_end;

        loc = m_caSeq[run_loc].inext;
        if (loc == -1)
        {
            if (wrapped)
                break; // covered the whole ring
            wrapped = true;
            loc = m_iHead;
        }
        if (loc != -1)
            from = m_caSeq[loc].seqstart;
    }

    page.m_lastSeq = last_represented;
    return page;
}

bool srt::CRcvLossList::commitLossReport(const LossReportPage& page)
{
    // Empty pages, foreign pages, pages prepared against an older state and
    // pages already committed are rejected without touching the cursor.
    if (page.m_owner != this)
        return false;
    if (page.m_generation != m_ullReportGeneration)
        return false;
    if (page.m_committed)
        return false;
    if (page.m_words.empty())
        return false;

    // The state generation is bumped as well so that committing one page
    // invalidates every other outstanding page, and so that the committed
    // page itself cannot be committed a second time.
    ++m_ullReportGeneration;
    page.m_committed = true;

    // Advance past the last represented seqno, then re-anchor in case the
    // successor has been recovered while the page was in flight.
    m_iReportCursor = CSeqNo::incseq(page.m_lastSeq);
    reanchorReportCursor();
    return true;
}

// ---------------------------------------------------------------------------
// CUDT: periodic LOSSREPORT scheduling, sending and commit/rollback
// ---------------------------------------------------------------------------

int srt::CUDT::defaultControlSend(const sockaddr_any& addr, CPacket& packet, const sockaddr_any& src)
{
    // Unlike CSndQueue::sendto(), this propagates the real CChannel result,
    // which is what the periodic NAK path needs to decide on commit/rollback.
    return m_pSndQueue->sendtoResult(addr, packet, src);
}

void srt::CUDT::processPeriodicLossReport(const steady_clock::time_point& currtime, const SendCallback& sink)
{
    // Build the packet outside the loss-list traversal, exactly like sendCtrl().
    CPacket ctrlpkt;
    setPacketTS(ctrlpkt, currtime);

    ScopedLock lock(m_RcvLossLock);
    if (m_pRcvLossList->getLossLength() == 0)
        return;

    // One report period sends at most one page, sized by the control-packet
    // word budget floor(m_iMaxDataPayloadSize / 4).
    const size_t max_words = m_iMaxDataPayloadSize / sizeof(int32_t);
    CRcvLossList::LossReportPage page = m_pRcvLossList->prepareLossReport(max_words);
    if (page.words().empty())
        return;

    // Same control header and host-order words as the legacy NAK path.
    ctrlpkt.pack(UMSG_LOSSREPORT, NULL, const_cast<int32_t*>(page.words().data()),
                 page.words().size() * sizeof(int32_t));
    ctrlpkt.set_id(m_PeerID);

    // The sink receives the fully packed CPacket (CChannel::sendto performs the
    // network byte-order conversion). If the loss list changed while/after
    // sending, the snapshot is still allowed to go out, but the commit below
    // will fail and the page gets re-prepared next period.
    const SendCallback active = sink ? sink
                                     : SendCallback(std::bind(&CUDT::defaultControlSend, this,
                                                              std::placeholders::_1, std::placeholders::_2,
                                                              std::placeholders::_3));
    const int nbsent = active(m_PeerAddr, ctrlpkt, m_SourceAddr);
    if (nbsent < 0)
    {
        // Send failure: do not advance the report cursor and do not count it.
        HLOGC(qrlog.Warn, log << CONID() << "periodic LOSSREPORT: send failed, page kept for the next period");
        return;
    }

    // Keep the same "last send" bookkeeping as sendCtrl(): successful control
    // traffic delays keepalive handling.
    m_tsLastSndTime.store(currtime);

    enterCS(m_StatsLock);
    m_stats.rcvr.sentNak.count(1);
    leaveCS(m_StatsLock);

    // Commit is rejected if the list changed since preparation (including
    // another commit); the cursor then stays where it was.
    if (!m_pRcvLossList->commitLossReport(page))
    {
        HLOGC(qrlog.Debug, log << CONID() << "periodic LOSSREPORT: page stale at commit, will re-prepare");
    }
}

int srt::CUDT::checkNAKTimer(const steady_clock::time_point& currtime)
{
    return checkNAKTimer(currtime, SendCallback());
}

int srt::CUDT::checkNAKTimer(const steady_clock::time_point& currtime, const SendCallback& sink)
{
    // XXX The problem with working NAKREPORT with SRT_ARQ_ONREQ
    // is not that it would be inappropriate, but because it's not
    // implemented. The reason for it is that the structure of the
    // loss list container (m_pRcvLossList) is such that it is expected
    // that the loss records are ordered by sequence numbers (so
    // that two ranges sticking together are merged in place).
    // Unfortunately in case of SRT_ARQ_ONREQ losses must be recorded
    // as before, but they should not be reported, until confirmed
    // by the filter. By this reason they appear often out of order
    // and for adding them properly the loss list container wasn't
    // prepared. This then requires some more effort to implement.
    if (!m_config.bRcvNakReport || m_PktFilterRexmitLevel != SRT_ARQ_ALWAYS)
        return BECAUSE_NO_REASON;

    /*
     * m_config.bRcvNakReport enables NAK reports for SRT.
     * Retransmission based on timeout is bandwidth consuming,
     * not knowing what to retransmit when the only NAK sent by receiver is lost,
     * all packets past last ACK are retransmitted (rexmitMethod() == SRM_FASTREXMIT).
     */
    enterCS(m_RcvLossLock);
    const int loss_len = m_pRcvLossList->getLossLength();
    leaveCS(m_RcvLossLock);

    SRT_ASSERT(loss_len >= 0);
    int debug_decision = BECAUSE_NO_REASON;

    if (loss_len > 0)
    {
        if (currtime <= m_tsNextNAKTime.load())
            return BECAUSE_NO_REASON; // wait for next NAK time

        // One period sends at most one cyclic page. The default sink goes
        // through the real send queue; tests inject an in-memory sink.
        processPeriodicLossReport(currtime, sink);
        debug_decision = BECAUSE_NAKREPORT;

        // Recalculate the next period from RTT and the congestion controller,
        // exactly as the legacy sendCtrl(UMSG_LOSSREPORT) path did.
        updateNakInterval();
    }

    // The interval advances even if the send failed: a failed report is
    // retried on the next period instead of spinning in the current one.
    m_tsNextNAKTime.store(currtime + m_tdNAKInterval);
    return debug_decision;
}

void srt::CUDT::updateNakInterval()
{
    // update next NAK time, which should wait enough time for the
    // retransmission, but not too long
    m_tdNAKInterval = microseconds_from(m_iSRTT + 4 * m_iRTTVar);

    // Fix the NAKreport period according to the congctl
    m_tdNAKInterval =
        microseconds_from(m_CongCtl->updateNAKInterval(count_microseconds(m_tdNAKInterval),
                                                                      m_RcvTimeWindow.getPktRcvSpeed(),
                                                                      m_pRcvLossList->getLossLength()));

    // This is necessary because a congctl need not wish to define
    // its own minimum interval, in which case the default one is used.
    if (m_tdNAKInterval < m_tdMinNakInterval)
        m_tdNAKInterval = m_tdMinNakInterval;
}
