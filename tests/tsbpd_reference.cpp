// Deterministic reference tests for the SRT receiver TSBPD clock path.
//
// An independent integer ledger (all times in microseconds) models the sender
// timestamps, the receiver base time, the negotiated delay and the drift
// window.  The ledger drives CTsbpdTime and the real CRcvBuffer/CUnitQueue/
// CPacket stack; every expected value is computed from the ledger's own
// integer arithmetic, never from production clock code.  No network, no
// sleeping, no wall clock.

#include "task_support/check.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tsbpd_time.h"
#include "buffer_rcv.h"
#include "queue.h"
#include "packet.h"
#include "sync.h"

using namespace srt;
using namespace srt::sync;

namespace {

typedef steady_clock::time_point time_point;

// ---------------------------------------------------------------------------
// Integer ledger: independent model of the TSBPD clock state.
// ---------------------------------------------------------------------------

static const int64_t WRAP_PERIOD_US = 30000000;     // 30 s wrap check period
static const int64_t WRAP_SPAN_US   = 4294967296LL; // 2^32 us timestamp span
static const uint32_t MAX_TIMESTAMP = 0xFFFFFFFFu;  // 32-bit microsecond timestamps
static const int     DRIFT_WINDOW   = 1000;         // samples per drift calculation
static const int64_t DRIFT_MAX      = 5000;         // max drift absorbed per commit

struct Ledger
{
    int64_t base_us;      // committed TSBPD base time
    int64_t delay_us;     // negotiated TSBPD delay
    int64_t drift_us;     // committed drift residual (after the last window)
    bool    wrap_check;   // inside the timestamp wrap check period
    int64_t window_sum;   // accumulated drift samples of the current window
    int     window_count; // samples collected in the current window
    int     first_rtt;    // first valid RTT sample, -1 while unknown
};

Ledger ledger_make(int64_t base_us, int64_t delay_us)
{
    Ledger L;
    L.base_us      = base_us;
    L.delay_us     = delay_us;
    L.drift_us     = 0;
    L.wrap_check   = false;
    L.window_sum   = 0;
    L.window_count = 0;
    L.first_rtt    = -1;
    return L;
}

// Carryover applied to the base time for a given packet timestamp.
int64_t ledger_carry(const Ledger& L, uint32_t ts)
{
    return (L.wrap_check && ts <= uint32_t(2 * WRAP_PERIOD_US)) ? WRAP_SPAN_US : 0;
}

// Source time of a packet without delay and drift.
int64_t ledger_pkt_base(const Ledger& L, uint32_t ts)
{
    return L.base_us + ledger_carry(L, ts) + int64_t(ts);
}

// Delivery time of a packet with the negotiated delay and current drift.
int64_t ledger_pkt_time(const Ledger& L, uint32_t ts)
{
    return ledger_pkt_base(L, ts) + L.delay_us + L.drift_us;
}

// RTT delta compensation for one sample: half of the change against the
// first valid RTT; zero when the sample carries no RTT or establishes it.
int64_t ledger_rtt_delta(const Ledger& L, int rtt)
{
    if (rtt < 0 || L.first_rtt == -1)
        return 0;
    return (rtt - L.first_rtt) / 2;
}

// Push one drift sample into the window; commit on a full window.
bool ledger_window_push(Ledger& L, int64_t sample_us)
{
    L.window_sum += sample_us;
    ++L.window_count;
    if (L.window_count < DRIFT_WINDOW)
        return false;

    const int64_t avg = L.window_sum / L.window_count;
    L.window_sum      = 0;
    L.window_count    = 0;

    int64_t over = 0;
    if (avg > DRIFT_MAX)
        over = DRIFT_MAX;
    else if (avg < -DRIFT_MAX)
        over = -DRIFT_MAX;

    L.drift_us = avg - over; // residual stays in the drift term
    L.base_us += over;       // only the clamped part moves into the base time
    return true;
}

// Ledger model of one ACKACK drift sample with RTT delta compensation.
bool ledger_add_drift(Ledger& L, uint32_t ts, int64_t arrival_us, int rtt)
{
    if (L.first_rtt == -1)
        L.first_rtt = rtt;
    const int64_t delta  = (rtt >= 0) ? (rtt - L.first_rtt) / 2 : 0;
    const int64_t sample = arrival_us - ledger_pkt_base(L, ts) - delta;
    return ledger_window_push(L, sample);
}

// Degenerate model: RTT delta compensation dropped.
bool ledger_add_drift_no_rtt_delta(Ledger& L, uint32_t ts, int64_t arrival_us, int rtt)
{
    if (L.first_rtt == -1)
        L.first_rtt = rtt;
    const int64_t sample = arrival_us - ledger_pkt_base(L, ts);
    return ledger_window_push(L, sample);
}

// Ledger model of the data-packet driven wrap check.
void ledger_update_base(Ledger& L, uint32_t ts)
{
    if (L.wrap_check)
    {
        if (ts >= uint32_t(WRAP_PERIOD_US) && ts <= uint32_t(2 * WRAP_PERIOD_US))
        {
            L.wrap_check = false;
            L.base_us += WRAP_SPAN_US;
        }
        return;
    }
    if (int64_t(ts) > int64_t(MAX_TIMESTAMP) - WRAP_PERIOD_US)
        L.wrap_check = true;
}

// ---------------------------------------------------------------------------
// Time helpers.
// ---------------------------------------------------------------------------

time_point tp_us(int64_t us)
{
    return time_point() + microseconds_from(us);
}

int64_t us_of(const time_point& tp)
{
    return count_microseconds(tp.time_since_epoch());
}

// ---------------------------------------------------------------------------
// Check helpers.  Failures are recorded with TASK_CHECK and propagate upwards
// through the int return value.
// ---------------------------------------------------------------------------

#define TASK_PROPAGATE(expr) do { if ((expr) != 0) return 1; } while (0)

// Compare the production clock against the ledger for one packet timestamp:
// carried base time, source time without delay and delivery time with the
// negotiated delay are all checked independently.
int check_clock(const CTsbpdTime& clock, const Ledger& L, uint32_t ts)
{
    TASK_CHECK(us_of(clock.getBaseTime(ts)) == L.base_us + ledger_carry(L, ts));
    TASK_CHECK(us_of(clock.getPktBaseTime(ts)) == ledger_pkt_base(L, ts));
    TASK_CHECK(us_of(clock.getPktTime(ts)) == ledger_pkt_time(L, ts));
    TASK_CHECK(clock.drift() == L.drift_us);
    return 0;
}

// Compare the exported internal state against the ledger.
int check_internal(const CTsbpdTime& clock, const Ledger& L)
{
    time_point          tb;
    bool                wrp = false;
    steady_clock::duration ud;
    clock.getInternalTimeBase(tb, wrp, ud);
    TASK_CHECK(us_of(tb) == L.base_us);
    TASK_CHECK(wrp == L.wrap_check);
    TASK_CHECK(count_microseconds(ud) == L.drift_us);
    return 0;
}

// Feed one ACKACK-style drift sample.  The arrival time is generated from the
// ledger so that the drift sample SRT computes equals sample_us.
int feed_drift_sample(CTsbpdTime& clock, Ledger& L, uint32_t ts, int rtt, int64_t sample_us)
{
    const int64_t arrival = ledger_pkt_base(L, ts) + ledger_rtt_delta(L, rtt) + sample_us;
    const bool prod = clock.addDriftSample(ts, tp_us(arrival), rtt);
    const bool led  = ledger_add_drift(L, ts, arrival, rtt);
    TASK_CHECK(prod == led);
    return 0;
}

// Feed count identical drift samples; reports whether a commit happened.
int feed_window(CTsbpdTime& clock, Ledger& L, uint32_t ts, int rtt, int64_t sample_us,
                int count, bool* committed)
{
    bool any = false;
    for (int i = 0; i < count; ++i)
    {
        const int64_t arrival = ledger_pkt_base(L, ts) + ledger_rtt_delta(L, rtt) + sample_us;
        const bool prod = clock.addDriftSample(ts, tp_us(arrival), rtt);
        const bool led  = ledger_add_drift(L, ts, arrival, rtt);
        TASK_CHECK(prod == led);
        if (prod)
            any = true;
    }
    if (committed)
        *committed = any;
    return 0;
}

// ---------------------------------------------------------------------------
// Receiver buffer helpers (real CUnitQueue / CPacket / CRcvBuffer path).
// ---------------------------------------------------------------------------

int insert_solo(CUnitQueue& queue, CRcvBuffer& rcv, int32_t seqno, uint32_t ts,
                int32_t msgno, const char* payload, size_t len)
{
    CUnit* u = queue.getNextAvailUnit();
    TASK_CHECK(u != NULL);
    u->m_Packet.set_seqno(seqno);
    u->m_Packet.set_timestamp(int32_t(ts));
    u->m_Packet.set_msgflags(PacketBoundaryBits(PB_SOLO) | MSGNO_PACKET_INORDER::wrap(1)
                             | MSGNO_SEQ::wrap(msgno));
    u->m_Packet.setLength(len);
    memcpy(u->m_Packet.data(), payload, len);
    TASK_CHECK(rcv.insert(u) == 0);
    return 0;
}

// Per the source convention readiness is queried before readMessage(); the
// read itself is then expected to return the whole message immediately.
int read_and_check(CRcvBuffer& rcv, const char* expected, size_t len, int64_t due_us,
                   int32_t seqno, int32_t msgno)
{
    TASK_CHECK(rcv.isRcvDataReady(tp_us(due_us)));
    char         buf[256];
    SRT_MSGCTRL  ctrl;
    memset(buf, 0, sizeof(buf));
    memset(&ctrl, 0, sizeof(ctrl));
    const int n = rcv.readMessage(buf, sizeof(buf), &ctrl);
    TASK_CHECK(n == int(len));
    TASK_CHECK(memcmp(buf, expected, len) == 0);
    TASK_CHECK(ctrl.srctime == due_us);
    TASK_CHECK(ctrl.pktseq == seqno);
    TASK_CHECK(ctrl.msgno == msgno);
    return 0;
}

// ---------------------------------------------------------------------------
// Group "drift".
// ---------------------------------------------------------------------------

// The drift window commits exactly on the 1000th sample, not earlier.
int case_drift_window_commit()
{
    CTsbpdTime clock;
    Ledger     L = ledger_make(10000000, 120000);
    clock.setTsbPdMode(tp_us(L.base_us), false, microseconds_from(L.delay_us));

    const uint32_t ts = 500000;
    bool committed = true;

    // 999 samples must not commit the window yet.
    TASK_PROPAGATE(feed_window(clock, L, ts, -1, 3000, DRIFT_WINDOW - 1, &committed));
    TASK_CHECK(!committed);
    TASK_CHECK(clock.drift() == 0);
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts));

    // The 1000th sample commits: average 3000 stays within the limit, so the
    // base time is untouched and the whole value lands in the drift term.
    committed = false;
    TASK_PROPAGATE(feed_window(clock, L, ts, -1, 3000, 1, &committed));
    TASK_CHECK(committed);
    TASK_CHECK(clock.drift() == 3000);
    TASK_CHECK(clock.overdrift() == 0);
    TASK_CHECK(L.base_us == 10000000);
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts));
    return 0;
}

