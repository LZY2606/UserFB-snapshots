// Regression tests for H.264 access unit framing.
//
// Registers three groups through task_support/check.h:
//   pictures   - AU splitting identity rules, slice grouping, NAL placement
//   timestamps - PES bound PTS/DTS attribution across arbitrary PES cuts
//   lifecycle  - finish(), error reporting/recovery, dual PID isolation
//
// The tests build their own PAT/PMT/PES/TS byte streams so PES boundaries can
// be redistributed freely; no external codec is used.

#include "task_support/check.h"

#include "h264_access_unit.h"
#include "mpegts_demuxer.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace mpegts;

namespace {

// ---------------------------------------------------------------------------
// Small byte / bit builders
// ---------------------------------------------------------------------------

struct ByteSink {
    std::vector<uint8_t> b;
    void u8(uint32_t x) { b.push_back(static_cast<uint8_t>(x & 0xff)); }
    void u16(uint32_t x) { u8(x >> 8); u8(x); }
    void u24(uint32_t x) { u8(x >> 16); u8(x >> 8); u8(x); }
    void u32(uint32_t x) { u16(x >> 16); u16(x); }
    void vec(const std::vector<uint8_t>& x) { b.insert(b.end(), x.begin(), x.end()); }
};

struct BitWriter {
    std::vector<uint8_t> out;
    uint8_t cur = 0;
    int bitsUsed = 0;

    void bit(int x) {
        cur = static_cast<uint8_t>((cur << 1) | (x & 1));
        if (++bitsUsed == 8) {
            out.push_back(cur);
            cur = 0;
            bitsUsed = 0;
        }
    }
    void bits(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            bit((value >> i) & 1u);
        }
    }
    void ue(uint32_t codeNum) {
        uint32_t code = codeNum + 1;
        int len = 1;
        while ((1u << len) <= code) {
            ++len;
        }
        for (int i = 0; i < len - 1; ++i) {
            bit(0);
        }
        bits(code, len);
    }
    void se(int32_t value) {
        uint32_t codeNum;
        if (value > 0) {
            codeNum = static_cast<uint32_t>(2 * value - 1);
        } else if (value < 0) {
            codeNum = static_cast<uint32_t>(-2 * value);
        } else {
            codeNum = 0;
        }
        ue(codeNum);
    }
    void alignWithStopBit() {
        bit(1); // rbsp_stop_one_bit
        while (bitsUsed != 0) {
            bit(0);
        }
    }
};

// Insert emulation prevention bytes and prepend the one byte NAL header.
std::vector<uint8_t> makeNal(uint8_t nalHeader, const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> nal;
    nal.push_back(nalHeader);
    int zeros = 0;
    for (uint8_t x : rbsp) {
        if (zeros >= 2 && x <= 0x03) {
            nal.push_back(0x03);
            zeros = 0;
        }
        nal.push_back(x);
        if (x == 0) {
            ++zeros;
        } else {
            zeros = 0;
        }
    }
    return nal;
}

std::vector<uint8_t> buildSps(int spsId, int fnMinus4, int pocType, int pocMinus4,
                              bool frameOnly, bool deltaAlwaysZero,
                              int highProfile = 0, bool scaling = false) {
    BitWriter w;
    int profile = highProfile ? 100 : 66;
    w.bits(profile, 8);
    w.bits(0x00, 8); // constraint flags
    w.bits(30, 8);   // level_idc
    w.ue(spsId);
    if (highProfile) {
        w.ue(1); // chroma_format_idc = 4:2:0
        w.ue(0); // bit_depth_luma_minus8
        w.ue(0); // bit_depth_chroma_minus8
        w.bit(0); // qpprime_y_zero_transform_bypass_flag
        w.bit(scaling ? 1 : 0); // seq_scaling_matrix_present_flag
        if (scaling) {
            // Eight lists; only the first one is present and uses flat value 8.
            for (int list = 0; list < 8; ++list) {
                if (list == 0) {
                    w.bit(1);
                    for (int j = 0; j < 16; ++j) {
                        w.se(0); // every delta scale is zero
                    }
                } else {
                    w.bit(0);
                }
            }
        }
    }
    w.ue(fnMinus4);
    w.ue(pocType);
    if (pocType == 0) {
        w.ue(pocMinus4);
    } else if (pocType == 1) {
        w.bit(deltaAlwaysZero ? 1 : 0);
        w.se(0); // offset_for_non_ref_pic
        w.se(0); // offset_for_top_to_bottom_field
        w.ue(0); // num_ref_frames_in_poc_cycle
    }
    w.ue(0); // max_num_ref_frames
    w.bit(0); // gaps_in_frame_num_value_allowed_flag
    w.ue(0); // pic_width_in_mbs_minus1
    w.ue(0); // pic_height_in_map_units_minus1
    w.bit(frameOnly ? 1 : 0); // frame_mbs_only_flag
    if (!frameOnly) {
        w.bit(0); // mb_adaptive_frame_field_flag
    }
    w.alignWithStopBit();
    return makeNal(0x67, w.out);
}

std::vector<uint8_t> buildPps(int ppsId, int spsId, bool bottomFieldPoc) {
    BitWriter w;
    w.ue(ppsId);
    w.ue(spsId);
    w.bit(0); // entropy_coding_mode_flag (CAVLC)
    w.bit(bottomFieldPoc ? 1 : 0);
    w.alignWithStopBit();
    return makeNal(0x68, w.out);
}

struct SliceLayout {
    int fnBits = 4;
    bool frameOnly = true;
    int pocType = 2;
    int pocLsbBits = 4;
    bool bottomFieldPoc = false;
    bool deltaAlwaysZero = false;
};

struct SliceDesc {
    int nalType = 1; // 1 non-IDR, 5 IDR
    int nalRefIdc = 2;
    int firstMb = 0;
    int ppsId = 0;
    int frameNum = 0;
    int idrPicId = 0;
    int fieldPic = 0;
    int bottomField = 0;
    int pocLsb = 0;
    int deltaPocBottom = 0;
    int deltaPoc0 = 0;
    int deltaPoc1 = 0;
    bool injectEscapeBytes = false;
};

