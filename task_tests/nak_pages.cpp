/*
 * SRT - Secure, Reliable, Transport
 *
 * Regression tests for the cyclic paging of periodic NAK LOSSREPORTs.
 * Three groups are registered through task_support/check.h:
 *   pages    - CRcvLossList LossReportPage prepare/commit ring semantics
 *   commit   - page token validity, generation invalidation, cursor recovery
 *   playback - end-to-end periodic NAK scheduling through a real CUDT with
 *              an in-memory send sink, plus TSBPD too-late dropping.
 *
 * No sockets, sleeps or real waiting are used: time is advanced manually and
 * every emitted control packet is captured through a memory callback.
 */

#include "../task_support/check.h"

#include "api.h"
#include "core.h"
#include "list.h"
#include "buffer_rcv.h"
#include "queue.h"
#include "packet.h"
#include "srt.h"

#include <functional>
#include <set>
#include <vector>

using namespace srt;
using namespace srt::sync;

typedef std::function<int(const sockaddr_any&, CPacket&, const sockaddr_any&)> NakSinkFn;

namespace srt
{
// Friend bridge to the private CUDT periodic-NAK internals. Declared in
// namespace srt to match the friend declaration inside CUDT.
class NakPageTestAccess
{
public:
    static CUDT* create()
    {
        CUDTSocket* sock = new CUDTSocket();
        return &sock->core();
    }

    static void destroy(CUDT* u)
    {
        // The receiver buffer in the playback tests is stack-owned.
        u->m_pRcvBuffer = NULL;
        CUDTSocket* sock = u->m_parent;
        delete sock;
    }

    static void open(CUDT* u) { u->open(); }

    static int configure(CUDT* u, size_t payload_bytes)
    {
        // Congestion controller (same selection as a normal live connection).
        TASK_CHECK(u->m_CongCtl.select("live"));
        TASK_CHECK(u->m_CongCtl.configure(u));
        u->m_PktFilterRexmitLevel = SRT_ARQ_ALWAYS;

        // Manual clock: timestamps are measured from the epoch so makeTS()
        // assertions in the periodic path hold.
        u->m_stats.tsStartTime = steady_clock::time_point();

        // Payload sizing and the receive time window the interval update reads.
        u->m_iMaxDataPayloadSize = (int) payload_bytes;
        u->m_RcvTimeWindow.initialize(CPacket::UDP_HDR_SIZE - CPacket::HDR_SIZE, u->m_iMaxDataPayloadSize);

        u->m_pRcvLossList = new CRcvLossList(256);

        // FreshLoss/TLPKTDROP coordination preconditions.
        u->m_bPeerRexmitFlag  = true;
        u->m_iReorderTolerance = 10;
        return 0;
    }

    static void attachRcvBuffer(CUDT* u, CUnitQueue* q, CRcvBuffer* b, int32_t curr_seqno)
    {
        u->m_pRcvBuffer    = b;
        u->m_bTLPktDrop    = true;
        u->m_iRcvCurrSeqNo = curr_seqno;
    }

    static int checkNAKTimer(CUDT* u, const steady_clock::time_point& t, const NakSinkFn& sink)
    {
        return u->checkNAKTimer(t, sink);
    }

    static void setNextNakTime(CUDT* u, const steady_clock::time_point& t) { u->m_tsNextNAKTime.store(t); }
    static steady_clock::duration nakInterval(CUDT* u) { return u->m_tdNAKInterval; }
    static void setNakReportEnabled(CUDT* u, bool on) { u->m_config.bRcvNakReport = on; }
    static void setArqLevel(CUDT* u, SRT_ARQLevel l) { u->m_PktFilterRexmitLevel = l; }
    static void setPeerId(CUDT* u, int32_t id) { u->m_PeerID = id; }
    static uint32_t sentNak(CUDT* u) { return u->m_stats.rcvr.sentNak.trace.count(); }
    static CRcvLossList* lossList(CUDT* u) { return u->m_pRcvLossList; }

    static void pushFreshLoss(CUDT* u, int32_t lo, int32_t hi)
    {
        u->m_FreshLoss.push_back(CRcvFreshLoss(lo, hi, 5));
    }
    static size_t freshLossCount(CUDT* u) { return u->m_FreshLoss.size(); }
    static int  dropTooLate(CUDT* u, int32_t seqno) { return u->rcvDropTooLateUpTo(seqno); }
    // DROPREQ coordination entry (explicit from/to range on the loss lists).
    static void dropFromLossLists(CUDT* u, int32_t from, int32_t to) { u->dropFromLossLists(from, to); }
};
} // namespace srt

