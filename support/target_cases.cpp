// Tests for the batched fragmentation mode (MP4E_open(..., enable_fragmentation == 2, ...)).
// The tests parse the produced boxes themselves; MP4D_open() does not understand
// fragments and is deliberately not used here.

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>

// ---------------------------------------------------------------------------
// Write callback with optional short-write / failure injection.
// ---------------------------------------------------------------------------
struct Writer
{
    std::vector<unsigned char> bytes;
    size_t fail_after;      // max total bytes that may reach the target
    bool partial;           // write prefix before reporting the short write
    bool failed;
    size_t bytes_at_failure;
    int calls;

    Writer() : fail_after((size_t)-1), partial(false), failed(false), bytes_at_failure(0), calls(0) {}
};

static int write_cb(int64_t offset, const void *buffer, size_t size, void *token)
{
    Writer &w = *(Writer *)token;
    w.calls++;
    if (w.failed)
        return 1;
    if (offset < 0)
    {
        w.failed = true;
        w.bytes_at_failure = w.bytes.size();
        return 1;
    }
    size_t start = (size_t)offset;
    if (w.fail_after != (size_t)-1 && start + size > w.fail_after)
    {
        size_t allowed = start < w.fail_after ? w.fail_after - start : 0;
        if (allowed > size)
            allowed = size;
        if (w.partial && allowed)
        {
            if (w.bytes.size() < start + allowed)
                w.bytes.resize(start + allowed);
            memcpy(w.bytes.data() + start, buffer, allowed);
        }
        w.failed = true;
        w.bytes_at_failure = w.bytes.size();
        return 1;
    }
    if (w.bytes.size() < start + size)
        w.bytes.resize(start + size);
    if (size)
        memcpy(w.bytes.data() + start, buffer, size);
    return 0;
}

// ---------------------------------------------------------------------------
// Minimal independent MP4 box parser (only what the tests need).
// ---------------------------------------------------------------------------
static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const unsigned char *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

struct Box
{
    uint32_t type;
    size_t off;
    size_t size;
};

static size_t box_header(const unsigned char *b, size_t off)
{
    return be32(b + off) == 1 ? 16 : 8;
}

static std::vector<Box> parse_boxes(const unsigned char *b, size_t begin, size_t end)
{
    std::vector<Box> out;
    size_t pos = begin;
    while (pos < end)
    {
        FB_CHECK(end - pos >= 8);
        uint32_t declared = be32(b + pos);
        uint32_t type = be32(b + pos + 4);
        size_t total;
        if (declared == 0)
            total = end - pos;
        else if (declared == 1)
        {
            FB_CHECK(end - pos >= 16);
            total = (size_t)be64(b + pos + 8);
        }
        else
            total = declared;
        FB_CHECK(total >= 8 && total <= end - pos);
        Box box = { type, pos, total };
        out.push_back(box);
        pos += total;
    }
    FB_CHECK(pos == end);
    return out;
}

static std::vector<Box> children_of(const unsigned char *b, const Box &parent)
{
    return parse_boxes(b, parent.off + box_header(b, parent.off), parent.off + parent.size);
}

static int count_children(const unsigned char *b, const Box &parent, uint32_t type)
{
    std::vector<Box> kids = children_of(b, parent);
    int count = 0;
    for (size_t i = 0; i < kids.size(); ++i)
        count += kids[i].type == type;
    return count;
}

static size_t full_payload(const unsigned char *b, const Box &box)
{
    return box.off + box_header(b, box.off) + 4;
}

// ---------------------------------------------------------------------------
// Parsed fragment structures.
// ---------------------------------------------------------------------------
struct ParsedRun
{
    uint32_t count;
    uint32_t flags;
    uint32_t data_offset;
    std::vector<uint32_t> durations, sizes, flags_per_sample;
};

struct ParsedTraf
{
    uint32_t track_id;
    uint32_t tfhd_flags;
    uint32_t tfdt_version;
    uint64_t tfdt;
    std::vector<ParsedRun> runs;
};

struct ParsedFrag
{
    uint32_t seq;
    std::vector<ParsedTraf> trafs;
    Box moof, mdat;
};

