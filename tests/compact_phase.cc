// ----------------------------------------------------------------------------
//
//  Regression tests for the compact phase-table mode of Resampler and
//  VResampler. The cases are registered in three groups:
//
//    clock   : exact rational clocking, UINT_MAX-safe arithmetic, chunking
//    signal  : PCM accuracy against an independent windowed-sinc reference,
//              phase interval endpoints, downsampling and NULL/channel rules
//    sharing : coefficient-table reuse rules and reference-counted lifetime
//
// ----------------------------------------------------------------------------

#include "zita-resampler/resampler.h"
#include "zita-resampler/vresampler.h"
#include "zita-resampler/resampler-table.h"
#include "task_support/check.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <stdint.h>
#include <limits.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


// ----------------------------------------------------------------------------
// Independent reference: a plain, directly evaluated windowed sinc. This
// mirrors the documented filter math but shares no code with the table
// generator, so quantised table rows are checked rather than re-derived.
// ----------------------------------------------------------------------------

static double ref_sinc (double x)
{
    x = fabs (x);
    if (x < 1e-12) return 1.0;
    x *= M_PI;
    return sin (x) / x;
}

static double ref_wind (double x)
{
    x = fabs (x);
    if (x >= 1.0) return 0.0;
    x *= M_PI;
    return 0.384 + 0.500 * cos (x) + 0.116 * cos (2.0 * x);
}

static double ref_weight (double fr, unsigned int hl, double t0, int k)
{
    double d = (double)((int) hl - 1 - k) + t0;
    return fr * ref_sinc (d * fr) * ref_wind (d / (double) hl);
}

static float ref_output (const std::vector<float>& x, unsigned int base,
                         unsigned int hl, double fr, double t0)
{
    double s = 0.0;
    for (unsigned int k = 0; k < 2 * hl; k++)
        s += (double) x [base + k] * ref_weight (fr, hl, t0, (int) k);
    return (float) s;
}

// Effective filter parameters, computed independently from the documented
// setup() rules (no private state is read for the reference).
static void eff_params (unsigned int fs_inp, unsigned int fs_out,
                        unsigned int hlen, double frel_in,
                        unsigned int& hl, double& fr)
{
    double r = (double) fs_out / (double) fs_inp;
    fr = frel_in;
    hl = hlen;
    if (r < 1.0)
    {
        fr *= r;
        hl = (unsigned int) ceil (hlen / r);
    }
}

// Mirror of the exact rational phase clock, using 64-bit quotient/remainder.
struct Clock
{
    uint64_t ph, np, dp, base;
    unsigned int tnp;
    Clock (uint64_t np_, uint64_t dp_, unsigned int tnp_) :
        ph (0), np (np_), dp (dp_), base (0), tnp (tnp_) {}
    double t0 (void) const
    {
        uint64_t ti = ph * (uint64_t) tnp / np;
        return (double) ti / (double) tnp;
    }
    unsigned int ti (void) const
    {
        return (unsigned int)(ph * (uint64_t) tnp / np);
    }
    void step (void)
    {
        ph += dp;
        uint64_t nr = ph / np;
        ph -= nr * np;
        base += nr;
    };
};

static unsigned int gcd_u (unsigned int a, unsigned int b)
{
    while (b) { unsigned int t = a % b; a = b; b = t; }
    return a;
}

static float bounded_signal (long n)
{
    // Amplitudes sum to 0.95, so every sample stays within [-1, 1].
    return 0.50f * sinf (0.013 * n + 0.4f)
         + 0.30f * sinf (0.047 * n + 1.3f)
         + 0.15f * sinf (0.091 * n + 2.1f);
}


// Drive a fixed Resampler over one contiguous stream, one large call.
static int drive_plain (Resampler& r, const std::vector<float>& x,
                        unsigned int nchan, unsigned int outputs,
                        std::vector<float>& out)
{
    out.clear ();
    out.resize (outputs * nchan);
    unsigned int in_frames = (unsigned int) x.size () / nchan;
    r.reset ();
    r.inp_data = const_cast<float *> (x.data ());
    r.inp_count = in_frames;
    r.out_data = out.data ();
    r.out_count = outputs;
    TASK_CHECK (r.process () == 0);
    TASK_CHECK (r.out_count == 0);
    out.resize ((outputs - r.out_count) * nchan);
    return 0;
}

