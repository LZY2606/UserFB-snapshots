/*
 * Regression tests for srt_dropmsg(): application-requested revocation of a
 * message still held in the sender buffer.
 *
 * Four groups are registered:
 *   buffer   - CSndBuffer ownership/accounting with controlled sequence
 *              numbers, partial ACK and the 31-bit sequence wrap.
 *   wire     - real loopback SRT connection through a UDP forwarder; observes
 *              the DROPREQ on the wire and in-order delivery past the gap.
 *   recovery - first DROPREQ is swallowed; a NAK must draw a second DROPREQ
 *              instead of the original payload.
 *   api      - C API error handling, return values and accounting.
 *
 * Progress is driven by observable events (forwarder counters, receiver
 * messages). Timeouts exist only to terminate a hung run, never as the
 * condition under which an assertion passes.
 */

#include "srt.h"
#include "../task_support/check.h"

#include "buffer_snd.h"
#include "packet.h"
#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

const int    HDR_SIZE      = 16; // 4 x uint32_t SRT header
const int    PAYLOAD       = 1456;
const int    WAIT_LIMIT_MS = 5000;

using srt::CPacket;
using srt::CSndBuffer;
using srt::CSeqNo;

// --------------------------------------------------------------------------
// Small helpers
// --------------------------------------------------------------------------

struct SrtGuard
{
    SrtGuard()  { srt_startup(); }
    ~SrtGuard() { srt_cleanup(); }
};

bool setFlag(SRTSOCKET s, SRT_SOCKOPT opt, int value)
{
    return srt_setsockflag(s, opt, &value, sizeof(value)) == 0;
}

// File/message mode, no TSBPD/TLPKTDROP interference, periodic NAK enabled.
bool configure(SRTSOCKET s, int fc = 0, bool message_api = true)
{
    const SRT_TRANSTYPE type = SRTT_FILE;
    const bool messageapi    = message_api;
    const bool nakreport     = true;
    const bool tsbpd         = false;
    const bool tlpktdrop     = false;
    const int  timeout       = 2000;
    bool ok = setFlag(s, SRTO_TRANSTYPE, (int) type)
           && setFlag(s, SRTO_MESSAGEAPI, messageapi ? 1 : 0)
           && setFlag(s, SRTO_NAKREPORT, nakreport ? 1 : 0)
           && setFlag(s, SRTO_TSBPDMODE, tsbpd ? 1 : 0)
           && setFlag(s, SRTO_TLPKTDROP, tlpktdrop ? 1 : 0)
           && setFlag(s, SRTO_RCVTIMEO, timeout)
           && setFlag(s, SRTO_SNDTIMEO, timeout)
           && setFlag(s, SRTO_CONNTIMEO, timeout);
    if (fc > 0)
        ok = ok && setFlag(s, SRTO_FC, fc);
    return ok;
}

void fillPattern(std::vector<char>& v, int seed)
{
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = char((i * 13 + seed) & 0xFF);
}

// --------------------------------------------------------------------------
// buffer group: drive CSndBuffer directly with controlled sequence numbers
// --------------------------------------------------------------------------

SRT_MSGCTRL makeCtrl(int32_t first_seq, int32_t msgno)
{
    SRT_MSGCTRL mc = srt_msgctrl_default;
    mc.pktseq      = first_seq;
    mc.msgno       = msgno;
    mc.msgttl      = SRT_MSGTTL_INF;
    mc.inorder     = 1;
    return mc;
}

// A: 2 packets, B: 2 packets, C: 1 packet, starting at sequence @a start.
void enqueueABC(CSndBuffer& buf, int32_t start, int32_t& msgA, int32_t& msgB, int32_t& msgC)
{
    std::vector<char> payloadA(2 * PAYLOAD);
    std::vector<char> payloadB(2 * PAYLOAD);
    std::vector<char> payloadC(PAYLOAD);
    fillPattern(payloadA, 1);
    fillPattern(payloadB, 2);
    fillPattern(payloadC, 3);

    msgA = 901;
    msgB = 902;
    msgC = 903;

    SRT_MSGCTRL ca = makeCtrl(start, msgA);
    buf.addBuffer(payloadA.data(), (int) payloadA.size(), ca);
    SRT_MSGCTRL cb = makeCtrl(ca.pktseq, msgB);
    buf.addBuffer(payloadB.data(), (int) payloadB.size(), cb);
    SRT_MSGCTRL cc = makeCtrl(cb.pktseq, msgC);
    buf.addBuffer(payloadC.data(), (int) payloadC.size(), cc);
}