static ParsedTraf parse_traf(const unsigned char *b, const Box &traf)
{
    ParsedTraf t;
    std::vector<Box> kids = children_of(b, traf);
    int tfhd_count = 0, tfdt_count = 0;
    t.track_id = 0;
    t.tfhd_flags = 0;
    t.tfdt_version = 0;
    t.tfdt = 0;
    for (size_t i = 0; i < kids.size(); ++i)
    {
        const Box &box = kids[i];
        if (box.type == BOX_tfhd)
        {
            size_t payload = full_payload(b, box);
            uint32_t vf = be32(b + payload - 4);
            t.tfhd_flags = vf & 0xffffff;
            t.track_id = be32(b + payload);
            tfhd_count++;
        } else if (box.type == BOX_tfdt)
        {
            size_t payload = full_payload(b, box);
            uint32_t vf = be32(b + payload - 4);
            t.tfdt_version = vf >> 24;
            FB_CHECK((vf & 0xffffff) == 0);
            t.tfdt = be64(b + payload);
            tfdt_count++;
        } else if (box.type == BOX_trun)
        {
            size_t payload = full_payload(b, box);
            uint32_t vf = be32(b + payload - 4);
            ParsedRun run;
            run.flags = vf & 0xffffff;
            run.count = be32(b + payload);
            size_t pos = payload + 4;
            run.data_offset = 0;
            if (run.flags & 0x001)
            {
                run.data_offset = be32(b + pos);
                pos += 4;
            } else
                FB_CHECK(0);
            if (run.flags & 0x004)
            {
                pos += 4; // first_sample_flags
            }
            FB_CHECK((run.flags & ~0x701u) == 0); // exactly the flags we emit
            for (uint32_t k = 0; k < run.count; ++k)
            {
                if (run.flags & 0x100)
                {
                    run.durations.push_back(be32(b + pos));
                    pos += 4;
                }
                if (run.flags & 0x200)
                {
                    run.sizes.push_back(be32(b + pos));
                    pos += 4;
                }
                if (run.flags & 0x400)
                {
                    run.flags_per_sample.push_back(be32(b + pos));
                    pos += 4;
                }
            }
            FB_CHECK(pos == box.off + box.size);
            t.runs.push_back(run);
        }
    }
    FB_CHECK(tfhd_count == 1 && tfdt_count == 1);
    return t;
}

// ---------------------------------------------------------------------------
// Expected fragment model and verification.
// ---------------------------------------------------------------------------
struct ExpectRun
{
    uint32_t first_payload_offset;
    std::vector<uint32_t> durations, sizes, flags;
};

struct ExpectTraf
{
    uint32_t track_id;
    uint64_t tfdt;
    std::vector<ExpectRun> runs;
};

struct ExpectFrag
{
    uint32_t seq;
    std::vector<ExpectTraf> trafs;
    std::vector<std::vector<unsigned char> > payload; // workspace, in mdat order
};

static void verify_frag(const Writer &w, const Box &moof, const Box &mdat, const ExpectFrag &e)
{
    const unsigned char *b = w.bytes.data();
    FB_CHECK(moof.type == BOX_moof && mdat.type == BOX_mdat);
    FB_CHECK(mdat.off == moof.off + moof.size); // exactly one mdat right after

    std::vector<Box> moof_kids = children_of(b, moof);
    FB_CHECK(moof_kids.size() >= 2);
    FB_CHECK(moof_kids[0].type == BOX_mfhd);
    Box mfhd = moof_kids[0];
    FB_CHECK(mfhd.size == 16);
    FB_CHECK(be32(b + full_payload(b, mfhd)) == e.seq);

    size_t mdat_payload = mdat.off + box_header(b, mdat.off);
    size_t payload_bytes = 0;
    for (size_t i = 0; i < e.payload.size(); ++i)
    {
        for (size_t k = 0; k < e.payload[i].size(); ++k)
            FB_CHECK(b[mdat_payload + payload_bytes + k] == e.payload[i][k]);
        payload_bytes += e.payload[i].size();
    }
    FB_CHECK(mdat.size == payload_bytes + 8);
    FB_CHECK(moof.size == mdat.off - moof.off);

    std::vector<Box> traf_boxes;
    for (size_t i = 1; i < moof_kids.size(); ++i)
    {
        FB_CHECK(moof_kids[i].type == BOX_traf);
        traf_boxes.push_back(moof_kids[i]);
    }
    FB_CHECK(traf_boxes.size() == e.trafs.size());

    uint32_t expected_offset_base = (uint32_t)(moof.size + 8);
    for (size_t i = 0; i < traf_boxes.size(); ++i)
    {
        ParsedTraf t = parse_traf(b, traf_boxes[i]);
        const ExpectTraf &et = e.trafs[i];
        FB_CHECK(t.track_id == et.track_id);
        FB_CHECK(t.tfhd_flags == 0x020000); // default-base-is-moof
        FB_CHECK(t.tfdt_version == 1);
        FB_CHECK(t.tfdt == et.tfdt);
        FB_CHECK(t.runs.size() == et.runs.size());
        for (size_t k = 0; k < t.runs.size(); ++k)
        {
            const ParsedRun &run = t.runs[k];
            const ExpectRun &er = et.runs[k];
            FB_CHECK(run.data_offset == expected_offset_base + er.first_payload_offset);
            FB_CHECK(run.count == er.sizes.size());
            FB_CHECK(run.sizes == er.sizes);
            FB_CHECK(run.durations == er.durations);
            FB_CHECK(run.flags_per_sample == er.flags);
            // Every sample of the run must lie inside the mdat payload.
            size_t run_bytes = 0;
            for (size_t j = 0; j < er.sizes.size(); ++j)
                run_bytes += er.sizes[j];
            FB_CHECK((size_t)run.data_offset + run_bytes <= moof.size + mdat.size);
        }
    }
}

