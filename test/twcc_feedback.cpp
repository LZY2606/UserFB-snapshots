/* Tests for transport-wide congestion control (TWCC) feedback generation.
 *
 * transport group: arrival ledger behavior through the explicit microsecond
 *                  recording entry (uvgrtp::twcc::record_packet)
 * wire group:      exact RTCP TWCC feedback bytes with a fixed clock
 * lifecycle group: real UDP reception with two media streams sharing one socket
 *
 * Registered through task_support/check.h, see task_support/driver.c. */

#include "task_support/check.h"

#include "twcc.hh"

#include "uvgrtp/lib.hh"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

    constexpr uint8_t EXT_ID = 3;

    struct ext_item {
        uint8_t id;
        std::vector<uint8_t> data;
    };

    /* Build an RTP packet with a one-byte header extension (profile 0xBEDE) */
    std::vector<uint8_t> make_rtp_ext(uint32_t ssrc, uint16_t media_seq, uint8_t payload_type,
        uint16_t profile, const std::vector<uint8_t>& ext_content,
        const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> ext = ext_content;
        while (ext.size() % 4 != 0)
            ext.push_back(0);

        std::vector<uint8_t> p;
        p.push_back(0x80 | 0x10); /* V=2, X=1 */
        p.push_back(payload_type);
        p.push_back((uint8_t)(media_seq >> 8));
        p.push_back((uint8_t)(media_seq & 0xff));
        p.push_back(0); p.push_back(0); p.push_back(0); p.push_back(1); /* timestamp */
        p.push_back((uint8_t)(ssrc >> 24));
        p.push_back((uint8_t)(ssrc >> 16));
        p.push_back((uint8_t)(ssrc >> 8));
        p.push_back((uint8_t)(ssrc & 0xff));
        p.push_back((uint8_t)(profile >> 8));
        p.push_back((uint8_t)(profile & 0xff));
        p.push_back((uint8_t)(ext.size() / 4 >> 8));
        p.push_back((uint8_t)((ext.size() / 4) & 0xff));
        p.insert(p.end(), ext.begin(), ext.end());
        p.insert(p.end(), payload.begin(), payload.end());
        return p;
    }

    /* RTP packet carrying one transport-wide sequence number */
    std::vector<uint8_t> make_rtp_tseq(uint32_t ssrc, uint16_t media_seq, uint16_t tseq,
        uint8_t ext_id = EXT_ID, uint8_t payload_type = 96)
    {
        std::vector<uint8_t> ext = {
            (uint8_t)((ext_id << 4) | 1), (uint8_t)(tseq >> 8), (uint8_t)(tseq & 0xff)
        };
        return make_rtp_ext(ssrc, media_seq, payload_type, 0xbede, ext, { 1, 2, 3, 4 });
    }

    /* RTP packet without any header extension */
    std::vector<uint8_t> make_rtp_no_ext(uint32_t ssrc, uint16_t media_seq)
    {
        std::vector<uint8_t> p;
        p.push_back(0x80);
        p.push_back(96);
        p.push_back((uint8_t)(media_seq >> 8));
        p.push_back((uint8_t)(media_seq & 0xff));
        p.insert(p.end(), { 0, 0, 0, 1 });
        p.push_back((uint8_t)(ssrc >> 24));
        p.push_back((uint8_t)(ssrc >> 16));
        p.push_back((uint8_t)(ssrc >> 8));
        p.push_back((uint8_t)(ssrc & 0xff));
        p.insert(p.end(), { 1, 2, 3, 4 });
        return p;
    }

    void record(uvgrtp::twcc& t, const std::vector<uint8_t>& packet, uint64_t arrival_us)
    {
        t.record_packet(packet.data(), packet.size(), arrival_us);
    }

    /* send callback capturing the packet instead of sending it */
    struct capture {
        std::vector<uint8_t> packet;
        rtp_error_t ret = RTP_OK;
        int calls = 0;

        rtp_error_t operator()(uint8_t* data, size_t size)
        {
            ++calls;
            packet.assign(data, data + size);
            return ret;
        }
    };

    uint16_t get_u16(const std::vector<uint8_t>& p, size_t off)
    {
        return (uint16_t)((p[off] << 8) | p[off + 1]);
    }

    uint32_t get_u32(const std::vector<uint8_t>& p, size_t off)
    {
        return ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
            ((uint32_t)p[off + 2] << 8) | (uint32_t)p[off + 3];
    }

    /* ------------------------------------------------------------------ */
    /* transport group: arrival ledger behavior                            */
    /* ------------------------------------------------------------------ */

    /* Transport sequences unwrap across zero; media RTP sequence numbers must
     * not be mistaken for transport sequences */
    int transport_record_and_unwrap()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(EXT_ID, 0x1001) == RTP_OK);

        uint16_t tseqs[] = { 65534, 65535, 0, 1 };
        for (int i = 0; i < 4; ++i) {
            /* media sequence numbers deliberately far away from the transport
             * sequence space */
            record(t, make_rtp_tseq(0xaaaa, (uint16_t)(1000 + i), tseqs[i]),
                2000000 + (uint64_t)i * 250);
        }

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(cap.packet.size() == 28);
        TASK_CHECK(get_u16(cap.packet, 12) == 65534); /* base = transport seq, not 1000 */
        TASK_CHECK(get_u16(cap.packet, 14) == 4);     /* wrapped range counts 4 */
        TASK_CHECK(get_u16(cap.packet, 20) == 0xd540); /* four small-delta symbols */

        TASK_CHECK(t.configure(0, 0x1001) == RTP_OK);
        return 0;
    }

    /* Gap filled out of order before the report; late packet after the report
     * is recorded but never re-reported */
    int transport_loss_reorder_late()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(EXT_ID, 0x1002) == RTP_OK);

        record(t, make_rtp_tseq(0xaaaa, 1, 10), 1000000);
        record(t, make_rtp_tseq(0xaaaa, 2, 11), 1001000);
        record(t, make_rtp_tseq(0xaaaa, 3, 13), 1002000);
        record(t, make_rtp_tseq(0xaaaa, 4, 14), 1003000);
        /* out-of-order fill of the gap before the feedback is generated */
        record(t, make_rtp_tseq(0xaaaa, 5, 12), 1004000);

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 10);
        TASK_CHECK(get_u16(cap.packet, 14) == 5);
        /* all five received; 12 arrived after 13/14, so 13 has a negative
         * (large) receive delta */
        TASK_CHECK(get_u16(cap.packet, 20) == 0xd590);
        TASK_CHECK(cap.packet[24] == 12);               /* delta of 12 */
        TASK_CHECK(get_u16(cap.packet, 25) == 0xfff8);  /* delta of 13: -8 ticks */

        record(t, make_rtp_tseq(0xaaaa, 6, 15), 1005000);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 15);
        TASK_CHECK(get_u16(cap.packet, 14) == 1);

        /* 16 and 18 received, 17 missing */
        record(t, make_rtp_tseq(0xaaaa, 7, 16), 1006000);
        record(t, make_rtp_tseq(0xaaaa, 8, 18), 1007000);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 16);
        TASK_CHECK(get_u16(cap.packet, 14) == 3);
        TASK_CHECK(get_u16(cap.packet, 20) == 0xd100); /* recv, lost, recv */

        /* 17 arrives late, after its range was already reported: it must not
         * produce a new report */
        record(t, make_rtp_tseq(0xaaaa, 9, 17), 1008000);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_NOT_FOUND);
        TASK_CHECK(cap.calls == 3); /* no packet was emitted for the late arrival */

        /* 19 is new and reported alone; the late 17 is not re-reported */
        record(t, make_rtp_tseq(0xaaaa, 10, 19), 1009000);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 19);
        TASK_CHECK(get_u16(cap.packet, 14) == 1);

        TASK_CHECK(t.configure(0, 0x1002) == RTP_OK);
        return 0;
    }

    /* Only the first valid arrival of a packet takes effect */
    int transport_duplicate_first_wins()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(EXT_ID, 0x1003) == RTP_OK);

        record(t, make_rtp_tseq(0xaaaa, 1, 20), 1000000);
        record(t, make_rtp_tseq(0xaaaa, 2, 20), 9000000); /* duplicate, much later */
        record(t, make_rtp_tseq(0xaaaa, 3, 21), 1002000);

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 20);
        TASK_CHECK(get_u16(cap.packet, 14) == 2);
        TASK_CHECK(get_u16(cap.packet, 20) == 0xd400); /* two small deltas */
        /* delta of 21 relative to the first arrival of 20: 8 ticks */
        TASK_CHECK(cap.packet[23] == 8);

        TASK_CHECK(t.configure(0, 0x1003) == RTP_OK);
        return 0;
    }

    /* Packets of different media SSRCs interleave in one shared ledger */
    int transport_cross_ssrc_interleave()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(EXT_ID, 0x1004) == RTP_OK);

        record(t, make_rtp_tseq(0xaaaa0001, 101, 1), 1000000);
        record(t, make_rtp_tseq(0xbbbb0002, 201, 2), 1000250);
        record(t, make_rtp_tseq(0xaaaa0001, 102, 3), 1000500);
        record(t, make_rtp_tseq(0xbbbb0002, 202, 4), 1000750);
        record(t, make_rtp_tseq(0xaaaa0001, 103, 5), 1001000);

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 1);
        TASK_CHECK(get_u16(cap.packet, 14) == 5);
        TASK_CHECK(get_u16(cap.packet, 20) == 0xd550); /* all five received */

        TASK_CHECK(t.configure(0, 0x1004) == RTP_OK);
        return 0;
    }

    /* Packets without a valid target extension item must not create records */
    int transport_invalid_extensions_ignored()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(EXT_ID, 0x1005) == RTP_OK);

        uint64_t ts = 1000000;

        /* no extension at all */
        record(t, make_rtp_no_ext(0xaaaa, 1), ts += 1000);
        /* unknown extension profile */
        record(t, make_rtp_ext(0xaaaa, 2, 96, 0xbedf, { 0x31, 0, 5 }, { 1 }), ts += 1000);
        /* only an unknown ID */
        record(t, make_rtp_ext(0xaaaa, 3, 96, 0xbede, { 0x71, 0, 5 }, { 1 }), ts += 1000);
        /* target ID with one-byte data */
        record(t, make_rtp_ext(0xaaaa, 4, 96, 0xbede, { 0x30, 5 }, { 1 }), ts += 1000);
        /* target ID with three-byte data */
        record(t, make_rtp_ext(0xaaaa, 5, 96, 0xbede, { 0x32, 0, 0, 5 }, { 1 }), ts += 1000);
        /* two target items in one extension */
        record(t, make_rtp_ext(0xaaaa, 6, 96, 0xbede,
            { 0x31, 0, 5, 0x31, 0, 6 }, { 1 }), ts += 1000);
        /* an element running out of the extension bounds corrupts the packet */
        record(t, make_rtp_ext(0xaaaa, 7, 96, 0xbede,
            { 0x31, 0, 5, 0x54, 0 }, { 1 }), ts += 1000);
        /* ID 15 stops the scan before the target item */
        record(t, make_rtp_ext(0xaaaa, 8, 96, 0xbede,
            { 0xf0, 0x31, 0, 5 }, { 1 }), ts += 1000);

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_NOT_FOUND);
        TASK_CHECK(cap.calls == 0);

        /* a valid packet is recorded normally afterwards */
        record(t, make_rtp_tseq(0xaaaa, 9, 60), ts += 1000);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 60);
        TASK_CHECK(get_u16(cap.packet, 14) == 1);

        TASK_CHECK(t.configure(0, 0x1005) == RTP_OK);
        return 0;
    }

    /* The ledger retains only the newest 8192 sequences; dropped ranges are
     * never reported */
    int transport_window_trim()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(EXT_ID, 0x1006) == RTP_OK);

        for (uint32_t i = 0; i < 9000; ++i) {
            record(t, make_rtp_tseq(0xaaaa, (uint16_t)i, (uint16_t)i), 1000000 + (uint64_t)i * 250);
        }

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 65536, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 9000 - 8192); /* oldest dropped */
        TASK_CHECK(get_u16(cap.packet, 14) == 8192);
        TASK_CHECK(t.send_feedback(0x11223344, 65536, std::ref(cap)) == RTP_NOT_FOUND);

        TASK_CHECK(t.configure(0, 0x1006) == RTP_OK);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /* wire group: exact feedback bytes with a fixed clock                 */
    /* ------------------------------------------------------------------ */

    int wire_feedback_bytes()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(1, 0x2001) == RTP_OK);

        record(t, make_rtp_tseq(0xaaaa, 1, 100, 1), 1000000);
        /* 101 lost */
        record(t, make_rtp_tseq(0xaaaa, 2, 102, 1), 1001000);
        record(t, make_rtp_tseq(0xaaaa, 3, 103, 1), 1100000);

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);

        /* reference time: floor(1000000/64000) = 15, first delta 160 ticks,
         * delta(102) = 4 ticks, delta(103) = 396 ticks (large) */
        const uint8_t expected[] = {
            0x8f, 0xcd, 0x00, 0x06,
            0x11, 0x22, 0x33, 0x44,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x64, 0x00, 0x04,
            0x00, 0x00, 0x0f, 0x00,
            0xd1, 0x80,
            0xa0, 0x04, 0x01, 0x8c,
            0x00, 0x00
        };
        TASK_CHECK(cap.packet.size() == sizeof(expected));
        TASK_CHECK(memcmp(cap.packet.data(), expected, sizeof(expected)) == 0);

        /* next packet continues after the reported range, feedback count is 1 */
        record(t, make_rtp_tseq(0xaaaa, 4, 104, 1), 1101000);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);

        const uint8_t expected2[] = {
            0x8f, 0xcd, 0x00, 0x05,
            0x11, 0x22, 0x33, 0x44,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x68, 0x00, 0x01,
            0x00, 0x00, 0x11, 0x01,
            0xd0, 0x00,
            0x34, 0x00
        };
        TASK_CHECK(cap.packet.size() == sizeof(expected2));
        TASK_CHECK(memcmp(cap.packet.data(), expected2, sizeof(expected2)) == 0);

        TASK_CHECK(t.configure(0, 0x2001) == RTP_OK);
        return 0;
    }

    /* max_bytes cuts the report at a status chunk boundary; the next call
     * continues with the remaining range */
    int wire_max_bytes_prefix()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(2, 0x2002) == RTP_OK);

        for (uint32_t i = 0; i < 14; ++i) {
            record(t, make_rtp_tseq(0xaaaa, (uint16_t)i, (uint16_t)(200 + i), 2),
                1000000 + (uint64_t)i * 250);
        }

        /* the full report needs 40 bytes, allow only one chunk (7 statuses) */
        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 32, std::ref(cap)) == RTP_OK);
        TASK_CHECK(cap.packet.size() == 32);
        TASK_CHECK(get_u16(cap.packet, 12) == 200);
        TASK_CHECK(get_u16(cap.packet, 14) == 7);
        TASK_CHECK(get_u16(cap.packet, 20) == 0xd555); /* seven small deltas */
        TASK_CHECK(cap.packet[19] == 0);               /* feedback count */
        TASK_CHECK(cap.packet[22] == 160);             /* first delta */
        for (int i = 23; i < 29; ++i) {
            TASK_CHECK(cap.packet[i] == 1);            /* 250 us spacing */
        }

        TASK_CHECK(t.send_feedback(0x11223344, 32, std::ref(cap)) == RTP_OK);
        TASK_CHECK(cap.packet.size() == 32);
        TASK_CHECK(get_u16(cap.packet, 12) == 207);
        TASK_CHECK(get_u16(cap.packet, 14) == 7);
        TASK_CHECK(cap.packet[19] == 1);               /* feedback count advanced */
        TASK_CHECK(cap.packet[22] == 167);             /* new reference time */

        TASK_CHECK(t.send_feedback(0x11223344, 32, std::ref(cap)) == RTP_NOT_FOUND);

        /* budget errors: too small for even the first chunk, state must not change */
        uvgrtp::twcc t2;
        TASK_CHECK(t2.configure(2, 0x2003) == RTP_OK);
        for (uint32_t i = 0; i < 7; ++i) {
            record(t2, make_rtp_tseq(0xaaaa, (uint16_t)i, (uint16_t)(200 + i), 2),
                1000000 + (uint64_t)i * 250);
        }
        capture cap2;
        TASK_CHECK(t2.send_feedback(0x11223344, 31, std::ref(cap2)) == RTP_INVALID_VALUE);
        TASK_CHECK(t2.send_feedback(0x11223344, 0, std::ref(cap2)) == RTP_INVALID_VALUE);
        TASK_CHECK(cap2.calls == 0);
        TASK_CHECK(t2.send_feedback(0x11223344, 32, std::ref(cap2)) == RTP_OK);
        TASK_CHECK(get_u16(cap2.packet, 12) == 200);   /* still the full range */
        TASK_CHECK(cap2.packet[19] == 0);              /* count not advanced by failures */

        TASK_CHECK(t.configure(0, 0x2002) == RTP_OK);
        TASK_CHECK(t2.configure(0, 0x2003) == RTP_OK);
        return 0;
    }

    /* A receive delta that does not fit into 16 bits ends the report; the next
     * report re-establishes the reference time */
    int wire_delta_overflow_cut()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(1, 0x2004) == RTP_OK);

        record(t, make_rtp_tseq(0xaaaa, 1, 300, 1), 64000);
        record(t, make_rtp_tseq(0xaaaa, 2, 301, 1), 139000);   /* +300 ticks: large */
        record(t, make_rtp_tseq(0xaaaa, 3, 302, 1), 10064000); /* +39700 ticks: cut */

        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 300);
        TASK_CHECK(get_u16(cap.packet, 14) == 2);       /* 302 not included */
        TASK_CHECK(get_u16(cap.packet, 20) == 0xd800);  /* small + large symbols */
        TASK_CHECK(cap.packet[19] == 0);
        TASK_CHECK(cap.packet[22] == 0);                /* first delta */
        TASK_CHECK(get_u16(cap.packet, 23) == 300);     /* second delta, 16 bits */

        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 302);
        TASK_CHECK(get_u16(cap.packet, 14) == 1);
        TASK_CHECK(cap.packet[18] == 0x9d);             /* new reference time 157 */
        TASK_CHECK(cap.packet[19] == 1);
        TASK_CHECK(cap.packet[22] == 64);               /* delta vs new reference */

        TASK_CHECK(t.configure(0, 0x2004) == RTP_OK);
        return 0;
    }

    /* No empty packets: unconfigured, no records and all-reported states */
    int wire_empty_reports()
    {
        uvgrtp::twcc t;
        capture cap;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_NOT_FOUND);

        TASK_CHECK(t.configure(1, 0x2005) == RTP_OK);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_NOT_FOUND);

        record(t, make_rtp_tseq(0xaaaa, 1, 400, 1), 1000000);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_NOT_FOUND);
        TASK_CHECK(cap.calls == 1);

        TASK_CHECK(t.configure(0, 0x2005) == RTP_OK);
        return 0;
    }

    /* A failing send keeps the reported range and feedback count untouched */
    int wire_send_failure_keeps_state()
    {
        uvgrtp::twcc t;
        TASK_CHECK(t.configure(1, 0x2006) == RTP_OK);

        record(t, make_rtp_tseq(0xaaaa, 1, 500, 1), 1000000);
        record(t, make_rtp_tseq(0xaaaa, 2, 501, 1), 1001000);

        capture cap;
        cap.ret = RTP_SEND_ERROR;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_SEND_ERROR);
        TASK_CHECK(cap.calls == 1);

        cap.ret = RTP_OK;
        TASK_CHECK(t.send_feedback(0x11223344, 1500, std::ref(cap)) == RTP_OK);
        TASK_CHECK(get_u16(cap.packet, 12) == 500); /* full range re-sent */
        TASK_CHECK(get_u16(cap.packet, 14) == 2);
        TASK_CHECK(cap.packet[19] == 0);            /* count still 0 */

        TASK_CHECK(t.configure(0, 0x2006) == RTP_OK);
        return 0;
    }

    /* ------------------------------------------------------------------ */
    /* lifecycle group: real UDP, two streams sharing one socket           */
    /* ------------------------------------------------------------------ */