std::vector<uint8_t> buildSlice(const SliceDesc& s, const SliceLayout& l) {
    BitWriter w;
    w.ue(s.firstMb);
    w.ue(7); // slice_type I
    w.ue(s.ppsId);
    w.bits(static_cast<uint32_t>(s.frameNum), l.fnBits);
    if (!l.frameOnly) {
        w.bit(s.fieldPic);
        if (s.fieldPic) {
            w.bit(s.bottomField);
        }
    }
    if (s.nalType == 5) {
        w.ue(s.idrPicId);
    }
    if (l.pocType == 0) {
        w.bits(static_cast<uint32_t>(s.pocLsb), l.pocLsbBits);
        if (l.bottomFieldPoc && !s.fieldPic) {
            w.se(s.deltaPocBottom);
        }
    } else if (l.pocType == 1 && !l.deltaAlwaysZero) {
        w.se(s.deltaPoc0);
        if (l.bottomFieldPoc && !s.fieldPic) {
            w.se(s.deltaPoc1);
        }
    }
    w.alignWithStopBit();
    if (s.injectEscapeBytes) {
        // Raw RBSP-looking patterns that emulation prevention must escape.
        const uint8_t pad[] = {0x00, 0x00, 0x00, 0x01, 0xaa, 0x00, 0x00, 0x03, 0x02};
        w.out.insert(w.out.end(), pad, pad + sizeof(pad));
    }
    uint8_t header = static_cast<uint8_t>((s.nalRefIdc << 5) | s.nalType);
    return makeNal(header, w.out);
}

// ---------------------------------------------------------------------------
// Annex-B helpers
// ---------------------------------------------------------------------------

struct LocatedNal {
    int type;
    size_t start;    // Offset of the first start code zero byte
    int startLen;    // 3 or 4
    size_t payload;  // Offset of NAL header byte
    std::vector<uint8_t> bytes; // Header + escaped body
};

std::vector<LocatedNal> locateNals(const std::vector<uint8_t>& x) {
    std::vector<LocatedNal> nals;
    size_t i = 0;
    while (i + 2 < x.size()) {
        int scLen = 0;
        if (i + 4 <= x.size() && x[i] == 0 && x[i + 1] == 0 && x[i + 2] == 0 && x[i + 3] == 1) {
            scLen = 4;
        } else if (x[i] == 0 && x[i + 1] == 0 && x[i + 2] == 1) {
            scLen = 3;
        }
        if (scLen == 0) {
            ++i;
            continue;
        }
        size_t payload = i + scLen;
        size_t next = x.size();
        size_t j = payload;
        while (j + 2 < x.size()) {
            if (j + 4 <= x.size() && x[j] == 0 && x[j + 1] == 0 && x[j + 2] == 0 && x[j + 3] == 1) {
                next = j;
                break;
            }
            if (x[j] == 0 && x[j + 1] == 0 && x[j + 2] == 1) {
                next = j;
                break;
            }
            ++j;
        }
        LocatedNal n;
        n.type = payload < x.size() ? (x[payload] & 0x1f) : -1;
        n.start = i;
        n.startLen = scLen;
        n.payload = payload;
        n.bytes.assign(x.begin() + payload, x.begin() + next);
        nals.push_back(std::move(n));
        i = next;
    }
    return nals;
}

