/*
* Tests for layer dependent scheduling in ScreamV2Tx.
* Synthetic in-memory packets and manual NTP time only,
*  no sockets, no sleeping.
*/
#include "ScreamTx.h"
#include "RtpQueue.h"
#include "task_support/check.h"
#include <cstdlib>
#include <cstring>

extern unsigned int task_packet_freed;

static const uint32_t SSRC_B = 100; // base stream
static const uint32_t SSRC_E = 200; // enhancement (child) stream
static const uint32_t SSRC_X = 300; // extra stream
static const uint32_t SSRC_Y = 400; // stream with a plain RTP queue

static const uint32_t T0 = 1024;          // init time, NTP Q16
static const uint32_t ANCHOR_RTP = 9000;  // anchor RTP timestamp
static const uint32_t ANCHOR_PLAYOUT = 20000; // anchor playout time, NTP Q16
static const uint32_t CLOCK = 90000;      // media clock rate
static const uint32_t TS1 = 9000;         // deadline = 20000
static const uint32_t TS2 = 12000;        // deadline = 20000 + 2184 = 22184
static const uint32_t DEADLINE1 = 20000;
static const uint32_t DEADLINE2 = 22184;

/*
* Minimal RtpQueueIface implementation without the layer capability,
*  used to verify that such queues are rejected by setLayerDependency
*  but still compile and work as before.
*/
class PlainQueue : public RtpQueueIface {
public:
    int clear() { return 0; }
    int sizeOfNextRtp() { return -1; }
    int seqNrOfNextRtp() { return -1; }
    int seqNrOfLastRtp() { return -1; }
    int bytesInQueue() { return 0; }
    int sizeOfQueue() { return 0; }
    float getDelay(float) { return 0.0f; }
    int getSizeOfLastFrame() { return 0; }
};

static ScreamV2Tx* makeTx(int cwnd = 1000000, float maxHeadroom = 3.0f) {
    ScreamV2Tx* tx = new ScreamV2Tx(0.7f, 0.9f, 0.1f, cwnd,
        1.5f, 1.5f, 1.2f, 0.05f, false, maxHeadroom, false, false);
    tx->enablePacketPacing(false);
    return tx;
}

static void reg(ScreamV2Tx* tx, RtpQueueIface* q, uint32_t ssrc, float prio) {
    tx->registerNewStream(q, ssrc, prio, 10000.0f, 100000.0f, 1000000.0f, 10.0f);
}

static void pushPkt(RtpQueue& q, ScreamV2Tx& tx, uint32_t now, uint32_t ssrc,
    uint16_t seq, int size, bool mark, uint32_t rtpTs) {
    void* p = std::malloc(size);
    q.push(p, size, ssrc, seq, mark, float(now) / 65536.0f, rtpTs);
    tx.newMediaFrame(now, ssrc, size, mark);
}

static void pushFrame(RtpQueue& q, ScreamV2Tx& tx, uint32_t now, uint32_t ssrc,
    uint16_t seq0, int nPkts, int size, uint32_t rtpTs) {
    for (int i = 0; i < nPkts; i++)
        pushPkt(q, tx, now, ssrc, seq0 + i, size, i == nPkts - 1, rtpTs);
}

/*
* Query the scheduler and transmit the head packet of q,
*  expects that ssrc expectSsrc is selected and can be sent immediately
*/
static bool sendFrom(ScreamV2Tx& tx, RtpQueue& q, uint32_t now, uint32_t expectSsrc,
    uint16_t* seqOut = 0, uint32_t* tsOut = 0) {
    uint32_t ssrc = 0;
    if (tx.isOkToTransmit(now, ssrc) != 0.0f)
        return false;
    if (ssrc != expectSsrc)
        return false;
    void* p = 0;
    int size = 0;
    uint32_t ssrc2 = 0, ts = 0;
    uint16_t seq = 0;
    bool mark = false;
    if (!q.pop(&p, size, ssrc2, seq, mark, ts))
        return false;
    tx.addTransmitted(now, ssrc2, size, seq, mark, 0.0f, ts);
    if (p)
        std::free(p);
    if (seqOut)
        *seqOut = seq;
    if (tsOut)
        *tsOut = ts;
    return true;
}

static bool noTx(ScreamV2Tx& tx, uint32_t now) {
    uint32_t ssrc = 0;
    return tx.isOkToTransmit(now, ssrc) == -1.0f;
}

