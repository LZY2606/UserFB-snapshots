/* Tests for linked left/right stereo regions in tsf.h.
 *
 * This file only declares the test cases; the TSF implementation and the test
 * driver live elsewhere. All expected values are derived independently from
 * the raw PCM built here and the documented formulas, never from a prior TSF
 * output.
 */
#include "task_support/check.h"
#include "task_support/fixture.h"
#include "task_support/alloc.h"
#include "tsf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Signals and font helpers                                                  */
/* ------------------------------------------------------------------------- */

#define PCM_LEN 256
#define POOL_SHORTS 4096

static short g_left[PCM_LEN];
static short g_right[PCM_LEN];
static short g_pool[POOL_SHORTS];

static void build_signals(void)
{
    int i;
    for (i = 0; i < PCM_LEN; ++i)
    {
        g_left[i]  = (short)(((i * 73) % 211 - 105) * 100);
        g_right[i] = (short)(((i * 97) % 193 - 96) * 110);
    }
}

static void clear_pool(void) { memset(g_pool, 0, sizeof(g_pool)); }

/* Load a font built by task_make_font_ext, taking ownership of nothing. */
static tsf* load_ext(const struct task_font_ext* e)
{
    struct task_font_bytes b = task_make_font_ext(e);
    tsf* f;
    if (!b.data) return NULL;
    f = tsf_load_memory(b.data, b.size);
    free(b.data);
    return f;
}

/* Canonical linked pair. The left end sits at short offset 0 and the right end
 * at `rightOffset` (defaults to PCM_LEN, i.e. adjacent). Both have the same
 * relative layout: start at 0, length `len`, loop [16, 16+loopLen). The zone
 * may reference either end. */
static void setup_pair(struct task_font_ext* e, unsigned rightOffset,
                       unsigned loopLen, unsigned len, int referenceRight, int loopMode)
{
    memset(e, 0, sizeof(*e));
    e->sampleCount = 2;
    e->pcmLen = POOL_SHORTS / 2;
    e->pcm = g_pool;
    e->specs[0].start = 0;
    e->specs[0].end = len;
    e->specs[0].startLoop = 16;
    e->specs[0].endLoop = 16 + loopLen;
    e->specs[0].sampleRate = 22050;
    e->specs[0].originalPitch = 60;
    e->specs[0].pitchCorrection = 0;
    e->specs[0].sampleLink = 1;
    e->specs[0].sampleType = 4;
    e->specs[1].start = rightOffset;
    e->specs[1].end = rightOffset + len;
    e->specs[1].startLoop = rightOffset + 16;
    e->specs[1].endLoop = rightOffset + 16 + loopLen;
    e->specs[1].sampleRate = 22050;
    e->specs[1].originalPitch = 60;
    e->specs[1].pitchCorrection = 0;
    e->specs[1].sampleLink = 0;
    e->specs[1].sampleType = 2;
    e->zoneCount = 1;
    e->zones[0].sample = referenceRight ? 1 : 0;
    e->zones[0].loopMode = loopMode;
    e->zones[0].genCount = 0;
}

/* Add a generator to the only zone. */
static void zone_gen(struct task_font_ext* e, int op, int amt)
{
    struct task_zone* z = &e->zones[0];
    z->genOp[z->genCount] = op;
    z->genAmt[z->genCount] = amt;
    z->genCount++;
}

/* ------------------------------------------------------------------------- */
/* Independent simulation of the documented render path                      */
/* ------------------------------------------------------------------------- */

/* Documented stereo balance gains for a merged pan p in [-0.5, 0.5]. */
static void balance_gains(double p, double* gl, double* gr)
{
    if (p < -0.5) p = -0.5;
    if (p >  0.5) p =  0.5;
    *gl = sqrt(1.0 - 2.0 * (p > 0.0 ? p : 0.0));
    *gr = sqrt(1.0 + 2.0 * (p < 0.0 ? p : 0.0));
}

/* Pitch ratio exactly as documented: note pitch relative to the sample's root
 * key, scaled by sample and output rates. */
static double expected_ratio(int sampleRate, int outRate, int key, int root, int keytrack)
{
    double note = key;
    double adjusted = root + (note - root) * (keytrack / 100.0);
    double inTimecents = adjusted * 100.0;
    double factor = sampleRate / (pow(2.0, root * 100.0 / 1200.0) * outRate);
    return pow(2.0, inTimecents / 1200.0) * factor;
}

