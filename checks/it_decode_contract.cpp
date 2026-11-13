#include "contract.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "dumb.h"
#include "internal/it.h"
}

namespace {

using Bytes = std::vector<unsigned char>;

void put_u16(Bytes &b, size_t p, uint32_t v) {
    b[p] = static_cast<unsigned char>(v);
    b[p + 1] = static_cast<unsigned char>(v >> 8);
}
void put_u32(Bytes &b, size_t p, uint32_t v) {
    b[p] = static_cast<unsigned char>(v);
    b[p + 1] = static_cast<unsigned char>(v >> 8);
    b[p + 2] = static_cast<unsigned char>(v >> 16);
    b[p + 3] = static_cast<unsigned char>(v >> 24);
}
void put_id(Bytes &b, size_t p, const char *id) {
    std::memcpy(b.data() + p, id, 4);
}

/* ------------------------- IT module construction ------------------------ */

struct SampleSpec {
    int flags = 0;          /* IT flag bits, combined below */
    unsigned convert = 1;   /* sample convert byte: bit 0 set = signed */
    long length = 0;        /* frames declared on disk */
    long loop_start = 0;
    long loop_end = 0;
    long sus_start = 0;
    long sus_end = 0;
    bool sixteen = false;
    bool stereo = false;
    bool packed = false;
    bool it215 = false;
    bool loop = false;
    bool sus_loop = false;
};

struct Module {
    Bytes bytes;
    long sample_header = -1;
    long data_offset = -1;
};

/* Build a minimal but structurally valid IT module holding exactly one sample
 * whose data begins at `data_offset`. Everything before the data is padded so
 * the data can be placed at a caller-chosen position. */
Module make_module(const SampleSpec &s, long data_offset,
                   Bytes data = Bytes()) {
    Module m;
    const size_t header_size = 0xc0 + 80;
    size_t total = static_cast<size_t>(data_offset) + data.size();
    if (total < header_size)
        total = header_size;
    m.bytes.assign(total, 0);

    Bytes &b = m.bytes;
    put_id(b, 0, "IMPM");
    b[32] = 1; b[33] = 0;  /* n_orders = 1 */
    b[34] = 0; b[35] = 0;  /* n_instruments */
    b[36] = 1; b[37] = 0;  /* n_samples = 1 */
    b[38] = 0; b[39] = 0;  /* n_patterns */
    put_u16(b, 40, 0x0215); /* const magic? cwt */
    put_u16(b, 42, 0x0214); /* cmwt: new instrument format */
    put_u16(b, 44, 1);      /* flags */
    put_u16(b, 46, 0);      /* special */
    b[48] = 128; b[49] = 48; b[50] = 6; b[51] = 125; b[52] = 128;
    put_u16(b, 54, 0);      /* message length */
    put_u32(b, 56, 0);      /* message offset */
    for (int i = 0; i < 64; ++i) { b[64 + i] = 32; b[128 + i] = 64; }
    b[192] = 0;             /* order[0] */
    put_u32(b, 193, 0);     /* sample header offset (no instruments) */
    put_u32(b, 197, 0);     /* pattern offsets (none) */

    m.sample_header = static_cast<long>(header_size);
    /* Place the sample header at 0xc0+80; the header is 80 bytes. Data then
     * cannot precede header_size+80. */
    long sh = m.sample_header;
    if (static_cast<size_t>(data_offset) < header_size + 80) {
        /* Not enough room: caller must choose a larger data_offset. */
    }
    put_u32(b, 193, static_cast<uint32_t>(sh));
    put_id(b, static_cast<size_t>(sh), "IMPS");
    b[sh + 17] = 64;            /* global volume */
    b[sh + 18] = static_cast<unsigned char>(s.flags);
    b[sh + 19] = 64;            /* default volume */
    b[sh + 46] = s.convert;
    put_u32(b, sh + 48, static_cast<uint32_t>(s.length));
    put_u32(b, sh + 52, static_cast<uint32_t>(s.loop_start));
    put_u32(b, sh + 56, static_cast<uint32_t>(s.loop_end));
    put_u32(b, sh + 60, 8363);
    put_u32(b, sh + 64, static_cast<uint32_t>(s.sus_start));
    put_u32(b, sh + 68, static_cast<uint32_t>(s.sus_end));
    put_u32(b, sh + 72, static_cast<uint32_t>(data_offset));
    b[sh + 76] = 0; b[sh + 77] = 0; b[sh + 78] = 0; b[sh + 79] = 0;

    m.data_offset = data_offset;
    if (!data.empty())
        std::memcpy(b.data() + data_offset, data.data(), data.size());
    return m;
}

unsigned sample_flags(const SampleSpec &s) {
    unsigned f = IT_SAMPLE_EXISTS;
    if (s.sixteen) f |= IT_SAMPLE_16BIT;
    if (s.stereo) f |= IT_SAMPLE_STEREO;
    if (s.packed) f |= 8;
    if (s.loop) f |= IT_SAMPLE_LOOP;
    if (s.sus_loop) f |= IT_SAMPLE_SUS_LOOP;
    return f;
}

/* Bit-level encoder for the IT 2.14/2.15 compressed formats. Produces the
 * 16-bit length-prefixed body of one block from a list of (value,width)
 * operations. Width-change control words are expressed as operations with a
 * flag marking "no PCM produced". */
struct BitOp {
    int value;
    int width;
    bool control;
};

void pack_bits(Bytes &out, const std::vector<BitOp> &ops) {
    unsigned acc = 0;
    int n = 0;
    for (const BitOp &op : ops) {
        unsigned v = static_cast<unsigned>(op.value) &
                     ((op.width >= 32) ? 0xffffffffu : ((1u << op.width) - 1));
        acc |= v << n;
        n += op.width;
        while (n >= 8) {
            out.push_back(static_cast<unsigned char>(acc & 0xff));
            acc >>= 8;
            n -= 8;
        }
    }
    if (n)
        out.push_back(static_cast<unsigned char>(acc & 0xff));
}

Bytes block_prefix(size_t body_size) {
    Bytes out(2);
    put_u16(out, 0, static_cast<uint32_t>(body_size));
    return out;
}

/* Append one compressed block whose body is built from ops. */
void append_block(Bytes &out, const std::vector<BitOp> &ops) {
    Bytes body;
    pack_bits(body, ops);
    out.push_back(static_cast<unsigned char>(body.size() & 0xff));
    out.push_back(static_cast<unsigned char>(body.size() >> 8));
    out.insert(out.end(), body.begin(), body.end());
}

/* Widening a 9-bit stream: Method 3 sends 0x1xx where xx+1 is the new width. */
BitOp ctrl_m3(int new_width) {
    return BitOp{0x100 | (new_width - 1), 9, true};
}

/* ------------------------------------------------------------ load helpers */

struct Loaded {
    DUH *duh = nullptr;
    DUMBFILE *f = nullptr;
    explicit Loaded(const Bytes &b) {
        f = dumbfile_open_memory(reinterpret_cast<const char *>(b.data()),
                                 b.size());
        if (f)
            duh = dumb_read_it_quick(f);
    }
    ~Loaded() {
        if (duh) unload_duh(duh);
        if (f) dumbfile_close(f);
    }
    IT_SAMPLE *sample() {
        if (!duh) return nullptr;
        DUMB_IT_SIGDATA *p = duh_get_it_sigdata(duh);
        if (!p || p->n_samples != 1) return nullptr;
        return &p->sample[0];
    }
    int pcm(int i) {
        IT_SAMPLE *s = sample();
        if (!s || !s->data) return -999999;
        if (s->flags & IT_SAMPLE_16BIT)
            return static_cast<short *>(s->data)[i];
        return static_cast<signed char *>(s->data)[i];
    }
};

int channels_of(const IT_SAMPLE *s) {
    return (s->flags & IT_SAMPLE_STEREO) ? 2 : 1;
}

/* --------------------------------------------------------- reference model */

/* Decode the same bit stream the loader sees. This mirrors the format (not
 * the loader code) using a simple bit array built by the caller. */
struct RefBitReader {
    std::vector<int> bits;
    size_t pos = 0;
    bool ok = true;
    int read(int n) {
        if (n <= 0 || n > 17) { ok = false; return 0; }
        if (pos + static_cast<size_t>(n) > bits.size()) { ok = false; return 0; }
        int v = 0;
        for (int i = 0; i < n; ++i)
            v |= bits[pos + i] << i;
        pos += n;
        return v;
    }
};

void bytes_to_bits(const Bytes &raw, RefBitReader &r) {
    for (unsigned char c : raw)
        for (int i = 0; i < 8; ++i)
            r.bits.push_back((c >> i) & 1);
}

int signextend(int v, int w) {
    int sign = 1 << (w - 1);
    return (v & sign) ? v - (sign << 1) : v;
}

/* Decode a single channel's stream, producing `disk` frames but retaining
 * only the first `retain`; also mirrors "each block restarts history". */
std::vector<int> ref_decompress8(const Bytes &stream, long disk, long retain,
                                 bool it215, bool &ok,
                                 std::string &error) {
    std::vector<int> out(static_cast<size_t>(retain), 0);
    size_t p = 0;
    long produced = 0;
    ok = true;
    while (produced < disk) {
        if (p + 2 > stream.size()) { ok = false; error = "block prefix"; return out; }
        int size = stream[p] | (stream[p + 1] << 8);
        p += 2;
        if (size <= 0) { ok = false; error = "zero block"; return out; }
        if (p + static_cast<size_t>(size) > stream.size()) {
            ok = false; error = "truncated block"; return out;
        }
        Bytes raw(stream.begin() + p, stream.begin() + p + size);
        p += size;
        RefBitReader r;
        bytes_to_bits(raw, r);
        long blocklen = disk - produced;
        if (blocklen > 0x8000) blocklen = 0x8000;
        int width = 9;
        signed char d1 = 0, d2 = 0;
        for (long i = 0; i < blocklen; ++i) {
            int val = r.read(width);
            if (!r.ok) { ok = false; error = "bits"; return out; }
            if (width < 7) {
                if (val == (1 << (width - 1))) {
                    int operand = r.read(3);
                    if (!r.ok) { ok = false; error = "ctrl1"; return out; }
                    int nw = operand + 1;
                    nw = (nw < width) ? nw : nw + 1;
                    if (nw < 1 || nw > 9) { ok = false; error = "w1"; return out; }
                    width = nw;
                    --i;
                    continue;
                }
            } else if (width < 9) {
                int border = (0xFF >> (9 - width)) - 4;
                if (val > border && val <= border + 8) {
                    int nw = val - border;
                    nw = (nw < width) ? nw : nw + 1;
                    if (nw < 1 || nw > 9) { ok = false; error = "w2"; return out; }
                    width = nw;
                    --i;
                    continue;
                }
            } else {
                if (val & 0x100) {
                    int nw = (val + 1) & 0xFF;
                    if (nw < 1 || nw > 9) { ok = false; error = "w3"; return out; }
                    width = nw;
                    --i;
                    continue;
                }
            }
            int v = (width < 8) ? signextend(val, width) : static_cast<signed char>(val);
            d1 = static_cast<signed char>(d1 + v);
            d2 = static_cast<signed char>(d2 + d1);
            if (produced + i < retain)
                out[static_cast<size_t>(produced + i)] = it215 ? d2 : d1;
        }
        produced += blocklen;
    }
    return out;
}

std::vector<int> ref_decompress16(const Bytes &stream, long disk, long retain,
                                  bool it215, bool &ok, std::string &error) {
    std::vector<int> out(static_cast<size_t>(retain), 0);
    size_t p = 0;
    long produced = 0;
    ok = true;
    while (produced < disk) {
        if (p + 2 > stream.size()) { ok = false; error = "block prefix"; return out; }
        int size = stream[p] | (stream[p + 1] << 8);
        p += 2;
        if (size <= 0) { ok = false; error = "zero block"; return out; }
        if (p + static_cast<size_t>(size) > stream.size()) {
            ok = false; error = "truncated block"; return out;
        }
        Bytes raw(stream.begin() + p, stream.begin() + p + size);
        p += size;
        RefBitReader r;
        bytes_to_bits(raw, r);
        long blocklen = disk - produced;
        if (blocklen > 0x4000) blocklen = 0x4000;
        int width = 17;
        signed short d1 = 0, d2 = 0;
        for (long i = 0; i < blocklen; ++i) {
            int val = r.read(width);
            if (!r.ok) { ok = false; error = "bits"; return out; }
            if (width == 17) {
                /* IT215/2.14 at width 17 use Method 3 whenever bit 16 is
                 * set; there is no plain 17-bit value above 0xFFFF. */
                if (val & 0x10000) {
                    int nw = (val + 1) & 0xFF;
                    if (width == 17 && (nw < 1 || nw > 17)) {
                        ok = false;
                        error = "w3";
                        return out;
                    }
                    if (nw < 1 || nw > 17) { ok = false; error = "w3"; return out; }
                    width = nw;
                    --i;
                    continue;
                }
            } else if (width < 7) {
                if (val == (1 << (width - 1))) {
                    int operand = r.read(4);
                    if (!r.ok) { ok = false; error = "ctrl1"; return out; }
                    int nw = operand + 1;
                    nw = (nw < width) ? nw : nw + 1;
                    if (nw < 1 || nw > 17) { ok = false; error = "w1"; return out; }
                    width = nw;
                    --i;
                    continue;
                }
            } else { /* 7 <= width < 17: Method 2 */
                int border = (0xFFFF >> (17 - width)) - 8;
                if (val > border && val <= border + 16) {
                    int nw = val - border;
                    nw = (nw < width) ? nw : nw + 1;
                    if (nw < 1 || nw > 17) { ok = false; error = "w2"; return out; }
                    width = nw;
                    --i;
                    continue;
                }
            }
            int v = (width < 16) ? signextend(val, width) : static_cast<short>(val);
            d1 = static_cast<short>(d1 + v);
            d2 = static_cast<short>(d2 + d1);
            if (produced + i < retain)
                out[static_cast<size_t>(produced + i)] = it215 ? d2 : d1;
        }
        produced += blocklen;
    }
    return out;
}

/* Compare the loader's PCM against a reference simulation. Returns "" on
 * success, otherwise a description. */
std::string check_pcm8(IT_SAMPLE *s, const std::vector<int> &want,
                       const char *which) {
    if (!s || !s->data) return std::string(which) + ": no PCM";
    int channels = channels_of(s);
    for (size_t i = 0; i < want.size(); ++i)
        for (int ch = 0; ch < channels; ++ch) {
            int got;
            if (s->flags & IT_SAMPLE_16BIT)
                got = static_cast<short *>(s->data)[i * channels + ch];
            else
                got = static_cast<signed char *>(s->data)[i * channels + ch];
            if (got != want[i])
                return std::string(which) + ": PCM mismatch at frame " +
                       std::to_string(i) + " ch " + std::to_string(ch) +
                       " got " + std::to_string(got) + " want " +
                       std::to_string(want[i]);
        }
    return "";
}

void expect_load(const Module &m, bool should_load, const char *what) {
    Loaded l(m.bytes);
    contract_expect((l.duh != nullptr) == should_load, what);
}

/* ============================================================= PCM cases */

/* Build a plain (uncompressed) sample in planar layout and exercise the
 * loop/sustain trimming rule. */
Bytes planar_data(const SampleSpec &s, const std::vector<int> &left,
                  const std::vector<int> &right) {
    Bytes out;
    auto emit = [&](int v) {
        if (s.sixteen) {
            out.push_back(static_cast<unsigned char>(v & 0xff));
            out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
        } else {
            out.push_back(static_cast<unsigned char>(v & 0xff));
        }
    };
    for (int v : left) emit(v);
    for (int v : right) emit(v);
    return out;
}

void test_pcm_uncompressed_trim() {
    /* 2 samples worth of frames marked 16-bit stereo; loop end and sustain
     * end exercise the "larger tail bound" rule. */
    SampleSpec s;
    s.sixteen = true;
    s.stereo = true;
    s.length = 40;
    s.loop = true; s.loop_start = 1; s.loop_end = 10;
    s.sus_loop = true; s.sus_start = 2; s.sus_end = 16;

    std::vector<int> left, right;
    for (int i = 0; i < 40; ++i) { left.push_back(i + 1); right.push_back(100 + i); }
    Bytes data = planar_data(s, left, right);
    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, data);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "uncompressed stereo module loaded");
    if (!sm) return;
    contract_expect(sm->length == 16, "retention reaches sustain loop end");
    for (int i = 0; i < 16; ++i) {
        contract_expect(static_cast<short *>(sm->data)[i * 2] == left[i],
                        "left channel PCM");
        contract_expect(static_cast<short *>(sm->data)[i * 2 + 1] == right[i],
                        "right channel located via disk frame count");
    }
}