int buffer_partial_ack()
{
    CSndBuffer buf(AF_INET, 64, PAYLOAD, 0);

    int32_t msgA = 0, msgB = 0, msgC = 0;
    enqueueABC(buf, 100, msgA, msgB, msgC);

    // Five blocks total: A(100,101) B(102,103) C(104).
    int blocks_before = 0, bytes_before = 0, span_before = 0;
    blocks_before = buf.getCurrBufSize(bytes_before, span_before);
    TASK_CHECK(blocks_before == 5);
    TASK_CHECK(bytes_before == 5 * PAYLOAD);

    // The head of A is acknowledged and recycled; the tail of A stays.
    buf.ackData(1);
    int blocks_mid = 0, bytes_mid = 0, span_mid = 0;
    blocks_mid = buf.getCurrBufSize(bytes_mid, span_mid);
    TASK_CHECK(blocks_mid == 4);
    TASK_CHECK(bytes_mid == 4 * PAYLOAD);

    // Revoke what remains of A: exactly its tail block.
    CSndBuffer::DropMsgResult ra = buf.dropMessage(msgA);
    TASK_CHECK(ra.found);
    TASK_CHECK(ra.blocks == 1);
    TASK_CHECK(ra.bytes == PAYLOAD);
    TASK_CHECK(ra.seqlo == 101 && ra.seqhi == 101);

    // Revocation is not an ACK: buffer occupancy/bytes are untouched.
    int blocks_after = 0, bytes_after = 0, span_after = 0;
    blocks_after = buf.getCurrBufSize(bytes_after, span_after);
    TASK_CHECK(blocks_after == 4);
    TASK_CHECK(bytes_after == bytes_mid);

    // Repeated revocation reports zero newly dropped bytes but stays found.
    CSndBuffer::DropMsgResult ra2 = buf.dropMessage(msgA);
    TASK_CHECK(ra2.found);
    TASK_CHECK(ra2.bytes == 0 && ra2.blocks == 0);

    // Revoke B: both blocks, exact range, C must not be touched.
    CSndBuffer::DropMsgResult rb = buf.dropMessage(msgB);
    TASK_CHECK(rb.found && rb.blocks == 2 && rb.bytes == 2 * PAYLOAD);
    TASK_CHECK(rb.seqlo == 102 && rb.seqhi == 103);

    // Unknown, in-range message number: successful no-op.
    CSndBuffer::DropMsgResult rn = buf.dropMessage(123456);
    TASK_CHECK(!rn.found && rn.bytes == 0);

    // Retransmit requests for revoked blocks become READ_DROP ranges whose
    // ownership is bounded by the message, including the partial ACK head.
    CPacket pkt;
    srt::sync::steady_clock::time_point ts;
    CSndBuffer::DropRange drop;
    pkt.set_seqno(101);
    TASK_CHECK(buf.readData(0, pkt, ts, drop) == CSndBuffer::READ_DROP);
    TASK_CHECK(drop.msgno == msgA);
    TASK_CHECK(drop.seqno[CSndBuffer::DropRange::BEGIN] == 101);
    TASK_CHECK(drop.seqno[CSndBuffer::DropRange::END] == 101);

    // B starts at buffer offset 1 (A's tail, B0, B1, C0).
    pkt.set_seqno(102);
    TASK_CHECK(buf.readData(1, pkt, ts, drop) == CSndBuffer::READ_DROP);
    TASK_CHECK(drop.msgno == msgB && drop.seqno[0] == 102 && drop.seqno[1] == 103);

    // C is still retrievable as retransmission payload (neighbor spared).
    pkt.set_seqno(104);
    TASK_CHECK(buf.readData(3, pkt, ts, drop) > 0);
    TASK_CHECK(pkt.seqno() == 104);

    // Unique send path skips every revoked block and lands on C, consuming
    // three sequence numbers (A-tail + B0 + B1) without emitting payload.
    CPacket unique;
    int seqinc = 0;
    const int n = buf.readData(unique, ts, 0 /*EK_NOENC*/, seqinc);
    TASK_CHECK(n == PAYLOAD);
    TASK_CHECK(seqinc == 3);
    TASK_CHECK(unique.seqno() == 104);

    // Real ACK recycling finally reclaims everything, including revoked blocks.
    buf.ackData(4);
    int blocks_end = 0, bytes_end = 0, span_end = 0;
    blocks_end = buf.getCurrBufSize(bytes_end, span_end);
    TASK_CHECK(blocks_end == 0 && bytes_end == 0);
    return 0;
}