/*
* Positive ACK through the per-packet standardized feedback entry
*/
static void ack(ScreamV2Tx& tx, uint32_t now, int streamId, uint16_t seq) {
    tx.incomingStandardizedFeedback(now, streamId, now, seq, 0, true);
}

// ================= dependency group =================

static int dep_config_validation() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE, qX;
    PlainQueue qP;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    reg(tx, &qX, SSRC_X, 0.5f);
    reg(tx, &qP, SSRC_Y, 0.5f);

    // identical SSRCs not allowed
    TASK_CHECK(!tx->setLayerDependency(SSRC_B, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    // clockRate must be positive
    TASK_CHECK(!tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, 0));
    // unknown child / base
    TASK_CHECK(!tx->setLayerDependency(999, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    TASK_CHECK(!tx->setLayerDependency(SSRC_E, 999, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    // child queue does not implement RtpQueueLayerIface
    TASK_CHECK(!tx->setLayerDependency(SSRC_Y, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    // valid configuration
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    // each child only once
    TASK_CHECK(!tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    // a base that others depend on cannot become a child
    TASK_CHECK(!tx->setLayerDependency(SSRC_B, SSRC_X, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    // a child cannot become a base (no chains)
    TASK_CHECK(!tx->setLayerDependency(SSRC_X, SSRC_E, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    // one base may have several children, failed attempts above
    //  left the state unchanged
    TASK_CHECK(tx->setLayerDependency(SSRC_X, SSRC_B, ANCHOR_RTP, 30000, CLOCK));
    // a child cannot depend on two bases either
    TASK_CHECK(!tx->setLayerDependency(SSRC_X, SSRC_E, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    uint32_t ssrc = 0;
    TASK_CHECK(tx->isOkToTransmit(T0, ssrc) == -1.0f);
    delete tx;
    return 0;
}

static int dep_no_config_scheduling_unchanged() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qA, qB;
    reg(tx, &qA, SSRC_B, 1.0f);
    reg(tx, &qB, SSRC_E, 0.5f);
    // no setLayerDependency at all, legacy behavior
    pushFrame(qB, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    pushFrame(qA, *tx, T0, SSRC_B, 1, 1, 100, TS1);
    // highest priority stream is selected first
    TASK_CHECK(sendFrom(*tx, qA, T0, SSRC_B));
    TASK_CHECK(sendFrom(*tx, qB, T0, SSRC_E));
    TASK_CHECK(noTx(*tx, T0));
    delete tx;
    return 0;
}

static int dep_enhancement_first_gating() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    // enhancement frame arrives before the base frame is even queued
    pushFrame(qE, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    TASK_CHECK(noTx(*tx, T0));

    // base frame, two packets, marker on the last one
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 1, 2, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));
    // transmitted but not yet ACKed, enhancement stays blocked
    TASK_CHECK(noTx(*tx, T0 + 100));

    ack(*tx, T0 + 200, 0, 1);
    // marker packet not ACKed yet
    TASK_CHECK(noTx(*tx, T0 + 200));
    ack(*tx, T0 + 300, 0, 2);
    // fully confirmed, enhancement may go
    TASK_CHECK(sendFrom(*tx, qE, T0 + 300, SSRC_E));
    TASK_CHECK(noTx(*tx, T0 + 300));
    delete tx;
    return 0;
}

static int dep_multi_child_one_base() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE, qX;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    reg(tx, &qX, SSRC_X, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));
    TASK_CHECK(tx->setLayerDependency(SSRC_X, SSRC_B, ANCHOR_RTP, 30000, CLOCK));

    pushFrame(qE, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    pushFrame(qX, *tx, T0, SSRC_X, 1, 1, 100, TS1);
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 1, 1, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));
    // both children blocked while the base frame is unconfirmed
    TASK_CHECK(noTx(*tx, T0 + 100));
    ack(*tx, T0 + 200, 0, 1);
    // one confirmed base frame releases both children
    TASK_CHECK(sendFrom(*tx, qE, T0 + 200, SSRC_E));
    TASK_CHECK(sendFrom(*tx, qX, T0 + 200, SSRC_X));
    TASK_CHECK(noTx(*tx, T0 + 200));
    delete tx;
    return 0;
}

// ================= feedback group =================

static int fb_marker_and_prefix() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    pushFrame(qE, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    pushFrame(qE, *tx, T0, SSRC_E, 2, 1, 100, TS2);
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 50, 3, 100, TS1);
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 60, 3, 100, TS2);
    for (int i = 0; i < 6; i++)
        TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));

    // only the marker ACKed, prefix packets missing
    ack(*tx, T0 + 200, 0, 52);
    TASK_CHECK(noTx(*tx, T0 + 200));
    // out of order positive ACKs merge
    ack(*tx, T0 + 300, 0, 50);
    TASK_CHECK(noTx(*tx, T0 + 300));
    ack(*tx, T0 + 400, 0, 51);
    TASK_CHECK(sendFrom(*tx, qE, T0 + 400, SSRC_E));

    // whole prefix ACKed but the marker indication is missing
    ack(*tx, T0 + 500, 0, 60);
    ack(*tx, T0 + 500, 0, 61);
    TASK_CHECK(noTx(*tx, T0 + 500));
    ack(*tx, T0 + 600, 0, 62);
    TASK_CHECK(sendFrom(*tx, qE, T0 + 600, SSRC_E));
    TASK_CHECK(noTx(*tx, T0 + 600));
    delete tx;
    return 0;
}

static int fb_higher_ack_not_enough() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    pushFrame(qE, *tx, T0, SSRC_E, 5, 1, 100, TS1);
    pushFrame(qE, *tx, T0, SSRC_E, 6, 1, 100, TS2);
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 20, 2, 100, TS1);
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 22, 1, 100, TS2);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));

    // a larger ACK advances the send window over seq 20,21 but
    //  does not positively acknowledge them
    ack(*tx, T0 + 200, 0, 22);
    TASK_CHECK(noTx(*tx, T0 + 200));
    ack(*tx, T0 + 300, 0, 20);
    TASK_CHECK(noTx(*tx, T0 + 300));
    ack(*tx, T0 + 400, 0, 21);
    // first frame confirmed, second was confirmed by the ACK of seq 22
    uint16_t seq = 0;
    TASK_CHECK(sendFrom(*tx, qE, T0 + 400, SSRC_E, &seq));
    TASK_CHECK(seq == 5);
    TASK_CHECK(sendFrom(*tx, qE, T0 + 400, SSRC_E, &seq));
    TASK_CHECK(seq == 6);
    TASK_CHECK(noTx(*tx, T0 + 400));
    delete tx;
    return 0;
}