// Same job, split into small, irregular chunks. null_mode 0 uses real input
// and real output, 1 passes NULL pointers, 2 passes explicit zero input and
// discards output through a throwaway buffer, so runs 1 and 2 can be matched.
static int drive_chunked (Resampler& r, const std::vector<float>& x,
                          unsigned int nchan, unsigned int outputs,
                          std::vector<float>& out,
                          unsigned int in_chunk, unsigned int out_chunk,
                          int null_mode)
{
    out.clear ();
    out.assign (outputs * nchan, 0.0f);
    std::vector<float> zeros (in_chunk * nchan + nchan, 0.0f);
    std::vector<float> discard (out_chunk * nchan + nchan, 7.0f);
    unsigned int in_frames = (unsigned int) x.size () / nchan;
    unsigned int in_pos = 0, out_pos = 0;
    r.reset ();
    while (out_pos < outputs)
    {
        unsigned int ic = in_frames - in_pos;
        if (ic > in_chunk) ic = in_chunk;
        unsigned int oc = outputs - out_pos;
        if (oc > out_chunk) oc = out_chunk;
        bool null_in = (null_mode != 0) && ((out_pos / out_chunk) & 1);
        bool null_out = (null_mode != 0) && ((out_pos / out_chunk) & 2);
        if (null_mode == 1 && null_in) r.inp_data = 0;
        else if (null_mode == 2 && null_in) r.inp_data = zeros.data ();
        else r.inp_data = const_cast<float *> (x.data ()) + in_pos * nchan;
        r.inp_count = ic;
        if (null_mode == 1 && null_out) r.out_data = 0;
        else if (null_mode == 2 && null_out) r.out_data = discard.data ();
        else r.out_data = out.data () + out_pos * nchan;
        r.out_count = oc;
        TASK_CHECK (r.process () == 0);
        in_pos += ic - r.inp_count;
        out_pos += oc - r.out_count;
    }
    out.resize (out_pos * nchan);
    return 0;
}


// ----------------------------------------------------------------------------
// clock group
// ----------------------------------------------------------------------------

static int clock_accept_reject (void)
{
    Resampler r;
    // Previously rejected only because the reduced denominator > 1000.
    TASK_CHECK (r.setup (48000, 47999, 1, 32) == 0);
    TASK_CHECK (r.setup (44100, 48001, 1, 32) == 0);
    // Near-UINT_MAX rates, through the legacy entry as well as compact.
    TASK_CHECK (r.setup (0xFFFFFFFFu, 0xFFFFFFFEu, 1, 32) == 0);
    TASK_CHECK (r.setup_compact (0xFFFFFFFEu, 0xFFFFFFFFu, 1, 32, 1024) == 0);
    // The 1/64 lower ratio bound is unchanged.
    TASK_CHECK (r.setup (6400, 100, 1, 32) == 0);
    TASK_CHECK (r.setup (6400, 99, 1, 32) != 0);
    // Parameter validation.
    TASK_CHECK (r.setup (0, 48000, 1, 32) != 0);
    TASK_CHECK (r.setup (48000, 0, 1, 32) != 0);
    TASK_CHECK (r.setup_compact (48000, 47999, 1, 32, 31) != 0);
    TASK_CHECK (r.setup_compact (48000, 47999, 1, 32, 1025) != 0);
    TASK_CHECK (r.setup_compact (48000, 47999, 0, 32, 1024) != 0);

    VResampler v;
    TASK_CHECK (v.setup_compact (1.5, 1, 32, 1024) == 0);
    TASK_CHECK (v.setup_compact (1.5, 1, 32, 32) == 0);
    TASK_CHECK (v.setup_compact (0.0, 1, 32, 1024) != 0);
    TASK_CHECK (v.setup_compact (70.0, 1, 32, 1024) != 0);
    TASK_CHECK (v.setup_compact (1.5, 1, 32, 31) != 0);
    TASK_CHECK (v.setup_compact (1.5, 1, 32, 1025) != 0);
    return 0;
}

// Exact rational bookkeeping: after L outputs the number of input frames
// read is 2*hl + floor(L * dp / np), with the phase kept as a remainder.
static int clock_counts_one (unsigned int fs_inp, unsigned int fs_out,
                             unsigned int outputs, const char *tag)
{
    Resampler r;
    TASK_CHECK (r.setup_compact (fs_inp, fs_out, 1, 32, 1024) == 0);
    unsigned int g = gcd_u (fs_out, fs_inp);
    uint64_t np = fs_out / g, dp = fs_inp / g;
    unsigned int hl = (unsigned int)(r.inpsize () / 2);
    std::vector<float> in (outputs * 4 + 4 * hl + 64, 0.0f);
    for (size_t i = 0; i < in.size (); i++) in [i] = bounded_signal ((long) i);
    std::vector<float> out (outputs + 64);

    r.reset ();
    unsigned int fed = 0, made = 0;
    unsigned int chunks[] = { 1, 3, 7, 16, 37, 128 };
    while (made < outputs)
    {
        unsigned int oc = outputs - made;
        if (oc > chunks [made % 6]) oc = chunks [made % 6];
        r.inp_data = in.data () + fed;
        r.inp_count = (unsigned int) in.size () - fed;
        r.out_data = out.data () + made;
        r.out_count = oc;
        TASK_CHECK (r.process () == 0);
        fed = (unsigned int) in.size () - r.inp_count;
        made += oc - r.out_count;
    }
    // L outputs consume the initial 2*hl prefill plus the integer input
    // advances of the first L - 1 phase steps; the step after the last
    // output is not read until the following output is requested.
    uint64_t expect_read = 2ull * hl + ((uint64_t)(outputs - 1) * dp) / np;
    (void) tag;
    TASK_CHECK (fed == expect_read);
    for (unsigned int i = 0; i < made; i++) TASK_CHECK (std::isfinite (out [i]));
    return 0;
}

static int clock_rational_counts (void)
{
    TASK_CHECK (clock_counts_one (48000, 47999, 5000, "48000/47999") == 0);
    TASK_CHECK (clock_counts_one (44100, 48001, 5000, "44100/48001") == 0);
    return 0;
}

