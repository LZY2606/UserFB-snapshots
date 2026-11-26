// Regression tests for seeking with a non-zero 40-bit sample origin.
//
// These tests only use the public decoder/encoder API and an independent byte
// catalog built in tests/seek_origin_support.h. No audio hardware or network
// is involved, and no production header macro is used as an oracle.

#include "task_support/check.h"
#include "tests/seek_origin_support.h"

#define PCM_SENT 0x7f3a1b2c
#define SENT_WORDS 8

static const int so_chunks [3] = {1, 7, 100};      // 1, 7, and cross-block

////////////////////////////// PCM comparison ////////////////////////////////

// Compare "frames" decoded frames against the exact pre-encode array. The
// first mismatch is reported with a SEEK_PCM_DIFF line, then the caller's
// TASK_CHECK turns the zero return into a failed case.

static int check_pcm_block (const char *material, const int32_t *got,
    const SoMaterial *m, int64_t first_frame, int frames)
{
    int i, c;

    for (i = 0; i < frames; ++i)
        for (c = 0; c < m->channels; ++c) {
            int32_t actual = got [i * m->channels + c];
            int32_t expected = m->pcm [(first_frame + i) * m->channels + c];

            if (actual != expected) {
                printf ("SEEK_PCM_DIFF material=%s target_frame=%lld channel=%d actual=%d expected=%d\n",
                    material, (long long) (first_frame + i), c, (int) actual, (int) expected);
                return 0;
            }
        }

    return 1;
}

// Decode exactly "want" frames into a sentinel-guarded buffer and compare them
// against the oracle.

static int unpack_checked (const char *material, WavpackContext *ctx, const SoMaterial *m,
    int64_t first_frame, int want, int *got_out)
{
    int nch = m->channels, total = want * nch, i, got;
    int32_t *buf = (int32_t *) malloc (sizeof (int32_t) * (total + 2 * SENT_WORDS));
    int32_t *out = buf + SENT_WORDS;

    TASK_CHECK (buf != NULL);

    for (i = 0; i < SENT_WORDS; ++i) {
        buf [i] = PCM_SENT;
        out [total + i] = PCM_SENT;
    }

    got = (int) WavpackUnpackSamples (ctx, out, (uint32_t) want);
    TASK_CHECK (got >= 0 && got <= want);
    TASK_CHECK (check_pcm_block (material, out, m, first_frame, got));

    for (i = 0; i < SENT_WORDS; ++i) {
        TASK_CHECK (buf [i] == PCM_SENT);
        TASK_CHECK (out [total + i] == PCM_SENT);
    }

    *got_out = got;
    free (buf);
    return 0;
}

// Read "frames" frames from the current position using an alternating chunk
// size (1, 7, cross-block) and check every sample plus the error counter.

static int read_range (const char *material, WavpackContext *ctx, const SoMaterial *m,
    int64_t first_frame, int frames, int *iter)
{
    int done = 0;

    while (done < frames) {
        int want = so_chunks [(*iter)++ % 3], got;

        if (want > frames - done)
            want = frames - done;

        TASK_CHECK (unpack_checked (material, ctx, m, first_frame + done, want, &got) == 0);
        TASK_CHECK (got == want);
        done += got;
    }

    TASK_CHECK (WavpackGetNumErrors (ctx) == 0);
    return 0;
}

// Seek to a valid relative frame, verify the relative cursor, then decode the
// rest of the material. The cursor is recorded before decoding so that a
// wrong PCM result is reported (and fails) before the cursor assertion.

static int seek_read (const char *material, WavpackContext *ctx, const SoMaterial *m,
    int64_t target, int frames, int *iter)
{
    int64_t cursor;

    TASK_CHECK (WavpackSeekSample64 (ctx, target));
    cursor = WavpackGetSampleIndex64 (ctx);
    TASK_CHECK (read_range (material, ctx, m, target, frames, iter) == 0);
    TASK_CHECK (cursor == target);
    return 0;
}

/////////////////////////////// player setup /////////////////////////////////

