/******************************************************************************
*   Regression checks for the fixed-rate RTP repair path (RFC 4588 style).
*   Packets are synthesised byte by byte and every timestamp is handed to the
*   API explicitly, so no real clock or network is involved.
******************************************************************************/

#include "rtp_jitter.h"
#include "task_support/check.h"
#include <arpa/inet.h>
#include <cstring>
#include <vector>

static const uint32 PRI_SSRC = 0x11111111u;
static const uint32 RTX_SSRC = 0x22222222u;
static const uint8  PRI_PT   = 96;
static const uint8  RTX_PT   = 97;
static const int64  A        = 1000000;     // anchor arrival (us)
static const int64  SLOT_US  = 20000;
static const int64  DELAY_US = 60000;
static const int64  RETRY_US = 40000;
static const uint32 BASE_TS  = 0x10203040u;
static const int64  TS_STEP  = 160;         // 8000 * 20 / 1000

struct Spec
{
    uint16              seq;
    uint32              ts;
    uint32              ssrc;
    uint8               pt;
    uint8               marker;
    std::vector<uint8>  media;
    std::vector<uint32> csrc;
    bool                has_ext;
    uint16              profile;
    std::vector<uint8>  ext;
    int                 padding;
    int                 version;
    int                 length_override;
    int                 corrupt_padding;
    bool                corrupt_extension;

    Spec()
        : seq(0), ts(0), ssrc(0), pt(0), marker(0),
          has_ext(false), profile(0), padding(0), version(2),
          length_override(-1), corrupt_padding(-1),
          corrupt_extension(false)
    {
    }
};

static rawrtp_ptr build(const Spec& s)
{
    std::vector<uint8> wire;

    wire.push_back((uint8)(((s.version & 3) << 6)
                         | (s.padding ? 0x20 : 0)
                         | (s.has_ext ? 0x10 : 0)
                         | ((int)s.csrc.size() & 0x0F)));
    wire.push_back((uint8)((s.marker ? 0x80 : 0) | (s.pt & 0x7F)));

    wire.push_back((uint8)(s.seq >> 8));
    wire.push_back((uint8)(s.seq & 0xFF));

    wire.push_back((uint8)(s.ts >> 24));
    wire.push_back((uint8)(s.ts >> 16));
    wire.push_back((uint8)(s.ts >> 8));
    wire.push_back((uint8)(s.ts));

    wire.push_back((uint8)(s.ssrc >> 24));
    wire.push_back((uint8)(s.ssrc >> 16));
    wire.push_back((uint8)(s.ssrc >> 8));
    wire.push_back((uint8)(s.ssrc));

    for (size_t i = 0; i < s.csrc.size(); ++i) {
        wire.push_back((uint8)(s.csrc[i] >> 24));
        wire.push_back((uint8)(s.csrc[i] >> 16));
        wire.push_back((uint8)(s.csrc[i] >> 8));
        wire.push_back((uint8)(s.csrc[i]));
    }

    if (s.has_ext) {
        wire.push_back((uint8)(s.profile >> 8));
        wire.push_back((uint8)(s.profile & 0xFF));
        uint16 words = (uint16)(s.ext.size() / 4);
        if (s.corrupt_extension) {
            words = (uint16)(words + 100);
        }
        wire.push_back((uint8)(words >> 8));
        wire.push_back((uint8)(words & 0xFF));
        wire.insert(wire.end(), s.ext.begin(), s.ext.end());
    }

    wire.insert(wire.end(), s.media.begin(), s.media.end());

    if (s.padding > 0) {
        for (int i = 0; i < s.padding - 1; ++i) {
            wire.push_back(0);
        }
        wire.push_back((uint8)(s.corrupt_padding >= 0 ? s.corrupt_padding : s.padding));
    }

    if ((s.length_override >= 0) && ((size_t)s.length_override < wire.size())) {
        wire.resize((size_t)s.length_override);
    }

    return rawrtp_ptr(new RTPPacket(wire.data(), (short)wire.size()));
}

static rawrtp_ptr primary(uint16 seq, uint32 ts, const std::vector<uint8>& media,
                          uint8 marker = 0)
{
    Spec s;
    s.seq    = seq;
    s.ts     = ts;
    s.ssrc   = PRI_SSRC;
    s.pt     = PRI_PT;
    s.marker = marker;
    s.media  = media;
    return build(s);
}

static rawrtp_ptr rtx(uint16 osn, uint32 ts, uint16 outer_seq,
                      const std::vector<uint8>& media, uint8 marker = 0)
{
    std::vector<uint8> payload;
    payload.push_back((uint8)(osn >> 8));
    payload.push_back((uint8)(osn & 0xFF));
    payload.insert(payload.end(), media.begin(), media.end());

    Spec s;
    s.seq    = outer_seq;
    s.ts     = ts;
    s.ssrc   = RTX_SSRC;
    s.pt     = RTX_PT;
    s.marker = marker;
    s.media  = payload;
    return build(s);
}

static RepairConfig base_config()
{
    RepairConfig c;
    c.primary_ssrc = PRI_SSRC;
    c.rtx_ssrc     = RTX_SSRC;
    c.primary_pt   = PRI_PT;
    c.rtx_pt       = RTX_PT;
    c.clock_rate   = 8000;
    c.packet_ms    = 20;
    c.delay_ms     = 60;
    c.retry_ms     = 40;
    c.window_packets = 8;
    return c;
}

