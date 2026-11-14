// Regression tests for multi-packet live messages:
//  - group "message":  receiver buffer whole-message delivery (controlled time)
//  - group "deadline": message-aware TLPKTDROP at the play deadline (controlled time)
//  - group "api":      public API over local connections with a UDP relay
//                      controlling missing, repaired and late fragments.

#include "task_support/check.h"

#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "srt.h"

#include "buffer_rcv.h"
#include "queue.h"
#include "packet.h"

using namespace srt;

namespace {

// ---------------------------------------------------------------------------
// Buffer-level fixtures (controlled time, no sockets).
// ---------------------------------------------------------------------------

typedef srt::sync::steady_clock::time_point time_point;

// Fixed time base: packet timestamp 0 plays exactly at T_PLAY.
const time_point  T_BASE = srt::sync::steady_clock::now();
const srt::sync::steady_clock::duration T_LAT = srt::sync::milliseconds_from(200);
const time_point  T_PLAY = T_BASE + T_LAT;

time_point usec_before(time_point t, int64_t us) { return t - srt::sync::microseconds_from(us); }
time_point usec_after(time_point t, int64_t us)  { return t + srt::sync::microseconds_from(us); }

struct BufFixture
{
    CUnitQueue  units;
    CRcvBuffer  buf;

    BufFixture(int initseq, int size, bool tsbpd, bool msgapi = true)
        : units(4 * size, 1500)
        , buf(initseq, size, &units, msgapi)
    {
        buf.setPeerRexmitFlag(true);
        if (tsbpd)
            buf.setTsbPdMode(T_BASE, false, T_LAT);
    }
};

// Insert one packet carrying a fragment of message `msg`.
// Returns the result of CRcvBuffer::insert().
int put(BufFixture& f, int seq, int msg, PacketBoundary pb, const char* data, uint32_t ts = 0)
{
    CUnit* u = f.units.getNextAvailUnit();
    if (!u)
        return -100;
    CPacket& p = u->m_Packet;
    p.set_seqno(seq);
    p.set_timestamp(ts);
    p.set_msgflags(msg | PacketBoundaryBits(pb) | MSGNO_PACKET_INORDER::wrap(1));
    p.setLength((int)std::strlen(data));
    std::memcpy(p.data(), data, p.getLength());
    return f.buf.insert(u);
}

// ---------------------------------------------------------------------------
// Group "message": whole-message delivery from the receiver buffer.
// ---------------------------------------------------------------------------

// A fragmented message is delivered as a whole, once, at its play time.
int case_message_whole_delivery()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 7, PB_FIRST, "abc") == 0);
    TASK_CHECK(put(f, 101, 7, PB_SUBSEQUENT, "def") == 0);

    // Incomplete message: not readable even when the play time has passed.
    TASK_CHECK(!f.buf.isRcvDataReady(usec_after(T_PLAY, 1000000)));

    TASK_CHECK(put(f, 102, 7, PB_LAST, "gh") == 0);

    // Complete, but not yet at the play time: must not be delivered early.
    TASK_CHECK(!f.buf.isRcvDataReady(usec_before(T_PLAY, 1)));
    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));

    char out[64];
    std::memset(out, 0, sizeof out);
    SRT_MSGCTRL mc = srt_msgctrl_default;
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 8);
    TASK_CHECK(std::memcmp(out, "abcdefgh", 8) == 0);
    TASK_CHECK(mc.msgno == 7);
    TASK_CHECK(mc.pktseq == 100); // seqno of the FIRST packet of the message
    TASK_CHECK(f.buf.empty());
    return 0;
}

// A ready message that doesn't fit the buffer is reported, not truncated
// and not consumed; a retry with a larger buffer reads the same message.
int case_message_short_buffer_reread()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 3, PB_FIRST, "abc") == 0);
    TASK_CHECK(put(f, 101, 3, PB_SUBSEQUENT, "def") == 0);
    TASK_CHECK(put(f, 102, 3, PB_LAST, "gh") == 0);
    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));

    char out[8];
    std::memset(out, 0x5A, sizeof out);
    SRT_MSGCTRL mc = srt_msgctrl_default;
    mc.msgno  = 4242; // sentinels: must survive the failed read
    mc.pktseq = 3333;

    // 7 bytes is less than the full 8-byte message: fail without consuming.
    TASK_CHECK(f.buf.readMessage(out, 7, &mc) == -1);
    TASK_CHECK(out[0] == 0x5A && out[6] == 0x5A); // output array untouched
    TASK_CHECK(mc.msgno == 4242 && mc.pktseq == 3333); // msgctrl untouched
    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY)); // message NOT consumed

    // Retry with a buffer that fits: same bytes, same message number.
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 8);
    TASK_CHECK(std::memcmp(out, "abcdefgh", 8) == 0);
    TASK_CHECK(mc.msgno == 3);
    TASK_CHECK(mc.pktseq == 100);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// An incomplete head message must not be glued together with the next