int buffer_wrap_zero()
{
    CSndBuffer buf(AF_INET, 64, PAYLOAD, 0);

    // Straddle the 31-bit sequence boundary.
    const int32_t near_max = 0x7FFFFFFD;
    int32_t msgA = 701, msgB = 702, msgC = 703;

    std::vector<char> payloadA(3 * PAYLOAD); // seq 0x7FFFFFFD, E, F
    std::vector<char> payloadB(2 * PAYLOAD); // seq 0, 1
    std::vector<char> payloadC(PAYLOAD);     // seq 2
    fillPattern(payloadA, 11);
    fillPattern(payloadB, 12);
    fillPattern(payloadC, 13);

    SRT_MSGCTRL ca = makeCtrl(near_max, msgA);
    buf.addBuffer(payloadA.data(), (int) payloadA.size(), ca);
    SRT_MSGCTRL cb = makeCtrl(ca.pktseq, msgB);
    buf.addBuffer(payloadB.data(), (int) payloadB.size(), cb);
    SRT_MSGCTRL cc = makeCtrl(cb.pktseq, msgC);
    buf.addBuffer(payloadC.data(), (int) payloadC.size(), cc);
    TASK_CHECK(cc.pktseq == 3); // next free sequence after the wrap

    // ACK the first two A blocks; A's last block (0x7FFFFFFF) is still queued.
    buf.ackData(2);

    CSndBuffer::DropMsgResult ra = buf.dropMessage(msgA);
    TASK_CHECK(ra.found && ra.blocks == 1 && ra.bytes == PAYLOAD);
    TASK_CHECK(ra.seqlo == 0x7FFFFFFF && ra.seqhi == 0x7FFFFFFF);

    // B spans the wrap: range boundaries must be reported correctly.
    CSndBuffer::DropMsgResult rb = buf.dropMessage(msgB);
    TASK_CHECK(rb.found && rb.blocks == 2 && rb.bytes == 2 * PAYLOAD);
    TASK_CHECK(rb.seqlo == 0 && rb.seqhi == 1);

    // Rexmit read at the wrapped boundary returns the exact wrapped range.
    CPacket pkt;
    srt::sync::steady_clock::time_point ts;
    CSndBuffer::DropRange drop;
    pkt.set_seqno(0);
    TASK_CHECK(buf.readData(1, pkt, ts, drop) == CSndBuffer::READ_DROP);
    TASK_CHECK(drop.msgno == msgB && drop.seqno[0] == 0 && drop.seqno[1] == 1);

    // C on the other side of the wrap is untouched.
    pkt.set_seqno(2);
    TASK_CHECK(buf.readData(3, pkt, ts, drop) > 0);
    TASK_CHECK(pkt.seqno() == 2);

    // Unique send skips revoked blocks across the wrap and reaches C.
    CPacket unique;
    int seqinc = 0;
    TASK_CHECK(buf.readData(unique, ts, 0, seqinc) == PAYLOAD);
    TASK_CHECK(seqinc == 3);
    TASK_CHECK(unique.seqno() == 2);
    return 0;
}

// --------------------------------------------------------------------------
// UDP forwarder: a deliberately visible man-in-the-middle for SRT traffic
// --------------------------------------------------------------------------

struct DropRecord
{
    int32_t msgno;
    int32_t lo;
    int32_t hi;
};

class UdpRelay
{
public:
    UdpRelay(uint16_t listenPort, uint16_t receiverPort, int32_t targetMsgNo,
             bool swallowFirstDropReq, bool holdNak)
        : m_listenPort(listenPort)
        , m_receiverPort(receiverPort)
        , m_targetMsgNo(targetMsgNo)
        , m_swallowFirstDropReq(swallowFirstDropReq)
        , m_holdNak(holdNak)
    {}

    ~UdpRelay() { stop(); }

    bool start()
    {
        m_sockSender = socket(AF_INET, SOCK_DGRAM, 0);
        m_sockReceiver = socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sockSender < 0 || m_sockReceiver < 0)
            return false;

        sockaddr_in front;
        std::memset(&front, 0, sizeof(front));
        front.sin_family = AF_INET;
        front.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        front.sin_port = htons(m_listenPort);
        if (bind(m_sockSender, (sockaddr*) &front, sizeof(front)) != 0)
            return false;

        std::memset(&m_receiverAddr, 0, sizeof(m_receiverAddr));
        m_receiverAddr.sin_family = AF_INET;
        m_receiverAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        m_receiverAddr.sin_port = htons(m_receiverPort);

        m_running = true;
        m_thread = std::thread(&UdpRelay::run, this);
        return true;
    }

    void stop()
    {
        if (!m_running.exchange(false))
            return;
        if (m_thread.joinable())
            m_thread.join();
        if (m_sockSender >= 0) { close(m_sockSender); m_sockSender = -1; }
        if (m_sockReceiver >= 0) { close(m_sockReceiver); m_sockReceiver = -1; }
    }

    bool waitDropReq(int minCount, int ms)
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        return m_cv.wait_for(lk, std::chrono::milliseconds(ms),
                             [&] { return (int) m_dropRecords.size() >= minCount; });
    }

    bool waitNak(int ms)
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        return m_cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return m_nakCount > 0; });
    }

    // Forward every NAK held back so far. Used to order the NAK strictly after
    // the application's srt_dropmsg call in the recovery scenario.
    void releaseNaks()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (size_t i = 0; i < m_heldNak.size(); ++i)
        {
            sendto(m_sockSender, m_heldNak[i].data(), m_heldNak[i].size(), 0,
                   (sockaddr*) &m_senderAddr, m_senderAddrLen);
        }
        m_heldNak.clear();
        m_holdNak = false; // periodic NAKs from now on pass straight through
    }

    bool waitDataDropped(int ms)
    {
        std::unique_lock<std::mutex> lk(m_mtx);
        return m_cv.wait_for(lk, std::chrono::milliseconds(ms),
                             [&] { return !m_droppedDataSeq.empty(); });
    }

    std::vector<DropRecord> dropRecords()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_dropRecords;
    }

    long nakCount()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_nakCount;
    }

    bool rexmitSeenForDropped()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_rexmitForDropped;
    }

    std::vector<int32_t> droppedDataSeq()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_droppedDataSeq;
    }