// The first valid RTT sample establishes the compensation baseline; later RTT
// changes are compensated by half of the difference; samples without RTT are
// handled separately.  Dropping the compensation would be caught here.
int case_drift_rtt_delta_halving()
{
    CTsbpdTime clock;
    Ledger     L    = ledger_make(10000000, 120000);
    Ledger     Ldeg = L; // degenerate model: RTT delta compensation dropped
    clock.setTsbPdMode(tp_us(L.base_us), false, microseconds_from(L.delay_us));

    const uint32_t ts = 700000;
    for (int i = 0; i < DRIFT_WINDOW; ++i)
    {
        int rtt;
        if (i < 10)
            rtt = -1;        // no RTT sample: must not set the baseline
        else if (i == 10)
            rtt = 40000;     // first valid RTT: establishes the baseline
        else if (i < 510)
            rtt = 41777;     // changed RTT: delta (41777-40000)/2 = 888 us
        else
            rtt = -1;        // no RTT sample: delta must be zero

        // Arrival generated from the ledger so the compensated sample is 0.
        const int64_t arrival = ledger_pkt_base(L, ts) + ledger_rtt_delta(L, rtt);
        const bool prod = clock.addDriftSample(ts, tp_us(arrival), rtt);
        const bool led  = ledger_add_drift(L, ts, arrival, rtt);
        const bool ldeg = ledger_add_drift_no_rtt_delta(Ldeg, ts, arrival, rtt);
        TASK_CHECK(prod == led);
        TASK_CHECK(led == ldeg); // commit timing must not depend on the model
    }

    TASK_CHECK(L.drift_us == 0);              // RTT delta fully compensated
    TASK_CHECK(Ldeg.drift_us != L.drift_us);  // degenerate model must differ
    TASK_CHECK(clock.drift() == L.drift_us);
    TASK_CHECK(clock.drift() != Ldeg.drift_us); // dropped compensation caught
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts));
    return 0;
}