namespace
{

const int32_t RANGE_FLAG = LOSSDATA_SEQNO_RANGE_FIRST;

// Decode host-order NAK words into the ordered list of lost seqnos they name.
// Also fails the caller's check when a range-start marker is unpaired or an
// endpoint word itself carries the range marker.
std::vector<int32_t> decodeWords(const std::vector<int32_t>& words, int* ok)
{
    std::vector<int32_t> out;
    for (size_t i = 0; i < words.size(); ++i)
    {
        if (words[i] & RANGE_FLAG)
        {
            if (i + 1 >= words.size())
            {
                *ok = 0;
                return out; // unpaired range marker
            }
            const int32_t lo = words[i] & ~RANGE_FLAG;
            const int32_t hi = words[i + 1];
            if (hi & RANGE_FLAG)
            {
                *ok = 0;
                return out; // range end must not carry the marker
            }
            ++i;
            for (int32_t s = lo;; s = CSeqNo::incseq(s))
            {
                out.push_back(s);
                if (s == hi)
                    break;
            }
        }
        else
        {
            out.push_back(words[i]);
        }
    }
    return out;
}

std::set<int32_t> toSet(const std::vector<int32_t>& v)
{
    return std::set<int32_t>(v.begin(), v.end());
}

// Expand the current loss list content (list order) into a seqno set.
std::set<int32_t> lossSet(const CRcvLossList& l)
{
    std::set<int32_t> out;
    CRcvLossList& mutable_list = const_cast<CRcvLossList&>(l);
    for (CRcvLossList::iterator it = mutable_list.begin(); it != mutable_list.end(); ++it)
    {
        const std::pair<int32_t, int32_t> r = *it;
        if (r.second == SRT_SEQNO_NONE)
        {
            out.insert(r.first); // singleton node
            continue;
        }
        for (int32_t s = r.first;; s = CSeqNo::incseq(s))
        {
            out.insert(s);
            if (s == r.second)
                break;
        }
    }
    return out;
}

// In-memory packet sink: clones every successfully "sent" CPacket and can be
// toggled to emulate send failures. It runs the same host/network byte-order
// round trip as CChannel::sendto(), so captured words are host order again.
struct MemorySink
{
    std::vector<CPacket*> packets;
    std::vector<int32_t>  lastWords;
    bool                  failRequests = false;
    int                   failures = 0;
    CRcvLossList*         mutateOnSend = NULL; // optional: bump list generation mid-send
    int32_t               removeOnSend = SRT_SEQNO_NONE;

    int send(const sockaddr_any&, CPacket& p, const sockaddr_any&)
    {
        if (failRequests)
        {
            ++failures;
            return -1;
        }

        if (mutateOnSend && removeOnSend != SRT_SEQNO_NONE)
        {
            // Models a receiver-side recovery landing between packet build and
            // commit. The loss list lock is already held by this thread.
            mutateOnSend->remove(removeOnSend);
            mutateOnSend = NULL;
            removeOnSend = SRT_SEQNO_NONE;
        }

        // Mirror CChannel::sendto() byte-order handling.
        p.toNetworkByteOrder();
        p.toHostByteOrder();

        packets.push_back(p.clone());
        lastWords.assign(reinterpret_cast<const int32_t*>(p.data()),
                         reinterpret_cast<const int32_t*>(p.data()) + p.getLength() / 4);
        return (int) p.getLength();
    }

    ~MemorySink()
    {
        for (size_t i = 0; i < packets.size(); ++i)
            delete packets[i];
    }
};

NakSinkFn bindSink(MemorySink& s)
{
    return std::bind(&MemorySink::send, &s, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
}

// ---------------------------------------------------------------------------
// pages group
// ---------------------------------------------------------------------------

int pages_full_ring_coverage()
{
    CRcvLossList losses(256);
    // Two non-adjacent ranges plus a singleton, 12 + 5 + 1 = 18 lost seqnos.
    TASK_CHECK(losses.insert(100, 111) == 12);
    TASK_CHECK(losses.insert(120, 124) == 5);
    TASK_CHECK(losses.insert(130, 130) == 1);

    const std::set<int32_t> expected = lossSet(losses);
    TASK_CHECK(expected.size() == 18);

    // Walk many pages with a two-word budget. A page may straddle the ring
    // wrap, but no seqno may repeat within a single page, and the walk must
    // reach the later gaps instead of repeating the head prefix forever.
    std::set<int32_t> covered;
    std::vector<int32_t> previousWords;
    int pages = 0;
    for (; pages < 12; ++pages)
    {
        CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
        TASK_CHECK(!page.words().empty());

        int decodeOk = 1;
        const std::vector<int32_t> seqnos = decodeWords(page.words(), &decodeOk);
        TASK_CHECK(decodeOk);
        TASK_CHECK(!seqnos.empty());

        // No seqno repeats inside one page (a page wraps at most one circle).
        const std::set<int32_t> pageSet = toSet(seqnos);
        TASK_CHECK(pageSet.size() == seqnos.size());

        // The walk must not get stuck emitting the identical prefix page.
        TASK_CHECK(previousWords.empty() || previousWords != page.words());
        previousWords = page.words();

        for (std::set<int32_t>::const_iterator it = pageSet.begin(); it != pageSet.end(); ++it)
            covered.insert(*it);

        TASK_CHECK(losses.commitLossReport(page));
        if (covered == expected)
            break;
    }

    // Every gap, including the late ones, got its turn within a few pages.
    TASK_CHECK(covered == expected);
    TASK_CHECK(pages < 12);

    // The old getLossArray() behavior would have stalled on the 100..111
    // prefix: assert the paging walk reached gap 130 within its first pages.
    std::set<int32_t> early;
    for (int i = 0; i < 3; ++i)
    {
        CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
        int decodeOk = 1;
        const std::vector<int32_t> seqnos = decodeWords(page.words(), &decodeOk);
        TASK_CHECK(decodeOk);
        for (size_t j = 0; j < seqnos.size(); ++j)
            early.insert(seqnos[j]);
        TASK_CHECK(losses.commitLossReport(page));
    }
    TASK_CHECK(early.count(130) == 1);
    return 0;
}

int pages_budgets_zero_one_two()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(10, 20) == 11); // one range node
    TASK_CHECK(losses.insert(30, 30) == 1);  // one singleton node

