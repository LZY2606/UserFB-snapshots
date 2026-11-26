/*
 * TWCC feedback tests.
 *
 * Three groups are registered with task_support/check.h:
 *   transport  - shared-socket semantics, real local UDP when binding is permitted
 *   wire       - fixed-clock byte-exact feedback and extension parsing
 *   lifecycle  - configuration sharing/conflict/release, trimming, commit semantics
 *
 * In sandboxes where local UDP binding is forbidden (EPERM), the transport case
 * runs the same shared-state/record logic in-process instead, so the suite stays
 * executable. The real UDP path is compiled unconditionally and runs wherever
 * bind(2) works.
 */

#include "task_support/check.h"

#include "twcc.hh"
#include "rtp.hh"

#include "uvgrtp/context.hh"
#include "uvgrtp/media_stream.hh"
#include "uvgrtp/rtcp.hh"
#include "uvgrtp/session.hh"
#include "uvgrtp/frame.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using uvgrtp::twcc::TwccState;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void hex_dump(const char* tag, const std::vector<uint8_t>& p)
{
    std::fprintf(stderr, "%s (%zu):", tag, p.size());
    for (uint8_t b : p)
        std::fprintf(stderr, " %02x", b);
    std::fprintf(stderr, "\n");
}

static bool bytes_equal(const std::vector<uint8_t>& got, const uint8_t* expect, size_t n)
{
    if (got.size() != n) {
        std::fprintf(stderr, "size mismatch: got %zu want %zu\n", got.size(), n);
        hex_dump("got", got);
        std::fprintf(stderr, "want (%zu):", n);
        for (size_t i = 0; i < n; ++i)
            std::fprintf(stderr, " %02x", expect[i]);
        std::fprintf(stderr, "\n");
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (got[i] != expect[i]) {
            std::fprintf(stderr, "byte %zu mismatch: got %02x want %02x\n", i, got[i], expect[i]);
            hex_dump("got", got);
            return false;
        }
    }
    return true;
}

/* Build a minimal RTP datagram carrying one target one-byte TWCC extension. */
static std::vector<uint8_t> make_twcc_datagram(uint32_t ssrc, uint16_t media_seq,
                                               uint32_t rtp_ts, uint8_t ext_id,
                                               uint16_t transport_seq,
                                               uint8_t payload_byte = 0x65)
{
    std::vector<uint8_t> p;
    p.push_back(0x90); /* V=2, X=1, CC=0 */
    p.push_back(96);   /* marker 0, payload type 96 */
    p.push_back(static_cast<uint8_t>(media_seq >> 8));
    p.push_back(static_cast<uint8_t>(media_seq & 0xFF));
    for (int i = 3; i >= 0; --i)
        p.push_back(static_cast<uint8_t>((rtp_ts >> (8 * i)) & 0xFF));
    for (int i = 3; i >= 0; --i)
        p.push_back(static_cast<uint8_t>((ssrc >> (8 * i)) & 0xFF));

    /* extension header: RFC8285 one-byte profile, one 32-bit word */
    p.push_back(0xBE);
    p.push_back(0xDE);
    p.push_back(0x00);
    p.push_back(0x01);

    /* element: id (high nibble), length-1 = 1 */
    p.push_back(static_cast<uint8_t>((ext_id << 4) | 0x01));
    p.push_back(static_cast<uint8_t>(transport_seq >> 8));
    p.push_back(static_cast<uint8_t>(transport_seq & 0xFF));
    p.push_back(0x00); /* padding element to align the extension word */

    p.push_back(payload_byte);
    p.push_back(0x11);
    return p;
}

/* ---------------- RTCP TWCC parsing for readback ------------------ */

struct TwccView {
    uint8_t fmt = 0;
    uint8_t pt = 0;
    uint16_t length = 0;
    uint32_t sender_ssrc = 0;
    uint32_t media_ssrc = 0;
    uint16_t base = 0;
    uint16_t status_count = 0;
    uint32_t reference_time = 0;
    uint8_t feedback_count = 0;
    std::vector<uint8_t> statuses;
    size_t delta_bytes = 0;
    size_t total_size = 0;
};

static uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static bool parse_twcc(const uint8_t* d, size_t n, TwccView& v)
{
    if (n < 20)
        return false;
    if ((d[0] & 0xC0) != 0x80)
        return false;
    v.fmt = d[0] & 0x1F;
    v.pt = d[1];
    v.length = rd16(&d[2]);
    if (v.fmt != 15 || v.pt != 205)
        return false;
    size_t declared = static_cast<size_t>(v.length + 1) * 4;
    if (declared > n)
        return false;

    v.sender_ssrc = rd32(&d[4]);
    v.media_ssrc = rd32(&d[8]);
    v.base = rd16(&d[12]);
    v.status_count = rd16(&d[14]);
    v.reference_time = (uint32_t(d[16]) << 16) | (uint32_t(d[17]) << 8) | d[18];
    v.feedback_count = d[19];
    v.total_size = declared;

    size_t off = 20;
    std::vector<uint8_t> symbols;
    while (symbols.size() < v.status_count) {
        if (off + 2 > declared)
            return false;
        uint16_t chunk = rd16(&d[off]);
        off += 2;
        if ((chunk & 0x8000) == 0) {
            uint8_t sym = static_cast<uint8_t>((chunk >> 13) & 0x3);
            unsigned run = chunk & 0x1FFF;
            for (unsigned i = 0; i < run && symbols.size() < v.status_count; ++i)
                symbols.push_back(sym);
        } else if ((chunk & 0x4000) == 0) {
            for (int i = 0; i < 14 && symbols.size() < v.status_count; ++i)
                symbols.push_back(static_cast<uint8_t>((chunk >> (13 - i)) & 1) ? 1 : 0);
        } else {
            for (int i = 0; i < 7 && symbols.size() < v.status_count; ++i)
                symbols.push_back(static_cast<uint8_t>((chunk >> (12 - 2 * i)) & 0x3));
        }
    }
    v.statuses = symbols;

    /* Walk deltas according to received symbols to validate widths. */
    for (uint8_t s : v.statuses) {
        if (s == 1) {
            if (off + 1 > declared)
                return false;
            off += 1;
            v.delta_bytes += 1;
        } else if (s == 2) {
            if (off + 2 > declared)
                return false;
            off += 2;
            v.delta_bytes += 2;
        } else if (s != 0) {
            return false;
        }
    }
    /* Only zero padding may remain up to the 4-byte RTCP boundary. */
    if (off > declared)
        return false;
    for (size_t i = off; i < declared; ++i) {
        if (d[i] != 0)
            return false;
    }
    return true;
}