void test_pcm_uncompressed_no_loop() {
    SampleSpec s;
    s.length = 12;
    std::vector<int> left, right;
    for (int i = 0; i < 12; ++i) { left.push_back(-(i + 1)); right.push_back(50 + i); }
    Bytes data = planar_data(s, left, right);
    /* mono-less: no stereo; test sign conversion via convert bit 0 clear */
    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, data);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "uncompressed mono loaded");
    if (!sm) return;
    contract_expect(sm->length == 12, "no loop keeps full length");
    for (int i = 0; i < 12; ++i)
        contract_expect(static_cast<signed char *>(sm->data)[i] ==
                            static_cast<signed char>(left[i]),
                        "mono PCM sign conversion");
}

std::vector<BitOp> uniform_ops(int count, int delta) {
    std::vector<BitOp> ops;
    ops.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) ops.push_back(BitOp{delta, 9, false});
    return ops;
}

/* A channel that holds a constant value: one delta equal to `value`, then
 * zeros so the running integral stays put. */
std::vector<BitOp> constant_ops(int count, int value) {
    std::vector<BitOp> ops;
    ops.reserve(static_cast<size_t>(count));
    ops.push_back(BitOp{value, 9, false});
    for (int i = 1; i < count; ++i) ops.push_back(BitOp{0, 9, false});
    return ops;
}