// Overdrift in both directions: only the clamped part moves into the base
// time, the residual stays in the drift term, and consecutive windows of
// different signs do not share their accumulated sums.
int case_drift_overdrift_split()
{
    CTsbpdTime clock;
    Ledger     L = ledger_make(10000000, 120000);
    clock.setTsbPdMode(tp_us(L.base_us), false, microseconds_from(L.delay_us));

    const uint32_t ts = 900000;
    bool committed = false;

    // Window 1: average +7300 -> only +5000 may move into the base time.
    TASK_PROPAGATE(feed_window(clock, L, ts, -1, 7300, DRIFT_WINDOW, &committed));
    TASK_CHECK(committed);
    TASK_CHECK(clock.overdrift() == 5000);
    TASK_CHECK(L.base_us == 10000000 + 5000);
    TASK_CHECK(L.drift_us == 2300);
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts));

    // 999 samples of the opposite sign must not commit yet; the committed
    // base time and the current residual stay effective for delivery times.
    committed = true;
    TASK_PROPAGATE(feed_window(clock, L, ts, -1, -6800, DRIFT_WINDOW - 1, &committed));
    TASK_CHECK(!committed);
    TASK_CHECK(clock.drift() == 2300);
    TASK_CHECK(clock.overdrift() == 0); // cleared again by the next sample
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts));

    // Sample 1000 commits: -5000 moves back out of the base time.
    committed = false;
    TASK_PROPAGATE(feed_window(clock, L, ts, -1, -6800, 1, &committed));
    TASK_CHECK(committed);
    TASK_CHECK(clock.overdrift() == -5000);
    TASK_CHECK(L.base_us == 10000000);
    TASK_CHECK(L.drift_us == -1800);
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts));

    // A fresh window starts from an empty sum: average +100, no overdrift.
    // A leaked window sum would shift this average.
    committed = false;
    TASK_PROPAGATE(feed_window(clock, L, ts, -1, 100, DRIFT_WINDOW, &committed));
    TASK_CHECK(committed);
    TASK_CHECK(clock.overdrift() == 0);
    TASK_CHECK(L.base_us == 10000000);
    TASK_CHECK(L.drift_us == 100);
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts));
    return 0;
}