typedef struct {
    SoMaterial *mat;
    SoCatalog cat, corr_cat;
    int has_corr;
    SoTraceMem wv, wvc;
    WavpackContext *ctx;
    int64_t shift;
} Player;

static int player_open (Player *p, SoMaterial *m, int64_t shift, int keep_total, int use_corr)
{
    char error [256] = {0};

    memset (p, 0, sizeof (*p));
    p->mat = m;
    p->shift = shift;
    p->has_corr = use_corr && m->hybrid;

    if (shift && !so_relabel_shift (m->wv.data, m->wv.size, shift, keep_total))
        return 0;

    if (shift && p->has_corr && !so_relabel_shift (m->wvc.data, m->wvc.size, shift, keep_total))
        return 0;

    if (!so_build_catalog (m->wv.data, m->wv.size, &p->cat, shift))
        return 0;

    if (p->has_corr && !so_build_catalog (m->wvc.data, m->wvc.size, &p->corr_cat, shift))
        return 0;

    p->wv.mem.data = m->wv.data;
    p->wv.mem.size = m->wv.size;

    if (p->has_corr) {
        p->wvc.mem.data = m->wvc.data;
        p->wvc.mem.size = m->wvc.size;
    }

    p->ctx = WavpackOpenFileInputEx64 (&so_trace_reader, &p->wv,
        p->has_corr ? &p->wvc : NULL, error, 0, 0);

    return p->ctx != NULL;
}

static void player_close (Player *p)
{
    if (p->ctx)
        WavpackCloseFile (p->ctx);

    so_catalog_free (&p->cat);

    if (p->has_corr)
        so_catalog_free (&p->corr_cat);

    p->ctx = NULL;
}

static int trace_has (const SoTraceMem *t, size_t offset)
{
    int i;

    for (i = 0; i < t->trace_count; ++i)
        if (t->trace [i] == offset)
            return 1;

    return 0;
}

// Confirm the catalog, the stored 40-bit origin, the header's total length and
// a straight sequential decode before any seeking happens.

static int verify_material (Player *p, int keep_total, const char *material)
{
    SoMaterial *m = p->mat;
    int iter = 0, g;

    TASK_CHECK (p->cat.groups > 0);
    TASK_CHECK (so_catalog_consistent (&p->cat));
    TASK_CHECK (p->cat.total_frames == m->frames);
    TASK_CHECK ((int64_t) p->cat.blocks [0].raw_index == p->shift);
    TASK_CHECK (so_total_samples (m->wv.data) == (keep_total ? (int64_t) m->frames : -1));

    for (g = 0; g < p->cat.groups; ++g) {
        TASK_CHECK (p->cat.blocks [p->cat.group_start [g]].initial);
        TASK_CHECK (p->cat.rel_start [g] ==
            p->cat.blocks [p->cat.group_start [g]].raw_index - p->shift);
    }

    if (p->has_corr) {
        TASK_CHECK (p->corr_cat.total_frames == m->frames);
        TASK_CHECK (so_catalog_consistent (&p->corr_cat));
        TASK_CHECK ((int64_t) p->corr_cat.blocks [0].raw_index == p->shift);
    }

    TASK_CHECK (WavpackGetNumSamples64 (p->ctx) == m->frames);
    TASK_CHECK (read_range (material, p->ctx, m, 0, m->frames, &iter) == 0);

    printf ("MATERIAL_OK material=%s channels=%d frames=%d block_samples=%d "
        "shift=%lld total=%s wv_blocks=%zu wv_groups=%d origin=%lld\n",
        material, m->channels, m->frames, m->block_samples, (long long) p->shift,
        keep_total ? "known" : "unknown", p->cat.count, p->cat.groups,
        (long long) (p->cat.blocks [0].raw_index - p->cat.rel_start [0]));

    if (p->has_corr)
        printf ("WVC_CATALOG material=%s blocks=%zu groups=%d frames=%lld\n",
            material, p->corr_cat.count, p->corr_cat.groups,
            (long long) p->corr_cat.total_frames);

    return 0;
}

// Decoding the hybrid material without the correction file must produce
// different PCM, proving that the lane test really needs the .wvc lane.