void test_pcm_compressed_loop_crossing() {
    /* Reproduce the witness: 8-bit stereo packed, 32769 frames, loop end 8.
     * The left channel spans two blocks; the second block's first frames must
     * not be mistaken for the right channel. */
    SampleSpec s;
    s.sixteen = false;
    s.stereo = true;
    s.packed = true;
    s.length = 32769;
    s.loop = true; s.loop_start = 0; s.loop_end = 8;

    Bytes stream;
    /* Channel 0: first block starts at value 1; the second block starts
     * fresh at 9-bit delta 7. Channel 1: both blocks emit 23. */
    append_block(stream, constant_ops(32768, 1));
    append_block(stream, constant_ops(1, 7));
    append_block(stream, constant_ops(32768, 23));
    append_block(stream, constant_ops(1, 23));

    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, stream);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "stereo packed loop module loaded");
    if (!sm) return;
    contract_expect(sm->length == 8, "damaged tail dropped at loop end");
    for (int i = 0; i < 8; ++i) {
        contract_expect(static_cast<signed char *>(sm->data)[i * 2] == 1,
                        "left channel constant");
        contract_expect(static_cast<signed char *>(sm->data)[i * 2 + 1] == 23,
                        "right channel begins at disk frame 32769");
    }
}

void test_pcm_sustain_farther() {
    /* Sustain loop reaches beyond the normal loop; retention must reach the
     * larger end, and all decoded values must match the reference. */
    SampleSpec s;
    s.sixteen = false;
    s.packed = true;
    s.it215 = true;
    s.convert = 5;  /* signed + IT215 second-order integration */
    s.length = 20;
    s.loop = true; s.loop_start = 1; s.loop_end = 6;
    s.sus_loop = true; s.sus_start = 2; s.sus_end = 14;

    Bytes stream;
    std::vector<BitOp> ops;
    for (int i = 0; i < 20; ++i) ops.push_back(BitOp{2, 9, false});
    append_block(stream, ops);

    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, stream);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "sustain-farther module loaded");
    if (!sm) return;
    contract_expect(sm->length == 14, "retention uses larger tail bound");

    bool ok;
    std::string err;
    std::vector<int> want = ref_decompress8(stream, 20, 14, true, ok, err);
    contract_expect(ok, "reference decoder accepted stream");
    std::string msg = check_pcm8(sm, want, "sustain-farther");
    contract_expect(msg.empty(), msg.c_str());
    /* IT215: deltas 2, integral 2, double integral 2,4,6,... */
    contract_expect(static_cast<signed char *>(sm->data)[0] == 2,
                    "IT215 first value");
    contract_expect(static_cast<signed char *>(sm->data)[1] == 6,
                    "IT215 second value (delta 2, d1=4, d2=6)");
}

