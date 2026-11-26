#include "../task_support/check.h"

#include "srt.h"
#include "buffer_snd.h"
#include "common.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using srt::CSeqNo;
using srt::CPacket;
using srt::CSndBuffer;
typedef srt::sync::steady_clock::time_point TestTimePoint;

namespace {

std::vector<char> make_bytes(size_t n, unsigned seed)
{
    std::vector<char> data(n);
    for (size_t i = 0; i < n; ++i)
        data[i] = char((i * 131 + seed * 17 + (i >> 3) * 7) & 0xff);
    return data;
}

void add_controlled(CSndBuffer& buf, int32_t msgno, int32_t seq, const std::vector<char>& data)
{
    SRT_MSGCTRL mc = srt_msgctrl_default;
    mc.msgno = msgno;
    mc.pktseq = seq;
    mc.msgttl = SRT_MSGTTL_INF;
    buf.addBuffer(data.data(), static_cast<int>(data.size()), mc);
}

int rexmit_read(CSndBuffer& buf, int offset, int32_t seq, CPacket& pkt, CSndBuffer::DropRange& drop)
{
    pkt.set_seqno(seq);
    TestTimePoint origin;
    return buf.readData(offset, pkt, origin, drop);
}

bool bytes_equal(const void* actual, int actual_len, const std::vector<char>& expected)
{
    return actual_len == static_cast<int>(expected.size())
        && std::memcmp(actual, expected.data(), expected.size()) == 0;
}

uint32_t wire_be32(const unsigned char* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

int32_t wire_be32s(const unsigned char* p) { return static_cast<int32_t>(wire_be32(p)); }

void expand_nak(const unsigned char* payload, size_t len, std::vector<int32_t>& out)
{
    const size_t words = len / sizeof(int32_t);
    for (size_t i = 0; i < words; ++i)
    {
        uint32_t word = wire_be32(payload + i * 4);
        if (word & 0x80000000u)
        {
            if (i + 1 >= words)
                break;
            const int32_t lo = static_cast<int32_t>(word & 0x7fffffffu);
            const int32_t hi = wire_be32s(payload + (i + 1) * 4);
            const int span = CSeqNo::seqlen(lo, hi);
            for (int j = 0; j < span && j < 100000; ++j)
                out.push_back(CSeqNo::incseq(lo, j));
            ++i;
        }
        else
        {
            out.push_back(static_cast<int32_t>(word));
        }
    }
}

struct DropReqInfo
{
    int32_t msgno = 0;
    int32_t lo = 0;
    int32_t hi = 0;
};

class UdpForwarder
{
public:
    UdpForwarder() = default;
    ~UdpForwarder() { stop(); }

    int start(const sockaddr_in& server)
    {
        m_server = server;
        m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_fd < 0)
            return -1;

        int yes = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr_addr_loopback(addr);
        addr.sin_port = 0;
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
        {
            ::close(m_fd);
            m_fd = -1;
            return -2;
        }

        socklen_t alen = sizeof addr;
        if (::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &alen) != 0)
        {
            ::close(m_fd);
            m_fd = -1;
            return -3;
        }
        m_port = ntohs(addr.sin_port);
        m_running = true;
        m_thread = std::thread(&UdpForwarder::run, this);
        return 0;
    }

    void stop()
    {
        if (!m_running.exchange(false))
            return;
        if (m_thread.joinable())
            m_thread.join();
        if (m_fd >= 0)
        {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    sockaddr_in address() const
    {
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(m_port);
        return addr;
    }

    void arm(int32_t target_msgno, int32_t prev_msgno = -1, int32_t next_msgno = -1,
             bool drop_first_dropreq = true, bool hold_naks = true)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_targetMsgno = target_msgno;
        m_prevMsgno = prev_msgno;
        m_nextMsgno = next_msgno;
        m_dropFirstDropReq = drop_first_dropreq;
        m_holdNaks = hold_naks;
        m_dropReqSeen = m_dropReqDropped = m_dropReqForwarded = 0;
        m_targetDataSeen = m_adjacentOriginalDropped = 0;
        m_droppedDropReqs.clear();
        m_forwardedDropReqs.clear();
        m_adjacentSeenSeqs.clear();
        m_nakCount = 0;
        m_nakSeqs.clear();
        m_latestNak.clear();
        m_nakReleased = false;
        m_targetDataAfterNak = 0;
    }

    bool wait_target_data(uint64_t count, int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [&] { return m_targetDataSeen >= count; });
    }