// message, and a message completed later is delivered normally.
int case_message_no_concat_next_frame()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 1, PB_FIRST, "ab") == 0);
    // seq 101 (LAST of message 1) is missing.
    TASK_CHECK(put(f, 102, 2, PB_FIRST, "xy") == 0);
    TASK_CHECK(put(f, 103, 2, PB_LAST, "z") == 0);

    // The head message is incomplete: nothing may be delivered, even though
    // message 2 is fully present and the play time has passed.
    TASK_CHECK(!f.buf.isRcvDataReady(usec_after(T_PLAY, 1000000)));

    // The missing LAST packet of message 1 arrives late and completes it.
    TASK_CHECK(put(f, 101, 1, PB_LAST, "cd") == 0);
    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));

    char out[16];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 4);
    TASK_CHECK(std::memcmp(out, "abcd", 4) == 0); // exactly message 1
    TASK_CHECK(mc.msgno == 1);

    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));
    n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 3);
    TASK_CHECK(std::memcmp(out, "xyz", 3) == 0); // then message 2
    TASK_CHECK(mc.msgno == 2);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// File (stream) mode keeps its old small-buffer truncation behavior.
int case_message_stream_truncation_kept()
{
    BufFixture f(100, 16, false, false); // stream API, no TSBPD
    TASK_CHECK(put(f, 100, 1, PB_FIRST, "abc") == 0);
    TASK_CHECK(put(f, 101, 1, PB_LAST, "def") == 0);

    char out[4];
    int n = f.buf.readBuffer(out, 2);
    TASK_CHECK(n == 2);
    TASK_CHECK(std::memcmp(out, "ab", 2) == 0);
    n = f.buf.readBuffer(out, 4);
    TASK_CHECK(n == 4);
    TASK_CHECK(std::memcmp(out, "cdef", 4) == 0);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// Message mode without TSBPD keeps its old small-buffer truncation behavior.
int case_message_nonlive_truncation_kept()
{
    BufFixture f(100, 16, false, true); // message API, no TSBPD
    TASK_CHECK(put(f, 100, 5, PB_FIRST, "abc") == 0);
    TASK_CHECK(put(f, 101, 5, PB_LAST, "def") == 0);

    char out[4];
    std::memset(out, 0, sizeof out);
    SRT_MSGCTRL mc = srt_msgctrl_default;
    const int n = f.buf.readMessage(out, 4, &mc); // smaller than the 6-byte message
    TASK_CHECK(n == 4); // truncated like before
    TASK_CHECK(std::memcmp(out, "abcd", 4) == 0);
    TASK_CHECK(mc.msgno == 5);
    return 0;
}

// ---------------------------------------------------------------------------
// Group "deadline": message-aware TLPKTDROP at the play deadline.
// ---------------------------------------------------------------------------

// FIRST arrived, a middle fragment is missing, LAST is known: at the play
// deadline the whole frame is dropped, and a late fragment cannot revive it.
int case_deadline_middle_missing()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 1, PB_FIRST, "ab") == 0);
    TASK_CHECK(put(f, 102, 1, PB_LAST, "e") == 0); // seq 101 missing

    // Before the play time nothing is dropped: the frame may still complete.
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(usec_before(T_PLAY, 1)) == SRT_SEQNO_NONE);

    // At the play time the drop covers the whole frame, including its LAST.
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == 103);

    const std::pair<int, int> d = f.buf.dropUpTo(103);
    TASK_CHECK(d.first == 1);  // one missing packet (seq 101)
    TASK_CHECK(d.second == 2); // two present packets discarded (seq 100, 102)
    TASK_CHECK(f.buf.empty());
    TASK_CHECK(f.buf.getStartSeqNo() == 103);

    // A late fragment of the dropped frame must not be accepted again.
    TASK_CHECK(put(f, 101, 1, PB_SUBSEQUENT, "cd") == -2);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// FIRST is lost but the tail of the frame arrived: the tail must not block
// the head of the buffer; the whole frame is dropped at the deadline.
int case_deadline_first_lost_tail_present()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 101, 1, PB_SUBSEQUENT, "cd") == 0);
    TASK_CHECK(put(f, 102, 1, PB_LAST, "e") == 0); // seq 100 (FIRST) missing

    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == 103);
    const std::pair<int, int> d = f.buf.dropUpTo(103);
    TASK_CHECK(d.first == 1);  // the missing FIRST
    TASK_CHECK(d.second == 2); // the tail fragments
    TASK_CHECK(f.buf.empty());
    TASK_CHECK(f.buf.getStartSeqNo() == 103);
    return 0;
}

// LAST is lost but the next frame has already started: the incomplete frame
// is dropped up to the next frame's FIRST; the neighbor frame stays intact.
int case_deadline_last_lost_next_started()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 1, PB_FIRST, "ab") == 0);
    // seq 101 (LAST of message 1) missing.
    TASK_CHECK(put(f, 102, 2, PB_FIRST, "xy") == 0);
    TASK_CHECK(put(f, 103, 2, PB_LAST, "z") == 0);

    // The drop ends exactly at the next message's FIRST packet.
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == 102);

    const std::pair<int, int> d = f.buf.dropUpTo(102);
    TASK_CHECK(d.first == 1);  // seq 101 missing
    TASK_CHECK(d.second == 1); // seq 100 discarded
    TASK_CHECK(f.buf.getStartSeqNo() == 102);

    // The neighbor frame was not touched and is delivered normally.
    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));
    char out[8];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 3);
    TASK_CHECK(std::memcmp(out, "xyz", 3) == 0);
    TASK_CHECK(mc.msgno == 2);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// A frame that never arrived at all: the gap is skipped up to the first
