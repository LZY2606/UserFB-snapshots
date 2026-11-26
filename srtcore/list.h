/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

/*****************************************************************************
Copyright (c) 2001 - 2011, The Board of Trustees of the University of Illinois.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the
  above copyright notice, this list of conditions
  and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the University of Illinois
  nor the names of its contributors may be used to
  endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*****************************************************************************/

/*****************************************************************************
written by
   Yunhong Gu, last updated 01/22/2011
modified by
   Haivision Systems Inc.
*****************************************************************************/

#ifndef INC_SRT_LIST_H
#define INC_SRT_LIST_H

#include <stdint.h>
#include <deque>
#include <vector>

#include "udt.h"
#include "common.h"
#include "utilities.h"

namespace srt {

class CSndLossList
{
public:
    CSndLossList(int size = 1024);
    ~CSndLossList();

    /// Insert a seq. no. into the sender loss list.
    /// @param [in] seqno1 sequence number starts.
    /// @param [in] seqno2 sequence number ends.
    /// @return number of packets that are not in the list previously.
    int insert(int32_t seqno1, int32_t seqno2);

    /// Remove the given sequence number and all numbers that precede it.
    /// @param [in] seqno sequence number.
    void removeUpTo(int32_t seqno);

    /// Read the loss length.∏
    /// @return The length of the list.
    int getLossLength() const;

    /// Read the first (smallest) loss seq. no. in the list and remove it.
    /// @return The seq. no. or -1 if the list is empty.
    int32_t popLostSeq();

    template <class Stream>
    Stream& traceState(Stream& sout) const
    {
        int pos = m_iHead;
        while (pos != SRT_SEQNO_NONE)
        {
            sout << "[" << pos << "]:" << m_caSeq[pos].seqstart;
            if (m_caSeq[pos].seqend != SRT_SEQNO_NONE)
                sout << ":" << m_caSeq[pos].seqend;
            if (m_caSeq[pos].inext == -1)
                sout << "=|";
            else
                sout << "->[" << m_caSeq[pos].inext << "]";
            sout << ", ";
            pos = m_caSeq[pos].inext;
        }
        sout << " {len:" << m_iLength << " head:" << m_iHead << " last:" << m_iLastInsertPos << "}";
        return sout;
    }
    void traceState() const;

    // Debug/unittest support.

    int head() const { return m_iHead; }
    int next(int loc) const { return m_caSeq[loc].inext; }
    int last() const { return m_iLastInsertPos; }

private:
    struct Seq
    {
        int32_t seqstart; // sequence number starts
        int32_t seqend;   // sequence number ends
        int     inext;    // index of the next node in the list
    } * m_caSeq;

    int       m_iHead;          // first node
    int       m_iLength;        // loss length
    const int m_iSize;          // size of the static array
    int       m_iLastInsertPos; // position of last insert node

    mutable srt::sync::Mutex m_ListLock; // used to synchronize list operation

private:
    /// Inserts an element to the beginning and updates head pointer.
    /// No lock.
    void insertHead(int pos, int32_t seqno1, int32_t seqno2);

    /// Inserts an element after previous element.
    /// No lock.
    void insertAfter(int pos, int pos_after, int32_t seqno1, int32_t seqno2);

    /// Check if it is possible to coalesce element at loc with further elements.
    /// @param loc - last changed location
    void coalesce(int loc);

    /// Update existing element with the new range (increase only)
    /// @param pos     position of the element being updated
    /// @param seqno1  first sequence number in range
    /// @param seqno2  last sequence number in range (SRT_SEQNO_NONE if no range)
    bool updateElement(int pos, int32_t seqno1, int32_t seqno2);

    static const int LOC_NONE = -1;

private:
    CSndLossList(const CSndLossList&);
    CSndLossList& operator=(const CSndLossList&);
};

////////////////////////////////////////////////////////////////////////////////

class CRcvLossList
{
public:
    CRcvLossList(int size = 1024);
    ~CRcvLossList();

    /// Insert a series of loss seq. no. between "seqno1" and "seqno2" into the receiver's loss list.
    /// @param [in] seqno1 sequence number starts.
    /// @param [in] seqno2 sequence number ends.
    /// @return length of the loss record inserted (seqlen(seqno1, seqno2)), -1 on error.
    int insert(int32_t seqno1, int32_t seqno2);

    /// Remove a loss seq. no. from the receiver's loss list.
    /// @param [in] seqno sequence number.
    /// @return if the packet is removed (true) or no such lost packet is found (false).

    bool remove(int32_t seqno);

    /// Remove all packets between seqno1 and seqno2.
    /// @param [in] seqno1 start sequence number.
    /// @param [in] seqno2 end sequence number.
    /// @return if the packet is removed (true) or no such lost packet is found (false).

    bool remove(int32_t seqno1, int32_t seqno2);


    /// Remove all numbers that precede the given sequence number.
    /// @param [in] seqno sequence number.
    /// @return the first removed sequence number
    int32_t removeUpTo(int32_t seqno);

    /// Find if there is any lost packets whose sequence number falling seqno1 and seqno2.
    /// @param [in] seqno1 start sequence number.
    /// @param [in] seqno2 end sequence number.
    /// @return True if found; otherwise false.

    bool find(int32_t seqno1, int32_t seqno2) const;

    /// Read the loss length.
    /// @return the length of the list.

    int getLossLength() const;

    /// Read the first (smallest) seq. no. in the list.
    /// @return the sequence number or -1 if the list is empty.

    int32_t getFirstLostSeq() const;

    /// Get a encoded loss array for NAK report.
    /// @param [inout] array the result list of seq. no. to be included in NAK.
    /// @return physical length of the result array.
    int getLossArray(FixedArray<int32_t>& array);

