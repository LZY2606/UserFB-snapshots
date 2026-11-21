/******************************************************************************
*   Regression checks for the RFC 4588 retransmission repair window.
*
*   Every packet is synthesized byte by byte and every clock is a manually
*   supplied microsecond timestamp: no sockets, no system clock, no threads.
*   Cases are registered through task_support/check.h in three groups:
*   normalize (parsing/config/wrapping), feedback (NACK behaviour) and
*   playout (deadline driven delivery).
******************************************************************************/

#include "task_support/check.h"
#include "rtp_jitter.h"

#include <vector>
#include <string>


static const uint32 PRIMARY_SSRC = 11;
static const uint32 RTX_SSRC     = 22;
static const uint8  PRIMARY_PT   = 8;
static const uint8  RTX_PT       = 97;


typedef std::vector<uint8> Bytes;


static void put_be16(Bytes& b, uint16 v)
{
    b.push_back(static_cast<uint8>(v >> 8));
    b.push_back(static_cast<uint8>(v & 0xFF));
}


static void put_be32(Bytes& b, uint32 v)
{
    b.push_back(static_cast<uint8>(v >> 24));
    b.push_back(static_cast<uint8>(v >> 16));
    b.push_back(static_cast<uint8>(v >> 8));
    b.push_back(static_cast<uint8>(v & 0xFF));
}


struct PacketOpts
{
    uint32  ssrc;
    uint8   pt;
    uint16  sequence;
    uint32  timestamp;
    bool    marker;
    uint8   cc;
    bool    extension;
    bool    padding;
    uint8   padding_count;
    Bytes   media;

    PacketOpts() :
        ssrc(PRIMARY_SSRC), pt(PRIMARY_PT), sequence(0), timestamp(0),
        marker(false), cc(0), extension(false), padding(false),
        padding_count(0) {}
};


static Bytes build_header(const PacketOpts& o, uint16 wire_sequence)
{
    Bytes b;
    uint8 flags = static_cast<uint8>((RTP_VERSION << 6) | (o.cc & 0x0F));
    if (o.padding) {
        flags |= 0x20;
    }
    if (o.extension) {
        flags |= 0x10;
    }
    b.push_back(flags);
    b.push_back(static_cast<uint8>((o.marker ? 0x80 : 0x00) | (o.pt & 0x7F)));
    put_be16(b, wire_sequence);
    put_be32(b, o.timestamp);
    put_be32(b, o.ssrc);
    for (uint8 i = 0; i < o.cc; ++i) {
        put_be32(b, 0x01020304u + i);
    }
    if (o.extension) {
        put_be16(b, 0xBEDE);            // profile-specific
        put_be16(b, 1);                 // one 32-bit extension word
        put_be32(b, 0x11223344u);
    }
    return b;
}


static void append_padding(Bytes& b, uint8 count)
{
    for (uint8 i = 1; i < count; ++i) {
        b.push_back(0x00);
    }
    b.push_back(count);
}


static rawrtp_ptr make_primary(const PacketOpts& o)
{
    Bytes b = build_header(o, o.sequence);
    b.insert(b.end(), o.media.begin(), o.media.end());
    if (o.padding) {
        append_padding(b, o.padding_count);
    }
    rawrtp_ptr p(new RTPPacket(b.empty() ? NULL : &b[0],
                               static_cast<short>(b.size())));
    p->payload_ms = 20;
    p->payload_type = o.pt;
    p->payload_bytes = static_cast<uint16>(o.media.size());
    return p;
}


static rawrtp_ptr make_rtx(const PacketOpts& o, uint16 outer_sequence,
                           uint16 osn)
{
    PacketOpts rtx = o;
    rtx.ssrc = RTX_SSRC;
    rtx.pt = RTX_PT;
    Bytes b = build_header(rtx, outer_sequence);
    put_be16(b, osn);                  // RFC 4588 two-byte big-endian OSN
    b.insert(b.end(), o.media.begin(), o.media.end());
    if (o.padding) {
        append_padding(b, o.padding_count);
    }
    rawrtp_ptr p(new RTPPacket(b.empty() ? NULL : &b[0],
                               static_cast<short>(b.size())));
    p->payload_ms = 20;
    p->payload_type = rtx.pt;
    p->payload_bytes = static_cast<uint16>(o.media.size() + 2);
    return p;
}


