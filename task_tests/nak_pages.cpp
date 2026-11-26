// Regression tests for the periodic NAK loss report paging.
//
// Groups:
//  - pages:    CRcvLossList::prepareLossReport() content and cursor rules.
//  - commit:   page validity/ownership/commit semantics, including through the
//              real periodic production entry (periodicNakStep).
//  - playback: TSBPD playback drop coordination through the production drop
//              entry, then loss reporting must not resurrect losses.
//
// No sockets, sleeps or real time waiting are used: the sender callback is an
// in-memory CPacket capture and all time bases are driven manually.

#include <stdint.h>

#include <cstring>
#include <string>
#include <vector>

#include "task_support/check.h"

#include "api.h"
#include "buffer_rcv.h"
#include "common.h"
#include "core.h"
#include "list.h"
#include "packet.h"
#include "queue.h"
#include "sync.h"

using namespace srt;

namespace {

// ---------------------------------------------------------------------------
// Encoding helpers

bool is_range_first(int32_t w)
{
    return (w & LOSSDATA_SEQNO_RANGE_FIRST) != 0;
}

int32_t word_seq(int32_t w)
{
    return w & (~LOSSDATA_SEQNO_RANGE_FIRST);
}

// Decode a NAK word stream into individual sequence numbers, verifying that
// range markers are paired and that no sequence is repeated.
bool decode_words(const std::vector<int32_t>& words, std::vector<int32_t>& out)
{
    out.clear();
    for (size_t i = 0; i < words.size(); ++i)
    {
        const int32_t w = words[i];
        if (is_range_first(w))
        {
            if (i + 1 >= words.size())
                return false; // unpaired range marker
            if (is_range_first(words[i + 1]))
                return false;
            const int32_t lo = word_seq(w);
            const int32_t hi = words[i + 1];
            if (CSeqNo::seqcmp(lo, hi) > 0)
                return false;
            const int len = CSeqNo::seqlen(lo, hi);
            if (len < 2)
                return false; // a range must span at least two seqnos
            for (int32_t s = lo;; s = CSeqNo::incseq(s))
            {
                out.push_back(s);
                if (s == hi)
                    break;
            }
            ++i;
        }
        else
        {
            out.push_back(w);
        }
    }

    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1; j < out.size(); ++j)
            if (out[i] == out[j])
                return false; // repeated sequence inside one page
    return true;
}

std::vector<int32_t> sorted_seqs(const std::vector<int32_t>& in)
{
    // Insertion sort using modular comparison; valid for any set within less
    // than half of the sequence space.
    std::vector<int32_t> v(in);
    for (size_t i = 1; i < v.size(); ++i)
    {
        int32_t key = v[i];
        size_t j = i;
        while (j > 0 && CSeqNo::seqcmp(v[j - 1], key) > 0)
        {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = key;
    }
    return v;
}

bool same_set(std::vector<int32_t> a, std::vector<int32_t> b)
{
    if (a.size() != b.size())
        return false;
    a = sorted_seqs(a);
    b = sorted_seqs(b);
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i])
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// In-memory sender: captures every real CPacket, optionally fails all sends.

struct CaptureSender
{
    std::vector<std::vector<int32_t> > pages; // raw NAK words, successfully sent
    bool                              fail;
    int                               calls;

    CaptureSender()
        : fail(false)
        , calls(0)
    {
    }

    static bool send(void* opaque, const sockaddr_any&, CPacket& packet, const sockaddr_any&)
    {
        CaptureSender* self = static_cast<CaptureSender*>(opaque);
        ++self->calls;

        // Validate the real CPacket built by the production path.
        if (!packet.isControl(UMSG_LOSSREPORT))
            return false;
        if (packet.getLength() % sizeof(int32_t) != 0)
            return false;

        std::vector<int32_t> words;
        const size_t n = packet.getLength() / sizeof(int32_t);
        const int32_t* data = reinterpret_cast<const int32_t*>(packet.data());
        for (size_t i = 0; i < n; ++i)
            words.push_back(data[i]);

        if (self->fail)
            return false;

        self->pages.push_back(words);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Friend facade over private CUDT state. Must live in namespace srt so that
// the in-class friend declaration in core.h refers to this very class.

} // anonymous namespace

namespace srt {

class NakPagesTest
{
public:
    // Create/destroy an otherwise unused CUDT instance. Production creates
    // these inside CUDTSocket; for these tests no socket layer is involved.
    // @param with_unit_queue when non-NULL, the receive buffer is backed by
    //        that unit queue so real media units can be inserted.
    static CUDT* create(CUnitQueue* with_unit_queue = NULL, int buffer_init_seqno = 0)
    {
        CUDT* core = new CUDT((CUDTSocket*)NULL);
        core->open();
        core->m_iMaxDataPayloadSize = 1316;
        core->m_iSRTT               = CUDT::INITIAL_RTT;
        core->m_iRTTVar             = CUDT::INITIAL_RTTVAR;

        core->m_config.sCongestion.set("live");
        if (!core->m_CongCtl.select("live") || !core->m_CongCtl.configure(core))
            return NULL;

        // The packet time window needs explicit size initialization (production
        // does it in CUDT::clearData()/applyResponseSettings()).
        core->m_RcvTimeWindow.initialize(CPacket::UDP_HDR_SIZE - CPacket::HDR_SIZE,
                                         (size_t)core->m_iMaxDataPayloadSize);

        core->m_PktFilterRexmitLevel = SRT_ARQ_ALWAYS;
        core->m_config.bRcvNakReport = true;
        core->m_tdMinNakInterval     = sync::milliseconds_from(1);
        core->m_tdNAKInterval        = core->m_tdMinNakInterval;
        core->m_tsNextNAKTime.store(sync::steady_clock::time_point());

        core->m_pRcvLossList = new CRcvLossList(1024);
        core->m_pRcvBuffer = new CRcvBuffer(buffer_init_seqno, 64, with_unit_queue, true);
        core->m_pRcvBuffer->setPeerRexmitFlag(true);
        core->m_iRcvCurrSeqNo = CSeqNo::decseq(buffer_init_seqno);
        return core;
    }

    static void destroy(CUDT* core)
    {
        if (!core)
            return;
        delete core->m_pRcvLossList;
        core->m_pRcvLossList = NULL;
        delete core->m_pRcvBuffer;
        core->m_pRcvBuffer = NULL;
        delete core;
    }