/* Produce the per-frame read position relative to each side's start. Position
 * advances by `ratio`, folding back by the loop period when the exclusive loop
 * end is reached. */
static void position_sequence(double* pos, int frames, double ratio, int looping,
                              double loopStart, double loopLenEx)
{
    int i;
    double p = 0.0;
    for (i = 0; i < frames; ++i)
    {
        pos[i] = p;
        p += ratio;
        if (looping && p >= loopStart + loopLenEx) p -= loopLenEx;
    }
}

/* Evaluate one side at a fractional position with linear interpolation. When
 * looping, the interpolation neighbour of the inclusive last loop sample is
 * the loop start; otherwise the neighbour at the play-range end clamps to this
 * side's last actual sample. */
static double sample_side(const short* pcm, int pcmLen, double p, int looping,
                          double loopStart, double loopLenEx)
{
    int pos0, nextPos;
    int loopEndIncl = (int)(loopStart + loopLenEx) - 1;
    int last = pcmLen - 1;
    double alpha, v;
    if (p >= (double)pcmLen) return 0.0;
    pos0 = (int)p;
    if (pos0 > last) return 0.0;
    if (looping && pos0 >= loopEndIncl) nextPos = (int)loopStart;
    else if (pos0 >= last) nextPos = last;
    else nextPos = pos0 + 1;
    if (nextPos > last) nextPos = last;
    alpha = p - (double)pos0;
    v = (double)pcm[pos0] * (1.0 - alpha) + (double)pcm[nextPos] * alpha;
    return v / 32767.0;
}

/* Documented biquad low-pass with independent state. */
struct test_biquad { double a0, a1, b1, b2, z1, z2; };
static void biquad_setup(struct test_biquad* b, double Fc, double QInv)
{
    double K = tan(3.14159265358979323846264338327950288 * Fc), KK = K * K;
    double norm = 1.0 / (1.0 + K * QInv + KK);
    b->a0 = KK * norm;
    b->a1 = 2.0 * b->a0;
    b->b1 = 2.0 * (KK - 1.0) * norm;
    b->b2 = (1.0 - K * QInv + KK) * norm;
    b->z1 = b->z2 = 0.0;
}
static double biquad_process(struct test_biquad* b, double in)
{
    double out = in * b->a0 + b->z1;
    b->z1 = in * b->a1 + b->z2 - b->b1 * out;
    b->z2 = in * b->a0 - b->b2 * out;
    return out;
}

static double cents_to_hz(double cents) { return 8.176 * pow(2.0, cents / 1200.0); }

/* ------------------------------------------------------------------------- */
/* Pairing tests (load-time decisions)                                       */
/* ------------------------------------------------------------------------- */

static int pairing_left_only_links(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    TASK_CHECK(tsf_get_presetcount(f) == 1);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    /* One hit -> one voice that carries both ends. */
    TASK_CHECK(tsf_active_voice_count(f) == 1);
    tsf_close(f);
    return 0;
}

static int pairing_right_only_links(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 1, 1);
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 1);
    tsf_close(f);
    return 0;
}

static int pairing_far_apart_storage(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    /* Right end stored far away from the left, beyond the left length. */
    memcpy(g_pool + 1024, g_right, sizeof(g_right));
    setup_pair(&e, 1024, 200, PCM_LEN, 0, 1);
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 1);
    tsf_close(f);
    return 0;
}