static RepairConfig default_cfg(uint16 window = 8)
{
    RepairConfig cfg;
    cfg.primary_ssrc = PRIMARY_SSRC;
    cfg.rtx_ssrc = RTX_SSRC;
    cfg.primary_pt = PRIMARY_PT;
    cfg.rtx_pt = RTX_PT;
    cfg.clock_rate = 8000;            // 160 timestamp units per 20 ms frame
    cfg.packet_ms = 20;
    cfg.delay_ms = 100;
    cfg.retry_ms = 30;
    cfg.window_packets = window;
    return cfg;
}


static PacketOpts media_opts(uint16 sequence, uint32 timestamp,
                             uint8 first_byte)
{
    PacketOpts o;
    o.sequence = sequence;
    o.timestamp = timestamp;
    o.media.push_back(first_byte);
    o.media.push_back(static_cast<uint8>(first_byte + 1));
    o.media.push_back(static_cast<uint8>(first_byte + 2));
    return o;
}


// expected media timestamp for slot k, given anchor timestamp T
static uint32 expected_ts(uint32 anchor_ts, uint32 k)
{
    return static_cast<uint32>(anchor_ts + k * 160u);
}


static uint16 hdr_sequence(const rawrtp_ptr& p)
{
    return static_cast<uint16>((p->pData[2] << 8) | p->pData[3]);
}


static uint32 hdr_timestamp(const rawrtp_ptr& p)
{
    return (static_cast<uint32>(p->pData[4]) << 24)
         | (static_cast<uint32>(p->pData[5]) << 16)
         | (static_cast<uint32>(p->pData[6]) << 8)
         | p->pData[7];
}


static uint32 hdr_ssrc(const rawrtp_ptr& p)
{
    return (static_cast<uint32>(p->pData[8]) << 24)
         | (static_cast<uint32>(p->pData[9]) << 16)
         | (static_cast<uint32>(p->pData[10]) << 8)
         | p->pData[11];
}


static bool same_sequences(const std::vector<uint16>& got,
                           const std::vector<uint16>& want)
{
    if (got.size() != want.size()) {
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i] != want[i]) {
            return false;
        }
    }
    return true;
}


// ==========================================================================
// normalize group
// ==========================================================================


static int run_config_validation()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);

    RepairConfig bad;

    bad = default_cfg(); bad.rtx_ssrc = bad.primary_ssrc;
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.rtx_pt = bad.primary_pt;
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.primary_pt = 128;
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.clock_rate = 0;
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.retry_ms = 0;
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.packet_ms = 0;
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.packet_ms = 65535;
    TASK_CHECK(q.configure_repair(bad) == true);     // upper edge is legal

    bad = default_cfg(); bad.clock_rate = 8001;      // 8001*20 not /1000
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.window_packets = 1;
    TASK_CHECK(q.configure_repair(bad) == false);

    bad = default_cfg(); bad.window_packets = 513;
    TASK_CHECK(q.configure_repair(bad) == false);

    // all failures preserved the original valid configuration
    PacketOpts anchor = media_opts(10, 1600, 0x40);
    TASK_CHECK(q.push_at(make_primary(anchor), 0) == RTPJitter::SUCCESS);

    // a successful re-configuration clears the old stream
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);
    TASK_CHECK(q.get_depth() == 0);
    TASK_CHECK(q.nacks_at(1000).empty());

    // zero delay is legal and negative timestamps are rejected everywhere
    RepairConfig immediate = default_cfg();
    immediate.delay_ms = 0;
    TASK_CHECK(q.configure_repair(immediate) == true);
    TASK_CHECK(q.push_at(make_primary(anchor), -1) == RTPJitter::BAD_PACKET);
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, -1) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.nacks_at(-1).empty());
    return 0;
}