/* ================================================================== */
/* WIRE GROUP                                                         */
/* ================================================================== */

static int wire_golden_three_small_deltas(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(10, 1000000));
    TASK_CHECK(st.record_sequence(11, 1000250));
    TASK_CHECK(st.record_sequence(12, 1000750));

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(64, 0xAABBCCDD, p) == RTP_OK);

    static const uint8_t expect[] = {
        0x8F, 0xCD, 0x00, 0x06,
        0xAA, 0xBB, 0xCC, 0xDD,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x0A,             /* base sequence 10 */
        0x00, 0x03,             /* status count 3 */
        0x00, 0x00, 0x0F,       /* reference time 15 */
        0x00,                   /* feedback count 0 */
        0xD5, 0x00,             /* 2-bit vector: symbols 1,1,1 */
        0xA0, 0x01, 0x02,       /* deltas 160,1,2 */
        0x00, 0x00, 0x00        /* padding */
    };
    TASK_CHECK(bytes_equal(p, expect, sizeof(expect)));

    TwccView v;
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.base == 10 && v.status_count == 3);
    TASK_CHECK(v.statuses.size() == 3);
    TASK_CHECK(v.statuses[0] == 1 && v.statuses[1] == 1 && v.statuses[2] == 1);
    return 0;
}

static int wire_golden_loss(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(10, 1000000));
    /* 11 is missing */
    TASK_CHECK(st.record_sequence(12, 1000500));

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(64, 0xAABBCCDD, p) == RTP_OK);

    static const uint8_t expect[] = {
        0x8F, 0xCD, 0x00, 0x05,
        0xAA, 0xBB, 0xCC, 0xDD,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x0A,
        0x00, 0x03,
        0x00, 0x00, 0x0F,
        0x00,
        0xD1, 0x00, /* symbols 1,0,1 */
        0xA0, 0x02
    };
    TASK_CHECK(bytes_equal(p, expect, sizeof(expect)));
    return 0;
}

static int wire_golden_large_delta(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(20, 1000000));
    TASK_CHECK(st.record_sequence(21, 1075000)); /* 300 ticks later -> symbol 2 */

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(64, 0xAABBCCDD, p) == RTP_OK);

    static const uint8_t expect[] = {
        0x8F, 0xCD, 0x00, 0x06,
        0xAA, 0xBB, 0xCC, 0xDD,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x14,
        0x00, 0x02,
        0x00, 0x00, 0x0F,
        0x00,
        0xD8, 0x00, /* symbols 1,2 */
        0xA0,
        0x01, 0x2C, /* signed 16-bit delta 300 */
        0x00, 0x00, 0x00
    };
    TASK_CHECK(bytes_equal(p, expect, sizeof(expect)));

    TwccView v;
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.statuses[0] == 1 && v.statuses[1] == 2);
    return 0;
}

static int wire_golden_zero_crossing(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(65534, 1000000));
    TASK_CHECK(st.record_sequence(65535, 1000250));
    TASK_CHECK(st.record_sequence(0, 1000500));
    TASK_CHECK(st.record_sequence(1, 1000750));

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(64, 0x01020304, p) == RTP_OK);

    static const uint8_t expect[] = {
        0x8F, 0xCD, 0x00, 0x06,
        0x01, 0x02, 0x03, 0x04,
        0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFE, /* base wraps representation */
        0x00, 0x04,
        0x00, 0x00, 0x0F,
        0x00,
        0xD5, 0x40, /* four times symbol 1 */
        0xA0, 0x01, 0x01, 0x01,
        0x00, 0x00
    };
    TASK_CHECK(bytes_equal(p, expect, sizeof(expect)));
    TASK_CHECK(st.test_highest() == 65537);
    return 0;
}

