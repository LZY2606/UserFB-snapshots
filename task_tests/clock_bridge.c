// Tests for the optional clock-tracking mode of the resampler.
//
// Three groups are registered: "clock" (estimator, servo and event handling),
// "audio" (PCM against an independent direct convolution) and "streaming"
// (chunking / output-capacity invariance and guard conditions). The reference
// clock below is written from the specification, not from the implementation:
// it differences the counters in the integer domain, fits s on p with an
// intercept, clamps the slope and advances U and q with the per-frame servo
// formula.

#include "task_support/check.h"
#include "clock_bridge.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define TAPS   64
#define NFILT  128
#define FLAGS  (SUBSAMPLE_INTERPOLATE | BLACKMAN_HARRIS)

#define FSRC   48000.0
#define FDST   44100.0
#define Q0     (FSRC / FDST)

#define MAXOUT 12000

////////////////////////////////////////////////////////////////////////////////////////////////////
// reference clock

typedef struct {
    uint64_t k, s, p;
} RefObs;

typedef struct {
    double q0, q, T, U;
    uint64_t produced;
    int nev, next;
    RefObs ev[CLOCK_BRIDGE_MAX_EVENTS];
    RefObs win[CLOCK_BRIDGE_WINDOW];
    int nwin;
    uint64_t base_s, base_p;
} RefClock;

static double ref_target (RefClock *r)
{
    int n = r->nwin, i;
    double ms = 0.0, mp = 0.0, sxy = 0.0, sxx = 0.0, slope;

    if (n < 2)
        return r->q0;

    // counters are differenced in the integer domain before any float math

    for (i = 0; i < n; ++i) {
        ms += (double) (r->win [i].s - r->base_s);
        mp += (double) (r->win [i].p - r->base_p);
    }

    ms /= n;
    mp /= n;

    for (i = 0; i < n; ++i) {
        double ds = (double) (r->win [i].s - r->base_s) - ms;
        double dp = (double) (r->win [i].p - r->base_p) - mp;

        sxy += ds * dp;
        sxx += dp * dp;
    }

    if (sxx == 0.0)
        return r->q0;

    slope = sxy / sxx;

    if (slope > r->q0 * 1.002)
        slope = r->q0 * 1.002;
    else if (slope < r->q0 * 0.998)
        slope = r->q0 * 0.998;

    return slope;
}

static void ref_push (RefClock *r, uint64_t s, uint64_t p)
{
    int evicted = 0;

    if (r->nwin == CLOCK_BRIDGE_WINDOW) {
        memmove (r->win, r->win + 1, (CLOCK_BRIDGE_WINDOW - 1) * sizeof (RefObs));
        r->nwin--;
        evicted = 1;
    }

    r->win [r->nwin].s = s;
    r->win [r->nwin].p = p;
    r->nwin++;

    // after an eviction a fresh integer base is chosen

    if (r->nwin == 1 || evicted ||
        s - r->base_s > ((uint64_t) 1 << 40) || p - r->base_p > ((uint64_t) 1 << 40) ||
        r->win [0].s < r->base_s || r->win [0].p < r->base_p) {
        r->base_s = r->win [0].s;
        r->base_p = r->win [0].p;
    }
}

static void ref_setup (RefClock *r, double q0, const RefObs *ev, int nev)
{
    memset (r, 0, sizeof (*r));
    r->q0 = r->q = r->T = q0;
    r->nev = nev;
    memcpy (r->ev, ev, nev * sizeof (RefObs));
}

// apply any observations due at frame k and recompute the target

static void ref_begin (RefClock *r, uint64_t k)
{
    while (r->next < r->nev && r->ev [r->next].k <= k) {
        ref_push (r, r->ev [r->next].s, r->ev [r->next].p);
        r->next++;
    }

    r->T = ref_target (r);
}

// per-frame servo: U_next = U_k + q_next, q_next = q + clamp((T-q)/64)