    static void setMaxPayload(CUDT* core, size_t bytes) { core->m_iMaxDataPayloadSize = (int)bytes; }

    static int stepWith(CUDT* core, const sync::steady_clock::time_point& now, CaptureSender& sender)
    {
        return core->periodicNakStep(now, &CaptureSender::send, &sender);
    }

    static CRcvLossList& losses(CUDT* core) { return *core->m_pRcvLossList; }
    static CRcvBuffer& buffer(CUDT* core) { return *core->m_pRcvBuffer; }
    static int lossLength(CUDT* core) { return core->m_pRcvLossList->getLossLength(); }
    static uint32_t sentNak(CUDT* core)
    {
        return core->m_stats.rcvr.sentNak.total.count();
    }
    static void resetSentNak(CUDT* core) { core->m_stats.rcvr.sentNak.reset(); }
    static void insertLoss(CUDT* core, int32_t lo, int32_t hi)
    {
        core->m_pRcvLossList->insert(lo, hi);
    }

    /// Reflect a received data packet in CUDT's "largest received" position,
    /// as CUDT::handleSocketPacketReception() does. Used to mirror the TSBPD
    /// drop clamp (rcvDropTooLateUpTo never drops past the latest received).
    static void noteReceived(CUDT* core, int32_t seqno)
    {
        if (CSeqNo::seqcmp(seqno, core->m_iRcvCurrSeqNo) > 0)
            core->m_iRcvCurrSeqNo = seqno;
    }

    static int stepRaw(CUDT* core, const sync::steady_clock::time_point& now,
                       CUDT::LossReportSender sender, void* opaque)
    {
        return core->periodicNakStep(now, sender, opaque);
    }

    static sync::steady_clock::time_point nextNak(CUDT* core) { return core->m_tsNextNAKTime.load(); }
    static void setNextNak(CUDT* core, const sync::steady_clock::time_point& t)
    {
        core->m_tsNextNAKTime.store(t);
    }
    static void setNakEnabled(CUDT* core, bool v) { core->m_config.bRcvNakReport = v; }
    static void setFilterLevel(CUDT* core, SRT_ARQLevel l) { core->m_PktFilterRexmitLevel = l; }

    /// The production sender must report failure when there is no send queue,
    /// instead of inferring success from a payload length.
    static bool defaultSenderWithNoQueue(CUDT* core, CPacket& packet)
    {
        TASK_CHECK(core->m_pSndQueue == NULL);
        return core->defaultLossReportSender(core, core->m_PeerAddr, packet, core->m_SourceAddr);
    }

    static int becauseNakReport() { return CUDT::BECAUSE_NAKREPORT; }
    static int becauseNoReason() { return CUDT::BECAUSE_NO_REASON; }

    // --- TSBPD playback / drop coordination (production entry points) ------
    static void configurePlayback(CUDT* core, int32_t start_seqno, const sync::steady_clock::time_point& timebase,
                                  const sync::steady_clock::duration& delay)
    {
        core->m_iRcvCurrSeqNo = CSeqNo::decseq(start_seqno);
        core->m_bTsbPd        = true;
        core->m_bTLPktDrop    = true;
        core->m_bPeerTsbPd    = true;
        core->m_iTsbPdDelay_ms =
            (int)sync::count_milliseconds(delay);
        core->m_pRcvBuffer->setTsbPdMode(timebase, false, delay);
        core->m_pRcvBuffer->setPeerRexmitFlag(true);

        // Enable the belated-loss cleanup branch inside dropFromLossLists().
        core->m_bPeerRexmitFlag   = true;
        core->m_iReorderTolerance = 1;
    }

    /// Mirrors the production TSBPD decision in CUDT::tsbpd(): when the packet
    /// at the head of the buffer is due and there is a sequence gap before it,
    /// call rcvDropTooLateUpTo(head_seqno).
    static int playbackDrop(CUDT* core, const sync::steady_clock::time_point& now)
    {
        const CRcvBuffer::PacketInfo info = core->m_pRcvBuffer->getFirstValidPacketInfo();
        if (info.seqno == SRT_SEQNO_NONE)
            return 0;
        if (sync::is_zero(info.tsbpd_time) || now < info.tsbpd_time)
            return 0;
        if (!info.seq_gap)
            return 0;
        return core->rcvDropTooLateUpTo(info.seqno);
    }

    static void addFreshLoss(CUDT* core, int32_t lo, int32_t hi, int ttl)
    {
        core->m_FreshLoss.push_back(CRcvFreshLoss(lo, hi, ttl));
    }
    static size_t freshLossCount(CUDT* core) { return core->m_FreshLoss.size(); }
    static bool freshLossHas(CUDT* core, int32_t s)
    {
        for (size_t i = 0; i < core->m_FreshLoss.size(); ++i)
            if (CSeqNo::seqcmp(core->m_FreshLoss[i].seq[0], s) <= 0 &&
                CSeqNo::seqcmp(s, core->m_FreshLoss[i].seq[1]) <= 0)
                return true;
        return false;
    }
};

class CoreHolder
{
public:
    CoreHolder()
        : core(NakPagesTest::create())
    {
    }
    ~CoreHolder() { NakPagesTest::destroy(core); }