static int wire_budget_prefixes_and_count(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(10, 1000000));
    TASK_CHECK(st.record_sequence(11, 1000250));
    TASK_CHECK(st.record_sequence(12, 1000750));

    /* Full 3-status packet needs 28 bytes. With 24 only the first two fit exactly. */
    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(24, 0xAABBCCDD, p) == RTP_OK);
    static const uint8_t first[] = {
        0x8F, 0xCD, 0x00, 0x05,
        0xAA, 0xBB, 0xCC, 0xDD,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x0A,
        0x00, 0x02,
        0x00, 0x00, 0x0F,
        0x00,
        0xD4, 0x00, /* symbols 1,1 */
        0xA0, 0x01
    };
    TASK_CHECK(bytes_equal(p, first, sizeof(first)));
    TASK_CHECK(st.feedback_count() == 1);

    /* Next call continues with a fresh reference time and increments count. */
    TASK_CHECK(st.poll_feedback(64, 0xAABBCCDD, p) == RTP_OK);
    static const uint8_t second[] = {
        0x8F, 0xCD, 0x00, 0x05,
        0xAA, 0xBB, 0xCC, 0xDD,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x0C, /* base 12 */
        0x00, 0x01,
        0x00, 0x00, 0x0F,
        0x01,       /* feedback count 1 */
        0xD0, 0x00, /* symbol 1 */
        0xA3,       /* tick 4003 - 15*256 */
        0x00        /* one padding byte */
    };
    TASK_CHECK(bytes_equal(p, second, sizeof(second)));
    TASK_CHECK(st.feedback_count() == 2);
    TASK_CHECK(!st.has_pending());
    return 0;
}

static int wire_tiny_budget_no_mutation(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(10, 1000000));

    /* Minimum possible datagram is 24 bytes (20 header + 2 chunk + 1 delta + pad). */
    const size_t tiny[] = {0, 8, 19, 20, 21, 22, 23};
    for (size_t b : tiny) {
        TwccState::PreparedPacket prep;
        TASK_CHECK(st.prepare_feedback(b, 1, prep) == RTP_INVALID_VALUE);
    }
    TASK_CHECK(st.feedback_count() == 0);
    TASK_CHECK(st.test_next_base() == 10);

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(24, 1, p) == RTP_OK);
    TASK_CHECK(st.feedback_count() == 1);
    TASK_CHECK(p.size() == 24);
    return 0;
}

static int wire_no_records_is_not_found(void)
{
    TwccState st;
    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(1500, 1, p) == RTP_NOT_FOUND);

    TwccState::PreparedPacket prep;
    TASK_CHECK(st.prepare_feedback(1500, 1, prep) == RTP_NOT_FOUND);
    return 0;
}

static int wire_unencodable_delta_restarts_reference(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(1, 1000000));
    /* 19 seconds later: 76000 ticks, outside signed 16 bit delta. */
    TASK_CHECK(st.record_sequence(2, 20000000));

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(1500, 7, p) == RTP_OK);
    TwccView v;
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.base == 1 && v.status_count == 1);

    /* Remaining range gets a newly established reference time. */
    TASK_CHECK(st.poll_feedback(1500, 7, p) == RTP_OK);
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.base == 2 && v.status_count == 1);
    TASK_CHECK(v.reference_time == 312); /* floor(20000000/64000) = 312 */
    TASK_CHECK(v.statuses[0] == 1);
    /* tick 80000 - 312*256 = 128 */
    TASK_CHECK(p[22] == 0x80);
    return 0;
}

static int wire_extension_parser(void)
{
    /* Baseline packet from task_support/baseline.cpp: id 3, value fffe. */
    uint8_t baseline[] = {0x90, 96, 0, 7, 0, 0, 0, 9, 0x12, 0x34, 0x56, 0x78,
                          0xbe, 0xde, 0, 1, 0x31, 0xff, 0xfe, 0, 0x65, 0x11};
    uint16_t seq = 0;
    TASK_CHECK(uvgrtp::twcc::extract_transport_sequence(baseline, sizeof(baseline), 3, seq));
    TASK_CHECK(seq == 0xFFFE);
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(baseline, sizeof(baseline), 4, seq));

    /* No extension bit. */
    std::vector<uint8_t> nox(baseline, baseline + sizeof(baseline));
    nox[0] = 0x80;
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(nox.data(), nox.size(), 3, seq));

    /* Wrong profile (two-byte form 0x100) is unsupported. */
    std::vector<uint8_t> prof(baseline, baseline + sizeof(baseline));
    prof[12] = 0x10;
    prof[13] = 0x00;
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(prof.data(), prof.size(), 3, seq));

    /* Unknown legal item only (id 4 with two data bytes). */
    uint8_t unknown[] = {0x90, 96, 0, 1, 0, 0, 0, 9, 1, 2, 3, 4,
                         0xbe, 0xde, 0, 1, 0x41, 0xAA, 0xBB, 0};
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(unknown, sizeof(unknown), 3, seq));
    TASK_CHECK(uvgrtp::twcc::extract_transport_sequence(unknown, sizeof(unknown), 4, seq));
    TASK_CHECK(seq == 0xAABB);

    /* Two target items -> no valid record. */
    uint8_t twice[] = {0x90, 96, 0, 1, 0, 0, 0, 9, 1, 2, 3, 4,
                       0xbe, 0xde, 0, 2,
                       0x31, 0x11, 0x22,
                       0x31, 0x33, 0x44,
                       0, 0};
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(twice, sizeof(twice), 3, seq));

    /* Out-of-bounds item (length nibble claims 8 data bytes in a 1-word ext). */
    uint8_t oob[] = {0x90, 96, 0, 1, 0, 0, 0, 9, 1, 2, 3, 4,
                     0xbe, 0xde, 0, 1, 0x37, 1, 2, 3};
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(oob, sizeof(oob), 3, seq));

    /* Target with wrong data length (1 byte) is invalid. */
    uint8_t wronglen[] = {0x90, 96, 0, 1, 0, 0, 0, 9, 1, 2, 3, 4,
                          0xbe, 0xde, 0, 1, 0x30, 0x55, 0, 0};
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(wronglen, sizeof(wronglen), 3, seq));

    /* ID 15 terminates scan; valid target before it counts. */
    uint8_t term[] = {0x90, 96, 0, 1, 0, 0, 0, 9, 1, 2, 3, 4,
                      0xbe, 0xde, 0, 1, 0x31, 0x77, 0x88, 0xF0};
    TASK_CHECK(uvgrtp::twcc::extract_transport_sequence(term, sizeof(term), 3, seq));
    TASK_CHECK(seq == 0x7788);

    /* One CSRC: extension starts 4 bytes later. */
    std::vector<uint8_t> csrc;
    csrc.push_back(0x91); /* CC=1, X=1 */
    csrc.push_back(96);
    /* seq, timestamp, SSRC */
    csrc.insert(csrc.end(), {0x00, 0x01, 0x00, 0x00, 0x00, 0x09,
                             0x12, 0x34, 0x56, 0x78});
    csrc.insert(csrc.end(), {0xDE, 0xAD, 0xBE, 0xEF}); /* CSRC */
    csrc.insert(csrc.end(), {0xBE, 0xDE, 0, 1, 0x31, 0x01, 0x02, 0});
    TASK_CHECK(uvgrtp::twcc::extract_transport_sequence(csrc.data(), csrc.size(), 3, seq));
    TASK_CHECK(seq == 0x0102);

    /* Truncated packet must not read out of bounds. */
    TASK_CHECK(!uvgrtp::twcc::extract_transport_sequence(baseline, 15, 3, seq));
    return 0;
}

