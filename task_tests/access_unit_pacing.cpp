#include "formats/h264.hh"
#include "formats/h265.hh"
#include "formats/media.hh"
#include "rtp.hh"
#include "socket.hh"

#include "task_support/check.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* In-memory transport boundary: all RTP construction and scheduling is the real
 * production path, but packets are recorded in memory together with the virtual
 * send time instead of touching the network. */
static std::vector<std::vector<unsigned char>> g_packets;
static std::vector<int64_t> g_send_times;
static int64_t g_now_ns;
static long g_sendmsg_calls;
static long g_fail_on_call; /* 0-based sendmsg call index that fails, -1 disables */

extern "C" ssize_t sendmsg(int, const struct msghdr* m, int)
{
    long call = g_sendmsg_calls++;

    if (call == g_fail_on_call) {
        errno = EIO;
        return -1;
    }

    std::vector<unsigned char> b;
    for (size_t i = 0; i < m->msg_iovlen; i++) {
        auto* p = static_cast<unsigned char*>(m->msg_iov[i].iov_base);
        b.insert(b.end(), p, p + m->msg_iov[i].iov_len);
    }

    g_send_times.push_back(g_now_ns);
    g_packets.push_back(std::move(b));
    return static_cast<ssize_t>(g_packets.back().size());
}

#ifdef __linux__
extern "C" int sendmmsg(int fd, struct mmsghdr* ms, unsigned n, int flags)
{
    for (unsigned i = 0; i < n; i++) {
        auto sz = sendmsg(fd, &ms[i].msg_hdr, flags);
        if (sz < 0) return -1;
        ms[i].msg_len = sz;
    }
    return n;
}
#endif

static constexpr int64_t MS_NS = 1000 * 1000;
static constexpr int64_t T0 = 1000 * MS_NS; /* virtual clock reading at test start */
static constexpr int64_t FRAME_50FPS_NS = 20 * MS_NS; /* frame interval of 50 fps */

static void reset_transport()
{
    g_packets.clear();
    g_send_times.clear();
    g_sendmsg_calls = 0;
    g_fail_on_call = -1;
    g_now_ns = T0;
}

static void install_virtual_clock(uvgrtp::formats::media& media)
{
    media.set_send_timing_hooks(
        []() -> int64_t { return g_now_ns; },
        [](int64_t deadline_ns) { if (deadline_ns > g_now_ns) g_now_ns = deadline_ns; });
}

struct fixture {
    std::shared_ptr<std::atomic<uint32_t>> ssrc;
    std::shared_ptr<uvgrtp::rtp> rtp;
    std::shared_ptr<uvgrtp::socket> sock;
    std::unique_ptr<uvgrtp::formats::media> media;
    sockaddr_in addr{};
    sockaddr_in6 addr6{};
};

static bool init_fixture(fixture& f, rtp_format_t fmt, int rce_flags)
{
    f.ssrc = std::make_shared<std::atomic<uint32_t>>(7);
    f.rtp = std::make_shared<uvgrtp::rtp>(fmt, f.ssrc, false);
    f.sock = std::make_shared<uvgrtp::socket>(RCE_NO_FLAGS);

    if (f.sock->init(AF_INET, SOCK_DGRAM, 0) != RTP_OK)
        return false;

    switch (fmt) {
    case RTP_FORMAT_H264:
        f.media.reset(new uvgrtp::formats::h264(f.sock, f.rtp, rce_flags));
        break;
    case RTP_FORMAT_H265:
        f.media.reset(new uvgrtp::formats::h265(f.sock, f.rtp, rce_flags));
        break;
    default:
        f.media.reset(new uvgrtp::formats::media(f.sock, f.rtp, rce_flags));
        break;
    }

    f.addr.sin_family = AF_INET;
    f.addr.sin_port = htons(4000);
    f.addr.sin_addr.s_addr = htonl(0x7f000001);
    return true;
}