// ---------------------------------------------------------------------------
// Group "handoff".
// ---------------------------------------------------------------------------

// One full 32-bit microsecond timestamp wrap: data packets drive the wrap
// check period, control samples must not commit the carryover, and source /
// delivery times are checked before, during and after the check period.
int case_handoff_wrap_carryover()
{
    CTsbpdTime clock;
    Ledger     L = ledger_make(10000000, 120000);
    clock.setTsbPdMode(tp_us(L.base_us), false, microseconds_from(L.delay_us));

    const uint32_t ts_low  = 5000000;                        // wrapped after carryover
    const uint32_t ts_edge = uint32_t(2 * WRAP_PERIOD_US);   // still carried
    const uint32_t ts_past = ts_edge + 1;                    // never carried
    const uint32_t ts_trig = MAX_TIMESTAMP - 20000000;       // triggers the check period

    // Phase 1: before the wrap check period.
    TASK_PROPAGATE(check_clock(clock, L, ts_low));
    TASK_PROPAGATE(check_clock(clock, L, ts_trig));
    // A data packet exactly at the trigger boundary does not start the period.
    clock.updateBaseTime(MAX_TIMESTAMP - uint32_t(WRAP_PERIOD_US));
    ledger_update_base(L, MAX_TIMESTAMP - uint32_t(WRAP_PERIOD_US));
    TASK_CHECK(!L.wrap_check);
    TASK_PROPAGATE(check_internal(clock, L));
    // One microsecond later the wrap check period begins.
    clock.updateBaseTime(ts_trig);
    ledger_update_base(L, ts_trig);
    TASK_CHECK(L.wrap_check);
    TASK_CHECK(L.base_us == 10000000); // base not shifted yet
    TASK_PROPAGATE(check_internal(clock, L));

    // Phase 2: inside the wrap check period.  Queries of small timestamps see
    // the carryover, large ones do not; the boundary is exactly 2*WRAP_PERIOD.
    TASK_PROPAGATE(check_clock(clock, L, ts_low));
    TASK_PROPAGATE(check_clock(clock, L, ts_edge));
    TASK_PROPAGATE(check_clock(clock, L, ts_past));
    TASK_PROPAGATE(check_clock(clock, L, ts_trig));
    // The carryover is exactly 2^32 microseconds, not one less.
    TASK_CHECK(us_of(clock.getPktBaseTime(ts_low)) != L.base_us + WRAP_SPAN_US - 1 + ts_low);

    // A control (ACKACK) sample queries the carried base time but must not
    // commit the carryover into the base time.
    TASK_PROPAGATE(feed_drift_sample(clock, L, 7000000u, -1, 1200));
    TASK_CHECK(L.wrap_check);
    TASK_CHECK(L.base_us == 10000000);
    TASK_PROPAGATE(check_internal(clock, L));

    // The remaining 999 drift samples of this window, still inside the check
    // period: the drift process continues across the wrap.
    bool committed = false;
    TASK_PROPAGATE(feed_window(clock, L, 7000000u, -1, 1200, DRIFT_WINDOW - 1, &committed));
    TASK_CHECK(committed);
    TASK_CHECK(L.drift_us == 1200);
    TASK_CHECK(L.base_us == 10000000);
    TASK_PROPAGATE(check_clock(clock, L, ts_low));

    // A data packet just below the wrap period does not end the check period.
    clock.updateBaseTime(uint32_t(WRAP_PERIOD_US) - 1);
    ledger_update_base(L, uint32_t(WRAP_PERIOD_US) - 1);
    TASK_CHECK(L.wrap_check);
    TASK_PROPAGATE(check_internal(clock, L));

    // Phase 3: a data packet at the wrap period boundary ends the check
    // period and commits the carryover exactly once.
    clock.updateBaseTime(uint32_t(WRAP_PERIOD_US));
    ledger_update_base(L, uint32_t(WRAP_PERIOD_US));
    TASK_CHECK(!L.wrap_check);
    TASK_CHECK(L.base_us == 10000000 + WRAP_SPAN_US);
    TASK_PROPAGATE(check_internal(clock, L));
    TASK_PROPAGATE(check_clock(clock, L, ts_low));
    TASK_PROPAGATE(check_clock(clock, L, ts_past));
    TASK_PROPAGATE(check_clock(clock, L, ts_trig));
    // Degradation probes: a carryover of 2^32 - 1 or a doubled carryover
    // would be caught by the independent ledger time.
    TASK_CHECK(us_of(clock.getPktBaseTime(ts_low)) != 10000000 + WRAP_SPAN_US - 1 + ts_low);
    TASK_CHECK(us_of(clock.getPktBaseTime(ts_low)) != 10000000 + 2 * WRAP_SPAN_US + ts_low);
    return 0;
}