static int64 slot_deadline(int64 slot)
{
    return A + DELAY_US + slot * SLOT_US;
}

static uint32 slot_ts(int64 slot, uint32 base = BASE_TS)
{
    return (uint32)(base + (uint32)(slot * TS_STEP));
}

static uint16 slot_seq(uint16 anchor, int64 slot)
{
    return (uint16)(anchor + (uint16)slot);
}

static uint16 pseq(const rawrtp_ptr& p)
{
    int16 v;
    memcpy(&v, p->pData + 2, 2);
    return (uint16)ntohs((uint16)v);
}

static uint32 pts(const rawrtp_ptr& p)
{
    uint32 v;
    memcpy(&v, p->pData + 4, 4);
    return ntohl(v);
}

static uint32 pssrc(const rawrtp_ptr& p)
{
    uint32 v;
    memcpy(&v, p->pData + 8, 4);
    return ntohl(v);
}

static uint8 ppt(const rawrtp_ptr& p)
{
    return (uint8)(p->pData[1] & 0x7F);
}

static uint8 pmarker(const rawrtp_ptr& p)
{
    return (uint8)(p->pData[1] & 0x80);
}

// compare the media bytes of a packet (payload minus padding) to expectation
static bool media_equals(const rawrtp_ptr& p, const std::vector<uint8>& expected)
{
    RTPPacket::Layout layout;
    if (!p || !p->parse_layout(layout)) {
        return false;
    }
    if (layout.media_len != (uint16)expected.size()) {
        return false;
    }
    return memcmp(p->pData + layout.payload_offset, expected.data(),
                  expected.size()) == 0;
}


/* ------------------------------------------------------------------------- */
/* normalize                                                                  */
/* ------------------------------------------------------------------------- */