static int fb_duplicate_and_other_ssrc() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE, qD;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    reg(tx, &qD, SSRC_X, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    pushFrame(qE, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    // base and an independent stream share RTP timestamp and sequence numbers
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 30, 2, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));
    TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));
    pushFrame(qD, *tx, T0 + 100, SSRC_X, 30, 2, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qD, T0 + 100, SSRC_X));
    TASK_CHECK(sendFrom(*tx, qD, T0 + 100, SSRC_X));

    // feedback for the other SSRC with identical seq/timestamp does not count
    ack(*tx, T0 + 200, 2, 30);
    ack(*tx, T0 + 200, 2, 31);
    TASK_CHECK(noTx(*tx, T0 + 200));
    ack(*tx, T0 + 300, 0, 30);
    TASK_CHECK(noTx(*tx, T0 + 300));
    // duplicate feedback does not count twice
    ack(*tx, T0 + 300, 0, 30);
    TASK_CHECK(noTx(*tx, T0 + 300));
    ack(*tx, T0 + 400, 0, 31);
    // duplicate of an already completing ACK is harmless
    ack(*tx, T0 + 400, 0, 31);
    TASK_CHECK(sendFrom(*tx, qE, T0 + 400, SSRC_E));
    TASK_CHECK(noTx(*tx, T0 + 400));
    delete tx;
    return 0;
}

static int fb_seq_wraparound() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    pushFrame(qE, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    // base frame straddles the 16bit sequence number wrap
    pushFrame(qB, *tx, T0 + 100, SSRC_B, 65534, 4, 100, TS1);
    for (int i = 0; i < 4; i++)
        TASK_CHECK(sendFrom(*tx, qB, T0 + 100, SSRC_B));

    ack(*tx, T0 + 200, 0, 65535);
    TASK_CHECK(noTx(*tx, T0 + 200));
    ack(*tx, T0 + 300, 0, 65534);
    TASK_CHECK(noTx(*tx, T0 + 300));
    ack(*tx, T0 + 400, 0, 0);
    TASK_CHECK(noTx(*tx, T0 + 400));
    // marker is on seq 1, after the wrap
    ack(*tx, T0 + 500, 0, 1);
    TASK_CHECK(sendFrom(*tx, qE, T0 + 500, SSRC_E));
    TASK_CHECK(noTx(*tx, T0 + 500));
    delete tx;
    return 0;
}