static int clock_near_uint_max (void)
{
    const unsigned int HI = 0xFFFFFFFFu;
    Resampler a, b;
    TASK_CHECK (a.setup (HI, HI - 1, 1, 32) == 0);          // slight downsample
    TASK_CHECK (b.setup_compact (HI - 1, HI, 1, 32, 1024) == 0);
    unsigned int hl = (unsigned int)(a.inpsize () / 2);
    unsigned int outputs = 3000;
    std::vector<float> in (outputs * 3 + 4 * hl + 64, 0.0f);
    for (size_t i = 0; i < in.size (); i++) in [i] = bounded_signal ((long) i);
    std::vector<float> oa (outputs), ob (outputs);

    unsigned int fed = 0, made = 0;
    a.reset ();
    while (made < outputs)
    {
        a.inp_data = in.data () + fed;
        a.inp_count = (unsigned int) in.size () - fed;
        a.out_data = oa.data () + made;
        a.out_count = outputs - made;
        TASK_CHECK (a.process () == 0);
        fed = (unsigned int) in.size () - a.inp_count;
        made = outputs - a.out_count;
    }
    uint64_t np = (uint64_t) HI - 1, dp = HI;
    uint64_t expect = 2ull * hl + ((uint64_t)(outputs - 1) * dp) / np;
    TASK_CHECK (fed == expect);
    for (unsigned int i = 0; i < outputs; i++) TASK_CHECK (std::isfinite (oa [i]));

    // Reverse direction, same sanity rules.
    fed = 0; made = 0; b.reset ();
    while (made < outputs)
    {
        b.inp_data = in.data () + fed;
        b.inp_count = (unsigned int) in.size () - fed;
        b.out_data = ob.data () + made;
        b.out_count = outputs - made;
        TASK_CHECK (b.process () == 0);
        fed = (unsigned int) in.size () - b.inp_count;
        made = outputs - b.out_count;
    }
    np = HI; dp = (uint64_t) HI - 1;
    unsigned int hl_b = (unsigned int)(b.inpsize () / 2);
    expect = 2ull * hl_b + ((uint64_t)(outputs - 1) * dp) / np;
    TASK_CHECK (fed == expect);
    for (unsigned int i = 0; i < outputs; i++) TASK_CHECK (std::isfinite (ob [i]));
    return 0;
}

// inpdist() is the true input position, on the logical phase denominator.
static int clock_inpdist (void)
{
    Resampler r;
    TASK_CHECK (r.setup_compact (48000, 47999, 1, 32, 1024) == 0);
    unsigned int hl = (unsigned int)(r.inpsize () / 2);
    TASK_CHECK (fabs (r.inpdist () - (1.0 - (int) hl)) < 1e-12);

    unsigned int g = gcd_u (47999, 48000);
    Clock clk (47999 / g, 48000 / g, 1024);
    std::vector<float> in (8192, 0.0f);
    for (size_t i = 0; i < in.size (); i++) in [i] = bounded_signal ((long) i);
    std::vector<float> out (1024);
    unsigned int fed = 0, made = 0, chunksz = 53;
    r.reset ();
    while (made < 800)
    {
        unsigned int oc = 800 - made;
        if (oc > chunksz) oc = chunksz;
        // Before the call, inpdist() describes the next output to produce.
        // nread = 2*hl - fed + clk.base: frames still missing before the
        // window center of the output at logical phase clk.ph / clk.np.
        long nread = (long)(2 * hl) - (long) fed + (long) clk.base;
        double want = (double)((long) hl + 1 - nread)
                    - (double) clk.ph / (double) clk.np;
        TASK_CHECK (fabs (want - r.inpdist ()) < 1e-9);
        r.inp_data = in.data () + fed;
        r.inp_count = (unsigned int) in.size () - fed;
        r.out_data = out.data () + made;
        r.out_count = oc;
        TASK_CHECK (r.process () == 0);
        fed = (unsigned int) in.size () - r.inp_count;
        unsigned int produced = oc - r.out_count;
        for (unsigned int m = 0; m < produced; m++) clk.step ();
        made += produced;
    }

    VResampler v;
    TASK_CHECK (v.setup_compact (1.5, 1, 32, 64) == 0);
    unsigned int vhl = (unsigned int)(v.inpsize () / 2);
    TASK_CHECK (fabs (v.inpdist () - (1.0 - (int) vhl)) < 1e-12);
    v.set_phase (0.25);
    TASK_CHECK (fabs (v.inpdist () - (1.0 - (int) vhl - 0.25)) < 1e-12);
    return 0;
}