// packet of the following message (the classic live-mode gap skip).
int case_deadline_whole_frame_missing()
{
    BufFixture f(100, 16, true);
    // message 1 (seq 100, 101) never arrived at all.
    TASK_CHECK(put(f, 102, 2, PB_FIRST, "xy") == 0);
    TASK_CHECK(put(f, 103, 2, PB_LAST, "z") == 0);

    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == 102);
    const std::pair<int, int> d = f.buf.dropUpTo(102);
    TASK_CHECK(d.first == 2);  // both packets of message 1 were missing
    TASK_CHECK(d.second == 0); // nothing present to discard
    TASK_CHECK(f.buf.getStartSeqNo() == 102);

    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));
    char out[8];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 3);
    TASK_CHECK(std::memcmp(out, "xyz", 3) == 0);
    TASK_CHECK(mc.msgno == 2);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// While the end of the head frame cannot be determined, nothing is dropped
// (no spinning around the passed deadline); a completing fragment arriving
// later makes the frame deliverable again.
int case_deadline_undetermined_end_waits()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 1, PB_FIRST, "ab") == 0);
    TASK_CHECK(put(f, 101, 1, PB_SUBSEQUENT, "cd") == 0);
    // No LAST packet and no following message: the frame end is unknown.

    // Even long after the deadline there is nothing to drop.
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(usec_after(T_PLAY, 10000000)) == SRT_SEQNO_NONE);
    TASK_CHECK(!f.buf.isRcvDataReady(usec_after(T_PLAY, 10000000)));

    // The LAST packet arrives late and completes the frame: delivered.
    TASK_CHECK(put(f, 102, 1, PB_LAST, "ef") == 0);
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(usec_after(T_PLAY, 10000000)) == SRT_SEQNO_NONE);
    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));

    char out[8];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 6);
    TASK_CHECK(std::memcmp(out, "abcdef", 6) == 0);
    TASK_CHECK(mc.msgno == 1);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// While the end of the head frame is unknown, a following message's FIRST
// packet provides the boundary: the incomplete frame is then dropped.
int case_deadline_boundary_from_next_first()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 1, PB_FIRST, "ab") == 0);
    TASK_CHECK(put(f, 101, 1, PB_SUBSEQUENT, "cd") == 0);
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == SRT_SEQNO_NONE);

    // A new packet of the next message determines the boundary.
    TASK_CHECK(put(f, 103, 2, PB_SOLO, "q") == 0); // seq 102 (LAST) still missing
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == 103);

    const std::pair<int, int> d = f.buf.dropUpTo(103);
    TASK_CHECK(d.first == 1);  // seq 102 missing
    TASK_CHECK(d.second == 2); // seq 100, 101 discarded
    TASK_CHECK(f.buf.getStartSeqNo() == 103);

    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));
    char out[4];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 1);
    TASK_CHECK(std::memcmp(out, "q", 1) == 0);
    TASK_CHECK(mc.msgno == 2);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// A frame completed before the deadline is delivered, never dropped.
int case_deadline_completed_frame_not_dropped()
{
    BufFixture f(100, 16, true);
    TASK_CHECK(put(f, 100, 1, PB_FIRST, "ab") == 0);
    TASK_CHECK(put(f, 101, 1, PB_LAST, "cd") == 0);
    TASK_CHECK(put(f, 102, 2, PB_SOLO, "q") == 0);

    // The head message is complete: no drop, even past the deadline.
    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(usec_after(T_PLAY, 10000000)) == SRT_SEQNO_NONE);
    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));

    char out[8];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 4);
    TASK_CHECK(std::memcmp(out, "abcd", 4) == 0);
    TASK_CHECK(mc.msgno == 1);
    n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 1);
    TASK_CHECK(std::memcmp(out, "q", 1) == 0);
    TASK_CHECK(mc.msgno == 2);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// The same rules apply when the 31-bit packet sequence space wraps around.
int case_deadline_seqno_wrap()
{
    const int W = 0x7FFFFFFE; // one before the maximum 31-bit seqno
    BufFixture f(W, 16, true);
    TASK_CHECK(put(f, W, 1, PB_FIRST, "ab") == 0);
    // seq 0x7FFFFFFF (LAST of message 1) missing.
    TASK_CHECK(put(f, 0, 2, PB_FIRST, "xy") == 0); // wrapped sequence numbers
    TASK_CHECK(put(f, 1, 2, PB_LAST, "z") == 0);

    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == 0);
    const std::pair<int, int> d = f.buf.dropUpTo(0);
    TASK_CHECK(d.first == 1);
    TASK_CHECK(d.second == 1);
    TASK_CHECK(f.buf.getStartSeqNo() == 0);

    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));
    char out[8];
    SRT_MSGCTRL mc = srt_msgctrl_default;
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 3);
    TASK_CHECK(std::memcmp(out, "xyz", 3) == 0);
    TASK_CHECK(mc.msgno == 2);
    TASK_CHECK(f.buf.empty());
    return 0;
}