    // Budget 0 and the empty list always produce an empty page.
    CRcvLossList::LossReportPage p0 = losses.prepareLossReport(0);
    TASK_CHECK(p0.words().empty());
    TASK_CHECK(!losses.commitLossReport(p0)); // empty pages cannot be committed

    CRcvLossList empty(16);
    CRcvLossList::LossReportPage pe = empty.prepareLossReport(10);
    TASK_CHECK(pe.words().empty());

    // Budget 1 on a range: only the segment head, next page resumes inside.
    CRcvLossList::LossReportPage p1 = losses.prepareLossReport(1);
    TASK_CHECK(p1.words().size() == 1);
    TASK_CHECK(p1.words()[0] == 10);
    TASK_CHECK(losses.commitLossReport(p1));

    CRcvLossList::LossReportPage p1b = losses.prepareLossReport(1);
    TASK_CHECK(p1b.words().size() == 1);
    TASK_CHECK(p1b.words()[0] == 11);
    TASK_CHECK(losses.commitLossReport(p1b));

    // Budget 2: remainder of the range is encoded as a proper two-word range.
    CRcvLossList::LossReportPage p2 = losses.prepareLossReport(2);
    TASK_CHECK(p2.words().size() == 2);
    TASK_CHECK(p2.words()[0] == (12 | RANGE_FLAG));
    TASK_CHECK(p2.words()[1] == 20);
    TASK_CHECK(losses.commitLossReport(p2));

    // Next page reaches the singleton. Use a one-word budget so the page ends
    // exactly on the tail instead of straddling the ring wrap.
    CRcvLossList::LossReportPage p3 = losses.prepareLossReport(1);
    TASK_CHECK(p3.words().size() == 1);
    TASK_CHECK(p3.words()[0] == 30);
    TASK_CHECK(losses.commitLossReport(p3));
    return 0;
}

int pages_range_resume_mid_segment()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(40, 49) == 10);

    // Walk the whole range with a one-word budget: each page names exactly
    // one seqno, and the following page continues inside the same node.
    for (int32_t expected = 40; expected <= 49; ++expected)
    {
        CRcvLossList::LossReportPage page = losses.prepareLossReport(1);
        TASK_CHECK(page.words().size() == 1);
        TASK_CHECK(page.words()[0] == expected);
        TASK_CHECK(losses.commitLossReport(page));
    }

    // Full round done: the cursor wrapped back to the head.
    CRcvLossList::LossReportPage again = losses.prepareLossReport(1);
    TASK_CHECK(again.words().size() == 1);
    TASK_CHECK(again.words()[0] == 40);
    TASK_CHECK(losses.commitLossReport(again));
    return 0;
}

int pages_cross_zero()
{
    CRcvLossList losses(64);
    // 0x7ffffffe, 0x7fffffff(=max), 0, 1 : a range crossing the seqno boundary.
    TASK_CHECK(losses.insert(CSeqNo::m_iMaxSeqNo - 1, 1) == 4);

    CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
    TASK_CHECK(page.words().size() == 2);
    TASK_CHECK(page.words()[0] == ((CSeqNo::m_iMaxSeqNo - 1) | RANGE_FLAG));
    TASK_CHECK(page.words()[1] == 1); // end word carries no marker

    int decodeOk = 1;
    const std::vector<int32_t> seqnos = decodeWords(page.words(), &decodeOk);
    TASK_CHECK(decodeOk);
    TASK_CHECK(seqnos.size() == 4);
    TASK_CHECK(seqnos[0] == CSeqNo::m_iMaxSeqNo - 1);
    TASK_CHECK(seqnos[1] == CSeqNo::m_iMaxSeqNo);
    TASK_CHECK(seqnos[2] == 0);
    TASK_CHECK(seqnos[3] == 1);
    TASK_CHECK(losses.commitLossReport(page));

    // After committing the whole range, the ring wraps back to its head.
    CRcvLossList::LossReportPage wrapped = losses.prepareLossReport(2);
    TASK_CHECK(wrapped.words().size() == 2);
    TASK_CHECK(wrapped.words()[0] == ((CSeqNo::m_iMaxSeqNo - 1) | RANGE_FLAG));
    TASK_CHECK(wrapped.words()[1] == 1);
    return 0;
}