std::vector<uint8_t> annexBOf(const std::vector<std::vector<uint8_t>>& nals) {
    std::vector<uint8_t> out;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    for (const auto& n : nals) {
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}

std::vector<uint8_t> normalizeStartCodes(const std::vector<uint8_t>& x) {
    std::vector<std::vector<uint8_t>> raw;
    for (const LocatedNal& n : locateNals(x)) {
        raw.push_back(n.bytes);
    }
    return annexBOf(raw);
}

std::vector<int> nalTypes(const H264AccessUnit& au) {
    std::vector<int> types;
    for (const LocatedNal& n : locateNals(au.annexB)) {
        types.push_back(n.type);
    }
    return types;
}

// ---------------------------------------------------------------------------
// TS/PES builder
// ---------------------------------------------------------------------------

struct TsMux {
    std::vector<uint8_t> ts;
    std::map<uint16_t, int> cc;

    uint8_t nextCc(uint16_t pid) {
        int v = cc[pid];
        cc[pid] = (v + 1) & 0x0f;
        return static_cast<uint8_t>(v);
    }

    void appendPacket(ByteSink& p) {
        while (p.b.size() < 188) {
            p.u8(0xff);
        }
        ts.insert(ts.end(), p.b.begin(), p.b.end());
    }

    void psiPacket(uint16_t pid, const std::vector<uint8_t>& section) {
        ByteSink p;
        p.u8(0x47);
        p.u16(0x4000 | (pid & 0x1fff)); // PUSI
        p.u8(0x10 | nextCc(pid));       // payload only
        p.u8(0x00);                     // pointer field
        p.vec(section);
        appendPacket(p);
    }

    void pesBytes(uint16_t pid, const std::vector<uint8_t>& pes) {
        size_t off = 0;
        bool first = true;
        while (first || off < pes.size()) {
            size_t remaining = pes.size() - off;
            ByteSink p;
            p.u8(0x47);

            if (remaining >= 184) {
                // Payload only packet.
                p.u16((first ? 0x4000 : 0) | (pid & 0x1fff));
                p.u8(0x10 | nextCc(pid));
                p.b.insert(p.b.end(), pes.begin() + off, pes.begin() + off + 184);
                off += 184;
            } else {
                // Short final packet: fill unused bytes with an adaptation
                // field so they are not mistaken for PES payload.
                size_t payloadLen = remaining;
                size_t afTotal = 184 - payloadLen; // length byte + af bytes
                p.u16((first ? 0x4000 : 0) | (pid & 0x1fff));
                p.u8(0x30 | nextCc(pid)); // adaptation + payload
                uint8_t afLength = static_cast<uint8_t>(afTotal - 1);
                p.u8(afLength);
                if (afLength > 0) {
                    p.u8(0x00); // no flags
                    for (int i = 0; i + 1 < afLength; ++i) {
                        p.u8(0xff);
                    }
                }
                p.b.insert(p.b.end(), pes.begin() + off, pes.begin() + off + payloadLen);
                off += payloadLen;
            }
            // Packet must be exactly 188 bytes.
            while (p.b.size() < 188) {
                p.u8(0xff);
            }
            ts.insert(ts.end(), p.b.begin(), p.b.end());
            first = false;
        }
    }
};

std::vector<uint8_t> patSection(uint16_t pmtPid) {
    ByteSink s;
    s.u8(0x00);
    s.u16(0xb00d); // section syntax indicator set, b0 zero, length 13
    s.u16(0x0001); // transport_stream_id
    s.u8(0xc1);
    s.u8(0x00);
    s.u8(0x00);
    s.u16(0x0001); // program_number
    s.u16(0xe000 | pmtPid);
    s.u32(0); // CRC (not verified by the demuxer)
    return s.b;
}

std::vector<uint8_t> pmtSection(const std::vector<std::pair<uint8_t, uint16_t>>& elems) {
    ByteSink body;
    body.u16(0x0001); // program_number
    body.u8(0xc1);
    body.u8(0x00);
    body.u8(0x00);
    body.u16(0xe000 | 0x0100); // PCR PID
    body.u16(0xf000);          // reserved + program_info_length 0
    for (const auto& e : elems) {
        body.u8(e.first);
        body.u16(0xe000 | e.second);
        body.u16(0xf000); // reserved + ES_info_length 0
    }

    ByteSink s;
    s.u8(0x02);
    uint16_t sectionLength = static_cast<uint16_t>(body.b.size() + 4); // plus CRC
    s.u16(0xb000 | sectionLength);
    s.vec(body.b);
    s.u32(0);
    return s.b;
}

void sendPsi(MpegTsDemuxer& d, TsMux& mx, uint16_t pmtPid,
             const std::vector<std::pair<uint8_t, uint16_t>>& elems) {
    mx.psiPacket(0x0000, patSection(pmtPid));
    mx.psiPacket(pmtPid, pmtSection(elems));
    SimpleBuffer sb;
    sb.append(mx.ts.data(), mx.ts.size());
    d.decode(sb);
    mx.ts.clear();
}

void writeTsStamp(ByteSink& p, int marker, uint64_t t) {
    p.u8((marker << 4) | (((t >> 30) & 0x07) << 1) | 1);
    p.u16((((t >> 15) & 0x7fff) << 1) | 1);
    p.u16(((t & 0x7fff) << 1) | 1);
}

struct PesTime {
    int flags = 0; // 0, 2 (PTS), 3 (PTS+DTS)
    uint64_t pts = 0;
    uint64_t dts = 0;
};

std::vector<uint8_t> buildPes(const std::vector<uint8_t>& payload, const PesTime& t,
                              bool zeroLength = false) {
    ByteSink p;
    p.u24(0x000001);
    p.u8(0xe0); // stream id video
    int headerDataLen = (t.flags == 3) ? 10 : (t.flags == 2 ? 5 : 0);
    uint32_t pesLen = zeroLength ? 0u : static_cast<uint32_t>(3 + headerDataLen + payload.size());
    p.u16(pesLen);
    p.u8(0x80); // marker bits
    p.u8(static_cast<uint32_t>(t.flags) << 6);
    p.u8(headerDataLen);
    if (t.flags == 2) {
        writeTsStamp(p, 2, t.pts);
    } else if (t.flags == 3) {
        writeTsStamp(p, 3, t.pts);
        writeTsStamp(p, 1, t.dts);
    }
    p.vec(payload);
    return p.b;
}

struct PesSegment {
    size_t begin = 0;
    size_t end = 0;
    PesTime time;
    bool zeroLength = false;
};

void feedSegments(MpegTsDemuxer& d, TsMux& mx, uint16_t pid,
                  const std::vector<uint8_t>& h264,
                  const std::vector<PesSegment>& segs) {
    for (const PesSegment& s : segs) {
        std::vector<uint8_t> payload(h264.begin() + s.begin, h264.begin() + s.end);
        mx.pesBytes(pid, buildPes(payload, s.time, s.zeroLength));
    }
    SimpleBuffer sb;
    sb.append(mx.ts.data(), mx.ts.size());
    d.decode(sb);
    mx.ts.clear();
}

PesSegment wholeSegment(size_t n, const PesTime& t, bool zeroLength = false) {
    return PesSegment{0, n, t, zeroLength};
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

struct ErrInfo {
    uint16_t pid;
    std::string message;
};

struct PesInfo {
    bool completed;
    bool broken;
    size_t size;
};

struct Harness {
    MpegTsDemuxer d;
    std::vector<H264AccessUnit> aus;
    std::vector<ErrInfo> errors;
    std::vector<PesInfo> pesFrames;

    void installH264() {
        d.h264OutCallback = [this](const H264AccessUnit& au) {
            H264AccessUnit copy;
            copy.pid = au.pid;
            copy.annexB = au.annexB;
            copy.hasPts = au.hasPts;
            copy.hasDts = au.hasDts;
            copy.pts = au.pts;
            copy.dts = au.dts;
            copy.isIdr = au.isIdr;
            aus.push_back(std::move(copy));
        };
        d.h264ErrorCallback = [this](uint16_t pid, const std::string& msg) {
            errors.push_back({pid, msg});
        };
    }

    void installEs() {
        d.esOutCallback = [this](const EsFrame& f) {
            pesFrames.push_back({f.mCompleted, f.mBroken, f.mData->size()});
        };
    }

    void feedTs(const std::vector<uint8_t>& ts) {
        SimpleBuffer sb;
        sb.append(ts.data(), ts.size());
        d.decode(sb);
    }
};

std::vector<uint8_t> readMaterial() {
    std::ifstream file("task_support/two_pictures.h264", std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
}

// Build the synthetic two-picture stream used by timestamp tests.
struct SyntheticTwoPic {
    std::vector<uint8_t> stream;
    size_t au2VclStart = 0;
    size_t au1VclStart = 0;
};

SyntheticTwoPic makeTwoPictureStream() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    SliceLayout layout;
    SliceDesc a;
    a.nalType = 5;
    a.nalRefIdc = 3;
    a.idrPicId = 0;
    SliceDesc b = a;
    b.idrPicId = 1;
    std::vector<uint8_t> sa = buildSlice(a, layout);
    std::vector<uint8_t> sb = buildSlice(b, layout);

    SyntheticTwoPic r;
    std::vector<std::vector<uint8_t>> all = {sps, pps, sa, sb};
    r.stream = annexBOf(all);
    auto nals = locateNals(r.stream);
    // NAL order: SPS PPS IDR IDR
    r.au1VclStart = nals[2].start;
    r.au2VclStart = nals[3].start;
    return r;
}

} // namespace

// ===========================================================================
// pictures group
// ===========================================================================

// The provided material: two pictures without AUD, each with two slices.
// Redistribute PES boundaries in small chunks and require exact normalized
// bytes and correct NAL grouping.
static int pictures_material_repartitioned() {
    std::vector<uint8_t> material = readMaterial();
    TASK_CHECK(material.size() == 1583);

    for (size_t chunk : {size_t(37), size_t(1), size_t(200)}) {
        Harness h;
        h.installH264();
        TsMux mx;
        sendPsi(h.d, mx, 0x0100, {{0x1b, 0x0101}});
        std::vector<PesSegment> segs;
        for (size_t off = 0; off < material.size(); off += chunk) {
            size_t end = std::min(material.size(), off + chunk);
            segs.push_back({off, end, PesTime{0, 0, 0}, false});
        }
        feedSegments(h.d, mx, 0x0101, material, segs);
        h.d.finish();

        TASK_CHECK(h.errors.empty());
        TASK_CHECK(h.aus.size() == 2);
        if (h.aus.size() != 2) {
            return 1;
        }
        TASK_CHECK(h.aus[0].pid == 0x0101);
        TASK_CHECK(h.aus[1].pid == 0x0101);
        TASK_CHECK(h.aus[0].isIdr);
        TASK_CHECK(h.aus[1].isIdr);

        std::vector<int> t0 = nalTypes(h.aus[0]);
        std::vector<int> t1 = nalTypes(h.aus[1]);
        TASK_CHECK((t0 == std::vector<int>{7, 8, 6, 5, 5}));
        TASK_CHECK((t1 == std::vector<int>{7, 8, 5, 5}));

        // Every start code is four bytes and bytes otherwise unchanged.
        TASK_CHECK(normalizeStartCodes(h.aus[0].annexB) == h.aus[0].annexB);
        TASK_CHECK(normalizeStartCodes(h.aus[1].annexB) == h.aus[1].annexB);

        std::vector<uint8_t> concatenated;
        concatenated.insert(concatenated.end(), h.aus[0].annexB.begin(), h.aus[0].annexB.end());
        concatenated.insert(concatenated.end(), h.aus[1].annexB.begin(), h.aus[1].annexB.end());
        TASK_CHECK(concatenated == normalizeStartCodes(material));
    }
    return 0;
}

// Identity matrix: frame_num, reference identity, IDR and idr_pic_id decide
// boundaries; first_mb_in_slice alone must not.
static int pictures_slice_identity_matrix() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    SliceLayout layout;

    std::vector<SliceDesc> descs(11);
    // AU1: three IDR slices with identical identity but different first_mb.
    descs[0] = SliceDesc{5, 3, 0, 0, 0};
    descs[1] = SliceDesc{5, 3, 2, 0, 0};
    descs[2] = SliceDesc{5, 3, 7, 0, 0};
    // AU2: frame_num 1, two slices.
    descs[3] = SliceDesc{1, 2, 0, 0, 1};
    descs[4] = SliceDesc{1, 2, 2, 0, 1};
    // AU3: frame_num 2, one slice.
    descs[5] = SliceDesc{1, 2, 0, 0, 2};
    // AU4: same frame_num 2 but reference identity drops to 0.
    descs[6] = SliceDesc{1, 0, 0, 0, 2};
    descs[7] = SliceDesc{1, 0, 2, 0, 2};
    // AU5: IDR again.
    descs[8] = SliceDesc{5, 3, 0, 0, 0};
    // AU6: IDR with a different idr_pic_id, two slices.
    descs[9] = SliceDesc{5, 3, 0, 0, 0, 1};
    descs[10] = SliceDesc{5, 3, 2, 0, 0, 1};

    std::vector<std::vector<uint8_t>> nals = {sps, pps};
    for (const SliceDesc& d : descs) {
        nals.push_back(buildSlice(d, layout));
    }
    std::vector<uint8_t> h264 = annexBOf(nals);

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0200, {{0x1b, 0x0201}});
    feedSegments(h.d, mx, 0x0201, h264, {wholeSegment(h264.size(), PesTime{2, 42, 0})});
    h.d.finish();

    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 6);
    if (h.aus.size() != 6) {
        std::printf("identity matrix AU count %zu errors=%zu\n", h.aus.size(), h.errors.size());
        return 1;
    }
    size_t expectedSlices[] = {3, 2, 1, 2, 1, 2};
    for (size_t i = 0; i < 6; ++i) {
        int sliceCount = 0;
        for (int t : nalTypes(h.aus[i])) {
            if (t == 1 || t == 5) {
                ++sliceCount;
            }
        }
        TASK_CHECK(sliceCount == static_cast<int>(expectedSlices[i]));
    }
    TASK_CHECK(h.aus[0].isIdr);
    TASK_CHECK(h.aus[5].isIdr);
    TASK_CHECK(!h.aus[1].isIdr);
    return 0;
}