static int fb_tx_ring_slot_reuse() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    pushFrame(qE, *tx, T0, SSRC_E, 7, 1, 100, TS1);
    // base frame, single packet with seq 100, never ACKed
    pushFrame(qB, *tx, T0 + 26, SSRC_B, 100, 1, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 26, SSRC_B));
    // transmit enough later packets that seq 4196 reuses the tx ring
    //  slot of seq 100 (kMaxTxPackets = 4096)
    for (int i = 0; i < 4096; i++) {
        pushPkt(qB, *tx, T0 + 76 + i, SSRC_B, 101 + i, 50, i == 4095, TS2);
        TASK_CHECK(sendFrom(*tx, qB, T0 + 76 + i, SSRC_B));
    }
    // an ACK for the overwritten seq 100 must not count as received
    ack(*tx, T0 + 4276, 0, 100);
    TASK_CHECK(noTx(*tx, T0 + 4276));
    // the blocked enhancement frame is released at its playout deadline
    unsigned int freedBefore = task_packet_freed;
    TASK_CHECK(noTx(*tx, DEADLINE1 + 100));
    TASK_CHECK(task_packet_freed == freedBefore + 1);
    TASK_CHECK(qE.sizeOfQueue() == 0);
    delete tx;
    return 0;
}

static int fb_rfc8888_bytes_entry() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    pushFrame(qE, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    pushFrame(qB, *tx, T0 + 76, SSRC_B, 40, 2, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 76, SSRC_B));
    TASK_CHECK(sendFrom(*tx, qB, T0 + 76, SSRC_B));
    TASK_CHECK(noTx(*tx, T0 + 76));

    /*
    * Hand crafted RFC8888 CCFB report, 24 bytes total :
    *  header(2) + length(2) + RTCP sender SSRC(4) +
    *  {SSRC=100, begin_seq=40, num_reports=2, two metric entries} +
    *  report timestamp(4)
    */
    unsigned char buf[24];
    std::memset(buf, 0, sizeof(buf));
    buf[3] = 5;            // length in 32bit words minus one
    buf[7] = 1;            // RTCP sender SSRC
    buf[11] = 100;         // RTP source SSRC
    buf[13] = 40;          // begin_seq
    buf[15] = 2;           // num_reports
    buf[16] = 0x80;        // seq 40 received, no CE
    buf[18] = 0x80;        // seq 41 received, no CE
    uint32_t rts = T0 + 176;
    buf[20] = (rts >> 24) & 0xFF;
    buf[21] = (rts >> 16) & 0xFF;
    buf[22] = (rts >> 8) & 0xFF;
    buf[23] = rts & 0xFF;
    tx->incomingStandardizedFeedback(T0 + 176, buf, sizeof(buf));

    TASK_CHECK(sendFrom(*tx, qE, T0 + 176, SSRC_E));
    TASK_CHECK(noTx(*tx, T0 + 176));
    delete tx;
    return 0;
}

// ================= playout group =================

static int playout_exact_deadline_and_fresh_frame() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    // first base frame transmitted but never confirmed
    pushFrame(qB, *tx, T0 + 76, SSRC_B, 1, 1, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 76, SSRC_B));
    pushFrame(qE, *tx, T0 + 100, SSRC_E, 1, 1, 100, TS1);
    // second base frame transmitted and confirmed
    pushFrame(qB, *tx, T0 + 176, SSRC_B, 2, 1, 100, TS2);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 176, SSRC_B));
    pushFrame(qE, *tx, T0 + 200, SSRC_E, 2, 1, 100, TS2);
    ack(*tx, T0 + 276, 0, 2);

    // blocked head frame cannot be skipped for the fresh frame behind it
    TASK_CHECK(noTx(*tx, T0 + 400));
    TASK_CHECK(noTx(*tx, DEADLINE1 - 1));
    // exactly at the deadline the stale frame is released and the
    //  confirmed fresh frame behind it becomes sendable
    unsigned int freedBefore = task_packet_freed;
    uint16_t seq = 0;
    uint32_t ts = 0;
    TASK_CHECK(sendFrom(*tx, qE, DEADLINE1, SSRC_E, &seq, &ts));
    TASK_CHECK(seq == 2);
    TASK_CHECK(ts == TS2);
    TASK_CHECK(task_packet_freed == freedBefore + 1);
    TASK_CHECK(qE.sizeOfQueue() == 0);
    TASK_CHECK(noTx(*tx, DEADLINE1));
    delete tx;
    return 0;
}