// The same rules apply when the circular buffer positions wrap around.
int case_deadline_ring_wrap()
{
    BufFixture f(100, 8, true); // small ring: capacity 7
    char out[8];
    SRT_MSGCTRL mc = srt_msgctrl_default;

    // Advance the ring head close to the end of the array.
    for (int i = 0; i < 6; ++i)
    {
        TASK_CHECK(put(f, 100 + i, 10 + i, PB_SOLO, "a") == 0);
        TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));
        TASK_CHECK(f.buf.readMessage(out, sizeof out, &mc) == 1);
    }
    TASK_CHECK(f.buf.getStartSeqNo() == 106);

    // Now the following packets wrap around the ring array.
    TASK_CHECK(put(f, 106, 20, PB_FIRST, "ab") == 0);
    // seq 107 (LAST of message 20) missing.
    TASK_CHECK(put(f, 108, 21, PB_FIRST, "xy") == 0);
    TASK_CHECK(put(f, 109, 21, PB_LAST, "z") == 0);

    TASK_CHECK(f.buf.getOverdueMsgDropSeqNo(T_PLAY) == 108);
    const std::pair<int, int> d = f.buf.dropUpTo(108);
    TASK_CHECK(d.first == 1);
    TASK_CHECK(d.second == 1);
    TASK_CHECK(f.buf.getStartSeqNo() == 108);

    TASK_CHECK(f.buf.isRcvDataReady(T_PLAY));
    const int n = f.buf.readMessage(out, sizeof out, &mc);
    TASK_CHECK(n == 3);
    TASK_CHECK(std::memcmp(out, "xyz", 3) == 0);
    TASK_CHECK(mc.msgno == 21);
    TASK_CHECK(f.buf.empty());
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Group "api": public API over local connections with a UDP relay.
// ---------------------------------------------------------------------------

#include <thread>
#include <mutex>
#include <chrono>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace {

// A UDP relay between the SRT sender and the SRT receiver. It forwards all
// traffic in both directions, can drop selected data packets (by ordinal of
// unique client->server data packets, retransmissions included), holds the
// dropped packets for a later explicit re-injection, and records the SRT
// header fields of every unique forwarded data packet.
class UdpRelay
{
public:
    struct Obs
    {
        uint32_t seqno;
        int      boundary;
        uint32_t msgno;
        uint32_t ts;
        size_t   len;
    };

    UdpRelay()
        : m_sock(-1)
        , m_stop(false)
        , m_client_known(false)
        , m_ordinal(0)
    {
    }

    ~UdpRelay() { stop(); }

    // Starts the relaying thread. Returns the relay port (the SRT client
    // connects here), or 0 on failure.
    uint16_t start(uint16_t server_port)
    {
        std::memset(&m_server, 0, sizeof m_server);
        m_server.sin_family      = AF_INET;
        m_server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        m_server.sin_port        = htons(server_port);

        m_sock = (int)::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_sock < 0)
            return 0;

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof addr);
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (::bind(m_sock, (struct sockaddr*)&addr, sizeof addr) != 0)
        {
            ::close(m_sock);
            m_sock = -1;
            return 0;
        }
        socklen_t alen = sizeof addr;
        if (::getsockname(m_sock, (struct sockaddr*)&addr, &alen) != 0)
        {
            ::close(m_sock);
            m_sock = -1;
            return 0;
        }

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 20000; // 20ms, just to keep the stop check responsive
        ::setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        m_thread = std::thread(&UdpRelay::run, this);
        return ntohs(addr.sin_port);
    }

    void stop()
    {
        if (m_sock < 0)
            return;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
        }
        m_thread.join();
        ::close(m_sock);
        m_sock = -1;
    }

    // Drop the ord-th unique client->server data packet (0-based), including
    // all its retransmissions. The dropped packet is held for release_held().
    void drop_ordinal(int ord)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_drop_ords.insert(ord);
    }

    // Re-inject the held (previously dropped) packets towards the server.
    void release_held()
    {
        std::vector<std::string> held;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            held.swap(m_held);
            m_held_seqnos.clear();
            m_drop_ords.clear();
        }
        for (size_t i = 0; i < held.size(); ++i)
            ::sendto(m_sock, held[i].data(), held[i].size(), 0,
                     (struct sockaddr*)&m_server, sizeof m_server);
    }

    std::vector<Obs> observed() const
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_observed;
    }