int pages_prepare_does_not_mutate()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(100, 100) == 1);
    TASK_CHECK(losses.insert(102, 104) == 3);
    const int lengthBefore = losses.getLossLength();
    const int32_t firstBefore = losses.getFirstLostSeq();

    CRcvLossList::LossReportPage a = losses.prepareLossReport(2);
    CRcvLossList::LossReportPage b = losses.prepareLossReport(2);
    CRcvLossList::LossReportPage c = losses.prepareLossReport(2);
    TASK_CHECK(a.words() == b.words());
    TASK_CHECK(b.words() == c.words());

    // Neither the loss set nor the ACK front moved.
    TASK_CHECK(losses.getLossLength() == lengthBefore);
    TASK_CHECK(losses.getFirstLostSeq() == firstBefore);

    // Legacy getLossArray keeps its original prefix behavior.
    FixedArray<int32_t> legacy(4);
    TASK_CHECK(losses.getLossArray(legacy) == 3);
    TASK_CHECK(legacy[0] == 100);
    TASK_CHECK(legacy[1] == (102 | RANGE_FLAG));
    TASK_CHECK(legacy[2] == 104);
    return 0;
}

int pages_greedy_merge_adjacent_nodes()
{
    CRcvLossList losses(64);
    // Adjacent singleton nodes (insertion keeps them as separate nodes).
    TASK_CHECK(losses.insert(70, 70) == 1);
    TASK_CHECK(losses.insert(71, 71) == 1);
    TASK_CHECK(losses.insert(72, 72) == 1);

    CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
    TASK_CHECK(page.words().size() == 2);
    TASK_CHECK(page.words()[0] == (70 | RANGE_FLAG));
    TASK_CHECK(page.words()[1] == 72);
    TASK_CHECK(losses.commitLossReport(page));
    return 0;
}

int pages_wrap_inside_one_page()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(10, 12) == 3); // range node at the head
    TASK_CHECK(losses.insert(20, 20) == 1); // singleton tail

    // Move the cursor onto the tail first.
    CRcvLossList::LossReportPage head = losses.prepareLossReport(2);
    TASK_CHECK(head.words().size() == 2);
    TASK_CHECK(head.words()[0] == (10 | RANGE_FLAG));
    TASK_CHECK(losses.commitLossReport(head));

    // A generous page starts at the tail singleton, then wraps once and
    // continues from the head within the same page.
    CRcvLossList::LossReportPage wrapped = losses.prepareLossReport(4);
    TASK_CHECK(wrapped.words().size() == 3);
    TASK_CHECK(wrapped.words()[0] == 20);                 // tail singleton
    TASK_CHECK(wrapped.words()[1] == (10 | RANGE_FLAG));  // wrapped range start
    TASK_CHECK(wrapped.words()[2] == 12);                 // range end, no marker

    int decodeOk = 1;
    const std::vector<int32_t> seqnos = decodeWords(wrapped.words(), &decodeOk);
    TASK_CHECK(decodeOk);
    // Exactly one circle: every seqno appears once despite the in-page wrap.
    TASK_CHECK(seqnos.size() == 4);
    TASK_CHECK(toSet(seqnos).size() == 4);
    TASK_CHECK(toSet(seqnos) == lossSet(losses));
    TASK_CHECK(losses.commitLossReport(wrapped));

    // A new ring starts where the completed round began (the tail singleton).
    CRcvLossList::LossReportPage again = losses.prepareLossReport(4);
    TASK_CHECK(again.words()[0] == 20);
    int againOk = 1;
    const std::vector<int32_t> againSeq = decodeWords(again.words(), &againOk);
    TASK_CHECK(againOk);
    TASK_CHECK(toSet(againSeq) == lossSet(losses));
    return 0;
}

// ---------------------------------------------------------------------------
// commit group
// ---------------------------------------------------------------------------

int commit_empty_foreign_and_double()
{
    CRcvLossList a(64);
    CRcvLossList b(64);
    TASK_CHECK(a.insert(1, 5) == 5);
    TASK_CHECK(b.insert(1, 5) == 5);

    CRcvLossList::LossReportPage empty = a.prepareLossReport(0);
    TASK_CHECK(empty.words().empty());
    TASK_CHECK(!a.commitLossReport(empty));

    // A page produced by another instance is rejected.
    CRcvLossList::LossReportPage foreign = b.prepareLossReport(4);
    TASK_CHECK(!foreign.words().empty());
    TASK_CHECK(!a.commitLossReport(foreign));

    // A page can only be committed once.
    CRcvLossList::LossReportPage mine = a.prepareLossReport(4);
    TASK_CHECK(a.commitLossReport(mine));
    TASK_CHECK(!a.commitLossReport(mine));
    return 0;
}

int commit_invalidated_by_every_modifier()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(10, 30) == 21);

    // insert() invalidates, even when the new range is rejected as too old.
    {
        CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
        TASK_CHECK(!page.words().empty());
        TASK_CHECK(losses.insert(11, 11) == 0); // rejected: not newer than the list
        TASK_CHECK(!losses.commitLossReport(page));
    }

    // remove() of a seqno that is not lost still invalidates.
    {
        CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
        TASK_CHECK(!page.words().empty());
        TASK_CHECK(!losses.remove(999)); // not in the list
        TASK_CHECK(!losses.commitLossReport(page));
    }

    // remove(range) with an inverted range invalidates as well.
    {
        CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
        TASK_CHECK(!page.words().empty());
        TASK_CHECK(!losses.remove(20, 10));
        TASK_CHECK(!losses.commitLossReport(page));
    }

    // removeUpTo() before the first loss changes nothing, yet invalidates.
    {
        CRcvLossList::LossReportPage page = losses.prepareLossReport(2);
        TASK_CHECK(!page.words().empty());
        TASK_CHECK(losses.removeUpTo(5) == 10); // first loss stays 10
        TASK_CHECK(losses.getFirstLostSeq() == 10);
        TASK_CHECK(!losses.commitLossReport(page));
    }

    // The cursor itself never moved because every commit was rejected.
    CRcvLossList::LossReportPage fresh = losses.prepareLossReport(2);
    TASK_CHECK(fresh.words()[0] == (10 | RANGE_FLAG));
    TASK_CHECK(losses.commitLossReport(fresh));
    return 0;
}