static int case_config_validation(void)
{
    RTPJitter q(20);
    RepairConfig good = base_config();
    RepairConfig bad;

    TASK_CHECK(q.repair_configured() == false);
    TASK_CHECK(q.configure_repair(good) == true);
    TASK_CHECK(q.repair_configured() == true);

    bad = good; bad.rtx_ssrc = good.primary_ssrc;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.rtx_pt = good.primary_pt;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.primary_pt = 128;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.rtx_pt = -1;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.clock_rate = 0;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.clock_rate = -8000;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.retry_ms = 0;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.retry_ms = -1;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.packet_ms = 0;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.packet_ms = 65536;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.delay_ms = -1;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.window_packets = 1;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.window_packets = 513;
    TASK_CHECK(q.configure_repair(bad) == false);
    bad = good; bad.clock_rate = 11; bad.packet_ms = 100;    // 1100/1000 not whole
    TASK_CHECK(q.configure_repair(bad) == false);

    // failures above must not have disturbed the accepted configuration
    TASK_CHECK(q.push_at(primary(500, slot_ts(0), std::vector<uint8>{0x55}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.configure_repair(good) == true);
    TASK_CHECK(q.get_depth() == 0);          // success clears the old stream
    TASK_CHECK(q.push_at(primary(500, slot_ts(0), std::vector<uint8>{0x55}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);
    return 0;
}

static int case_config_clears_legacy(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.push(primary(10, 100, std::vector<uint8>{0x66})) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.configure_repair(base_config()) == true);
    TASK_CHECK(q.get_depth() == 0);
    TASK_CHECK(q.get_depth_ms() == 0);
    TASK_CHECK(q.pop(out) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.push(primary(10, 100, std::vector<uint8>{0x66})) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 0);
    return 0;
}

static int case_rtx_decode(void)
{
    std::vector<uint8> body = {0x0A, 0x0B, 0x0C, 0x0D};
    rawrtp_ptr p = rtx(42, BASE_TS, 60000, body, 0x80);
    uint16 before_len = p->nLen;
    std::vector<uint8> before(p->pData, p->pData + p->nLen);

    rawrtp_ptr restored = RTPPacket::restore_rtx(*p, 42, PRI_SSRC, PRI_PT, 20);

    TASK_CHECK(restored.get() != NULL);
    TASK_CHECK(pseq(restored) == 42);
    TASK_CHECK(pts(restored) == BASE_TS);
    TASK_CHECK(pssrc(restored) == PRI_SSRC);
    TASK_CHECK(ppt(restored) == PRI_PT);
    TASK_CHECK(pmarker(restored) == 0x80);
    TASK_CHECK(media_equals(restored, body));
    TASK_CHECK(restored->payload_bytes == body.size());
    TASK_CHECK(restored->payload_ms == 20);
    TASK_CHECK(restored->payload_type == PRI_PT);
    TASK_CHECK(restored->nLen == (uint16)(before_len - 2));

    // the source object must be untouched
    TASK_CHECK(p->nLen == before_len);
    TASK_CHECK(memcmp(p->pData, before.data(), before.size()) == 0);

    // the outer sequence number plays no part in the result
    rawrtp_ptr other = rtx(42, BASE_TS, 7, body);
    rawrtp_ptr restored2 = RTPPacket::restore_rtx(*other, 42, PRI_SSRC, PRI_PT, 20);
    TASK_CHECK(restored2.get() != NULL);
    TASK_CHECK(pseq(restored2) == 42);
    return 0;
}

static int case_rtx_envelope(void)
{
    uint16 osn = 777;
    uint32 ts  = 0xDEADBEEF;
    std::vector<uint8> media = {0x11, 0x22, 0x33, 0x44, 0x55};

    // build the RTX packet by hand so CSRCs, extension and padding are present
    Spec s;
    s.seq    = 321;
    s.ts     = ts;
    s.ssrc   = RTX_SSRC;
    s.pt     = RTX_PT;
    s.marker = 0x80;
    s.media.push_back((uint8)(osn >> 8));
    s.media.push_back((uint8)(osn & 0xFF));
    s.media.insert(s.media.end(), media.begin(), media.end());
    s.csrc.push_back(0xAABBCCDD);
    s.csrc.push_back(0x01020304);
    s.has_ext = true;
    s.profile = 0xBEDE;
    s.ext = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    s.padding = 4;
    rawrtp_ptr p = build(s);

    RTPPacket::Layout rtx_layout;
    TASK_CHECK(p->parse_layout(rtx_layout));
    TASK_CHECK(rtx_layout.header_len == 12 + 8 + 4 + 8);
    TASK_CHECK(rtx_layout.padding_len == 4);
    TASK_CHECK(rtx_layout.media_len == 7);

    std::vector<uint8> original(p->pData, p->pData + p->nLen);
    rawrtp_ptr restored = RTPPacket::restore_rtx(*p, osn, PRI_SSRC, PRI_PT, 20);
    TASK_CHECK(restored.get() != NULL);

    RTPPacket::Layout rs;
    TASK_CHECK(restored->parse_layout(rs));
    TASK_CHECK(rs.header_len == rtx_layout.header_len);
    TASK_CHECK(rs.payload_len == (uint16)(rtx_layout.payload_len - 2));
    TASK_CHECK(rs.padding_len == 4);
    TASK_CHECK(rs.media_len == 5);

    // header: sequence, SSRC and payload type are rewritten, marker preserved
    TASK_CHECK(pseq(restored) == osn);
    TASK_CHECK(pssrc(restored) == PRI_SSRC);
    TASK_CHECK(ppt(restored) == PRI_PT);
    TASK_CHECK(pmarker(restored) == 0x80);
    TASK_CHECK(pts(restored) == ts);
    TASK_CHECK(restored->pData[0] == p->pData[0]);

    // CSRCs and extension are copied verbatim
    TASK_CHECK(memcmp(restored->pData + 12, p->pData + 12, 4 * 2) == 0);
    TASK_CHECK(memcmp(restored->pData + 20, p->pData + 20, 4 + 8) == 0);

    // media bytes and padding are preserved
    TASK_CHECK(media_equals(restored, media));
    TASK_CHECK(restored->pData[restored->nLen - 1] == 4);
    TASK_CHECK(restored->pData[restored->nLen - 2] == 0);
    TASK_CHECK(restored->payload_bytes == 5);

    TASK_CHECK(memcmp(p->pData, original.data(), original.size()) == 0);
    return 0;
}

static int case_reject_format(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    std::vector<uint8> body = {0x01, 0x02};

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(10, slot_ts(0), body), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);

    // wrong version
    Spec bad;
    bad.seq = 11; bad.ts = slot_ts(1); bad.ssrc = PRI_SSRC; bad.pt = PRI_PT;
    bad.version = 1; bad.media = body;
    TASK_CHECK(q.push_at(build(bad), A + 1000) == RTPJitter::BAD_PACKET);

    // truncated fixed header
    bad.version = 2; bad.length_override = 10;
    TASK_CHECK(q.push_at(build(bad), A + 1000) == RTPJitter::BAD_PACKET);

    // padding larger than the payload
    Spec pad;
    pad.seq = 11; pad.ts = slot_ts(1); pad.ssrc = PRI_SSRC; pad.pt = PRI_PT;
    pad.media = body; pad.padding = 9; pad.corrupt_padding = 40;
    TASK_CHECK(q.push_at(build(pad), A + 1000) == RTPJitter::BAD_PACKET);

    // extension length beyond the packet
    Spec ext;
    ext.seq = 11; ext.ts = slot_ts(1); ext.ssrc = PRI_SSRC; ext.pt = PRI_PT;
    ext.media = body; ext.has_ext = true; ext.ext = {0,0,0,0};
    ext.corrupt_extension = true;
    TASK_CHECK(q.push_at(build(ext), A + 1000) == RTPJitter::BAD_PACKET);

    // RTX payload shorter than the two byte OSN
    Spec shortrtx;
    shortrtx.seq = 1; shortrtx.ts = slot_ts(1); shortrtx.ssrc = RTX_SSRC;
    shortrtx.pt = RTX_PT; shortrtx.media = {0x00};
    TASK_CHECK(q.push_at(build(shortrtx), A + 1000) == RTPJitter::BAD_PACKET);

    // wrong SSRC / payload type on the media plane
    Spec wrong;
    wrong.seq = 11; wrong.ts = slot_ts(1); wrong.ssrc = 0x33333333;
    wrong.pt = PRI_PT; wrong.media = body;
    TASK_CHECK(q.push_at(build(wrong), A + 1000) == RTPJitter::BAD_PACKET);

    wrong.ssrc = PRI_SSRC; wrong.pt = RTX_PT;
    TASK_CHECK(q.push_at(build(wrong), A + 1000) == RTPJitter::BAD_PACKET);

    // restore_rtx itself also rejects malformed input
    rawrtp_ptr v1 = build(bad);
    TASK_CHECK(RTPPacket::restore_rtx(*v1, 11, PRI_SSRC, PRI_PT, 20).get() == NULL);

    // no side effects from any of the rejects
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.nacks_at(A + 2000).empty());
    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 10);
    return 0;
}