static int ignore_wvc_differs (const SoMaterial *m, const char *material)
{
    SoMem mem;
    WavpackContext *ctx;
    int32_t *out;
    int total = m->frames * m->channels, i, diffs = 0;
    char error [128] = {0};

    memset (&mem, 0, sizeof (mem));
    mem.data = m->wv.data;
    mem.size = m->wv.size;

    ctx = WavpackOpenFileInputEx64 (&so_reader, &mem, NULL, error, 0, 0);
    TASK_CHECK (ctx != NULL);

    out = (int32_t *) malloc (sizeof (int32_t) * total);
    TASK_CHECK (out != NULL);
    TASK_CHECK ((int) WavpackUnpackSamples (ctx, out, (uint32_t) m->frames) == m->frames);

    for (i = 0; i < total; ++i)
        if (out [i] != m->pcm [i])
            ++diffs;

    TASK_CHECK (diffs > 0);
    printf ("WVC_IGNORED_DIFF material=%s differing_samples=%d\n", material, diffs);

    free (out);
    WavpackCloseFile (ctx);
    return 0;
}

/////////////////////////////// origin group /////////////////////////////////

static int run_origin (const char *material, int channels, int64_t shift, int keep_total)
{
    static const int64_t targets [] = {350, 5, 200, 130, 0, 396, 63, 65, 257, 320, 129, 385};
    SoMaterial m;
    Player p;
    int iter = 0;
    size_t t;

    TASK_CHECK (so_material_create (&m, channels, 397, 64, 0, SO_ORIGIN_BASE + shift,
        keep_total ? 397 : -1));
    TASK_CHECK (player_open (&p, &m, shift, keep_total, 0));
    TASK_CHECK (verify_material (&p, keep_total, material) == 0);

    for (t = 0; t < sizeof (targets) / sizeof (targets [0]); ++t)
        if (targets [t] < m.frames)
            TASK_CHECK (seek_read (material, p.ctx, &m, targets [t],
                m.frames - (int) targets [t], &iter) == 0);

    TASK_CHECK (WavpackGetNumErrors (p.ctx) == 0);
    player_close (&p);
    so_material_free (&m);
    return 0;
}

static int origin_zero_known_stereo (void)
{
    return run_origin ("origin/zero_known_stereo", 2, 0, 1);
}

static int origin_small_known_stereo (void)
{
    return run_origin ("origin/small_known_stereo", 2, 12345, 1);
}

static int origin_cross32_known_stereo (void)
{
    return run_origin ("origin/cross32_known_stereo", 2, 0xfffffff0LL, 1);
}

static int origin_cross32_unknown_mono (void)
{
    return run_origin ("origin/cross32_unknown_mono", 1, 0xfffffff0LL, 0);
}

static int origin_over32_known_stereo (void)
{
    return run_origin ("origin/over32_known_stereo", 2, (1LL << 32) + 12345, 1);
}

static int origin_over32_unknown_stereo (void)
{
    return run_origin ("origin/over32_unknown_stereo", 2, (1LL << 32) + 999, 0);
}

//////////////////////////////// lanes group /////////////////////////////////