/* Build an Annex B access unit from complete NAL units */
static std::vector<uint8_t> annexb(std::initializer_list<std::vector<uint8_t>> nals)
{
    std::vector<uint8_t> out;
    for (auto& nal : nals) {
        out.insert(out.end(), { 0, 0, 0, 1 });
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

static bool pkt_marker(const std::vector<unsigned char>& p) { return p.size() >= 12 && (p[1] & 0x80); }
static uint16_t pkt_seq(const std::vector<unsigned char>& p) { return uint16_t((p[2] << 8) | p[3]); }
static uint32_t pkt_ts(const std::vector<unsigned char>& p)
{
    return (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | uint32_t(p[7]);
}
static std::vector<unsigned char> pkt_payload(const std::vector<unsigned char>& p)
{
    return std::vector<unsigned char>(p.begin() + 12, p.end());
}

/* Check that all packets share "ts", that sequence numbers are contiguous
 * starting from "seq0" and that only the last packet has the marker bit */
static int check_au_packets(size_t first, size_t count, uint32_t ts, uint16_t seq0)
{
    TASK_CHECK(first + count <= g_packets.size());

    for (size_t i = 0; i < count; ++i) {
        const auto& p = g_packets[first + i];
        TASK_CHECK(p.size() >= 12);
        TASK_CHECK(pkt_ts(p) == ts);
        TASK_CHECK(pkt_seq(p) == uint16_t(seq0 + i));
        TASK_CHECK(pkt_marker(p) == (i == count - 1));
    }

    return 0;
}

/* accessunit: multiple NAL units with aggregation disabled form one access unit */
static int case_multi_nal_no_aggr()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);
    f.rtp->set_timestamp(90000);

    std::vector<uint8_t> sps = { 0x67, 0x11, 0x22 };
    std::vector<uint8_t> pps = { 0x68, 0x33, 0x44 };
    std::vector<uint8_t> idr = { 0x65, 0x55, 0x66 };
    auto au = annexb({ sps, pps, idr });

    uint16_t seq0 = f.rtp->get_sequence();
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    TASK_CHECK(g_packets.size() == 3);
    TASK_CHECK(check_au_packets(0, 3, 90000, seq0) == 0);
    TASK_CHECK(pkt_payload(g_packets[0]) == sps);
    TASK_CHECK(pkt_payload(g_packets[1]) == pps);
    TASK_CHECK(pkt_payload(g_packets[2]) == idr);

    /* no scheduling flags: everything is sent immediately at the calling moment */
    TASK_CHECK(g_send_times.size() == 3);
    for (auto t : g_send_times)
        TASK_CHECK(t == T0);

    return 0;
}

/* accessunit: small NAL units are aggregated and a large NAL unit is fragmented,
 * the marker goes only to the last FU of the access unit */
static int case_aggregate_and_fu()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);
    f.rtp->set_payload_size(100);
    f.rtp->set_timestamp(555);

    std::vector<uint8_t> small1 = { 0x67, 0x01, 0x02 };
    std::vector<uint8_t> small2 = { 0x68, 0x03, 0x04 };
    std::vector<uint8_t> big(250, 0xAA);
    big[0] = 0x65; /* IDR, NRI = 3 */

    auto au = annexb({ small1, small2, big });

    uint16_t seq0 = f.rtp->get_sequence();
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    /* one STAP-A + three FUs */
    TASK_CHECK(g_packets.size() == 4);
    TASK_CHECK(check_au_packets(0, 4, 555, seq0) == 0);

    /* STAP-A: header (NRI 3, type 24), then [uint16 size][NAL unit] entries */
    std::vector<unsigned char> stap = { 0x78, 0x00, 0x03, 0x67, 0x01, 0x02, 0x00, 0x03, 0x68, 0x03, 0x04 };
    TASK_CHECK(pkt_payload(g_packets[0]) == stap);

    /* FU packets: indicator keeps NRI and uses type 28 (FU-A) */
    auto fu1 = pkt_payload(g_packets[1]);
    auto fu2 = pkt_payload(g_packets[2]);
    auto fu3 = pkt_payload(g_packets[3]);

    TASK_CHECK(fu1.size() == 100 && fu2.size() == 100 && fu3.size() == 55);
    TASK_CHECK(fu1[0] == 0x7C && fu2[0] == 0x7C && fu3[0] == 0x7C);
    TASK_CHECK(fu1[1] == 0x85); /* start, NAL type 5 */
    TASK_CHECK(fu2[1] == 0x05); /* middle */
    TASK_CHECK(fu3[1] == 0x45); /* end */

    /* the fragmented content reassembles to the original NAL unit payload */
    std::vector<unsigned char> content;
    content.insert(content.end(), fu1.begin() + 2, fu1.end());
    content.insert(content.end(), fu2.begin() + 2, fu2.end());
    content.insert(content.end(), fu3.begin() + 2, fu3.end());
    TASK_CHECK(content == std::vector<unsigned char>(big.begin() + 1, big.end()));

    return 0;
}