private:
    static uint32_t w32(const unsigned char* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
             | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    void run()
    {
        while (m_running)
        {
            pollfd fds[2];
            fds[0].fd = m_sockSender;
            fds[0].events = POLLIN;
            fds[1].fd = m_sockReceiver;
            fds[1].events = POLLIN;
            const int pr = poll(fds, 2, 50);
            if (pr <= 0)
                continue;

            if (fds[0].revents & POLLIN)
                relayFromSender();
            if (fds[1].revents & POLLIN)
                relayFromReceiver();
        }
    }

    void relayFromSender()
    {
        unsigned char buf[65535];
        sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        const ssize_t n = recvfrom(m_sockSender, buf, sizeof(buf), 0, (sockaddr*) &from, &fromLen);
        if (n < HDR_SIZE)
            return;

        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_senderAddrLen == 0)
            {
                m_senderAddr = from;
                m_senderAddrLen = fromLen;
            }
        }

        const uint32_t w0 = w32(buf);
        const bool isControl = (w0 & 0x80000000u) != 0;

        if (isControl)
        {
            const int type = int((w0 >> 16) & 0x7FFFu);
            if (type == 7 /*UMSG_DROPREQ*/ && n >= HDR_SIZE + 8)
            {
                const int32_t msgno = int32_t(w32(buf + 4));
                const int32_t lo = int32_t(w32(buf + HDR_SIZE));
                const int32_t hi = int32_t(w32(buf + HDR_SIZE + 4));
                bool swallow = false;
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_dropRecords.push_back(DropRecord { msgno, lo, hi });
                    if (m_swallowFirstDropReq && !m_firstDropReqSwallowed)
                    {
                        m_firstDropReqSwallowed = true;
                        swallow = true;
                    }
                    m_cv.notify_all();
                }
                if (swallow)
                    return;
            }
        }
        else
        {
            // Data packet: word0 low 31 bits = seqno, word1 low 26 bits = msgno.
            const int32_t seqno = int32_t(w0 & 0x7FFFFFFFu);
            const uint32_t w1 = w32(buf + 4);
            const int32_t msgno = int32_t(w1 & 0x03FFFFFFu);
            const int boundary = int((w1 >> 30) & 0x3u);
            const bool rexmit = (w1 & 0x04000000u) != 0;

            std::lock_guard<std::mutex> lk(m_mtx);

            // A seqno once selected for loss must never cross, whether as an
            // original or as a retransmission. This makes the gap deterministic
            // and independent of the srt_dropmsg/rexmit scheduling race.
            bool seqAlreadyLost = false;
            for (size_t i = 0; i < m_droppedDataSeq.size(); ++i)
            {
                if (m_droppedDataSeq[i] == seqno)
                {
                    seqAlreadyLost = true;
                    break;
                }
            }
            if (seqAlreadyLost)
            {
                if (rexmit)
                    m_rexmitForDropped = true;
                m_cv.notify_all();
                return;
            }

            // Lose the first packet of the target message to force a gap.
            if (msgno == m_targetMsgNo && (boundary == 2 /*PB_FIRST*/ || boundary == 3 /*PB_SOLO*/)
                && m_droppedDataSeq.empty())
            {
                m_droppedDataSeq.push_back(seqno);
                m_cv.notify_all();
                return;
            }
        }

        sendto(m_sockReceiver, buf, n, 0, (sockaddr*) &m_receiverAddr, sizeof(m_receiverAddr));
    }

    void relayFromReceiver()
    {
        unsigned char buf[65535];
        sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        const ssize_t n = recvfrom(m_sockReceiver, buf, sizeof(buf), 0, (sockaddr*) &from, &fromLen);
        if (n < HDR_SIZE)
            return;

        const uint32_t w0 = w32(buf);
        bool held = false;
        if ((w0 & 0x80000000u) != 0)
        {
            const int type = int((w0 >> 16) & 0x7FFFu);
            if (type == 3 /*UMSG_LOSSREPORT*/)
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                ++m_nakCount;
                if (m_holdNak)
                {
                    m_heldNak.push_back(std::vector<unsigned char>(buf, buf + n));
                    held = true;
                }
                m_cv.notify_all();
            }
        }

        if (held)
            return;

        sockaddr_in target;
        socklen_t targetLen;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_senderAddrLen == 0)
                return;
            target = m_senderAddr;
            targetLen = m_senderAddrLen;
        }
        sendto(m_sockSender, buf, n, 0, (sockaddr*) &target, targetLen);
    }

    uint16_t m_listenPort;
    uint16_t m_receiverPort;
    int32_t  m_targetMsgNo;
    bool     m_swallowFirstDropReq;
    bool     m_holdNak;

    int m_sockSender = -1;   // faces the SRT sender
    int m_sockReceiver = -1; // faces the SRT receiver
    sockaddr_in m_receiverAddr;
    sockaddr_in m_senderAddr;
    socklen_t m_senderAddrLen = 0;

    std::thread m_thread;
    std::atomic<bool> m_running { false };

    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::vector<DropRecord> m_dropRecords;
    std::vector<int32_t> m_droppedDataSeq;
    std::vector<std::vector<unsigned char> > m_heldNak;
    long m_nakCount = 0;
    bool m_rexmitForDropped = false;
    bool m_firstDropReqSwallowed = false;
};