static int case_pre_anchor_rtx_fixed(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    std::vector<uint8> body = {0x33};

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(rtx(100, slot_ts(0), 5, body), A) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 0);
    TASK_CHECK(q.nacks_at(A).empty());
    TASK_CHECK(q.pop_at(out, A) == RTPJitter::BUFFERING);
    TASK_CHECK(out.get() == NULL);

    // the first legal media packet establishes the anchor
    TASK_CHECK(q.push_at(primary(100, slot_ts(0), body), A + 10) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);

    // an RTX packet for that very slot now duplicates the media packet
    TASK_CHECK(q.push_at(rtx(100, slot_ts(0), 500, std::vector<uint8>{0x44}), A + 20)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.pop_at(out, A + 10 + DELAY_US) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 100);
    TASK_CHECK(media_equals(out, body));
    return 0;
}


/* ------------------------------------------------------------------------- */
/* feedback                                                                   */
/* ------------------------------------------------------------------------- */

static int case_reveal_order(void)
{
    RTPJitter q(20);
    std::vector<uint16> n;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(100, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(103, slot_ts(3), std::vector<uint8>{0x04}), A + 1000) == RTPJitter::SUCCESS);

    n = q.nacks_at(A + 2000);
    TASK_CHECK(n.size() == 2);
    TASK_CHECK(n[0] == 101);
    TASK_CHECK(n[1] == 102);
    TASK_CHECK(q.nacks_at(A + 3000).empty());

    // filling the hole from a repair packet removes it from feedback
    TASK_CHECK(q.push_at(rtx(102, slot_ts(2), 999, std::vector<uint8>{0x03}), A + 3000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.nacks_at(A + 4000).empty());

    // the remaining hole is still retried after the interval
    n = q.nacks_at(A + 2000 + RETRY_US);
    TASK_CHECK(n.size() == 1);
    TASK_CHECK(n[0] == 101);

    // an original packet for the hole also fills it
    TASK_CHECK(q.push_at(primary(101, slot_ts(1), std::vector<uint8>{0x02}),
                         A + 2000 + RETRY_US + 1000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.nacks_at(A + 2000 + RETRY_US + 2000).empty());
    return 0;
}

static int case_retry_interval(void)
{
    RTPJitter q(20);
    RepairConfig c = base_config();
    c.delay_ms = 200;       // long enough for a late (skipped) round
    std::vector<uint16> n;
    int64 d = A + 200 * 1000 + SLOT_US;     // deadline of slot 1

    TASK_CHECK(q.configure_repair(c));
    TASK_CHECK(q.push_at(primary(200, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(202, slot_ts(2), std::vector<uint8>{0x03}), A + 1000) == RTPJitter::SUCCESS);

    n = q.nacks_at(A + 2000);
    TASK_CHECK(n.size() == 1 && n[0] == 201);

    // a repeated query does not re-report
    TASK_CHECK(q.nacks_at(A + 2000).empty());
    TASK_CHECK(q.nacks_at(A + 2000 + RETRY_US - 1).empty());

    // retry only after the full retry interval
    n = q.nacks_at(A + 2000 + RETRY_US);
    TASK_CHECK(n.size() == 1 && n[0] == 201);
    TASK_CHECK(q.nacks_at(A + 2000 + RETRY_US).empty());
    TASK_CHECK(d > A + 2000 + 3 * RETRY_US);

    // a query that skipped several rounds reports once and does not replay
    // the missed rounds
    n = q.nacks_at(A + 2000 + 3 * RETRY_US);
    TASK_CHECK(n.size() == 1 && n[0] == 201);
    TASK_CHECK(q.nacks_at(A + 2000 + 3 * RETRY_US).empty());
    return 0;
}

static int case_repair_cancels_retry(void)
{
    RTPJitter q(20);
    std::vector<uint16> n;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(300, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(303, slot_ts(3), std::vector<uint8>{0x04}), A + 1000) == RTPJitter::SUCCESS);

    n = q.nacks_at(A + 2000);
    TASK_CHECK(n.size() == 2 && n[0] == 301 && n[1] == 302);
    TASK_CHECK(q.nacks_at(A + 3000).empty());

    TASK_CHECK(q.push_at(rtx(301, slot_ts(1), 11, std::vector<uint8>{0x02}), A + 3000)
               == RTPJitter::SUCCESS);
    n = q.nacks_at(A + 2000 + RETRY_US);
    TASK_CHECK(n.size() == 1 && n[0] == 302);

    // expired holes disappear without ever being consumed
    TASK_CHECK(q.nacks_at(A + DELAY_US + 2 * SLOT_US + 1).empty());
    return 0;
}

static int case_consumed_hole_gone(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(400, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(402, slot_ts(2), std::vector<uint8>{0x03}), A + 1000) == RTPJitter::SUCCESS);

    TASK_CHECK(q.nacks_at(A + 2000).size() == 1);
    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 400);
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::DROPPED_PACKET);

    // the hole was consumed: no more feedback, and playing it back is too late
    TASK_CHECK(q.nacks_at(A + DELAY_US + SLOT_US + 1000).empty());
    TASK_CHECK(q.push_at(primary(401, slot_ts(1), std::vector<uint8>{0x02}),
                         A + DELAY_US + SLOT_US + 2000) == RTPJitter::BAD_PACKET);
    return 0;
}