// Changing pic_parameter_set_id starts a new picture.
static int pictures_pps_change() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    std::vector<uint8_t> pps0 = buildPps(0, 0, false);
    std::vector<uint8_t> pps1 = buildPps(1, 0, false);
    SliceLayout layout;
    SliceDesc s0;
    s0.nalType = 5;
    SliceDesc s1;
    s1.nalType = 5;
    s1.ppsId = 1;
    SliceDesc s2;
    s2.nalType = 5;
    s2.ppsId = 0;

    std::vector<uint8_t> h264 = annexBOf({sps, pps0, pps1,
                                          buildSlice(s0, layout),
                                          buildSlice(s1, layout),
                                          buildSlice(s2, layout)});
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0210, {{0x1b, 0x0211}});
    feedSegments(h.d, mx, 0x0211, h264, {wholeSegment(h264.size(), PesTime{})});
    h.d.finish();
    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 3);
    return 0;
}

// POC type 0 fields split pictures.
static int pictures_poc_type0() {
    std::vector<uint8_t> sps = buildSps(0, 0, 0, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, true);
    SliceLayout l;
    l.pocType = 0;
    l.bottomFieldPoc = true;

    SliceDesc a;
    a.nalType = 5;
    a.pocLsb = 4;
    SliceDesc a2 = a; // second slice same picture
    a2.firstMb = 2;
    SliceDesc b;
    b.nalType = 5;
    b.pocLsb = 6; // different pic_order_cnt_lsb
    SliceDesc c;
    c.nalType = 5;
    c.pocLsb = 6;
    c.deltaPocBottom = 2; // different delta_pic_order_cnt_bottom

    std::vector<uint8_t> h264 = annexBOf({sps, pps,
                                          buildSlice(a, l), buildSlice(a2, l),
                                          buildSlice(b, l), buildSlice(c, l)});
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0220, {{0x1b, 0x0221}});
    feedSegments(h.d, mx, 0x0221, h264, {wholeSegment(h264.size(), PesTime{})});
    h.d.finish();
    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 3);
    if (h.aus.size() == 3) {
        int n0 = 0;
        for (int t : nalTypes(h.aus[0])) if (t == 5) ++n0;
        TASK_CHECK(n0 == 2);
    }
    return 0;
}