int commit_intervening_commit_invalidates()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(10, 10) == 1);
    TASK_CHECK(losses.insert(20, 20) == 1);

    CRcvLossList::LossReportPage older = losses.prepareLossReport(1);
    TASK_CHECK(older.words()[0] == 10);
    CRcvLossList::LossReportPage younger = losses.prepareLossReport(1);
    TASK_CHECK(younger.words()[0] == 10);

    TASK_CHECK(losses.commitLossReport(younger));
    // The older token must not be accepted even though the list content is
    // identical and node slots may have been reused.
    TASK_CHECK(!losses.commitLossReport(older));

    // The successful commit advanced the cursor to the remaining loss.
    CRcvLossList::LossReportPage next = losses.prepareLossReport(1);
    TASK_CHECK(next.words()[0] == 20);
    return 0;
}

int commit_cursor_recovery_and_reset()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(10, 10) == 1);
    TASK_CHECK(losses.insert(20, 20) == 1);
    TASK_CHECK(losses.insert(30, 30) == 1);

    CRcvLossList::LossReportPage first = losses.prepareLossReport(1);
    TASK_CHECK(first.words()[0] == 10);
    TASK_CHECK(losses.commitLossReport(first)); // cursor -> successor of 10

    // Recover the seqno the cursor would name next.
    TASK_CHECK(losses.remove(20));
    CRcvLossList::LossReportPage second = losses.prepareLossReport(1);
    TASK_CHECK(second.words()[0] == 30); // skipped the recovered gap

    // Empty the list: the cursor resets and the next non-empty round starts
    // at the new head.
    TASK_CHECK(losses.remove(30));
    TASK_CHECK(losses.remove(10));
    TASK_CHECK(losses.getLossLength() == 0);
    CRcvLossList::LossReportPage empty = losses.prepareLossReport(4);
    TASK_CHECK(empty.words().empty());

    TASK_CHECK(losses.insert(100, 102) == 3);
    CRcvLossList::LossReportPage restarted = losses.prepareLossReport(2);
    TASK_CHECK(restarted.words().size() == 2);
    TASK_CHECK(restarted.words()[0] == (100 | RANGE_FLAG));
    TASK_CHECK(restarted.words()[1] == 102);
    return 0;
}

int commit_split_range_and_slot_reuse()
{
    CRcvLossList losses(32);
    TASK_CHECK(losses.insert(10, 20) == 11);

    // Recovering the middle splits the range into [10-14] and [16-20].
    TASK_CHECK(losses.remove(15));
    TASK_CHECK(losses.getLossLength() == 10);

    // First page covers the lower split range.
    CRcvLossList::LossReportPage low = losses.prepareLossReport(2);
    TASK_CHECK(low.words().size() == 2);
    TASK_CHECK(low.words()[0] == (10 | RANGE_FLAG));
    TASK_CHECK(low.words()[1] == 14);
    TASK_CHECK(losses.commitLossReport(low));

    // The cursor lands on the upper split range.
    CRcvLossList::LossReportPage high = losses.prepareLossReport(2);
    TASK_CHECK(high.words().size() == 2);
    TASK_CHECK(high.words()[0] == (16 | RANGE_FLAG));
    TASK_CHECK(high.words()[1] == 20);
    TASK_CHECK(losses.commitLossReport(high));

    // Re-insert a newer range so freed node slots get reused; any page prepared
    // before this mutation must be stale.
    CRcvLossList::LossReportPage old = losses.prepareLossReport(2);
    TASK_CHECK(!old.words().empty());
    TASK_CHECK(losses.insert(21, 25) == 5);
    TASK_CHECK(!losses.commitLossReport(old));

    // The cursor wrapped to the head of the surviving ranges.
    CRcvLossList::LossReportPage again = losses.prepareLossReport(4);
    int decodeOk = 1;
    const std::vector<int32_t> seqnos = decodeWords(again.words(), &decodeOk);
    TASK_CHECK(decodeOk);
    const std::set<int32_t> got = toSet(seqnos);
    TASK_CHECK(got == lossSet(losses));
    return 0;
}