static std::vector<Box> top_level(const Writer &w)
{
    return parse_boxes(w.bytes.data(), 0, w.bytes.size());
}

static void check_file_shape(const Writer &w, int fragments, int tracks)
{
    std::vector<Box> top = top_level(w);
    FB_CHECK(top.size() == (size_t)(2 + 2 * fragments));
    FB_CHECK(top[0].type == BOX_ftyp);
    FB_CHECK(top[1].type == BOX_moov);
    FB_CHECK(count_children(w.bytes.data(), top[1], BOX_trak) == tracks);
    for (int i = 0; i < fragments; ++i)
    {
        FB_CHECK(top[2 + 2 * i].type == BOX_moof);
        FB_CHECK(top[3 + 2 * i].type == BOX_mdat);
    }
}

static Box fragment_moof(const Writer &w, int index)
{
    return top_level(w)[2 + 2 * index];
}

static Box fragment_mdat(const Writer &w, int index)
{
    return top_level(w)[3 + 2 * index];
}

// ---------------------------------------------------------------------------
// Mux helpers.
// ---------------------------------------------------------------------------
struct TestMux
{
    Writer w;
    MP4E_mux_t *mux;

    explicit TestMux(size_t fail_after = (size_t)-1, int mode = 2)
    {
        w.fail_after = fail_after;
        mux = MP4E_open(0, mode, &w, write_cb);
        FB_CHECK(mux != 0);
    }

    int add_video(unsigned time_scale, unsigned default_duration, int width, int height, const unsigned char *sps = 0, int sps_bytes = 0, const unsigned char *pps = 0, int pps_bytes = 0)
    {
        MP4E_track_t tr;
        memset(&tr, 0, sizeof(tr));
        tr.object_type_indication = MP4_OBJECT_TYPE_AVC;
        tr.track_media_kind = e_video;
        tr.time_scale = time_scale;
        tr.default_duration = default_duration;
        tr.u.v.width = width;
        tr.u.v.height = height;
        memcpy(tr.language, "und", 4);
        int id = MP4E_add_track(mux, &tr);
        FB_CHECK(id >= 0);
        static const unsigned char default_sps[4] = { 0x67, 0x42, 0x00, 0x1e };
        static const unsigned char default_pps[2] = { 0x68, 0xce };
        if (!sps)
        {
            sps = default_sps;
            sps_bytes = 4;
        }
        if (!pps)
        {
            pps = default_pps;
            pps_bytes = 2;
        }
        FB_CHECK(MP4E_set_sps(mux, id, sps, sps_bytes) == 0);
        FB_CHECK(MP4E_set_pps(mux, id, pps, pps_bytes) == 0);
        return id;
    }

    int add_audio(unsigned time_scale, unsigned default_duration, unsigned channels = 2)
    {
        MP4E_track_t tr;
        memset(&tr, 0, sizeof(tr));
        tr.object_type_indication = MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
        tr.track_media_kind = e_audio;
        tr.time_scale = time_scale;
        tr.default_duration = default_duration;
        tr.u.a.channelcount = channels;
        memcpy(tr.language, "und", 4);
        int id = MP4E_add_track(mux, &tr);
        FB_CHECK(id >= 0);
        static const unsigned char dsi[2] = { 0x11, 0x90 };
        FB_CHECK(MP4E_set_dsi(mux, id, dsi, 2) == 0);
        return id;
    }

    int put(int track, const std::vector<unsigned char> &data, int duration, int kind = MP4E_SAMPLE_DEFAULT)
    {
        static const unsigned char dummy = 0;
        const void *p = data.empty() ? (const void *)&dummy : (const void *)data.data();
        return MP4E_put_sample(mux, track, p, (int)data.size(), duration, kind);
    }

    void put_ok(int track, const std::vector<unsigned char> &data, int duration, int kind = MP4E_SAMPLE_DEFAULT)
    {
        FB_CHECK(put(track, data, duration, kind) == 0);
    }

    int flush()
    {
        return MP4E_flush_fragment(mux);
    }

    void flush_ok()
    {
        FB_CHECK(flush() == 0);
    }

    int close()
    {
        int err = MP4E_close(mux);
        mux = 0;
        return err;
    }
};