static int pairing_rejects_bad_link(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    e.specs[0].sampleLink = 0; /* must point at right */
    e.specs[1].sampleLink = 1; /* must point at left */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

static int pairing_rejects_type_mismatch(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    e.specs[1].sampleType = 4; /* both claim left */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

static int pairing_rejects_tuning_mismatch(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    e.specs[1].sampleRate = 44100;
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

static int pairing_rejects_rom(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    e.specs[1].sampleType |= 0x8000;
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

static int pairing_rejects_length_mismatch(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    e.specs[1].end -= 2; /* different raw length */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

static int pairing_rejects_relative_loop_mismatch(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    e.specs[1].startLoop += 4; /* relative loop start differs */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

static int pairing_rejects_eos_as_end(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    e.specs[0].sampleLink = 2; /* EOS terminator is not a playable right end */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

/* A bad linked pair must not affect the other zones of the preset. Sample 0/1
 * form a bad pair, sample 2 is a valid plain mono layer, sample 3 is another. */
static int pairing_bad_pair_keeps_other_layers(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    memset(&e, 0, sizeof(e));
    e.sampleCount = 4;
    e.pcmLen = 2 * PCM_LEN; /* two samples per sdta entry, covering both halves */
    e.pcm = g_pool;
    e.specs[0].start = 0; e.specs[0].end = PCM_LEN; e.specs[0].startLoop = 16; e.specs[0].endLoop = 216;
    e.specs[0].sampleRate = 22050; e.specs[0].originalPitch = 60;
    e.specs[0].sampleLink = 0; e.specs[0].sampleType = 4; /* bad: links to self */
    e.specs[1].start = PCM_LEN; e.specs[1].end = 2 * PCM_LEN; e.specs[1].startLoop = PCM_LEN + 16; e.specs[1].endLoop = PCM_LEN + 216;
    e.specs[1].sampleRate = 22050; e.specs[1].originalPitch = 60;
    e.specs[1].sampleLink = 1; e.specs[1].sampleType = 2;
    e.specs[2].start = 0; e.specs[2].end = PCM_LEN; e.specs[2].startLoop = 16; e.specs[2].endLoop = 216;
    e.specs[2].sampleRate = 22050; e.specs[2].originalPitch = 60;
    e.specs[2].sampleLink = 0; e.specs[2].sampleType = 1; /* plain mono */
    e.specs[3].start = PCM_LEN; e.specs[3].end = 2 * PCM_LEN; e.specs[3].startLoop = PCM_LEN + 16; e.specs[3].endLoop = PCM_LEN + 216;
    e.specs[3].sampleRate = 22050; e.specs[3].originalPitch = 60;
    e.specs[3].sampleLink = 0; e.specs[3].sampleType = 1; /* plain mono */
    e.zoneCount = 3;
    e.zones[0].sample = 0; e.zones[0].loopMode = 0;
    e.zones[1].sample = 2; e.zones[1].loopMode = 0;
    e.zones[2].sample = 3; e.zones[2].loopMode = 0;
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 8));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 2);
    tsf_close(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Render tests                                                              */
/* ------------------------------------------------------------------------- */

/* Render one linked pair at key 61 with INTERLEAVED output and compare both
 * channels to an independently simulated unity-gain stereo path. This crosses
 * the loop several times because the fractional pitch step is not integral. */
static int render_interleaved_fractional_loop(void)
{
    enum { N = 1200 };
    struct task_font_ext e;
    static float out[N * 2];
    static double pos[N];
    tsf* f;
    double ratio = expected_ratio(22050, 22050, 61, 60, 100);
    int i, folded = 0;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, out, N, 0);
    TASK_CHECK(tsf_active_voice_count(f) == 1);

    position_sequence(pos, N, ratio, 1, 16.0, 200.0);
    for (i = 0; i < N; ++i)
    {
        double expL = sample_side(g_left, PCM_LEN, pos[i], 1, 16.0, 200.0);
        double expR = sample_side(g_right, PCM_LEN, pos[i], 1, 16.0, 200.0);
        double dL = fabs(out[i * 2] - expL), dR = fabs(out[i * 2 + 1] - expR);
        TASK_CHECK(dL <= 2e-5);
        TASK_CHECK(dR <= 2e-5);
        /* The two ends must remain distinct: not a duplicated mono layer. */
        TASK_CHECK(out[i * 2] != out[i * 2 + 1] || expL == expR);
        if (pos[i] < 16.0 && i > 0) folded = 1;
    }
    /* Confirm the position really crossed a loop boundary. */
    TASK_CHECK(folded);
    tsf_close(f);
    return 0;
}

/* MONO output is the half-sum of the two weighted ends, including a
 * non-centre balance derived from region pan. */
static int render_mono_balance(void)
{
    enum { N = 700 };
    struct task_font_ext e;
    static float out[N];
    static double pos[N];
    tsf* f;
    double ratio = expected_ratio(22050, 22050, 61, 60, 100);
    double gl, gr, p = 0.25;
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    zone_gen(&e, 17, (int)(p * 1000.0)); /* fine pan in permille */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_MONO, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, out, N, 0);

    gl = 0.0; gr = 0.0;
    balance_gains(p, &gl, &gr);
    position_sequence(pos, N, ratio, 1, 16.0, 200.0);
    for (i = 0; i < N; ++i)
    {
        double vL = sample_side(g_left, PCM_LEN, pos[i], 1, 16.0, 200.0);
        double vR = sample_side(g_right, PCM_LEN, pos[i], 1, 16.0, 200.0);
        double exp = 0.5 * (vL * gl + vR * gr);
        TASK_CHECK(fabs(out[i] - exp) <= 2e-5);
    }
    tsf_close(f);
    return 0;
}

/* The three output layouts must agree in content. */
static int render_three_layouts_consistent(void)
{
    enum { N = 600 };
    struct task_font_ext e;
    static float inter[N * 2], split[N * 2], mono[N];
    tsf* f;
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));

    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, inter, N, 0);
    tsf_close(f);

    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_UNWEAVED, 22050, 0);
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, split, N, 0);
    tsf_close(f);

    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_MONO, 22050, 0);
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, mono, N, 0);
    tsf_close(f);

    for (i = 0; i < N; ++i)
    {
        TASK_CHECK(fabs(inter[i * 2] - split[i]) <= 2e-5);
        TASK_CHECK(fabs(inter[i * 2 + 1] - split[N + i]) <= 2e-5);
        /* Centre balance unity: mono is the half sum of the two planes. */
        TASK_CHECK(fabs(mono[i] - 0.5 * (inter[i * 2] + inter[i * 2 + 1])) <= 2e-5);
    }
    return 0;
}