// A job run in one call and the same job split into chunks must agree,
// including NULL input padding and NULL output skipping.
static int clock_chunk_consistency (void)
{
    Resampler r;
    TASK_CHECK (r.setup_compact (48000, 47999, 2, 32, 1024) == 0);
    unsigned int hl = (unsigned int)(r.inpsize () / 2);
    unsigned int total_in = 6000;
    std::vector<float> x (total_in * 2, 0.0f);
    for (unsigned int i = 0; i < total_in; i++)
    {
        x [2 * i]     = bounded_signal (i);
        x [2 * i + 1] = 0.5f * bounded_signal (10000 + i);
    }
    (void) hl;
    unsigned int outputs = 4000;
    std::vector<float> a, b, c, d;
    TASK_CHECK (drive_plain (r, x, 2, outputs, a) == 0);
    TASK_CHECK (drive_chunked (r, x, 2, outputs, b, 37, 29, 0) == 0);
    TASK_CHECK (drive_chunked (r, x, 2, outputs, c, 31, 23, 1) == 0);  // NULL input / output
    TASK_CHECK (drive_chunked (r, x, 2, outputs, d, 31, 23, 2) == 0);  // zero buffers instead
    TASK_CHECK (a.size () == b.size ());
    TASK_CHECK (a.size () == c.size ());
    TASK_CHECK (c.size () == d.size ());
    for (size_t i = 0; i < a.size (); i++)
    {
        TASK_CHECK (a [i] == b [i]);
        TASK_CHECK (c [i] == d [i]);   // NULL pointers == explicit zeros
    }
    return 0;
}

// A compact VResampler whose budget does not compress the 120-phase default
// table must be bit-identical to a legacy VResampler, including while the
// ratio filter and phase controls are used.
static int clock_variable_matches_legacy (void)
{
    const double ratio = 47999.0 / 48000.0;
    VResampler ref, compact;
    TASK_CHECK (ref.setup (ratio, 1, 32) == 0);
    TASK_CHECK (compact.setup_compact (ratio, 1, 32, 1024) == 0);
    std::vector<float> in (8192), o1 (4096), o2 (4096);
    for (size_t i = 0; i < in.size (); i++) in [i] = bounded_signal ((long) i);

    ref.set_phase (0.1);
    compact.set_phase (0.1);
    ref.set_rrfilt (4.0);
    compact.set_rrfilt (4.0);
    ref.set_rratio (1.03);
    compact.set_rratio (1.03);

    unsigned int fed = 0, made = 0;
    while (made < 3000)
    {
        unsigned int oc = 97;
        if (made + oc > 3000) oc = 3000 - made;
        ref.inp_data = in.data () + fed;
        compact.inp_data = in.data () + fed;
        ref.inp_count = compact.inp_count = (unsigned int) in.size () - fed;
        ref.out_data = o1.data () + made;
        compact.out_data = o2.data () + made;
        ref.out_count = compact.out_count = oc;
        TASK_CHECK (ref.process () == 0);
        TASK_CHECK (compact.process () == 0);
        TASK_CHECK (ref.inp_count == compact.inp_count);
        fed = (unsigned int) in.size () - ref.inp_count;
        made += oc - ref.out_count;
    }
    for (unsigned int i = 0; i < made; i++) TASK_CHECK (o1 [i] == o2 [i]);
    TASK_CHECK (fabs (ref.inpdist () - compact.inpdist ()) < 1e-12);
    return 0;
}


// ----------------------------------------------------------------------------
// signal group
// ----------------------------------------------------------------------------

// Full PCM check against the independent windowed-sinc reference, budget
// 1024 and hlen 32: max absolute error must stay below 2e-4.
static int signal_pcm_accuracy (void)
{
    const unsigned int fs_inp = 48000, fs_out = 47999, hlen = 32, budget = 1024;
    Resampler r;
    TASK_CHECK (r.setup_compact (fs_inp, fs_out, 1, hlen, budget) == 0);
    unsigned int g = gcd_u (fs_out, fs_inp);
    Clock clk (fs_out / g, fs_inp / g, budget);
    unsigned int hl; double fr;
    eff_params (fs_inp, fs_out, hlen, 1.0 - 2.6 / hlen, hl, fr);
    TASK_CHECK ((int) hl == r.inpsize () / 2);

    const unsigned int outputs = 3000;
    std::vector<float> x (outputs * 3 + 4 * hl + 64, 0.0f);
    for (size_t i = 0; i < x.size (); i++) x [i] = bounded_signal ((long) i);
    std::vector<float> got (outputs);
    r.reset ();
    r.inp_data = x.data ();
    r.inp_count = (unsigned int) x.size ();
    r.out_data = got.data ();
    r.out_count = outputs;
    TASK_CHECK (r.process () == 0);
    TASK_CHECK (r.out_count == 0);

    double maxerr = 0.0;
    for (unsigned int m = 0; m < outputs; m++)
    {
        float want = ref_output (x, (unsigned int) clk.base, hl, fr, clk.t0 ());
        double e = fabs ((double) got [m] - (double) want);
        if (e > maxerr) maxerr = e;
        clk.step ();
    }
    TASK_CHECK (maxerr <= 2e-4);
    return 0;
}