// Group migration: the internal time base is exported inside and after the
// wrap check period, applied to new receive paths, and a new drift is synced
// afterwards.  The second path keeps its own negotiated delay; neither the
// delay, the drift nor the wrap span may be applied twice.
int case_handoff_group_migration()
{
    CTsbpdTime path1;
    Ledger     L1 = ledger_make(10000000, 120000);
    path1.setTsbPdMode(tp_us(L1.base_us), false, microseconds_from(L1.delay_us));

    const uint32_t ts      = 500000;
    const uint32_t ts_low  = 5000000;
    const uint32_t ts_trig = MAX_TIMESTAMP - 20000000;
    bool committed = false;

    // Establish a committed drift on the first path.
    TASK_PROPAGATE(feed_window(path1, L1, ts, -1, 2300, DRIFT_WINDOW, &committed));
    TASK_CHECK(committed);
    TASK_CHECK(L1.drift_us == 2300);

    // Enter the wrap check period and export the internal time base.
    path1.updateBaseTime(ts_trig);
    ledger_update_base(L1, ts_trig);
    TASK_CHECK(L1.wrap_check);

    time_point             tb;
    bool                   wrp = false;
    steady_clock::duration ud;
    path1.getInternalTimeBase(tb, wrp, ud);
    TASK_CHECK(us_of(tb) == L1.base_us);
    TASK_CHECK(wrp);
    TASK_CHECK(count_microseconds(ud) == 2300);

    // Migrate to a second path with its own negotiated delay.
    CTsbpdTime path2;
    Ledger     L2 = ledger_make(L1.base_us, 150000);
    L2.drift_us   = L1.drift_us;
    L2.wrap_check = L1.wrap_check;
    path2.applyGroupTime(tb, wrp, 150000, ud);
    TASK_PROPAGATE(check_clock(path2, L2, ts_low));  // carried
    TASK_PROPAGATE(check_clock(path2, L2, ts_trig)); // not carried
    // Degradation probe: folding the delay into the base time would count it twice.
    TASK_CHECK(us_of(path2.getPktTime(ts_low)) != ledger_pkt_time(L2, ts_low) + L2.delay_us);

    // Exit the wrap check period on the first path and export again.
    path1.updateBaseTime(40000000u);
    ledger_update_base(L1, 40000000u);
    TASK_CHECK(!L1.wrap_check);
    TASK_CHECK(L1.base_us == 10000000 + WRAP_SPAN_US);
    path1.getInternalTimeBase(tb, wrp, ud);
    TASK_CHECK(us_of(tb) == L1.base_us);
    TASK_CHECK(!wrp);

    // Migrate the post-wrap state to a third path.
    CTsbpdTime path3;
    Ledger     L3 = ledger_make(L1.base_us, 150000);
    L3.drift_us = L1.drift_us;
    path3.applyGroupTime(tb, wrp, 150000, ud);
    TASK_PROPAGATE(check_clock(path3, L3, ts_low));
    // The handshake base time must not be reused as the internal base time.
    TASK_CHECK(us_of(path3.getPktBaseTime(ts_low)) != 10000000 + ts_low);

    // A new drift window commits on the first path and is synced to the
    // second path, which keeps its own delay by definition of the interface.
    committed = false;
    TASK_PROPAGATE(feed_window(path1, L1, ts, -1, 1400, DRIFT_WINDOW, &committed));
    TASK_CHECK(committed);
    TASK_CHECK(L1.drift_us == 1400);
    path1.getInternalTimeBase(tb, wrp, ud);
    TASK_CHECK(us_of(tb) == L1.base_us);
    TASK_CHECK(count_microseconds(ud) == 1400);

    path2.applyGroupDrift(tb, wrp, ud);
    L2.base_us    = L1.base_us;
    L2.wrap_check = L1.wrap_check;
    L2.drift_us   = L1.drift_us;
    TASK_PROPAGATE(check_clock(path2, L2, ts_low));
    TASK_PROPAGATE(check_clock(path2, L2, ts_trig));
    // Degradation probes: delay, drift and wrap span must not be applied twice.
    TASK_CHECK(us_of(path2.getPktTime(ts_low)) != ledger_pkt_time(L2, ts_low) + L2.delay_us);
    TASK_CHECK(us_of(path2.getPktTime(ts_low)) != ledger_pkt_time(L2, ts_low) + L2.drift_us);
    TASK_CHECK(us_of(path2.getPktBaseTime(ts_low)) != ledger_pkt_base(L2, ts_low) + WRAP_SPAN_US);
    return 0;
}