void test_pcm_16bit_stereo_packed() {
    SampleSpec s;
    s.sixteen = true;
    s.stereo = true;
    s.packed = true;
    s.length = 6;
    s.loop = true; s.loop_start = 0; s.loop_end = 6;

    Bytes left, right;
    /* Constant 16-bit values: one delta to establish the value, then zero
     * deltas so the integration history keeps it. */
    std::vector<BitOp> lo, ro;
    int lv = 1234, rv = 2345;  /* bit 16 must stay clear at width 17 */
    lo.push_back(BitOp{lv & 0x1ffff, 17, false});
    ro.push_back(BitOp{rv & 0x1ffff, 17, false});
    for (int i = 1; i < 6; ++i) {
        lo.push_back(BitOp{0, 17, false});
        ro.push_back(BitOp{0, 17, false});
    }
    append_block(left, lo);
    append_block(right, ro);
    left.insert(left.end(), right.begin(), right.end());

    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, left);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "16-bit stereo packed loaded");
    if (!sm) return;
    bool ok;
    std::string err;
    std::vector<int> wantL = ref_decompress16(left, 6, 6, false, ok, err);
    contract_expect(ok, "ref left ok");
    for (int i = 0; i < 6; ++i) {
        contract_expect(static_cast<short *>(sm->data)[i * 2] == lv,
                        "16-bit left PCM constant");
        contract_expect(static_cast<short *>(sm->data)[i * 2 + 1] == rv,
                        "16-bit right PCM constant");
    }
}