static std::vector<unsigned char> bytes_of(const char *s)
{
    size_t n = strlen(s);
    return std::vector<unsigned char>((const unsigned char *)s, (const unsigned char *)s + n);
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

// Two tracks, several samples each, A/B/A submission order, one batch.
// mdat keeps the submission order; A gets two truns because B interrupts it.
FB_TEST("layout", two_tracks_interleaved_runs)
{
    TestMux m;
    int v = m.add_video(90000, 3000, 320, 240);
    int a = m.add_audio(48000, 1024);
    FB_CHECK(v == 0 && a == 1);

    std::vector<unsigned char> v1 = bytes_of("VID1");
    std::vector<unsigned char> v2 = bytes_of("VID2");
    std::vector<unsigned char> v3 = bytes_of("VID3");
    std::vector<unsigned char> a1 = bytes_of("AUD1");
    std::vector<unsigned char> a2 = bytes_of("AUD2");

    m.put_ok(v, v1, 3000, MP4E_SAMPLE_RANDOM_ACCESS); // key frame
    m.put_ok(v, v2, 3000);                            // non-key
    m.put_ok(a, a1, 1024);
    m.put_ok(v, v3, 0, MP4E_SAMPLE_RANDOM_ACCESS);    // duration 0 -> default 3000
    m.put_ok(a, a2, 1024);
    m.flush_ok();
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 1, 2);

    ExpectFrag e;
    e.seq = 1;
    ExpectTraf tv;
    tv.track_id = 1; // video track_num 0 -> ID 1
    tv.tfdt = 0;
    ExpectRun r1;
    r1.first_payload_offset = 0;
    r1.sizes.push_back((uint32_t)v1.size());
    r1.sizes.push_back((uint32_t)v2.size());
    r1.durations.push_back(3000);
    r1.durations.push_back(3000);
    r1.flags.push_back(0x02000000);
    r1.flags.push_back(0x01010000);
    ExpectRun r2;
    r2.first_payload_offset = (uint32_t)(v1.size() + v2.size() + a1.size());
    r2.sizes.push_back((uint32_t)v3.size());
    r2.durations.push_back(3000);
    r2.flags.push_back(0x02000000);
    tv.runs.push_back(r1);
    tv.runs.push_back(r2);
    e.trafs.push_back(tv);

    ExpectTraf ta;
    ta.track_id = 2;
    ta.tfdt = 0;
    ExpectRun ra1;
    ra1.first_payload_offset = (uint32_t)(v1.size() + v2.size());
    ra1.sizes.push_back((uint32_t)a1.size());
    ra1.durations.push_back(1024);
    ra1.flags.push_back(0x01010000);
    ExpectRun ra2;
    ra2.first_payload_offset = (uint32_t)(v1.size() + v2.size() + a1.size() + v3.size());
    ra2.sizes.push_back((uint32_t)a2.size());
    ra2.durations.push_back(1024);
    ra2.flags.push_back(0x01010000);
    ta.runs.push_back(ra1);
    ta.runs.push_back(ra2);
    e.trafs.push_back(ta);

    e.payload.push_back(v1);
    e.payload.push_back(v2);
    e.payload.push_back(a1);
    e.payload.push_back(v3);
    e.payload.push_back(a2);

    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), e);
}

// moov is written before the first non-empty batch and contains every configured
// track, even the one that has not produced a sample yet. MP4E_close commits the
// last non-empty batch.
FB_TEST("layout", moov_all_tracks_and_close_commits)
{
    TestMux m;
    int v = m.add_video(90000, 3000, 160, 120);
    int a = m.add_audio(44100, 1024);
    FB_CHECK(v == 0 && a == 1);

    // Only video in the first batch; audio must still be described in moov.
    std::vector<unsigned char> v1 = bytes_of("V");
    std::vector<unsigned char> v2 = bytes_of("VV");
    m.put_ok(v, v1, 3000);
    m.put_ok(v, v2, 3000);
    m.flush_ok();

    // Only audio in the second batch, which is committed by MP4E_close.
    std::vector<unsigned char> a1 = bytes_of("AAAA");
    m.put_ok(a, a1, 1024);
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 2, 2);

    ExpectFrag e0;
    e0.seq = 1;
    ExpectTraf t0;
    t0.track_id = 1;
    t0.tfdt = 0;
    ExpectRun run0;
    run0.first_payload_offset = 0;
    run0.sizes.push_back((uint32_t)v1.size());
    run0.sizes.push_back((uint32_t)v2.size());
    run0.durations.push_back(3000);
    run0.durations.push_back(3000);
    run0.flags.push_back(0x01010000);
    run0.flags.push_back(0x01010000);
    run0.first_payload_offset = 0;
    t0.runs.push_back(run0);
    e0.trafs.push_back(t0);
    e0.payload.push_back(v1);
    e0.payload.push_back(v2);
    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), e0);

    // Second batch has audio only; its tfdt is audio's own clock (still zero).
    ExpectFrag e1;
    e1.seq = 2;
    ExpectTraf t1;
    t1.track_id = 2;
    t1.tfdt = 0;
    ExpectRun run1;
    run1.first_payload_offset = 0;
    run1.sizes.push_back((uint32_t)a1.size());
    run1.durations.push_back(1024);
    run1.flags.push_back(0x01010000);
    t1.runs.push_back(run1);
    e1.trafs.push_back(t1);
    e1.payload.push_back(a1);
    verify_frag(m.w, fragment_moof(m.w, 1), fragment_mdat(m.w, 1), e1);
}