// ---------------------------------------------------------------------------
// Group "delivery".
// ---------------------------------------------------------------------------

// Real CUnitQueue/CPacket/CRcvBuffer scenario: SOLO media messages with a
// sequence gap that is filled later; readiness is checked one microsecond
// before and exactly at the delivery deadline.
int case_delivery_solo_gap_fill()
{
    CUnitQueue queue(16, 1500);
    CRcvBuffer rcv(100, 16, &queue, true);
    Ledger     L = ledger_make(10000000, 120000);
    rcv.setTsbPdMode(tp_us(L.base_us), false, microseconds_from(L.delay_us));

    static const char payload1[] = "TSBPDREF-ALPHA-payload-1";
    static const char payload2[] = "TSBPDREF-BRAVO-payload-2";
    static const char payload3[] = "TSBPDREF-GAMMA-payload-3";

    // seq 100 and 102 arrive; 101 is a sequence gap.
    TASK_PROPAGATE(insert_solo(queue, rcv, 100, 2000, 1, payload1, sizeof(payload1)));
    TASK_PROPAGATE(insert_solo(queue, rcv, 102, 4500, 3, payload3, sizeof(payload3)));

    const int64_t due1 = ledger_pkt_time(L, 2000);
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due1 - 1)));
    TASK_CHECK(rcv.isRcvDataReady(tp_us(due1)));
    CRcvBuffer::PacketInfo info = rcv.getFirstValidPacketInfo();
    TASK_CHECK(info.seqno == 100);
    TASK_CHECK(!info.seq_gap);
    TASK_CHECK(us_of(info.tsbpd_time) == due1);
    TASK_PROPAGATE(read_and_check(rcv, payload1, sizeof(payload1), due1, 100, 1));

    // Head of the queue is now seq 102 behind the gap: not ready even when
    // its delivery time has passed.
    const int64_t due3 = ledger_pkt_time(L, 4500);
    info = rcv.getFirstValidPacketInfo();
    TASK_CHECK(info.seqno == 102);
    TASK_CHECK(info.seq_gap);
    TASK_CHECK(us_of(info.tsbpd_time) == due3);
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due3)));
    TASK_CHECK(rcv.getFirstNonreadSeqNo() == 101);

    // Fill the gap; both messages become readable in order.
    TASK_PROPAGATE(insert_solo(queue, rcv, 101, 3200, 2, payload2, sizeof(payload2)));
    const int64_t due2 = ledger_pkt_time(L, 3200);
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due2 - 1)));
    TASK_CHECK(rcv.isRcvDataReady(tp_us(due2)));
    info = rcv.getFirstValidPacketInfo();
    TASK_CHECK(info.seqno == 101);
    TASK_CHECK(!info.seq_gap);
    TASK_CHECK(us_of(info.tsbpd_time) == due2);
    TASK_PROPAGATE(read_and_check(rcv, payload2, sizeof(payload2), due2, 101, 2));

    TASK_CHECK(rcv.isRcvDataReady(tp_us(due3)));
    TASK_PROPAGATE(read_and_check(rcv, payload3, sizeof(payload3), due3, 102, 3));

    // Queue head after consumption: empty.
    info = rcv.getFirstValidPacketInfo();
    TASK_CHECK(info.seqno == -1);
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due3)));
    TASK_CHECK(rcv.getStartSeqNo() == 103);
    return 0;
}