void test_pcm_width_switching() {
    /* 8-bit mono spanning two compressed blocks (8-bit blocks hold at most
     * 32768 frames). Block 1 switches width via all three control-code
     * methods; block 2 restarts at width 9 with fresh history. */
    SampleSpec s;
    s.packed = true;
    s.length = 32770;
    s.flags = sample_flags(s);

    Bytes stream;
    std::vector<BitOp> ops;
    /* Four deltas at width 9. 252 keeps bit 8 clear (bit 8 would be a
     * Method 3 control marker). */
    ops.push_back(BitOp{1, 9, false});
    ops.push_back(BitOp{2, 9, false});
    ops.push_back(BitOp{3, 9, false});
    ops.push_back(BitOp{252, 9, false});
    /* Method 3: 9 -> 6. */
    ops.push_back(ctrl_m3(6));
    ops.push_back(BitOp{3, 6, false});
    /* Method 1 (widths below 7): 6 -> 8. Operand 6 gives new=7, +1 = 8. */
    ops.push_back(BitOp{1 << (6 - 1), 6, true});
    ops.push_back(BitOp{6, 3, true});
    ops.push_back(BitOp{5, 8, false});
    /* Method 2 (widths 7 and 8): 8 -> 4. Border for width 8 is 123. */
    ops.push_back(BitOp{123 + 4, 8, true});
    /* Method 1: 4 -> 2. Operand 1 gives new=2, below the old width. */
    ops.push_back(BitOp{1 << (4 - 1), 4, true});
    ops.push_back(BitOp{1, 3, true});
    /* Eight deltas so far; fill block 1 to 32768 frames at width 2. */
    long plain = 8;
    ops.push_back(BitOp{1, 2, false});
    ops.push_back(BitOp{0, 2, false});
    plain += 2;
    while (plain < 32768) {
        ops.push_back(BitOp{0, 2, false});
        ++plain;
    }
    append_block(stream, ops);

    /* Second block: two frames, width 9 and integration history reset. */
    append_block(stream, {BitOp{1, 9, false}, BitOp{0, 9, false}});

    Module m = make_module(s, 0x200, stream);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "width-switching module loaded");
    if (!sm) return;
    contract_expect(sm->length == 32770, "width-switching length");
    bool ok;
    std::string err;
    std::vector<int> want = ref_decompress8(stream, 32770, 32770, false, ok, err);
    contract_expect(ok, "reference decoder accepted width changes");
    std::string msg = check_pcm8(sm, want, "width-switching");
    contract_expect(msg.empty(), msg.c_str());
    /* The first frame of block 2 restarts both width and history. */
    contract_expect(static_cast<signed char *>(sm->data)[32768] == 1,
                    "block restart resets width and integration history");
    contract_expect(static_cast<signed char *>(sm->data)[32769] == 1,
                    "second frame of restarted block");
}

void test_pcm_integration_wraparound() {
    /* The running integrators are 8-bit signed and wrap on overflow; this
     * exercises both IT214 (first-order) and IT215 (second-order) wrap. */
    SampleSpec s;
    s.packed = true;
    s.length = 8;
    s.convert = 5; /* signed + IT215 */
    s.flags = sample_flags(s);

    Bytes stream;
    std::vector<BitOp> ops;
    int deltas[] = {120, 20, 20, -40, -40, -40, 100, -100};
    for (int d : deltas)
        ops.push_back(BitOp{d & 0xff, 9, false}); /* keep bit 8 clear */
    append_block(stream, ops);
    Module m = make_module(s, 0x200, stream);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "wraparound module loaded");
    if (!sm) return;
    bool ok;
    std::string err;
    std::vector<int> want = ref_decompress8(stream, 8, 8, true, ok, err);
    contract_expect(ok, "reference decoder accepted wraparound stream");
    std::string msg = check_pcm8(sm, want, "wraparound");
    contract_expect(msg.empty(), msg.c_str());
    /* IT215 with delta 120: d1=120, d2=120; then +20 -> d1=-116,d2=4. */
    contract_expect(static_cast<signed char *>(sm->data)[0] == 120,
                    "first integrated value");
    contract_expect(static_cast<signed char *>(sm->data)[1] == 4,
                    "wrapped second-order value");
}

void test_pcm_signed_conversion() {
    /* convert bit 0 clear means the stored values are unsigned and the loader
     * flips the sign bit. */
    SampleSpec s;
    s.packed = true;
    s.length = 4;
    s.convert = 0; /* unsigned, IT214 */
    Bytes stream;
    std::vector<BitOp> ops;
    int vals[] = {0x00, 0x01, 0xFF, 0x80};
    for (int v : vals) ops.push_back(BitOp{v, 9, false});
    append_block(stream, ops);
    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, stream);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "unsigned-convert module loaded");
    if (!sm) return;
    bool ok;
    std::string err;
    std::vector<int> raw = ref_decompress8(stream, 4, 4, false, ok, err);
    contract_expect(ok, "ref unsigned ok");
    for (int i = 0; i < 4; ++i) {
        int want = static_cast<signed char>(
            static_cast<unsigned char>(static_cast<signed char>(raw[i])) ^
            0x80);
        contract_expect(static_cast<signed char *>(sm->data)[i] == want,
                        "unsigned-to-signed conversion");
    }
}

/* ========================================================= framing cases */