static int run_rtx_restore_format()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);

    // anchor the stream with a plain primary packet
    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);

    // RTX carrying CSRC, an extension and legal padding, with an outer
    //  sequence number deliberately unrelated to the OSN
    PacketOpts o;
    o.sequence = 11;
    o.timestamp = 1760;
    o.marker = true;
    o.cc = 1;
    o.extension = true;
    o.padding = true;
    o.padding_count = 2;
    o.media.push_back(0xA1);
    o.media.push_back(0xA2);
    o.media.push_back(0xA3);
    rawrtp_ptr rtx = make_rtx(o, 0x1234, 11);
    uint16 original_len = rtx->nLen;

    TASK_CHECK(q.push_at(rtx, 1000) == RTPJitter::SUCCESS);

    // the caller's input object must be untouched
    TASK_CHECK(rtx->nLen == original_len);
    TASK_CHECK(rtx->pData[1] == (0x80 | RTX_PT));

    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 100000) == RTPJitter::SUCCESS);   // slot 10
    TASK_CHECK(q.pop_at(out, 120000) == RTPJitter::SUCCESS);   // slot 11

    TASK_CHECK(hdr_sequence(out) == 11);                       // OSN, not outer
    TASK_CHECK(hdr_ssrc(out) == PRIMARY_SSRC);
    TASK_CHECK((out->pData[1] & 0x7F) == PRIMARY_PT);
    TASK_CHECK((out->pData[1] & 0x80) != 0);                   // marker kept
    TASK_CHECK(hdr_timestamp(out) == 1760);
    TASK_CHECK(out->pData[0] == 0xB1);                         // V/P/X/CC kept
    TASK_CHECK(out->nLen == original_len - 2);                 // OSN removed

    // 12 fixed + 4 CSRC + 8 extension = 24 header bytes in both forms
    const uint16 header_len = 24;
    TASK_CHECK(out->pData[12] == 0x01);                        // CSRC preserved
    TASK_CHECK(out->pData[16] == 0xBE);                        // extension kept
    TASK_CHECK(out->pData[header_len] == 0xA1);                // media kept
    TASK_CHECK(out->pData[header_len + 1] == 0xA2);
    TASK_CHECK(out->pData[header_len + 2] == 0xA3);
    TASK_CHECK(out->pData[out->nLen - 1] == 2);                // padding kept
    TASK_CHECK(out->payload_bytes == 3);
    TASK_CHECK(out->payload_type == PRIMARY_PT);
    TASK_CHECK(out->payload_ms == 20);
    return 0;
}


static int run_malformed_rejected()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);
    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);

    // RTX before the first primary establishes no state
    RTPJitter fresh(20);
    TASK_CHECK(fresh.configure_repair(default_cfg()) == true);
    TASK_CHECK(fresh.push_at(make_rtx(media_opts(11, 1760, 0x20), 900, 11), 0)
               == RTPJitter::BAD_PACKET);
    TASK_CHECK(fresh.get_depth() == 0);

    PacketOpts o = media_opts(11, 1760, 0x20);

    // wrong RTP version
    rawrtp_ptr bad_version = make_primary(o);
    bad_version->pData[0] = 0x40;
    TASK_CHECK(q.push_at(bad_version, 1000) == RTPJitter::BAD_PACKET);

    // truncated fixed header
    Bytes tiny(10, 0x80);
    rawrtp_ptr truncated(new RTPPacket(&tiny[0], static_cast<short>(tiny.size())));
    TASK_CHECK(q.push_at(truncated, 1000) == RTPJitter::BAD_PACKET);

    // padding count of zero and a count larger than the payload are both illegal
    // a padding count of zero is illegal; force the final byte to zero
    PacketOpts pad0 = o;
    pad0.padding = true;
    pad0.padding_count = 0;
    rawrtp_ptr bad_pad0 = make_primary(pad0);
    bad_pad0->pData[bad_pad0->nLen - 1] = 0;
    TASK_CHECK(q.push_at(bad_pad0, 1000) == RTPJitter::BAD_PACKET);

    // a padding count larger than the whole payload is illegal
    rawrtp_ptr bad_pad_big = make_primary(o);
    bad_pad_big->pData[0] |= 0x20;
    bad_pad_big->pData[bad_pad_big->nLen - 1] = 20;   // payload is only 3 bytes
    TASK_CHECK(q.push_at(bad_pad_big, 1000) == RTPJitter::BAD_PACKET);

    // extension advertising more data than the packet holds
    rawrtp_ptr bad_ext = make_primary(o);
    bad_ext->pData[0] |= 0x10;
    bad_ext->nLen = 14;                       // nowhere near a full extension
    TASK_CHECK(q.push_at(bad_ext, 1000) == RTPJitter::BAD_PACKET);

    // RTX payload shorter than the two-byte OSN
    PacketOpts no_media = o;
    no_media.media.clear();
    rawrtp_ptr short_rtx = make_rtx(no_media, 901, 11);
    short_rtx->nLen = static_cast<uint16>(short_rtx->nLen - 2);  // drop OSN
    TASK_CHECK(q.push_at(short_rtx, 1000) == RTPJitter::BAD_PACKET);

    // SSRC/PT mismatches on both sides
    PacketOpts wrong_ssrc = o;
    wrong_ssrc.ssrc = 999;
    TASK_CHECK(q.push_at(make_primary(wrong_ssrc), 1000)
               == RTPJitter::BAD_PACKET);
    PacketOpts wrong_pt = o;
    wrong_pt.pt = 77;
    TASK_CHECK(q.push_at(make_primary(wrong_pt), 1000)
               == RTPJitter::BAD_PACKET);

    // none of the rejections changed the buffer or timing state
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.nacks_at(2000).empty());
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 100000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 10);
    return 0;
}