/* An active low-pass filter must keep independent z1/z2 history per end; the
 * left samples must not enter the right feedback path. */
static int render_independent_filter_history(void)
{
    enum { N = 900 };
    struct task_font_ext e;
    static float out[N * 2];
    static double pos[N];
    tsf* f;
    double ratio = expected_ratio(22050, 22050, 61, 60, 100);
    double Fc, QInv, gl, gr;
    struct test_biquad bl, br;
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    zone_gen(&e, 8, 10000);            /* active low-pass cutoff */
    zone_gen(&e, 9, 0);                /* Q = 0 dB -> QInv = 1 */
    zone_gen(&e, 17, -300);            /* non-centre balance, left louder */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, out, N, 0);

    /* QInv = 1 / 10^(Qdb/20), Qdb = initialFilterQ/10 = 0. */
    QInv = 1.0 / pow(10.0, 0.0 / 20.0);
    Fc = cents_to_hz(10000.0) / 22050.0;
    biquad_setup(&bl, Fc, QInv);
    biquad_setup(&br, Fc, QInv);
    balance_gains(-0.3, &gl, &gr);
    position_sequence(pos, N, ratio, 1, 16.0, 200.0);
    for (i = 0; i < N; ++i)
    {
        double vL = sample_side(g_left, PCM_LEN, pos[i], 1, 16.0, 200.0);
        double vR = sample_side(g_right, PCM_LEN, pos[i], 1, 16.0, 200.0);
        double fL = biquad_process(&bl, vL);
        double fR = biquad_process(&br, vR);
        TASK_CHECK(fabs(out[i * 2] - fL * gl) <= 2e-5);
        TASK_CHECK(fabs(out[i * 2 + 1] - fR * gr) <= 2e-5);
    }
    tsf_close(f);
    return 0;
}

/* Region pan and channel pan offset merge into a single stereo balance p in
 * [-0.5, 0.5]. With region pan 0 and channel pan 0.5 the merge gives a left-
 * attenuated balance, while the actual sample content must stay left/right. */
static int render_region_and_channel_pan_merge(void)
{
    enum { N = 500 };
    struct task_font_ext e;
    static float out[N * 2];
    static double pos[N];
    tsf* f;
    double ratio = expected_ratio(22050, 22050, 61, 60, 100);
    double p, gl, gr;
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_channel_set_presetindex(f, 0, 0));
    /* tsf_channel_set_pan takes pan - 0.5 internally, so 1.0 => panOffset +0.5. */
    TASK_CHECK(tsf_channel_set_pan(f, 0, 1.0f));
    TASK_CHECK(tsf_channel_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, out, N, 0);

    p = 0.0 + 0.5; /* region pan + channel panOffset, clamped */
    if (p > 0.5) p = 0.5;
    balance_gains(p, &gl, &gr);
    position_sequence(pos, N, ratio, 1, 16.0, 200.0);
    for (i = 0; i < N; ++i)
    {
        double expL = sample_side(g_left, PCM_LEN, pos[i], 1, 16.0, 200.0) * gl;
        double expR = sample_side(g_right, PCM_LEN, pos[i], 1, 16.0, 200.0) * gr;
        TASK_CHECK(fabs(out[i * 2] - expL) <= 2e-5);
        TASK_CHECK(fabs(out[i * 2 + 1] - expR) <= 2e-5);
    }
    tsf_close(f);
    return 0;
}