// Endpoint rows of the phase intervals, including the mirrored reverse half
// window, must stay continuous: every compressed phase index, in particular
// 0 and tnp - 1, is checked against the direct reference.
static int signal_phase_endpoints (void)
{
    const unsigned int fs_inp = 2049, fs_out = 2048, hlen = 32, budget = 1024;
    Resampler r;
    TASK_CHECK (r.setup_compact (fs_inp, fs_out, 1, hlen, budget) == 0);
    unsigned int g = gcd_u (fs_out, fs_inp);
    Clock clk (fs_out / g, fs_inp / g, budget);
    unsigned int hl; double fr;
    eff_params (fs_inp, fs_out, hlen, 1.0 - 2.6 / hlen, hl, fr);

    const unsigned int outputs = 2048;   // phases sweep the whole table
    std::vector<float> x (outputs * 3 + 4 * hl + 64, 0.0f);
    for (size_t i = 0; i < x.size (); i++) x [i] = bounded_signal ((long) i);
    std::vector<float> got (outputs);
    r.reset ();
    r.inp_data = x.data ();
    r.inp_count = (unsigned int) x.size ();
    r.out_data = got.data ();
    r.out_count = outputs;
    TASK_CHECK (r.process () == 0);

    bool saw_first = false, saw_last = false;
    double maxerr = 0.0;
    for (unsigned int m = 0; m < outputs; m++)
    {
        unsigned int ti = clk.ti ();
        if (ti == 0) saw_first = true;
        if (ti == budget - 1) saw_last = true;
        float want = ref_output (x, (unsigned int) clk.base, hl, fr, clk.t0 ());
        double e = fabs ((double) got [m] - (double) want);
        if (e > maxerr) maxerr = e;
        clk.step ();
    }
    TASK_CHECK (saw_first);
    TASK_CHECK (saw_last);
    TASK_CHECK (maxerr <= 2e-4);
    return 0;
}

// Downsampling: the window is lengthened and the cut-off narrowed by the
// ratio, independently of the compressed table size, and the reference still
// matches with the same tolerance.
static int signal_downsampling (void)
{
    const unsigned int fs_inp = 47999, fs_out = 8000, hlen = 32, budget = 1024;
    Resampler r;
    TASK_CHECK (r.setup_compact (fs_inp, fs_out, 1, hlen, budget) == 0);
    unsigned int g = gcd_u (fs_out, fs_inp);
    Clock clk (fs_out / g, fs_inp / g, budget);
    unsigned int hl; double fr;
    eff_params (fs_inp, fs_out, hlen, 1.0 - 2.6 / hlen, hl, fr);
    TASK_CHECK ((int) hl == r.inpsize () / 2);
    TASK_CHECK (hl > hlen);                              // window lengthened
    TASK_CHECK (fr < 1.0 - 2.6 / hlen);                  // cut-off narrowed

    const unsigned int outputs = 400;
    std::vector<float> x (outputs * 64 + 4 * hl + 64, 0.0f);
    for (size_t i = 0; i < x.size (); i++) x [i] = bounded_signal ((long) i);
    std::vector<float> got (outputs);
    r.reset ();
    r.inp_data = x.data ();
    r.inp_count = (unsigned int) x.size ();
    r.out_data = got.data ();
    r.out_count = outputs;
    TASK_CHECK (r.process () == 0);

    double maxerr = 0.0;
    for (unsigned int m = 0; m < outputs; m++)
    {
        float want = ref_output (x, (unsigned int) clk.base, hl, fr, clk.t0 ());
        double e = fabs ((double) got [m] - (double) want);
        if (e > maxerr) maxerr = e;
        clk.step ();
    }
    TASK_CHECK (maxerr <= 2e-4);
    return 0;
}

// A smaller budget trades precision for memory; output must stay finite and
// close to the reference, with a looser bound than the 1024-budget guarantee.
static int signal_budget_tradeoff (void)
{
    const unsigned int fs_inp = 48000, fs_out = 47999, hlen = 32;
    unsigned int hl; double fr;
    eff_params (fs_inp, fs_out, hlen, 1.0 - 2.6 / hlen, hl, fr);
    unsigned int g = gcd_u (fs_out, fs_inp);
    const unsigned int outputs = 1500;
    std::vector<float> x (outputs * 3 + 4 * hl + 64, 0.0f);
    for (size_t i = 0; i < x.size (); i++) x [i] = bounded_signal ((long) i);

    double first_err = -1.0, last_err = 0.0;
    const unsigned int budgets [] = { 32, 64, 256, 1024 };
    for (unsigned int b = 0; b < 4; b++)
    {
        Resampler r;
        TASK_CHECK (r.setup_compact (fs_inp, fs_out, 1, hlen, budgets [b]) == 0);
        Clock clk (fs_out / g, fs_inp / g, budgets [b]);
        std::vector<float> got (outputs);
        r.reset ();
        r.inp_data = x.data ();
        r.inp_count = (unsigned int) x.size ();
        r.out_data = got.data ();
        r.out_count = outputs;
        TASK_CHECK (r.process () == 0);
        double maxerr = 0.0;
        for (unsigned int m = 0; m < outputs; m++)
        {
            float want = ref_output (x, (unsigned int) clk.base, hl, fr, clk.t0 ());
            double e = fabs ((double) got [m] - (double) want);
            if (e > maxerr) maxerr = e;
            clk.step ();
        }
        TASK_CHECK (std::isfinite (maxerr));
        TASK_CHECK (maxerr < 1e-2);
        if (first_err < 0.0) first_err = maxerr;
        last_err = maxerr;
    }
    // More phase intervals buy accuracy: the full budget is at least as good.
    TASK_CHECK (last_err <= first_err + 1e-12);
    return 0;
}