// Same real receive path, but the sequence gap is explicitly skipped with
// dropMessage instead of being filled.
int case_delivery_drop_gap_skip()
{
    CUnitQueue queue(16, 1500);
    CRcvBuffer rcv(200, 16, &queue, true);
    Ledger     L = ledger_make(10000000, 120000);
    rcv.setTsbPdMode(tp_us(L.base_us), false, microseconds_from(L.delay_us));

    static const char payload1[] = "TSBPDREF-ECHO-one-1";
    static const char payload3[] = "TSBPDREF-ECHO-three-3";

    TASK_PROPAGATE(insert_solo(queue, rcv, 200, 1000, 1, payload1, sizeof(payload1)));
    TASK_PROPAGATE(insert_solo(queue, rcv, 202, 3000, 3, payload3, sizeof(payload3)));

    const int64_t due1 = ledger_pkt_time(L, 1000);
    const int64_t due3 = ledger_pkt_time(L, 3000);
    TASK_PROPAGATE(read_and_check(rcv, payload1, sizeof(payload1), due1, 200, 1));

    // The message behind the missing seq 201 is not readable.
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due3)));
    TASK_CHECK(rcv.getFirstNonreadSeqNo() == 201);

    // Explicitly skip the gap.
    TASK_CHECK(rcv.dropMessage(201, 201, 0, CRcvBuffer::DROP_EXISTING) == 1);
    TASK_CHECK(rcv.getStartSeqNo() == 202);
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due3 - 1)));
    TASK_CHECK(rcv.isRcvDataReady(tp_us(due3)));
    CRcvBuffer::PacketInfo info = rcv.getFirstValidPacketInfo();
    TASK_CHECK(info.seqno == 202);
    TASK_CHECK(!info.seq_gap);
    TASK_CHECK(us_of(info.tsbpd_time) == due3);
    TASK_PROPAGATE(read_and_check(rcv, payload3, sizeof(payload3), due3, 202, 3));

    info = rcv.getFirstValidPacketInfo();
    TASK_CHECK(info.seqno == -1);
    TASK_CHECK(rcv.getStartSeqNo() == 203);
    return 0;
}