// Two consecutive explicit flushes, A/B/A ordering in each, sequence numbers
// increment and per-track clocks advance by their own durations only.
FB_TEST("layout", two_consecutive_flushes_ABxA)
{
    TestMux m;
    int a = m.add_audio(1000, 100);
    int b = m.add_audio(48000, 1024);
    FB_CHECK(a == 0 && b == 1);

    std::vector<unsigned char> a1 = bytes_of("a1");
    std::vector<unsigned char> a2 = bytes_of("a2");
    std::vector<unsigned char> b1 = bytes_of("b1");
    std::vector<unsigned char> b2 = bytes_of("b2");
    std::vector<unsigned char> b3 = bytes_of("b3");

    m.put_ok(a, a1, 100);
    m.put_ok(b, b1, 1000);
    m.put_ok(a, a2, 100);
    m.flush_ok();

    m.put_ok(b, b2, 2000);
    m.put_ok(a, a1, 100);
    m.put_ok(b, b3, 0); // default 1024
    m.flush_ok();
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 2, 2);

    ExpectFrag f0;
    f0.seq = 1;
    ExpectTraf ta0;
    ta0.track_id = 1; // track A (track_num 0)
    ta0.tfdt = 0;
    ExpectRun ra0;
    ra0.first_payload_offset = 0;
    ra0.sizes.push_back((uint32_t)a1.size());
    ra0.durations.push_back(100);
    ra0.flags.push_back(0x01010000);
    ExpectRun ra1;
    ra1.first_payload_offset = (uint32_t)(a1.size() + b1.size());
    ra1.sizes.push_back((uint32_t)a2.size());
    ra1.durations.push_back(100);
    ra1.flags.push_back(0x01010000);
    ta0.runs.push_back(ra0);
    ta0.runs.push_back(ra1);
    f0.trafs.push_back(ta0);

    ExpectTraf tb0;
    tb0.track_id = 2;
    tb0.tfdt = 0;
    ExpectRun rb0;
    rb0.first_payload_offset = (uint32_t)a1.size();
    rb0.sizes.push_back((uint32_t)b1.size());
    rb0.durations.push_back(1000);
    rb0.flags.push_back(0x01010000);
    tb0.runs.push_back(rb0);
    f0.trafs.push_back(tb0);
    f0.payload.push_back(a1);
    f0.payload.push_back(b1);
    f0.payload.push_back(a2);
    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), f0);

    ExpectFrag f1;
    f1.seq = 2;
    ExpectTraf tb1;
    tb1.track_id = 2;
    tb1.tfdt = 1000; // B's own clock
    ExpectRun rb2;
    rb2.first_payload_offset = 0;
    rb2.sizes.push_back((uint32_t)b2.size());
    rb2.durations.push_back(2000);
    rb2.flags.push_back(0x01010000);
    ExpectRun rb3;
    rb3.first_payload_offset = (uint32_t)(b2.size() + a1.size());
    rb3.sizes.push_back((uint32_t)b3.size());
    rb3.durations.push_back(1024); // default duration
    rb3.flags.push_back(0x01010000);
    tb1.runs.push_back(rb2);
    tb1.runs.push_back(rb3);
    f1.trafs.push_back(tb1);

    ExpectTraf ta1;
    ta1.track_id = 1;
    ta1.tfdt = 200; // A's own clock, unaffected by B's fragments
    ExpectRun ra2;
    ra2.first_payload_offset = (uint32_t)b2.size();
    ra2.sizes.push_back((uint32_t)a1.size());
    ra2.durations.push_back(100);
    ra2.flags.push_back(0x01010000);
    ta1.runs.push_back(ra2);
    f1.trafs.push_back(ta1);
    f1.payload.push_back(b2);
    f1.payload.push_back(a1);
    f1.payload.push_back(b3);
    verify_frag(m.w, fragment_moof(m.w, 1), fragment_mdat(m.w, 1), f1);
}

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------