    bool ok() const { return core != NULL; }
    CUDT* get() { return core; }

private:
    CoreHolder(const CoreHolder&);
    CoreHolder& operator=(const CoreHolder&);
    CUDT* core;
};

} // namespace srt

// Re-open the anonymous namespace for the cases below. Both NakPagesTest and
// CoreHolder are found via the using-declarations.
namespace {

using srt::NakPagesTest;
using srt::CoreHolder;

// ---------------------------------------------------------------------------
// Group "pages"

int case_pages_empty_and_zero_budget()
{
    CRcvLossList list(16);

    TASK_CHECK(list.prepareLossReport(0).words().empty());
    TASK_CHECK(list.prepareLossReport(4).words().empty());

    list.insert(10, 12);
    TASK_CHECK(list.prepareLossReport(0).words().empty());

    CRcvLossList::LossReportPage p = list.prepareLossReport(4);
    std::vector<int32_t> seqs;
    TASK_CHECK(decode_words(p.words(), seqs));
    TASK_CHECK(seqs.size() == 3);
    TASK_CHECK(seqs[0] == 10 && seqs[1] == 11 && seqs[2] == 12);
    TASK_CHECK(!list.commitLossReport(CRcvLossList::LossReportPage()));
    return 0;
}

int case_pages_budget_one_and_two()
{
    CRcvLossList list(64);
    list.insert(100, 105); // 6 contiguous losses

    // Budget 1: a single word holding the first seqno of the range.
    CRcvLossList::LossReportPage p1 = list.prepareLossReport(1);
    TASK_CHECK(p1.words().size() == 1);
    TASK_CHECK(p1.words()[0] == 100);
    TASK_CHECK(list.commitLossReport(p1));

    // Budget 2: the remainder of the range, encoded as one range pair.
    CRcvLossList::LossReportPage p2 = list.prepareLossReport(2);
    TASK_CHECK(p2.words().size() == 2);
    TASK_CHECK(is_range_first(p2.words()[0]));
    TASK_CHECK(word_seq(p2.words()[0]) == 101);
    TASK_CHECK(p2.words()[1] == 105);
    TASK_CHECK(list.commitLossReport(p2));

    // The cursor wrapped back to the head; a fresh page starts the range anew.
    CRcvLossList::LossReportPage p3 = list.prepareLossReport(4);
    TASK_CHECK(p3.words().size() == 2);
    TASK_CHECK(word_seq(p3.words()[0]) == 100 && p3.words()[1] == 105);
    return 0;
}

int case_pages_cover_without_prefix_repeat()
{
    CRcvLossList list(256);
    list.insert(10, 10);
    list.insert(20, 25); // range of 6
    list.insert(30, 30);
    list.insert(40, 40);
    list.insert(50, 60); // range of 11

    std::vector<int32_t> expected;
    expected.push_back(10);
    for (int32_t s = 20; s <= 25; ++s)
        expected.push_back(s);
    expected.push_back(30);
    expected.push_back(40);
    for (int32_t s = 50; s <= 60; ++s)
        expected.push_back(s);

    std::vector<std::vector<int32_t> > pages;
    for (int guard = 0; guard < 3; ++guard)
    {
        CRcvLossList::LossReportPage p = list.prepareLossReport(4); // small budget -> several pages
        std::vector<int32_t> seqs;
        TASK_CHECK(decode_words(p.words(), seqs));
        TASK_CHECK(!seqs.empty());
        pages.push_back(seqs);
        TASK_CHECK(list.commitLossReport(p));
    }

    TASK_CHECK(pages.size() == 3);
    // Page 1 covers the head of the table; it must not restart at 10 forever.
    TASK_CHECK(same_set(pages[0], std::vector<int32_t>{10, 20, 21, 22, 23, 24, 25, 30}));
    // Page 2 continues with 40 and the 50..60 range (then greedily wraps for
    // its last remaining word), rather than repeating page 1.
    TASK_CHECK(pages[1].size() > 0 && pages[1][0] == 40);
    TASK_CHECK(std::find(pages[1].begin(), pages[1].end(), 50) != pages[1].end());
    TASK_CHECK(std::find(pages[1].begin(), pages[1].end(), 60) != pages[1].end());
    TASK_CHECK(!(pages[0] == pages[1]));

    // Union of the first two pages covers every loss at least once.
    std::vector<int32_t> covered(pages[0]);
    covered.insert(covered.end(), pages[1].begin(), pages[1].end());
    for (size_t i = 0; i < expected.size(); ++i)
        TASK_CHECK(std::find(covered.begin(), covered.end(), expected[i]) != covered.end());
    return 0;
}

int case_pages_range_continuation()
{
    // Budget too small to hold a whole range: the next page continues past
    // the encoded sequences, never mid-marker.
    CRcvLossList list(32);
    list.insert(100, 101);
    list.insert(110, 111);
    list.insert(120, 121);

    CRcvLossList::LossReportPage p1 = list.prepareLossReport(3);
    std::vector<int32_t> s1;
    TASK_CHECK(decode_words(p1.words(), s1));
    // 3 words: one full range pair (100,101) plus the first seq of the next.
    TASK_CHECK(s1.size() == 3);
    TASK_CHECK(s1[0] == 100 && s1[1] == 101 && s1[2] == 110);
    TASK_CHECK(list.commitLossReport(p1));

    CRcvLossList::LossReportPage p2 = list.prepareLossReport(3);
    std::vector<int32_t> s2;
    TASK_CHECK(decode_words(p2.words(), s2));
    // The cursor resumed inside the 110-111 range: 111 first, then 120..121.
    TASK_CHECK(s2.size() == 3);
    TASK_CHECK(s2[0] == 111 && s2[1] == 120 && s2[2] == 121);
    TASK_CHECK(list.commitLossReport(p2));

    // Sweep complete: back to head.
    CRcvLossList::LossReportPage p3 = list.prepareLossReport(2);
    TASK_CHECK(word_seq(p3.words()[0]) == 100 && p3.words()[1] == 101);
    return 0;
}

int case_pages_cross_zero()
{
    CRcvLossList list(64);
    const int32_t last = CSeqNo::m_iMaxSeqNo; // 0x7fffffff

    // A single contiguous range crossing 0x7fffffff/0 stays one range pair.
    list.insert(last - 2, 2);
    TASK_CHECK(list.getLossLength() == 6);

    CRcvLossList::LossReportPage p = list.prepareLossReport(4);
    std::vector<int32_t> seqs;
    TASK_CHECK(decode_words(p.words(), seqs));
    TASK_CHECK(seqs.size() == 6);
    TASK_CHECK(seqs[0] == last - 2);
    TASK_CHECK(seqs[3] == 0);
    TASK_CHECK(seqs[5] == 2);
    TASK_CHECK(p.words().size() == 2);
    TASK_CHECK(list.commitLossReport(p));

    // Ranges adjacent across the 0x7fffffff/0 boundary coalesce into a single
    // wrap-crossing range when both are present.
    CRcvLossList list2(64);
    list2.insert(last - 1, last);
    list2.insert(0, 1);

    CRcvLossList::LossReportPage a = list2.prepareLossReport(4);
    std::vector<int32_t> sa;
    TASK_CHECK(decode_words(a.words(), sa));
    TASK_CHECK(sa.size() == 4);
    TASK_CHECK(sa[0] == last - 1 && sa[1] == last && sa[2] == 0 && sa[3] == 1);
    TASK_CHECK(a.words().size() == 2); // a single range pair across zero
    TASK_CHECK(list2.commitLossReport(a));

    // A non-adjacent range across zero is a separate pair, consumed in order.
    CRcvLossList list3(64);
    list3.insert(last, last);
    list3.insert(2, 2);

    CRcvLossList::LossReportPage b = list3.prepareLossReport(2);
    std::vector<int32_t> sb;
    TASK_CHECK(decode_words(b.words(), sb));
    TASK_CHECK(sb.size() == 2);
    TASK_CHECK(b.words().size() == 2);
    // Modular order across zero, not signed order.
    TASK_CHECK(sb[0] == last && sb[1] == 2);
    return 0;
}

int case_pages_prepare_is_pure()
{
    CRcvLossList list(64);
    list.insert(10, 12);
    list.insert(30, 30);

    const int length_before = list.getLossLength();
    CRcvLossList::LossReportPage p1 = list.prepareLossReport(2);
    for (int i = 0; i < 5; ++i)
    {
        CRcvLossList::LossReportPage p = list.prepareLossReport(2);
        TASK_CHECK(p.words() == p1.words()); // identical content every time
    }
    TASK_CHECK(p1.words().size() == 2);
    TASK_CHECK(word_seq(p1.words()[0]) == 10 && p1.words()[1] == 12);
    TASK_CHECK(list.getLossLength() == length_before); // nothing removed

    // The legacy getLossArray still starts from the head, unchanged.
    FixedArray<int32_t> arr(8);
    const int n = list.getLossArray(arr);
    TASK_CHECK(n == 3);
    TASK_CHECK(word_seq(arr[0]) == 10 && arr[1] == 12 && arr[2] == 30);
    return 0;
}

int case_pages_modular_order_not_signed()
{
    // A wrap-crossing range must be encoded in modular order, not sorted as
    // signed integers (which would put 0 before 0x7fffffff).
    CRcvLossList list(64);
    const int32_t hi = CSeqNo::m_iMaxSeqNo;
    list.insert(hi, 1); // [0x7fffffff, 0, 1]

    CRcvLossList::LossReportPage p = list.prepareLossReport(2);
    TASK_CHECK(p.words().size() == 2);
    TASK_CHECK(is_range_first(p.words()[0]));
    TASK_CHECK(word_seq(p.words()[0]) == hi);
    TASK_CHECK(p.words()[1] == 1);

    std::vector<int32_t> seqs;
    TASK_CHECK(decode_words(p.words(), seqs));
    TASK_CHECK(seqs.size() == 3);
    TASK_CHECK(seqs[0] == hi && seqs[1] == 0 && seqs[2] == 1);
    return 0;
}

// ---------------------------------------------------------------------------
// Group "commit"

int case_commit_foreign_and_empty()
{
    CRcvLossList a(16);
    CRcvLossList b(16);
    a.insert(10, 10);
    b.insert(20, 20);

    // Empty page.
    TASK_CHECK(!a.commitLossReport(CRcvLossList::LossReportPage()));

    // Page prepared on another instance.
    CRcvLossList::LossReportPage bp = b.prepareLossReport(4);
    TASK_CHECK(!a.commitLossReport(bp));
    TASK_CHECK(a.commitLossReport(bp) == false);

    // The cursor of "a" was not moved by the rejected commits.
    CRcvLossList::LossReportPage ap = a.prepareLossReport(4);
    TASK_CHECK(ap.words().size() == 1 && ap.words()[0] == 10);
    TASK_CHECK(a.commitLossReport(ap));
    return 0;
}

int case_commit_twice()
{
    CRcvLossList list(16);
    list.insert(10, 10);
    list.insert(20, 20);

    CRcvLossList::LossReportPage p = list.prepareLossReport(1);
    TASK_CHECK(p.words().size() == 1 && p.words()[0] == 10);
    TASK_CHECK(list.commitLossReport(p));

    // Re-committing the same page must fail, and must not move the cursor
    // back to the cursor state of the committed page.
    TASK_CHECK(!list.commitLossReport(p));

    CRcvLossList::LossReportPage next = list.prepareLossReport(1);
    TASK_CHECK(next.words().size() == 1 && next.words()[0] == 20);
    return 0;
}

int case_commit_stale_after_modifications()
{
    // insert invalidates a prepared page, even when it changes nothing.
    {
        CRcvLossList list(16);
        list.insert(10, 10);
        CRcvLossList::LossReportPage p = list.prepareLossReport(1);
        list.insert(10, 10); // identical set: still invalidates
        TASK_CHECK(!list.commitLossReport(p));
    }

    // remove of an absent sequence still invalidates.
    {
        CRcvLossList list(16);
        list.insert(10, 10);
        list.insert(20, 20);
        CRcvLossList::LossReportPage p = list.prepareLossReport(1);
        TASK_CHECK(!list.remove(99)); // no such loss
        TASK_CHECK(!list.commitLossReport(p));
    }

    // removeUpTo with nothing in range still invalidates.
    {
        CRcvLossList list(16);
        list.insert(100, 100);
        CRcvLossList::LossReportPage p = list.prepareLossReport(1);
        list.removeUpTo(50); // older than first: removes nothing
        TASK_CHECK(!list.commitLossReport(p));
    }

    // A range remove that changes nothing visible still invalidates. The
    // range form only reports whether the range arguments were sane, not
    // whether anything was actually removed.
    {
        CRcvLossList list(16);
        list.insert(10, 10);
        list.insert(20, 20);
        CRcvLossList::LossReportPage p = list.prepareLossReport(1);
        list.remove(15, 15);
        TASK_CHECK(list.getLossLength() == 2);
        TASK_CHECK(!list.commitLossReport(p));
    }
    return 0;
}

int case_commit_stale_after_other_commit()
{
    CRcvLossList list(16);
    list.insert(10, 10);
    list.insert(20, 20);
    list.insert(30, 30);

    CRcvLossList::LossReportPage p1 = list.prepareLossReport(1);
    CRcvLossList::LossReportPage p1_again = list.prepareLossReport(1);
    TASK_CHECK(list.commitLossReport(p1));

    // p1_again was prepared in the same state as p1, but another page has
    // been committed since; it must be rejected even though it looks valid.
    TASK_CHECK(!list.commitLossReport(p1_again));

    CRcvLossList::LossReportPage p2 = list.prepareLossReport(1);
    TASK_CHECK(p2.words().size() == 1 && p2.words()[0] == 20);
    return 0;
}

int case_commit_cursor_follows_removed_loss()
{
    CRcvLossList list(16);
    list.insert(10, 10);
    list.insert(20, 20);
    list.insert(30, 30);

    // Commit a page ending at 20; the cursor moves to 21.
    CRcvLossList::LossReportPage p = list.prepareLossReport(2);
    TASK_CHECK(p.words().size() == 2);
    TASK_CHECK(p.words()[0] == 10 && p.words()[1] == 20);
    TASK_CHECK(list.commitLossReport(p));

    CRcvLossList::LossReportPage q = list.prepareLossReport(1);
    TASK_CHECK(q.words()[0] == 30);

    // Lose the sequence right after the cursor (30) before reporting: the
    // next page must wrap to the table head, which is still present.
    TASK_CHECK(list.remove(30));
    CRcvLossList::LossReportPage r = list.prepareLossReport(1);
    TASK_CHECK(r.words()[0] == 10);

    // Clearing the list resets the cursor; a new non-empty list starts at head.
    TASK_CHECK(list.remove(10));
    TASK_CHECK(list.remove(20));
    TASK_CHECK(list.getLossLength() == 0);
    list.insert(500, 500);
    list.insert(600, 600);
    CRcvLossList::LossReportPage t = list.prepareLossReport(1);
    TASK_CHECK(t.words()[0] == 500);
    return 0;
}

int case_commit_through_periodic_path()
{
    CoreHolder holder;
    TASK_CHECK(holder.ok());
    if (!holder.ok())
        return 0;
    CUDT* core = holder.get();
    NakPagesTest::setMaxPayload(core, 8); // 8 payload bytes -> 2 words per page
    TASK_CHECK(core->maxDataPayloadSize() == 8);

    for (int32_t s = 100; s < 130; ++s)
        NakPagesTest::losses(core).insert(s, s);

    CaptureSender sender;

    // Advance manually: one page per call, no spinning on failure.
    std::vector<int32_t> seen;
    sync::steady_clock::time_point now = NakPagesTest::nextNak(core);
    for (int i = 0; i < 40 && seen.size() < 30; ++i)
    {
        now = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
        sender.calls = 0;
        NakPagesTest::stepWith(core, now, sender);
        TASK_CHECK(sender.calls <= 1); // at most one page per period

        for (size_t p = 0; p < sender.pages.size(); ++p)
        {
            std::vector<int32_t> seqs;
            TASK_CHECK(decode_words(sender.pages[p], seqs));
            for (size_t k = 0; k < seqs.size(); ++k)
                seen.push_back(seqs[k]);
        }
        sender.pages.clear();
    }

    TASK_CHECK(seen.size() == 30);
    std::vector<int32_t> expected;
    for (int32_t s = 100; s < 130; ++s)
        expected.push_back(s);
    TASK_CHECK(same_set(seen, expected));
    TASK_CHECK(NakPagesTest::sentNak(core) == 15); // each 2-word page carries 2 losses
    return 0;
}

int case_commit_failure_resends_same_page()
{
    CoreHolder holder;
    TASK_CHECK(holder.ok());
    if (!holder.ok())
        return 0;
    CUDT* core = holder.get();
    NakPagesTest::setMaxPayload(core, 12); // 3 words per page
    TASK_CHECK(core->maxDataPayloadSize() == 12);

    NakPagesTest::losses(core).insert(100, 100);
    NakPagesTest::losses(core).insert(200, 200);
    NakPagesTest::losses(core).insert(300, 300);
    NakPagesTest::resetSentNak(core);

    CaptureSender failing;
    failing.fail = true;
    const sync::steady_clock::time_point t1 = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    NakPagesTest::stepWith(core, t1, failing);

    // Failure: one attempt, no sentNak, cursor unchanged.
    TASK_CHECK(failing.calls == 1);
    TASK_CHECK(NakPagesTest::sentNak(core) == 0);

    CaptureSender ok;
    const sync::steady_clock::time_point t2 = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    NakPagesTest::stepWith(core, t2, ok);
    TASK_CHECK(ok.calls == 1);
    TASK_CHECK(ok.pages.size() == 1);
    // Same first page as if the failed attempt had never happened.
    std::vector<int32_t> seqs;
    TASK_CHECK(decode_words(ok.pages[0], seqs));
    TASK_CHECK(seqs.size() == 3);
    TASK_CHECK(seqs[0] == 100 && seqs[1] == 200 && seqs[2] == 300);
    TASK_CHECK(NakPagesTest::sentNak(core) == 1);

    // Failure/attempt inside the same period is not repeated: the schedule
    // has already advanced.
    CaptureSender early;
    early.fail = true;
    NakPagesTest::stepWith(core, t2, early);
    TASK_CHECK(early.calls == 0);
    return 0;
}

int case_commit_snapshot_allowed_but_stale()
{
    CoreHolder holder;
    TASK_CHECK(holder.ok());
    if (!holder.ok())
        return 0;
    CUDT* core = holder.get();
    NakPagesTest::setMaxPayload(core, 8);

    NakPagesTest::losses(core).insert(100, 100);
    NakPagesTest::losses(core).insert(200, 200);
    NakPagesTest::losses(core).insert(300, 300);
    NakPagesTest::resetSentNak(core);

    // A sender that mutates the loss list during the send: the already built
    // snapshot is still allowed out, but its commit must fail, so the next
    // period prepares the page again from the head.
    struct MutatingSender
    {
        CUDT* core;
        static bool send(void* opaque, const sockaddr_any&, CPacket& packet, const sockaddr_any&)
        {
            MutatingSender* self = static_cast<MutatingSender*>(opaque);
            if (!packet.isControl(UMSG_LOSSREPORT))
                return false;
            // Modification while the packet is in flight.
            NakPagesTest::insertLoss(self->core, 400, 400);
            return true;
        }
    };

    MutatingSender mut;
    mut.core = core;
    const sync::steady_clock::time_point t = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    NakPagesTest::stepRaw(core, t, &MutatingSender::send, &mut);
    TASK_CHECK(NakPagesTest::sentNak(core) == 1); // the snapshot went out

    // Next period: commit failed, so the page restarts from the head (100),
    // not from after the previously sent prefix.
    CaptureSender ok;
    const sync::steady_clock::time_point t2 = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    NakPagesTest::stepWith(core, t2, ok);
    TASK_CHECK(ok.pages.size() == 1);
    std::vector<int32_t> seqs;
    TASK_CHECK(decode_words(ok.pages[0], seqs));
    TASK_CHECK(seqs.size() == 2);
    TASK_CHECK(seqs[0] == 100 && seqs[1] == 200);
    return 0;
}

int case_commit_nakreport_and_filter_gate()
{
    CoreHolder holder;
    TASK_CHECK(holder.ok());
    if (!holder.ok())
        return 0;
    CUDT* core = holder.get();
    NakPagesTest::setMaxPayload(core, 8);
    NakPagesTest::losses(core).insert(100, 100);

    CaptureSender sender;

    // NAKREPORT disabled.
    NakPagesTest::setNakEnabled(core, false);
    sync::steady_clock::time_point now = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    TASK_CHECK(NakPagesTest::stepWith(core, now, sender) == 0);
    TASK_CHECK(sender.calls == 0);

    // Enabled, but the packet filter isn't in ALWAYS mode.
    NakPagesTest::setNakEnabled(core, true);
    NakPagesTest::setFilterLevel(core, SRT_ARQ_ONREQ);
    now = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    TASK_CHECK(NakPagesTest::stepWith(core, now, sender) == 0);
    TASK_CHECK(sender.calls == 0);

    // Restored: the periodic page is sent.
    NakPagesTest::setFilterLevel(core, SRT_ARQ_ALWAYS);
    now = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    TASK_CHECK(NakPagesTest::stepWith(core, now, sender) != 0);
    TASK_CHECK(sender.calls == 1);
    return 0;
}

int case_commit_no_immediate_spin_and_one_page_per_period()
{
    CoreHolder holder;
    TASK_CHECK(holder.ok());
    if (!holder.ok())
        return 0;
    CUDT* core = holder.get();
    NakPagesTest::setMaxPayload(core, 8); // 2 words per page
    for (int32_t s = 0; s < 10; ++s)
        NakPagesTest::losses(core).insert(1000 + s, 1000 + s);

    CaptureSender sender;

    const sync::steady_clock::time_point t = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    NakPagesTest::stepWith(core, t, sender);
    TASK_CHECK(sender.pages.size() == 1);

    // Calling again immediately (same time) does nothing.
    NakPagesTest::stepWith(core, t, sender);
    TASK_CHECK(sender.pages.size() == 1);

    // Next period sends the next page, not the remaining pages at once.
    const sync::steady_clock::time_point t2 = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    NakPagesTest::stepWith(core, t2, sender);
    TASK_CHECK(sender.pages.size() == 2);
    std::vector<int32_t> p2;
    TASK_CHECK(decode_words(sender.pages[1], p2));
    TASK_CHECK(p2.size() == 2 && p2[0] == 1002 && p2[1] == 1003);
    return 0;
}

int case_commit_heap_reuse_and_range_restore()
{
    // A remove that splits a range must invalidate the old page: the token
    // check cannot rely on node slots or on the range shape.
    CRcvLossList list(32);
    list.insert(100, 110);

    CRcvLossList::LossReportPage p = list.prepareLossReport(4);
    TASK_CHECK(word_seq(p.words()[0]) == 100 && p.words()[1] == 110);

    TASK_CHECK(list.remove(105)); // split into [100,104] and [106,110]
    TASK_CHECK(!list.commitLossReport(p));

    // Pick up the split state and make sure both halves are reported. After a
    // rejected commit the cursor is unchanged (still at the head).
    CRcvLossList::LossReportPage a = list.prepareLossReport(2);
    std::vector<int32_t> sa;
    TASK_CHECK(decode_words(a.words(), sa));
    TASK_CHECK(sa.size() == 5 && sa[0] == 100 && sa[4] == 104);
    TASK_CHECK(list.commitLossReport(a));

    CRcvLossList::LossReportPage b = list.prepareLossReport(2);
    std::vector<int32_t> sb;
    TASK_CHECK(decode_words(b.words(), sb));
    TASK_CHECK(sb.size() == 5 && sb[0] == 106 && sb[4] == 110);
    return 0;
}

int case_commit_page_after_complete_sweep()
{
    CRcvLossList list(64);
    list.insert(100, 100);
    list.insert(200, 200);

    // A single page covering the entire list: cursor was on 100, the page
    // ends at 200 (the last loss) so the next period starts at the head.
    CRcvLossList::LossReportPage p = list.prepareLossReport(4);
    std::vector<int32_t> s;
    TASK_CHECK(decode_words(p.words(), s));
    TASK_CHECK(s.size() == 2);
    TASK_CHECK(list.commitLossReport(p));

    CRcvLossList::LossReportPage next = list.prepareLossReport(4);
    std::vector<int32_t> n;
    TASK_CHECK(decode_words(next.words(), n));
    TASK_CHECK(n.size() == 2 && n[0] == 100 && n[1] == 200);
    return 0;
}

// ---------------------------------------------------------------------------
// Group "playback"
//
// Replays identifiable single-packet media messages through the real
// CUnitQueue/CRcvBuffer/CRcvLossList and drives the production TSBPD drop
// coordination entry (CUDT::rcvDropTooLateUpTo) by hand. Before the packet at
// the head is due the gap must stay; after it is due the gap is dropped from
// both the loss list and the FreshLoss list, and a later NAK report must not
// resurrect it.

class PlaybackFixture
{
public:
    explicit PlaybackFixture(int32_t first_seqno = 101)
        : units(32, 1500)
        , core(NakPagesTest::create(&units, first_seqno))
    {
        const sync::steady_clock::time_point base =
            sync::steady_clock::time_point() + sync::microseconds_from(1000000);
        // Single-message-per-packet setup: seqno 101 is the first message.
        NakPagesTest::configurePlayback(core, 101, base, sync::microseconds_from(1000));
        timebase = base;
        good     = true;
    }