static int wire_ingest_rejects_without_record(void)
{
    TwccState st;
    uint8_t cur = 0;
    TASK_CHECK(st.configure_stream(3, cur) == RTP_OK);

    /* Unknown id. */
    auto unknown = make_twcc_datagram(1, 100, 7, 4, 55);
    TASK_CHECK(!st.ingest_datagram(unknown.data(), unknown.size(), 1000000));
    TASK_CHECK(!st.has_pending());

    /* Matching id records. */
    auto good = make_twcc_datagram(1, 100, 7, 3, 55);
    TASK_CHECK(st.ingest_datagram(good.data(), good.size(), 1000000));
    TASK_CHECK(st.has_pending());

    /* Disabled state drops everything. */
    uint8_t rel = 3;
    TASK_CHECK(st.configure_stream(0, rel) == RTP_OK);
    TASK_CHECK(st.extension_id() == 0);
    auto again = make_twcc_datagram(1, 101, 7, 3, 56);
    TASK_CHECK(!st.ingest_datagram(again.data(), again.size(), 1000250));
    return 0;
}

/* ================================================================== */
/* LIFECYCLE GROUP                                                    */
/* ================================================================== */

static int lifecycle_config_accounting(void)
{
    TwccState st;
    uint8_t a = 0, b = 0, c = 0;

    TASK_CHECK(st.configure_stream(3, a) == RTP_OK);
    TASK_CHECK(a == 3 && st.extension_id() == 3 && st.user_count() == 1);

    /* Same id on a second stream uses the shared configuration. */
    TASK_CHECK(st.configure_stream(3, b) == RTP_OK);
    TASK_CHECK(b == 3 && st.user_count() == 2);

    /* Same stream repeating its id is idempotent. */
    TASK_CHECK(st.configure_stream(3, a) == RTP_OK);
    TASK_CHECK(st.user_count() == 2);

    /* Conflicting id on another/new token is rejected without changing it. */
    TASK_CHECK(st.configure_stream(4, c) == RTP_INVALID_VALUE);
    TASK_CHECK(c == 0 && st.extension_id() == 3 && st.user_count() == 2);

    /* Extension ids 15 and above are invalid; 0 on a fresh token is a no-op. */
    uint8_t bad = 0;
    TASK_CHECK(st.configure_stream(15, bad) == RTP_INVALID_VALUE);
    TASK_CHECK(st.configure_stream(200, bad) == RTP_INVALID_VALUE);
    TASK_CHECK(bad == 0);
    TASK_CHECK(st.configure_stream(0, c) == RTP_OK);
    TASK_CHECK(st.user_count() == 2);

    /* One release keeps shared state, last release clears it. */
    TASK_CHECK(st.configure_stream(0, a) == RTP_OK);
    TASK_CHECK(a == 0 && st.user_count() == 1 && st.extension_id() == 3);
    TASK_CHECK(st.configure_stream(0, b) == RTP_OK);
    TASK_CHECK(st.user_count() == 0 && st.extension_id() == 0);

    /* After clearing, a different id is allowed and feedback count was reset. */
    TASK_CHECK(st.configure_stream(9, a) == RTP_OK);
    TASK_CHECK(st.extension_id() == 9);
    TASK_CHECK(st.feedback_count() == 0);
    return 0;
}