static int run_sequence_wrap()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);

    const uint16 anchor_seq = 65534;
    const uint32 anchor_ts = 0xFFFFFF60u;     // next slot lands on 0x00000000

    TASK_CHECK(q.push_at(make_primary(media_opts(anchor_seq, anchor_ts, 0x10)), 0)
               == RTPJitter::SUCCESS);
    // skipping 65535 reveals the wrap-around gap; slots 0 and 1 follow
    TASK_CHECK(q.push_at(make_primary(media_opts(0, expected_ts(anchor_ts, 2), 0x30)), 1000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(1, expected_ts(anchor_ts, 3), 0x40)), 2000)
               == RTPJitter::SUCCESS);

    std::vector<uint16> nacks = q.nacks_at(3000);
    TASK_CHECK(same_sequences(nacks, std::vector<uint16>(1, 65535)));

    PacketOpts missing = media_opts(65535, expected_ts(anchor_ts, 1), 0x20);
    TASK_CHECK(q.push_at(make_rtx(missing, 0x7777, 65535), 4000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.nacks_at(5000).empty());

    const int64 base = 100000;                // delay_ms * 1000
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, base) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 65534);
    TASK_CHECK(hdr_timestamp(out) == anchor_ts);

    TASK_CHECK(q.pop_at(out, base + 20000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 65535);
    TASK_CHECK(hdr_timestamp(out) == 0);

    TASK_CHECK(q.pop_at(out, base + 40000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 0);
    TASK_CHECK(hdr_timestamp(out) == expected_ts(anchor_ts, 2));

    TASK_CHECK(q.pop_at(out, base + 60000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 1);
    return 0;
}


static int run_legacy_and_mode_guards()
{
    RTPJitter q(20);

    // repair APIs refuse everything before configuration
    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::BAD_PACKET);
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 0) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.nacks_at(0).empty());

    // the legacy push/pop path keeps its original behaviour
    rawrtp_ptr legacy = make_primary(media_opts(100, 16000, 0x42));
    TASK_CHECK(q.push(legacy) == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);
    TASK_CHECK(q.pop(out) == RTPJitter::SUCCESS);
    TASK_CHECK(out == legacy);

    // once repair mode is on, the legacy entry points reject and state stays
    TASK_CHECK(q.configure_repair(default_cfg()) == true);
    TASK_CHECK(q.push(legacy) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.pop(out) == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 0);
    return 0;
}


static int run_reset_reuse()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);
    TASK_CHECK(q.push_at(make_primary(media_opts(100, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 1);

    TASK_CHECK(q.reset() == RTPJitter::SUCCESS);

    // configuration survives, the stream and timing anchor are gone
    TASK_CHECK(q.get_depth() == 0);
    TASK_CHECK(q.nacks_at(5000).empty());
    TASK_CHECK(q.push_at(make_rtx(media_opts(100, 1600, 0x20), 5, 100), 1000)
               == RTPJitter::BAD_PACKET);     // no anchor again yet

    // the same sequence number can anchor a brand new stream
    TASK_CHECK(q.push_at(make_primary(media_opts(100, 3200, 0x30)), 2000)
               == RTPJitter::SUCCESS);
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 2000 + 100000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 100);
    TASK_CHECK(hdr_timestamp(out) == 3200);
    return 0;
}


// ==========================================================================
// feedback group
// ==========================================================================


static int run_nack_lifecycle()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);

    // one packet alone reveals nothing, so nothing can ever be requested
    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.nacks_at(1000).empty());

    // arriving 12 first reveals gap 11 out of order
    TASK_CHECK(q.push_at(make_primary(media_opts(12, 1920, 0x30)), 1000)
               == RTPJitter::SUCCESS);

    std::vector<uint16> want;
    want.push_back(11);
    TASK_CHECK(same_sequences(q.nacks_at(2000), want));     // first query reports
    TASK_CHECK(q.nacks_at(2000).empty());                   // same instant: no repeat
    TASK_CHECK(q.nacks_at(31999).empty());                  // retry_ms not reached
    TASK_CHECK(same_sequences(q.nacks_at(32000), want));    // retry boundary

    // a late query reports once; missed retry rounds are not replayed
    TASK_CHECK(same_sequences(q.nacks_at(95000), want));
    TASK_CHECK(q.nacks_at(95000).empty());

    // the repair landing immediately cancels further requests for the gap
    TASK_CHECK(q.push_at(make_rtx(media_opts(11, 1760, 0x20), 0x4444, 11), 96000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.nacks_at(96000).empty());

    // a second gap further on is reported in play order alongside nothing else
    TASK_CHECK(q.push_at(make_primary(media_opts(14, 2240, 0x50)), 100000)
               == RTPJitter::SUCCESS);
    want.clear();
    want.push_back(13);
    TASK_CHECK(same_sequences(q.nacks_at(110000), want));

    // once the slot's playout instant has passed the gap is no longer requested
    const int64 slot13_deadline = 100000 + 3 * 20000;       // A + delay + 3 frames
    TASK_CHECK(q.nacks_at(slot13_deadline).size() == 1);    // still due exactly on time
    TASK_CHECK(q.nacks_at(slot13_deadline + 1).empty());    // one microsecond too late
    return 0;
}