static int run_lanes (const char *material, int64_t shift, int keep_total)
{
    static const int64_t candidates [] = {350, 40, 200, 130, 250, 396, 63, 65, 257, 320, 180, 340};
    SoMaterial m;
    Player p;
    int iter = 0, used = 0;
    size_t t;

    TASK_CHECK (so_material_create (&m, 2, 397, 64, 1, SO_ORIGIN_BASE + shift + 777,
        keep_total ? 397 : -1));
    TASK_CHECK (player_open (&p, &m, shift, keep_total, 1));
    TASK_CHECK (p.has_corr);
    TASK_CHECK (verify_material (&p, keep_total, material) == 0);
    TASK_CHECK (ignore_wvc_differs (&m, material) == 0);

    for (t = 0; t < sizeof (candidates) / sizeof (candidates [0]); ++t) {
        int64_t target = candidates [t], cursor;
        const SoBlock *bw, *bc;

        bw = so_find_block (&p.cat, target, 0);
        bc = so_find_block (&p.corr_cat, target, 0);
        TASK_CHECK (bw != NULL && bc != NULL);

        // Only use targets whose block really does start at a different byte
        // position in the two lanes, so the per-lane offset checks below are
        // meaningful (block zero, for instance, is at offset zero in both).
        if (bw->phys_offset == bc->phys_offset)
            continue;

        // Park in a different block first, so the measured seek is guaranteed
        // to have to load the target block rather than reusing a resident one.
        TASK_CHECK (WavpackSeekSample64 (p.ctx, target >= 64 ? 0 : 100));

        p.wv.trace_count = p.wvc.trace_count = 0;
        TASK_CHECK (WavpackSeekSample64 (p.ctx, target));
        cursor = WavpackGetSampleIndex64 (p.ctx);

        // Search/load trace: each lane's own reader must have read from its
        // own catalog entry for the target block. The WV offset must never be
        // accepted as evidence for the correction lane.
        TASK_CHECK (p.wv.trace_count > 0 && p.wvc.trace_count > 0);
        TASK_CHECK (trace_has (&p.wv, bw->phys_offset));
        TASK_CHECK (trace_has (&p.wvc, bc->phys_offset));

        printf ("LANES_TARGET material=%s target=%lld wv_block=%zu wvc_block=%zu "
            "wv_trace_hit=%d wvc_trace_hit=%d\n", material, (long long) target,
            bw->phys_offset, bc->phys_offset,
            trace_has (&p.wv, bw->phys_offset), trace_has (&p.wvc, bc->phys_offset));

        TASK_CHECK (read_range (material, p.ctx, &m, target, m.frames - (int) target, &iter) == 0);
        TASK_CHECK (cursor == target);
        ++used;
    }

    // Enough targets with genuinely distinct per-lane byte positions were
    // exercised, and they span several block groups.
    TASK_CHECK (used >= 5);
    TASK_CHECK (WavpackGetNumErrors (p.ctx) == 0);
    player_close (&p);
    so_material_free (&m);
    return 0;
}

static int lanes_known_zero_stereo (void)
{
    return run_lanes ("lanes/known_zero_stereo", 0, 1);
}

static int lanes_known_over32_stereo (void)
{
    return run_lanes ("lanes/known_over32_stereo", (1LL << 32) + 12345, 1);
}

static int lanes_unknown_small_stereo (void)
{
    return run_lanes ("lanes/unknown_small_stereo", 777, 0);
}

/////////////////////////////// restart group ////////////////////////////////

static int restart_same_block_backward (void)
{
    const char *material = "restart/same_block_backward";
    SoMaterial m;
    Player p;
    int iter = 0;

    TASK_CHECK (so_material_create (&m, 2, 397, 64, 0, SO_ORIGIN_BASE + 4321, 397));
    TASK_CHECK (player_open (&p, &m, 4321, 1, 0));
    TASK_CHECK (verify_material (&p, 1, material) == 0);

    TASK_CHECK (seek_read (material, p.ctx, &m, 40, 7, &iter) == 0);
    TASK_CHECK (seek_read (material, p.ctx, &m, 20, 7, &iter) == 0);   // backwards, same block
    TASK_CHECK (seek_read (material, p.ctx, &m, 55, 9, &iter) == 0);   // forwards, same block
    TASK_CHECK (seek_read (material, p.ctx, &m, 5, 10, &iter) == 0);
    TASK_CHECK (WavpackGetNumErrors (p.ctx) == 0);

    player_close (&p);
    so_material_free (&m);
    return 0;
}