// --------------------------------------------------------------------------
// Connected SRT pair, optionally through a relay
// --------------------------------------------------------------------------

struct ReceivedMsg
{
    int32_t msgno;
    std::vector<char> data;
};

struct PairEndpoints
{
    uint16_t receiverPort;
    uint16_t relayPort;
};

// Bring up a listener/receiver and a connected sender. When @a relayPort is
// non-zero the sender connects through the relay instead of directly.
bool connectPair(SRTSOCKET& sender, SRTSOCKET& receiver, const PairEndpoints& ep, int fc)
{
    receiver = srt_create_socket();
    if (receiver == SRT_INVALID_SOCK || !configure(receiver, fc))
        return false;

    sockaddr_in raddr;
    std::memset(&raddr, 0, sizeof(raddr));
    raddr.sin_family = AF_INET;
    raddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    raddr.sin_port = htons(ep.receiverPort);
    if (srt_bind(receiver, (sockaddr*) &raddr, sizeof(raddr)) != 0
        || srt_listen(receiver, 2) != 0)
        return false;

    sender = srt_create_socket();
    if (sender == SRT_INVALID_SOCK || !configure(sender, 0))
        return false;

    sockaddr_in target;
    std::memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(ep.relayPort ? ep.relayPort : ep.receiverPort);
    if (srt_connect(sender, (sockaddr*) &target, sizeof(target)) != 0)
        return false;

    SRTSOCKET accepted = srt_accept(receiver, NULL, NULL);
    if (accepted == SRT_INVALID_SOCK)
        return false;
    srt_close(receiver);     // the listener is no longer needed
    receiver = accepted;     // accepted socket carries the data connection
    // The accepted socket inherits the listener's options; only the timeouts
    // are (re)asserted here because they remain settable after connection.
    const int timeout = 2000;
    return setFlag(receiver, SRTO_RCVTIMEO, timeout)
        && setFlag(receiver, SRTO_SNDTIMEO, timeout);
}

void receiverLoop(SRTSOCKET sock, std::vector<ReceivedMsg>& out, std::mutex& mtx,
                  std::condition_variable& cv, std::atomic<bool>& stop, int wanted)
{
    std::vector<char> buf(8 * PAYLOAD);
    while (!stop)
    {
        SRT_MSGCTRL mc = srt_msgctrl_default;
        const int n = srt_recvmsg2(sock, buf.data(), (int) buf.size(), &mc);
        if (n <= 0)
            continue; // timeout or transient error: re-check stop and retry
        ReceivedMsg rm;
        rm.msgno = mc.msgno;
        rm.data.assign(buf.begin(), buf.begin() + n);
        {
            std::lock_guard<std::mutex> lk(mtx);
            out.push_back(std::move(rm));
            if ((int) out.size() >= wanted)
            {
                cv.notify_all();
                return;
            }
        }
    }
}

bool waitMessages(std::vector<ReceivedMsg>& msgs, std::mutex& mtx,
                  std::condition_variable& cv, int wanted)
{
    std::unique_lock<std::mutex> lk(mtx);
    return cv.wait_for(lk, std::chrono::milliseconds(WAIT_LIMIT_MS),
                       [&] { return (int) msgs.size() >= wanted; });
}

// RAII teardown so an early TASK_CHECK return can never leave a receiver
// thread, relay or SRT socket alive.
struct ThreadGuard
{
    std::thread& t;
    std::atomic<bool>& stop;
    ~ThreadGuard() { stop = true; if (t.joinable()) t.join(); }
};

struct RelayGuard
{
    UdpRelay& r;
    ~RelayGuard() { r.stop(); }
};

struct SocketGuard
{
    SRTSOCKET& s;
    ~SocketGuard() { if (s != SRT_INVALID_SOCK) srt_close(s); }
};

// --------------------------------------------------------------------------
// wire group
// --------------------------------------------------------------------------