private:
    static uint32_t rd32(const char* p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return ntohl(v);
    }

    void run()
    {
        char buf[2048];
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                if (m_stop)
                    break;
            }

            sockaddr_in from;
            socklen_t   flen = sizeof from;
            const ssize_t n =
                ::recvfrom(m_sock, buf, sizeof buf, 0, (struct sockaddr*)&from, &flen);
            if (n < 0)
                continue; // receive timeout: re-check the stop flag

            if (from.sin_port == m_server.sin_port)
            {
                // server -> client
                std::lock_guard<std::mutex> lk(m_mtx);
                if (m_client_known)
                    ::sendto(m_sock, buf, n, 0, (struct sockaddr*)&m_client, sizeof m_client);
                continue;
            }

            // client -> server
            bool drop = false;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_client       = from;
                m_client_known = true;

                const uint32_t w0 = rd32(buf);
                if ((w0 & 0x80000000u) == 0 && n >= 16) // SRT data packet
                {
                    const uint32_t seq = w0 & 0x7FFFFFFFu;
                    if (m_held_seqnos.count(seq))
                    {
                        drop = true; // retransmission of a dropped packet
                    }
                    else if (!m_seen.count(seq))
                    {
                        const int ord = m_ordinal++;
                        if (m_drop_ords.count(ord))
                        {
                            m_held_seqnos.insert(seq);
                            m_held.push_back(std::string(buf, buf + n));
                            drop = true;
                        }
                        else
                        {
                            m_seen.insert(seq);
                            const uint32_t w1 = rd32(buf + 4);
                            Obs o;
                            o.seqno    = seq;
                            o.boundary = (int)(w1 >> 30);
                            o.msgno    = w1 & 0x03FFFFFFu;
                            o.ts       = rd32(buf + 8);
                            o.len      = (size_t)n - 16;
                            m_observed.push_back(o);
                        }
                    }
                }
            }
            if (!drop)
                ::sendto(m_sock, buf, n, 0, (struct sockaddr*)&m_server, sizeof m_server);
        }
    }

    int                     m_sock;
    std::thread             m_thread;
    mutable std::mutex      m_mtx;
    bool                    m_stop;
    sockaddr_in             m_server;
    sockaddr_in             m_client;
    bool                    m_client_known;
    int                     m_ordinal;
    std::set<int>           m_drop_ords;
    std::set<uint32_t>      m_seen;
    std::set<uint32_t>      m_held_seqnos;
    std::vector<std::string> m_held;
    std::vector<Obs>        m_observed;
};

uint16_t pick_port()
{
    const int s = (int)::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return 0;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    uint16_t port        = 0;
    if (::bind(s, (struct sockaddr*)&addr, sizeof addr) == 0)
    {
        socklen_t alen = sizeof addr;
        if (::getsockname(s, (struct sockaddr*)&addr, &alen) == 0)
            port = ntohs(addr.sin_port);
    }
    ::close(s);
    return port;
}

sockaddr_in loopback_addr(uint16_t port)
{
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    return addr;
}

bool set_opt(SRTSOCKET s, SRT_SOCKOPT o, int v)
{
    return srt_setsockopt(s, 0, o, &v, (int)sizeof v) != SRT_ERROR;
}

bool set_opt(SRTSOCKET s, SRT_SOCKOPT o, bool v)
{
    return srt_setsockopt(s, 0, o, &v, (int)sizeof v) != SRT_ERROR;
}

struct SrtSession
{
    int rc;
    SrtSession() : rc(srt_startup()) {}
    ~SrtSession()
    {
        if (rc == 0)
            srt_cleanup();
    }
};

// A pair of connected SRT sockets through the UDP relay.
struct LivePair
{
    SRTSOCKET listener;
    SRTSOCKET server;
    SRTSOCKET client;
    UdpRelay  relay;
    uint16_t  relay_port;

    LivePair()
        : listener(SRT_INVALID_SOCK)
        , server(SRT_INVALID_SOCK)
        , client(SRT_INVALID_SOCK)
        , relay_port(0)
    {
    }

    ~LivePair() { close(); }

    void close()
    {
        if (client != SRT_INVALID_SOCK)
            srt_close(client);
        if (server != SRT_INVALID_SOCK)
            srt_close(server);
        if (listener != SRT_INVALID_SOCK)
            srt_close(listener);
        client = server = listener = SRT_INVALID_SOCK;
        relay.stop();
    }