int commit_no_successor_wraps_to_head()
{
    CRcvLossList losses(64);
    TASK_CHECK(losses.insert(10, 10) == 1);
    TASK_CHECK(losses.insert(20, 20) == 1);

    // Report the tail, leaving only the older head loss.
    CRcvLossList::LossReportPage first = losses.prepareLossReport(1);
    TASK_CHECK(first.words()[0] == 10);
    TASK_CHECK(losses.commitLossReport(first));
    CRcvLossList::LossReportPage tail = losses.prepareLossReport(1);
    TASK_CHECK(tail.words()[0] == 20);
    TASK_CHECK(losses.commitLossReport(tail)); // cursor -> successor of 20

    // Recover the tail after the cursor passed it: no modular successor
    // remains, so the cursor returns to the head (seqno 10).
    TASK_CHECK(losses.remove(20));
    CRcvLossList::LossReportPage back = losses.prepareLossReport(1);
    TASK_CHECK(back.words().size() == 1);
    TASK_CHECK(back.words()[0] == 10);
    TASK_CHECK(losses.commitLossReport(back));
    return 0;
}

// ---------------------------------------------------------------------------
// playback group (real CUDT periodic path with an in-memory send sink)
// ---------------------------------------------------------------------------

steady_clock::time_point testTime(int64_t us)
{
    return steady_clock::time_point() + microseconds_from(us);
}

int playback_periodic_pages_cover_all()
{
    CUDT* u = NakPageTestAccess::create();
    NakPageTestAccess::open(u);
    TASK_CHECK(NakPageTestAccess::configure(u, 4) == 0); // word budget = 1
    NakPageTestAccess::setPeerId(u, 4242);

    CRcvLossList* losses = NakPageTestAccess::lossList(u);
    TASK_CHECK(losses->insert(100, 109) == 10);

    MemorySink sink;
    const NakSinkFn fn = bindSink(sink);

    steady_clock::time_point now = testTime(10 * 1000 * 1000);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));

    std::set<int32_t> covered;
    for (int round = 0; round < 12; ++round)
    {
        const int decision = NakPageTestAccess::checkNAKTimer(u, now, fn);
        TASK_CHECK(decision != 0);
        TASK_CHECK(sink.packets.size() == static_cast<size_t>(round + 1));

        // The captured packet is a real LOSSREPORT control packet.
        const CPacket& pkt = *sink.packets.back();
        TASK_CHECK(pkt.isControl(UMSG_LOSSREPORT));
        TASK_CHECK(pkt.id() == 4242);
        TASK_CHECK((pkt.getLength() % 4) == 0);

        int decodeOk = 1;
        const std::vector<int32_t> seqnos = decodeWords(sink.lastWords, &decodeOk);
        TASK_CHECK(decodeOk);
        TASK_CHECK(seqnos.size() == 1); // one-word pages, one seqno at a time
        for (size_t i = 0; i < seqnos.size(); ++i)
            TASK_CHECK(covered.insert(seqnos[i]).second);

        // Scheduling: an immediate second call must not spin another packet.
        TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) == 0);
        TASK_CHECK(sink.packets.size() == static_cast<size_t>(round + 1));

        if (covered.size() == 10)
            break;

        now += NakPageTestAccess::nakInterval(u) + microseconds_from(1);
        NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));
    }

    std::set<int32_t> expected;
    for (int32_t s = 100; s <= 109; ++s)
        expected.insert(s);
    TASK_CHECK(covered == expected);
    TASK_CHECK(NakPageTestAccess::sentNak(u) == sink.packets.size());

    NakPageTestAccess::destroy(u);
    return 0;
}

int playback_send_failure_resends_same_page()
{
    CUDT* u = NakPageTestAccess::create();
    NakPageTestAccess::open(u);
    TASK_CHECK(NakPageTestAccess::configure(u, 4) == 0); // word budget = 1
    CRcvLossList* losses = NakPageTestAccess::lossList(u);
    TASK_CHECK(losses->insert(50, 52) == 3);

    MemorySink sink;
    sink.failRequests = true;
    const NakSinkFn fn = bindSink(sink);

    steady_clock::time_point now = testTime(5 * 1000 * 1000);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));

    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) != 0);
    TASK_CHECK(sink.failures == 1);
    TASK_CHECK(sink.packets.empty());
    TASK_CHECK(NakPageTestAccess::sentNak(u) == 0); // failures are not counted

    // The cursor did not move: the next successful period sends the same page.
    now += NakPageTestAccess::nakInterval(u) + microseconds_from(1);
    sink.failRequests = false;
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));
    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) != 0);
    TASK_CHECK(sink.packets.size() == 1);
    TASK_CHECK(sink.lastWords.size() == 1);
    TASK_CHECK(sink.lastWords[0] == 50);
    TASK_CHECK(NakPageTestAccess::sentNak(u) == 1);

    // Following period resumes inside the range.
    now += NakPageTestAccess::nakInterval(u) + microseconds_from(1);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));
    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) != 0);
    TASK_CHECK(sink.lastWords[0] == 51);

    NakPageTestAccess::destroy(u);
    return 0;
}