void test_framing_two_samples_order() {
    /* Two samples whose table order differs from physical order; the first
     * declared sample's data lies after the second's. Ranges must be derived
     * from file positions, not table order. */
    SampleSpec a;
    a.packed = true; a.length = 4;
    SampleSpec sb = a;
    a.flags = sample_flags(a);
    sb.flags = sample_flags(sb);

    Bytes streamA, streamB;
    std::vector<BitOp> opsA, opsB;
    for (int i = 0; i < 4; ++i) opsA.push_back(BitOp{1, 9, false});
    for (int i = 0; i < 4; ++i) opsB.push_back(BitOp{2, 9, false});
    append_block(streamA, opsA);
    append_block(streamB, opsB);

    /* Layout: sample header area at 0xc0..0x110; two sample headers at 0x110
     * and 0x160; data for sample #2 first at 0x200, sample #1 at 0x300. */
    size_t total = 0x400;
    Bytes b(total, 0);
    put_id(b, 0, "IMPM");
    b[32] = 1; b[36] = 2; b[38] = 0;
    put_u16(b, 40, 0x0215); put_u16(b, 42, 0x0214); put_u16(b, 44, 1);
    b[48] = 128; b[49] = 48; b[50] = 6; b[51] = 125; b[52] = 128;
    for (int i = 0; i < 64; ++i) { b[64 + i] = 32; b[128 + i] = 64; }
    b[192] = 0;
    put_u32(b, 193, 0x110);  /* sample table: entry 0 */
    put_u32(b, 197, 0x160);  /* entry 1 */

    auto write_header = [&](size_t at, const SampleSpec &s, size_t data) {
        put_id(b, at, "IMPS");
        b[at + 17] = 64; b[at + 18] = static_cast<unsigned char>(s.flags);
        b[at + 19] = 64; b[at + 46] = s.convert;
        put_u32(b, at + 48, static_cast<uint32_t>(s.length));
        put_u32(b, at + 60, 8363);
        put_u32(b, at + 72, static_cast<uint32_t>(data));
    };
    write_header(0x110, a, 0x300);
    write_header(0x160, sb, 0x200);
    std::memcpy(b.data() + 0x200, streamB.data(), streamB.size());
    std::memcpy(b.data() + 0x300, streamA.data(), streamA.size());

    Loaded l(b);
    contract_expect(l.duh != nullptr, "two-sample module loads");
    if (!l.duh) return;
    DUMB_IT_SIGDATA *p = duh_get_it_sigdata(l.duh);
    contract_expect(p && p->n_samples == 2, "two samples present");
    if (!p) return;
    /* Deltas accumulate within a block: sample 0 sees 1,2,3,4 and sample 1
     * sees 2,4,6,8. */
    for (int i = 0; i < 4; ++i) {
        contract_expect(static_cast<signed char *>(p->sample[0].data)[i] ==
                            i + 1,
                        "sample 0 data from its own range");
        contract_expect(static_cast<signed char *>(p->sample[1].data)[i] ==
                            2 * (i + 1),
                        "sample 1 data from its own range");
    }
}

void test_framing_shared_start() {
    /* Two samples share one data start. Both must decode the same bytes, and
     * neither range may collapse to zero length. */
    Bytes stream;
    std::vector<BitOp> ops;
    for (int i = 0; i < 4; ++i) ops.push_back(BitOp{3, 9, false});
    append_block(stream, ops);

    size_t total = 0x300 + stream.size();
    Bytes b(total, 0);
    put_id(b, 0, "IMPM");
    b[32] = 1; b[36] = 2; b[38] = 0;
    put_u16(b, 40, 0x0215); put_u16(b, 42, 0x0214); put_u16(b, 44, 1);
    b[48] = 128; b[49] = 48; b[50] = 6; b[51] = 125; b[52] = 128;
    for (int i = 0; i < 64; ++i) { b[64 + i] = 32; b[128 + i] = 64; }
    put_u32(b, 193, 0x110); put_u32(b, 197, 0x160);
    auto header = [&](size_t at, size_t data) {
        put_id(b, at, "IMPS");
        b[at + 17] = 64; b[at + 18] = 1 | 8; b[at + 19] = 64;
        b[at + 46] = 1; /* signed */
        put_u32(b, at + 48, 4);
        put_u32(b, at + 60, 8363);
        put_u32(b, at + 72, static_cast<uint32_t>(data));
    };
    header(0x110, 0x300);
    header(0x160, 0x300);
    std::memcpy(b.data() + 0x300, stream.data(), stream.size());

    Loaded l(b);
    contract_expect(l.duh != nullptr, "shared-start module loads");
    if (!l.duh) return;
    DUMB_IT_SIGDATA *p = duh_get_it_sigdata(l.duh);
    contract_expect(p && p->n_samples == 2, "both shared samples present");
    if (!p) return;
    for (int n = 0; n < 2; ++n) {
        contract_expect(p->sample[n].length == 4, "shared range non-empty");
        for (int i = 0; i < 4; ++i)
            contract_expect(static_cast<signed char *>(p->sample[n].data)[i] ==
                                3 * (i + 1),
                            "shared-start PCM decoded per sample");
    }
}

/* ========================================================== errors cases */

void test_errors_truncated_before_retain() {
    SampleSpec s;
    s.packed = true;
    s.length = 8;
    Bytes stream;
    std::vector<BitOp> ops;
    for (int i = 0; i < 8; ++i) ops.push_back(BitOp{1, 9, false});
    append_block(stream, ops);
    /* Retain the prefix with a block header claiming more bits than exist. */
    stream.resize(3);
    put_u16(stream, 0, 1); /* body claims 1 byte, only one byte remains */
    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, stream);
    expect_load(m, false, "exhausted block rejects the module");
}

void test_errors_zero_length_block() {
    SampleSpec s;
    s.packed = true;
    s.length = 4;
    Bytes stream = {0, 0};
    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, stream);
    expect_load(m, false, "zero-length block rejects the module");
}

void test_errors_corrupt_tail_after_retain() {
    /* Corruption lies past the retained tail; the module must still fail. */
    SampleSpec s;
    s.packed = true;
    s.length = 32768 + 8;
    s.loop = true; s.loop_start = 0; s.loop_end = 4;

    Bytes stream;
    std::vector<BitOp> first, second;
    for (int i = 0; i < 32768; ++i) first.push_back(BitOp{1, 9, false});
    append_block(stream, first);
    /* Second block declares a body but provides an illegal width code that
     * aborts decoding; the retained prefix was already produced. */
    size_t at = stream.size();
    stream.resize(at + 2);
    put_u16(stream, at, 1);
    stream.push_back(0xFF); /* 0x1FF control claims width 0 via (val+1)&0xFF */
    (void)second;
    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, stream);
    expect_load(m, false, "corruption in dropped tail rejects the module");
}