static int playout_dual_child_deadlines() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE1, qE2;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE1, SSRC_E, 1.0f);
    reg(tx, &qE2, SSRC_X, 1.0f);
    // two children of the same base with different playout anchors
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, 20000, CLOCK));
    TASK_CHECK(tx->setLayerDependency(SSRC_X, SSRC_B, ANCHOR_RTP, 30000, CLOCK));

    pushFrame(qB, *tx, T0 + 76, SSRC_B, 1, 1, 100, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 76, SSRC_B));
    pushFrame(qE1, *tx, T0 + 100, SSRC_E, 1, 1, 100, TS1);
    pushFrame(qE2, *tx, T0 + 100, SSRC_X, 1, 1, 100, TS1);
    // base frame never confirmed, both children blocked
    TASK_CHECK(noTx(*tx, T0 + 400));

    unsigned int freedBefore = task_packet_freed;
    // first child's deadline passes
    TASK_CHECK(noTx(*tx, 20000));
    TASK_CHECK(qE1.sizeOfQueue() == 0);
    TASK_CHECK(task_packet_freed == freedBefore + 1);
    TASK_CHECK(qE2.sizeOfQueue() == 1);
    // second child still held until its own deadline
    TASK_CHECK(noTx(*tx, 29999));
    TASK_CHECK(qE2.sizeOfQueue() == 1);
    TASK_CHECK(noTx(*tx, 30000));
    TASK_CHECK(qE2.sizeOfQueue() == 0);
    TASK_CHECK(task_packet_freed == freedBefore + 2);

    // the base frame proof is gone now, a late ACK must not
    //  re-enable anything
    ack(*tx, 30100, 0, 1);
    TASK_CHECK(noTx(*tx, 30100));
    TASK_CHECK(qE1.sizeOfQueue() == 0);
    TASK_CHECK(qE2.sizeOfQueue() == 0);
    delete tx;
    return 0;
}

static int playout_blocked_credit_vs_independent() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE, qI;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f); // blocked child, high priority
    reg(tx, &qI, SSRC_X, 0.5f); // independent stream, low priority
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    // child frame whose base frame is never even sent
    pushFrame(qE, *tx, T0, SSRC_E, 1, 1, 100, TS1);
    pushFrame(qI, *tx, T0 + 76, SSRC_X, 1, 1, 100, TS1);
    pushFrame(qI, *tx, T0 + 76, SSRC_X, 2, 1, 100, TS2);
    // the blocked high priority stream must not starve the independent one
    TASK_CHECK(sendFrom(*tx, qI, T0 + 76, SSRC_X));
    // the child has now accumulated credit but is still blocked
    TASK_CHECK(sendFrom(*tx, qI, T0 + 76, SSRC_X));
    // everything eligible sent, only the blocked child remains
    TASK_CHECK(noTx(*tx, T0 + 76));
    delete tx;
    return 0;
}

static int playout_expiry_queue_stats_and_ownership() {
    ScreamV2Tx* tx = makeTx();
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    // two enhancement frames, base frames never sent
    pushFrame(qE, *tx, T0, SSRC_E, 1, 3, 100, TS1);
    pushFrame(qE, *tx, T0 + 76, SSRC_E, 4, 2, 100, TS2);
    TASK_CHECK(qE.sizeOfQueue() == 5);
    TASK_CHECK(qE.bytesInQueue() == 500);

    unsigned int freedBefore = task_packet_freed;
    // first frame expires exactly at its deadline
    TASK_CHECK(noTx(*tx, DEADLINE1));
    TASK_CHECK(task_packet_freed == freedBefore + 3);
    // queue state immediately reflects the remaining content
    TASK_CHECK(qE.sizeOfQueue() == 2);
    TASK_CHECK(qE.bytesInQueue() == 200);
    TASK_CHECK(qE.seqNrOfNextRtp() == 4);
    TASK_CHECK(qE.seqNrOfLastRtp() == 5);
    TASK_CHECK(qE.sizeOfNextRtp() == 100);
    float curr = float(DEADLINE1) / 65536.0f;
    TASK_CHECK(qE.getDelay(curr) == curr - float(T0 + 76) / 65536.0f);

    // second frame one tick before and at its own deadline
    TASK_CHECK(noTx(*tx, DEADLINE2 - 1));
    TASK_CHECK(qE.sizeOfQueue() == 2);
    TASK_CHECK(noTx(*tx, DEADLINE2));
    TASK_CHECK(task_packet_freed == freedBefore + 5);
    TASK_CHECK(qE.sizeOfQueue() == 0);
    TASK_CHECK(qE.bytesInQueue() == 0);
    TASK_CHECK(qE.sizeOfNextRtp() == -1);
    TASK_CHECK(qE.seqNrOfNextRtp() == -1);
    delete tx;
    return 0;
}