static void ref_advance (RefClock *r)
{
    double step = (r->T - r->q) / 64.0;
    double limit = r->q0 * 1e-6;

    if (step > limit)
        step = limit;
    else if (step < -limit)
        step = -limit;

    r->q += step;
    r->U += r->q;
    r->produced++;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// configuration helpers and reference convolution

static double test_lowpass (double fs, double fd)
{
    double lp = fd / (fs * 1.002);

    return lp < 1.0 ? lp : 1.0;
}

static ClockBridge *make_bridge (int nch, double fs, double fd, int cap,
    const RefObs *ev, int nev)
{
    ClockBridge *b = clockBridgeCreate (nch, TAPS, NFILT, fs, fd, FLAGS, cap);
    int i;

    if (!b)
        return NULL;

    for (i = 0; i < nev; ++i)
        if (clockBridgeSubmit (b, ev [i].k, ev [i].s, ev [i].p)) {
            clockBridgeFree (b);
            return NULL;
        }

    return b;
}

// resubmit a schedule to an already-created bridge (used after reset)

static int make_bridge_events (ClockBridge *b, const RefObs *ev, int nev)
{
    int i;

    for (i = 0; i < nev; ++i)
        if (clockBridgeSubmit (b, ev [i].k, ev [i].s, ev [i].p))
            return 1;

    return 0;
}

// Independent direct convolution using the resampler's original coefficient
// tables. "U" is the reference nominal source position; the startup delay is
// retained exactly as in the legacy path (offset = taps/2 + U).

static double ref_conv (Resample *tbl, const double *in, long nin, double U)
{
    int taps = tbl->numTaps, nf = tbl->numFilters;
    double off = (double) (taps / 2) + U;
    double frac = off - floor (off);
    double fv = frac * nf;
    int fi = (int) floor (fv);
    double f2 = fv - fi;
    long a = (long) floor (off);
    double c0 = 0.0, c1 = 0.0;
    int t;

    if (fi < 0) fi = 0;
    if (fi >= nf) fi = nf - 1;

    for (t = 0; t < taps; ++t) {
        long idx = a - taps / 2 + 1 + t - taps;
        double x = (idx >= 0 && idx < nin) ? in [idx] : 0.0;

        c0 += tbl->filters [fi][t] * x;
        c1 += tbl->filters [fi + 1][t] * x;
    }

    return c0 * (1.0 - f2) + c1 * f2;
}

static int compare_query (ClockBridge *b, RefClock *r, uint64_t expect_frames)
{
    ClockBridgeInfo info;

    if (clockBridgeQuery (b, &info))
        return 1;

    if (info.frames != expect_frames)
        return 2;

    if (fabs (((double) info.position_int + info.position_frac) - r->U) > 1e-9)
        return 3;

    if (info.position_frac < 0.0 || info.position_frac >= 1.0)
        return 4;

    if (fabs (info.q - r->q) > 1e-12)
        return 5;

    if (fabs (info.target - r->T) > 1e-12)
        return 6;

    if (info.window != r->nwin)
        return 7;

    return 0;
}

// Process one mono frame at a time and check the reported clock state after
// every frame. Returns 0 on success.

static int run_clock (ClockBridge *b, RefClock *r, const double *in, int nin, int frames)
{
    int fed = 0, k;

    for (k = 0; k < frames; ++k) {
        artsample_t sample;
        const artsample_t *ip [1];
        artsample_t *out [1];
        ResampleResult res;

        ref_begin (r, (uint64_t) k);

        ip [0] = (artsample_t *) (in + fed);
        out [0] = &sample;
        res = clockBridgeProcess (b, ip, nin - fed, out, 1);

        if (res.output_generated != 1)
            return 1;

        fed += res.input_used;
        ref_advance (r);

        if (compare_query (b, r, (uint64_t) k + 1))
            return 2;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// group: clock

// helper used by the clamp case: drive enough frames to apply the second
// observation, then check the clamped target directly

static int run_clock_silent_drain (ClockBridge *b, const double *in, int nin, int frames)
{
    ClockBridgeInfo info;
    int fed = 0, k;

    for (k = 0; k < frames; ++k) {
        artsample_t sample;
        const artsample_t *ip [1];
        artsample_t *out [1];
        ResampleResult res;

        ip [0] = (artsample_t *) (in + fed);
        out [0] = &sample;
        res = clockBridgeProcess (b, ip, nin - fed, out, 1);

        if (res.output_generated != 1)
            return 1;

        fed += res.input_used;
    }

    if (clockBridgeQuery (b, &info))
        return 2;

    if (info.window < 2)
        return 3;

    return 0;
}

static int case_clock_defaults (void)
{
    ClockBridge *b;
    ClockBridgeInfo info;

    // invalid configurations must be refused

    TASK_CHECK (clockBridgeCreate (0, TAPS, NFILT, 48000, 44100, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (9, TAPS, NFILT, 48000, 44100, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, 66, NFILT, 48000, 44100, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, 0, 48000, 44100, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, NFILT, 7999, 44100, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, NFILT, 48000.5, 44100, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, NFILT, 192001, 44100, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, NFILT, 8000, 40000, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, NFILT, 48000, 11000, FLAGS, 4) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, NFILT, 48000, 44100, FLAGS, 0) == NULL);
    TASK_CHECK (clockBridgeCreate (2, TAPS, NFILT, 48000, 44100, FLAGS, 65) == NULL);

    b = clockBridgeCreate (2, TAPS, NFILT, FSRC, FDST, FLAGS, 4);
    TASK_CHECK (b != NULL);
    TASK_CHECK (clockBridgeQuery (b, &info) == 0);

    TASK_CHECK (info.frames == 0);
    TASK_CHECK (info.position_int == 0);
    TASK_CHECK (info.position_frac == 0.0);
    TASK_CHECK (fabs (info.q - Q0) < 1e-15);
    TASK_CHECK (fabs (info.target - Q0) < 1e-15);
    TASK_CHECK (info.window == 0);

    clockBridgeFree (b);
    return 0;
}

static int case_clock_event_timing (void)
{
    static double in [4000];
    RefObs ev [2];
    RefClock ref;
    ClockBridge *b;
    ClockBridgeInfo info;
    int i;

    for (i = 0; i < 4000; ++i)
        in [i] = 0.4 * sin (i * 0.021) + 0.1 * cos (i * 0.077);

    ev [0] = (RefObs) { 0, (uint64_t) 1 << 60, (uint64_t) 1 << 60 };
    ev [1] = (RefObs) { 25, ((uint64_t) 1 << 60) + 20000, ((uint64_t) 1 << 60) + 18000 };

    b = make_bridge (1, FSRC, FDST, 4, ev, 2);
    TASK_CHECK (b != NULL);
    ref_setup (&ref, Q0, ev, 2);

    // frame 0 is not yet produced; its event must apply when it is

    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (info.window == 0);

    TASK_CHECK (run_clock (b, &ref, in, 4000, 40) == 0);

    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (info.window == 2);
    TASK_CHECK (ref.U > 40.0 * 0.998 * Q0 && ref.U < 40.0 * 1.002 * Q0 + 2.0);

    clockBridgeFree (b);
    return 0;
}

static int case_clock_reject (void)
{
    static double in [4000];
    RefObs ev [2];
    RefClock ref;
    ClockBridge *b;
    ClockBridgeInfo before, after;
    uint64_t base = (uint64_t) 1 << 60;
    int i;

    for (i = 0; i < 4000; ++i)
        in [i] = 0.3 * sin (i * 0.013) + 0.2 * cos (i * 0.029);

    ev [0] = (RefObs) { 0, base, base };
    ev [1] = (RefObs) { 10, base + 54000, base + 49600 };

    b = make_bridge (1, FSRC, FDST, 2, ev, 2);
    TASK_CHECK (b != NULL);
    ref_setup (&ref, Q0, ev, 2);

    TASK_CHECK (clockBridgeQuery (b, &before) == 0);

    // duplicate k, non-increasing s and an oversized step are all refused and
    // must not disturb the queue, the window or the audio state

    TASK_CHECK (clockBridgeSubmit (b, 10, base + 100000, base + 90000) != 0);
    TASK_CHECK (clockBridgeSubmit (b, 20, base, base) != 0);
    TASK_CHECK (clockBridgeSubmit (b, 20, base + 54000 + ((uint64_t) 1 << 32) + 1, base + 60000) != 0);
    TASK_CHECK (clockBridgeSubmit (b, 20, base + 54001, base + 49600) != 0);

    TASK_CHECK (clockBridgeQuery (b, &after) == 0);
    TASK_CHECK (before.frames == after.frames && before.position_int == after.position_int &&
        before.position_frac == after.position_frac && before.q == after.q &&
        before.target == after.target && before.window == after.window);

    // queue is full at two entries

    TASK_CHECK (clockBridgeSubmit (b, 30, base + 108000, base + 99200) != 0);

    TASK_CHECK (clockBridgeQuery (b, &after) == 0);
    TASK_CHECK (after.frames == 0 && after.window == 0);

    // processing still works and uses exactly the accepted observations

    TASK_CHECK (run_clock (b, &ref, in, 4000, 60) == 0);

    // an observation for an already-produced frame is refused

    TASK_CHECK (clockBridgeQuery (b, &after) == 0);
    TASK_CHECK (clockBridgeSubmit (b, after.frames - 1, base + 200000, base + 180000) != 0);
    TASK_CHECK (clockBridgeSubmit (b, after.frames + 5, base + 200001, base + 180001) == 0);

    clockBridgeFree (b);
    return 0;
}

static int case_clock_fit_2p60 (void)
{
    static double in [4000];
    RefObs ev [12];
    RefClock ref;
    ClockBridge *b;
    uint64_t base = ((uint64_t) 1 << 60) + 12345;
    int i;

    for (i = 0; i < 4000; ++i)
        in [i] = 0.25 * sin (i * 0.037) + 0.15 * sin (i * 0.0113);

    // twelve observations: the window must stay at eight and the fit must
    // follow the most recent pairs; the counters sit near 2^60 and the window
    // eviction must reselect the integer origin

    for (i = 0; i < 12; ++i) {
        uint64_t p = base + (uint64_t) i * 4410;
        uint64_t s = base + (uint64_t) i * 4800;

        if (i < 6)
            s += (uint64_t) i * 3;      // positive drift
        else
            s -= (uint64_t) (i - 5) * 3; // negative drift

        ev [i] = (RefObs) { (uint64_t) i * 90, s, p };
    }

    b = make_bridge (1, FSRC, FDST, CLOCK_BRIDGE_MAX_EVENTS, ev, 12);
    TASK_CHECK (b != NULL);
    ref_setup (&ref, Q0, ev, 12);

    TASK_CHECK (run_clock (b, &ref, in, 4000, 1000) == 0);
    TASK_CHECK (ref.nwin == CLOCK_BRIDGE_WINDOW);

    clockBridgeFree (b);
    return 0;
}

static int case_clock_clamp (void)
{
    static double in [2000];
    ClockBridgeInfo info;
    RefObs evp [2], evn [2];
    ClockBridge *b;
    uint64_t base = (uint64_t) 1 << 60;
    int i;

    for (i = 0; i < 2000; ++i)
        in [i] = sin (i * 0.05);

    // slope far above the allowed range clamps to q0 * 1.002

    evp [0] = (RefObs) { 0, base, base };
    evp [1] = (RefObs) { 30, base + 2200, base + 2000 };

    b = make_bridge (1, FSRC, FDST, 4, evp, 2);
    TASK_CHECK (b != NULL);
    TASK_CHECK (run_clock_silent_drain (b, in, 2000, 40) == 0);
    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (fabs (info.target - Q0 * 1.002) <= 1e-12);
    clockBridgeFree (b);

    // slope far below clamps to q0 * 0.998

    evn [0] = (RefObs) { 0, base, base };
    evn [1] = (RefObs) { 30, base + 2000, base + 2000 };

    b = make_bridge (1, FSRC, FDST, 4, evn, 2);
    TASK_CHECK (b != NULL);
    TASK_CHECK (run_clock_silent_drain (b, in, 2000, 40) == 0);
    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (fabs (info.target - Q0 * 0.998) <= 1e-12);
    clockBridgeFree (b);

    return 0;
}

static int case_clock_reset (void)
{
    static double in [2000];
    RefObs ev [3];
    RefClock ref;
    ClockBridge *b;
    ClockBridgeInfo info;
    int i, a, c;

    for (i = 0; i < 2000; ++i)
        in [i] = 0.2 * sin (i * 0.019);

    ev [0] = (RefObs) { 0, (uint64_t) 1 << 60, (uint64_t) 1 << 60 };
    ev [1] = (RefObs) { 50, ((uint64_t) 1 << 60) + 54000, ((uint64_t) 1 << 60) + 49600 };
    ev [2] = (RefObs) { 120, ((uint64_t) 1 << 60) + 130000, ((uint64_t) 1 << 60) + 119400 };

    b = make_bridge (1, FSRC, FDST, 8, ev, 3);
    TASK_CHECK (b != NULL);
    ref_setup (&ref, Q0, ev, 3);

    a = run_clock (b, &ref, in, 2000, 200);
    TASK_CHECK (a == 0);

    clockBridgeReset (b);
    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (info.frames == 0 && info.position_int == 0 && info.position_frac == 0.0);
    TASK_CHECK (fabs (info.q - Q0) < 1e-15 && fabs (info.target - Q0) < 1e-15);
    TASK_CHECK (info.window == 0);

    // after reset the same schedule must reproduce the same states; pending
    // observations are discarded by the reset, so they are resubmitted

    TASK_CHECK (make_bridge_events (b, ev, 3) == 0);
    ref_setup (&ref, Q0, ev, 3);
    c = run_clock (b, &ref, in, 2000, 200);
    TASK_CHECK (c == 0);
    TASK_CHECK (a == c);

    clockBridgeFree (b);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// group: audio

static double audio_in_a [3][4000];
static double audio_out [3][MAXOUT];

// build the reference cursor, run the whole block and compare PCM and cursor

static int compare_audio_block (ClockBridge *b, const RefObs *ev, int nev, int nch, int nin, int cap,
    Resample *tbl, double *maxerr_out)
{
    RefClock ref;
    ResampleResult res;
    const artsample_t *ip [3];
    artsample_t *op [3];
    double maxerr = 0.0;
    int c, n;

    ref_setup (&ref, Q0, ev, nev);

    for (c = 0; c < nch; ++c) {
        ip [c] = (artsample_t *) audio_in_a [c];
        op [c] = (artsample_t *) audio_out [c];
    }

    res = clockBridgeProcess (b, ip, nin, op, cap);

    if ((int) res.output_generated >= MAXOUT)
        return 1;

    for (n = 0; n < (int) res.output_generated; ++n) {
        ref_begin (&ref, (uint64_t) n);

        for (c = 0; c < nch; ++c) {
            double refv = ref_conv (tbl, audio_in_a [c], nin, ref.U);
            double d = fabs (refv - audio_out [c][n]);

            if (d > maxerr)
                maxerr = d;
        }

        ref_advance (&ref);
    }

    if (compare_query (b, &ref, (uint64_t) res.output_generated))
        return 2;

    *maxerr_out = maxerr;
    return 0;
}

static int case_audio_impulse (void)
{
    RefObs ev [2];
    RefClock ref;
    Resample *tbl;
    ClockBridge *b;
    int i;

    for (i = 0; i < 4000; ++i)
        audio_in_a [0][i] = (i == 1500) ? 1.0 : 0.0;

    ev [0] = (RefObs) { 0, (uint64_t) 1 << 60, (uint64_t) 1 << 60 };
    ev [1] = (RefObs) { 200, ((uint64_t) 1 << 60) + 200000, ((uint64_t) 1 << 60) + 184000 };

    tbl = resampleInit (1, TAPS, NFILT, test_lowpass (FSRC, FDST), FLAGS);
    TASK_CHECK (tbl != NULL);

    b = make_bridge (1, FSRC, FDST, 8, ev, 2);
    TASK_CHECK (b != NULL);

    {
        double maxerr = 0.0;

        TASK_CHECK (compare_audio_block (b, ev, 2, 1, 4000, MAXOUT, tbl, &maxerr) == 0);
        TASK_CHECK (maxerr <= 1e-9);
    }

    // query cursor is close to the reference after the run

    {
        ClockBridgeInfo info;

        ref_setup (&ref, Q0, ev, 2);
        clockBridgeQuery (b, &info);
        TASK_CHECK ((double) info.position_int + info.position_frac > 0.0);
    }

    clockBridgeFree (b);
    resampleFree (tbl);
    return 0;
}

static int case_audio_tones (void)
{
    RefObs ev [3];
    Resample *tbl;
    ClockBridge *b;
    double maxerr = 0.0;
    int i;

    // aperiodic two-tone signal plus a weak high component

    for (i = 0; i < 4000; ++i)
        audio_in_a [0][i] = 0.35 * sin (i * 0.031) + 0.22 * sin (i * 0.0173) +
            0.09 * cos (i * 0.211) + 0.05 * sin (i * 0.00051);

    ev [0] = (RefObs) { 0, (uint64_t) 1 << 61, (uint64_t) 1 << 61 };
    ev [1] = (RefObs) { 300, ((uint64_t) 1 << 61) + 300000, ((uint64_t) 1 << 61) + 275600 };
    ev [2] = (RefObs) { 900, ((uint64_t) 1 << 61) + 900500, ((uint64_t) 1 << 61) + 827000 };

    tbl = resampleInit (1, TAPS, NFILT, test_lowpass (FSRC, FDST), FLAGS);
    TASK_CHECK (tbl != NULL);
    b = make_bridge (1, FSRC, FDST, 8, ev, 3);
    TASK_CHECK (b != NULL);

    TASK_CHECK (compare_audio_block (b, ev, 3, 1, 4000, MAXOUT, tbl, &maxerr) == 0);
    TASK_CHECK (maxerr <= 1e-9);

    clockBridgeFree (b);
    resampleFree (tbl);
    return 0;
}

static int case_audio_antiphase (void)
{
    RefObs ev [2];
    Resample *tbl;
    ClockBridge *b;
    double maxerr = 0.0;
    int i;

    for (i = 0; i < 4000; ++i) {
        audio_in_a [0][i] = 0.31 * sin (i * 0.023) + 0.17 * cos (i * 0.067);
        audio_in_a [1][i] = -audio_in_a [0][i];
    }

    ev [0] = (RefObs) { 0, (uint64_t) 1 << 60, (uint64_t) 1 << 60 };
    ev [1] = (RefObs) { 400, ((uint64_t) 1 << 60) + 430000, ((uint64_t) 1 << 60) + 395000 };

    tbl = resampleInit (2, TAPS, NFILT, test_lowpass (FSRC, FDST), FLAGS);
    TASK_CHECK (tbl != NULL);
    b = make_bridge (2, FSRC, FDST, 8, ev, 2);
    TASK_CHECK (b != NULL);

    TASK_CHECK (compare_audio_block (b, ev, 2, 2, 4000, MAXOUT, tbl, &maxerr) == 0);
    TASK_CHECK (maxerr <= 1e-9);

    clockBridgeFree (b);
    resampleFree (tbl);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// group: streaming

#define SN 3000
#define SCH 3

static double stream_in [SCH][SN];
static double stream_in_i [SCH * SN];
static double stream_out [SCH][MAXOUT];
static double stream_out_i [SCH * MAXOUT];

static void make_stream_input (void)
{
    int i, c;

    for (i = 0; i < SN; ++i) {
        stream_in [0][i] = 0.3 * sin (i * 0.031) + 0.2 * cos (i * 0.071);
        stream_in [1][i] = 0.25 * sin (i * 0.013) + 0.11 * cos (i * 0.113);
        stream_in [2][i] = -stream_in [0][i];
    }

    for (i = 0; i < SN; ++i)
        for (c = 0; c < SCH; ++c)
            stream_in_i [i * SCH + c] = stream_in [c][i];
}

static int run_planar (ClockBridge *b, int chunk, int cap, double out [SCH][MAXOUT], unsigned *total_out)
{
    unsigned ri = 0, total = 0;

    for (;;) {
        int take = chunk;
        const artsample_t *ip [SCH];
        artsample_t *op [SCH];
        ResampleResult res;
        int c;

        if ((int) (SN - ri) < take)
            take = (int) (SN - ri);

        for (c = 0; c < SCH; ++c) {
            ip [c] = (artsample_t *) stream_in [c] + ri;
            op [c] = (artsample_t *) out [c] + total;
        }

        res = clockBridgeProcess (b, ip, take, op, cap);
        ri += res.input_used;
        total += res.output_generated;

        if (total >= MAXOUT)
            return 1;

        if (ri >= SN && res.output_generated == 0)
            break;

        if (res.input_used == 0 && res.output_generated == 0)
            break;
    }

    *total_out = total;
    return ri == SN ? 0 : 2;
}

static int run_interleaved (ClockBridge *b, int chunk, int cap, double *out, unsigned *total_out)
{
    unsigned ri = 0, total = 0;

    for (;;) {
        int take = chunk;
        ResampleResult res;

        if ((int) (SN - ri) < take)
            take = (int) (SN - ri);

        res = clockBridgeProcessInterleaved (b, (artsample_t *) stream_in_i + ri * SCH, take,
            (artsample_t *) out + total * SCH, cap);

        ri += res.input_used;
        total += res.output_generated;

        if (total >= MAXOUT)
            return 1;

        if (ri >= SN && res.output_generated == 0)
            break;

        if (res.input_used == 0 && res.output_generated == 0)
            break;
    }

    *total_out = total;
    return ri == SN ? 0 : 2;
}

static void stream_events (RefObs *ev, int *nev)
{
    uint64_t base = (uint64_t) 1 << 60;
    uint64_t p1 = 1764000;      // 400 output frames of playback clock

    ev [0] = (RefObs) { 0, base, base };
    ev [1] = (RefObs) { 400, base + (uint64_t) (p1 * Q0 * 1.001 + 0.5), base + p1 };
    *nev = 2;
}

static int compare_pcm (double a [SCH][MAXOUT], double b [SCH][MAXOUT], unsigned n)
{
    unsigned i;
    int c;

    for (c = 0; c < SCH; ++c)
        for (i = 0; i < n; ++i)
            if (fabs (a [c][i] - b [c][i]) > 1e-9)
                return 1;

    return 0;
}

static int case_stream_chunking (void)
{
    static double whole [SCH][MAXOUT], chunked [SCH][MAXOUT], small [SCH][MAXOUT];
    static double small_i [SCH][MAXOUT];
    RefObs ev [2];
    unsigned nwhole, nchunked, nsmall, ninter;
    ClockBridge *b;
    int nev;

    make_stream_input ();
    stream_events (ev, &nev);

    b = make_bridge (SCH, FSRC, FDST, 8, ev, nev);
    TASK_CHECK (b != NULL);
    TASK_CHECK (run_planar (b, SN, MAXOUT, whole, &nwhole) == 0);
    clockBridgeFree (b);

    b = make_bridge (SCH, FSRC, FDST, 8, ev, nev);
    TASK_CHECK (b != NULL);
    TASK_CHECK (run_planar (b, 113, MAXOUT, chunked, &nchunked) == 0);
    clockBridgeFree (b);

    b = make_bridge (SCH, FSRC, FDST, 8, ev, nev);
    TASK_CHECK (b != NULL);
    TASK_CHECK (run_planar (b, 7, 17, small, &nsmall) == 0);
    clockBridgeFree (b);

    TASK_CHECK (nwhole == nchunked);
    TASK_CHECK (nwhole == nsmall);
    TASK_CHECK (nwhole > (unsigned) (SN / Q0 * 0.9));
    TASK_CHECK (compare_pcm (whole, chunked, nwhole) == 0);
    TASK_CHECK (compare_pcm (whole, small, nwhole) == 0);

    // now the interleaved layout with a small output capacity

    b = make_bridge (SCH, FSRC, FDST, 8, ev, nev);
    TASK_CHECK (b != NULL);
    TASK_CHECK (run_interleaved (b, 113, 17, &stream_out_i [0], &ninter) == 0);
    clockBridgeFree (b);

    TASK_CHECK (ninter == nwhole);

    {
        unsigned i;
        int c;

        for (i = 0; i < ninter; ++i)
            for (c = 0; c < SCH; ++c)
                small_i [c][i] = stream_out_i [i * SCH + c];

        TASK_CHECK (compare_pcm (whole, small_i, ninter) == 0);
    }

    return 0;
}

static int case_stream_guards (void)
{
    static double out [SCH][MAXOUT], again [SCH][MAXOUT];
    RefObs ev [2];
    ClockBridgeInfo info, info2, zero;
    ClockBridge *b;
    unsigned n1, n2;
    int nev, i;

    make_stream_input ();
    stream_events (ev, &nev);

    // zero output capacity: nothing produced, no event consumed, no state change

    b = make_bridge (1, FSRC, FDST, 8, ev, 1);
    TASK_CHECK (b != NULL);
    TASK_CHECK (clockBridgeQuery (b, &zero) == 0);

    {
        const artsample_t *ip [1];
        artsample_t out_f [8], *op [1];

        ip [0] = (artsample_t *) stream_in [0];
        op [0] = out_f;
        TASK_CHECK (clockBridgeProcess (b, ip, SN, op, 0).output_generated == 0);
        TASK_CHECK (clockBridgeProcess (b, ip, SN, op, 0).input_used == 0);
    }

    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (info.frames == zero.frames && info.position_int == zero.position_int &&
        info.position_frac == zero.position_frac && info.q == zero.q &&
        info.target == zero.target && info.window == 0);

    // the pending event is still pending: the first real frame applies it

    {
        const artsample_t *ip [1];
        artsample_t out_f [64], *op [1];

        ip [0] = (artsample_t *) stream_in [0];
        op [0] = out_f;
        TASK_CHECK (clockBridgeProcess (b, ip, SN, op, 64).output_generated == 64);
    }

    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (info.window == 1);

    // repeated query is stable

    TASK_CHECK (clockBridgeQuery (b, &info2) == 0);
    TASK_CHECK (info.frames == info2.frames && info.position_int == info2.position_int &&
        info.position_frac == info2.position_frac && info.q == info2.q &&
        info.target == info2.target && info.window == info2.window);

    clockBridgeFree (b);

    // no input and not enough history: nothing produced, event not consumed

    b = make_bridge (1, FSRC, FDST, 8, ev, 1);
    TASK_CHECK (b != NULL);
    TASK_CHECK (clockBridgeProcessInterleaved (b, NULL, 0, NULL, 10).output_generated == 0);
    TASK_CHECK (clockBridgeQuery (b, &info2) == 0);
    TASK_CHECK (info2.frames == 0 && info2.window == 0);

    // with enough history, output without input is allowed; use a strongly
    // upsampling configuration so the history has plenty of slack

    {
        ClockBridge *u = clockBridgeCreate (1, TAPS, NFILT, 12000, 48000, FLAGS, 8);
        const artsample_t *ip [1];
        artsample_t out_f [8], *op [1];
        ResampleResult r;
        int i, no_input = 0;

        TASK_CHECK (u != NULL);
        ip [0] = (artsample_t *) stream_in [0];
        op [0] = out_f;
        TASK_CHECK (clockBridgeProcess (u, ip, SN, op, 1).output_generated == 1);
        TASK_CHECK (clockBridgeProcess (u, ip + 1, SN - 1, op, 1).output_generated == 1);

        for (i = 0; i < 64; ++i) {
            int got = clockBridgeProcessInterleaved (u, NULL, 0, out_f, 1).output_generated;

            no_input += got;
        }

        // the first calls after a little input still have plenty of history

        TASK_CHECK (no_input > 0);

        clockBridgeFree (u);
    }

    clockBridgeFree (b);

    // reset returns to the initial state and re-running the same schedule is
    // deterministic

    b = make_bridge (SCH, FSRC, FDST, 8, ev, nev);
    TASK_CHECK (b != NULL);
    TASK_CHECK (run_planar (b, SN, MAXOUT, out, &n1) == 0);

    clockBridgeReset (b);
    TASK_CHECK (clockBridgeQuery (b, &info) == 0);
    TASK_CHECK (info.frames == 0 && info.position_int == 0 && info.position_frac == 0.0);
    TASK_CHECK (fabs (info.q - Q0) < 1e-15 && fabs (info.target - Q0) < 1e-15);
    TASK_CHECK (info.window == 0);

    // reset discarded the pending observations too, so resubmit the schedule

    TASK_CHECK (make_bridge_events (b, ev, nev) == 0);
    TASK_CHECK (run_planar (b, SN, MAXOUT, again, &n2) == 0);
    TASK_CHECK (n1 == n2);
    TASK_CHECK (compare_pcm (out, again, n1) == 0);

    clockBridgeFree (b);

    (void) i;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

const struct task_case task_cases [] = {
    { "clock", "defaults", case_clock_defaults },
    { "clock", "event_timing", case_clock_event_timing },
    { "clock", "reject", case_clock_reject },
    { "clock", "fit_2p60", case_clock_fit_2p60 },
    { "clock", "clamp", case_clock_clamp },
    { "clock", "reset", case_clock_reset },
    { "audio", "impulse", case_audio_impulse },
    { "audio", "tones", case_audio_tones },
    { "audio", "antiphase", case_audio_antiphase },
    { "streaming", "chunking", case_stream_chunking },
    { "streaming", "guards", case_stream_guards },
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]);