static int lifecycle_scenarios(void)
{
    TwccState st;

    /* Cross-zero, gap + out-of-order fill, duplicate and cross-SSRC interleaving.
     * The ledger never sees SSRC: calls are interleaved as two media would. */
    TASK_CHECK(st.record_sequence(65534, 1000000));        /* SSRC A */
    TASK_CHECK(st.record_sequence(65535, 1000250));        /* SSRC B */
    TASK_CHECK(st.record_sequence(0, 1000500));            /* SSRC A */
    TASK_CHECK(st.record_sequence(2, 1000750));            /* SSRC B, gap at 1 */
    TASK_CHECK(st.record_sequence(1, 1000625));            /* SSRC A fills gap (seq-order time) */
    TASK_CHECK(!st.record_sequence(0, 99999999));          /* duplicate keeps first time */

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(1500, 0x42, p) == RTP_OK);
    TwccView v;
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.base == 65534);
    TASK_CHECK(v.status_count == 5);
    for (int i = 0; i < 5; ++i)
        TASK_CHECK(v.statuses[i] == 1);

    /* Late arrival after the interval was reported is not reported again. */
    TASK_CHECK(!st.record_sequence(65535, 2000000));

    /* A new sequence starts the next report at 3 (expanded 65539). */
    TASK_CHECK(st.record_sequence(3, 1001250));
    TASK_CHECK(st.poll_feedback(1500, 0x42, p) == RTP_OK);
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.base == 3 && v.status_count == 1);
    TASK_CHECK(v.feedback_count == 1);

    /* Duplicate of the pending sequence does not change its arrival. */
    TASK_CHECK(!st.record_sequence(3, 77777777));
    return 0;
}

static int lifecycle_late_after_feedback_loss(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(100, 1000000));
    TASK_CHECK(st.record_sequence(102, 1000500)); /* 101 lost for first report */

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(1500, 1, p) == RTP_OK);
    TwccView v;
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.status_count == 3);
    TASK_CHECK(v.statuses[0] == 1 && v.statuses[1] == 0 && v.statuses[2] == 1);

    /* Missing packet arrives after the report: recorded nowhere reportable. */
    TASK_CHECK(!st.record_sequence(101, 2000000));

    /* Only genuinely new sequence appears in the next report. */
    TASK_CHECK(st.record_sequence(103, 1000750));
    TASK_CHECK(st.poll_feedback(1500, 1, p) == RTP_OK);
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.base == 103 && v.status_count == 1);
    return 0;
}

static int lifecycle_trim_to_8192(void)
{
    TwccState st;
    /* 8201 contiguous sequences, one tick apart. */
    for (int i = 0; i <= 8200; ++i)
        TASK_CHECK(st.record_sequence(static_cast<uint16_t>(i), 1000000 + int64_t(i) * 250));

    TASK_CHECK(st.test_highest() == 8200);
    TASK_CHECK(st.test_arrival_count() == uvgrtp::twcc::MAX_RETAINED_SEQUENCES);
    TASK_CHECK(st.test_next_base() == 9); /* newest 8192: 9..8200 */

    std::vector<uint8_t> p;
    TASK_CHECK(st.poll_feedback(65535, 0x01020304, p) == RTP_OK);
    TASK_CHECK((p[12] << 8 | p[13]) == 9);
    TASK_CHECK(rd16(&p[14]) == 8192);
    TASK_CHECK(st.test_arrival_count() == 0);
    TASK_CHECK(!st.has_pending());
    return 0;
}

static int lifecycle_send_failure_does_not_advance(void)
{
    TwccState st;
    TASK_CHECK(st.record_sequence(1, 1000000));
    TASK_CHECK(st.record_sequence(2, 1000250));

    rtp_error_t rc = st.send_feedback(1500, 9,
        [](const std::vector<uint8_t>&) { return RTP_SEND_ERROR; });
    TASK_CHECK(rc == RTP_SEND_ERROR);
    TASK_CHECK(st.feedback_count() == 0);
    TASK_CHECK(st.test_next_base() == 1);
    TASK_CHECK(st.has_pending());

    /* A successful send through the same API advances exactly once. */
    std::vector<uint8_t> captured;
    rc = st.send_feedback(1500, 9, [&](const std::vector<uint8_t>& p) {
        captured = p;
        return RTP_OK;
    });
    TASK_CHECK(rc == RTP_OK);
    TASK_CHECK(captured.size() >= 20);
    TASK_CHECK(st.feedback_count() == 1);
    TASK_CHECK(!st.has_pending());
    return 0;
}