static int case_no_phantom_gap(void)
{
    RTPJitter q(20);
    std::vector<uint16> n;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(500, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(505, slot_ts(5), std::vector<uint8>{0x06}), A + 1000) == RTPJitter::SUCCESS);

    n = q.nacks_at(A + 2000);
    TASK_CHECK(n.size() == 4);
    TASK_CHECK(n[0] == 501 && n[1] == 502 && n[2] == 503 && n[3] == 504);
    TASK_CHECK(q.nacks_at(A + 3000).empty());

    // nothing beyond the newest known sequence is manufactured
    for (size_t i = 0; i < n.size(); ++i) {
        TASK_CHECK(n[i] <= 504);
    }
    return 0;
}

static int case_query_no_side_effect(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(600, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(603, slot_ts(3), std::vector<uint8>{0x04}), A + 1000) == RTPJitter::SUCCESS);

    int64 before_depth = q.get_depth();
    TASK_CHECK(q.nacks_at(A + 2000).size() == 2);
    TASK_CHECK(q.get_depth() == before_depth);

    // playout still starts at the anchor and has not advanced
    TASK_CHECK(q.pop_at(out, slot_deadline(0) - 1) == RTPJitter::BUFFERING);
    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 600);

    // the anchor is unchanged: a later slot still maps correctly
    TASK_CHECK(q.push_at(primary(604, slot_ts(4), std::vector<uint8>{0x05}), A + 3000)
               == RTPJitter::SUCCESS);
    return 0;
}

static int case_window_overflow(void)
{
    RTPJitter q(20);
    RepairConfig c = base_config();
    c.window_packets = 4;
    std::vector<uint16> n;

    TASK_CHECK(q.configure_repair(c));
    TASK_CHECK(q.push_at(primary(700, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);

    // exactly outside the window from the current playout head
    TASK_CHECK(q.push_at(primary(704, slot_ts(4), std::vector<uint8>{0x05}), A + 1000)
               == RTPJitter::BUFFER_OVERFLOW);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.nacks_at(A + 1000).empty());

    // the last slot inside the window is accepted
    TASK_CHECK(q.push_at(primary(703, slot_ts(3), std::vector<uint8>{0x04}), A + 2000)
               == RTPJitter::SUCCESS);
    n = q.nacks_at(A + 3000);
    TASK_CHECK(n.size() == 2 && n[0] == 701 && n[1] == 702);
    TASK_CHECK(q.nacks_at(A + 4000).empty());

    // after the head advances, slot 4 becomes reachable
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(704, slot_ts(4), std::vector<uint8>{0x05}), A + 4000)
               == RTPJitter::SUCCESS);

    // and slot 5 now is out of range
    TASK_CHECK(q.push_at(primary(705, slot_ts(5), std::vector<uint8>{0x06}), A + 5000)
               == RTPJitter::BUFFER_OVERFLOW);
    return 0;
}

static int case_timestamp_mismatch(void)
{
    RTPJitter q(20);

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(800, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);

    TASK_CHECK(q.push_at(primary(801, slot_ts(1) + 1, std::vector<uint8>{0x02}), A + 1000)
               == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.nacks_at(A + 1000).empty());

    TASK_CHECK(q.push_at(primary(801, slot_ts(1), std::vector<uint8>{0x02}), A + 2000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 2);

    // the same applies to a repair packet's timestamp
    TASK_CHECK(q.push_at(rtx(802, slot_ts(2) + 1, 12, std::vector<uint8>{0x03}), A + 3000)
               == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 2);
    return 0;
}

static int case_unconfigured(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.push_at(primary(1, 0, std::vector<uint8>{0x01}), A) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.pop_at(out, A) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.nacks_at(A).empty());
    TASK_CHECK(q.get_depth() == 0);
    return 0;
}

static int case_legacy_rejected(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push(primary(1, 100, std::vector<uint8>{0x01})) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.pop(out) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 0);

    TASK_CHECK(q.push_at(primary(50, slot_ts(0), std::vector<uint8>{0x02}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push(primary(2, 200, std::vector<uint8>{0x03})) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.pop(out) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 1);
    return 0;
}


/* ------------------------------------------------------------------------- */
/* playout                                                                    */
/* ------------------------------------------------------------------------- */

static int case_basic_timeline(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    rawrtp_ptr sentinel = primary(999, 0xAAAAAAAA, std::vector<uint8>{0xEE});

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(900, slot_ts(0), std::vector<uint8>{0x10}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(901, slot_ts(1), std::vector<uint8>{0x11}), A + 1000) == RTPJitter::SUCCESS);

    out = sentinel;
    TASK_CHECK(q.pop_at(out, slot_deadline(0) - 1) == RTPJitter::BUFFERING);
    TASK_CHECK(out.get() == sentinel.get());
    TASK_CHECK(q.get_depth() == 2);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 900);
    TASK_CHECK(media_equals(out, std::vector<uint8>{0x10}));
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.get_depth_ms() == 20);

    out = sentinel;
    TASK_CHECK(q.pop_at(out, slot_deadline(1) - 1) == RTPJitter::BUFFERING);
    TASK_CHECK(out.get() == sentinel.get());

    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 901);
    TASK_CHECK(q.get_depth() == 0);

    out = sentinel;
    TASK_CHECK(q.pop_at(out, slot_deadline(2)) == RTPJitter::BUFFER_EMPTY);
    TASK_CHECK(out.get() == sentinel.get());
    return 0;
}