int wire_dropreq_and_delivery()
{
    SrtGuard guard;

    // Fixed loopback ports; the relay owns the sender-facing one. On a fresh
    // connection the first message always gets msgno 1, so the relay can be
    // armed before the connection is established.
    const PairEndpoints ep { 44101, 44102 };
    const int32_t targetMsgNo = 1;

    UdpRelay relay(ep.relayPort, ep.receiverPort, targetMsgNo,
                   /*swallowFirstDropReq*/ false, /*holdNak*/ false);
    if (!relay.start())
    {
        std::fprintf(stderr, "wire: relay start failed (binding disabled?)\n");
        return 1;
    }
    RelayGuard rg { relay };

    SRTSOCKET sender = SRT_INVALID_SOCK, receiver = SRT_INVALID_SOCK;
    if (!connectPair(sender, receiver, ep, 0))
    {
        std::fprintf(stderr, "wire: connect failed: %s\n", srt_getlasterror_str());
        return 1;
    }
    SocketGuard sg_s { sender }, sg_r { receiver };

    std::vector<char> payloadA(3 * PAYLOAD);
    std::vector<char> payloadB(PAYLOAD);
    std::vector<char> payloadC(2 * PAYLOAD);
    fillPattern(payloadA, 21);
    fillPattern(payloadB, 22);
    fillPattern(payloadC, 23);

    SRT_MSGCTRL ca = srt_msgctrl_default;
    ca.inorder = 1;
    if (srt_sendmsg2(sender, payloadA.data(), (int) payloadA.size(), &ca) != (int) payloadA.size())
    {
        std::fprintf(stderr, "wire: send A failed: %s\n", srt_getlasterror_str());
        return 1;
    }
    TASK_CHECK(ca.msgno == targetMsgNo);

    SRT_MSGCTRL cb = srt_msgctrl_default;
    cb.inorder = 1;
    SRT_MSGCTRL cc = srt_msgctrl_default;
    cc.inorder = 1;
    TASK_CHECK(srt_sendmsg2(sender, payloadB.data(), (int) payloadB.size(), &cb) == (int) payloadB.size());
    TASK_CHECK(srt_sendmsg2(sender, payloadC.data(), (int) payloadC.size(), &cc) == (int) payloadC.size());

    // The gap packet must actually have been lost before we revoke A.
    TASK_CHECK(relay.waitDataDropped(WAIT_LIMIT_MS));

    std::vector<ReceivedMsg> received;
    std::mutex rmtx;
    std::condition_variable rcv;
    std::atomic<bool> stop { false };
    std::thread rth(receiverLoop, receiver, std::ref(received), std::ref(rmtx),
                    std::ref(rcv), std::ref(stop), 2);
    ThreadGuard tg { rth, stop };

    const int dropped = srt_dropmsg(sender, ca.msgno);
    TASK_CHECK(dropped > 0);
    TASK_CHECK(dropped <= (int) payloadA.size());

    // A DROPREQ for A's remaining range is observed on the wire.
    TASK_CHECK(relay.waitDropReq(1, WAIT_LIMIT_MS));
    const std::vector<DropRecord> records = relay.dropRecords();
    TASK_CHECK(!records.empty());
    bool match = false;
    for (size_t i = 0; i < records.size(); ++i)
        if (records[i].msgno == ca.msgno) match = true;
    TASK_CHECK(match);
    const DropRecord& r0 = records[0];
    TASK_CHECK(r0.lo >= 0 && r0.hi >= 0);
    TASK_CHECK(CSeqNo::seqoff(r0.lo, r0.hi) >= 0);

    // The receiver skips A entirely and delivers B then C, intact and ordered.
    TASK_CHECK(waitMessages(received, rmtx, rcv, 2));

    TASK_CHECK(received.size() >= 2);
    TASK_CHECK(received[0].msgno == cb.msgno);
    TASK_CHECK(received[0].data == payloadB);
    TASK_CHECK(received[1].msgno == cc.msgno);
    TASK_CHECK(received[1].data == payloadC);
    for (size_t i = 0; i < received.size(); ++i)
        TASK_CHECK(received[i].msgno != ca.msgno);

    // The connection is still healthy: another message goes through.
    std::vector<char> payloadD(PAYLOAD);
    fillPattern(payloadD, 24);
    SRT_MSGCTRL cd = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(sender, payloadD.data(), (int) payloadD.size(), &cd) == (int) payloadD.size());

    return 0;
}

// --------------------------------------------------------------------------
// recovery group
// --------------------------------------------------------------------------