/* Plain mono samples keep their old interpolation neighbour at the play-range
 * end (no clamping change) so the mono interface is untouched. */
static int render_mono_sample_unchanged(void)
{
    enum { N = 300 };
    struct task_font_ext e;
    static float out[N];
    static double pos[N];
    tsf* f;
    double ratio;
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memset(&e, 0, sizeof(e));
    e.sampleCount = 1;
    e.pcmLen = POOL_SHORTS;
    e.pcm = g_pool;
    e.specs[0].start = 0; e.specs[0].end = PCM_LEN; e.specs[0].startLoop = 16; e.specs[0].endLoop = 216;
    e.specs[0].sampleRate = 22050; e.specs[0].originalPitch = 60;
    e.specs[0].sampleType = 1;
    e.zoneCount = 1;
    e.zones[0].sample = 0; e.zones[0].loopMode = 1;
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_MONO, 22050, 0);
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, out, N, 0);
    ratio = expected_ratio(22050, 22050, 61, 60, 100);
    position_sequence(pos, N, ratio, 1, 16.0, 200.0);
    for (i = 0; i < N; ++i)
        TASK_CHECK(fabs(out[i] - sample_side(g_left, PCM_LEN, pos[i], 1, 16.0, 200.0)) <= 2e-5);
    tsf_close(f);
    return 0;
}

/* Two zones referencing the same pair are independent timbres, not deduped,
 * and their summed output saturates in the short path. */
static int render_short_quantized_and_saturates(void)
{
    enum { N = 400 };
    struct task_font_ext e;
    static short out[N * 2];
    static float fout[N * 2];
    static double pos[N];
    tsf* f;
    double ratio = expected_ratio(22050, 22050, 61, 60, 100);
    int i, sat = 0;
    build_signals(); clear_pool();
    /* Louder signals so two layers exceed full scale. */
    for (i = 0; i < PCM_LEN; ++i) { g_left[i] = 30000 - (i % 7) * 500; g_right[i] = -(28000 - (i % 5) * 400); }
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    /* Duplicate the zone: same sample id, same generators -> independent layer. */
    e.zoneCount = 2;
    e.zones[1] = e.zones[0];

    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 8));
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 2); /* not deduped */
    tsf_render_float(f, fout, N, 0);
    tsf_close(f);

    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 8));
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    tsf_render_short(f, out, N, 0);
    tsf_close(f);

    position_sequence(pos, N, ratio, 1, 16.0, 200.0);
    for (i = 0; i < N; ++i)
    {
        double vL = 2.0 * sample_side(g_left, PCM_LEN, pos[i], 1, 16.0, 200.0);
        double vR = 2.0 * sample_side(g_right, PCM_LEN, pos[i], 1, 16.0, 200.0);
        int eL = (int)(vL * 32767.5), eR = (int)(vR * 32767.5);
        if (eL < -32768) eL = -32768; else if (eL > 32767) eL = 32767;
        if (eR < -32768) eR = -32768; else if (eR > 32767) eR = 32767;
        if (abs((int)out[i * 2] - eL) > 2) return 1;
        if (abs((int)out[i * 2 + 1] - eR) > 2) return 1;
        if (out[i * 2] == 32767 || out[i * 2] == -32768 ||
            out[i * 2 + 1] == 32767 || out[i * 2 + 1] == -32768) sat = 1;
        /* float path remains consistent with the same source. */
        TASK_CHECK(fabs(fout[i * 2] - vL) <= 2e-5);
    }
    TASK_CHECK(sat);
    return 0;
}

/* A non-looping linked pair: the interpolation neighbour at the play-range end
 * clamps to this side's last actual sample on both planes. Rendering begins at
 * the very start so every frame up to and past the end is checked. */