// POC type 1 delta fields split pictures.
static int pictures_poc_type1() {
    std::vector<uint8_t> sps = buildSps(0, 0, 1, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, true);
    SliceLayout l;
    l.pocType = 1;
    l.bottomFieldPoc = true;

    SliceDesc a;
    a.nalType = 5;
    a.deltaPoc0 = 0;
    a.deltaPoc1 = 0;
    SliceDesc b;
    b.nalType = 5;
    b.deltaPoc0 = 1; // delta_pic_order_cnt[0] differs
    SliceDesc c;
    c.nalType = 5;
    c.deltaPoc0 = 1;
    c.deltaPoc1 = -2; // delta_pic_order_cnt[1] differs

    std::vector<uint8_t> h264 = annexBOf({sps, pps, buildSlice(a, l),
                                          buildSlice(b, l), buildSlice(c, l)});
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0230, {{0x1b, 0x0231}});
    feedSegments(h.d, mx, 0x0231, h264, {wholeSegment(h264.size(), PesTime{})});
    h.d.finish();
    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 3);
    return 0;
}

// Field/frame identity and bottom field identity split pictures.
static int pictures_field_identity() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, false, false);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    SliceLayout l;
    l.frameOnly = false;

    SliceDesc frame;
    frame.nalType = 5;
    frame.fieldPic = 0;
    SliceDesc top;
    top.nalType = 5;
    top.fieldPic = 1;
    top.bottomField = 0;
    SliceDesc bottom;
    bottom.nalType = 5;
    bottom.fieldPic = 1;
    bottom.bottomField = 1;
    SliceDesc bottomSlice = bottom;
    bottomSlice.firstMb = 2; // second slice of the same bottom field

    std::vector<uint8_t> h264 = annexBOf({sps, pps,
                                          buildSlice(frame, l),
                                          buildSlice(top, l),
                                          buildSlice(bottom, l),
                                          buildSlice(bottomSlice, l)});
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0240, {{0x1b, 0x0241}});
    feedSegments(h.d, mx, 0x0241, h264, {wholeSegment(h264.size(), PesTime{})});
    h.d.finish();
    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 3);
    if (h.aus.size() == 3) {
        int bottomSlices = 0;
        for (int t : nalTypes(h.aus[2])) if (t == 5) ++bottomSlices;
        TASK_CHECK(bottomSlices == 2);
    }
    return 0;
}

// High profile SPS with chroma and a scaling list must parse so slices frame.
static int pictures_high_profile() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false, 100, true);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    SliceLayout l;
    SliceDesc s;
    s.nalType = 5;
    s.injectEscapeBytes = true;
    std::vector<uint8_t> h264 = annexBOf({sps, pps, buildSlice(s, l)});

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0250, {{0x1b, 0x0251}});
    // Feed one byte per PES to stress cross-PES start codes and escaping.
    std::vector<PesSegment> segs;
    for (size_t i = 0; i < h264.size(); ++i) {
        segs.push_back({i, i + 1, PesTime{}, false});
    }
    feedSegments(h.d, mx, 0x0251, h264, segs);
    h.d.finish();
    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 1);
    if (h.aus.size() == 1) {
        TASK_CHECK(nalTypes(h.aus[0]) == (std::vector<int>{7, 8, 5}));
        TASK_CHECK(normalizeStartCodes(h.aus[0].annexB) == h.aus[0].annexB);
    }
    return 0;
}

// SPS/PPS/SEI/AUD after VCL lead the next AU; EOS/filler stay current.
static int pictures_nal_placement() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    SliceLayout l;
    SliceDesc idrA;
    idrA.nalType = 5;
    idrA.idrPicId = 0;
    SliceDesc idrAslice = idrA;
    idrAslice.firstMb = 2;
    SliceDesc idrB = idrA;
    idrB.idrPicId = 1;
    SliceDesc idrC = idrA;
    idrC.idrPicId = 2;

    std::vector<uint8_t> sei(1, 0x06);
    sei.push_back(0x05);
    sei.push_back(0xaa);
    std::vector<uint8_t> aud(1, 0x09);
    aud.push_back(0x10);
    std::vector<uint8_t> eos(1, 0x0a);
    std::vector<uint8_t> filler(4, 0x0c);
    filler[1] = 0xff;
    filler[2] = 0xff;
    filler[3] = 0x80;

    std::vector<uint8_t> h264 = annexBOf({
        sps, pps, sei, buildSlice(idrA, l), buildSlice(idrAslice, l),
        sps, pps, sei, aud, buildSlice(idrB, l), eos, filler,
        buildSlice(idrC, l)});

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0260, {{0x1b, 0x0261}});
    feedSegments(h.d, mx, 0x0261, h264, {wholeSegment(h264.size(), PesTime{2, 7, 0})});
    h.d.finish();

    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 3);
    if (h.aus.size() == 3) {
        TASK_CHECK(nalTypes(h.aus[0]) == (std::vector<int>{7, 8, 6, 5, 5}));
        TASK_CHECK(nalTypes(h.aus[1]) == (std::vector<int>{7, 8, 6, 9, 5, 10, 12}));
        TASK_CHECK(nalTypes(h.aus[2]) == (std::vector<int>{5}));
    }
    return 0;
}

// Parameter sets / AUD / SEI without any VCL do not produce a picture.
static int pictures_pure_parameter_sets() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    std::vector<uint8_t> aud = {0x09, 0x10};
    std::vector<uint8_t> sei = {0x06, 0x01, 0x80};
    std::vector<uint8_t> h264 = annexBOf({aud, sps, pps, sei});

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0270, {{0x1b, 0x0271}});
    feedSegments(h.d, mx, 0x0271, h264, {wholeSegment(h264.size(), PesTime{2, 9, 0})});
    h.d.finish();
    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.empty());
    return 0;
}