    bool wait_dropped_dropreq(int32_t msgno, int32_t lo, int32_t hi, int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return contains_drop(m_droppedDropReqs, msgno, lo, hi);
        });
    }

    bool wait_forwarded_dropreq(int32_t msgno, int32_t lo, int32_t hi, int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            return contains_drop(m_forwardedDropReqs, msgno, lo, hi);
        });
    }

    bool wait_nak_covers(const std::vector<int32_t>& seqs, int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
            for (size_t i = 0; i < seqs.size(); ++i)
            {
                if (std::find(m_nakSeqs.begin(), m_nakSeqs.end(), seqs[i]) == m_nakSeqs.end())
                    return false;
            }
            return !seqs.empty();
        });
    }

    bool release_latest_nak()
    {
        std::vector<unsigned char> packet;
        sockaddr_in to {};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_latestNak.empty() || !m_hasClient)
                return false;
            packet = m_latestNak;
            to = m_client;
            m_nakReleased = true;
        }
        return ::sendto(m_fd, packet.data(), packet.size(), 0,
                        reinterpret_cast<sockaddr*>(&to), sizeof to)
            == static_cast<ssize_t>(packet.size());
    }

    uint64_t target_data_after_nak()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_targetDataAfterNak;
    }

private:
    static void addr_addr_loopback(sockaddr_in& addr)
    {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    static bool contains_drop(const std::vector<DropReqInfo>& list, int32_t msgno, int32_t lo, int32_t hi)
    {
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].msgno == msgno && list[i].lo == lo && list[i].hi == hi)
                return true;
        }
        return false;
    }

    void forward(const unsigned char* packet, ssize_t len, const sockaddr_in& to)
    {
        ::sendto(m_fd, packet, static_cast<size_t>(len), 0,
                 reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    void run()
    {
        unsigned char buf[65536];
        while (m_running.load())
        {
            pollfd pfd;
            pfd.fd = m_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (::poll(&pfd, 1, 25) <= 0)
                continue;

            sockaddr_in src {};
            socklen_t srclen = sizeof src;
            ssize_t len = ::recvfrom(m_fd, buf, sizeof buf, 0,
                                     reinterpret_cast<sockaddr*>(&src), &srclen);
            if (len <= 0)
                continue;

            const bool from_server = src.sin_port == m_server.sin_port
                && src.sin_addr.s_addr == m_server.sin_addr.s_addr;

            if (!from_server)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_client = src;
                m_hasClient = true;
            }

            bool control = len >= 16 && (wire_be32(buf) & 0x80000000u) != 0;
            if (control)
            {
                const int type = static_cast<int>((wire_be32(buf) >> 16) & 0x7fffu);
                if (type == 7 && len >= 28 && !from_server)
                {
                    DropReqInfo drop;
                    drop.msgno = wire_be32s(buf + 4);
                    drop.lo = wire_be32s(buf + 16);
                    drop.hi = wire_be32s(buf + 20);

                    bool swallow = false;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        ++m_dropReqSeen;
                        if (m_dropFirstDropReq && m_dropReqDropped == 0)
                        {
                            ++m_dropReqDropped;
                            m_droppedDropReqs.push_back(drop);
                            swallow = true;
                        }
                        else
                        {
                            ++m_dropReqForwarded;
                            m_forwardedDropReqs.push_back(drop);
                        }
                        m_cv.notify_all();
                    }
                    if (swallow)
                        continue;
                }
                else if (type == 3 && from_server && len >= 16)
                {
                    std::vector<int32_t> seqs;
                    expand_nak(buf + 16, static_cast<size_t>(len - 16), seqs);
                    bool hold;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        ++m_nakCount;
                        m_nakSeqs = seqs;
                        m_latestNak.assign(buf, buf + len);
                        hold = m_holdNaks;
                        m_cv.notify_all();
                    }
                    if (hold)
                        continue;
                }
            }
            else if (!from_server && len >= 8)
            {
                const uint32_t seq_word = wire_be32(buf);
                const int32_t seq = static_cast<int32_t>(seq_word & 0x7fffffffu);
                const int32_t msgno = wire_be32s(buf + 4) & 0x01ffffff;

                std::lock_guard<std::mutex> lock(m_mutex);
                if (msgno == m_targetMsgno)
                {
                    ++m_targetDataSeen;
                    if (m_nakReleased)
                        ++m_targetDataAfterNak;
                    m_cv.notify_all();
                    continue;
                }

                if (msgno == m_prevMsgno || msgno == m_nextMsgno)
                {
                    if (m_adjacentSeenSeqs.insert(seq).second)
                    {
                        ++m_adjacentOriginalDropped;
                        m_cv.notify_all();
                        continue;
                    }
                }
            }

            if (from_server)
            {
                sockaddr_in client;
                bool has_client;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    client = m_client;
                    has_client = m_hasClient;
                }
                if (has_client)
                    forward(buf, len, client);
            }
            else
            {
                forward(buf, len, m_server);
            }
        }
    }

    int m_fd = -1;
    uint16_t m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_running { false };

    sockaddr_in m_server {};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    sockaddr_in m_client {};
    bool m_hasClient = false;

    int32_t m_targetMsgno = -1;
    int32_t m_prevMsgno = -1;
    int32_t m_nextMsgno = -1;
    bool m_dropFirstDropReq = true;
    bool m_holdNaks = true;

    uint64_t m_dropReqSeen = 0;
    uint64_t m_dropReqDropped = 0;
    uint64_t m_dropReqForwarded = 0;
    uint64_t m_targetDataSeen = 0;
    uint64_t m_adjacentOriginalDropped = 0;
    int m_nakCount = 0;
    bool m_nakReleased = false;
    uint64_t m_targetDataAfterNak = 0;
    std::vector<int32_t> m_nakSeqs;
    std::vector<unsigned char> m_latestNak;
    std::vector<DropReqInfo> m_droppedDropReqs;
    std::vector<DropReqInfo> m_forwardedDropReqs;
    std::set<int32_t> m_adjacentSeenSeqs;
};