static int render_nonloop_end_clamp(void)
{
    enum { N = 280 };
    struct task_font_ext e;
    static float out[N * 2];
    tsf* f;
    double ratio = expected_ratio(22050, 22050, 60, 60, 100);
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 0); /* loop mode none */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    tsf_render_float(f, out, N, 0);
    TASK_CHECK(tsf_active_voice_count(f) == 0); /* played to the end */
    for (i = 0; i < PCM_LEN; ++i)
    {
        double p = i * ratio;
        double expL = sample_side(g_left, PCM_LEN, p, 0, 0.0, 0.0);
        double expR = sample_side(g_right, PCM_LEN, p, 0, 0.0, 0.0);
        TASK_CHECK(fabs(out[i * 2] - expL) <= 2e-5);
        TASK_CHECK(fabs(out[i * 2 + 1] - expR) <= 2e-5);
    }
    tsf_close(f);
    return 0;
}

/* Address generators applied to a linked pair must shrink both ends by the same
 * signed amount computed in a wide domain. Here start +8 (fine) and loop start
 * +4 move the play range and loop for both samples identically. */
static int pairing_applies_generators_to_both_ends(void)
{
    enum { N = 400 };
    struct task_font_ext e;
    static float out[N * 2];
    static double pos[N];
    tsf* f;
    double ratio;
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    /* loop [16,216), play [0,256) */
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    zone_gen(&e, 0, 8);   /* StartAddrsOffset +8 (both ends) */
    zone_gen(&e, 2, 4);   /* StartloopAddrsOffset +4 (both ends) */
    zone_gen(&e, 1, -2);  /* EndAddrsOffset -2 -> shorter play range */
    zone_gen(&e, 3, -2);  /* EndloopAddrsOffset -2 -> shorter loop */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 61, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 1);
    tsf_render_float(f, out, N, 0);

    /* Independently derive the shifted, shortened geometry: reading starts at
     * raw offset 8, the loop is raw [20,214) (exclusive end), so the last loop
     * sample is 213 and the loop period is 194. The shared read pointer is kept
     * in coordinates relative to the sample start, so it folds at 12+194=206.
     * `sample_side` then evaluates at the raw-offset position 8+pos. */
    ratio = expected_ratio(22050, 22050, 61, 60, 100);
    position_sequence(pos, N, ratio, 1, 12.0, 194.0);
    for (i = 0; i < N; ++i)
    {
        double rel = 8.0 + pos[i];
        double expL = sample_side(g_left, PCM_LEN, rel, 1, 20.0, 194.0);
        double expR = sample_side(g_right, PCM_LEN, rel, 1, 20.0, 194.0);
        TASK_CHECK(fabs(out[i * 2] - expL) <= 2e-5);
        TASK_CHECK(fabs(out[i * 2 + 1] - expR) <= 2e-5);
    }
    tsf_close(f);
    return 0;
}

/* An oversized address offset must not wrap around. The offset is so large it
 * drives the shortened range past the source end, so the region is dropped
 * rather than becoming a valid-looking huge range. */
static int pairing_rejects_overflowing_offset(void)
{
    struct task_font_ext e;
    tsf* f;
    build_signals(); clear_pool();
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    zone_gen(&e, 4, 32767); /* StartAddrsCoarseOffset: +32767<<15 shorts */
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 4));
    TASK_CHECK(tsf_note_on(f, 0, 60, 1.0f));
    TASK_CHECK(tsf_active_voice_count(f) == 0);
    tsf_close(f);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Lifetime tests                                                            */
/* ------------------------------------------------------------------------- */

static int lifetime_no_allocations_after_setup(void)
{
    enum { N = 512 };
    struct task_font_ext e;
    static float out[N * 2];
    tsf* f;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);
    f = load_ext(&e);
    TASK_CHECK(f != NULL);
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(f, 8));
    /* Prebuild the channel infrastructure. */
    TASK_CHECK(tsf_channel_set_presetindex(f, 0, 0));

    task_alloc_reset();
    TASK_CHECK(tsf_channel_note_on(f, 0, 61, 1.0f));
    tsf_render_float(f, out, N, 0);
    TASK_CHECK(task_alloc_calls() == 0);
    TASK_CHECK(tsf_active_voice_count(f) >= 1);
    tsf_close(f);
    return 0;
}

/* tsf_copy shares the read-only font and copies no active voices; closing the
 * original leaves the copy able to play independently. */