/* accessunit: an access unit that does not fit into one internal transaction
 * batch is still sent as a single access unit */
static int case_cross_internal_batches()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H265, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);
    f.rtp->set_payload_size(64);
    f.rtp->set_timestamp(4242);

    /* 400000 byte NAL unit -> 6558 FU packets of 61 payload bytes (last one 21),
     * which exceeds the 5000 packet transaction capacity */
    std::vector<uint8_t> big(400000, 0xAA);
    big[0] = 0x26; /* IDR_W_RADL */
    big[1] = 0x01;

    auto au = annexb({ big });

    uint16_t seq0 = f.rtp->get_sequence();
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    const size_t expected_packets = 6558;
    TASK_CHECK(g_packets.size() == expected_packets);
    TASK_CHECK(check_au_packets(0, expected_packets, 4242, seq0) == 0);

    /* FU payload headers of the first and the last packet across batch boundary */
    auto first = pkt_payload(g_packets[0]);
    auto last = pkt_payload(g_packets.back());
    TASK_CHECK(first.size() == 64);
    TASK_CHECK(first[0] == (49 << 1) && first[1] == 0x01);
    TASK_CHECK(first[2] == (0x80 | 19)); /* start fragment, IDR_W_RADL */
    TASK_CHECK(last.size() == 24);
    TASK_CHECK(last[0] == (49 << 1) && last[1] == 0x01);
    TASK_CHECK(last[2] == (0x40 | 19)); /* end fragment */

    /* no scheduling: all packets leave at the calling moment */
    TASK_CHECK(g_send_times.size() == expected_packets);
    for (auto t : g_send_times)
        TASK_CHECK(t == T0);

    return 0;
}

/* accessunit: two consecutive pushes form two access units */
static int case_two_access_units()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);

    std::vector<uint8_t> n1 = { 0x67, 0x11 };
    std::vector<uint8_t> n2 = { 0x68, 0x22 };
    std::vector<uint8_t> n3 = { 0x65, 0x33 };
    std::vector<uint8_t> n4 = { 0x41, 0x44 };

    auto au1 = annexb({ n1, n2 });
    auto au2 = annexb({ n3, n4 });

    uint16_t seq0 = f.rtp->get_sequence();

    f.rtp->set_timestamp(1000);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au1.data(), au1.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    f.rtp->set_timestamp(2000);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au2.data(), au2.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    TASK_CHECK(g_packets.size() == 4);
    TASK_CHECK(check_au_packets(0, 2, 1000, seq0) == 0);
    TASK_CHECK(check_au_packets(2, 2, 2000, uint16_t(seq0 + 2)) == 0);
    TASK_CHECK(pkt_payload(g_packets[0]) == n1);
    TASK_CHECK(pkt_payload(g_packets[1]) == n2);
    TASK_CHECK(pkt_payload(g_packets[2]) == n3);
    TASK_CHECK(pkt_payload(g_packets[3]) == n4);

    return 0;
}

/* accessunit: single NAL unit push keeps working and carries the marker */
static int case_h265_single_nal()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H265, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);
    f.rtp->set_timestamp(31337);

    std::vector<uint8_t> nal = { 0x26, 0x01, 0x99 };
    auto au = annexb({ nal });

    uint16_t seq0 = f.rtp->get_sequence();
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    TASK_CHECK(g_packets.size() == 1);
    TASK_CHECK(check_au_packets(0, 1, 31337, seq0) == 0);
    TASK_CHECK(pkt_payload(g_packets[0]) == nal);

    return 0;
}