static int restart_cross_block_jump (void)
{
    const char *material = "restart/cross_block_jump";
    SoMaterial m;
    Player p;
    int iter = 0;

    TASK_CHECK (so_material_create (&m, 2, 397, 64, 0, SO_ORIGIN_BASE + 1, 397));
    TASK_CHECK (player_open (&p, &m, 1, 1, 0));
    TASK_CHECK (verify_material (&p, 1, material) == 0);

    TASK_CHECK (seek_read (material, p.ctx, &m, 350, 5, &iter) == 0);
    TASK_CHECK (seek_read (material, p.ctx, &m, 3, 7, &iter) == 0);
    TASK_CHECK (seek_read (material, p.ctx, &m, 300, 1, &iter) == 0);
    TASK_CHECK (seek_read (material, p.ctx, &m, 64, 7, &iter) == 0);
    TASK_CHECK (seek_read (material, p.ctx, &m, 200, 40, &iter) == 0); // crosses a block
    TASK_CHECK (WavpackGetNumErrors (p.ctx) == 0);

    player_close (&p);
    so_material_free (&m);
    return 0;
}

static int restart_consecutive_no_read (void)
{
    const char *material = "restart/consecutive_no_read";
    static const int64_t chain [] = {200, 100, 130, 0, 257};
    SoMaterial m;
    Player p;
    int iter = 0, i;

    TASK_CHECK (so_material_create (&m, 2, 397, 64, 0, SO_ORIGIN_BASE + 60, 397));
    TASK_CHECK (player_open (&p, &m, 60, 1, 0));
    TASK_CHECK (verify_material (&p, 1, material) == 0);

    // Several seeks back to back, with no decoding in between.
    for (i = 0; i < (int) (sizeof (chain) / sizeof (chain [0])); ++i)
        TASK_CHECK (WavpackSeekSample64 (p.ctx, chain [i]));

    TASK_CHECK (WavpackGetSampleIndex64 (p.ctx) == 257);
    TASK_CHECK (read_range (material, p.ctx, &m, 257, m.frames - 257, &iter) == 0);
    TASK_CHECK (WavpackGetNumErrors (p.ctx) == 0);

    player_close (&p);
    so_material_free (&m);
    return 0;
}

static int restart_end_short_then_start (void)
{
    const char *material = "restart/end_short_then_start";
    SoMaterial m;
    Player p;
    int iter = 0, got;
    int32_t eof_buf [4] = {PCM_SENT, PCM_SENT, PCM_SENT, PCM_SENT};

    TASK_CHECK (so_material_create (&m, 2, 397, 64, 0, SO_ORIGIN_BASE + 9, 397));
    TASK_CHECK (player_open (&p, &m, 9, 1, 0));
    TASK_CHECK (verify_material (&p, 1, material) == 0);

    // A short read at the end: ask for more than remains.
    TASK_CHECK (WavpackSeekSample64 (p.ctx, m.frames - 17));
    TASK_CHECK (WavpackGetSampleIndex64 (p.ctx) == m.frames - 17);
    TASK_CHECK (unpack_checked (material, p.ctx, &m, m.frames - 17, 100, &got) == 0);
    TASK_CHECK (got == 17);

    // Reaching the end is expressed by reading, not by a seek to the length.
    // The API zero-fills the region it is handed even when nothing is left,
    // so keep the sentinels just outside that one-frame stereo region.
    TASK_CHECK (WavpackUnpackSamples (p.ctx, eof_buf + 1, 1) == 0);
    TASK_CHECK (eof_buf [0] == PCM_SENT && eof_buf [3] == PCM_SENT);

    // Return to the start and read the whole material again.
    TASK_CHECK (seek_read (material, p.ctx, &m, 0, m.frames, &iter) == 0);
    TASK_CHECK (WavpackGetNumErrors (p.ctx) == 0);

    player_close (&p);
    so_material_free (&m);
    return 0;
}