    // Returns 0 on success, a positive stage number on failure.
    int open(int latency_ms, bool rcv_syn, int sndbuf_bytes)
    {
        listener = srt_create_socket();
        if (listener == SRT_INVALID_SOCK)
            return 1;
        if (!set_opt(listener, SRTO_LATENCY, latency_ms))
            return 2;
        if (!set_opt(listener, SRTO_RCVSYN, rcv_syn))
            return 3;
        // Bound all blocking operations so a failure can't hang the test run.
        if (!set_opt(listener, SRTO_RCVTIMEO, 8000))
            return 3;

        const uint16_t port = pick_port();
        if (!port)
            return 4;
        sockaddr_in la = loopback_addr(port);
        if (srt_bind(listener, (struct sockaddr*)&la, (int)sizeof la) == SRT_ERROR)
            return 5;
        if (srt_listen(listener, 1) == SRT_ERROR)
            return 6;

        relay_port = relay.start(port);
        if (!relay_port)
            return 7;

        client = srt_create_socket();
        if (client == SRT_INVALID_SOCK)
            return 8;
        if (!set_opt(client, SRTO_LATENCY, latency_ms))
            return 9;
        if (!set_opt(client, SRTO_SNDTIMEO, 8000))
            return 9;
        if (sndbuf_bytes > 0 && !set_opt(client, SRTO_SNDBUF, sndbuf_bytes))
            return 10;

        sockaddr_in ra = loopback_addr(relay_port);
        if (srt_connect(client, (struct sockaddr*)&ra, (int)sizeof ra) == SRT_ERROR)
            return 11;

        for (int t = 0; t < 100; ++t)
        {
            server = srt_accept(listener, 0, 0);
            if (server != SRT_INVALID_SOCK)
                return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return 12;
    }
};

std::string make_frame(size_t size, unsigned seed)
{
    std::string frame(size, '\0');
    unsigned v = seed;
    for (size_t i = 0; i < size; ++i)
    {
        v       = v * 1103515245u + 12345u;
        frame[i] = char((v >> 16) & 0xFF);
    }
    return frame;
}

// One compressed-frame-sized message is submitted and delivered as a whole;
// the relay verifies the packet boundaries on the wire.
int case_api_roundtrip()
{
    SrtSession session;
    TASK_CHECK(session.rc == 0);

    LivePair cp;
    TASK_CHECK(cp.open(120, true, 0) == 0);

    const std::string frame = make_frame(4000, 1); // spans 3 packets
    SRT_MSGCTRL       sm    = srt_msgctrl_default;
    const int         sent  = srt_sendmsg2(cp.client, frame.data(), (int)frame.size(), &sm);
    TASK_CHECK(sent == (int)frame.size()); // the whole frame in one call
    TASK_CHECK(sm.msgno >= 1);

    char        buf[7000];
    SRT_MSGCTRL rm = srt_msgctrl_default;
    const int   rcvd = srt_recvmsg2(cp.server, buf, (int)sizeof buf, &rm);
    TASK_CHECK(rcvd == (int)frame.size()); // the whole frame in one delivery
    TASK_CHECK(std::memcmp(buf, frame.data(), frame.size()) == 0);
    TASK_CHECK(rm.msgno == sm.msgno);
    TASK_CHECK(rm.pktseq == sm.pktseq); // seqno of the first packet of the frame
    TASK_CHECK(rm.srctime != 0);

    // Packet boundaries, message number and source time on the wire.
    const std::vector<UdpRelay::Obs> obs = cp.relay.observed();
    TASK_CHECK(obs.size() == 3);
    if (obs.size() == 3)
    {
        TASK_CHECK(obs[0].boundary == 2); // PB_FIRST
        TASK_CHECK(obs[1].boundary == 0); // PB_SUBSEQUENT
        TASK_CHECK(obs[2].boundary == 1); // PB_LAST
        TASK_CHECK((int)obs[0].msgno == sm.msgno);
        TASK_CHECK(obs[0].msgno == obs[1].msgno && obs[1].msgno == obs[2].msgno);
        TASK_CHECK(obs[0].ts == obs[1].ts && obs[1].ts == obs[2].ts); // one source time
        TASK_CHECK(obs[0].len + obs[1].len + obs[2].len == frame.size());
        TASK_CHECK(obs[0].len <= 1456 && obs[1].len <= 1456 && obs[2].len <= 1456);
        // consecutive 31-bit sequence numbers (wrap-safe)
        TASK_CHECK(((obs[1].seqno - obs[0].seqno) & 0x7FFFFFFFu) == 1);
        TASK_CHECK(((obs[2].seqno - obs[0].seqno) & 0x7FFFFFFFu) == 2);
        TASK_CHECK((int)obs[0].seqno == sm.pktseq);
    }

    // The successor message keeps working and gets the next message number.
    const std::string next = make_frame(100, 2);
    SRT_MSGCTRL       sm2  = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, next.data(), (int)next.size(), &sm2) == (int)next.size());
    SRT_MSGCTRL rm2 = srt_msgctrl_default;
    const int   rcvd2 = srt_recvmsg2(cp.server, buf, (int)sizeof buf, &rm2);
    TASK_CHECK(rcvd2 == (int)next.size());
    TASK_CHECK(std::memcmp(buf, next.data(), next.size()) == 0);
    TASK_CHECK(rm2.msgno == rm.msgno + 1);

    cp.close();
    return 0;
}