static int run_nack_ordering_and_fill()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);
    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);

    // jump to 13: gaps 11 and 12 must come back in play order
    TASK_CHECK(q.push_at(make_primary(media_opts(13, 2080, 0x40)), 1000)
               == RTPJitter::SUCCESS);
    std::vector<uint16> got = q.nacks_at(2000);
    TASK_CHECK(got.size() == 2 && got[0] == 11 && got[1] == 12);

    // a restored RTX can reveal and fill a slot just like a primary
    TASK_CHECK(q.push_at(make_rtx(media_opts(11, 1760, 0x20), 0x5555, 11), 3000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.nacks_at(4000).empty());                  // 12 still cooling down
    std::vector<uint16> want12;
    want12.push_back(12);
    TASK_CHECK(same_sequences(q.nacks_at(32000), want12)); // retry boundary

    TASK_CHECK(q.push_at(make_primary(media_opts(12, 1920, 0x30)), 33000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.nacks_at(34000).empty());

    // queries never advanced the play head: all four slots still play in order
    rawrtp_ptr out;
    int64 base = 100000;
    for (uint16 k = 0; k < 4; ++k) {
        TASK_CHECK(q.pop_at(out, base + k * 20000) == RTPJitter::SUCCESS);
        TASK_CHECK(hdr_sequence(out) == static_cast<uint16>(10 + k));
    }
    return 0;
}


// ==========================================================================
// playout group
// ==========================================================================


static int run_playout_lifecycle()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);

    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(12, 1920, 0x30)), 1000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 2);
    TASK_CHECK(q.get_depth_ms() == 40);

    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 50000) == RTPJitter::BUFFERING);  // before deadline

    std::vector<uint16> want;
    want.push_back(11);
    TASK_CHECK(same_sequences(q.nacks_at(60000), want));
    TASK_CHECK(q.push_at(make_rtx(media_opts(11, 1760, 0x20), 0x6666, 11), 70000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 3);

    TASK_CHECK(q.pop_at(out, 100000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 10);
    TASK_CHECK(q.pop_at(out, 100000) == RTPJitter::BUFFERING);

    TASK_CHECK(q.pop_at(out, 120000) == RTPJitter::SUCCESS);   // repaired slot
    TASK_CHECK(hdr_sequence(out) == 11);
    TASK_CHECK(hdr_ssrc(out) == PRIMARY_SSRC);
    TASK_CHECK((out->pData[1] & 0x7F) == PRIMARY_PT);
    TASK_CHECK(out->pData[12] == 0x20);

    TASK_CHECK(q.pop_at(out, 140000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 12);
    TASK_CHECK(q.pop_at(out, 140000) == RTPJitter::BUFFER_EMPTY);
    TASK_CHECK(q.get_depth() == 0);

    // a revealed but unfilled slot is dropped and still advances the head
    rawrtp_ptr sentinel = make_primary(media_opts(99, 0, 0x7F));
    rawrtp_ptr out2 = sentinel;
    TASK_CHECK(q.push_at(make_primary(media_opts(14, 2240, 0x50)), 150000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.pop_at(out2, 160000) == RTPJitter::DROPPED_PACKET);
    TASK_CHECK(out2 == sentinel);                              // output not rewritten
    TASK_CHECK(q.pop_at(out2, 180000) == RTPJitter::SUCCESS);  // slot 14 deadline
    TASK_CHECK(hdr_sequence(out2) == 14);

    // exact playout instant still accepts; one microsecond later is too late
    TASK_CHECK(q.push_at(make_primary(media_opts(15, 2400, 0x60)), 200000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(16, 2560, 0x70)), 220001)
               == RTPJitter::BAD_PACKET);
    TASK_CHECK(q.get_depth() == 1);                            // rejection: no state change

    // duplicate primary and RTX for an occupied slot succeed but lose the race
    TASK_CHECK(q.push_at(make_primary(media_opts(15, 2400, 0x61)), 200000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_rtx(media_opts(15, 2400, 0x62), 0x1111, 15), 200000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.pop_at(out, 200000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 15);
    TASK_CHECK(out->pData[12] == 0x60);                        // first content wins
    return 0;
}


static int run_rtx_first_contention()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);
    TASK_CHECK(q.push_at(make_primary(media_opts(20, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);

    // RTX reaches the slot first; the later primary duplicate must not replace it
    TASK_CHECK(q.push_at(make_rtx(media_opts(21, 1760, 0xAA), 0x2222, 21), 1000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(21, 1760, 0xBB)), 2000)
               == RTPJitter::SUCCESS);

    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 100000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 20);
    TASK_CHECK(q.pop_at(out, 120000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 21);
    TASK_CHECK(out->pData[12] == 0xAA);                        // RTX content kept
    return 0;
}


static int run_window_boundary()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg(4)) == true);   // slots 0..3 from head
    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);

    TASK_CHECK(q.push_at(make_primary(media_opts(11, 1760, 0x20)), 0)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(12, 1920, 0x30)), 0)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(13, 2080, 0x40)), 0)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.get_depth() == 4);

    // distance 4 from the play head is outside the window
    TASK_CHECK(q.push_at(make_primary(media_opts(14, 2240, 0x50)), 0)
               == RTPJitter::BUFFER_OVERFLOW);
    TASK_CHECK(q.get_depth() == 4);                            // nothing changed

    // after the head advances one slot, 14 is reachable and 15 overflows
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 100000) == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(14, 2240, 0x50)), 100000)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(15, 2400, 0x60)), 100000)
               == RTPJitter::BUFFER_OVERFLOW);
    TASK_CHECK(q.get_depth() == 4);
    return 0;
}