void test_errors_cross_source_boundary() {
    /* A compressed block claims more body bytes than the space up to the
     * sample's source-range boundary. Another sample's data start sits right
     * after that boundary, so the block must fail rather than borrowing the
     * neighbouring bytes. */
    SampleSpec s;
    s.packed = true;
    s.length = 4;
    s.flags = sample_flags(s);

    std::vector<BitOp> ops;
    for (int i = 0; i < 4; ++i) ops.push_back(BitOp{1, 9, false});
    Bytes good;
    append_block(good, ops);

    Bytes b(0x400, 0);
    put_id(b, 0, "IMPM");
    b[32] = 1; b[36] = 2;
    put_u16(b, 40, 0x0215); put_u16(b, 42, 0x0214); put_u16(b, 44, 1);
    b[48] = 128; b[49] = 48; b[50] = 6; b[51] = 125; b[52] = 128;
    for (int i = 0; i < 64; ++i) { b[64 + i] = 32; b[128 + i] = 64; }
    put_u32(b, 193, 0x110); put_u32(b, 197, 0x160);

    auto header = [&](size_t at, size_t data) {
        put_id(b, at, "IMPS");
        b[at + 17] = 64; b[at + 18] = static_cast<unsigned char>(s.flags);
        b[at + 19] = 64; b[at + 46] = 1;
        put_u32(b, at + 48, 4);
        put_u32(b, at + 60, 8363);
        put_u32(b, at + 72, static_cast<uint32_t>(data));
    };
    header(0x110, 0x200); /* sample 0 data starts at 0x200 */
    header(0x160, 0x205); /* sample 1 data starts at 0x205 */
    /* Sample 0's block prefix says its body is 0x6000 bytes, far beyond both
     * the file and sample 1's data start. */
    put_u16(b, 0x200, 0x6000);
    b[0x202] = 0x00;
    b[0x203] = 0x00;
    /* Sample 1's own data is a valid stream. */
    std::memcpy(b.data() + 0x205, good.data(), good.size());

    expect_load(Module{b}, false,
                "block overrunning the source range is rejected");
}

void test_errors_recovery() {
    /* After a rejection, a valid module still loads. */
    SampleSpec bad;
    bad.packed = true;
    bad.length = 8;
    Bytes bad_stream = {0, 0};
    bad.flags = sample_flags(bad);
    Module mb = make_module(bad, 0x200, bad_stream);
    expect_load(mb, false, "bad module rejected");

    SampleSpec good;
    good.packed = true;
    good.length = 4;
    Bytes stream;
    std::vector<BitOp> ops;
    for (int i = 0; i < 4; ++i) ops.push_back(BitOp{5, 9, false});
    append_block(stream, ops);
    good.flags = sample_flags(good);
    Module mg = make_module(good, 0x200, stream);
    Loaded l(mg.bytes);
    contract_expect(l.duh != nullptr, "valid module loads after failure");
}

void test_errors_memory_backend_bounds() {
    /* The memory backend may seek to the end but not past it, and a failed
     * seek leaves no readable out-of-bounds pointer. */
    const char payload[] = "abcdef";
    DUMBFILE *f = dumbfile_open_memory(payload, 6);
    contract_expect(f != nullptr, "memory file opens");

    contract_expect(dumbfile_seek(f, 6, DFS_SEEK_SET) == 0,
                    "seek to end is allowed");
    contract_expect(dumbfile_getc(f) < 0, "read at end fails");

    contract_expect(dumbfile_seek(f, -1, DFS_SEEK_SET) != 0,
                    "negative absolute seek fails");
    contract_expect(dumbfile_error(f), "failed seek sets error state");
    contract_expect(dumbfile_getc(f) < 0, "read after failed seek fails");

    contract_expect(dumbfile_seek(f, 7, DFS_SEEK_SET) != 0,
                    "seek past end fails");
    contract_expect(dumbfile_getc(f) < 0, "read past end fails");

    contract_expect(dumbfile_seek(f, 2, DFS_SEEK_SET) == 0,
                    "valid absolute seek recovers");
    contract_expect(!dumbfile_error(f), "error state cleared");
    contract_expect(dumbfile_getc(f) == 'c', "recovers to correct byte");

    contract_expect(dumbfile_seek(f, 1, DFS_SEEK_SET) == 0, "position to 1");
    contract_expect(
        dumbfile_seek(f, (dumb_off_t)INT64_MAX, DFS_SEEK_CUR) != 0,
        "relative signed overflow rejected");
    contract_expect(dumbfile_error(f), "overflow enters error state");
    contract_expect(dumbfile_getc(f) < 0, "no read after overflow");
    contract_expect(dumbfile_seek(f, 0, DFS_SEEK_SET) == 0,
                    "absolute seek recovers after overflow");
    dumbfile_close(f);
}

void test_errors_offset_out_of_range() {
    /* A component offset beyond end of file is rejected before any seek. */
    SampleSpec s;
    s.packed = true;
    s.length = 4;
    s.flags = sample_flags(s);
    Bytes stream;
    std::vector<BitOp> ops;
    for (int i = 0; i < 4; ++i) ops.push_back(BitOp{1, 9, false});
    append_block(stream, ops);
    Module m = make_module(s, 0x200, stream);
    put_u32(m.bytes, 193, 0x7fffffff); /* sample header offset, far past EOF */
    expect_load(m, false, "out-of-range component offset rejected");

    /* A negative (sign-extended) offset is rejected too. */
    Module m2 = make_module(s, 0x200, stream);
    put_u32(m2.bytes, 193, 0xffffffff);
    expect_load(m2, false, "negative component offset rejected");
}