static int playout_sent_ledger_unaffected() {
    // small congestion window and minimal headroom so that the
    //  bytes-in-flight accounting is observable through isOkToTransmit
    ScreamV2Tx* tx = makeTx(5000, 1.0f);
    tx->setCwndMin(5000);
    RtpQueue qB, qE;
    reg(tx, &qB, SSRC_B, 1.0f);
    reg(tx, &qE, SSRC_E, 1.0f);
    TASK_CHECK(tx->setLayerDependency(SSRC_E, SSRC_B, ANCHOR_RTP, ANCHOR_PLAYOUT, CLOCK));

    // confirm the base frame
    pushFrame(qB, *tx, T0 + 76, SSRC_B, 1, 1, 2500, TS1);
    TASK_CHECK(sendFrom(*tx, qB, T0 + 76, SSRC_B));
    ack(*tx, T0 + 176, 0, 1);

    // enhancement frame, 4 packets of 2500 bytes
    pushFrame(qE, *tx, T0 + 276, SSRC_E, 1, 4, 2500, TS1);
    TASK_CHECK(sendFrom(*tx, qE, T0 + 276, SSRC_E));
    TASK_CHECK(sendFrom(*tx, qE, T0 + 276, SSRC_E));
    TASK_CHECK(sendFrom(*tx, qE, T0 + 276, SSRC_E));
    // 7500 bytes in flight, the 4th packet does not fit the window
    TASK_CHECK(noTx(*tx, T0 + 300));

    // the unsent 4th packet expires at the playout deadline
    unsigned int freedBefore = task_packet_freed;
    TASK_CHECK(noTx(*tx, DEADLINE1));
    TASK_CHECK(task_packet_freed == freedBefore + 1);
    TASK_CHECK(qE.sizeOfQueue() == 0);

    // the dropped unsent packet must not have reduced bytesInFlight :
    //  a fresh independent packet still does not fit the window
    pushPkt(qB, *tx, DEADLINE1, SSRC_B, 2, 2500, true, TS2);
    TASK_CHECK(noTx(*tx, DEADLINE1));

    // the already sent packets continue through the normal feedback path
    ack(*tx, DEADLINE1 + 100, 1, 1);
    ack(*tx, DEADLINE1 + 100, 1, 2);
    ack(*tx, DEADLINE1 + 100, 1, 3);
    uint32_t ssrc = 0;
    TASK_CHECK(tx->isOkToTransmit(DEADLINE1 + 100, ssrc) == 0.0f);
    TASK_CHECK(ssrc == SSRC_B);
    delete tx;
    return 0;
}

const struct task_case task_cases[] = {
    {"dependency", "config_validation", dep_config_validation},
    {"dependency", "no_config_scheduling_unchanged", dep_no_config_scheduling_unchanged},
    {"dependency", "enhancement_first_gating", dep_enhancement_first_gating},
    {"dependency", "multi_child_one_base", dep_multi_child_one_base},
    {"feedback", "marker_and_prefix", fb_marker_and_prefix},
    {"feedback", "higher_ack_not_enough", fb_higher_ack_not_enough},
    {"feedback", "duplicate_and_other_ssrc", fb_duplicate_and_other_ssrc},
    {"feedback", "seq_wraparound", fb_seq_wraparound},
    {"feedback", "tx_ring_slot_reuse", fb_tx_ring_slot_reuse},
    {"feedback", "rfc8888_bytes_entry", fb_rfc8888_bytes_entry},
    {"playout", "exact_deadline_and_fresh_frame", playout_exact_deadline_and_fresh_frame},
    {"playout", "dual_child_deadlines", playout_dual_child_deadlines},
    {"playout", "blocked_credit_vs_independent", playout_blocked_credit_vs_independent},
    {"playout", "expiry_queue_stats_and_ownership", playout_expiry_queue_stats_and_ownership},
    {"playout", "sent_ledger_unaffected", playout_sent_ledger_unaffected},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