static int restart_sixch_nonfirst_preroll (void)
{
    const char *material = "restart/sixch_nonfirst_preroll";
    static const int64_t targets [] = {63, 130, 66, 200, 100, 388, 5};
    SoMaterial m;
    Player p;
    int iter = 0;
    size_t t;

    TASK_CHECK (so_material_create (&m, 6, 389, 64, 0, SO_ORIGIN_BASE, 389));
    TASK_CHECK (player_open (&p, &m, 0, 1, 0));
    TASK_CHECK (p.cat.groups > 1);

    // Each multichannel group must really contain more than one sub-stream.
    for (t = 0; t < (size_t) p.cat.groups; ++t)
        TASK_CHECK (p.cat.group_end_idx [t] > p.cat.group_start [t] + 1);

    TASK_CHECK (verify_material (&p, 1, material) == 0);

    for (t = 0; t < sizeof (targets) / sizeof (targets [0]); ++t) {
        int n;

        if (targets [t] >= m.frames)
            continue;

        n = m.frames - (int) targets [t];

        if (n > 50)
            n = 50;

        TASK_CHECK (seek_read (material, p.ctx, &m, targets [t], n, &iter) == 0);
    }

    TASK_CHECK (WavpackGetNumErrors (p.ctx) == 0);
    player_close (&p);
    so_material_free (&m);
    return 0;
}

static int restart_interleaved_two_instances (void)
{
    const char *mat_a = "restart/interleaved_a";
    const char *mat_b = "restart/interleaved_b";
    SoMaterial a, b;
    Player pa, pb;
    int ia = 0, ib = 0;
    int step;

    TASK_CHECK (so_material_create (&a, 2, 397, 64, 0, SO_ORIGIN_BASE + 31337, 397));
    TASK_CHECK (so_material_create (&b, 6, 389, 64, 0, SO_ORIGIN_BASE, 389));
    TASK_CHECK (player_open (&pa, &a, 31337, 1, 0));
    TASK_CHECK (player_open (&pb, &b, 0, 1, 0));
    TASK_CHECK (verify_material (&pa, 1, mat_a) == 0);
    TASK_CHECK (verify_material (&pb, 1, mat_b) == 0);

    // Alternate operations on the two independent contexts; any state leaking
    // between them would surface as a PCM difference for the other material.
    for (step = 0; step < 4; ++step) {
        TASK_CHECK (seek_read (mat_a, pa.ctx, &a, 200 - step * 40, 5, &ia) == 0);
        TASK_CHECK (seek_read (mat_b, pb.ctx, &b, 63 + step * 30, 1, &ib) == 0);
        TASK_CHECK (seek_read (mat_a, pa.ctx, &a, 20 + step * 50, 7, &ia) == 0);
        TASK_CHECK (seek_read (mat_b, pb.ctx, &b, 130 - step * 20, 7, &ib) == 0);
        TASK_CHECK (seek_read (mat_a, pa.ctx, &a, 350 - step * 60, 1, &ia) == 0);
        TASK_CHECK (seek_read (mat_b, pb.ctx, &b, 300 - step * 50, 3, &ib) == 0);
    }

    TASK_CHECK (WavpackGetNumErrors (pa.ctx) == 0);
    TASK_CHECK (WavpackGetNumErrors (pb.ctx) == 0);

    player_close (&pa);
    player_close (&pb);
    so_material_free (&a);
    so_material_free (&b);
    return 0;
}

//////////////////////////////// registry ////////////////////////////////////

const struct task_case task_cases [] = {
    {"origin", "zero_known_stereo", origin_zero_known_stereo},
    {"origin", "small_known_stereo", origin_small_known_stereo},
    {"origin", "cross32_known_stereo", origin_cross32_known_stereo},
    {"origin", "cross32_unknown_mono", origin_cross32_unknown_mono},
    {"origin", "over32_known_stereo", origin_over32_known_stereo},
    {"origin", "over32_unknown_stereo", origin_over32_unknown_stereo},
    {"lanes", "known_zero_stereo", lanes_known_zero_stereo},
    {"lanes", "known_over32_stereo", lanes_known_over32_stereo},
    {"lanes", "unknown_small_stereo", lanes_unknown_small_stereo},
    {"restart", "same_block_backward", restart_same_block_backward},
    {"restart", "cross_block_jump", restart_cross_block_jump},
    {"restart", "consecutive_no_read", restart_consecutive_no_read},
    {"restart", "end_short_then_start", restart_end_short_then_start},
    {"restart", "sixch_nonfirst_preroll", restart_sixch_nonfirst_preroll},
    {"restart", "interleaved_two_instances", restart_interleaved_two_instances}
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]);