// Compact variable engine: the interpolated coefficient rows still match the
// direct windowed-sinc reference, and the dynamic ratio controls keep working.
static int signal_variable_compact (void)
{
    const double ratio = 48001.0 / 44100.0;
    const unsigned int hlen = 32, budget = 64, NP = 120, outputs = 2000;
    VResampler v;
    TASK_CHECK (v.setup_compact (ratio, 1, hlen, budget) == 0);
    double frel = 1.0 - 2.6 / hlen;
    unsigned int hl = hlen;
    if (ratio < 1.0) { frel *= ratio; hl = (unsigned int) ceil (hlen / ratio); }
    TASK_CHECK ((int) hl == v.inpsize () / 2);

    std::vector<float> x ((size_t)(outputs / ratio) + 4 * hl + 256, 0.0f);
    for (size_t i = 0; i < x.size (); i++) x [i] = bounded_signal ((long) i);
    std::vector<float> got (outputs);
    v.reset ();
    v.inp_data = x.data ();
    v.inp_count = (unsigned int) x.size ();
    v.out_data = got.data ();
    v.out_count = outputs;
    TASK_CHECK (v.process () == 0);

    double dp = NP / ratio, ph = 0.0;
    long base = 0;
    double maxerr = 0.0;
    for (unsigned int m = 0; m < outputs; m++)
    {
        double t = ph / NP;
        double s = 0.0;
        for (unsigned int k = 0; k < 2 * hl; k++)
        {
            double d = (double)((int) hl - 1 - (int) k) + t;
            s += (double) x [base + k]
               * (frel * ref_sinc (d * frel) * ref_wind (d / (double) hl));
        }
        double e = fabs ((double) got [m] - s);
        if (e > maxerr) maxerr = e;
        ph += dp;
        if (ph >= NP) { long nr = (long) floor (ph / NP); ph -= nr * NP; base += nr; }
    }
    TASK_CHECK (maxerr <= 2e-4);

    // Dynamic ratio and filtering controls remain usable on the NPHASE clock.
    v.set_rrfilt (3.0);
    v.set_rratio (1.05);
    v.reset ();
    std::vector<float> got2 (outputs);
    v.inp_data = x.data ();
    v.inp_count = (unsigned int) x.size ();
    v.out_data = got2.data ();
    v.out_count = outputs;
    TASK_CHECK (v.process () == 0);
    for (unsigned int i = 0; i < outputs - v.out_count; i++)
        TASK_CHECK (std::isfinite (got2 [i]));
    TASK_CHECK (v.inp_count < (unsigned int) x.size ());
    return 0;
}


// Empty input, NULL input zero-fill, NULL output skip and multichannel frame
// counting keep their original semantics.
static int signal_null_and_channels (void)
{
    Resampler r;
    TASK_CHECK (r.setup_compact (48000, 47999, 1, 32, 1024) == 0);
    unsigned int hl = (unsigned int)(r.inpsize () / 2);
    std::vector<float> in (4096, 0.0f), out (2048);
    for (size_t i = 0; i < in.size (); i++) in [i] = bounded_signal ((long) i);

    // Empty input: nothing happens, state and distance are unchanged.
    r.reset ();
    r.inp_count = 0; r.inp_data = in.data ();
    r.out_count = 10; r.out_data = out.data ();
    double d0 = r.inpdist ();
    TASK_CHECK (r.process () == 0);
    TASK_CHECK (r.out_count == 10);
    TASK_CHECK (r.inpdist () == d0);

    // NULL input is identical to an explicit zero buffer.
    std::vector<float> o1 (1024), o2 (1024), zeros (4096, 0.0f);
    r.reset ();
    r.inp_data = 0; r.inp_count = 4096; r.out_data = o1.data (); r.out_count = 1024;
    TASK_CHECK (r.process () == 0);
    r.reset ();
    r.inp_data = zeros.data (); r.inp_count = 4096;
    r.out_data = o2.data (); r.out_count = 1024;
    TASK_CHECK (r.process () == 0);
    for (unsigned int i = 0; i < 1024; i++) TASK_CHECK (o1 [i] == o2 [i]);

    // NULL output advances the state exactly like a discarded real buffer.
    std::vector<float> tail_a (1024), tail_b (1024);
    r.reset ();
    r.inp_data = in.data (); r.inp_count = 4096;
    r.out_data = 0; r.out_count = 300;
    TASK_CHECK (r.process () == 0);
    r.out_data = tail_a.data (); r.out_count = 1024;
    TASK_CHECK (r.process () == 0);
    r.reset ();
    r.inp_data = in.data (); r.inp_count = 4096;
    r.out_data = out.data (); r.out_count = 300;
    TASK_CHECK (r.process () == 0);
    r.out_data = tail_b.data (); r.out_count = 1024;
    TASK_CHECK (r.process () == 0);
    for (unsigned int i = 0; i < 1024; i++) TASK_CHECK (tail_a [i] == tail_b [i]);

    // Multichannel: same frame count as mono, channels stay independent.
    Resampler rm, rs;
    TASK_CHECK (rm.setup_compact (48000, 47999, 3, 32, 1024) == 0);
    TASK_CHECK (rs.setup_compact (48000, 47999, 1, 32, 1024) == 0);
    std::vector<float> mi (2048 * 3), ch0 (2048), mo (2048 * 3), so (2048);
    for (unsigned int i = 0; i < 2048; i++)
    {
        for (unsigned int c = 0; c < 3; c++)
            mi [3 * i + c] = bounded_signal (c * 5000 + i);
        ch0 [i] = mi [3 * i];
    }
    rm.reset (); rs.reset ();
    rm.inp_data = mi.data (); rm.inp_count = 2048;
    rm.out_data = mo.data (); rm.out_count = 1500;
    rs.inp_data = ch0.data (); rs.inp_count = 2048;
    rs.out_data = so.data (); rs.out_count = 1500;
    TASK_CHECK (rm.process () == 0);
    TASK_CHECK (rs.process () == 0);
    TASK_CHECK (rm.out_count == rs.out_count);
    unsigned int made = 1500 - rm.out_count;
    for (unsigned int i = 0; i < made; i++) TASK_CHECK (mo [3 * i] == so [i]);
    (void) hl;
    return 0;
}