int playback_stale_snapshot_commit_fails()
{
    CUDT* u = NakPageTestAccess::create();
    NakPageTestAccess::open(u);
    TASK_CHECK(NakPageTestAccess::configure(u, 16) == 0);
    CRcvLossList* losses = NakPageTestAccess::lossList(u);
    TASK_CHECK(losses->insert(60, 64) == 5);

    MemorySink sink;
    // The send itself succeeds, but the loss list changes before commit,
    // exactly as if a retransmitted packet landed during the send.
    sink.mutateOnSend = losses;
    sink.removeOnSend = 62;
    const NakSinkFn fn = bindSink(sink);

    steady_clock::time_point now = testTime(7 * 1000 * 1000);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));

    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) != 0);
    TASK_CHECK(sink.packets.size() == 1); // the snapshot was still sent
    TASK_CHECK(NakPageTestAccess::sentNak(u) == 1); // counted as sent

    // The commit failed, so the cursor stayed at the head: the next period
    // re-prepares from 60 instead of skipping ahead.
    now += NakPageTestAccess::nakInterval(u) + microseconds_from(1);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));
    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) != 0);
    TASK_CHECK(sink.packets.size() == 2);
    int decodeOk = 1;
    const std::vector<int32_t> seqnos = decodeWords(sink.lastWords, &decodeOk);
    TASK_CHECK(decodeOk);
    TASK_CHECK(seqnos.front() == 60);
    TASK_CHECK(toSet(seqnos).count(62) == 0); // the recovered seqno is gone

    NakPageTestAccess::destroy(u);
    return 0;
}

int playback_nakreport_and_filter_restrictions()
{
    CUDT* u = NakPageTestAccess::create();
    NakPageTestAccess::open(u);
    TASK_CHECK(NakPageTestAccess::configure(u, 16) == 0);
    CRcvLossList* losses = NakPageTestAccess::lossList(u);
    TASK_CHECK(losses->insert(80, 82) == 3);

    MemorySink sink;
    const NakSinkFn fn = bindSink(sink);
    steady_clock::time_point now = testTime(8 * 1000 * 1000);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));

    // NAKREPORT disabled: nothing is sent.
    NakPageTestAccess::setNakReportEnabled(u, false);
    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) == 0);
    TASK_CHECK(sink.packets.empty());

    // Re-enable NAKREPORT but switch the filter ARQ level away from ALWAYS:
    // the existing packet-filter restriction is preserved.
    NakPageTestAccess::setNakReportEnabled(u, true);
    NakPageTestAccess::setArqLevel(u, SRT_ARQ_ONREQ);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));
    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) == 0);
    TASK_CHECK(sink.packets.empty());

    NakPageTestAccess::setArqLevel(u, SRT_ARQ_ALWAYS);
    NakPageTestAccess::setNextNakTime(u, now - microseconds_from(1));
    TASK_CHECK(NakPageTestAccess::checkNAKTimer(u, now, fn) != 0);
    TASK_CHECK(sink.packets.size() == 1);

    NakPageTestAccess::destroy(u);
    return 0;
}

int playback_droreq_invalidates_page()
{
    CUDT* u = NakPageTestAccess::create();
    NakPageTestAccess::open(u);
    TASK_CHECK(NakPageTestAccess::configure(u, 16) == 0);
    CRcvLossList* losses = NakPageTestAccess::lossList(u);
    TASK_CHECK(losses->insert(90, 95) == 6);
    NakPageTestAccess::pushFreshLoss(u, 90, 95);

    // A page prepared before a DROPREQ must be stale afterwards, and the
    // dropped range is removed through the production coordination entry.
    CRcvLossList::LossReportPage page = losses->prepareLossReport(4);
    TASK_CHECK(!page.words().empty());

    NakPageTestAccess::dropFromLossLists(u, 90, 92); // DROPREQ path
    TASK_CHECK(losses->getLossLength() == 3);
    TASK_CHECK(!losses->commitLossReport(page));
    // The FreshLoss record is stripped to the surviving range rather than
    // deleted, and its dropped part is gone.
    TASK_CHECK(NakPageTestAccess::freshLossCount(u) == 1);

    // The remaining losses (93..95) are what the next page reports.
    CRcvLossList::LossReportPage next = losses->prepareLossReport(2);
    TASK_CHECK(next.words().size() == 2);
    TASK_CHECK(next.words()[0] == (93 | RANGE_FLAG));
    TASK_CHECK(next.words()[1] == 95);

    NakPageTestAccess::destroy(u);
    return 0;
}

int putMediaPacket(CUnitQueue& q, CRcvBuffer& b, int seq, int msg, uint32_t ts, char payload)
{
    CUnit* unit = q.getNextAvailUnit();
    TASK_CHECK(unit != NULL);
    CPacket& p = unit->m_Packet;
    p.set_seqno(seq);
    p.set_timestamp(ts);
    p.set_msgflags(msg | PacketBoundaryBits(PB_SOLO) | MSGNO_PACKET_INORDER::wrap(1));
    p.setLength(1);
    p.data()[0] = payload;
    TASK_CHECK(b.insert(unit) == 0);
    return 0;
}