int recovery_lost_dropreq()
{
    SrtGuard guard;

    const PairEndpoints ep { 44201, 44202 };
    const int32_t targetMsgNo = 1;

    // Swallow the explicit DROPREQ and hold NAKs until the test releases them,
    // so the second DROPREQ is provably drawn by the NAK, not by timing.
    UdpRelay relay(ep.relayPort, ep.receiverPort, targetMsgNo,
                   /*swallowFirstDropReq*/ true, /*holdNak*/ true);
    if (!relay.start())
    {
        std::fprintf(stderr, "recovery: relay start failed (binding disabled?)\n");
        return 1;
    }
    RelayGuard rg { relay };

    SRTSOCKET sender = SRT_INVALID_SOCK, receiver = SRT_INVALID_SOCK;
    if (!connectPair(sender, receiver, ep, 0))
    {
        std::fprintf(stderr, "recovery: connect failed: %s\n", srt_getlasterror_str());
        return 1;
    }
    SocketGuard sg_s { sender }, sg_r { receiver };

    std::vector<char> payloadA(3 * PAYLOAD);
    std::vector<char> payloadB(PAYLOAD);
    fillPattern(payloadA, 31);
    fillPattern(payloadB, 32);

    SRT_MSGCTRL ca = srt_msgctrl_default;
    ca.inorder = 1;
    TASK_CHECK(srt_sendmsg2(sender, payloadA.data(), (int) payloadA.size(), &ca) == (int) payloadA.size());
    TASK_CHECK(ca.msgno == targetMsgNo);

    SRT_MSGCTRL cb = srt_msgctrl_default;
    cb.inorder = 1;
    TASK_CHECK(srt_sendmsg2(sender, payloadB.data(), (int) payloadB.size(), &cb) == (int) payloadB.size());

    // The first packet of A is lost by the relay; the receiver reports the gap.
    TASK_CHECK(relay.waitDataDropped(WAIT_LIMIT_MS));
    TASK_CHECK(relay.waitNak(WAIT_LIMIT_MS));
    const std::vector<int32_t> lostSeq = relay.droppedDataSeq();
    TASK_CHECK(lostSeq.size() == 1);
    const int32_t lost = lostSeq[0];

    std::vector<ReceivedMsg> received;
    std::mutex rmtx;
    std::condition_variable rcv;
    std::atomic<bool> stop { false };
    std::thread rth(receiverLoop, receiver, std::ref(received), std::ref(rmtx),
                    std::ref(rcv), std::ref(stop), 1);
    ThreadGuard tg { rth, stop };

    // Revoke A: this emits DROPREQ #1, which the relay deliberately drops.
    const int dropped = srt_dropmsg(sender, ca.msgno);
    TASK_CHECK(dropped > 0);

    // Now let the held NAK reach the sender. It must be answered with another
    // DROPREQ (never the original payload), recovering the lost notification.
    relay.releaseNaks();
    TASK_CHECK(relay.waitDropReq(2, WAIT_LIMIT_MS));
    const std::vector<DropRecord> records = relay.dropRecords();
    TASK_CHECK(records.size() >= 2);
    TASK_CHECK(records[0].msgno == ca.msgno && records[1].msgno == ca.msgno);
    // Both ranges start at the lost sequence (it cannot be ACKed across a gap).
    TASK_CHECK(records[0].lo == records[1].lo);
    TASK_CHECK(records[1].lo == lost);
    // The NAK-driven range can only be narrower (intermediate blocks may have
    // been ACK-recycled), never wider than the explicit one.
    TASK_CHECK(CSeqNo::seqcmp(records[1].hi, records[0].hi) <= 0);
    TASK_CHECK(relay.nakCount() >= 1);

    // The original payload must never have been re-sent for the lost sequence.
    TASK_CHECK(!relay.rexmitSeenForDropped());
    TASK_CHECK(relay.droppedDataSeq().size() == 1);

    // B is delivered completely once the gap is resolved.
    TASK_CHECK(waitMessages(received, rmtx, rcv, 1));

    TASK_CHECK(received.size() >= 1);
    TASK_CHECK(received[0].msgno == cb.msgno);
    TASK_CHECK(received[0].data == payloadB);

    return 0;
}

// --------------------------------------------------------------------------
// api group
// --------------------------------------------------------------------------

int api_errors_without_connection()
{
    SrtGuard guard;

    // Nonexistent socket.
    TASK_CHECK(srt_dropmsg(SRT_INVALID_SOCK, 1) == SRT_ERROR);
    int err = 0;
    TASK_CHECK(srt_getlasterror(&err) == SRT_EINVSOCK);
    TASK_CHECK(err != 0);

    // Existing but never connected socket.
    const SRTSOCKET idle = srt_create_socket();
    TASK_CHECK(idle != SRT_INVALID_SOCK);
    TASK_CHECK(configure(idle));
    TASK_CHECK(srt_dropmsg(idle, 1) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_ENOCONN);

    srt_close(idle);
    return 0;
}

int api_param_and_mode_validation()
{
    SrtGuard guard;

    const PairEndpoints ep { 44301, 0 }; // direct connection, no relay
    SRTSOCKET sender = SRT_INVALID_SOCK, receiver = SRT_INVALID_SOCK;
    if (!connectPair(sender, receiver, ep, 0))
    {
        std::fprintf(stderr, "api: connect failed: %s\n", srt_getlasterror_str());
        return 1;
    }

    // Control message number and out-of-range values.
    TASK_CHECK(srt_dropmsg(sender, SRT_MSGNO_CONTROL) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVPARAM);
    TASK_CHECK(srt_dropmsg(sender, SRT_MSGNO_NONE) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVPARAM);
    TASK_CHECK(srt_dropmsg(sender, srt::MSGNO_SEQ_MAX + 1) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVPARAM);

    // In-range but never used message number is a successful no-op.
    TASK_CHECK(srt_dropmsg(sender, 424242) == 0);

    srt_close(sender);
    srt_close(receiver);

    // Byte-stream API sockets reject the operation.
    SRTSOCKET ss = srt_create_socket(), rr = srt_create_socket();
    TASK_CHECK(ss != SRT_INVALID_SOCK && rr != SRT_INVALID_SOCK);
    TASK_CHECK(configure(ss, 0, /*message_api*/ false));
    TASK_CHECK(configure(rr, 0, /*message_api*/ false));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(44302);
    TASK_CHECK(srt_bind(rr, (sockaddr*) &addr, sizeof(addr)) == 0);
    TASK_CHECK(srt_listen(rr, 2) == 0);
    TASK_CHECK(srt_connect(ss, (sockaddr*) &addr, sizeof(addr)) == 0);
    SRTSOCKET accepted = srt_accept(rr, NULL, NULL);
    TASK_CHECK(accepted != SRT_INVALID_SOCK);

    TASK_CHECK(srt_dropmsg(ss, 1) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVALBUFFERAPI);

    srt_close(ss);
    srt_close(accepted);
    srt_close(rr);
    return 0;
}