void test_pcm_uncompressed_bigendian_stereo() {
    /* 16-bit stereo planar, big-endian (convert bit 1), with a loop so the
     * right channel must be located by the full on-disk frame count. */
    SampleSpec s;
    s.sixteen = true;
    s.stereo = true;
    s.length = 10;
    s.loop = true; s.loop_start = 0; s.loop_end = 3;
    s.convert = 1 | 2;
    s.flags = sample_flags(s);

    Bytes data;
    auto emit = [&](int v) {
        data.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
        data.push_back(static_cast<unsigned char>(v & 0xff));
    };
    for (int i = 0; i < 10; ++i) emit(1000 + i);   /* left */
    for (int i = 0; i < 10; ++i) emit(-2000 - i);  /* right */
    Module m = make_module(s, 0x200, data);
    Loaded l(m.bytes);
    IT_SAMPLE *sm = l.sample();
    contract_expect(sm != nullptr, "big-endian stereo module loaded");
    if (!sm) return;
    contract_expect(sm->length == 3, "big-endian retention at loop end");
    for (int i = 0; i < 3; ++i) {
        contract_expect(static_cast<short *>(sm->data)[i * 2] == 1000 + i,
                        "big-endian left channel");
        contract_expect(static_cast<short *>(sm->data)[i * 2 + 1] == -2000 - i,
                        "big-endian right channel after full left channel");
    }
}

/* ======================================================= resources cases */

size_t g_alloc_total = 0;
size_t g_alloc_max = 0;
void alloc_observer(size_t n) {
    g_alloc_total += n;
    if (n > g_alloc_max) g_alloc_max = n;
}

void test_resources_small_retention() {
    /* A huge declared sample with a tiny loop end: allocation must be bounded
     * by the retained PCM plus one compressed block. */
    SampleSpec s;
    s.packed = true;
    s.length = 2000000;
    s.loop = true; s.loop_start = 0; s.loop_end = 4;

    Bytes stream;
    /* First block: 32768 frames. Second: rest of the 2M frames. */
    std::vector<BitOp> ops;
    ops.reserve(32768);
    for (int i = 0; i < 32768; ++i) ops.push_back(BitOp{1, 9, false});
    append_block(stream, ops);
    long remaining = 2000000 - 32768;
    while (remaining > 0) {
        long n = remaining > 32768 ? 32768 : remaining;
        std::vector<BitOp> o;
        o.reserve(static_cast<size_t>(n));
        for (long i = 0; i < n; ++i) o.push_back(BitOp{1, 9, false});
        append_block(stream, o);
        remaining -= n;
    }

    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, stream);
    g_alloc_total = 0;
    g_alloc_max = 0;
    _dumb_it_sample_alloc_hook = alloc_observer;
    Loaded l(m.bytes);
    _dumb_it_sample_alloc_hook = nullptr;

    contract_expect(l.duh != nullptr, "large declared sample loads");
    contract_expect(g_alloc_max <= 65535, "no allocation near full PCM size");
    /* The retained PCM is 4 bytes; the block buffer must not exceed 65535. */
    contract_expect(g_alloc_total < 4 * 1024 * 1024,
                    "total sample allocation bounded");
    if (l.sample())
        contract_expect(l.sample()->length == 4, "retained length is small");
}

void test_resources_uncompressed_trim() {
    /* Uncompressed stereo with a tiny loop must allocate only the retained
     * head, even though the file holds the whole left and right channels. */
    SampleSpec s;
    s.sixteen = false;
    s.stereo = true;
    s.length = 100000;
    s.loop = true; s.loop_start = 0; s.loop_end = 3;
    std::vector<int> left(100000, 1), right(100000, 23);
    Bytes data = planar_data(s, left, right);
    s.flags = sample_flags(s);
    Module m = make_module(s, 0x200, data);

    g_alloc_total = 0;
    g_alloc_max = 0;
    _dumb_it_sample_alloc_hook = alloc_observer;
    Loaded l(m.bytes);
    _dumb_it_sample_alloc_hook = nullptr;
    contract_expect(l.duh != nullptr, "uncompressed trimmed module loads");
    contract_expect(g_alloc_max <= 64, "uncompressed allocation is retained size");
}

} // namespace

void register_contract() {
    contract_add("pcm", "uncompressed_trim", test_pcm_uncompressed_trim);
    contract_add("pcm", "uncompressed_no_loop", test_pcm_uncompressed_no_loop);
    contract_add("pcm", "compressed_loop_crossing",
                 test_pcm_compressed_loop_crossing);
    contract_add("pcm", "sustain_farther", test_pcm_sustain_farther);
    contract_add("pcm", "sixteen_bit_stereo", test_pcm_16bit_stereo_packed);
    contract_add("pcm", "width_switching", test_pcm_width_switching);
    contract_add("pcm", "signed_conversion", test_pcm_signed_conversion);
    contract_add("pcm", "integration_wraparound",
                 test_pcm_integration_wraparound);
    contract_add("pcm", "uncompressed_bigendian_stereo",
                 test_pcm_uncompressed_bigendian_stereo);
    contract_add("framing", "two_samples_order", test_framing_two_samples_order);
    contract_add("framing", "shared_start", test_framing_shared_start);
    contract_add("errors", "truncated_before_retain",
                 test_errors_truncated_before_retain);
    contract_add("errors", "zero_length_block", test_errors_zero_length_block);
    contract_add("errors", "corrupt_tail", test_errors_corrupt_tail_after_retain);
    contract_add("errors", "cross_source_boundary",
                 test_errors_cross_source_boundary);
    contract_add("errors", "recovery", test_errors_recovery);
    contract_add("errors", "memory_backend_bounds",
                 test_errors_memory_backend_bounds);
    contract_add("errors", "offset_out_of_range",
                 test_errors_offset_out_of_range);
    contract_add("resources", "small_retention", test_resources_small_retention);
    contract_add("resources", "uncompressed_trim",
                 test_resources_uncompressed_trim);
}