static int case_drop_and_empty(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    rawrtp_ptr sentinel = primary(999, 0xAAAAAAAA, std::vector<uint8>{0xEE});

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(1000, slot_ts(0), std::vector<uint8>{0xA0}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(1002, slot_ts(2), std::vector<uint8>{0xA2}), A + 1000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(1003, slot_ts(3), std::vector<uint8>{0xA3}), A + 2000) == RTPJitter::SUCCESS);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1000);

    // slot 1 was revealed but never filled: dropped at its deadline
    out = sentinel;
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::DROPPED_PACKET);
    TASK_CHECK(out.get() == sentinel.get());

    TASK_CHECK(q.pop_at(out, slot_deadline(2)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1002);

    TASK_CHECK(q.pop_at(out, slot_deadline(3)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1003);

    // past the newest known slot nothing advances
    out = sentinel;
    TASK_CHECK(q.pop_at(out, slot_deadline(4)) == RTPJitter::BUFFER_EMPTY);
    TASK_CHECK(out.get() == sentinel.get());
    TASK_CHECK(q.pop_at(out, slot_deadline(4)) == RTPJitter::BUFFER_EMPTY);
    return 0;
}

static int case_late_reject(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(1100, slot_ts(0), std::vector<uint8>{0xB0}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(1102, slot_ts(2), std::vector<uint8>{0xB2}), A + 1000) == RTPJitter::SUCCESS);

    // one microsecond after the deadline is too late
    TASK_CHECK(q.push_at(primary(1101, slot_ts(1), std::vector<uint8>{0xB1}),
                         slot_deadline(1) + 1) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 2);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::DROPPED_PACKET);

    // at the exact deadline the repair is still accepted
    RTPJitter r(20);
    TASK_CHECK(r.configure_repair(base_config()));
    TASK_CHECK(r.push_at(primary(1200, slot_ts(0), std::vector<uint8>{0xC0}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(r.push_at(primary(1202, slot_ts(2), std::vector<uint8>{0xC2}), A + 1000) == RTPJitter::SUCCESS);
    TASK_CHECK(r.push_at(rtx(1201, slot_ts(1), 22, std::vector<uint8>{0xC1}),
                         slot_deadline(1)) == RTPJitter::SUCCESS);
    TASK_CHECK(r.nacks_at(slot_deadline(1)).empty());

    TASK_CHECK(r.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(r.pop_at(out, slot_deadline(1)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1201);
    TASK_CHECK(media_equals(out, std::vector<uint8>{0xC1}));
    return 0;
}

static int case_first_content_wins(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(1300, slot_ts(0), std::vector<uint8>{0xD0}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(1302, slot_ts(2), std::vector<uint8>{0xD2}), A + 1000) == RTPJitter::SUCCESS);

    // repair fills the hole first
    TASK_CHECK(q.push_at(rtx(1301, slot_ts(1), 33, std::vector<uint8>{0xAA, 0xBB}),
                         A + 2000) == RTPJitter::SUCCESS);
    int depth = q.get_depth();

    // the original media packet arriving afterwards is a duplicate
    TASK_CHECK(q.push_at(primary(1301, slot_ts(1), std::vector<uint8>{0x11, 0x22}),
                         A + 3000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == depth);
    TASK_CHECK(q.nacks_at(A + 3000).empty());

    // and a second repair packet changes nothing either
    TASK_CHECK(q.push_at(rtx(1301, slot_ts(1), 34, std::vector<uint8>{0xCC, 0xDD}),
                         A + 3500) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == depth);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1301);
    TASK_CHECK(pts(out) == slot_ts(1));
    TASK_CHECK(pssrc(out) == PRI_SSRC);
    TASK_CHECK(ppt(out) == PRI_PT);
    TASK_CHECK(media_equals(out, std::vector<uint8>{0xAA, 0xBB}));   // first accepted wins

    // the mirrored case: original first, repair afterwards
    RTPJitter r(20);
    TASK_CHECK(r.configure_repair(base_config()));
    TASK_CHECK(r.push_at(primary(1400, slot_ts(0), std::vector<uint8>{0xE0}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(r.push_at(primary(1402, slot_ts(2), std::vector<uint8>{0xE2}), A + 1000) == RTPJitter::SUCCESS);
    TASK_CHECK(r.push_at(primary(1401, slot_ts(1), std::vector<uint8>{0x11, 0x22}), A + 2000) == RTPJitter::SUCCESS);
    TASK_CHECK(r.push_at(rtx(1401, slot_ts(1), 44, std::vector<uint8>{0xCC, 0xDD}), A + 3000) == RTPJitter::SUCCESS);

    TASK_CHECK(r.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(r.pop_at(out, slot_deadline(1)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1401);
    TASK_CHECK(media_equals(out, std::vector<uint8>{0x11, 0x22}));
    return 0;
}

static int case_sequence_timestamp_wrap(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    const uint16 anchor = 65534;
    const uint32 base   = 0xFFFFFF40;
    std::vector<uint16> n;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(anchor, slot_ts(0, base), std::vector<uint8>{0x01}), A)
               == RTPJitter::SUCCESS);

    // slot 2 wraps the sequence number to zero and the timestamp to small values
    TASK_CHECK(q.push_at(primary(0, slot_ts(2, base), std::vector<uint8>{0x03}), A + 1000)
               == RTPJitter::SUCCESS);
    n = q.nacks_at(A + 2000);
    TASK_CHECK(n.size() == 1);
    TASK_CHECK(n[0] == 65535);

    // slot 1 still fits at its deadline
    TASK_CHECK(q.push_at(primary(65535, slot_ts(1, base), std::vector<uint8>{0x02}),
                         slot_deadline(1)) == RTPJitter::SUCCESS);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 65534);
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 65535);
    TASK_CHECK(pts(out) == slot_ts(1, base));
    TASK_CHECK(q.pop_at(out, slot_deadline(2)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 0);
    TASK_CHECK(pts(out) == slot_ts(2, base));
    return 0;
}

static int case_rtx_osn_wraps(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    const uint16 anchor = 65534;
    const uint32 base   = 0xFFFFFE00;
    std::vector<uint16> n;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(anchor, slot_ts(0, base), std::vector<uint8>{0x01}), A)
               == RTPJitter::SUCCESS);

    // OSN 0 restored for slot 2; the outer sequence is unrelated
    TASK_CHECK(q.push_at(rtx(0, slot_ts(2, base), 45000, std::vector<uint8>{0x03}), A + 1000)
               == RTPJitter::SUCCESS);
    n = q.nacks_at(A + 2000);
    TASK_CHECK(n.size() == 1 && n[0] == 65535);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 65534);

    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::DROPPED_PACKET);

    TASK_CHECK(q.pop_at(out, slot_deadline(2)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 0);
    TASK_CHECK(pts(out) == slot_ts(2, base));
    TASK_CHECK(pssrc(out) == PRI_SSRC);
    TASK_CHECK(ppt(out) == PRI_PT);
    TASK_CHECK(media_equals(out, std::vector<uint8>{0x03}));
    return 0;
}

static int case_reset_reuse_sequences(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(65534, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(rtx(0, slot_ts(2), 5, std::vector<uint8>{0x03}), A + 1000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 2);
    TASK_CHECK(q.nacks_at(A + 2000).size() == 1);

    TASK_CHECK(q.reset() == RTPJitter::SUCCESS);
    TASK_CHECK(q.repair_configured() == true);
    TASK_CHECK(q.get_depth() == 0);
    TASK_CHECK(q.get_depth_ms() == 0);
    TASK_CHECK(q.nacks_at(A + 3000).empty());

    // the old sequence numbers are usable again once a new anchor is chosen
    TASK_CHECK(q.push_at(primary(65534, slot_ts(0), std::vector<uint8>{0x99}), A + 5000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.push_at(rtx(0, slot_ts(2), 5, std::vector<uint8>{0x98}), A + 6000)
               == RTPJitter::SUCCESS);

    TASK_CHECK(q.pop_at(out, A + 5000 + DELAY_US) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 65534);
    TASK_CHECK(media_equals(out, std::vector<uint8>{0x99}));
    TASK_CHECK(q.pop_at(out, A + 5000 + DELAY_US + SLOT_US) == RTPJitter::DROPPED_PACKET);
    TASK_CHECK(q.pop_at(out, A + 5000 + DELAY_US + 2 * SLOT_US) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 0);
    return 0;
}

static int case_rtx_envelope_playout(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    uint16 osn = 0x0501;                         // 1281
    std::vector<uint8> media = {0xDE, 0xAD, 0xBE, 0xEF, 0x77};

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(1280, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(1282, slot_ts(2), std::vector<uint8>{0x03}), A + 1000) == RTPJitter::SUCCESS);

    // repair packet carrying CSRCs, an extension and padding
    Spec s;
    s.seq    = 61000;                            // outer sequence, unrelated
    s.ts     = slot_ts(1);
    s.ssrc   = RTX_SSRC;
    s.pt     = RTX_PT;
    s.media.push_back((uint8)(osn >> 8));
    s.media.push_back((uint8)(osn & 0xFF));
    s.media.insert(s.media.end(), media.begin(), media.end());
    s.csrc.push_back(0xCAFEBABE);
    s.has_ext = true;
    s.profile = 0xBEDE;
    s.ext = {0x10, 0x20, 0x30, 0x40};
    s.padding = 3;
    rawrtp_ptr repair_packet = build(s);
    std::vector<uint8> repair_bytes(repair_packet->pData,
                                    repair_packet->pData + repair_packet->nLen);

    TASK_CHECK(q.push_at(repair_packet, A + 2000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(repair_packet, A + 3000) == RTPJitter::SUCCESS);   // duplicate
    TASK_CHECK(q.get_depth() == 3);
    TASK_CHECK(repair_packet->nLen == (uint16)repair_bytes.size());
    TASK_CHECK(memcmp(repair_packet->pData, repair_bytes.data(), repair_bytes.size()) == 0);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1280);
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::SUCCESS);

    RTPPacket::Layout layout;
    TASK_CHECK(out->parse_layout(layout));
    TASK_CHECK(pseq(out) == osn);
    TASK_CHECK(pts(out) == slot_ts(1));
    TASK_CHECK(pssrc(out) == PRI_SSRC);
    TASK_CHECK(ppt(out) == PRI_PT);
    TASK_CHECK(out->pData[0] == 0xB1);           // version, padding, extension, one CSRC
    TASK_CHECK(out->pData[1] == PRI_PT);         // marker clear, primary PT
    TASK_CHECK(layout.header_len == 12 + 4 + 4 + 4);
    TASK_CHECK(layout.padding_len == 3);
    TASK_CHECK(layout.media_len == 5);
    TASK_CHECK(media_equals(out, media));
    TASK_CHECK(out->pData[12] == 0xCA);          // CSRC preserved
    TASK_CHECK(out->pData[out->nLen - 1] == 3);  // padding preserved
    TASK_CHECK(out->payload_bytes == 5);
    TASK_CHECK(out->payload_ms == 20);

    TASK_CHECK(q.pop_at(out, slot_deadline(2)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1282);
    return 0;
}

static int case_rtx_outer_sequence_ignored(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;
    std::vector<uint8> media = {0xAB, 0xCD};

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.push_at(primary(1600, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(primary(1603, slot_ts(3), std::vector<uint8>{0x04}), A + 1000) == RTPJitter::SUCCESS);

    // two repair packets for the same hole with wildly different outer
    // sequence numbers: both address the same original slot
    TASK_CHECK(q.push_at(rtx(1601, slot_ts(1), 3, media), A + 2000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(rtx(1601, slot_ts(1), 65535, media), A + 3000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(rtx(1602, slot_ts(2), 9, std::vector<uint8>{0x05}), A + 4000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 4);

    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1601);
    TASK_CHECK(media_equals(out, media));

    TASK_CHECK(q.pop_at(out, slot_deadline(2)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1602);
    TASK_CHECK(media_equals(out, std::vector<uint8>{0x05}));

    TASK_CHECK(q.pop_at(out, slot_deadline(3)) == RTPJitter::SUCCESS);
    TASK_CHECK(pseq(out) == 1603);
    return 0;
}

static int case_depth_metrics(void)
{
    RTPJitter q(20);

    TASK_CHECK(q.configure_repair(base_config()));
    TASK_CHECK(q.get_depth() == 0 && q.get_depth_ms() == 0);

    TASK_CHECK(q.push_at(primary(1500, slot_ts(0), std::vector<uint8>{0x01}), A) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1 && q.get_depth_ms() == 20);
    TASK_CHECK(q.push_at(primary(1502, slot_ts(2), std::vector<uint8>{0x03}), A + 1000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 2 && q.get_depth_ms() == 40);

    // unrepaired hole is not retained content
    TASK_CHECK(q.get_depth() == 2);
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, slot_deadline(0)) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1 && q.get_depth_ms() == 20);
    TASK_CHECK(q.pop_at(out, slot_deadline(1)) == RTPJitter::DROPPED_PACKET);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.pop_at(out, slot_deadline(2)) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 0 && q.get_depth_ms() == 0);
    return 0;
}

static int case_legacy_still_works(void)
{
    RTPJitter q(20);
    rawrtp_ptr out;

    q.reset();
    rawrtp_ptr p = primary(100, 16000, std::vector<uint8>{0x42, 0x43});
    p->payload_ms    = 20;
    p->payload_type  = RTP_PAYLOAD_G711U;
    p->payload_bytes = 2;

    TASK_CHECK(q.push(p) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.get_depth_ms() == 20);
    TASK_CHECK(q.pop(out) == RTPJitter::SUCCESS);
    TASK_CHECK(out.get() == p.get());
    TASK_CHECK(out->pData[12] == 0x42);
    TASK_CHECK(q.get_depth() == 0);
    return 0;
}


const struct task_case task_cases[] =
{
    { "normalize", "config_validation",    case_config_validation },
    { "normalize", "config_clears_legacy", case_config_clears_legacy },
    { "normalize", "rtx_decode",           case_rtx_decode },
    { "normalize", "rtx_envelope",         case_rtx_envelope },
    { "normalize", "reject_format",        case_reject_format },
    { "normalize", "pre_anchor_rtx",       case_pre_anchor_rtx_fixed },
    { "feedback",  "reveal_order",         case_reveal_order },
    { "feedback",  "retry_interval",       case_retry_interval },
    { "feedback",  "repair_cancels",       case_repair_cancels_retry },
    { "feedback",  "consumed_hole_gone",   case_consumed_hole_gone },
    { "feedback",  "no_phantom_gap",       case_no_phantom_gap },
    { "feedback",  "query_no_side_effect", case_query_no_side_effect },
    { "feedback",  "window_overflow",      case_window_overflow },
    { "feedback",  "timestamp_mismatch",   case_timestamp_mismatch },
    { "feedback",  "unconfigured",         case_unconfigured },
    { "feedback",  "legacy_rejected",      case_legacy_rejected },
    { "playout",   "basic_timeline",       case_basic_timeline },
    { "playout",   "drop_and_empty",       case_drop_and_empty },
    { "playout",   "late_reject",          case_late_reject },
    { "playout",   "first_content_wins",   case_first_content_wins },
    { "playout",   "sequence_ts_wrap",     case_sequence_timestamp_wrap },
    { "playout",   "rtx_osn_wrap",         case_rtx_osn_wraps },
    { "playout",   "reset_reuse",          case_reset_reuse_sequences },
    { "playout",   "rtx_envelope_playout", case_rtx_envelope_playout },
    { "playout",   "rtx_outer_sequence",   case_rtx_outer_sequence_ignored },
    { "playout",   "depth_metrics",        case_depth_metrics },
    { "playout",   "legacy_still_works",   case_legacy_still_works }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
