/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2024 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

/*****************************************************************************
written by
   Haivision Systems Inc.
*****************************************************************************/

// Circular paging of the receiver's periodic loss report.
//
// The classic CRcvLossList::getLossArray() always starts encoding from the
// head of the list, so when early losses stay unrecovered the later gaps are
// never reported. This file implements a separate report cursor that walks the
// loss ranges in circular order, one prepared page per periodic report. The
// page is prepared without side effects and only committed once it has been
// sent successfully; see list.h for the full contract.

#include "platform_sys.h"

#include "list.h"
#include "packet.h"

namespace {

struct LossSegment
{
    int32_t lo;
    int32_t hi;
};

} // namespace

srt::CRcvLossList::LossReportPage::LossReportPage()
    : m_owner(NULL)
    , m_generation(0)
    , m_next_seqno(SRT_SEQNO_NONE)
    , m_words()
{
}

srt::CRcvLossList::LossReportPage srt::CRcvLossList::prepareLossReport(size_t max_words) const
{
    LossReportPage page;
    page.m_owner      = this;
    page.m_generation = m_uReportGeneration;
    page.m_next_seqno = SRT_SEQNO_NONE;

    if (max_words == 0 || m_iLength == 0 || m_iHead == -1)
        return page;

    // Collect the loss ranges in list order. The receiver loss list keeps its
    // entries ordered and coalesced, so every node is already a maximal
    // contiguous range (it may cross 0x7fffffff/0).
    std::vector<LossSegment> nodes;
    for (int p = m_iHead; p != -1; p = m_caSeq[p].inext)
    {
        LossSegment seg;
        seg.lo = m_caSeq[p].seqstart;
        seg.hi = (m_caSeq[p].seqend == SRT_SEQNO_NONE) ? m_caSeq[p].seqstart : m_caSeq[p].seqend;
        nodes.push_back(seg);
    }
    SRT_ASSERT(!nodes.empty());

    // Locate the node holding the report cursor, or the first node that
    // follows it in modular sequence order. If there is none, wrap to head.
    const int n      = (int)nodes.size();
    int       start_node         = 0;
    int32_t   start_seq          = nodes[0].lo;
    bool      start_at_node_begin = true;

    if (m_iReportCursor != SRT_SEQNO_NONE)
    {
        int  found  = -1;
        bool inside = false;
        for (int i = 0; i < n; ++i)
        {
            const LossSegment& seg = nodes[i];
            if (CSeqNo::seqcmp(m_iReportCursor, seg.lo) >= 0 && CSeqNo::seqcmp(seg.hi, m_iReportCursor) >= 0)
            {
                found  = i;
                inside = true;
                break;
            }
            if (CSeqNo::seqcmp(seg.lo, m_iReportCursor) > 0)
            {
                found = i; // first range that lies after the cursor
                break;
            }
        }

        if (found != -1)
        {
            start_node          = found;
            start_seq           = inside ? m_iReportCursor : nodes[found].lo;
            start_at_node_begin = !inside || (m_iReportCursor == nodes[found].lo);
        }
        // else: no successor in the list - wrap to the head (start_node = 0).
    }

    // Build the ordered segments of a single lap, without repeating any
    // sequence number. When the cursor sits inside a range, that range is
    // split into a tail (first) and a head (last) which wraps the lap.
    std::vector<LossSegment> lap;
    if (start_at_node_begin)
    {
        for (int k = 0; k < n; ++k)
            lap.push_back(nodes[(start_node + k) % n]);
    }
    else
    {
        LossSegment head;
        head.lo = start_seq;
        head.hi = nodes[start_node].hi;
        lap.push_back(head);

        for (int k = 1; k < n; ++k)
            lap.push_back(nodes[(start_node + k) % n]);

        LossSegment tail;
        tail.lo = nodes[start_node].lo;
        tail.hi = CSeqNo::decseq(start_seq);
        lap.push_back(tail);
    }

    // Fill the page greedily in circular order.
    int32_t last_seq = SRT_SEQNO_NONE;
    for (size_t i = 0; i < lap.size(); ++i)
    {
        const size_t budget = max_words - page.m_words.size();
        if (budget == 0)
            break;

        const LossSegment& seg = lap[i];
        if (budget == 1)
        {
            // Only one word left: emit the first sequence of this range;
            // the next page resumes inside it.
            page.m_words.push_back(seg.lo);
            last_seq = seg.lo;
            break;
        }

        // At least two words available: merge the whole contiguous range.
        if (seg.lo == seg.hi)
        {
            page.m_words.push_back(seg.lo);
            last_seq = seg.lo;
        }
        else
        {
            page.m_words.push_back(seg.lo | LOSSDATA_SEQNO_RANGE_FIRST);
            page.m_words.push_back(seg.hi);
            last_seq = seg.hi;
        }
    }

    if (last_seq != SRT_SEQNO_NONE)
        page.m_next_seqno = CSeqNo::incseq(last_seq);

    return page;
}

bool srt::CRcvLossList::commitLossReport(const LossReportPage& page)
{
    if (page.m_owner != this)
        return false;
    if (page.m_words.empty())
        return false;
    if (page.m_generation != m_uReportGeneration)
        return false;
    if (page.m_next_seqno == SRT_SEQNO_NONE)
        return false;

    // Advance to the strict modular successor of the last represented seqno.
    // Wrapping past the last sequence number simply means "back to the head",
    // which is also represented by the "no cursor" value.
    if (CSeqNo::seqcmp(page.m_next_seqno, m_iLargestSeq) > 0)
        m_iReportCursor = SRT_SEQNO_NONE;
    else
        m_iReportCursor = page.m_next_seqno;

    // Invalidate any other page prepared in the previous state, even if the
    // loss set happens to look identical afterwards.
    ++m_uReportGeneration;
    return true;
}