struct SrtNetwork
{
    SRTSOCKET listener = SRT_INVALID_SOCK;
    SRTSOCKET sender = SRT_INVALID_SOCK;
    SRTSOCKET receiver = SRT_INVALID_SOCK;
    UdpForwarder proxy;
    bool started = false;

    ~SrtNetwork()
    {
        if (receiver != SRT_INVALID_SOCK)
            srt_close(receiver);
        if (sender != SRT_INVALID_SOCK)
            srt_close(sender);
        if (listener != SRT_INVALID_SOCK)
            srt_close(listener);
        proxy.stop();
        if (started)
            srt_cleanup();
    }

    static int configure(SRTSOCKET sock, bool message_api, int timeout_ms = 5000)
    {
        SRT_TRANSTYPE tt = SRTT_FILE;
        int yes = 1;
        int no = 0;
        return srt_setsockflag(sock, SRTO_TRANSTYPE, &tt, sizeof tt) == 0
            && srt_setsockflag(sock, SRTO_MESSAGEAPI, &message_api, sizeof message_api) == 0
            && srt_setsockflag(sock, SRTO_CONNTIMEO, &timeout_ms, sizeof timeout_ms) == 0
            && srt_setsockflag(sock, SRTO_SNDTIMEO, &timeout_ms, sizeof timeout_ms) == 0
            && srt_setsockflag(sock, SRTO_RCVTIMEO, &timeout_ms, sizeof timeout_ms) == 0
            && srt_setsockflag(sock, SRTO_NAKREPORT, &yes, sizeof yes) == 0
            && srt_setsockflag(sock, SRTO_TLPKTDROP, &no, sizeof no) == 0;
    }