int playback_tsbpd_too_late_drop()
{
    CUDT* u = NakPageTestAccess::create();
    NakPageTestAccess::open(u);
    TASK_CHECK(NakPageTestAccess::configure(u, 64) == 0);

    CUnitQueue units(32, 1500);
    CRcvBuffer buffer(100, 32, &units, true);
    NakPageTestAccess::attachRcvBuffer(u, &units, &buffer, 103);

    const steady_clock::time_point base  = testTime(1000000);
    const steady_clock::duration   delay = microseconds_from(1000);
    buffer.setPeerRexmitFlag(true);
    buffer.setTsbPdMode(base, false, delay);

    // Gap at 100; packet 101 is the first playable media message.
    CRcvLossList* losses = NakPageTestAccess::lossList(u);
    TASK_CHECK(losses->insert(100, 100) == 1);
    NakPageTestAccess::pushFreshLoss(u, 100, 100);
    TASK_CHECK(putMediaPacket(units, buffer, 101, 1, 0, 'A') == 0);

    const CRcvBuffer::PacketInfo info = buffer.getFirstValidPacketInfo();
    TASK_CHECK(info.seq_gap);
    TASK_CHECK(info.seqno == 101);
    TASK_CHECK(info.tsbpd_time == base + delay);

    // Before the deadline the gap is retained and nothing is readable.
    TASK_CHECK(!buffer.isRcvDataReady(base + delay - microseconds_from(1)));
    TASK_CHECK(losses->getLossLength() == 1);
    TASK_CHECK(NakPageTestAccess::freshLossCount(u) == 1);

    // At the deadline run the production too-late drop coordination entry:
    // it clears both the loss list and the FreshLoss record over the gap.
    const steady_clock::time_point deadline = base + delay;
    const int dropped = NakPageTestAccess::dropTooLate(u, 101);
    TASK_CHECK(dropped == 1); // one missing packet jumped over
    TASK_CHECK(losses->getLossLength() == 0);
    TASK_CHECK(NakPageTestAccess::freshLossCount(u) == 0);

    // A report must not resurrect the played-away gap.
    CRcvLossList::LossReportPage page = losses->prepareLossReport(8);
    TASK_CHECK(page.words().empty());

    // The waiting message is now delivered with its payload.
    TASK_CHECK(buffer.isRcvDataReady(deadline));
    char out[16] = {};
    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    TASK_CHECK(buffer.readMessage(out, sizeof(out), &ctrl) == 1);
    TASK_CHECK(out[0] == 'A');
    TASK_CHECK(ctrl.msgno == 1);

    // A later gap (102) with the following media message 103 works normally,
    // and reports only name the new gap.
    TASK_CHECK(losses->insert(102, 102) == 1);
    TASK_CHECK(putMediaPacket(units, buffer, 103, 2, 1000, 'B') == 0);
    CRcvLossList::LossReportPage later = losses->prepareLossReport(8);
    TASK_CHECK(later.words().size() == 1);
    TASK_CHECK(later.words()[0] == 102);
    TASK_CHECK(losses->commitLossReport(later));

    const CRcvBuffer::PacketInfo info2 = buffer.getFirstValidPacketInfo();
    TASK_CHECK(info2.seq_gap);
    TASK_CHECK(info2.seqno == 103);
    TASK_CHECK(info2.tsbpd_time == base + microseconds_from(1000) + delay);

    // Drop 102 too and read the successor message.
    const int dropped2 = NakPageTestAccess::dropTooLate(u, 103);
    TASK_CHECK(dropped2 == 1);
    TASK_CHECK(losses->getLossLength() == 0);
    TASK_CHECK(buffer.isRcvDataReady(info2.tsbpd_time));
    char out2[16] = {};
    SRT_MSGCTRL ctrl2 = srt_msgctrl_default;
    TASK_CHECK(buffer.readMessage(out2, sizeof(out2), &ctrl2) == 1);
    TASK_CHECK(out2[0] == 'B');
    TASK_CHECK(ctrl2.msgno == 2);

    NakPageTestAccess::destroy(u);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// task_support/check.h registration
// ---------------------------------------------------------------------------

extern "C" const struct task_case task_cases[] = {
    {"pages",    "full_ring_coverage",            pages_full_ring_coverage},
    {"pages",    "budgets_0_1_2",                 pages_budgets_zero_one_two},
    {"pages",    "range_resume_mid_segment",      pages_range_resume_mid_segment},
    {"pages",    "cross_zero",                    pages_cross_zero},
    {"pages",    "prepare_does_not_mutate",       pages_prepare_does_not_mutate},
    {"pages",    "greedy_merge_adjacent_nodes",   pages_greedy_merge_adjacent_nodes},
    {"pages",    "wrap_inside_one_page",          pages_wrap_inside_one_page},

    {"commit",   "empty_foreign_double",          commit_empty_foreign_and_double},
    {"commit",   "invalidated_by_every_modifier", commit_invalidated_by_every_modifier},
    {"commit",   "intervening_commit",            commit_intervening_commit_invalidates},
    {"commit",   "cursor_recovery_reset",         commit_cursor_recovery_and_reset},
    {"commit",   "split_range_slot_reuse",        commit_split_range_and_slot_reuse},
    {"commit",   "no_successor_wraps_to_head",    commit_no_successor_wraps_to_head},

    {"playback", "periodic_pages_cover_all",      playback_periodic_pages_cover_all},
    {"playback", "send_failure_resends_same_page",playback_send_failure_resends_same_page},
    {"playback", "stale_snapshot_commit_fails",   playback_stale_snapshot_commit_fails},
    {"playback", "nakreport_filter_restrictions", playback_nakreport_and_filter_restrictions},
    {"playback", "droreq_invalidates_page",       playback_droreq_invalidates_page},
    {"playback", "tsbpd_too_late_drop",           playback_tsbpd_too_late_drop},
};

extern "C" const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
