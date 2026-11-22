// Tests for per-channel source position offsets in VResampler / CResampler.
// No Python, no audio device: everything is driven through the public API.
//
// Three groups are registered with task_support/check.h:
//   coordinate - API contract, validation, inpdist bookkeeping
//   window     - the offset really moves the filter sampling position
//   streaming  - real filter output, chunk invariance and allocation budget

#include "task_support/check.h"
#include <zita-resampler/vresampler.h>
#include <zita-resampler/cresampler.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

// ---------------------------------------------------------------------------
// Global allocation counter, used to prove process () / set_channel_offsets ()
// never allocate. Snapshots are taken around the calls under test.
// ---------------------------------------------------------------------------
static long g_allocs = 0;

void *operator new (std::size_t n)
{
    ++g_allocs;
    void *p = std::malloc (n ? n : 1);
    if (!p) throw std::bad_alloc ();
    return p;
}
void *operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void *p) noexcept { std::free (p); }
void operator delete[] (void *p) noexcept { std::free (p); }

namespace {

// A check usable from inside lambdas (TASK_CHECK would return from a lambda
// with the wrong type). Counts through task_check and aborts on failure.
int must (int v, const char *e, const char *file, int line)
{
    if (!task_check (v, e, file, line)) std::abort ();
    return v;
}
#define TASK_ASSERT(x) must (!!(x), #x, __FILE__, __LINE__)

// Traits so the same tests drive both engines.
template <class R> struct Engine;

template <> struct Engine<VResampler>
{
    static int setup (VResampler &r, double ratio, unsigned nchan)
    { return r.setup (ratio, nchan, 32); }
    static void set_rratio (VResampler &r, double v) { r.set_rratio (v); }
};

template <> struct Engine<CResampler>
{
    static int setup (CResampler &r, double ratio, unsigned nchan)
    { return r.setup (ratio, nchan); }
    static void set_rratio (CResampler &r, double v) { r.set_ratio (v); }
};

// ---------------------------------------------------------------------------
// Deterministic input signal.
// ---------------------------------------------------------------------------
double sample_signal (unsigned k, unsigned c)
{
    return 0.23 * std::sin (0.021 * k + 0.7 * c)
         + 0.09 * std::cos (0.0061 * k + 1.3 * c)
         + 0.04 * std::sin (0.00037 * k * (c + 1));
}

std::vector<float> padded (unsigned prefix, unsigned nchan, const std::vector<float> &payload)
{
    std::vector<float> s ((size_t) prefix * nchan, 0.0f);
    s.insert (s.end (), payload.begin (), payload.end ());
    return s;
}

struct Driver
{
    unsigned nchan, in_chunk, out_cap, total_out;
    bool     skip_out;
    unsigned in_pos;        // input frame to start from (updated in place)
};

// Streaming driver. Calls `on_frame(produced_output_frames)` before each
// process (). Returns the produced interleaved frames (empty if process
// failed).
template <class R, class F>
std::vector<float> drive (R &r, const std::vector<float> &stream, const Driver &d, F on_frame)
{
    std::vector<float> buffer ((size_t) d.out_cap * d.nchan);
    std::vector<float> out;
    unsigned frames = (unsigned) (stream.size () / d.nchan);
    unsigned pos = d.in_pos;      // caller's input start (advanced in place)
    unsigned produced = 0;
    while (produced < d.total_out)
    {
        on_frame (produced);
        unsigned io = frames - pos < d.in_chunk ? frames - pos : d.in_chunk;
        unsigned oo = d.total_out - produced < d.out_cap ? d.total_out - produced : d.out_cap;
        r.inp_count = io;
        r.inp_data  = io ? const_cast<float *> (&stream [(size_t) pos * d.nchan]) : 0;
        r.out_count = oo;
        r.out_data  = d.skip_out ? 0 : buffer.data ();
        unsigned i0 = io, o0 = oo;
        if (r.process ()) return std::vector<float> ();
        unsigned used = i0 - r.inp_count, made = o0 - r.out_count;
        pos += used;
        if (!d.skip_out)
            out.insert (out.end (), buffer.begin (), buffer.begin () + made * d.nchan);
        produced += made;
        if (!used && !made) break;
    }
    const_cast<Driver &> (d).in_pos = pos;
    return out;
}

struct Noop { void operator () (unsigned) const { } };

struct ScheduleStep
{
    unsigned frame;
    double   offs [6];
    double   rratio;
};

// Drive while applying a schedule at exact output frames: each process ()
// call is capped so that no call crosses a scheduled frame boundary, making
// the resulting sound independent of chunking.
template <class R>
std::vector<float> drive_scheduled (R &r, const std::vector<float> &stream,
                                    unsigned nchan, unsigned in_chunk, unsigned out_cap,
                                    unsigned total_out, const ScheduleStep *steps,
                                    unsigned nsteps)
{
    std::vector<float> buffer ((size_t) out_cap * nchan);
    std::vector<float> out;
    unsigned frames = (unsigned) (stream.size () / nchan);
    unsigned pos = 0, produced = 0, si = 0;
    while (produced < total_out)
    {
        // Apply every schedule entry due at or before this frame.
        while (si < nsteps && steps [si].frame <= produced)
        {
            double o [6];
            for (unsigned c = 0; c < nchan; ++c) o [c] = steps [si].offs [c];
            if (r.set_channel_offsets (o, nchan)) return std::vector<float> ();
            if (si > 0) Engine<R>::set_rratio (r, steps [si].rratio);
            ++si;
        }
        unsigned cap = out_cap;
        if (si < nsteps && steps [si].frame - produced < cap)
            cap = steps [si].frame - produced;
        if (total_out - produced < cap) cap = total_out - produced;
        unsigned io = frames - pos < in_chunk ? frames - pos : in_chunk;
        r.inp_count = io;
        r.inp_data  = io ? const_cast<float *> (&stream [(size_t) pos * nchan]) : 0;
        r.out_count = cap;
        r.out_data  = buffer.data ();
        unsigned i0 = io, o0 = cap;
        if (r.process ()) return std::vector<float> ();
        unsigned used = i0 - r.inp_count, made = o0 - r.out_count;
        pos += used;
        out.insert (out.end (), buffer.begin (), buffer.begin () + made * nchan);
        produced += made;
        if (!used && !made) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// coordinate: API contract.
// ---------------------------------------------------------------------------
template <class R>
int coordinate_api (void)
{
    typedef Engine<R> E;
    R r;
    double d = 0;
    double inf2 [2] = { 0.0, std::numeric_limits<double>::infinity () };

    // Unconfigured: every new call reports failure.
    TASK_CHECK (r.nchan () == 0);
    TASK_CHECK (r.prepare_channel_alignment (4) != 0);
    TASK_CHECK (r.set_channel_offsets (inf2, 2) != 0);
    TASK_CHECK (r.channel_inpdist (0, &d) != 0);

    TASK_CHECK (E::setup (r, 1.0, 2) == 0);
    int k = r.inpsize ();
    TASK_CHECK (k > 0);

    // Not prepared: the new calls fail, old API unaffected.
    TASK_CHECK (r.set_channel_offsets (inf2, 2) != 0);
    TASK_CHECK (r.channel_inpdist (0, &d) != 0);
    TASK_CHECK (r.inpsize () == k);

    // max_shift valid range is 1..32; inpsize grows by 2*max_shift.
    TASK_CHECK (r.prepare_channel_alignment (0) != 0);
    TASK_CHECK (r.prepare_channel_alignment (33) != 0);
    TASK_CHECK (r.prepare_channel_alignment (32) == 0);
    TASK_CHECK (r.inpsize () == k + 64);
    TASK_CHECK (r.prepare_channel_alignment (32) == 0);   // idempotent
    TASK_CHECK (r.prepare_channel_alignment (4) == 0);    // re-parameterised
    TASK_CHECK (r.inpsize () == k + 8);

    // channel_inpdist bounds / NULL.
    TASK_CHECK (r.channel_inpdist (0, &d) == 0);
    TASK_CHECK (r.channel_inpdist (1, &d) == 0);
    TASK_CHECK (r.channel_inpdist (2, &d) != 0);
    TASK_CHECK (r.channel_inpdist (0, 0) != 0);

    // set: count, finiteness and range are validated atomically.
    double good [2] = { -3.25, 3.75 };
    double nan2 [2] = { std::numeric_limits<double>::quiet_NaN (), 0.0 };
    double hi   [2] = { 0.0, 4.0001 };
    double lo   [2] = { -4.0001, 0.0 };
    double one  [1] = { 0.0 };
    TASK_CHECK (r.set_channel_offsets (0, 2) != 0);
    TASK_CHECK (r.set_channel_offsets (one, 1) != 0);
    TASK_CHECK (r.set_channel_offsets (one, 3) != 0);
    TASK_CHECK (r.set_channel_offsets (inf2, 2) != 0);
    TASK_CHECK (r.set_channel_offsets (nan2, 2) != 0);
    TASK_CHECK (r.set_channel_offsets (hi, 2) != 0);
    TASK_CHECK (r.set_channel_offsets (lo, 2) != 0);

    TASK_CHECK (r.set_channel_offsets (good, 2) == 0);
    double a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    TASK_CHECK (r.channel_inpdist (0, &a0) == 0);
    TASK_CHECK (r.channel_inpdist (1, &a1) == 0);
    TASK_CHECK (std::fabs (a0 - (r.inpdist () + 3.25)) < 1e-12);
    TASK_CHECK (std::fabs (a1 - (r.inpdist () - 3.75)) < 1e-12);

    // A later rejected update leaves the stored offsets untouched.
    TASK_CHECK (r.set_channel_offsets (hi, 2) != 0);
    TASK_CHECK (r.channel_inpdist (0, &b0) == 0);
    TASK_CHECK (r.channel_inpdist (1, &b1) == 0);
    TASK_CHECK (b0 == a0 && b1 == a1);

    // Exact endpoints are accepted.
    double edge [2] = { -4.0, 4.0 };
    TASK_CHECK (r.set_channel_offsets (edge, 2) == 0);

    // clear () revokes preparation.
    r.clear ();
    TASK_CHECK (r.set_channel_offsets (good, 2) != 0);
    TASK_CHECK (r.channel_inpdist (0, &d) != 0);

    // A second setup () also revokes it; inpsize returns to K.
    TASK_CHECK (E::setup (r, 1.0, 2) == 0);
    TASK_CHECK (r.inpsize () == k);
    TASK_CHECK (r.set_channel_offsets (good, 2) != 0);
    TASK_CHECK (r.prepare_channel_alignment (8) == 0);
    TASK_CHECK (E::setup (r, 1.0, 2) == 0);
    TASK_CHECK (r.inpsize () == k);
    TASK_CHECK (r.set_channel_offsets (good, 2) != 0);
    return 0;
}

// prepare is only allowed after setup/reset and before any I/O is committed.
template <class R>
int coordinate_prepare_timing (void)
{
    typedef Engine<R> E;
    {
        R r;
        TASK_CHECK (r.prepare_channel_alignment (8) != 0);
        TASK_CHECK (E::setup (r, 1.0, 2) == 0);
        TASK_CHECK (r.prepare_channel_alignment (8) == 0);
    }
    {
        // Consuming input revokes the right to prepare. Ask for one output
        // frame but supply only one input: it is stored, no output is made.
        R r;
        TASK_CHECK (E::setup (r, 1.0, 2) == 0);
        TASK_CHECK (r.prepare_channel_alignment (8) == 0);
        std::vector<float> in (4096 * 2, 0.0f);
        r.inp_count = 1;
        r.inp_data = in.data ();
        r.out_count = 1;
        r.out_data = 0;
        TASK_CHECK (r.process () == 0);
        TASK_CHECK (r.inp_count == 0);          // the frame was committed
        TASK_CHECK (r.prepare_channel_alignment (8) != 0);
        // reset re-arms it and keeps the preparation/offsets.
        TASK_CHECK (r.reset () == 0);
        TASK_CHECK (r.prepare_channel_alignment (8) == 0);
        double off [2] = { -1.0, 2.0 };
        TASK_CHECK (r.set_channel_offsets (off, 2) == 0);
    }
    {
        // Producing output revokes it as well.
        R r;
        TASK_CHECK (E::setup (r, 1.0, 2) == 0);
        TASK_CHECK (r.prepare_channel_alignment (8) == 0);
        std::vector<float> in (4096 * 2, 0.0f), out (16 * 2);
        r.inp_count = 4096;
        r.inp_data = in.data ();
        r.out_count = 16;
        r.out_data = out.data ();
        TASK_CHECK (r.process () == 0);
        TASK_CHECK (r.prepare_channel_alignment (8) != 0);
    }
    return 0;
}

// inpdist stays the common nominal position; channel_inpdist = inpdist-offset.
template <class R>
int coordinate_inpdist_nominal (void)
{
    typedef Engine<R> E;
    R r;
    TASK_CHECK (E::setup (r, 0.75, 3) == 0);
    TASK_CHECK (r.prepare_channel_alignment (6) == 0);
    double off [3] = { -6.0, 0.0, 2.5 };
    TASK_CHECK (r.set_channel_offsets (off, 3) == 0);

    std::vector<float> in (4096 * 3, 0.0f), out (32 * 3);
    for (int step = 0; step < 30; ++step)
    {
        double nominal = r.inpdist ();
        for (unsigned c = 0; c < 3; ++c)
        {
            double d = 0;
            TASK_CHECK (r.channel_inpdist (c, &d) == 0);
            TASK_CHECK (std::fabs (d - (nominal - off [c])) < 1e-9);
        }
        r.inp_count = 9;
        r.inp_data = in.data ();
        r.out_count = 5;
        r.out_data = out.data ();
        TASK_CHECK (r.process () == 0);
    }
    // Setting offsets does not move the common position.
    double before = r.inpdist ();
    TASK_CHECK (r.set_channel_offsets (off, 3) == 0);
    TASK_CHECK (r.inpdist () == before);
    return 0;
}

// reset keeps the preparation and the offsets.
template <class R>
int coordinate_reset_keeps (void)
{
    typedef Engine<R> E;
    R r;
    TASK_CHECK (E::setup (r, 1.0, 2) == 0);
    TASK_CHECK (r.prepare_channel_alignment (5) == 0);
    double off [2] = { -5.0, 3.25 };
    TASK_CHECK (r.set_channel_offsets (off, 2) == 0);
    int sz = r.inpsize ();
    TASK_CHECK (r.reset () == 0);
    TASK_CHECK (r.inpsize () == sz);
    double d = 0;
    TASK_CHECK (r.channel_inpdist (0, &d) == 0);
    TASK_CHECK (std::fabs (d - (r.inpdist () + 5.0)) < 1e-12);
    TASK_CHECK (r.channel_inpdist (1, &d) == 0);
    TASK_CHECK (std::fabs (d - (r.inpdist () - 3.25)) < 1e-12);
    return 0;
}

// Pre-filling inpsize()/2-1 zero frames aligns the nominal source position
// to 0: inpdist() becomes zero. The first output still needs its full window.
template <class R>
int coordinate_prefill (void)
{
    typedef Engine<R> E;
    const double ratios [] = { 0.5, 1.0, 2.0 };
    for (unsigned ri = 0; ri < 3; ++ri)
    {
        R r;
        TASK_CHECK (E::setup (r, ratios [ri], 1) == 0);
        TASK_CHECK (r.prepare_channel_alignment (8) == 0);
        double zero = 0.0;
        TASK_CHECK (r.set_channel_offsets (&zero, 1) == 0);
        int k = r.inpsize ();
        TASK_CHECK ((k % 2) == 0);
        unsigned prefix = (unsigned) k / 2 - 1;
        unsigned N = 600;
        std::vector<float> stream (prefix + N, 0.0f);
        for (unsigned i = 0; i < N; ++i) stream [prefix + i] = (float) i;   // ramp
        std::vector<float> out (64);

        // Feed exactly the prefill with output discarded: inpdist reaches 0.
        r.inp_count = prefix;
        r.inp_data = stream.data ();
        r.out_count = prefix;
        r.out_data = 0;
        TASK_CHECK (r.process () == 0);
        TASK_CHECK (r.inp_count == 0);
        TASK_CHECK (std::fabs (r.inpdist ()) < 1e-9);

        // The first real output now waits for the rest of its window.
        unsigned pos = prefix;
        r.inp_count = N;
        r.inp_data = stream.data () + pos;
        r.out_count = 1;
        r.out_data = out.data ();
        TASK_CHECK (r.process () == 0);
        TASK_CHECK (r.out_count == 0);               // one frame produced
        TASK_CHECK (std::isfinite (out [0]));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// window: independent cubic reference for the CResampler.
// ---------------------------------------------------------------------------
double cubic4 (double y0, double y1, double y2, double y3, double f)
{
    double a = f, b = 1 - a, d = a * b / 2;
    return (-d * b) * y0 + (b + (3 * b - 1) * d) * y1
         + (a + (3 * a - 1) * d) * y2 + (-d * a) * y3;
}

int window_cubic_c (void)
{
    const double offs [] = { -7.75, -0.25, 0.0, 0.999, 8.0, -8.0, 3.5 };
    const double ratios [] = { 0.5, 1.0, 2.0 };
    for (unsigned ri = 0; ri < 3; ++ri)
        for (unsigned oi = 0; oi < sizeof (offs) / sizeof (offs [0]); ++oi)
        {
            double ratio = ratios [ri];
            unsigned ms = 8, N = 1400;
            std::vector<float> data (N);
            for (unsigned k = 0; k < N; ++k) data [k] = (float) sample_signal (k, 0);

            CResampler r;
            TASK_CHECK (r.setup (ratio, 1) == 0);
            TASK_CHECK (r.prepare_channel_alignment (ms) == 0);
            double o = offs [oi];
            TASK_CHECK (r.set_channel_offsets (&o, 1) == 0);
            std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, 1, data);
            Driver d = { 1, 61, 89, 600, false, 0 };
            std::vector<float> out = drive (r, stream, d, Noop ());
            TASK_CHECK (out.size () > 200);

            double maxerr = 0;
            for (unsigned n = 40; n < out.size (); ++n)
            {
                double p = n / ratio + o;
                double fl = std::floor (p);
                int j = (int) fl;
                double f = p - fl;
                double y [4];
                for (int t = 0; t < 4; ++t)
                {
                    int idx = j - 1 + t;
                    y [t] = (idx >= 0 && idx < (int) N) ? data [idx] : 0.0;
                }
                maxerr = std::max (maxerr, std::fabs (out [n] - cubic4 (y [0], y [1], y [2], y [3], f)));
            }
            TASK_CHECK (maxerr < 1e-5);
        }
    return 0;
}

// Per-channel impulse positions show that each offset selects its own window.
template <class R>
int window_impulse_positions (void)
{
    typedef Engine<R> E;
    unsigned nchan = 6, ms = 8, imp = 400, N = 900;
    double ratio = 1.0;
    double offs [6] = { -7.75, -0.25, 0.0, 0.999, 8.0, -8.0 };

    std::vector<float> data (N * nchan, 0.0f);
    for (unsigned c = 0; c < nchan; ++c) data [imp * nchan + c] = 1.0f;

    R r;
    TASK_CHECK (E::setup (r, ratio, nchan) == 0);
    TASK_CHECK (r.prepare_channel_alignment (ms) == 0);
    TASK_CHECK (r.set_channel_offsets (offs, nchan) == 0);
    std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
    Driver d = { nchan, 113, 97, 800, false, 0 };
    std::vector<float> out = drive (r, stream, d, Noop ());
    TASK_CHECK (out.size () == 800u * nchan);

    double nominal_centroid = 0;
    double centroid [6];
    for (unsigned c = 0; c < nchan; ++c)
    {
        double num = 0, den = 0;
        for (unsigned n = 0; n < 800; ++n)
        {
            double v = out [n * nchan + c];
            num += n * v * v;
            den += v * v;
        }
        TASK_CHECK (den > 0.0);
        centroid [c] = num / den;
        if (offs [c] == 0.0) nominal_centroid = centroid [c];
    }
    for (unsigned c = 0; c < nchan; ++c)
        TASK_CHECK (std::fabs (centroid [c] - (nominal_centroid - offs [c] * ratio)) < 0.35);

    // Identical offsets across channels give identical output (no mixing).
    R q;
    TASK_CHECK (E::setup (q, ratio, nchan) == 0);
    TASK_CHECK (q.prepare_channel_alignment (ms) == 0);
    double same [6] = { 2.5, 2.5, 2.5, 2.5, 2.5, 2.5 };
    TASK_CHECK (q.set_channel_offsets (same, nchan) == 0);
    Driver d2 = { nchan, 113, 97, 800, false, 0 };
    std::vector<float> out2 = drive (q, stream, d2, Noop ());
    TASK_CHECK (out2.size () == out.size ());
    for (unsigned n = 0; n < 800; ++n)
        for (unsigned c = 1; c < nchan; ++c)
            TASK_CHECK (out2 [n * nchan + c] == out2 [n * nchan + 0]);
    return 0;
}

// The window must keep support for a large offset introduced later, even
// though the current offsets were small the whole time.
template <class R>
int window_late_large (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 12, N = 900;
    double ratio = 1.0;
    double small [2] = { 0.0, 0.0 };
    double large [2] = { -12.0, 12.0 };

    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c);

    R r;
    TASK_CHECK (E::setup (r, ratio, nchan) == 0);
    TASK_CHECK (r.prepare_channel_alignment (ms) == 0);
    TASK_CHECK (r.set_channel_offsets (small, nchan) == 0);
    std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
    unsigned sw = 260;
    unsigned applied = ~0u;
    Driver d = { nchan, 53, 71, 600, false, 0 };
    std::vector<float> out = drive (r, stream, d, [&] (unsigned f)
    {
        if (applied == ~0u && f >= sw)
        {
            TASK_ASSERT (r.set_channel_offsets (large, nchan) == 0);
            applied = f;
        }
    });

    R q;
    TASK_CHECK (E::setup (q, ratio, nchan) == 0);
    TASK_CHECK (q.prepare_channel_alignment (ms) == 0);
    TASK_CHECK (q.set_channel_offsets (large, nchan) == 0);
    Driver dq = { nchan, 53, 71, 600, false, 0 };
    std::vector<float> out2 = drive (q, stream, dq, Noop ());
    TASK_CHECK (out2.size () == out.size ());
    TASK_CHECK (applied != ~0u);
    // The new offsets take effect from the next output frame after the
    // update, so compare strictly after `applied`.
    double maxdiff = 0;
    for (unsigned n = applied + 1; n < 600; ++n)
        for (unsigned c = 0; c < nchan; ++c)
            maxdiff = std::max (maxdiff, (double) std::fabs (out [n * nchan + c] - out2 [n * nchan + c]));
    TASK_CHECK (maxdiff <= 2e-6);
    return 0;
}

// Zero offsets on the aligned path reproduce the unaligned engine exactly.
template <class R>
int window_zero_eq (void)
{
    typedef Engine<R> E;
    unsigned nchan = 3, ms = 8, N = 700;
    double ratio = 0.8;
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c + 1);

    R a;
    TASK_CHECK (E::setup (a, ratio, nchan) == 0);
    std::vector<float> sa = padded ((unsigned) a.inpsize () / 2 - 1, nchan, data);
    Driver d = { nchan, 37, 53, 500, false, 0 };
    std::vector<float> oa = drive (a, sa, d, Noop ());

    R b;
    TASK_CHECK (E::setup (b, ratio, nchan) == 0);
    TASK_CHECK (b.prepare_channel_alignment (ms) == 0);
    double zero [3] = { 0.0, 0.0, 0.0 };
    TASK_CHECK (b.set_channel_offsets (zero, nchan) == 0);
    std::vector<float> sb = padded ((unsigned) b.inpsize () / 2 - 1, nchan, data);
    Driver db = { nchan, 37, 53, 500, false, 0 };
    std::vector<float> ob = drive (b, sb, db, Noop ());
    TASK_CHECK (!oa.empty () && oa.size () == ob.size ());
    double maxdiff = 0;
    for (size_t i = 0; i < oa.size (); ++i) maxdiff = std::max (maxdiff, (double) std::fabs (oa [i] - ob [i]));
    TASK_CHECK (maxdiff <= 1e-6);
    return 0;
}

// After long silence, switching to a large offset must not replay stale
// history: the signal that starts at the switch must be clean.
template <class R>
int window_silence_switch (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 10;
    double ratio = 1.0;
    double zero [2] = { 0.0, 0.0 };
    double other [2] = { -10.0, 10.0 };

    unsigned silence = 500, ND = 400;
    std::vector<float> data (ND * nchan);
    for (unsigned k = 0; k < ND; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) (0.4 * std::sin (0.03 * k + c));

    auto build = [&] (R &r, const double *offs, std::vector<float> &stream) -> unsigned
    {
        TASK_ASSERT (E::setup (r, ratio, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        TASK_ASSERT (r.set_channel_offsets (offs, nchan) == 0);
        unsigned prefix = (unsigned) r.inpsize () / 2 - 1;
        stream.assign ((size_t) (prefix + silence) * nchan, 0.0f);
        stream.insert (stream.end (), data.begin (), data.end ());
        return prefix + silence;
    };

    R r;
    std::vector<float> stream;
    unsigned sound_start = build (r, zero, stream);
    unsigned sw = (unsigned) (sound_start * ratio);
    unsigned applied = ~0u;
    Driver d = { nchan, 61, 43, sw + 260, false, 0 };
    std::vector<float> out = drive (r, stream, d, [&] (unsigned f)
    {
        if (applied == ~0u && f >= sw)
        {
            TASK_ASSERT (r.set_channel_offsets (other, nchan) == 0);
            applied = f;
        }
    });

    R q;
    std::vector<float> stream2;
    build (q, other, stream2);
    Driver dq = { nchan, 61, 43, sw + 260, false, 0 };
    std::vector<float> out2 = drive (q, stream2, dq, Noop ());
    TASK_CHECK (out2.size () == out.size ());
    TASK_CHECK (applied != ~0u);
    double maxdiff = 0;
    for (unsigned f = applied + 1; f < (unsigned) (out.size () / nchan); ++f)
        for (unsigned c = 0; c < nchan; ++c)
            maxdiff = std::max (maxdiff, (double) std::fabs (out [f * nchan + c] - out2 [f * nchan + c]));
    TASK_CHECK (maxdiff <= 2e-6);
    return 0;
}

// NULL output skips exactly those frames, leaving the sound intact.
template <class R>
int window_null_out (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 8, N = 900;
    double ratio = 1.1;
    double off [2] = { -4.5, 6.25 };
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c + 2);

    auto build = [&] (R &r) -> std::vector<float>
    {
        TASK_ASSERT (E::setup (r, ratio, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        TASK_ASSERT (r.set_channel_offsets (off, nchan) == 0);
        return padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
    };

    unsigned skip = 200, total = 600;
    R a;
    std::vector<float> sa = build (a);
    Driver da = { nchan, 113, 97, total, false, 0 };
    std::vector<float> oa = drive (a, sa, da, Noop ());

    R b;
    std::vector<float> sb = build (b);
    Driver db = { nchan, 113, 97, skip, true, 0 };
    TASK_CHECK (drive (b, sb, db, Noop ()).empty ());
    Driver dc = { nchan, 113, 97, total - skip, false, db.in_pos };
    std::vector<float> ob = drive (b, sb, dc, Noop ());
    TASK_CHECK (ob.size () == (total - skip) * nchan);
    double maxdiff = 0;
    for (unsigned f = 0; f < total - skip; ++f)
        for (unsigned c = 0; c < nchan; ++c)
            maxdiff = std::max (maxdiff,
                (double) std::fabs (ob [f * nchan + c] - oa [(skip + f) * nchan + c]));
    TASK_CHECK (maxdiff <= 2e-6);
    return 0;
}

// ---------------------------------------------------------------------------
// streaming: VResampler sine position accuracy.
// ---------------------------------------------------------------------------
int streaming_sine_v (void)
{
    const double ratios [] = { 0.5, 1.0, 2.0 };
    const double offs [] = { -7.75, -0.25, 0.0, 0.999, 8.0, -8.0 };
    const double w = 0.005;
    for (unsigned ri = 0; ri < 3; ++ri)
        for (unsigned oi = 0; oi < sizeof (offs) / sizeof (offs [0]); ++oi)
        {
            double ratio = ratios [ri];
            unsigned N = 5000;
            std::vector<float> data (N);
            for (unsigned k = 0; k < N; ++k) data [k] = (float) (0.25 * std::sin (2 * M_PI * w * k));

            VResampler r;
            TASK_CHECK (r.setup (ratio, 1, 32) == 0);
            TASK_CHECK (r.prepare_channel_alignment (8) == 0);
            double o = offs [oi];
            TASK_CHECK (r.set_channel_offsets (&o, 1) == 0);
            std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, 1, data);
            Driver d = { 1, 97, 83, 2200, false, 0 };
            std::vector<float> out = drive (r, stream, d, Noop ());
            TASK_CHECK (out.size () > 800);

            double maxerr = 0;
            for (unsigned n = 120; n + 120 < out.size (); ++n)
            {
                double p = n / ratio + o;
                double e = 0.25 * std::sin (2 * M_PI * w * p);
                maxerr = std::max (maxerr, std::fabs (out [n] - e));
            }
            TASK_CHECK (maxerr <= 2e-4);
        }
    return 0;
}

// Amplitude / polarity are preserved: peak of a 0.25 sine stays ~0.25.
template <class R>
int streaming_amplitude (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 6, N = 3000;
    double ratio = 1.0;
    double off [2] = { -6.0, 6.0 };
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c)
            data [k * nchan + c] = (float) (0.25 * std::sin (2 * M_PI * 0.005 * k));

    R r;
    TASK_CHECK (E::setup (r, ratio, nchan) == 0);
    TASK_CHECK (r.prepare_channel_alignment (ms) == 0);
    TASK_CHECK (r.set_channel_offsets (off, nchan) == 0);
    std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
    Driver d = { nchan, 71, 59, 2200, false, 0 };
    std::vector<float> out = drive (r, stream, d, Noop ());
    TASK_CHECK (!out.empty ());
    double peak = 0;
    for (unsigned n = 200; n + 200 < out.size () / nchan; ++n)
        for (unsigned c = 0; c < nchan; ++c)
            peak = std::max (peak, (double) std::fabs (out [n * nchan + c]));
    TASK_CHECK (peak > 0.2 && peak < 0.3);
    return 0;
}

// Same offset / ratio schedule, different chunking: identical frames.
template <class R>
int streaming_chunks (void)
{
    typedef Engine<R> E;
    unsigned nchan = 6, ms = 8, N = 4000, total = 2400;
    double ratio = 1.0;
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c);

    struct Step { unsigned frame; double offs [6]; double rratio; };
    const Step steps [] =
    {
        {    0, { -7.75, -0.25, 0.0, 0.999, 8.0, -8.0 }, 1.0  },
        {  300, {  7.75,  0.25, 0.0, -0.999, -8.0, 8.0 }, 0.95 },
        {  750, { -7.75, -0.25, 0.0, 0.999, 8.0, -8.0 }, 1.0  },
        { 1200, {  3.5,  1.25, -2.0, 0.5, -0.75, 6.0 }, 1.05 },
        { 1700, { -1.0,  2.0, -3.0, 4.0, -5.0, 6.0 }, 1.0  },
    };
    const unsigned nsteps = sizeof (steps) / sizeof (steps [0]);
    std::vector<ScheduleStep> sched (nsteps);
    for (unsigned s = 0; s < nsteps; ++s)
    {
        sched [s].frame = steps [s].frame;
        for (unsigned c = 0; c < 6; ++c) sched [s].offs [c] = steps [s].offs [c];
        sched [s].rratio = steps [s].rratio;
    }

    auto run = [&] (unsigned in_chunk, unsigned out_cap) -> std::vector<float>
    {
        R r;
        TASK_ASSERT (E::setup (r, ratio, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
        return drive_scheduled (r, stream, nchan, in_chunk, out_cap, total,
                                sched.data (), nsteps);
    };

    std::vector<float> ref = run (N, total);
    TASK_CHECK (!ref.empty ());
    const unsigned chunks [][2] =
    {
        { 1, 113 }, { 7, 113 }, { 113, 113 }, { 1, 1 },
        { 7, 3 }, { 113, 97 }, { 50, 13 },
    };
    for (unsigned i = 0; i < sizeof (chunks) / sizeof (chunks [0]); ++i)
    {
        std::vector<float> got = run (chunks [i][0], chunks [i][1]);
        TASK_CHECK (got.size () == ref.size ());
        double maxdiff = 0;
        for (size_t k = 0; k < got.size (); ++k)
            maxdiff = std::max (maxdiff, (double) std::fabs (got [k] - ref [k]));
        TASK_CHECK (maxdiff <= 2e-6);
    }
    return 0;
}

// Mid-stream update: new value applies from the next output frame and does
// not change the common position or already delivered sound.
template <class R>
int streaming_update (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 8, N = 2000;
    double ratio = 1.0;
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c + 3);

    double a [2] = { -8.0, 8.0 }, b [2] = { 8.0, -8.0 };
    auto build = [&] (R &r) -> std::vector<float>
    {
        TASK_ASSERT (E::setup (r, ratio, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        TASK_ASSERT (r.set_channel_offsets (a, nchan) == 0);
        return padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
    };

    R r;
    std::vector<float> stream = build (r);
    Driver d0 = { nchan, 97, 61, 400, false, 0 };
    std::vector<float> pre = drive (r, stream, d0, Noop ());
    TASK_CHECK (pre.size () == 400u * nchan);
    double nominal = r.inpdist ();
    TASK_CHECK (r.set_channel_offsets (b, nchan) == 0);
    TASK_CHECK (r.inpdist () == nominal);
    Driver d1 = { nchan, 97, 61, 600, false, d0.in_pos };
    std::vector<float> post = drive (r, stream, d1, Noop ());

    R q;
    std::vector<float> stream2 = build (q);
    Driver e0 = { nchan, 97, 61, 400, false, 0 };
    std::vector<float> pre2 = drive (q, stream2, e0, Noop ());
    TASK_CHECK (pre2.size () == pre.size ());
    TASK_CHECK (q.set_channel_offsets (b, nchan) == 0);
    Driver e1 = { nchan, 97, 61, 600, false, e0.in_pos };
    std::vector<float> post2 = drive (q, stream2, e1, Noop ());
    TASK_CHECK (post2.size () == post.size ());
    double maxdiff = 0;
    for (size_t i = 0; i < post.size (); ++i)
        maxdiff = std::max (maxdiff, (double) std::fabs (post [i] - post2 [i]));
    TASK_CHECK (maxdiff <= 2e-6);
    return 0;
}

// Force many internal buffer relocations with a large ratio and tight output.
template <class R>
int streaming_moves (void)
{
    typedef Engine<R> E;
    unsigned nchan = 6, ms = 8, N = 3000;
    double ratio = 2.0;
    double offs [6] = { -8.0, -4.5, -0.25, 0.999, 5.0, 8.0 };
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c);

    auto run = [&] (unsigned in_chunk, unsigned out_cap) -> std::vector<float>
    {
        R r;
        TASK_ASSERT (E::setup (r, ratio, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        TASK_ASSERT (r.set_channel_offsets (offs, nchan) == 0);
        std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
        Driver d = { nchan, in_chunk, out_cap, 3000, false, 0 };
        return drive (r, stream, d, Noop ());
    };
    std::vector<float> ref = run (N, 3000);
    TASK_CHECK (!ref.empty ());
    const unsigned chunks [][2] = { { 1, 3 }, { 7, 5 }, { 113, 97 }, { 3, 1 }, { 50, 13 } };
    for (unsigned i = 0; i < sizeof (chunks) / sizeof (chunks [0]); ++i)
    {
        std::vector<float> got = run (chunks [i][0], chunks [i][1]);
        TASK_CHECK (got.size () == ref.size ());
        double maxdiff = 0;
        for (size_t k = 0; k < got.size (); ++k)
            maxdiff = std::max (maxdiff, (double) std::fabs (got [k] - ref [k]));
        TASK_CHECK (maxdiff <= 2e-6);
    }
    return 0;
}

// reset reuses the prepared object, keeping offsets, and yields the sound of
// a fresh prepared object.
template <class R>
int streaming_reset (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 8, N = 1000;
    double ratio = 1.0;
    double off [2] = { -3.5, 4.25 };
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c);

    R r;
    TASK_CHECK (E::setup (r, ratio, nchan) == 0);
    TASK_CHECK (r.prepare_channel_alignment (ms) == 0);
    TASK_CHECK (r.set_channel_offsets (off, nchan) == 0);
    std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
    Driver d = { nchan, 97, 71, 500, false, 0 };
    std::vector<float> first = drive (r, stream, d, Noop ());
    TASK_CHECK (!first.empty ());
    int sz = r.inpsize ();
    TASK_CHECK (r.reset () == 0);
    TASK_CHECK (r.inpsize () == sz);
    std::vector<float> second = drive (r, stream, { nchan, 97, 71, 500, false, 0 }, Noop ());
    TASK_CHECK (second.size () == first.size ());
    double maxdiff = 0;
    for (size_t i = 0; i < first.size (); ++i)
        maxdiff = std::max (maxdiff, (double) std::fabs (first [i] - second [i]));
    TASK_CHECK (maxdiff <= 2e-6);
    double dist = 0;
    TASK_CHECK (r.channel_inpdist (0, &dist) == 0);
    TASK_CHECK (r.channel_inpdist (1, &dist) == 0);
    return 0;
}

// Rejected updates never change the sound; process and valid updates do not
// allocate.
template <class R>
int streaming_noalloc (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 8, N = 1200;
    double ratio = 1.0;
    double good [2] = { -2.5, 3.5 };
    double bad [2] = { 0.0, 9.0 };
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c);

    auto build = [&] (R &r) -> std::vector<float>
    {
        TASK_ASSERT (E::setup (r, ratio, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        TASK_ASSERT (r.set_channel_offsets (good, nchan) == 0);
        return padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
    };

    R a;
    std::vector<float> sa = build (a);
    Driver d = { nchan, 83, 67, 700, false, 0 };
    std::vector<float> oa = drive (a, sa, d, Noop ());

    // Same run, but interleave rejected updates and disable allocation checks
    // around each call.
    R b;
    std::vector<float> sb = build (b);
    std::vector<float> buffer (67 * nchan), ob;
    ob.reserve (700 * nchan + 67 * nchan);          // no growth inside the loop
    unsigned pos = 0, produced = 0;
    long before = g_allocs;
    while (produced < 700)
    {
        TASK_CHECK (b.set_channel_offsets (bad, nchan) != 0);
        unsigned frames = (unsigned) (sb.size () / nchan);
        unsigned io = frames - pos < 83u ? frames - pos : 83u;
        unsigned oo = 700 - produced < 67u ? 700 - produced : 67u;
        b.inp_count = io;
        b.inp_data = io ? &sb [(size_t) pos * nchan] : 0;
        b.out_count = oo;
        b.out_data = buffer.data ();
        unsigned i0 = io, o0 = oo;
        TASK_CHECK (b.process () == 0);
        unsigned used = i0 - b.inp_count, made = o0 - b.out_count;
        pos += used;
        produced += made;
        ob.insert (ob.end (), buffer.begin (), buffer.begin () + made * nchan);
        if (!used && !made) break;
    }
    TASK_CHECK (g_allocs == before);
    TASK_CHECK (ob.size () == oa.size ());
    double maxdiff = 0;
    for (size_t i = 0; i < ob.size (); ++i)
        maxdiff = std::max (maxdiff, (double) std::fabs (ob [i] - oa [i]));
    TASK_CHECK (maxdiff <= 2e-6);

    // Valid updates are allocation free.
    long b2 = g_allocs;
    TASK_CHECK (b.set_channel_offsets (good, nchan) == 0);
    TASK_CHECK (b.set_channel_offsets (good, nchan) == 0);
    TASK_CHECK (g_allocs == b2);
    return 0;
}

// NULL input inserts silence, matching an explicit zero stream.
template <class R>
int streaming_null_in (void)
{
    typedef Engine<R> E;
    unsigned nchan = 2, ms = 6;
    double ratio = 1.0;
    double off [2] = { -4.0, 4.0 };
    unsigned zeros = 400, N = 500;
    std::vector<float> data (N * nchan);
    for (unsigned k = 0; k < N; ++k)
        for (unsigned c = 0; c < nchan; ++c) data [k * nchan + c] = (float) sample_signal (k, c + 1);

    auto build = [&] (R &r) -> unsigned
    {
        TASK_ASSERT (E::setup (r, ratio, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        TASK_ASSERT (r.set_channel_offsets (off, nchan) == 0);
        return (unsigned) r.inpsize () / 2 - 1;
    };

    // Reference: explicit zero frames then data.
    R a;
    unsigned prefix = build (a);
    std::vector<float> zstream ((size_t) (prefix + zeros) * nchan, 0.0f);
    zstream.insert (zstream.end (), data.begin (), data.end ());
    Driver d = { nchan, 53, 47, 500, false, 0 };
    std::vector<float> ref = drive (a, zstream, d, Noop ());
    TASK_CHECK (!ref.empty ());

    // Subject: NULL input for the zero region, then the real data.
    R b;
    build (b);
    std::vector<float> buffer (47 * nchan), out;
    out.reserve (500 * nchan + 47 * nchan);
    unsigned left = prefix + zeros, pos = 0, produced = 0;
    long before = g_allocs;
    while (left > 0 || pos < N)
    {
        unsigned io, use_null = 0;
        if (left > 0)
        {
            io = left < 53u ? left : 53u;
            use_null = 1;
        }
        else
        {
            io = (N - pos) < 53u ? (N - pos) : 53u;
        }
        unsigned oo = 500 - produced < 47u ? 500 - produced : 47u;
        b.inp_count = io;
        b.inp_data = use_null ? 0 : &data [(size_t) pos * nchan];
        b.out_count = oo;
        b.out_data = buffer.data ();
        unsigned i0 = io, o0 = oo;
        TASK_CHECK (b.process () == 0);
        unsigned used = i0 - b.inp_count, made = o0 - b.out_count;
        if (use_null) left -= used; else pos += used;
        produced += made;
        out.insert (out.end (), buffer.begin (), buffer.begin () + made * nchan);
        if (!used && !made) break;
        if (produced >= 500) break;
    }
    TASK_CHECK (g_allocs == before);
    TASK_CHECK (out.size () == ref.size ());
    double maxdiff = 0;
    for (size_t i = 0; i < out.size (); ++i)
        maxdiff = std::max (maxdiff, (double) std::fabs (out [i] - ref [i]));
    TASK_CHECK (maxdiff <= 2e-6);
    return 0;
}

// set_phase / set_rratio / set_rrfilt keep their meaning alongside offsets.
int streaming_setters_v (void)
{
    unsigned nchan = 1, N = 2000;
    std::vector<float> data (N);
    for (unsigned k = 0; k < N; ++k) data [k] = (float) sample_signal (k, 0);
    double off [1] = { 2.0 };

    auto run = [&] (double phase, double rratio, double rrfilt) -> std::vector<float>
    {
        VResampler r;
        TASK_ASSERT (r.setup (0.9, nchan, 32) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (4) == 0);
        TASK_ASSERT (r.set_channel_offsets (off, nchan) == 0);
        r.set_phase (phase);
        r.set_rrfilt (rrfilt);
        r.set_rratio (rratio);
        std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
        Driver d = { nchan, 97, 71, 800, false, 0 };
        return drive (r, stream, d, Noop ());
    };

    std::vector<float> base = run (0.0, 1.0, 0.0);
    TASK_CHECK (!base.empty ());
    std::vector<float> phased = run (0.3, 1.0, 0.0);
    TASK_CHECK (phased.size () == base.size ());
    double diff = 0;
    for (size_t i = 0; i < base.size (); ++i)
        diff = std::max (diff, (double) std::fabs (base [i] - phased [i]));
    TASK_CHECK (diff > 1e-3);

    // set_rrfilt smooths a ratio change instead of applying it instantly, so
    // the two runs differ but stay finite.
    std::vector<float> smooth = run (0.0, 1.02, 50.0);
    std::vector<float> abrupt = run (0.0, 1.02, 0.0);
    TASK_CHECK (smooth.size () == abrupt.size ());
    double sdiff = 0;
    for (size_t i = 0; i < smooth.size (); ++i)
    {
        TASK_CHECK (std::isfinite (smooth [i]));
        sdiff = std::max (sdiff, (double) std::fabs (smooth [i] - abrupt [i]));
    }
    TASK_CHECK (sdiff > 0.0);

    // inpdist vs channel_inpdist relation holds through set_phase.
    VResampler r;
    TASK_CHECK (r.setup (0.9, nchan, 32) == 0);
    TASK_CHECK (r.prepare_channel_alignment (4) == 0);
    TASK_CHECK (r.set_channel_offsets (off, nchan) == 0);
    r.set_phase (0.25);
    double d = 0;
    TASK_CHECK (r.channel_inpdist (0, &d) == 0);
    TASK_CHECK (std::fabs (d - (r.inpdist () - 2.0)) < 1e-12);
    return 0;
}

// set_phase / set_rratio keep their meaning alongside offsets.
template <class R>
int streaming_setters (void)
{
    typedef Engine<R> E;
    unsigned nchan = 1, ms = 4, N = 2000;
    std::vector<float> data (N);
    for (unsigned k = 0; k < N; ++k) data [k] = (float) sample_signal (k, 0);
    double off [1] = { 2.0 };

    auto run = [&] (double phase, double rratio) -> std::vector<float>
    {
        R r;
        TASK_ASSERT (E::setup (r, 0.9, nchan) == 0);
        TASK_ASSERT (r.prepare_channel_alignment (ms) == 0);
        TASK_ASSERT (r.set_channel_offsets (off, nchan) == 0);
        r.set_phase (phase);
        E::set_rratio (r, rratio);
        std::vector<float> stream = padded ((unsigned) r.inpsize () / 2 - 1, nchan, data);
        Driver d = { nchan, 97, 71, 800, false, 0 };
        return drive (r, stream, d, Noop ());
    };

    std::vector<float> base = run (0.0, 1.0);
    TASK_CHECK (!base.empty ());
    // set_phase changes the common sample position while offsets stay valid.
    std::vector<float> phased = run (0.3, 1.0);
    TASK_CHECK (phased.size () == base.size ());
    double diff = 0;
    for (size_t i = 0; i < base.size (); ++i)
        diff = std::max (diff, (double) std::fabs (base [i] - phased [i]));
    TASK_CHECK (diff > 1e-3);

    // set_rratio still changes the output rate.
    std::vector<float> faster = run (0.0, 1.02);
    TASK_CHECK (!faster.empty ());

    // The documented relationship between inpdist and channel_inpdist holds
    // for every phase.
    R r;
    TASK_CHECK (E::setup (r, 0.9, nchan) == 0);
    TASK_CHECK (r.prepare_channel_alignment (ms) == 0);
    TASK_CHECK (r.set_channel_offsets (off, nchan) == 0);
    r.set_phase (0.25);
    double d = 0;
    TASK_CHECK (r.channel_inpdist (0, &d) == 0);
    TASK_CHECK (std::fabs (d - (r.inpdist () - 2.0)) < 1e-12);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------
#define VCASE(g, n, f) { g, n, f }
const task_case task_cases [] =
{
    VCASE ("coordinate", "V-api", coordinate_api<VResampler>),
    VCASE ("coordinate", "V-prepare-timing", coordinate_prepare_timing<VResampler>),
    VCASE ("coordinate", "V-inpdist-nominal", coordinate_inpdist_nominal<VResampler>),
    VCASE ("coordinate", "V-reset-keeps", coordinate_reset_keeps<VResampler>),
    VCASE ("coordinate", "V-prefill", coordinate_prefill<VResampler>),
    VCASE ("window", "V-impulse", window_impulse_positions<VResampler>),
    VCASE ("window", "V-late-large", window_late_large<VResampler>),
    VCASE ("window", "V-zero-eq", window_zero_eq<VResampler>),
    VCASE ("window", "V-silence", window_silence_switch<VResampler>),
    VCASE ("window", "V-null-out", window_null_out<VResampler>),
    VCASE ("streaming", "V-sine", streaming_sine_v),
    VCASE ("streaming", "V-amplitude", streaming_amplitude<VResampler>),
    VCASE ("streaming", "V-chunks", streaming_chunks<VResampler>),
    VCASE ("streaming", "V-update", streaming_update<VResampler>),
    VCASE ("streaming", "V-moves", streaming_moves<VResampler>),
    VCASE ("streaming", "V-reset", streaming_reset<VResampler>),
    VCASE ("streaming", "V-noalloc", streaming_noalloc<VResampler>),
    VCASE ("streaming", "V-null-in", streaming_null_in<VResampler>),
    VCASE ("streaming", "V-setters", streaming_setters_v),

    VCASE ("coordinate", "C-api", coordinate_api<CResampler>),
    VCASE ("coordinate", "C-prepare-timing", coordinate_prepare_timing<CResampler>),
    VCASE ("coordinate", "C-inpdist-nominal", coordinate_inpdist_nominal<CResampler>),
    VCASE ("coordinate", "C-reset-keeps", coordinate_reset_keeps<CResampler>),
    VCASE ("coordinate", "C-prefill", coordinate_prefill<CResampler>),
    VCASE ("window", "C-cubic", window_cubic_c),
    VCASE ("window", "C-impulse", window_impulse_positions<CResampler>),
    VCASE ("window", "C-late-large", window_late_large<CResampler>),
    VCASE ("window", "C-zero-eq", window_zero_eq<CResampler>),
    VCASE ("window", "C-silence", window_silence_switch<CResampler>),
    VCASE ("window", "C-null-out", window_null_out<CResampler>),
    VCASE ("streaming", "C-amplitude", streaming_amplitude<CResampler>),
    VCASE ("streaming", "C-chunks", streaming_chunks<CResampler>),
    VCASE ("streaming", "C-update", streaming_update<CResampler>),
    VCASE ("streaming", "C-moves", streaming_moves<CResampler>),
    VCASE ("streaming", "C-reset", streaming_reset<CResampler>),
    VCASE ("streaming", "C-noalloc", streaming_noalloc<CResampler>),
    VCASE ("streaming", "C-null-in", streaming_null_in<CResampler>),
    VCASE ("streaming", "C-setters", streaming_setters<CResampler>),
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]);