    int setup()
    {
        TASK_CHECK(srt_startup() == 0);
        started = true;

        listener = srt_create_socket();
        sender = srt_create_socket();
        TASK_CHECK(listener != SRT_INVALID_SOCK && sender != SRT_INVALID_SOCK);
        TASK_CHECK(configure(listener, true));
        TASK_CHECK(configure(sender, true));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TASK_CHECK(srt_bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        TASK_CHECK(srt_listen(listener, 2) == 0);

        int addrlen = sizeof addr;
        TASK_CHECK(srt_getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0);
        TASK_CHECK(proxy.start(addr) == 0);

        sockaddr_in proxy_addr = proxy.address();
        TASK_CHECK(srt_connect(sender, reinterpret_cast<sockaddr*>(&proxy_addr), sizeof proxy_addr) == 0);
        receiver = srt_accept(listener, NULL, NULL);
        TASK_CHECK(receiver != SRT_INVALID_SOCK);
        // PRE/PREBIND options set on the listener are inherited by the
        // accepted socket; only the receive timeout is changed afterwards.
        int rcv_timeout = 5000;
        TASK_CHECK(srt_setsockflag(receiver, SRTO_RCVTIMEO, &rcv_timeout, sizeof rcv_timeout) == 0);
        return 0;
    }

    int payload_size() const
    {
        int size = 0;
        int len = sizeof size;
        if (srt_getsockflag(sender, SRTO_PAYLOADSIZE, &size, &len) != 0 || size <= 0)
            size = 1456;
        return size;
    }
};

int send_controlled(SRTSOCKET sock, int32_t msgno, const std::vector<char>& data, SRT_MSGCTRL& mc)
{
    mc = srt_msgctrl_default;
    mc.msgno = msgno;
    mc.inorder = 1;
    return srt_sendmsg2(sock, data.data(), static_cast<int>(data.size()), &mc);
}

int receive_compare(SRTSOCKET sock, int32_t msgno, const std::vector<char>& expected)
{
    std::vector<char> buffer(expected.size() + 64);
    SRT_MSGCTRL mc = srt_msgctrl_default;
    int n = srt_recvmsg2(sock, buffer.data(), static_cast<int>(buffer.size()), &mc);
    TASK_CHECK(n == static_cast<int>(expected.size()));
    TASK_CHECK(mc.msgno == msgno);
    TASK_CHECK(bytes_equal(buffer.data(), n, expected));
    return 0;
}

bool wait_sndbuf_drained(SRTSOCKET sock, int timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        size_t blocks = 999;
        size_t bytes = 999;
        if (srt_getsndbuffer(sock, &blocks, &bytes) == 0 && blocks == 0 && bytes == 0)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

// Buffer group ----------------------------------------------------------------

static int case_buffer_partial_ack_wrap(void)
{
    CSndBuffer buf(AF_INET, 64, 188, 0);
    const std::vector<char> victim = make_bytes(400, 11); // 188 + 188 + 24
    const std::vector<char> after = make_bytes(100, 12);

    add_controlled(buf, 7101, 0x7ffffffe, victim);
    add_controlled(buf, 7102, 1, after);
    TASK_CHECK(buf.getCurrBufSize() == 4);

    buf.ackData(1); // first victim fragment is already ACK-reclaimed.
    TASK_CHECK(buf.getCurrBufSize() == 3);

    CSndBuffer::DropRange range;
    TASK_CHECK(buf.dropMessage(7101, range) == 212);
    TASK_CHECK(range.msgno == 7101);
    TASK_CHECK(range.seqno[0] == 0x7fffffff);
    TASK_CHECK(range.seqno[1] == 0); // 31-bit sequence wrap.
    TASK_CHECK(buf.getCurrBufSize() == 3); // revocation is not an ACK/reclaim.
    TASK_CHECK(buf.dropMessage(7101, range) == 0);

    CPacket pkt;
    CSndBuffer::DropRange nak;
    TASK_CHECK(rexmit_read(buf, 0, 0x7fffffff, pkt, nak) == CSndBuffer::READ_DROP);
    TASK_CHECK(nak.seqno[0] == 0x7fffffff && nak.seqno[1] == 0);

    TASK_CHECK(rexmit_read(buf, 1, 0, pkt, nak) == CSndBuffer::READ_DROP);
    TASK_CHECK(nak.seqno[0] == 0 && nak.seqno[1] == 0);

    TASK_CHECK(rexmit_read(buf, 2, 1, pkt, nak) == static_cast<int>(after.size()));
    TASK_CHECK(bytes_equal(pkt.data(), static_cast<int>(after.size()), after));
    return 0;
}

static int case_buffer_nak_neighbors_reclaim(void)
{
    CSndBuffer buf(AF_INET, 64, 188, 0);
    const std::vector<char> before = make_bytes(90, 21);
    const std::vector<char> victim = make_bytes(400, 22);
    const std::vector<char> after = make_bytes(130, 23);

    add_controlled(buf, 7201, 100, before); // seq 100
    add_controlled(buf, 7202, 101, victim); // 101,102,103
    add_controlled(buf, 7203, 104, after);  // 104
    TASK_CHECK(buf.getCurrBufSize() == 5);

    CSndBuffer::DropRange range;
    TASK_CHECK(buf.dropMessage(7202, range) == static_cast<int>(victim.size()));
    TASK_CHECK(range.seqno[0] == 101 && range.seqno[1] == 103);

    CPacket pkt;
    CSndBuffer::DropRange nak;

    // A NAK list is processed in sequence order: before, victim ranges, after.
    TASK_CHECK(rexmit_read(buf, 0, 100, pkt, nak) == static_cast<int>(before.size()));
    TASK_CHECK(bytes_equal(pkt.data(), static_cast<int>(before.size()), before));

    TASK_CHECK(rexmit_read(buf, 1, 101, pkt, nak) == CSndBuffer::READ_DROP);
    TASK_CHECK(nak.msgno == 7202 && nak.seqno[0] == 101 && nak.seqno[1] == 103);

    // A NAK that starts in the middle still gets a drop answer, never payload.
    TASK_CHECK(rexmit_read(buf, 2, 102, pkt, nak) == CSndBuffer::READ_DROP);
    TASK_CHECK(nak.seqno[0] == 102 && nak.seqno[1] == 103);

    TASK_CHECK(rexmit_read(buf, 4, 104, pkt, nak) == static_cast<int>(after.size()));
    TASK_CHECK(bytes_equal(pkt.data(), static_cast<int>(after.size()), after));

    // A real cumulative ACK reclaims everything; only then is the msgno absent.
    buf.ackData(5);
    TASK_CHECK(buf.getCurrBufSize() == 0);
    TASK_CHECK(buf.dropMessage(7202, range) == 0);
    return 0;
}

static int case_buffer_unsent_tail_skipped(void)
{
    CSndBuffer buf(AF_INET, 64, 188, 0);
    const std::vector<char> victim = make_bytes(400, 31);
    const std::vector<char> after = make_bytes(77, 32);
    add_controlled(buf, 7301, 0x7ffffffe, victim);

    CPacket pkt;
    TestTimePoint origin;
    int skip = 0;
    TASK_CHECK(buf.readData(pkt, origin, 0, skip) == 188);
    TASK_CHECK(skip == 0);
    TASK_CHECK(pkt.seqno() == 0x7ffffffe);

    CSndBuffer::DropRange range;
    TASK_CHECK(buf.dropMessage(7301, range) == static_cast<int>(victim.size()));
    TASK_CHECK(range.seqno[0] == 0x7ffffffe && range.seqno[1] == 0);

    // The second and third fragments were never originally extracted.
    TASK_CHECK(buf.readData(pkt, origin, 0, skip) == CSndBuffer::READ_NONE);
    TASK_CHECK(skip == 2);

    // The scheduler/circular block continues with the next message.
    add_controlled(buf, 7302, 1, after);
    TASK_CHECK(buf.readData(pkt, origin, 0, skip) == static_cast<int>(after.size()));
    TASK_CHECK(skip == 0);
    TASK_CHECK(pkt.seqno() == 1);
    TASK_CHECK(bytes_equal(pkt.data(), static_cast<int>(after.size()), after));
    return 0;
}

// Wire group ------------------------------------------------------------------

static int case_buffer_concurrent_drop_ack(void)
{
    const int messages = 200;
    const int worker_count = 8;
    CSndBuffer buf(AF_INET, 512, 188, 0);

    for (int i = 0; i < messages; ++i)
    {
        const std::vector<char> data = make_bytes(10, static_cast<unsigned>(i));
        add_controlled(buf, 8000 + i, 4000 + i, data);
    }
    TASK_CHECK(buf.getCurrBufSize() == messages);

    std::vector<std::atomic<int>> winners(messages);
    std::vector<std::atomic<int>> bad_results(messages);
    for (int i = 0; i < messages; ++i)
    {
        winners[i].store(0);
        bad_results[i].store(0);
    }

    std::atomic<bool> start(false);
    std::vector<std::thread> workers;
    for (int w = 0; w < worker_count; ++w)
    {
        workers.emplace_back([&] {
            while (!start.load())
                std::this_thread::yield();

            for (int i = 0; i < messages; ++i)
            {
                CSndBuffer::DropRange range;
                const int result = buf.dropMessage(8000 + i, range);
                if (result == 10)
                    ++winners[i];
                else if (result != 0)
                    ++bad_results[i];
            }
        });
    }

    std::thread acker([&] {
        while (!start.load())
            std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        buf.ackData(messages);
    });

    start.store(true);
    for (size_t i = 0; i < workers.size(); ++i)
        workers[i].join();
    acker.join();

    for (int i = 0; i < messages; ++i)
    {
        TASK_CHECK(bad_results[i].load() == 0);
        TASK_CHECK(winners[i].load() <= 1);
        CSndBuffer::DropRange range;
        TASK_CHECK(buf.dropMessage(8000 + i, range) == 0);
    }
    TASK_CHECK(buf.getCurrBufSize() == 0);
    return 0;
}

// Wire group ------------------------------------------------------------------

static int case_wire_real_dropreq_and_successor(void)
{
    SrtNetwork net;
    TASK_CHECK(net.setup() == 0);
    net.proxy.arm(8102, -1, -1, false, false);

    const std::vector<char> victim = make_bytes(4096, 41);
    SRT_MSGCTRL vmc;
    TASK_CHECK(send_controlled(net.sender, 8102, victim, vmc) == static_cast<int>(victim.size()));

    const int payload_size = net.payload_size();
    const int frags = (static_cast<int>(victim.size()) + payload_size - 1) / payload_size;
    TASK_CHECK(frags >= 2);
    TASK_CHECK(net.proxy.wait_target_data(static_cast<uint64_t>(frags)));

    TASK_CHECK(srt_dropmsg(net.sender, 8102) == static_cast<int>(victim.size()));
    const int32_t hi = CSeqNo::incseq(vmc.pktseq, frags - 1);
    TASK_CHECK(net.proxy.wait_forwarded_dropreq(8102, vmc.pktseq, hi));
    TASK_CHECK(srt_dropmsg(net.sender, 8102) == 0);

    const std::vector<char> successor = make_bytes(257, 42);
    SRT_MSGCTRL smc;
    TASK_CHECK(send_controlled(net.sender, 8199, successor, smc) == static_cast<int>(successor.size()));
    TASK_CHECK(receive_compare(net.receiver, 8199, successor) == 0);
    TASK_CHECK(wait_sndbuf_drained(net.sender));
    return 0;
}

// Recovery group --------------------------------------------------------------

static int case_recovery_lost_dropreq_nak_neighbors(void)
{
    SrtNetwork net;
    TASK_CHECK(net.setup() == 0);
    net.proxy.arm(9102, 9101, 9103, true, true);

    const std::vector<char> before = make_bytes(97, 51);
    const std::vector<char> victim = make_bytes(5000, 52);
    const std::vector<char> after = make_bytes(131, 53);
    const std::vector<char> marker = make_bytes(300, 54);

    SRT_MSGCTRL bmc, vmc, amc, mmc;
    TASK_CHECK(send_controlled(net.sender, 9101, before, bmc) == static_cast<int>(before.size()));
    TASK_CHECK(send_controlled(net.sender, 9102, victim, vmc) == static_cast<int>(victim.size()));
    TASK_CHECK(send_controlled(net.sender, 9103, after, amc) == static_cast<int>(after.size()));
    // This forwarded later packet makes the receiver observe the whole gap and NAK it.
    TASK_CHECK(send_controlled(net.sender, 9104, marker, mmc) == static_cast<int>(marker.size()));

    const int frags = CSeqNo::seqoff(vmc.pktseq, amc.pktseq);
    TASK_CHECK(frags >= 3);
    TASK_CHECK(net.proxy.wait_target_data(static_cast<uint64_t>(frags)));
    TASK_CHECK(net.proxy.wait_nak_covers(std::vector<int32_t> { bmc.pktseq, vmc.pktseq, amc.pktseq }));

    const int32_t victim_hi = CSeqNo::decseq(amc.pktseq);
    TASK_CHECK(victim_hi == CSeqNo::incseq(vmc.pktseq, frags - 1));

    TASK_CHECK(srt_dropmsg(net.sender, 9102) == static_cast<int>(victim.size()));
    TASK_CHECK(net.proxy.wait_dropped_dropreq(9102, vmc.pktseq, victim_hi));

    // The first DROPREQ was lost by the forwarder. Release the observed NAK:
    // the sender must answer with another DROPREQ, not victim payload.
    TASK_CHECK(net.proxy.release_latest_nak());
    TASK_CHECK(net.proxy.wait_forwarded_dropreq(9102, vmc.pktseq, victim_hi));

    TASK_CHECK(receive_compare(net.receiver, 9101, before) == 0);
    TASK_CHECK(receive_compare(net.receiver, 9103, after) == 0);
    TASK_CHECK(receive_compare(net.receiver, 9104, marker) == 0);

    const std::vector<char> last = make_bytes(222, 55);
    SRT_MSGCTRL lmc;
    TASK_CHECK(send_controlled(net.sender, 9105, last, lmc) == static_cast<int>(last.size()));
    TASK_CHECK(receive_compare(net.receiver, 9105, last) == 0);
    TASK_CHECK(wait_sndbuf_drained(net.sender));
    // Processing the released NAK must have produced a DROPREQ, not victim DATA.
    TASK_CHECK(net.proxy.target_data_after_nak() == 0);
    return 0;
}

// API group -------------------------------------------------------------------

static int case_api_validation(void)
{
    TASK_CHECK(srt_startup() == 0);

    TASK_CHECK(srt_dropmsg(SRT_INVALID_SOCK, 1001) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVSOCK);

    const SRTSOCKET sock = srt_create_socket();
    TASK_CHECK(sock != SRT_INVALID_SOCK);

    TASK_CHECK(srt_dropmsg(sock, SRT_MSGNO_CONTROL) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVPARAM);
    TASK_CHECK(srt_dropmsg(sock, SRT_MSGNO_NONE) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVPARAM);
    TASK_CHECK(srt_dropmsg(sock, srt::MSGNO_SEQ_MAX + 1) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVPARAM);

    TASK_CHECK(srt_dropmsg(sock, 1001) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_ENOCONN);

    TASK_CHECK(srt_dropmsg(SRTGROUP_MASK | 0x1234, 1001) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVPARAM);

    srt_close(sock);
    srt_cleanup();
    return 0;
}

static int case_api_stream_rejected(void)
{
    SrtNetwork net;
    TASK_CHECK(net.setup() == 0);

    SRTSOCKET stream_sender = srt_create_socket();
    TASK_CHECK(stream_sender != SRT_INVALID_SOCK);
    TASK_CHECK(SrtNetwork::configure(stream_sender, false) == 0);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int addrlen = sizeof addr;
    TASK_CHECK(srt_getsockname(net.listener, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0);

    // This second connection is intentionally direct; it only exercises mode checks.
    TASK_CHECK(srt_connect(stream_sender, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    SRTSOCKET stream_receiver = srt_accept(net.listener, NULL, NULL);
    TASK_CHECK(stream_receiver != SRT_INVALID_SOCK);

    TASK_CHECK(srt_dropmsg(stream_sender, 777) == SRT_ERROR);
    TASK_CHECK(srt_getlasterror(NULL) == SRT_EINVALBUFFERAPI);

    srt_close(stream_receiver);
    srt_close(stream_sender);
    return 0;
}

const struct task_case task_cases[] = {
    { "buffer", "partial_ack_and_31bit_wrap", case_buffer_partial_ack_wrap },
    { "buffer", "nak_neighbors_and_ack_reclaim", case_buffer_nak_neighbors_reclaim },
    { "buffer", "unsent_tail_is_skipped", case_buffer_unsent_tail_skipped },
    { "buffer", "concurrent_duplicate_drop_and_ack", case_buffer_concurrent_drop_ack },
    { "wire", "real_dropreq_and_successor", case_wire_real_dropreq_and_successor },
    { "recovery", "lost_dropreq_then_nak", case_recovery_lost_dropreq_nak_neighbors },
    { "api", "error_codes", case_api_validation },
    { "api", "stream_api_rejected", case_api_stream_rejected },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