int api_bytes_accounting_and_continuity()
{
    SrtGuard guard;

    // A small receiver flight flag size keeps part of a big message unACKed.
    const PairEndpoints ep { 44303, 0 };
    SRTSOCKET sender = SRT_INVALID_SOCK, receiver = SRT_INVALID_SOCK;
    if (!connectPair(sender, receiver, ep, 32))
    {
        std::fprintf(stderr, "api accounting: connect failed: %s\n", srt_getlasterror_str());
        return 1;
    }

    // 64 packets: only a flight window worth can be in flight at once, so a
    // substantial tail normally stays in the sender buffer when we revoke.
    std::vector<char> payloadA(64 * PAYLOAD);
    std::vector<char> payloadB(PAYLOAD);
    fillPattern(payloadA, 41);
    fillPattern(payloadB, 42);

    SRT_MSGCTRL ca = srt_msgctrl_default;
    ca.inorder = 1;
    TASK_CHECK(srt_sendmsg2(sender, payloadA.data(), (int) payloadA.size(), &ca) == (int) payloadA.size());

    size_t blocksBefore = 0, bytesBefore = 0;
    TASK_CHECK(srt_getsndbuffer(sender, &blocksBefore, &bytesBefore) == 0);

    // A valid call; on loopback some ACKs may already have recycled part of A,
    // so the byte count is asserted for range, not for a fixed positive value.
    const int dropped = srt_dropmsg(sender, ca.msgno);
    TASK_CHECK(dropped >= 0);
    TASK_CHECK(dropped <= (int) payloadA.size());

    // Revocation must not free window slots or alter sender-buffer occupancy.
    size_t blocksAfter = 0, bytesAfter = 0;
    TASK_CHECK(srt_getsndbuffer(sender, &blocksAfter, &bytesAfter) == 0);
    TASK_CHECK(blocksAfter == blocksBefore);
    TASK_CHECK(bytesAfter == bytesBefore);

    // A second call revokes nothing new.
    TASK_CHECK(srt_dropmsg(sender, ca.msgno) == 0);

    // Send B. A may already have been delivered intact (there is no relay
    // loss here), so read until B shows up, checking its content and order.
    SRT_MSGCTRL cb = srt_msgctrl_default;
    cb.inorder = 1;
    TASK_CHECK(srt_sendmsg2(sender, payloadB.data(), (int) payloadB.size(), &cb) == (int) payloadB.size());

    std::vector<char> rxBuf(128 * PAYLOAD);
    bool gotB = false;
    int32_t prevMsgNo = SRT_MSGNO_NONE;
    for (int i = 0; i < 200 && !gotB; ++i)
    {
        SRT_MSGCTRL rmc = srt_msgctrl_default;
        const int n = srt_recvmsg2(receiver, rxBuf.data(), (int) rxBuf.size(), &rmc);
        if (n <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        // Messages must arrive in sending order.
        TASK_CHECK(prevMsgNo == SRT_MSGNO_NONE || rmc.msgno > prevMsgNo);
        prevMsgNo = rmc.msgno;
        if (rmc.msgno == cb.msgno)
        {
            TASK_CHECK(n == (int) payloadB.size());
            TASK_CHECK(std::memcmp(rxBuf.data(), payloadB.data(), payloadB.size()) == 0);
            gotB = true;
        }
    }
    TASK_CHECK(gotB);

    // Eventually the real ACK flow drains the sender buffer and the connection
    // stays up, including the blocks that were only revoked, not acknowledged.
    bool drained = false;
    for (int i = 0; i < 100; ++i)
    {
        size_t blocks = 0, bytes = 0;
        srt_getsndbuffer(sender, &blocks, &bytes);
        if (blocks == 0) { drained = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    TASK_CHECK(drained);
    TASK_CHECK(srt_getsockstate(sender) == SRTS_CONNECTED);

    srt_close(sender);
    srt_close(receiver);
    return 0;
}

} // namespace

extern const struct task_case task_cases[] = {
    { "buffer",   "partial_ack_ownership",      buffer_partial_ack            },
    { "buffer",   "sequence_wrap_zero",         buffer_wrap_zero             },
    { "wire",     "dropreq_and_delivery",       wire_dropreq_and_delivery    },
    { "recovery", "lost_dropreq_nak_repeat",    recovery_lost_dropreq        },
    { "api",      "errors_without_connection",  api_errors_without_connection},
    { "api",      "param_and_mode_validation",  api_param_and_mode_validation},
    { "api",      "bytes_accounting_continuity",api_bytes_accounting_and_continuity },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