// Per-track decode time accumulates real durations (default when 0). Tracks absent
// from a batch keep their own clock, and empty flushes change neither boxes nor
// sequence numbers.
FB_TEST("clock", per_track_tfdt_and_empty_flush)
{
    TestMux m;
    int a = m.add_audio(1000, 100);
    int b = m.add_audio(48000, 1024);
    FB_CHECK(a == 0 && b == 1);

    std::vector<unsigned char> a1 = bytes_of("x");
    std::vector<unsigned char> b1 = bytes_of("y");

    m.put_ok(a, a1, 100);
    m.put_ok(b, b1, 1024);
    m.put_ok(b, b1, 2048);
    m.put_ok(a, a1, 0); // default 100
    m.flush_ok();

    // Empty batch: no boxes, no sequence increment.
    FB_CHECK(m.flush() == 0);
    size_t bytes_after_empty = m.w.bytes.size();

    m.put_ok(a, a1, 50); // A only
    m.flush_ok();
    FB_CHECK(m.w.bytes.size() > bytes_after_empty);

    m.put_ok(b, b1, 1024);
    m.put_ok(a, a1, 0); // default 100
    m.flush_ok();
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 3, 2);

    ExpectFrag f0;
    f0.seq = 1;
    ExpectTraf ta0;
    ta0.track_id = 1;
    ta0.tfdt = 0;
    ExpectRun ra0;
    ra0.first_payload_offset = 0;
    ra0.sizes.push_back((uint32_t)a1.size());
    ra0.durations.push_back(100);
    ra0.flags.push_back(0x01010000);
    ExpectRun ra1;
    ra1.first_payload_offset = (uint32_t)(a1.size() + 2 * b1.size());
    ra1.sizes.push_back((uint32_t)a1.size());
    ra1.durations.push_back(100); // default, not 0
    ra1.flags.push_back(0x01010000);
    ta0.runs.push_back(ra0);
    ta0.runs.push_back(ra1);
    f0.trafs.push_back(ta0);

    ExpectTraf tb0;
    tb0.track_id = 2;
    tb0.tfdt = 0;
    ExpectRun rb0;
    rb0.first_payload_offset = (uint32_t)a1.size();
    rb0.sizes.push_back((uint32_t)b1.size());
    rb0.sizes.push_back((uint32_t)b1.size());
    rb0.durations.push_back(1024);
    rb0.durations.push_back(2048);
    rb0.flags.push_back(0x01010000);
    rb0.flags.push_back(0x01010000);
    tb0.runs.push_back(rb0);
    f0.trafs.push_back(tb0);
    f0.payload.push_back(a1);
    f0.payload.push_back(b1);
    f0.payload.push_back(b1);
    f0.payload.push_back(a1);
    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), f0);

    // Batch 2: A only. A's clock advanced by 100+100 from batch 1.
    ExpectFrag f1;
    f1.seq = 2;
    ExpectTraf ta1;
    ta1.track_id = 1;
    ta1.tfdt = 200;
    ExpectRun ra2;
    ra2.first_payload_offset = 0;
    ra2.sizes.push_back((uint32_t)a1.size());
    ra2.durations.push_back(50);
    ra2.flags.push_back(0x01010000);
    ta1.runs.push_back(ra2);
    f1.trafs.push_back(ta1);
    f1.payload.push_back(a1);
    verify_frag(m.w, fragment_moof(m.w, 1), fragment_mdat(m.w, 1), f1);

    // Batch 3: B continues from its own 1024+2048 clock; A continues from 250.
    ExpectFrag f2;
    f2.seq = 3;
    ExpectTraf tb2;
    tb2.track_id = 2;
    tb2.tfdt = 3072;
    ExpectRun rb2;
    rb2.first_payload_offset = 0;
    rb2.sizes.push_back((uint32_t)b1.size());
    rb2.durations.push_back(1024);
    rb2.flags.push_back(0x01010000);
    tb2.runs.push_back(rb2);
    f2.trafs.push_back(tb2);
    ExpectTraf ta2;
    ta2.track_id = 1;
    ta2.tfdt = 250;
    ExpectRun ra3;
    ra3.first_payload_offset = (uint32_t)b1.size();
    ra3.sizes.push_back((uint32_t)a1.size());
    ra3.durations.push_back(100);
    ra3.flags.push_back(0x01010000);
    ta2.runs.push_back(ra3);
    f2.trafs.push_back(ta2);
    f2.payload.push_back(b1);
    f2.payload.push_back(a1);
    verify_frag(m.w, fragment_moof(m.w, 2), fragment_mdat(m.w, 2), f2);

    // No sample ever started a fragment for the fourth slot.
    FB_CHECK(top_level(m.w).size() == 8);
}

// ---------------------------------------------------------------------------
// continuation
// ---------------------------------------------------------------------------