/* accessunit: non-H26x sending keeps its original behavior */
static int case_generic_media_unchanged()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_GENERIC, RCE_NO_FLAGS));
    f.rtp->set_timestamp(777);

    std::vector<uint8_t> data = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    uint16_t seq0 = f.rtp->get_sequence();

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, data.data(), data.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    TASK_CHECK(g_packets.size() == 1);
    TASK_CHECK(pkt_marker(g_packets[0]));
    TASK_CHECK(pkt_ts(g_packets[0]) == 777);
    TASK_CHECK(pkt_seq(g_packets[0]) == seq0);
    TASK_CHECK(pkt_payload(g_packets[0]) == data);

    return 0;
}

/* accessunit: invalid input is rejected without sending anything */
static int case_invalid_inputs()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);
    f.rtp->set_timestamp(999);

    uint16_t seq0 = f.rtp->get_sequence();

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, nullptr, 0,
        RTP_NO_FLAGS, 7) == RTP_INVALID_VALUE);

    std::vector<uint8_t> nal = { 0x65, 0x01 };
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, nal.data(), 0,
        RTP_NO_FLAGS, 7) == RTP_INVALID_VALUE);

    /* no start codes -> no NAL units found */
    std::vector<uint8_t> junk = { 1, 2, 3, 4, 5, 6, 7, 8 };
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, junk.data(), junk.size(),
        RTP_NO_FLAGS, 7) == RTP_INVALID_VALUE);

    TASK_CHECK(g_packets.empty());
    TASK_CHECK(g_sendmsg_calls == 0);

    /* nothing was consumed: the next valid push uses the first sequence number */
    auto au = annexb({ nal });
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    TASK_CHECK(g_packets.size() == 1);
    TASK_CHECK(pkt_seq(g_packets[0]) == seq0);
    TASK_CHECK(pkt_marker(g_packets[0]));

    return 0;
}

/* pacing: with RCE_FRAME_RATE each access unit occupies exactly one frame slot */
static int case_frame_rate_slots()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_FRAME_RATE));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);

    std::vector<uint8_t> n1 = { 0x67, 0x11 };
    std::vector<uint8_t> n2 = { 0x65, 0x22 };
    auto au = annexb({ n1, n2 });

    for (int i = 0; i < 3; ++i) {
        TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
            RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);
    }

    /* the first access unit establishes the sync point, later ones advance by
     * the frame interval; both packets of an access unit share the slot */
    std::vector<int64_t> expected = {
        T0, T0,
        T0 + FRAME_50FPS_NS, T0 + FRAME_50FPS_NS,
        T0 + 2 * FRAME_50FPS_NS, T0 + 2 * FRAME_50FPS_NS
    };

    TASK_CHECK(g_send_times == expected);
    TASK_CHECK(g_packets.size() == 6);
    TASK_CHECK(!pkt_marker(g_packets[0]) && pkt_marker(g_packets[1]));
    TASK_CHECK(!pkt_marker(g_packets[2]) && pkt_marker(g_packets[3]));
    TASK_CHECK(!pkt_marker(g_packets[4]) && pkt_marker(g_packets[5]));

    return 0;
}

/* pacing: with only RCE_PACE_FRAGMENT_SENDING the access unit starts at the
 * calling moment and its packets are spread by the pace ratio */
static int case_pace_only()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_PACE_FRAGMENT_SENDING));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);
    f.media->set_pace(8, 10);

    std::vector<uint8_t> n1 = { 0x67, 0x11 };
    std::vector<uint8_t> n2 = { 0x68, 0x22 };
    std::vector<uint8_t> n3 = { 0x65, 0x33 };
    std::vector<uint8_t> n4 = { 0x41, 0x44 };
    auto au = annexb({ n1, n2, n3, n4 });

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    /* P = 4: deadline of packet i is au_start + floor(i * 20ms * 8 / (4 * 10)) */
    std::vector<int64_t> expected = {
        T0, T0 + 4 * MS_NS, T0 + 8 * MS_NS, T0 + 12 * MS_NS
    };
    TASK_CHECK(g_send_times == expected);

    /* the next access unit starts at the (virtual) calling moment */
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    std::vector<int64_t> expected2 = {
        T0, T0 + 4 * MS_NS, T0 + 8 * MS_NS, T0 + 12 * MS_NS,
        T0 + 12 * MS_NS, T0 + 16 * MS_NS, T0 + 20 * MS_NS, T0 + 24 * MS_NS
    };
    TASK_CHECK(g_send_times == expected2);
    TASK_CHECK(g_packets.size() == 8);
    TASK_CHECK(pkt_marker(g_packets[3]) && pkt_marker(g_packets[7]));
    TASK_CHECK(!pkt_marker(g_packets[2]) && !pkt_marker(g_packets[6]));

    return 0;
}