#ifndef _WIN32

    int make_udp_receiver(uint16_t port)
    {
        int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return -1;
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    bool send_udp(int fd, const std::vector<uint8_t>& data, uint16_t dst_port)
    {
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(dst_port);
        return sendto(fd, data.data(), data.size(), 0, (sockaddr*)&addr, sizeof(addr)) ==
            (ssize_t)data.size();
    }

    /* Receive one RTCP TWCC feedback packet (PT=205) from "sender_ssrc",
     * skipping any other RTCP traffic, within timeout_ms */
    bool recv_twcc_packet(int fd, uint32_t sender_ssrc, std::vector<uint8_t>& out, int timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        uint8_t buf[2048];

        for (;;) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            int remaining = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();

            pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (poll(&pfd, 1, remaining) <= 0)
                return false;

            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            uint32_t sender = n >= 8 ?
                ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                ((uint32_t)buf[6] << 8) | (uint32_t)buf[7] : 0;
            if (n >= 20 && buf[1] == 205 && (buf[0] & 0x1f) == 15 &&
                sender == sender_ssrc) {
                out.assign(buf, buf + n);
                return true;
            }
        }
    }

    struct frame_counter {
        std::atomic<int> count{ 0 };
        std::mutex mutex;
        std::vector<uint8_t> first_bytes;
    };

    void counting_hook(void* arg, uvgrtp::frame::rtp_frame* frame)
    {
        frame_counter* c = (frame_counter*)arg;
        if (frame->payload_len > 0) {
            std::lock_guard<std::mutex> lg(c->mutex);
            c->first_bytes.push_back(frame->payload[0]);
        }
        c->count.fetch_add(1);
        uvgrtp::frame::dealloc_frame(frame);
    }

    bool wait_for_count(frame_counter& c, int target, int timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (c.count.load() < target) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    constexpr uint16_t LPORT = 26100;
    constexpr uint16_t RPORT = 26150; /* RTCP feedback goes to RPORT + 1 */

    constexpr uint32_t SSRC_A = 0x11110001;
    constexpr uint32_t SSRC_B = 0x22220002;

    /* Two media streams share one receive socket: one extension ID, one
     * ledger, one feedback count; conflicts are reported and the shared state
     * recovers after release */
    int lifecycle_shared_socket_two_streams()
    {
        uvgrtp::context ctx;
        uvgrtp::session* sess = ctx.create_session("127.0.0.1");
        TASK_CHECK(sess != nullptr);

        int flags = RCE_RTCP;
        uvgrtp::media_stream* s1 = sess->create_stream(LPORT, RPORT, RTP_FORMAT_GENERIC, flags);
        uvgrtp::media_stream* s2 = sess->create_stream(LPORT, RPORT, RTP_FORMAT_GENERIC, flags);
        TASK_CHECK(s1 != nullptr && s2 != nullptr);
        TASK_CHECK(s1->configure_ctx(RCC_REMOTE_SSRC, SSRC_A) == RTP_OK);
        TASK_CHECK(s2->configure_ctx(RCC_REMOTE_SSRC, SSRC_B) == RTP_OK);

        frame_counter c1, c2;
        TASK_CHECK(s1->install_receive_hook(&c1, counting_hook) == RTP_OK);
        TASK_CHECK(s2->install_receive_hook(&c2, counting_hook) == RTP_OK);

        /* one shared configuration: conflict is an error, same ID is reused */
        TASK_CHECK(s1->configure_twcc(EXT_ID) == RTP_OK);
        TASK_CHECK(s2->configure_twcc(4) == RTP_INVALID_VALUE);
        TASK_CHECK(s2->configure_twcc(EXT_ID) == RTP_OK);
        TASK_CHECK(s1->configure_twcc(EXT_ID) == RTP_OK); /* idempotent */

        int rtcp_rx = make_udp_receiver(RPORT + 1);
        int rtp_tx = (int)socket(AF_INET, SOCK_DGRAM, 0);
        TASK_CHECK(rtcp_rx >= 0 && rtp_tx >= 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        /* interleaved media of both streams, transport sequence 4 is lost */
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_A, 501, 1, EXT_ID, 0), LPORT));
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_A, 502, 2, EXT_ID, 0), LPORT));
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_B, 901, 3, EXT_ID, 0), LPORT));
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_A, 503, 5, EXT_ID, 0), LPORT));

        /* normal media delivery is preserved for both streams */
        TASK_CHECK(wait_for_count(c1, 3, 3000));
        TASK_CHECK(wait_for_count(c2, 1, 3000));

        uint32_t sender1 = s1->get_ssrc();
        uint32_t sender2 = s2->get_ssrc();
        std::vector<uint8_t> fb;

        TASK_CHECK(s1->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
        TASK_CHECK(recv_twcc_packet(rtcp_rx, sender1, fb, 3000));
        TASK_CHECK(fb[0] == 0x8f && fb[1] == 205);
        TASK_CHECK(get_u32(fb, 8) == 0);        /* media SSRC */
        TASK_CHECK(get_u16(fb, 12) == 1);       /* base */
        TASK_CHECK(get_u16(fb, 14) == 5);       /* 1,2,3,4,5 */
        TASK_CHECK(get_u16(fb, 20) == 0xd510);  /* recv,recv,recv,lost,recv */
        TASK_CHECK(fb[19] == 0);                /* feedback count */

        /* the other stream shares the ledger: nothing new to report */
        TASK_CHECK(s2->get_rtcp()->send_twcc_feedback(1500) == RTP_NOT_FOUND);

        /* ... but it can trigger feedback once something new arrived, and the
         * feedback packet count is shared */
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_B, 902, 6, EXT_ID, 0), LPORT));
        TASK_CHECK(wait_for_count(c2, 2, 3000));
        TASK_CHECK(s2->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
        TASK_CHECK(recv_twcc_packet(rtcp_rx, sender2, fb, 3000));
        TASK_CHECK(get_u16(fb, 12) == 6);
        TASK_CHECK(get_u16(fb, 14) == 1);
        TASK_CHECK(fb[19] == 1);

        /* releasing one stream keeps the shared state alive for the other */
        TASK_CHECK(s1->configure_twcc(0) == RTP_OK);
        TASK_CHECK(s1->get_rtcp()->send_twcc_feedback(1500) == RTP_NOT_FOUND);
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_A, 504, 7, EXT_ID, 0), LPORT));
        TASK_CHECK(wait_for_count(c1, 4, 3000));
        TASK_CHECK(s2->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
        TASK_CHECK(recv_twcc_packet(rtcp_rx, sender2, fb, 3000));
        TASK_CHECK(get_u16(fb, 12) == 7);
        TASK_CHECK(fb[19] == 2);

        /* releasing the last user clears the shared state */
        TASK_CHECK(s2->configure_twcc(0) == RTP_OK);
        TASK_CHECK(s2->get_rtcp()->send_twcc_feedback(1500) == RTP_NOT_FOUND);

        /* a different extension ID is accepted again and starts fresh */
        TASK_CHECK(s1->configure_twcc(9) == RTP_OK);
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_A, 505, 100, 9, 0), LPORT));
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_A, 506, 101, EXT_ID, 0), LPORT));
        TASK_CHECK(wait_for_count(c1, 6, 3000));
        TASK_CHECK(s1->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
        TASK_CHECK(recv_twcc_packet(rtcp_rx, sender1, fb, 3000));
        TASK_CHECK(get_u16(fb, 12) == 100); /* fresh origin */
        TASK_CHECK(get_u16(fb, 14) == 1);   /* old ID 3 packet not recorded */
        TASK_CHECK(fb[19] == 0);            /* fresh feedback count */

        close(rtp_tx);
        close(rtcp_rx);
        sess->destroy_stream(s1);
        sess->destroy_stream(s2);
        ctx.destroy_session(sess);
        return 0;
    }

    constexpr uint16_t LPORT_A = 26200;
    constexpr uint16_t LPORT_B = 26300;
    constexpr uint16_t RPORT2 = 26250;

    constexpr uint32_t SSRC_C = 0x33330003;
    constexpr uint32_t SSRC_D = 0x44440004;

    /* Disabled streams behave as before; different sockets are independent */
    int lifecycle_disabled_and_independent_sockets()
    {
        uvgrtp::context ctx;
        uvgrtp::session* sess = ctx.create_session("127.0.0.1");
        TASK_CHECK(sess != nullptr);

        int flags = RCE_RTCP;
        uvgrtp::media_stream* sa = sess->create_stream(LPORT_A, RPORT2, RTP_FORMAT_GENERIC, flags);
        uvgrtp::media_stream* sb = sess->create_stream(LPORT_B, RPORT2, RTP_FORMAT_GENERIC, flags);
        TASK_CHECK(sa != nullptr && sb != nullptr);
        TASK_CHECK(sa->configure_ctx(RCC_REMOTE_SSRC, SSRC_C) == RTP_OK);
        TASK_CHECK(sb->configure_ctx(RCC_REMOTE_SSRC, SSRC_D) == RTP_OK);

        frame_counter ca, cb;
        TASK_CHECK(sa->install_receive_hook(&ca, counting_hook) == RTP_OK);
        TASK_CHECK(sb->install_receive_hook(&cb, counting_hook) == RTP_OK);

        int rtcp_rx = make_udp_receiver(RPORT2 + 1);
        int rtp_tx = (int)socket(AF_INET, SOCK_DGRAM, 0);
        TASK_CHECK(rtcp_rx >= 0 && rtp_tx >= 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        /* without configuration, packets with the extension are delivered as
         * before and nothing is recorded */
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_C, 1, 1, EXT_ID, 0), LPORT_A));
        TASK_CHECK(wait_for_count(ca, 1, 3000));
        TASK_CHECK(sa->get_rtcp()->send_twcc_feedback(1500) == RTP_NOT_FOUND);

        /* different sockets may use different extension IDs */
        TASK_CHECK(sa->configure_twcc(EXT_ID) == RTP_OK);
        TASK_CHECK(sb->configure_twcc(9) == RTP_OK);

        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_C, 2, 2, EXT_ID, 0), LPORT_A));
        TASK_CHECK(send_udp(rtp_tx, make_rtp_tseq(SSRC_D, 7, 50, 9, 0), LPORT_B));
        TASK_CHECK(wait_for_count(ca, 2, 3000));
        TASK_CHECK(wait_for_count(cb, 1, 3000));

        std::vector<uint8_t> fb;
        TASK_CHECK(sa->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
        TASK_CHECK(recv_twcc_packet(rtcp_rx, sa->get_ssrc(), fb, 3000));
        TASK_CHECK(get_u16(fb, 12) == 2); /* seq 1 arrived before configure */
        TASK_CHECK(get_u16(fb, 14) == 1);
        TASK_CHECK(fb[19] == 0);

        TASK_CHECK(sb->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
        TASK_CHECK(recv_twcc_packet(rtcp_rx, sb->get_ssrc(), fb, 3000));
        TASK_CHECK(get_u16(fb, 12) == 50);
        TASK_CHECK(get_u16(fb, 14) == 1);
        TASK_CHECK(fb[19] == 0); /* independent feedback count */

        close(rtp_tx);
        close(rtcp_rx);
        sess->destroy_stream(sa);
        sess->destroy_stream(sb);
        ctx.destroy_session(sess);
        return 0;
    }

#endif /* !_WIN32 */

} // namespace

const struct task_case task_cases[] = {
    { "transport", "record_and_unwrap", transport_record_and_unwrap },
    { "transport", "loss_reorder_late", transport_loss_reorder_late },
    { "transport", "duplicate_first_wins", transport_duplicate_first_wins },
    { "transport", "cross_ssrc_interleave", transport_cross_ssrc_interleave },
    { "transport", "invalid_extensions_ignored", transport_invalid_extensions_ignored },
    { "transport", "window_trim", transport_window_trim },
    { "wire", "feedback_bytes", wire_feedback_bytes },
    { "wire", "max_bytes_prefix", wire_max_bytes_prefix },
    { "wire", "delta_overflow_cut", wire_delta_overflow_cut },
    { "wire", "empty_reports", wire_empty_reports },
    { "wire", "send_failure_keeps_state", wire_send_failure_keeps_state },
#ifndef _WIN32
    { "lifecycle", "shared_socket_two_streams", lifecycle_shared_socket_two_streams },
    { "lifecycle", "disabled_and_independent_sockets", lifecycle_disabled_and_independent_sockets },
#endif
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