// A continuation slice extends the most recent sample on its own track: payload
// is appended, size grows, duration is counted once.
FB_TEST("continuation", appends_to_previous_sample)
{
    TestMux m;
    int a = m.add_audio(1000, 100);
    int b = m.add_audio(48000, 1024);
    FB_CHECK(a == 0 && b == 1);

    std::vector<unsigned char> base = bytes_of("base");
    std::vector<unsigned char> slice = bytes_of("slice");
    std::vector<unsigned char> b1 = bytes_of("bb");

    m.put_ok(a, base, 100, MP4E_SAMPLE_RANDOM_ACCESS);
    FB_CHECK(m.put(a, slice, 999, MP4E_SAMPLE_CONTINUATION) == 0);
    m.put_ok(b, b1, 1024);
    m.flush_ok();
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 1, 2);

    std::vector<unsigned char> merged = base;
    merged.insert(merged.end(), slice.begin(), slice.end());

    ExpectFrag e;
    e.seq = 1;
    ExpectTraf ta;
    ta.track_id = 1;
    ta.tfdt = 0;
    ExpectRun run;
    run.first_payload_offset = 0;
    run.sizes.push_back((uint32_t)merged.size()); // base + slice in one sample
    run.durations.push_back(100);                 // counted once
    run.flags.push_back(0x02000000);              // base sample's key-frame identity
    ta.runs.push_back(run);
    e.trafs.push_back(ta);
    ExpectTraf tb;
    tb.track_id = 2;
    tb.tfdt = 0;
    ExpectRun rb;
    rb.first_payload_offset = (uint32_t)merged.size();
    rb.sizes.push_back((uint32_t)b1.size());
    rb.durations.push_back(1024);
    rb.flags.push_back(0x01010000);
    tb.runs.push_back(rb);
    e.trafs.push_back(tb);
    e.payload.push_back(merged);
    e.payload.push_back(b1);
    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), e);
}

// A continuation without a predecessor, or one that would continue another
// track's last sample, is rejected without modifying the pending batch.
FB_TEST("continuation", cross_track_and_missing_predecessor_rejected)
{
    TestMux m;
    int a = m.add_audio(1000, 100);
    int b = m.add_audio(48000, 1024);
    FB_CHECK(a == 0 && b == 1);

    std::vector<unsigned char> a1 = bytes_of("aaa");
    std::vector<unsigned char> b1 = bytes_of("bbb");
    std::vector<unsigned char> bad = bytes_of("XXX");

    // No predecessor at all.
    FB_CHECK(m.put(a, bad, 100, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);

    m.put_ok(a, a1, 100);
    m.put_ok(b, b1, 1024);
    // The last submitted sample belongs to track B, so continuing A is rejected.
    FB_CHECK(m.put(a, bad, 100, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);
    // Continuing a track that is not the last one also fails.
    m.put_ok(a, a1, 100);
    FB_CHECK(m.put(b, bad, 100, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);

    m.flush_ok();
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 1, 2);

    // The rejected slices must not have entered the mdat or the sample sizes.
    std::vector<unsigned char> a_good = a1;
    a_good.insert(a_good.end(), a1.begin(), a1.end());
    ExpectFrag e;
    e.seq = 1;
    ExpectTraf ta;
    ta.track_id = 1;
    ta.tfdt = 0;
    ExpectRun r0;
    r0.first_payload_offset = 0;
    r0.sizes.push_back((uint32_t)a1.size());
    r0.durations.push_back(100);
    r0.flags.push_back(0x01010000);
    ExpectRun r1;
    r1.first_payload_offset = (uint32_t)(a1.size() + b1.size());
    r1.sizes.push_back((uint32_t)a1.size());
    r1.durations.push_back(100);
    r1.flags.push_back(0x01010000);
    ta.runs.push_back(r0);
    ta.runs.push_back(r1);
    e.trafs.push_back(ta);
    ExpectTraf tb;
    tb.track_id = 2;
    tb.tfdt = 0;
    ExpectRun rb;
    rb.first_payload_offset = (uint32_t)a1.size();
    rb.sizes.push_back((uint32_t)b1.size());
    rb.durations.push_back(1024);
    rb.flags.push_back(0x01010000);
    tb.runs.push_back(rb);
    e.trafs.push_back(tb);
    e.payload.push_back(a1);
    e.payload.push_back(b1);
    e.payload.push_back(a1);
    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), e);
}

// After a flush the previous batch's samples can no longer be extended.
FB_TEST("continuation", after_flush_rejected)
{
    TestMux m;
    int a = m.add_audio(1000, 100);
    int b = m.add_audio(48000, 1024);
    FB_CHECK(a == 0 && b == 1);

    std::vector<unsigned char> a1 = bytes_of("A");
    std::vector<unsigned char> a2 = bytes_of("B");
    std::vector<unsigned char> bad = bytes_of("!");

    m.put_ok(a, a1, 100);
    m.flush_ok();
    FB_CHECK(m.put(a, bad, 100, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);
    FB_CHECK(m.flush() == 0); // still empty; nothing new written

    m.put_ok(b, a2, 1024);
    m.flush_ok();
    FB_CHECK(m.put(b, bad, 100, MP4E_SAMPLE_CONTINUATION) == MP4E_STATUS_BAD_ARGUMENTS);
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 2, 2);
    FB_CHECK(fragment_moof(m.w, 0).type == BOX_moof);
    // First fragment has exactly the one accepted sample.
    ExpectFrag e0;
    e0.seq = 1;
    ExpectTraf ta;
    ta.track_id = 1;
    ta.tfdt = 0;
    ExpectRun ra;
    ra.first_payload_offset = 0;
    ra.sizes.push_back((uint32_t)a1.size());
    ra.durations.push_back(100);
    ra.flags.push_back(0x01010000);
    ta.runs.push_back(ra);
    e0.trafs.push_back(ta);
    e0.payload.push_back(a1);
    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), e0);

    ExpectFrag e1;
    e1.seq = 2;
    ExpectTraf tb;
    tb.track_id = 2;
    tb.tfdt = 0;
    ExpectRun rb;
    rb.first_payload_offset = 0;
    rb.sizes.push_back((uint32_t)a2.size());
    rb.durations.push_back(1024);
    rb.flags.push_back(0x01010000);
    tb.runs.push_back(rb);
    e1.trafs.push_back(tb);
    e1.payload.push_back(a2);
    verify_frag(m.w, fragment_moof(m.w, 1), fragment_mdat(m.w, 1), e1);
}