static int run_nacks_after_drain()
{
    RTPJitter q(20);
    TASK_CHECK(q.configure_repair(default_cfg()) == true);
    TASK_CHECK(q.push_at(make_primary(media_opts(10, 1600, 0x10)), 0)
               == RTPJitter::SUCCESS);
    TASK_CHECK(q.push_at(make_primary(media_opts(11, 1760, 0x20)), 1000)
               == RTPJitter::SUCCESS);

    // play both known slots; the head then sits one past the newest slot
    rawrtp_ptr out;
    TASK_CHECK(q.pop_at(out, 100000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 10);
    TASK_CHECK(q.pop_at(out, 120000) == RTPJitter::SUCCESS);
    TASK_CHECK(hdr_sequence(out) == 11);
    TASK_CHECK(q.pop_at(out, 120000) == RTPJitter::BUFFER_EMPTY);

    // no phantom gaps may be enumerated through the sequence wrap
    TASK_CHECK(q.nacks_at(130000).empty());
    TASK_CHECK(q.nacks_at(200000000).empty());
    return 0;
}


const task_case task_cases[] = {
    { "normalize", "config_validation",   run_config_validation   },
    { "normalize", "rtx_restore_format",  run_rtx_restore_format  },
    { "normalize", "malformed_rejected",  run_malformed_rejected  },
    { "normalize", "sequence_wrap",       run_sequence_wrap       },
    { "normalize", "legacy_mode_guards",  run_legacy_and_mode_guards },
    { "normalize", "reset_reuse",         run_reset_reuse         },
    { "feedback",  "nack_lifecycle",      run_nack_lifecycle      },
    { "feedback",  "nack_ordering_fill",  run_nack_ordering_and_fill },
    { "feedback",  "nacks_after_drain",   run_nacks_after_drain   },
    { "playout",   "playout_lifecycle",   run_playout_lifecycle   },
    { "playout",   "rtx_first_contention", run_rtx_first_contention },
    { "playout",   "window_boundary",     run_window_boundary     },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