/* pacing: with both flags the frame slot is the starting point for pacing */
static int case_frame_rate_and_pace()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_FRAME_RATE | RCE_PACE_FRAGMENT_SENDING));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);
    /* keep the default pace ratio of 8/10 */

    std::vector<uint8_t> n1 = { 0x67, 0x11 };
    std::vector<uint8_t> n2 = { 0x68, 0x22 };
    std::vector<uint8_t> n3 = { 0x65, 0x33 };
    std::vector<uint8_t> n4 = { 0x41, 0x44 };
    auto au = annexb({ n1, n2, n3, n4 });

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    /* the packets of each access unit are paced from its frame slot, not
     * treated as individual frames */
    std::vector<int64_t> expected = {
        T0, T0 + 4 * MS_NS, T0 + 8 * MS_NS, T0 + 12 * MS_NS,
        T0 + 20 * MS_NS, T0 + 24 * MS_NS, T0 + 28 * MS_NS, T0 + 32 * MS_NS
    };
    TASK_CHECK(g_send_times == expected);

    return 0;
}

/* pacing: an access unit that is late resynchronizes the schedule once */
static int case_late_resync()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_FRAME_RATE));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);

    std::vector<uint8_t> nal = { 0x65, 0x11 };
    auto au = annexb({ nal });

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    /* the next access unit starts one second behind the schedule */
    g_now_ns += 1000 * MS_NS;

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    /* the late access unit is sent immediately from the resynchronized point
     * and the following one continues from the new sync point */
    std::vector<int64_t> expected = {
        T0,
        T0 + 1000 * MS_NS,
        T0 + 1000 * MS_NS + FRAME_50FPS_NS
    };
    TASK_CHECK(g_send_times == expected);

    return 0;
}

/* pacing: two pushes with the same explicit RTP timestamp still occupy two slots */
static int case_same_timestamp_two_slots()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_FRAME_RATE));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);

    std::vector<uint8_t> nal = { 0x65, 0x11 };
    auto au = annexb({ nal });

    f.rtp->set_timestamp(7777);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    f.rtp->set_timestamp(7777);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    TASK_CHECK(g_packets.size() == 2);
    TASK_CHECK(pkt_ts(g_packets[0]) == 7777);
    TASK_CHECK(pkt_ts(g_packets[1]) == 7777);

    std::vector<int64_t> expected = { T0, T0 + FRAME_50FPS_NS };
    TASK_CHECK(g_send_times == expected);

    return 0;
}

/* pacing: invalid input does not occupy a frame slot */
static int case_invalid_no_slot()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_FRAME_RATE));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);

    std::vector<uint8_t> nal = { 0x65, 0x11 };
    auto au = annexb({ nal });

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    std::vector<uint8_t> junk = { 9, 9, 9, 9 };
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, junk.data(), junk.size(),
        RTP_NO_FLAGS, 7) == RTP_INVALID_VALUE);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, nullptr, 0,
        RTP_NO_FLAGS, 7) == RTP_INVALID_VALUE);

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_NO_FLAGS, 7) == RTP_OK);

    /* the invalid calls did not consume frame slots */
    std::vector<int64_t> expected = { T0, T0 + FRAME_50FPS_NS };
    TASK_CHECK(g_send_times == expected);

    return 0;
}

/* recovery: a send error stops the rest of the access unit and the error is
 * returned; sequence numbers of the enqueued packets are not rolled back */