// Two H.264 PIDs in one PMT frame independently and keep their pid.
static int pictures_dual_pid() {
    std::vector<uint8_t> material = readMaterial();
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0300, {{0x1b, 0x0301}, {0x1b, 0x0302}});

    // Interleave PES chunks of both PIDs.
    std::vector<PesSegment> segsA;
    std::vector<PesSegment> segsB;
    for (size_t off = 0; off < material.size(); off += 97) {
        size_t end = std::min(material.size(), off + 97);
        segsA.push_back({off, end, PesTime{2, 1000 + off, 0}, false});
        segsB.push_back({off, end, PesTime{2, 5000 + off, 0}, false});
    }
    for (size_t i = 0; i < segsA.size(); ++i) {
        feedSegments(h.d, mx, 0x0301, material, {segsA[i]});
        feedSegments(h.d, mx, 0x0302, material, {segsB[i]});
    }
    h.d.finish();

    TASK_CHECK(h.errors.empty());
    int pid1 = 0;
    int pid2 = 0;
    for (const H264AccessUnit& au : h.aus) {
        if (au.pid == 0x0301) {
            ++pid1;
        }
        if (au.pid == 0x0302) {
            ++pid2;
        }
    }
    TASK_CHECK(pid1 == 2);
    TASK_CHECK(pid2 == 2);
    TASK_CHECK(h.aus.size() == 4);
    return 0;
}

// ===========================================================================
// timestamps group
// ===========================================================================

static int timestamps_single_pes_pts_only() {
    SyntheticTwoPic s = makeTwoPictureStream();
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0400, {{0x1b, 0x0401}});
    feedSegments(h.d, mx, 0x0401, s.stream,
                 {wholeSegment(s.stream.size(), PesTime{2, 12345, 0})});
    h.d.finish();
    TASK_CHECK(h.aus.size() == 2);
    if (h.aus.size() == 2) {
        TASK_CHECK(h.aus[0].hasPts);
        TASK_CHECK(!h.aus[0].hasDts);
        TASK_CHECK(h.aus[0].pts == 12345);
        TASK_CHECK(h.aus[0].dts == 12345); // aliases PTS while hasDts is false
        TASK_CHECK(!h.aus[1].hasPts);
        TASK_CHECK(!h.aus[1].hasDts);
    }
    return 0;
}

static int timestamps_pts_dts_33bit() {
    SyntheticTwoPic s = makeTwoPictureStream();
    const uint64_t pts = 0x1FFFFFFFFull; // max 33 bit
    const uint64_t dts = 0x1FFFFFFF0ull;
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0410, {{0x1b, 0x0411}});
    feedSegments(h.d, mx, 0x0411, s.stream,
                 {wholeSegment(s.stream.size(), PesTime{3, pts, dts})});
    h.d.finish();
    TASK_CHECK(h.aus.size() == 2);
    if (h.aus.size() == 2) {
        TASK_CHECK(h.aus[0].hasPts);
        TASK_CHECK(h.aus[0].hasDts);
        TASK_CHECK(h.aus[0].pts == pts);
        TASK_CHECK(h.aus[0].dts == dts);
        TASK_CHECK(!h.aus[1].hasPts);
    }
    return 0;
}

// The first zero byte of AU2's VCL start code lands in an earlier PES; that
// PES's time must win even when only one or two zero bytes are present in it.
static int timestamps_start_code_crosses_pes() {
    SyntheticTwoPic s = makeTwoPictureStream();
    // Three PES arrangement: PES1 owns AU1's start/time, PES2 ends after the
    // first zero(s) of AU2's start code and donates its time, PES3 carries
    // the rest. The first zero decides the origin.
    for (int zerosInMiddlePes = 1; zerosInMiddlePes <= 2; ++zerosInMiddlePes) {
        Harness h;
        h.installH264();
        TsMux mx;
        sendPsi(h.d, mx, 0x0430, {{0x1b, 0x0431}});

        size_t cut1 = s.au1VclStart + 1;
        size_t cut2 = s.au2VclStart + zerosInMiddlePes;
        TASK_CHECK(cut1 < s.au2VclStart);
        std::vector<PesSegment> segs = {
            {0, cut1, PesTime{2, 111, 0}, false},
            {cut1, cut2, PesTime{2, 222, 0}, false},
            {cut2, s.stream.size(), PesTime{3, 333, 330}, false},
        };
        feedSegments(h.d, mx, 0x0431, s.stream, segs);
        h.d.finish();
        TASK_CHECK(h.errors.empty());
        TASK_CHECK(h.aus.size() == 2);
        if (h.aus.size() == 2) {
            TASK_CHECK(h.aus[0].pts == 111);
            TASK_CHECK(h.aus[1].hasPts);
            TASK_CHECK(h.aus[1].pts == 222);
            TASK_CHECK(!h.aus[1].hasDts);
            TASK_CHECK(h.aus[1].dts == 222);
        }
    }
    return 0;
}

// A PES carrying only the tail of an old NAL must not bank its PTS for a
// later picture.
static int timestamps_tail_only_pes() {
    SyntheticTwoPic s = makeTwoPictureStream();
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0440, {{0x1b, 0x0441}});

    size_t cut1 = s.au1VclStart + 3; // inside AU1 slice header/body
    size_t cut2 = s.au2VclStart;     // AU2 start fully inside PES3
    PesSegment segs[] = {
        {0, cut1, PesTime{2, 111, 0}, false},
        {cut1, cut2, PesTime{2, 222, 0}, false}, // tail only, no start code
        {cut2, s.stream.size(), PesTime{2, 333, 0}, false},
    };
    feedSegments(h.d, mx, 0x0441, s.stream,
                 std::vector<PesSegment>(segs, segs + 3));
    h.d.finish();
    TASK_CHECK(h.aus.size() == 2);
    if (h.aus.size() == 2) {
        TASK_CHECK(h.aus[0].pts == 111);
        TASK_CHECK(h.aus[1].pts == 333);
    }
    return 0;
}

// Missing timestamps are not inherited from a previous frame.
static int timestamps_no_carryover() {
    SyntheticTwoPic s = makeTwoPictureStream();
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0450, {{0x1b, 0x0451}});

    size_t cut = s.au2VclStart;
    PesSegment segs[] = {
        {0, cut, PesTime{0, 0, 0}, false},       // no PTS/DTS
        {cut, s.stream.size(), PesTime{2, 777, 0}, false},
    };
    feedSegments(h.d, mx, 0x0451, s.stream,
                 std::vector<PesSegment>(segs, segs + 2));
    h.d.finish();
    TASK_CHECK(h.aus.size() == 2);
    if (h.aus.size() == 2) {
        TASK_CHECK(!h.aus[0].hasPts);
        TASK_CHECK(h.aus[0].pts == 0);
        TASK_CHECK(h.aus[1].hasPts);
        TASK_CHECK(h.aus[1].pts == 777);
    }
    return 0;
}