// ---------------------------------------------------------------------------
// failure
// ---------------------------------------------------------------------------

// A real short-write callback (writes a prefix, then reports failure): the flush
// reports the error, the broken muxer emits no later fragment, and MP4E_close
// still releases everything and reports the error.
FB_TEST("failure", short_write_then_no_more_output)
{
    // Learn where the first fragment starts for this exact track setup.
    size_t moof_start;
    {
        TestMux probe;
        int a = probe.add_audio(1000, 100);
        FB_CHECK(a == 0);
        std::vector<unsigned char> p = bytes_of("pq");
        probe.put_ok(a, p, 100);
        FB_CHECK(probe.close() == 0);
        moof_start = fragment_moof(probe.w, 0).off;
    }

    TestMux m;
    m.w.fail_after = moof_start + 2; // partial 'moof', then fail
    m.w.partial = true;
    int a = m.add_audio(1000, 100);
    FB_CHECK(a == 0);

    std::vector<unsigned char> p1 = bytes_of("p1");
    std::vector<unsigned char> p2 = bytes_of("p2");
    m.put_ok(a, p1, 100);
    m.put_ok(a, p2, 100);

    FB_CHECK(m.flush() == MP4E_STATUS_FILE_WRITE_ERROR);
    size_t after_failure = m.w.bytes.size();
    FB_CHECK(after_failure == moof_start + 2); // only the short prefix landed
    FB_CHECK(m.w.failed);

    // No new fragment may be emitted after the writer failed.
    FB_CHECK(m.put(a, bytes_of("zz"), 100) == MP4E_STATUS_FILE_WRITE_ERROR);
    FB_CHECK(m.flush() == MP4E_STATUS_FILE_WRITE_ERROR);
    FB_CHECK(m.w.bytes.size() == after_failure);

    // close must free resources and still report the write failure.
    FB_CHECK(m.close() == MP4E_STATUS_FILE_WRITE_ERROR);
    FB_CHECK(m.w.bytes.size() == after_failure);
}

// Bad arguments do not disturb the pending batch; flush outside mode 2 is rejected.
FB_TEST("failure", bad_arguments_leave_batch_intact)
{
    {
        TestMux one(0xffffffff, 1);
        FB_CHECK(one.flush() == MP4E_STATUS_BAD_ARGUMENTS);
        FB_CHECK(one.close() == 0);
        FB_CHECK(top_level(one.w).size() == 1); // only ftyp from MP4E_open
    }

    TestMux m;
    int a = m.add_audio(1000, 100);
    FB_CHECK(a == 0);
    std::vector<unsigned char> good = bytes_of("ok");
    std::vector<unsigned char> extra = bytes_of("nope");

    m.put_ok(a, good, 100);
    FB_CHECK(m.put(7, extra, 100) == MP4E_STATUS_BAD_ARGUMENTS);   // no such track
    FB_CHECK(m.put(-1, extra, 100) == MP4E_STATUS_BAD_ARGUMENTS);
    FB_CHECK(m.put(a, extra, -1) == MP4E_STATUS_BAD_ARGUMENTS);    // negative duration
    m.flush_ok();
    FB_CHECK(m.close() == 0);

    check_file_shape(m.w, 1, 1);
    ExpectFrag e;
    e.seq = 1;
    ExpectTraf ta;
    ta.track_id = 1;
    ta.tfdt = 0;
    ExpectRun ra;
    ra.first_payload_offset = 0;
    ra.sizes.push_back((uint32_t)good.size());
    ra.durations.push_back(100);
    ra.flags.push_back(0x01010000);
    ta.runs.push_back(ra);
    e.trafs.push_back(ta);
    e.payload.push_back(good);
    verify_frag(m.w, fragment_moof(m.w, 0), fragment_mdat(m.w, 0), e);
}