static int case_send_failure_stops_au()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);

    std::vector<uint8_t> n1 = { 0x67, 0x11 };
    std::vector<uint8_t> n2 = { 0x68, 0x22 };
    std::vector<uint8_t> n3 = { 0x65, 0x33 };
    std::vector<uint8_t> n4 = { 0x41, 0x44 };
    auto au1 = annexb({ n1, n2, n3, n4 });

    uint16_t seq0 = f.rtp->get_sequence();
    g_fail_on_call = 1; /* the second packet fails */

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au1.data(), au1.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_SEND_ERROR);

    /* the first packet went out, the second was attempted and the rest of the
     * access unit was not sent */
    TASK_CHECK(g_sendmsg_calls == 2);
    TASK_CHECK(g_packets.size() == 1);
    TASK_CHECK(pkt_seq(g_packets[0]) == seq0);
    TASK_CHECK(pkt_payload(g_packets[0]) == n1);

    /* the next access unit continues normally; the failed access unit consumed
     * the sequence numbers of all its four enqueued packets */
    g_fail_on_call = -1;

    std::vector<uint8_t> n5 = { 0x61, 0x55 };
    std::vector<uint8_t> n6 = { 0x41, 0x66 };
    auto au2 = annexb({ n5, n6 });

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au2.data(), au2.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    TASK_CHECK(g_packets.size() == 3);
    TASK_CHECK(pkt_seq(g_packets[1]) == uint16_t(seq0 + 4));
    TASK_CHECK(pkt_seq(g_packets[2]) == uint16_t(seq0 + 5));
    TASK_CHECK(pkt_payload(g_packets[1]) == n5);
    TASK_CHECK(pkt_payload(g_packets[2]) == n6);
    TASK_CHECK(pkt_marker(g_packets[2]));
    TASK_CHECK(!pkt_marker(g_packets[1]));

    return 0;
}

/* recovery: an access unit that attempted sending occupies its frame slot */
static int case_failure_consumes_slot()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_FRAME_RATE));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);

    std::vector<uint8_t> n1 = { 0x67, 0x11 };
    std::vector<uint8_t> n2 = { 0x65, 0x22 };
    auto au = annexb({ n1, n2 });

    g_fail_on_call = 1; /* the second packet of the first access unit fails */

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_SEND_ERROR);

    g_fail_on_call = -1;

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au.data(), au.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    /* the failed access unit occupied the first slot, so the next access unit
     * is sent at the second slot */
    std::vector<int64_t> expected = {
        T0,
        T0 + FRAME_50FPS_NS, T0 + FRAME_50FPS_NS
    };
    TASK_CHECK(g_send_times == expected);
    TASK_CHECK(g_packets.size() == 3);

    return 0;
}

/* recovery: nothing from a failed access unit leaks into the next one */
static int case_no_leftover_after_failure()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_NO_FLAGS));
    install_virtual_clock(*f.media);
    f.rtp->set_payload_size(100);

    /* STAP-A of two small NAL units + two FUs of a larger one */
    std::vector<uint8_t> small1 = { 0x67, 0x01 };
    std::vector<uint8_t> small2 = { 0x68, 0x02 };
    std::vector<uint8_t> big(150, 0xAA);
    big[0] = 0x65;
    auto au1 = annexb({ small1, small2, big });

    uint16_t seq0 = f.rtp->get_sequence();

    f.rtp->set_timestamp(111);
    g_fail_on_call = 0; /* the very first packet fails */
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au1.data(), au1.size(),
        RTP_NO_FLAGS, 7) == RTP_SEND_ERROR);

    TASK_CHECK(g_sendmsg_calls == 1);
    TASK_CHECK(g_packets.empty());

    g_fail_on_call = -1;

    std::vector<uint8_t> n1 = { 0x61, 0x77 };
    std::vector<uint8_t> n2 = { 0x41, 0x88 };
    auto au2 = annexb({ n1, n2 });

    f.rtp->set_timestamp(222);
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au2.data(), au2.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    /* the second access unit contains only its own NAL units, the three
     * sequence numbers of the failed access unit were consumed */
    TASK_CHECK(g_packets.size() == 2);
    TASK_CHECK(pkt_seq(g_packets[0]) == uint16_t(seq0 + 3));
    TASK_CHECK(pkt_seq(g_packets[1]) == uint16_t(seq0 + 4));
    TASK_CHECK(pkt_ts(g_packets[0]) == 222);
    TASK_CHECK(pkt_ts(g_packets[1]) == 222);
    TASK_CHECK(pkt_payload(g_packets[0]) == n1);
    TASK_CHECK(pkt_payload(g_packets[1]) == n2);
    TASK_CHECK(!pkt_marker(g_packets[0]) && pkt_marker(g_packets[1]));

    return 0;
}