// One PES carrying several pictures donates its time to the first picture
// only; the material stream is a two picture example.
static int timestamps_one_pes_several_pictures() {
    std::vector<uint8_t> material = readMaterial();
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0460, {{0x1b, 0x0461}});
    feedSegments(h.d, mx, 0x0461, material,
                 {wholeSegment(material.size(), PesTime{2, 9000, 0})});
    h.d.finish();
    TASK_CHECK(h.aus.size() == 2);
    if (h.aus.size() == 2) {
        TASK_CHECK(h.aus[0].hasPts && h.aus[0].pts == 9000);
        TASK_CHECK(!h.aus[1].hasPts);
    }
    return 0;
}

// ===========================================================================
// lifecycle group
// ===========================================================================

static int lifecycle_finish_flushes_and_is_idempotent() {
    std::vector<uint8_t> material = readMaterial();
    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0500, {{0x1b, 0x0501}});
    feedSegments(h.d, mx, 0x0501, material,
                 {wholeSegment(material.size(), PesTime{2, 1, 0})});

    // Before finish only the first AU could be emitted (boundary detected).
    TASK_CHECK(h.aus.size() >= 1);
    h.d.finish();
    TASK_CHECK(h.aus.size() == 2);
    h.d.finish();
    h.d.finish();
    TASK_CHECK(h.aus.size() == 2);
    TASK_CHECK(h.errors.empty());
    return 0;
}

// A zero length PES is an unbounded video PES; finish delivers it through
// esOutCallback as completed and still emits the H.264 AU.
static int lifecycle_zero_length_pes() {
    std::vector<uint8_t> material = readMaterial();
    Harness h;
    h.installH264();
    h.installEs();
    TsMux mx;
    sendPsi(h.d, mx, 0x0510, {{0x1b, 0x0511}});
    feedSegments(h.d, mx, 0x0511, material,
                 {wholeSegment(material.size(), PesTime{2, 55, 0}, true)});

    TASK_CHECK(h.pesFrames.empty()); // not completed by length
    h.d.finish();
    TASK_CHECK(h.pesFrames.size() == 1);
    if (h.pesFrames.size() == 1) {
        TASK_CHECK(h.pesFrames[0].completed);
        TASK_CHECK(!h.pesFrames[0].broken);
        TASK_CHECK(h.pesFrames[0].size == material.size());
    }
    TASK_CHECK(h.aus.size() == 2);
    h.d.finish();
    TASK_CHECK(h.pesFrames.size() == 1);
    TASK_CHECK(h.aus.size() == 2);
    return 0;
}

// With h264OutCallback installed the original esOutCallback behavior remains.
static int lifecycle_esout_still_served() {
    std::vector<uint8_t> material = readMaterial();
    Harness h;
    h.installH264();
    h.installEs();
    TsMux mx;
    sendPsi(h.d, mx, 0x0520, {{0x1b, 0x0521}});
    feedSegments(h.d, mx, 0x0521, material,
                 {wholeSegment(material.size(), PesTime{2, 9, 0})});
    TASK_CHECK(h.pesFrames.size() == 1);
    if (h.pesFrames.size() == 1) {
        TASK_CHECK(h.pesFrames[0].completed);
        TASK_CHECK(h.pesFrames[0].size == material.size());
    }
    h.d.finish();
    TASK_CHECK(h.pesFrames.size() == 1);
    TASK_CHECK(h.aus.size() == 2);
    return 0;
}

static std::vector<uint8_t> recoveryStream() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    SliceLayout l;
    SliceDesc idr;
    idr.nalType = 5;
    return annexBOf({sps, pps, buildSlice(idr, l)});
}

// Incomplete NAL header (empty NAL between two start codes) reports an error,
// drops the pending picture and recovers at a fresh legal IDR.
static int lifecycle_incomplete_header_recovery() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    std::vector<uint8_t> pps = buildPps(0, 0, false);
    SliceLayout l;
    SliceDesc idr;
    idr.nalType = 5;
    std::vector<uint8_t> good = annexBOf({sps, pps, buildSlice(idr, l)});

    // good AU, then an empty NAL, then a fresh legal IDR AU.
    std::vector<uint8_t> bad = good;
    static const uint8_t emptyNal[] = {0, 0, 0, 1}; // start code with no header
    bad.insert(bad.end(), emptyNal, emptyNal + sizeof(emptyNal));
    std::vector<uint8_t> recovery = recoveryStream();

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0530, {{0x1b, 0x0531}});
    feedSegments(h.d, mx, 0x0531, bad, {wholeSegment(bad.size(), PesTime{})});
    // The second start code of recovery makes the empty NAL observable; feed
    // recovery which starts with its own SPS start code.
    feedSegments(h.d, mx, 0x0531, recovery,
                 {wholeSegment(recovery.size(), PesTime{2, 88, 0})});
    h.d.finish();

    TASK_CHECK(h.errors.size() >= 1);
    if (!h.errors.empty()) {
        TASK_CHECK(h.errors[0].pid == 0x0531);
    }
    TASK_CHECK(h.aus.size() == 1);
    if (h.aus.size() == 1) {
        TASK_CHECK(h.aus[0].pid == 0x0531);
        TASK_CHECK(h.aus[0].isIdr);
        TASK_CHECK(h.aus[0].pts == 88);
    }
    return 0;
}

// Unknown PPS is reported and framing recovers at a new legal IDR.
static int lifecycle_unknown_pps_recovery() {
    std::vector<uint8_t> sps = buildSps(0, 0, 2, 0, true, false);
    SliceLayout l;
    SliceDesc bad;
    bad.nalType = 5;
    bad.ppsId = 9;
    std::vector<uint8_t> badStream = annexBOf({sps, buildSlice(bad, l)});
    // The recovery SPS start code completes the bad slice and reports it.
    std::vector<uint8_t> recovery = recoveryStream();

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0540, {{0x1b, 0x0541}});
    feedSegments(h.d, mx, 0x0541, badStream,
                 {wholeSegment(badStream.size(), PesTime{})});
    feedSegments(h.d, mx, 0x0541, recovery,
                 {wholeSegment(recovery.size(), PesTime{})});
    h.d.finish();

    TASK_CHECK(h.errors.size() == 1);
    if (h.errors.size() == 1) {
        TASK_CHECK(h.errors[0].pid == 0x0541);
    }
    TASK_CHECK(h.aus.size() == 1);
    return 0;
}