    /// A prepared, not-yet-committed page of a periodic loss report.
    ///
    /// A page is a snapshot of a portion of the loss list in SRT NAK wire
    /// encoding (host byte order): a single sequence number occupies one
    /// word, while a contiguous closed range occupies two words, the first
    /// one carrying @c LOSSDATA_SEQNO_RANGE_FIRST.
    ///
    /// A page belongs to the loss list object that produced it and to the
    /// state of that object at preparation time. It becomes stale after any
    /// modification call (insert/remove/removeUpTo), after another page has
    /// been committed, and after it has been committed itself. Stale and
    /// foreign pages are rejected by commitLossReport().
    class LossReportPage
    {
    public:
        LossReportPage();

        /// Read the encoded words of this page. Never modified after
        /// preparation.
        const std::vector<int32_t>& words() const { return m_words; }

    private:
        friend class CRcvLossList;

        const CRcvLossList* m_owner;      //< list that produced this page
        uint64_t m_generation;            //< state generation at preparation
        int32_t m_next_seqno;             //< cursor to use after a commit
        std::vector<int32_t> m_words;     //< encoded NAK words
    };

    /// Prepare a page of the periodic loss report without modifying the list.
    ///
    /// The page is filled greedily, following the circular report cursor,
    /// for at most one full lap of the loss list. Preparation does not move
    /// the cursor, does not delete losses and does not affect ACK positions
    /// or loss statistics; repeated preparation without an intervening
    /// modification yields identical content.
    ///
    /// @param [in] max_words maximum number of encoded words.
    /// @return the prepared page (empty if @a max_words is 0 or the list is empty).
    LossReportPage prepareLossReport(size_t max_words) const;

    /// Commit a previously prepared page, advancing the report cursor to the
    /// strict modular successor of the last sequence number represented by it.
    ///
    /// @param [in] page page returned by prepareLossReport() on this instance.
    /// @return true if the page was valid and the cursor was advanced;
    /// false for empty, foreign, already committed or stale pages. On failure
    /// the cursor is unchanged.
    bool commitLossReport(const LossReportPage& page);

private:
    struct Seq
    {
        int32_t seqstart; // sequence number starts
        int32_t seqend;   // sequence number ends
        int     inext;    // index of the next node in the list
        int     iprior;   // index of the previous node in the list
    } * m_caSeq;

    int m_iHead;   // first node in the list
    int m_iTail;   // last node in the list;
    int m_iLength; // loss length
    int m_iSize;   // size of the static array
    int m_iLargestSeq; // largest seq ever seen

    // Periodic loss report paging state.
    // m_iReportCursor is the next sequence position to start a page from;
    // SRT_SEQNO_NONE means "start from the head". It only moves on a
    // successful commit, so preparing a page has no side effects.
    int32_t  m_iReportCursor;
    // Incremented on every modification call (even a no-op one) and on every
    // successful commit; used to invalidate prepared pages.
    uint64_t m_uReportGeneration;

private:
    /// RAII helper used by every modification entry point: invalidates
    /// prepared pages at entry (even for a no-op call) and, at exit, resets
    /// the report cursor when the list has become empty.
    struct ReportStateGuard
    {
        CRcvLossList& list;
        explicit ReportStateGuard(CRcvLossList& l)
            : list(l)
        {
            ++list.m_uReportGeneration;
        }
        ~ReportStateGuard()
        {
            if (list.m_iLength == 0)
                list.m_iReportCursor = SRT_SEQNO_NONE;
        }
    };

private:
    CRcvLossList(const CRcvLossList&);
    CRcvLossList& operator=(const CRcvLossList&);

public:
    struct iterator
    {
        int32_t head;
        Seq*    seq;

        iterator(Seq* str, int32_t v)
            : head(v)
            , seq(str)
        {
        }

        iterator next() const
        {
            if (head == -1)
                return *this; // should report error, but we can only throw exception, so simply ignore it.

            return iterator(seq, seq[head].inext);
        }

        iterator& operator++()
        {
            *this = next();
            return *this;
        }

        iterator operator++(int)
        {
            iterator old(seq, head);
            *this = next();
            return old;
        }

        bool operator==(const iterator& second) const
        {
            // Ignore seq - should be the same and this is only a sanity check.
            return head == second.head;
        }

        bool operator!=(const iterator& second) const { return !(*this == second); }

        std::pair<int32_t, int32_t> operator*() { return std::make_pair(seq[head].seqstart, seq[head].seqend); }
    };

    iterator begin() { return iterator(m_caSeq, m_iHead); }
    iterator end() { return iterator(m_caSeq, -1); }
};

struct CRcvFreshLoss
{
    int32_t                             seq[2];
    int                                 ttl;
    srt::sync::steady_clock::time_point timestamp;

    CRcvFreshLoss(int32_t seqlo, int32_t seqhi, int initial_ttl);

// Don't WTF when looking at this. The Windows system headers define
// a publicly visible preprocessor macro with that name. REALLY!
#ifdef DELETE
#undef DELETE
#endif
    enum Emod
    {
        NONE,     //< the given sequence was not found in this range
        STRIPPED, //< it was equal to first or last, already taken care of
        SPLIT,    //< found in the middle, you have to split this range into two
        DELETE    //< This was a range of one element exactly equal to sequence. Simply delete it.
    };

    Emod revoke(int32_t sequence);
    Emod revoke(int32_t lo, int32_t hi);

    static bool removeOne(std::deque<CRcvFreshLoss>& w_container, int32_t sequence, int* had_ttl = NULL);
};

} // namespace srt

#endif