// A receive buffer that satisfies the single-packet minimum but not the
// complete message yields SRT_ELARGEMSG without consuming the message and
// without clearing the epoll read state; a larger buffer re-reads it.
int case_api_short_buffer_epoll()
{
    SrtSession session;
    TASK_CHECK(session.rc == 0);

    LivePair cp;
    TASK_CHECK(cp.open(120, false, 0) == 0); // asynchronous receiving

    const int eid = srt_epoll_create();
    TASK_CHECK(eid >= 0);
    int ev = SRT_EPOLL_IN;
    TASK_CHECK(srt_epoll_add_usock(eid, cp.server, &ev) == 0);

    const std::string frame = make_frame(4000, 3);
    SRT_MSGCTRL       sm    = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, frame.data(), (int)frame.size(), &sm) == (int)frame.size());

    SRTSOCKET rfds[2];
    int       rnum = 2;
    TASK_CHECK(srt_epoll_wait(eid, rfds, &rnum, 0, 0, 5000, 0, 0, 0, 0) > 0);
    TASK_CHECK(rnum >= 1 && rfds[0] == cp.server);

    // 2000 bytes: above the single-packet minimum, below the whole message.
    char small[2000];
    std::memset(small, 0xAB, sizeof small);
    SRT_MSGCTRL rm = srt_msgctrl_default;
    rm.msgno       = 7777; // sentinels: must survive the failed read
    rm.pktseq      = 6666;
    TASK_CHECK(srt_recvmsg2(cp.server, small, (int)sizeof small, &rm) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(0) == SRT_ELARGEMSG);
    TASK_CHECK(small[0] == (char)0xAB && small[sizeof small - 1] == (char)0xAB);
    TASK_CHECK(rm.msgno == 7777 && rm.pktseq == 6666);

    // The size error must not clear the read-ready state.
    rnum = 2;
    TASK_CHECK(srt_epoll_wait(eid, rfds, &rnum, 0, 0, 2000, 0, 0, 0, 0) > 0);

    // Re-reading with a large buffer returns the same bytes and message number.
    char        big[7000];
    SRT_MSGCTRL rm2 = srt_msgctrl_default;
    const int   rcvd = srt_recvmsg2(cp.server, big, (int)sizeof big, &rm2);
    TASK_CHECK(rcvd == (int)frame.size());
    TASK_CHECK(std::memcmp(big, frame.data(), frame.size()) == 0);
    TASK_CHECK(rm2.msgno == sm.msgno);
    TASK_CHECK(rm2.pktseq == sm.pktseq);

    // The frame was consumed exactly once: nothing more to read now.
    TASK_CHECK(srt_recvmsg2(cp.server, big, (int)sizeof big, &rm2) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(0) == SRT_EASYNCRCV);

    srt_epoll_release(eid);
    cp.close();
    return 0;
}

// The same size error in synchronous receive mode.
int case_api_short_buffer_sync()
{
    SrtSession session;
    TASK_CHECK(session.rc == 0);

    LivePair cp;
    TASK_CHECK(cp.open(120, true, 0) == 0);
    TASK_CHECK(set_opt(cp.server, SRTO_RCVTIMEO, 5000));

    const std::string frame = make_frame(3000, 4);
    SRT_MSGCTRL       sm    = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, frame.data(), (int)frame.size(), &sm) == (int)frame.size());

    char small[1600]; // above the single-packet minimum, below the message
    std::memset(small, 0xCD, sizeof small);
    SRT_MSGCTRL rm = srt_msgctrl_default;
    rm.msgno       = 5555;
    TASK_CHECK(srt_recvmsg2(cp.server, small, (int)sizeof small, &rm) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(0) == SRT_ELARGEMSG);
    TASK_CHECK(small[0] == (char)0xCD && small[sizeof small - 1] == (char)0xCD);
    TASK_CHECK(rm.msgno == 5555);

    char        big[7000];
    SRT_MSGCTRL rm2 = srt_msgctrl_default;
    const int   rcvd = srt_recvmsg2(cp.server, big, (int)sizeof big, &rm2);
    TASK_CHECK(rcvd == (int)frame.size());
    TASK_CHECK(std::memcmp(big, frame.data(), frame.size()) == 0);
    TASK_CHECK(rm2.msgno == sm.msgno);

    cp.close();
    return 0;
}

// A message exceeding the whole sending buffer is still rejected, while a
// multi-packet message that fits goes through.
int case_api_send_buffer_limit()
{
    SrtSession session;
    TASK_CHECK(session.rc == 0);

    LivePair cp;
    TASK_CHECK(cp.open(120, true, 7 * 1456) == 0); // send buffer for ~6-7 packets

    const std::string huge(20000, 'H');
    SRT_MSGCTRL       sm = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, huge.data(), (int)huge.size(), &sm) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(0) == SRT_ELARGEMSG);

    const std::string frame = make_frame(4000, 5);
    TASK_CHECK(srt_sendmsg2(cp.client, frame.data(), (int)frame.size(), &sm) == (int)frame.size());

    char        buf[7000];
    SRT_MSGCTRL rm = srt_msgctrl_default;
    const int   rcvd = srt_recvmsg2(cp.server, buf, (int)sizeof buf, &rm);
    TASK_CHECK(rcvd == (int)frame.size());
    TASK_CHECK(std::memcmp(buf, frame.data(), frame.size()) == 0);

    cp.close();
    return 0;
}