// ----------------------------------------------------------------------------
// sharing group
// ----------------------------------------------------------------------------

// Capture the private table list through the public diagnostic entry point.
static std::string capture_table_list (void)
{
    const char *tmp = getenv ("TMPDIR");
    char path [256];
    snprintf (path, sizeof path, "%s/zita_tab_XXXXXX",
              (tmp && *tmp) ? tmp : "/tmp");
    int fd = mkstemp (path);
    if (fd < 0) return "";
    fflush (stdout);
    int saved = dup (STDOUT_FILENO);
    FILE *f = fdopen (fd, "w");
    dup2 (fileno (f), STDOUT_FILENO);
    Resampler_table::print_list ();
    fflush (stdout);
    dup2 (saved, STDOUT_FILENO);
    close (saved);
    fclose (f);

    FILE *rd = fopen (path, "r");
    std::string s;
    if (rd)
    {
        char buf [512];
        size_t n;
        while ((n = fread (buf, 1, sizeof buf, rd)) > 0) s.append (buf, n);
        fclose (rd);
    }
    unlink (path);
    return s;
}

struct TableRow { int refc; double fr; unsigned int hl, np, den; };

static std::vector<TableRow> parse_rows (const std::string& s)
{
    std::vector<TableRow> rows;
    size_t pos = 0;
    while (true)
    {
        size_t hit = s.find ("refc =", pos);
        if (hit == std::string::npos) break;
        TableRow row;
        int found = sscanf (s.c_str () + hit,
                            "refc = %d fr = %lf hl = %u np = %u den = %u",
                            &row.refc, &row.fr, &row.hl, &row.np, &row.den);
        if (found == 5) rows.push_back (row);
        pos = hit + 6;
    }
    return rows;
}

static int count_rows (const std::vector<TableRow>& rows, unsigned int np)
{
    int n = 0;
    for (size_t i = 0; i < rows.size (); i++)
        if (rows [i].np == np) n++;
    return n;
}

// Compatible layouts share one table; different cut-off, window length or
// incompatible phase grids never do. The table keeps at most budget rows.
static int sharing_rules (void)
{
    std::string before = capture_table_list ();
    {
        // den 3001 and 6002: same fr/hl/physical size, 6002 = 2 * 3001.
        Resampler a, b, c, d, e;
        TASK_CHECK (a.setup_compact (2003, 3001, 1, 32, 1024) == 0);
        TASK_CHECK (b.setup_compact (2003, 6002, 1, 32, 1024) == 0);
        // den 3002 does not divide, and is not a multiple of, 3001.
        TASK_CHECK (c.setup_compact (2003, 3002, 1, 32, 1024) == 0);
        // Different window length.
        TASK_CHECK (d.setup_compact (2003, 3001, 1, 48, 1024) == 0);
        // Downsampling changes the cut-off and window length.
        TASK_CHECK (e.setup_compact (3001, 2003, 1, 32, 1024) == 0);

        std::vector<TableRow> rows = parse_rows (capture_table_list ());
        TASK_CHECK (count_rows (rows, 1024) >= 3);
        bool shared = false;
        for (size_t i = 0; i < rows.size (); i++)
            if (rows [i].np == 1024 && rows [i].refc >= 2) shared = true;
        TASK_CHECK (shared);
        for (size_t i = 0; i < rows.size (); i++) TASK_CHECK (rows [i].np <= 1024);
    }
    // Everything released again: the compact rows created above are gone.
    std::vector<TableRow> after = parse_rows (capture_table_list ());
    TASK_CHECK (count_rows (after, 1024) == count_rows (parse_rows (before), 1024));

    // Many distinct coprime denominators on one compatible grid still use a
    // single shared table rather than one full table per denominator.
    {
        std::vector<Resampler *> rs;
        for (unsigned int k = 1; k <= 6; k++)
        {
            Resampler *r = new Resampler ();
            TASK_CHECK (r->setup_compact (2003, 3001 * k, 1, 32, 1024) == 0);
            rs.push_back (r);
        }
        std::vector<TableRow> rows = parse_rows (capture_table_list ());
        int found = -1;
        for (size_t i = 0; i < rows.size (); i++)
            if (rows [i].np == 1024 && rows [i].refc >= 6) found = (int) i;
        TASK_CHECK (found >= 0);
        for (size_t i = 0; i < rs.size (); i++) delete rs [i];
    }

    // Variable and fixed engines share a table when layouts agree, and the
    // legacy 120-phase table is shared as before.
    {
        Resampler fx;
        VResampler v1, v2, v3;
        TASK_CHECK (fx.setup_compact (91, 1560, 1, 32, 1024) == 0); // np 120
        TASK_CHECK (v1.setup_compact (120.0 / 7.0, 1, 32, 1024) == 0);
        TASK_CHECK (v2.setup_compact (120.0 / 7.0, 1, 32, 32) == 0);
        TASK_CHECK (v3.setup_compact (120.0 / 7.0, 1, 32, 32) == 0);
        std::vector<TableRow> rows = parse_rows (capture_table_list ());
        bool cross = false, vshare = false;
        for (size_t i = 0; i < rows.size (); i++)
        {
            if (rows [i].np == 120 && rows [i].refc >= 2) cross = true;
            if (rows [i].np == 32 && rows [i].refc >= 2) vshare = true;
        }
        TASK_CHECK (cross);
        TASK_CHECK (vshare);
    }
    {
        Resampler a, b;
        TASK_CHECK (a.setup (44100, 48000, 1, 32) == 0);
        TASK_CHECK (b.setup (44100, 48000, 1, 32) == 0);
        std::vector<TableRow> rows = parse_rows (capture_table_list ());
        bool legacy = false;
        for (size_t i = 0; i < rows.size (); i++)
            if (rows [i].np == 160 && rows [i].refc >= 2) legacy = true;
        TASK_CHECK (legacy);
    }
    return 0;
}