/* recovery: a paced access unit stops at the failed packet and the next one
 * is paced from its own calling moment */
static int case_paced_send_failure()
{
    reset_transport();

    fixture f;
    TASK_CHECK(init_fixture(f, RTP_FORMAT_H264, RCE_PACE_FRAGMENT_SENDING));
    install_virtual_clock(*f.media);
    f.media->set_fps(50, 1);
    f.media->set_pace(8, 10);

    std::vector<uint8_t> n1 = { 0x67, 0x01 };
    std::vector<uint8_t> n2 = { 0x68, 0x02 };
    std::vector<uint8_t> n3 = { 0x65, 0x03 };
    std::vector<uint8_t> n4 = { 0x41, 0x04 };
    std::vector<uint8_t> n5 = { 0x01, 0x05 };
    auto au1 = annexb({ n1, n2, n3, n4, n5 });

    uint16_t seq0 = f.rtp->get_sequence();
    g_fail_on_call = 2; /* the third packet fails */

    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au1.data(), au1.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_SEND_ERROR);

    /* P = 5: deadlines are au_start + floor(i * 20ms * 8 / (5 * 10)) */
    TASK_CHECK(g_sendmsg_calls == 3);
    TASK_CHECK(g_packets.size() == 2);

    std::vector<int64_t> expected = { T0, T0 + 3200 * 1000 };
    TASK_CHECK(g_send_times == expected);

    /* the failed access unit consumed five sequence numbers */
    g_fail_on_call = -1;

    auto au2 = annexb({ n1, n2 });
    TASK_CHECK(f.media->push_frame(f.addr, f.addr6, au2.data(), au2.size(),
        RTP_H26X_DO_NOT_AGGR, 7) == RTP_OK);

    /* the virtual clock advanced to the failed packet's deadline, the new
     * access unit is paced from there with P = 2 */
    int64_t au2_start = T0 + 6400 * 1000;
    std::vector<int64_t> expected2 = {
        T0, T0 + 3200 * 1000,
        au2_start, au2_start + 8 * MS_NS
    };
    TASK_CHECK(g_send_times == expected2);

    TASK_CHECK(g_packets.size() == 4);
    TASK_CHECK(pkt_seq(g_packets[2]) == uint16_t(seq0 + 5));
    TASK_CHECK(pkt_seq(g_packets[3]) == uint16_t(seq0 + 6));
    TASK_CHECK(pkt_marker(g_packets[3]));
    TASK_CHECK(!pkt_marker(g_packets[2]));

    return 0;
}

const struct task_case task_cases[] = {
    { "accessunit", "multi_nal_no_aggr", case_multi_nal_no_aggr },
    { "accessunit", "aggregate_and_fu", case_aggregate_and_fu },
    { "accessunit", "cross_internal_batches", case_cross_internal_batches },
    { "accessunit", "two_access_units", case_two_access_units },
    { "accessunit", "h265_single_nal", case_h265_single_nal },
    { "accessunit", "generic_media_unchanged", case_generic_media_unchanged },
    { "accessunit", "invalid_inputs", case_invalid_inputs },
    { "pacing", "frame_rate_slots", case_frame_rate_slots },
    { "pacing", "pace_only", case_pace_only },
    { "pacing", "frame_rate_and_pace", case_frame_rate_and_pace },
    { "pacing", "late_resync", case_late_resync },
    { "pacing", "same_timestamp_two_slots", case_same_timestamp_two_slots },
    { "pacing", "invalid_no_slot", case_invalid_no_slot },
    { "recovery", "send_failure_stops_au", case_send_failure_stops_au },
    { "recovery", "failure_consumes_slot", case_failure_consumes_slot },
    { "recovery", "no_leftover_after_failure", case_no_leftover_after_failure },
    { "recovery", "paced_send_failure", case_paced_send_failure },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