static int lifecycle_concurrent_record_and_poll(void)
{
    TwccState st;
    uint8_t cur = 0;
    TASK_CHECK(st.configure_stream(3, cur) == RTP_OK);

    std::atomic<bool> recording_done{false};
    std::thread recorder([&] {
        for (int i = 0; i < 3000; ++i) {
            (void)st.record_sequence(static_cast<uint16_t>(i), 1000000 + int64_t(i) * 250);
            if ((i % 64) == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        recording_done.store(true);
    });

    std::thread sender([&] {
        while (!recording_done.load() || st.has_pending()) {
            std::vector<uint8_t> p;
            rtp_error_t rc = st.poll_feedback(120, 0xABCDEF01, p);
            if (rc == RTP_OK)
                continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    recorder.join();
    sender.join();

    /* Drain anything left. */
    for (int k = 0; k < 10000; ++k) {
        std::vector<uint8_t> p;
        if (st.poll_feedback(120, 1, p) != RTP_OK)
            break;
    }
    TASK_CHECK(!st.has_pending());
    TASK_CHECK(st.test_arrival_count() == 0);
    TASK_CHECK(st.feedback_count() > 0);
    return 0;
}

/* ================================================================== */
/* TRANSPORT GROUP                                                    */
/* ================================================================== */

#ifndef _WIN32
static bool udp_bind_available()
{
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bool ok = (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    ::close(fd);
    return ok;
}

static uint16_t free_udp_port()
{
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t len = sizeof(a);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    uint16_t port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

struct UdpPeer {
    int fd = -1;
    uint16_t port = 0;
    bool listen()
    {
        fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return false;
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
            return false;
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        timeval tv{};
        tv.tv_usec = 50 * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return true;
    }
    ~UdpPeer()
    {
        if (fd >= 0)
            ::close(fd);
    }
    void send_to(uint16_t dst_port, const std::vector<uint8_t>& data)
    {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(dst_port);
        ::sendto(fd, data.data(), data.size(), 0,
                 reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    /* Receive datagrams until a TWCC FMT=15 packet arrives or timeout. */
    bool recv_twcc(TwccView& view, int total_wait_ms)
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(total_wait_ms);
        uint8_t buf[4096];
        while (std::chrono::steady_clock::now() < deadline) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                TwccView candidate;
                if (parse_twcc(buf, static_cast<size_t>(n), candidate)) {
                    view = candidate;
                    return true;
                }
            }
        }
        return false;
    }
};

struct HookState {
    std::atomic<int> count{0};
    std::atomic<uint32_t> last_seq{0};
    std::atomic<uint32_t> last_ts{0};
};

static void transport_hook(void* arg, uvgrtp::frame::rtp_frame* frame)
{
    auto* st = static_cast<HookState*>(arg);
    if (st && frame) {
        /* Publish sequence/timestamp before the count so a waiter that observes
         * the target count also sees the final metadata. */
        st->last_seq.store(frame->header.seq);
        st->last_ts.store(frame->header.timestamp);
        st->count.fetch_add(1);
    }
    uvgrtp::frame::dealloc_frame(frame);
}

static bool wait_count(HookState& st, int wanted, int timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (st.count.load() >= wanted)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return st.count.load() >= wanted;
}

/* In-process variant used only when the sandbox forbids bind(2). It exercises the
 * same TwccState record/feedback code and the real RTP parser for delivery. */
static int transport_inproc_fallback(void)
{
    auto shared = std::make_shared<TwccState>();
    uint8_t audio_token = 0, video_token = 0;
    TASK_CHECK(shared->configure_stream(3, audio_token) == RTP_OK);
    TASK_CHECK(shared->configure_stream(3, video_token) == RTP_OK);
    TASK_CHECK(shared->user_count() == 2);
    TASK_CHECK(shared->configure_stream(4, video_token) == RTP_INVALID_VALUE);

    auto ssrc_atomic = std::make_shared<std::atomic<uint32_t>>(0x12345678);
    uvgrtp::rtp media_parser(RTP_FORMAT_GENERIC, ssrc_atomic, false);

    struct Item { uint32_t ssrc; uint16_t mseq; uint16_t tseq; int64_t us; };
    Item items[] = {
        {0xA0A0A0A0, 100,   4000, 1000000},
        {0xA0A0A0A0, 101,   4001, 1000250},
        {0xB0B0B0B0, 7,     4004, 1000500},
        {0xA0A0A0A0, 50000, 4002, 1000750},
        {0xA0A0A0A0, 103,   4003, 1001000},
        {0xA0A0A0A0, 104,   4000, 1001250} /* duplicate transport seq */
    };

    for (const Item& it : items) {
        auto d = make_twcc_datagram(it.ssrc, it.mseq, 0x11223344, 3, it.tseq);
        TASK_CHECK(shared->ingest_datagram(d.data(), d.size(), it.us) ==
                   (it.tseq != 4000 || it.mseq == 100)); /* last 4000 is duplicate */

        /* Ordinary media parsing/delivery metadata is unaffected. */
        uvgrtp::frame::rtp_frame* frame = nullptr;
        rtp_error_t rc = media_parser.packet_handler(nullptr, 0, d.data(), d.size(), &frame);
        TASK_CHECK(rc == RTP_PKT_MODIFIED);
        TASK_CHECK(frame != nullptr);
        TASK_CHECK(frame->header.seq == it.mseq);
        TASK_CHECK(frame->header.timestamp == 0x11223344);
        uvgrtp::frame::dealloc_frame(frame);
    }

    std::vector<uint8_t> p;
    TASK_CHECK(shared->poll_feedback(1500, 0x12345678, p) == RTP_OK);
    TwccView v;
    TASK_CHECK(parse_twcc(p.data(), p.size(), v));
    TASK_CHECK(v.base == 4000 && v.status_count == 5);
    /* Sequence-order times are 0,250,750,1000,500 us: the last delta is -2 ticks,
     * which is a legal two-byte symbol 2 rather than a one-byte symbol 1. */
    TASK_CHECK(v.statuses.size() == 5);
    TASK_CHECK(v.statuses[0] == 1 && v.statuses[1] == 1 &&
               v.statuses[2] == 1 && v.statuses[3] == 1 && v.statuses[4] == 2);
    return 0;
}
#endif /* !_WIN32 */

static int transport_shared_socket(void)
{
#ifdef _WIN32
    /* POSIX UDP harness not ported in this task environment. */
    TASK_CHECK(true);
    return 0;
#else
    if (!udp_bind_available()) {
        std::fprintf(stderr, "UDP bind unavailable; running in-process transport fallback\n");
        return transport_inproc_fallback();
    }

    UdpPeer peer_a, peer_b;
    TASK_CHECK(peer_a.listen());
    TASK_CHECK(peer_b.listen());
    uint16_t uvg_port = free_udp_port();
    TASK_CHECK(uvg_port != 0);

    uvgrtp::context ctx;
    uvgrtp::session* sess = ctx.create_session("127.0.0.1");
    TASK_CHECK(sess != nullptr);

    int flags = RCE_RTCP | RCE_RTCP_MUX;
    uvgrtp::media_stream* audio =
        sess->create_stream(uvg_port, peer_a.port, RTP_FORMAT_PCMU, flags);
    uvgrtp::media_stream* video =
        sess->create_stream(uvg_port, peer_b.port, RTP_FORMAT_H264, flags);
    TASK_CHECK(audio != nullptr);
    TASK_CHECK(video != nullptr);

    constexpr uint32_t REMOTE_A = 0xA0A0A0A0u;
    constexpr uint32_t REMOTE_B = 0xB0B0B0B0u;
    constexpr uint32_t LOCAL_A = 0x0C0C0C0Cu;
    constexpr uint32_t LOCAL_B = 0x0D0D0D0Du;

    TASK_CHECK(audio->configure_ctx(RCC_REMOTE_SSRC, (ssize_t)REMOTE_A) == RTP_OK);
    TASK_CHECK(video->configure_ctx(RCC_REMOTE_SSRC, (ssize_t)REMOTE_B) == RTP_OK);
    TASK_CHECK(audio->configure_ctx(RCC_SSRC, (ssize_t)LOCAL_A) == RTP_OK);
    TASK_CHECK(video->configure_ctx(RCC_SSRC, (ssize_t)LOCAL_B) == RTP_OK);

    HookState hook_a, hook_b;
    TASK_CHECK(audio->install_receive_hook(&hook_a, transport_hook) == RTP_OK);
    /* The H264 hook may aggregate packets; it only supplements the audio delivery
     * assertion, so callers synchronize video arrivals with a short settle. */
    TASK_CHECK(video->install_receive_hook(&hook_b, transport_hook) == RTP_OK);

    /* Shared extension configuration. */
    TASK_CHECK(audio->configure_twcc(3) == RTP_OK);
    TASK_CHECK(audio->configure_twcc(3) == RTP_OK); /* idempotent */
    TASK_CHECK(video->configure_twcc(3) == RTP_OK); /* shares existing config */
    TASK_CHECK(video->configure_twcc(4) == RTP_INVALID_VALUE);
    TASK_CHECK(audio->configure_twcc(15) == RTP_INVALID_VALUE);

    auto send_a = [&](uint16_t mseq, uint16_t tseq) {
        auto d = make_twcc_datagram(REMOTE_A, mseq, 0x11223344, 3, tseq);
        peer_a.send_to(uvg_port, d);
    };
    auto send_b = [&](uint16_t mseq, uint16_t tseq, uint32_t rtp_ts = 0x55667788) {
        auto d = make_twcc_datagram(REMOTE_B, mseq, rtp_ts, 3, tseq);
        peer_b.send_to(uvg_port, d);
    };

    /* Cross-SSRC interleaving, gap then fill, unrelated media sequences. */
    send_a(100, 4000);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    send_a(101, 4001);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    send_b(7, 4004);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    send_a(50000, 4002);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    send_a(103, 4003);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    send_a(104, 4000); /* duplicate transport sequence, new media sequence */

    TASK_CHECK(wait_count(hook_a, 5, 3000));
    /* The video datagram is a single H264 NAL and is delivered through its hook. */
    TASK_CHECK(wait_count(hook_b, 1, 3000));
    TASK_CHECK(hook_a.last_seq.load() == 104);
    TASK_CHECK(hook_a.last_ts.load() == 0x11223344);
    TASK_CHECK(hook_b.last_seq.load() == 7);

    /* Audio stream triggers and reads back a real RTCP datagram. */
    TASK_CHECK(audio->get_rtcp() != nullptr);
    TASK_CHECK(audio->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
    TwccView view;
    TASK_CHECK(peer_a.recv_twcc(view, 3000));
    TASK_CHECK(view.sender_ssrc == LOCAL_A);
    TASK_CHECK(view.media_ssrc == 0);
    TASK_CHECK(view.base == 4000);
    TASK_CHECK(view.status_count == 5);
    TASK_CHECK(view.feedback_count == 0);
    /* Every status must mean "received with a delta"; out-of-order arrival can make
     * the sequence-order delta negative, which is encoded as symbol 2. */
    TASK_CHECK(view.statuses.size() == 5);
    for (uint8_t s : view.statuses)
        TASK_CHECK(s == 1 || s == 2);

    /* The shared interval was fully reported: the other stream finds nothing. */
    TASK_CHECK(video->get_rtcp()->send_twcc_feedback(1500) == RTP_NOT_FOUND);

    /* Releasing the audio stream revokes only its own right to trigger. */
    TASK_CHECK(audio->configure_twcc(0) == RTP_OK);
    TASK_CHECK(audio->get_rtcp()->send_twcc_feedback(1500) == RTP_NOT_FOUND);

    /* New transport sequence on video; shared feedback count is now 1. */
    send_b(8, 4005, 0x55667789);
    TASK_CHECK(wait_count(hook_b, 2, 3000));
    TASK_CHECK(video->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
    TASK_CHECK(peer_b.recv_twcc(view, 3000));
    TASK_CHECK(view.sender_ssrc == LOCAL_B);
    TASK_CHECK(view.base == 4005 && view.status_count == 1);
    TASK_CHECK(view.feedback_count == 1);

    /* Last user release clears shared state and revokes reports. */
    TASK_CHECK(video->configure_twcc(0) == RTP_OK);
    TASK_CHECK(video->get_rtcp()->send_twcc_feedback(1500) == RTP_NOT_FOUND);

    /* Recovery: after full release a new id is accepted and count restarts at 0. */
    TASK_CHECK(audio->configure_twcc(5) == RTP_OK);
    auto recovered = make_twcc_datagram(REMOTE_A, 200, 0x11223344, 5, 7000);
    peer_a.send_to(uvg_port, recovered);
    TASK_CHECK(wait_count(hook_a, 6, 3000));
    TASK_CHECK(audio->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
    TASK_CHECK(peer_a.recv_twcc(view, 3000));
    TASK_CHECK(view.base == 7000 && view.status_count == 1);
    TASK_CHECK(view.feedback_count == 0);

    sess->destroy_stream(audio);
    sess->destroy_stream(video);
    ctx.destroy_session(sess);
    return 0;
#endif
}

static int transport_other_sockets_independent(void)
{
    /* Core guarantee: separate socket flows own separate TwccState objects. */
    TwccState one, two;
    uint8_t a = 0, b = 0;
    TASK_CHECK(one.configure_stream(3, a) == RTP_OK);
    TASK_CHECK(two.configure_stream(4, b) == RTP_OK);
    TASK_CHECK(one.extension_id() == 3 && two.extension_id() == 4);

    TASK_CHECK(one.record_sequence(10, 1000000));
    TASK_CHECK(two.record_sequence(20, 2000000));
    TASK_CHECK(one.test_next_base() == 10);
    TASK_CHECK(two.test_next_base() == 20);

    std::vector<uint8_t> p1, p2;
    TASK_CHECK(one.poll_feedback(1500, 1, p1) == RTP_OK);
    TASK_CHECK(two.poll_feedback(1500, 2, p2) == RTP_OK);
    TwccView v1, v2;
    TASK_CHECK(parse_twcc(p1.data(), p1.size(), v1));
    TASK_CHECK(parse_twcc(p2.data(), p2.size(), v2));
    TASK_CHECK(v1.base == 10 && v1.sender_ssrc == 1);
    TASK_CHECK(v2.base == 20 && v2.sender_ssrc == 2);
    TASK_CHECK(one.feedback_count() == 1 && two.feedback_count() == 1);

#ifndef _WIN32
    /* When binding works, prove a second uvgRTP socket stays independent too. */
    if (udp_bind_available()) {
        UdpPeer peer_c;
        TASK_CHECK(peer_c.listen());
        uint16_t port = free_udp_port();
        TASK_CHECK(port != 0);

        uvgrtp::context ctx;
        uvgrtp::session* sess = ctx.create_session("127.0.0.1");
        TASK_CHECK(sess != nullptr);
        uvgrtp::media_stream* other =
            sess->create_stream(port, peer_c.port, RTP_FORMAT_PCMU, RCE_RTCP | RCE_RTCP_MUX);
        TASK_CHECK(other != nullptr);
        TASK_CHECK(other->configure_ctx(RCC_REMOTE_SSRC, (ssize_t)0xC3C3C3C3u) == RTP_OK);
        TASK_CHECK(other->configure_twcc(11) == RTP_OK);

        auto d = make_twcc_datagram(0xC3C3C3C3u, 1, 9, 11, 321);
        peer_c.send_to(port, d);
        /* Give the receiver thread a moment to account the datagram. */
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            /* There is no hook here; a fixed short settle is sufficient. */
            if (i == 19)
                break;
        }
        TASK_CHECK(other->get_rtcp()->send_twcc_feedback(1500) == RTP_OK);
        TwccView view;
        TASK_CHECK(peer_c.recv_twcc(view, 3000));
        TASK_CHECK(view.base == 321);

        sess->destroy_stream(other);
        ctx.destroy_session(sess);
    }
#endif
    return 0;
}

/* ------------------------------------------------------------------ */
/* case registry (consumed by task_support/driver.c)                  */
/* ------------------------------------------------------------------ */

extern const struct task_case task_cases[] = {
    {"transport", "shared_socket", transport_shared_socket},
    {"transport", "other_sockets_independent", transport_other_sockets_independent},

    {"wire", "golden_three_small_deltas", wire_golden_three_small_deltas},
    {"wire", "golden_loss", wire_golden_loss},
    {"wire", "golden_large_delta", wire_golden_large_delta},
    {"wire", "golden_zero_crossing", wire_golden_zero_crossing},
    {"wire", "budget_prefixes_and_count", wire_budget_prefixes_and_count},
    {"wire", "tiny_budget_no_mutation", wire_tiny_budget_no_mutation},
    {"wire", "no_records_is_not_found", wire_no_records_is_not_found},
    {"wire", "unencodable_delta_restarts_reference", wire_unencodable_delta_restarts_reference},
    {"wire", "extension_parser", wire_extension_parser},
    {"wire", "ingest_rejects_without_record", wire_ingest_rejects_without_record},

    {"lifecycle", "config_accounting", lifecycle_config_accounting},
    {"lifecycle", "scenarios", lifecycle_scenarios},
    {"lifecycle", "late_after_feedback_loss", lifecycle_late_after_feedback_loss},
    {"lifecycle", "trim_to_8192", lifecycle_trim_to_8192},
    {"lifecycle", "send_failure_does_not_advance", lifecycle_send_failure_does_not_advance},
    {"lifecycle", "concurrent_record_and_poll", lifecycle_concurrent_record_and_poll},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