// Interleaved creation/release of both engines must not corrupt survivors:
// chunked and single-shot processing still agree, and tables are reclaimed.
static int sharing_lifetime (void)
{
    std::vector<float> x (6000, 0.0f), ref (4000), chunk (4000);
    for (size_t i = 0; i < x.size (); i++) x [i] = bounded_signal ((long) i);

    Resampler *r1 = new Resampler ();
    VResampler *v1 = new VResampler ();
    TASK_CHECK (r1->setup_compact (48000, 47999, 1, 32, 1024) == 0);
    TASK_CHECK (v1->setup_compact (47999.0 / 48000.0, 1, 32, 256) == 0);
    {
        Resampler r2;
        VResampler v2;
        TASK_CHECK (r2.setup_compact (2003, 3001, 1, 32, 1024) == 0);
        TASK_CHECK (v2.setup_compact (2.0, 1, 32, 256) == 0);
        r2.inp_data = x.data (); r2.inp_count = 2048;
        r2.out_data = 0; r2.out_count = 512;
        TASK_CHECK (r2.process () == 0);
        v2.set_rratio (1.04);
        v2.inp_data = x.data (); v2.inp_count = 2048;
        v2.out_data = 0; v2.out_count = 256;
        TASK_CHECK (v2.process () == 0);
    } // r2, v2 released here; r1/v1 tables must remain valid.

    Resampler hold;
    TASK_CHECK (hold.setup_compact (2003, 6002, 1, 32, 1024) == 0);

    TASK_CHECK (drive_plain (*r1, x, 1, 4000, ref) == 0);
    TASK_CHECK (drive_chunked (*r1, x, 1, 4000, chunk, 41, 33, 0) == 0);
    TASK_CHECK (ref.size () == chunk.size ());
    for (size_t i = 0; i < ref.size (); i++) TASK_CHECK (ref [i] == chunk [i]);

    // The surviving variable engine still runs with its adjusted ratio.
    v1->set_rratio (0.97);
    v1->reset ();
    std::vector<float> vout (2048);
    v1->inp_data = x.data (); v1->inp_count = 4096;
    v1->out_data = vout.data (); v1->out_count = 2048;
    TASK_CHECK (v1->process () == 0);
    for (unsigned int i = 0; i < 2048 - v1->out_count; i++)
        TASK_CHECK (std::isfinite (vout [i]));

    delete v1;
    delete r1;
    std::vector<TableRow> rows = parse_rows (capture_table_list ());
    TASK_CHECK (count_rows (rows, 1024) >= 1);   // hold still references one
    return 0;
}


// ----------------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------------

extern const struct task_case task_cases [] = {
    { "clock",   "accept_reject",          clock_accept_reject },
    { "clock",   "rational_counts",        clock_rational_counts },
    { "clock",   "near_uint_max",          clock_near_uint_max },
    { "clock",   "inpdist_position",       clock_inpdist },
    { "clock",   "chunk_consistency",      clock_chunk_consistency },
    { "clock",   "variable_matches_legacy", clock_variable_matches_legacy },
    { "signal",  "pcm_accuracy_1024_32",   signal_pcm_accuracy },
    { "signal",  "phase_endpoints",        signal_phase_endpoints },
    { "signal",  "downsampling",           signal_downsampling },
    { "signal",  "budget_tradeoff",        signal_budget_tradeoff },
    { "signal",  "variable_compact",       signal_variable_compact },
    { "signal",  "null_and_channels",      signal_null_and_channels },
    { "sharing", "table_rules",            sharing_rules },
    { "sharing", "interleaved_lifetime",   sharing_lifetime },
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]);