    ~PlaybackFixture()
    {
        // Release the units still owned by the buffer before the queue goes.
        NakPagesTest::destroy(core);
    }

    bool ok() const { return core != NULL && good; }

    bool put(int32_t seqno, uint32_t ts_us, char payload, int msgno)
    {
        CUnit* unit = units.getNextAvailUnit();
        if (!unit)
        {
            good = false;
            return false;
        }
        CPacket& p = unit->m_Packet;
        p.set_seqno(seqno);
        p.set_timestamp(ts_us);
        p.set_msgflags(PacketBoundaryBits(PB_SOLO) | MSGNO_PACKET_INORDER::wrap(1) |
                       MSGNO_SEQ::wrap(msgno));
        p.setLength(1);
        p.data()[0] = payload;
        if (NakPagesTest::buffer(core).insert(unit) != 0)
        {
            good = false;
            return false;
        }
        NakPagesTest::noteReceived(core, seqno);
        return true;
    }

    CUDT* get() { return core; }
    sync::steady_clock::time_point timebase;

private:
    CUnitQueue units;
    CUDT*      core;
    bool       good;
};

int case_playback_gap_until_due_then_drop()
{
    PlaybackFixture f;
    TASK_CHECK(f.ok());
    if (!f.ok())
        return 0;
    CUDT* core = f.get();

    // Peer ISN is 101 and the first two packets (101,102) are missing.
    // Messages 2..4 arrive at 103,105,107; 104/106 are also gaps.
    // Payload letter and message number identify each message.
    TASK_CHECK(f.put(103, 0, 'B', 2));
    TASK_CHECK(f.put(105, 1000, 'C', 3));
    TASK_CHECK(f.put(107, 2000, 'D', 4));

    // Record the gaps the way CUDT::processData() does: loss list + FreshLoss.
    NakPagesTest::insertLoss(core, 101, 102);
    NakPagesTest::insertLoss(core, 104, 104);
    NakPagesTest::insertLoss(core, 106, 106);
    NakPagesTest::addFreshLoss(core, 101, 102, 2);
    NakPagesTest::addFreshLoss(core, 104, 104, 2);
    NakPagesTest::addFreshLoss(core, 106, 106, 2);
    TASK_CHECK(NakPagesTest::lossLength(core) == 4);

    // The first present packet is 103; the gap precedes it.
    const sync::steady_clock::time_point first_due = f.timebase + sync::microseconds_from(1000);

    // Before the head packet is due: the gap must be kept intact.
    TASK_CHECK(NakPagesTest::playbackDrop(core, first_due - sync::microseconds_from(1)) == 0);
    TASK_CHECK(NakPagesTest::lossLength(core) == 4);
    TASK_CHECK(NakPagesTest::freshLossCount(core) == 3);
    TASK_CHECK(NakPagesTest::freshLossHas(core, 101));
    TASK_CHECK(!NakPagesTest::buffer(core).isRcvDataReady(first_due - sync::microseconds_from(1)));

    // At/after due: production drop coordination skips 101..102.
    TASK_CHECK(NakPagesTest::playbackDrop(core, first_due) > 0);
    TASK_CHECK(!NakPagesTest::losses(core).find(101, 102));
    TASK_CHECK(NakPagesTest::losses(core).find(104, 106));
    TASK_CHECK(NakPagesTest::lossLength(core) == 2);
    // FreshLoss for the played-out gap is gone; later gaps are still pending.
    TASK_CHECK(!NakPagesTest::freshLossHas(core, 101));
    TASK_CHECK(!NakPagesTest::freshLossHas(core, 102));
    TASK_CHECK(NakPagesTest::freshLossHas(core, 104));

    // Message 2 is now readable with its real content and number.
    TASK_CHECK(NakPagesTest::buffer(core).isRcvDataReady(first_due));
    char out[8] = {0};
    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    TASK_CHECK(NakPagesTest::buffer(core).readMessage(out, sizeof out, &ctrl) == 1);
    TASK_CHECK(out[0] == 'B');
    TASK_CHECK(ctrl.msgno == 2);

    // A later periodic report must not resurrect the played-out gaps.
    CaptureSender sender;
    NakPagesTest::resetSentNak(core);
    NakPagesTest::setNextNak(core, sync::steady_clock::time_point());
    NakPagesTest::stepWith(core, first_due, sender);
    TASK_CHECK(sender.pages.size() == 1);
    std::vector<int32_t> seqs;
    TASK_CHECK(decode_words(sender.pages[0], seqs));
    TASK_CHECK(!std::count(seqs.begin(), seqs.end(), 101));
    TASK_CHECK(!std::count(seqs.begin(), seqs.end(), 102));
    TASK_CHECK(std::find(seqs.begin(), seqs.end(), 104) != seqs.end());
    TASK_CHECK(std::find(seqs.begin(), seqs.end(), 106) != seqs.end());

    // Advance to message 3's due time: gap 104 is dropped, then 'C' is read.
    const sync::steady_clock::time_point second_due = f.timebase + sync::microseconds_from(2000);
    TASK_CHECK(NakPagesTest::playbackDrop(core, second_due) > 0);
    TASK_CHECK(!NakPagesTest::losses(core).find(104, 104));
    TASK_CHECK(NakPagesTest::losses(core).find(106, 106));
    TASK_CHECK(!NakPagesTest::freshLossHas(core, 104));

    char out2[8] = {0};
    SRT_MSGCTRL ctrl2 = srt_msgctrl_default;
    TASK_CHECK(NakPagesTest::buffer(core).readMessage(out2, sizeof out2, &ctrl2) == 1);
    TASK_CHECK(out2[0] == 'C');
    TASK_CHECK(ctrl2.msgno == 3);

    // The played-out gaps stay absent from subsequent reports.
    CaptureSender sender2;
    NakPagesTest::setNextNak(core, sync::steady_clock::time_point());
    NakPagesTest::stepWith(core, second_due, sender2);
    TASK_CHECK(sender2.pages.size() == 1);
    std::vector<int32_t> seqs2;
    TASK_CHECK(decode_words(sender2.pages[0], seqs2));
    TASK_CHECK(!std::count(seqs2.begin(), seqs2.end(), 101));
    TASK_CHECK(!std::count(seqs2.begin(), seqs2.end(), 102));
    TASK_CHECK(!std::count(seqs2.begin(), seqs2.end(), 104));
    TASK_CHECK(std::find(seqs2.begin(), seqs2.end(), 106) != seqs2.end());
    return 0;
}

int case_playback_after_drop_successor_readable()
{
    PlaybackFixture f(201);
    TASK_CHECK(f.ok());
    if (!f.ok())
        return 0;
    CUDT* core = f.get();

    // 201 is missing; 202 and 204 are present and 203 is a later gap.
    TASK_CHECK(f.put(202, 0, 'X', 10));
    TASK_CHECK(f.put(204, 1000, 'Y', 11));
    NakPagesTest::insertLoss(core, 201, 201);
    NakPagesTest::insertLoss(core, 203, 203);
    NakPagesTest::addFreshLoss(core, 201, 201, 2);
    NakPagesTest::addFreshLoss(core, 203, 203, 2);

    const sync::steady_clock::time_point due = f.timebase + sync::microseconds_from(1000);
    TASK_CHECK(!NakPagesTest::buffer(core).isRcvDataReady(due - sync::microseconds_from(1)));

    // Production drop advances past 201; message 'X' (msgno 10) is delivered.
    TASK_CHECK(NakPagesTest::playbackDrop(core, due) > 0);
    TASK_CHECK(NakPagesTest::lossLength(core) == 1);
    TASK_CHECK(NakPagesTest::losses(core).find(203, 203));
    TASK_CHECK(!NakPagesTest::freshLossHas(core, 201));
    TASK_CHECK(NakPagesTest::freshLossHas(core, 203));

    char out[8] = {0};
    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    TASK_CHECK(NakPagesTest::buffer(core).readMessage(out, sizeof out, &ctrl) == 1);
    TASK_CHECK(out[0] == 'X');
    TASK_CHECK(ctrl.msgno == 10);

    // The next message 'Y' at 204 is blocked by gap 203 until its due time.
    const sync::steady_clock::time_point due2 = f.timebase + sync::microseconds_from(2000);
    TASK_CHECK(NakPagesTest::playbackDrop(core, due2) > 0);
    TASK_CHECK(NakPagesTest::lossLength(core) == 0);
    TASK_CHECK(NakPagesTest::freshLossCount(core) == 0);

    char out2[8] = {0};
    SRT_MSGCTRL ctrl2 = srt_msgctrl_default;
    TASK_CHECK(NakPagesTest::buffer(core).readMessage(out2, sizeof out2, &ctrl2) == 1);
    TASK_CHECK(out2[0] == 'Y');
    TASK_CHECK(ctrl2.msgno == 11);

    // No loss is reported anymore.
    CaptureSender sender;
    NakPagesTest::setNextNak(core, sync::steady_clock::time_point());
    NakPagesTest::stepWith(core, due2, sender);
    TASK_CHECK(sender.pages.empty());
    TASK_CHECK(NakPagesTest::sentNak(core) == 0);
    return 0;
}

int case_playback_empty_page_after_drop()
{
    // A list with all losses dropped must yield no periodic page at all.
    PlaybackFixture f(300);
    TASK_CHECK(f.ok());
    if (!f.ok())
        return 0;
    CUDT* core = f.get();
    TASK_CHECK(f.put(301, 0, 'Z', 1));
    NakPagesTest::insertLoss(core, 300, 300);
    NakPagesTest::addFreshLoss(core, 300, 300, 0);

    const sync::steady_clock::time_point due = f.timebase + sync::microseconds_from(1000);
    TASK_CHECK(NakPagesTest::playbackDrop(core, due) > 0);
    TASK_CHECK(NakPagesTest::lossLength(core) == 0);
    TASK_CHECK(NakPagesTest::freshLossCount(core) == 0);

    CaptureSender sender;
    NakPagesTest::setNextNak(core, sync::steady_clock::time_point());
    NakPagesTest::stepWith(core, due, sender);
    TASK_CHECK(sender.calls == 0);
    TASK_CHECK(sender.pages.empty());
    return 0;
}

int case_commit_production_sender_honours_channel_result()
{
    CoreHolder holder;
    TASK_CHECK(holder.ok());
    if (!holder.ok())
        return 0;
    CUDT* core = holder.get();
    NakPagesTest::setMaxPayload(core, 8);
    NakPagesTest::losses(core).insert(100, 100);
    NakPagesTest::losses(core).insert(200, 200);

    // This fixture has no sending queue: the production sender must report
    // failure rather than deriving success from the packet length.
    CPacket pkt;
    int32_t words[2] = {100, 200};
    pkt.pack(UMSG_LOSSREPORT, NULL, words, sizeof words);
    TASK_CHECK(!NakPagesTest::defaultSenderWithNoQueue(core, pkt));

    // The full periodic step therefore reports nothing and keeps the cursor.
    CaptureSender sender;
    sender.fail = true;
    NakPagesTest::resetSentNak(core);
    const sync::steady_clock::time_point now = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    TASK_CHECK(NakPagesTest::stepWith(core, now, sender) == NakPagesTest::becauseNakReport());
    TASK_CHECK(sender.calls == 1);
    TASK_CHECK(NakPagesTest::sentNak(core) == 0);

    CaptureSender ok;
    const sync::steady_clock::time_point t2 = NakPagesTest::nextNak(core) + sync::microseconds_from(1);
    NakPagesTest::stepWith(core, t2, ok);
    TASK_CHECK(ok.pages.size() == 1);
    std::vector<int32_t> seqs;
    TASK_CHECK(decode_words(ok.pages[0], seqs));
    TASK_CHECK(seqs.size() == 2 && seqs[0] == 100 && seqs[1] == 200);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------

const task_case task_cases[] = {
    {"pages", "empty_and_zero_budget", case_pages_empty_and_zero_budget},
    {"pages", "budget_one_and_two", case_pages_budget_one_and_two},
    {"pages", "cover_without_prefix_repeat", case_pages_cover_without_prefix_repeat},
    {"pages", "range_continuation", case_pages_range_continuation},
    {"pages", "cross_zero", case_pages_cross_zero},
    {"pages", "prepare_is_pure", case_pages_prepare_is_pure},
    {"pages", "modular_order_not_signed", case_pages_modular_order_not_signed},

    {"commit", "foreign_and_empty", case_commit_foreign_and_empty},
    {"commit", "commit_twice", case_commit_twice},
    {"commit", "stale_after_modifications", case_commit_stale_after_modifications},
    {"commit", "stale_after_other_commit", case_commit_stale_after_other_commit},
    {"commit", "cursor_follows_removed_loss", case_commit_cursor_follows_removed_loss},
    {"commit", "through_periodic_path", case_commit_through_periodic_path},
    {"commit", "failure_resends_same_page", case_commit_failure_resends_same_page},
    {"commit", "snapshot_allowed_but_stale", case_commit_snapshot_allowed_but_stale},
    {"commit", "nakreport_and_filter_gate", case_commit_nakreport_and_filter_gate},
    {"commit", "no_spin_one_page_per_period", case_commit_no_immediate_spin_and_one_page_per_period},
    {"commit", "heap_reuse_and_range_restore", case_commit_heap_reuse_and_range_restore},
    {"commit", "page_after_complete_sweep", case_commit_page_after_complete_sweep},
    {"commit", "production_sender_honours_channel", case_commit_production_sender_honours_channel_result},

    {"playback", "gap_until_due_then_drop", case_playback_gap_until_due_then_drop},
    {"playback", "after_drop_successor_readable", case_playback_after_drop_successor_readable},
    {"playback", "empty_page_after_drop", case_playback_empty_page_after_drop},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