// A packet queued before a clock change must be delivered according to the
// currently effective clock: a committed drift moves its delivery time.
int case_delivery_queued_clock_change()
{
    CUnitQueue queue(16, 1500);
    CRcvBuffer rcv(300, 16, &queue, true);
    Ledger     L = ledger_make(10000000, 120000);
    rcv.setTsbPdMode(tp_us(L.base_us), false, microseconds_from(L.delay_us));

    static const char payload[] = "TSBPDREF-FOXTROT-clock-7";
    TASK_PROPAGATE(insert_solo(queue, rcv, 300, 5000, 7, payload, sizeof(payload)));

    const int64_t due_old = ledger_pkt_time(L, 5000);
    TASK_CHECK(rcv.isRcvDataReady(tp_us(due_old)));

    // 999 drift samples of +4000 us: not committed, the queued packet keeps
    // its original delivery time.
    const uint32_t ts_ctl = 9000;
    for (int i = 0; i < DRIFT_WINDOW - 1; ++i)
    {
        const int64_t arrival = ledger_pkt_base(L, ts_ctl) + 4000;
        const bool prod = rcv.addRcvTsbPdDriftSample(ts_ctl, tp_us(arrival), -1);
        const bool led  = ledger_add_drift(L, ts_ctl, arrival, -1);
        TASK_CHECK(prod == led);
        TASK_CHECK(!prod);
    }
    TASK_CHECK(rcv.isRcvDataReady(tp_us(due_old)));

    // The 1000th sample commits drift +4000: the queued packet's delivery
    // time moves accordingly.
    const int64_t arrival = ledger_pkt_base(L, ts_ctl) + 4000;
    TASK_CHECK(rcv.addRcvTsbPdDriftSample(ts_ctl, tp_us(arrival), -1));
    TASK_CHECK(ledger_add_drift(L, ts_ctl, arrival, -1));
    TASK_CHECK(L.drift_us == 4000);

    const int64_t due_new = ledger_pkt_time(L, 5000);
    TASK_CHECK(due_new == due_old + 4000);
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due_old)));
    TASK_CHECK(!rcv.isRcvDataReady(tp_us(due_new - 1)));
    TASK_CHECK(rcv.isRcvDataReady(tp_us(due_new)));
    CRcvBuffer::PacketInfo info = rcv.getFirstValidPacketInfo();
    TASK_CHECK(info.seqno == 300);
    TASK_CHECK(us_of(info.tsbpd_time) == due_new);
    TASK_PROPAGATE(read_and_check(rcv, payload, sizeof(payload), due_new, 300, 7));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    { "drift",    "window_commit",         case_drift_window_commit         },
    { "drift",    "rtt_delta_halving",     case_drift_rtt_delta_halving     },
    { "drift",    "overdrift_split",       case_drift_overdrift_split       },
    { "handoff",  "wrap_carryover",        case_handoff_wrap_carryover      },
    { "handoff",  "group_migration",       case_handoff_group_migration     },
    { "delivery", "solo_gap_fill",         case_delivery_solo_gap_fill      },
    { "delivery", "drop_gap_skip",         case_delivery_drop_gap_skip      },
    { "delivery", "queued_clock_change",   case_delivery_queued_clock_change},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