static int lifetime_copy_after_original_close(void)
{
    enum { N = 900 };
    struct task_font_ext e;
    static float copyout[N * 2], refout[N * 2];
    tsf* original;
    tsf* copy;
    tsf* reference;
    int i;
    build_signals(); clear_pool();
    memcpy(g_pool, g_left, sizeof(g_left));
    memcpy(g_pool + PCM_LEN, g_right, sizeof(g_right));
    setup_pair(&e, PCM_LEN, 200, PCM_LEN, 0, 1);

    original = load_ext(&e);
    TASK_CHECK(original != NULL);
    tsf_set_output(original, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(original, 4));
    TASK_CHECK(tsf_note_on(original, 0, 61, 1.0f));
    copy = tsf_copy(original);
    TASK_CHECK(copy != NULL);
    /* A copy carries no active voices. */
    TASK_CHECK(tsf_active_voice_count(copy) == 0);
    tsf_close(original);

    tsf_set_output(copy, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(copy, 4));
    TASK_CHECK(tsf_note_on(copy, 0, 61, 1.0f));
    tsf_render_float(copy, copyout, N, 0);
    tsf_close(copy);

    /* Reference playback from a freshly loaded instance gives identical audio. */
    reference = load_ext(&e);
    TASK_CHECK(reference != NULL);
    tsf_set_output(reference, TSF_STEREO_INTERLEAVED, 22050, 0);
    TASK_CHECK(tsf_set_max_voices(reference, 4));
    TASK_CHECK(tsf_note_on(reference, 0, 61, 1.0f));
    tsf_render_float(reference, refout, N, 0);
    tsf_close(reference);

    /* Equal to a fresh instance, and to the PCM-derived expectation, so this
     * does not treat any library output as a golden reference. */
    {
        static double pos[N];
        double ratio = expected_ratio(22050, 22050, 61, 60, 100);
        position_sequence(pos, N, ratio, 1, 16.0, 200.0);
        for (i = 0; i < N * 2; ++i)
            TASK_CHECK(fabs(copyout[i] - refout[i]) <= 2e-5);
        for (i = 0; i < N; ++i)
        {
            double expL = sample_side(g_left, PCM_LEN, pos[i], 1, 16.0, 200.0);
            double expR = sample_side(g_right, PCM_LEN, pos[i], 1, 16.0, 200.0);
            TASK_CHECK(fabs(copyout[i * 2] - expL) <= 2e-5);
            TASK_CHECK(fabs(copyout[i * 2 + 1] - expR) <= 2e-5);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Case registry                                                             */
/* ------------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    { "pairing", "left_only_links", pairing_left_only_links },
    { "pairing", "right_only_links", pairing_right_only_links },
    { "pairing", "far_apart_storage", pairing_far_apart_storage },
    { "pairing", "rejects_bad_link", pairing_rejects_bad_link },
    { "pairing", "rejects_type_mismatch", pairing_rejects_type_mismatch },
    { "pairing", "rejects_tuning_mismatch", pairing_rejects_tuning_mismatch },
    { "pairing", "rejects_rom", pairing_rejects_rom },
    { "pairing", "rejects_length_mismatch", pairing_rejects_length_mismatch },
    { "pairing", "rejects_relative_loop_mismatch", pairing_rejects_relative_loop_mismatch },
    { "pairing", "rejects_eos_as_end", pairing_rejects_eos_as_end },
    { "pairing", "bad_pair_keeps_other_layers", pairing_bad_pair_keeps_other_layers },
    { "pairing", "applies_generators_to_both_ends", pairing_applies_generators_to_both_ends },
    { "pairing", "rejects_overflowing_offset", pairing_rejects_overflowing_offset },
    { "render", "interleaved_fractional_loop", render_interleaved_fractional_loop },
    { "render", "mono_balance", render_mono_balance },
    { "render", "three_layouts_consistent", render_three_layouts_consistent },
    { "render", "independent_filter_history", render_independent_filter_history },
    { "render", "mono_sample_unchanged", render_mono_sample_unchanged },
    { "render", "short_quantized_and_saturates", render_short_quantized_and_saturates },
    { "render", "nonloop_end_clamp", render_nonloop_end_clamp },
    { "render", "region_and_channel_pan_merge", render_region_and_channel_pan_merge },
    { "lifetime", "no_allocations_after_setup", lifetime_no_allocations_after_setup },
    { "lifetime", "copy_after_original_close", lifetime_copy_after_original_close },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