// TLPKTDROP: a frame with a missing LAST fragment is dropped as a whole at
// its play time once the next frame's FIRST is seen; a late fragment must
// not resurrect it. The drop is visible in the receiver statistics.
int case_api_tlpktdrop()
{
    SrtSession session;
    TASK_CHECK(session.rc == 0);

    LivePair cp;
    TASK_CHECK(cp.open(300, true, 0) == 0);
    TASK_CHECK(set_opt(cp.server, SRTO_RCVTIMEO, 8000));

    // Drop the LAST packet of the first frame (3rd data packet, ordinal 2).
    cp.relay.drop_ordinal(2);

    const std::string f1 = make_frame(4000, 6);
    const std::string f2 = make_frame(100, 7);
    const std::string f3 = make_frame(50, 8);

    SRT_MSGCTRL sm1 = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, f1.data(), (int)f1.size(), &sm1) == (int)f1.size());
    SRT_MSGCTRL sm2 = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, f2.data(), (int)f2.size(), &sm2) == (int)f2.size());

    // Frame 1 stays incomplete; at its play time it is skipped as a whole,
    // so the first delivered message is frame 2.
    char        buf[7000];
    SRT_MSGCTRL rm = srt_msgctrl_default;
    const int   rcvd = srt_recvmsg2(cp.server, buf, (int)sizeof buf, &rm);
    TASK_CHECK(rcvd == (int)f2.size());
    TASK_CHECK(std::memcmp(buf, f2.data(), f2.size()) == 0);
    TASK_CHECK(rm.msgno == sm2.msgno);

    // The missing fragment arrives late: frame 1 must not be resurrected.
    cp.relay.release_held();

    SRT_MSGCTRL sm3 = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, f3.data(), (int)f3.size(), &sm3) == (int)f3.size());
    SRT_MSGCTRL rm3 = srt_msgctrl_default;
    const int   rcvd3 = srt_recvmsg2(cp.server, buf, (int)sizeof buf, &rm3);
    TASK_CHECK(rcvd3 == (int)f3.size());
    TASK_CHECK(std::memcmp(buf, f3.data(), f3.size()) == 0);
    TASK_CHECK(rm3.msgno == sm3.msgno);

    // The whole 3-packet frame was counted as dropped by the receiver.
    SRT_TRACEBSTATS st;
    TASK_CHECK(srt_bstats(cp.server, &st, 0) == 0);
    TASK_CHECK(st.pktRcvDropTotal >= 3);

    cp.close();
    return 0;
}

// A fragment re-injected before the play deadline repairs the frame:
// it is then delivered whole, not dropped.
int case_api_repair_before_deadline()
{
    SrtSession session;
    TASK_CHECK(session.rc == 0);

    LivePair cp;
    TASK_CHECK(cp.open(300, true, 0) == 0);
    TASK_CHECK(set_opt(cp.server, SRTO_RCVTIMEO, 8000));

    // Drop the middle packet of the frame (ordinal 1 of 0,1,2).
    cp.relay.drop_ordinal(1);

    const std::string f1 = make_frame(4000, 9);
    SRT_MSGCTRL       sm1 = srt_msgctrl_default;
    TASK_CHECK(srt_sendmsg2(cp.client, f1.data(), (int)f1.size(), &sm1) == (int)f1.size());

    // Re-inject the missing fragment well before the 300ms play deadline.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cp.relay.release_held();

    char        buf[7000];
    SRT_MSGCTRL rm = srt_msgctrl_default;
    const int   rcvd = srt_recvmsg2(cp.server, buf, (int)sizeof buf, &rm);
    TASK_CHECK(rcvd == (int)f1.size());
    TASK_CHECK(std::memcmp(buf, f1.data(), f1.size()) == 0);
    TASK_CHECK(rm.msgno == sm1.msgno);

    cp.close();
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"message", "whole_delivery", case_message_whole_delivery},
    {"message", "short_buffer_reread", case_message_short_buffer_reread},
    {"message", "no_concat_next_frame", case_message_no_concat_next_frame},
    {"message", "stream_truncation_kept", case_message_stream_truncation_kept},
    {"message", "nonlive_truncation_kept", case_message_nonlive_truncation_kept},
    {"deadline", "middle_missing", case_deadline_middle_missing},
    {"deadline", "first_lost_tail_present", case_deadline_first_lost_tail_present},
    {"deadline", "last_lost_next_started", case_deadline_last_lost_next_started},
    {"deadline", "whole_frame_missing", case_deadline_whole_frame_missing},
    {"deadline", "undetermined_end_waits", case_deadline_undetermined_end_waits},
    {"deadline", "boundary_from_next_first", case_deadline_boundary_from_next_first},
    {"deadline", "completed_frame_not_dropped", case_deadline_completed_frame_not_dropped},
    {"deadline", "seqno_wrap", case_deadline_seqno_wrap},
    {"deadline", "ring_wrap", case_deadline_ring_wrap},
    {"api", "roundtrip", case_api_roundtrip},
    {"api", "short_buffer_epoll", case_api_short_buffer_epoll},
    {"api", "short_buffer_sync", case_api_short_buffer_sync},
    {"api", "send_buffer_limit", case_api_send_buffer_limit},
    {"api", "tlpktdrop", case_api_tlpktdrop},
    {"api", "repair_before_deadline", case_api_repair_before_deadline},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