// Unsupported partition NAL (type 2) reports and recovers.
static int lifecycle_partition_nal_recovery() {
    std::vector<uint8_t> partitionNal = {0x82, 0x11, 0x22, 0x33}; // type 2
    std::vector<uint8_t> badStream = annexBOf({partitionNal});
    std::vector<uint8_t> recovery = recoveryStream();

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0550, {{0x1b, 0x0551}});
    feedSegments(h.d, mx, 0x0551, badStream,
                 {wholeSegment(badStream.size(), PesTime{})});
    feedSegments(h.d, mx, 0x0551, recovery,
                 {wholeSegment(recovery.size(), PesTime{})});
    h.d.finish();

    TASK_CHECK(h.errors.size() == 1);
    if (h.errors.size() == 1) {
        TASK_CHECK(h.errors[0].pid == 0x0551);
    }
    TASK_CHECK(h.aus.size() == 1);
    return 0;
}

// Unsupported extension NAL (type 20) reports and recovers.
static int lifecycle_extension_nal_recovery() {
    std::vector<uint8_t> extNal(6, 0x00);
    extNal[0] = 0x74; // nal_ref_idc 3, type 20
    extNal[1] = 0x80;
    std::vector<uint8_t> badStream = annexBOf({extNal});
    std::vector<uint8_t> recovery = recoveryStream();

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0560, {{0x1b, 0x0561}});
    feedSegments(h.d, mx, 0x0561, badStream,
                 {wholeSegment(badStream.size(), PesTime{})});
    feedSegments(h.d, mx, 0x0561, recovery,
                 {wholeSegment(recovery.size(), PesTime{})});
    h.d.finish();

    TASK_CHECK(h.errors.size() == 1);
    TASK_CHECK(h.aus.size() == 1);
    return 0;
}

// An error on one PID does not disturb framing on another PID.
static int lifecycle_dual_pid_error_isolation() {
    std::vector<uint8_t> partitionNal = {0x82, 0x11};
    std::vector<uint8_t> badStream = annexBOf({partitionNal});
    std::vector<uint8_t> material = readMaterial();

    Harness h;
    h.installH264();
    TsMux mx;
    sendPsi(h.d, mx, 0x0570, {{0x1b, 0x0571}, {0x1b, 0x0572}});
    // Feed the bad PID first; its error is reported at finish.
    feedSegments(h.d, mx, 0x0571, badStream,
                 {wholeSegment(badStream.size(), PesTime{})});
    // Good PID frames normally in the same stream.
    feedSegments(h.d, mx, 0x0572, material,
                 {wholeSegment(material.size(), PesTime{2, 4242, 0})});
    h.d.finish();

    TASK_CHECK(h.errors.size() == 1);
    if (h.errors.size() == 1) {
        TASK_CHECK(h.errors[0].pid == 0x0571);
    }
    int goodPidAus = 0;
    for (const H264AccessUnit& au : h.aus) {
        TASK_CHECK(au.pid == 0x0572);
        if (au.pid == 0x0572) {
            ++goodPidAus;
        }
    }
    TASK_CHECK(goodPidAus == 2);
    if (h.aus.size() == 2) {
        TASK_CHECK(h.aus[0].pts == 4242 || h.aus[1].pts == 4242);
    }
    return 0;
}

// Byte-wise decode() calls (small input buffers) still frame correctly.
static int lifecycle_incremental_decode() {
    std::vector<uint8_t> material = readMaterial();
    Harness h;
    h.installH264();

    // PAT/PMT first.
    TsMux psiMux;
    sendPsi(h.d, psiMux, 0x0580, {{0x1b, 0x0581}});

    // Build the PES/TS without decoding it, then feed in 17 byte chunks.
    TsMux mx;
    mx.pesBytes(0x0581, buildPes(material, PesTime{2, 321, 0}, false));
    for (size_t off = 0; off < mx.ts.size(); off += 17) {
        size_t end = std::min(mx.ts.size(), off + 17);
        SimpleBuffer sb;
        sb.append(mx.ts.data() + off, end - off);
        h.d.decode(sb);
    }
    h.d.finish();
    TASK_CHECK(h.errors.empty());
    TASK_CHECK(h.aus.size() == 2);
    if (h.aus.size() == 2) {
        TASK_CHECK(h.aus[0].pts == 321);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"pictures", "material_two_pictures_repartitioned", pictures_material_repartitioned},
    {"pictures", "slice_identity_matrix", pictures_slice_identity_matrix},
    {"pictures", "pps_change", pictures_pps_change},
    {"pictures", "poc_type0", pictures_poc_type0},
    {"pictures", "poc_type1", pictures_poc_type1},
    {"pictures", "field_identity", pictures_field_identity},
    {"pictures", "high_profile", pictures_high_profile},
    {"pictures", "nal_placement", pictures_nal_placement},
    {"pictures", "pure_parameter_sets", pictures_pure_parameter_sets},
    {"pictures", "dual_pid", pictures_dual_pid},

    {"timestamps", "single_pes_pts_only", timestamps_single_pes_pts_only},
    {"timestamps", "pts_dts_33bit", timestamps_pts_dts_33bit},
    {"timestamps", "start_code_crosses_pes", timestamps_start_code_crosses_pes},
    {"timestamps", "tail_only_pes", timestamps_tail_only_pes},
    {"timestamps", "no_carryover", timestamps_no_carryover},
    {"timestamps", "one_pes_several_pictures", timestamps_one_pes_several_pictures},

    {"lifecycle", "finish_flushes_and_is_idempotent", lifecycle_finish_flushes_and_is_idempotent},
    {"lifecycle", "zero_length_pes", lifecycle_zero_length_pes},
    {"lifecycle", "esout_still_served", lifecycle_esout_still_served},
    {"lifecycle", "incomplete_header_recovery", lifecycle_incomplete_header_recovery},
    {"lifecycle", "unknown_pps_recovery", lifecycle_unknown_pps_recovery},
    {"lifecycle", "partition_nal_recovery", lifecycle_partition_nal_recovery},
    {"lifecycle", "extension_nal_recovery", lifecycle_extension_nal_recovery},
    {"lifecycle", "dual_pid_error_isolation", lifecycle_dual_pid_error_isolation},
    {"lifecycle", "incremental_decode", lifecycle_incremental_decode},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
