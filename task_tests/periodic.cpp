// Regression tests for PandaResampler::PeriodicPlan.
//
// Registered through task_support/check.h (no own main): three groups
// closure, orbit, streaming. Everything here uses only the C++17 standard
// library plus the production PandaResampler headers.

#include "pandaresampler.hh"
#include "task_support/check.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <vector>

using PandaResampler::PeriodicPlan;
using PandaResampler::Resampler2;

/* ---------------- heap activity guard ---------------- */

static std::atomic<unsigned int> g_new_calls{0};

void* operator new (std::size_t n)
{
  ++g_new_calls;
  void *p = std::malloc (n);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[] (std::size_t n)
{
  ++g_new_calls;
  void *p = std::malloc (n);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete (void *p) noexcept { std::free (p); }
void operator delete[] (void *p) noexcept { std::free (p); }
void operator delete (void *p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void *p, std::size_t) noexcept { std::free (p); }

/* ---------------- small helpers ---------------- */

static float
wave_sample (int kind, unsigned i, unsigned n)
{
  switch (kind) {
    case 0:   // DC
      return 0.375f;
    case 1:   // boundary pulse (sits on the loop seam)
      return (i == 0) ? 0.9f : 0.0f;
    case 2: { // asymmetric deterministic loop
      const double ph = 6.283185307179586 * i / n;
      return float (0.5 * std::sin (ph) + 0.25 * std::sin (3.0 * ph + 0.7) + (i % 3) * 0.03 - 0.04);
    }
  }
  return 0.0f;
}

static std::vector<float>
make_loop (int kind, unsigned n)
{
  std::vector<float> v (n);
  for (unsigned i = 0; i < n; i++) v[i] = wave_sample (kind, i, n);
  return v;
}

static float
max_abs_diff (const std::vector<float>& a, const std::vector<float>& b)
{
  float m = 0.0f;
  const size_t k = std::min (a.size(), b.size());
  for (size_t i = 0; i < k; i++) m = std::max (m, std::fabs (a[i] - b[i]));
  return m;
}

/* Production scalar IIR resampler, fed the repeated loop until fully settled,
 * then one more output period is captured. This reference is independent of
 * PeriodicPlan (it drives the ordinary zero-state Resampler2 by brute force). */
static std::vector<float>
reference_converged (Resampler2::Mode mode, unsigned ratio,
                     Resampler2::Precision prec, const std::vector<float>& loop,
                     unsigned turns = 4096)
{
  const unsigned n = unsigned (loop.size());
  Resampler2 rs (mode, ratio, prec, false, Resampler2::FILTER_IIR);
  const unsigned in_per  = (mode == Resampler2::UP) ? n : n * ratio;
  const unsigned out_per = (mode == Resampler2::UP) ? n * ratio : n;

  std::vector<float> in (in_per), scratch (out_per), steady (out_per);
  for (unsigned i = 0; i < in_per; i++) in[i] = loop[i % n];

  for (unsigned t = 0; t < turns; t++)
    rs.process_block (in.data(), in_per, scratch.data());
  rs.process_block (in.data(), in_per, steady.data());
  return steady;
}

static std::unique_ptr<PeriodicPlan>
make_plan (Resampler2::Mode mode, unsigned ratio, Resampler2::Precision prec,
           const std::vector<std::vector<float>>& planar)
{
  std::vector<const float*> ptrs (planar.size());
  for (size_t c = 0; c < planar.size(); c++) ptrs[c] = planar[c].data();
  return std::make_unique<PeriodicPlan> (mode, ratio, prec,
                                         unsigned (planar.size()),
                                         ptrs.data(), planar[0].size());
}

static std::vector<float>
render_mono (PeriodicPlan& plan, size_t frames)
{
  std::vector<float> out (frames);
  float *p = out.data();
  float *const pp[1] = { p };
  plan.render (const_cast<float* const*> (pp), frames);
  return out;
}

/* Independent from-scratch halfband allpass reference (mirrors the hiir
 * polyphase recurrence but is its own implementation) with its own periodic
 * boundary solve. Used for the factor-2 closure cases. */
struct RefAllpass {
  std::vector<double> coef;   // NC coefficients
  std::vector<float>  mem;    // NC+2 memories
  int nc() const { return int (coef.size()); }
  void clear() { std::fill (mem.begin(), mem.end(), 0.0f); }
  // exact analogue of hiir StageProcFpu::process_sample_pos
  void pos (float& s0, float& s1)
  {
    int remaining = nc();
    int cnt = 2;
    while (remaining >= 2) {
      const float t0 = (s0 - mem[cnt])     * float (coef[cnt - 2]) + mem[cnt - 2];
      const float t1 = (s1 - mem[cnt + 1]) * float (coef[cnt + 1 - 2]) + mem[cnt - 1];
      mem[cnt - 2] = s0;
      mem[cnt - 1] = s1;
      s0 = t0;
      s1 = t1;
      remaining -= 2;
      cnt += 2;
    }
    if (remaining == 1) {
      const float t0 = (s0 - mem[cnt]) * float (coef[cnt - 2]) + mem[cnt - 2];
      mem[cnt - 2] = s0;
      mem[cnt - 1] = s1;
      mem[cnt] = t0;
      s0 = t0;
    } else {
      mem[cnt - 2] = s0;
      mem[cnt - 1] = s1;
    }
  }
  void up (float in, float& o0, float& o1)
  { float e = in, o = in; pos (e, o); o0 = e; o1 = o; }
  float down (float i0, float i1)
  { float s0 = i1, s1 = i0; pos (s0, s1); return 0.5f * (s0 + s1); }
};

static void
ref_gauss (std::vector<double>& A, std::vector<double>& b, int k)
{
  for (int col = 0; col < k; col++) {
    int piv = col;
    double best = std::fabs (A[col * k + col]);
    for (int r = col + 1; r < k; r++)
      if (std::fabs (A[r * k + col]) > best) { best = std::fabs (A[r * k + col]); piv = r; }
    for (int c = 0; c < k; c++) std::swap (A[col * k + c], A[piv * k + c]);
    std::swap (b[col], b[piv]);
    const double d = A[col * k + col];
    for (int c = 0; c < k; c++) A[col * k + c] /= d;
    b[col] /= d;
    for (int r = 0; r < k; r++) {
      if (r == col) continue;
      const double f = A[r * k + col];
      for (int c = 0; c < k; c++) A[r * k + c] -= f * A[col * k + c];
      b[r] -= f * b[col];
    }
  }
}

/* Expected steady output for a factor-2 conversion, computed entirely with
 * RefAllpass and an independent periodic boundary equation. */
static std::vector<float>
independent_ratio2 (Resampler2::Mode mode, Resampler2::Precision prec,
                    const std::vector<float>& loop)
{
  const unsigned n = unsigned (loop.size());
  const bool up = (mode == Resampler2::UP);

  // production coefficient value, but the recursion and solve are local
  Resampler2 rs (mode, 2, prec, false, Resampler2::FILTER_IIR);
  const int nc = int (rs.iir_stage_ncoefs (2));
  std::vector<double> c (nc);
  rs.iir_stage_coefs (2, c.data());

  const unsigned g = unsigned (std::gcd (n, 2u));
  const size_t orbit = up ? size_t (n) : size_t (n) * (2u / g);
  const size_t steps = up ? orbit : orbit / 2;   // filter steps over the orbit
  const int M = nc + 2;

  // run the independent allpass for one orbit; input provider supplies samples
  auto run_orbit = [&] (RefAllpass& f, bool forcing) {
    size_t ti = 0;
    for (size_t s = 0; s < steps; s++) {
      if (up) {
        const float x = forcing ? loop[ti % n] : 0.0f;
        float a, b; f.up (x, a, b);
        ti += 1;
      } else {
        const float x0 = forcing ? loop[ti % n] : 0.0f;
        const float x1 = forcing ? loop[(ti + 1) % n] : 0.0f;
        (void) f.down (x0, x1);
        ti += 2;
      }
    }
  };

  // measure transition map A (zero forcing) with the independent filter
  std::vector<double> base (M, 0.0), col (M), A (size_t (M) * M, 0.0);
  {
    RefAllpass f; f.coef = c; f.mem.assign (M, 0.0f);
    run_orbit (f, false);
    for (int i = 0; i < M; i++) base[i] = f.mem[i];
  }
  for (int j = 0; j < M; j++) {
    RefAllpass f; f.coef = c; f.mem.assign (M, 0.0f);
    f.mem[j] = 1.0f;
    run_orbit (f, false);
    for (int i = 0; i < M; i++) col[i] = f.mem[i] - base[i];
    for (int i = 0; i < M; i++) A[size_t (i) * M + j] = (i == j ? 1.0 : 0.0) - col[i];
  }
  // forcing b with the real loop from a zero state
  std::vector<double> b (M, 0.0);
  {
    RefAllpass f; f.coef = c; f.mem.assign (M, 0.0f);
    run_orbit (f, true);
    for (int i = 0; i < M; i++) b[i] = f.mem[i];
  }
  std::vector<double> sol = b;
  ref_gauss (A, sol, M);

  RefAllpass f; f.coef = c; f.mem.resize (M);
  for (int i = 0; i < M; i++) f.mem[i] = float (sol[i]);

  const size_t P = up ? size_t (n) * 2 : size_t (n) / g;
  std::vector<float> out;
  out.reserve (P);
  if (up) {
    for (unsigned i = 0; i < n; i++) {
      float a, bb; f.up (loop[i], a, bb);
      out.push_back (a); out.push_back (bb);
    }
  } else {
    size_t t = 0;
    while (out.size() < P) {
      const float x0 = loop[t % n];
      const float x1 = loop[(t + 1) % n];
      out.push_back (f.down (x0, x1));
      t += 2;
    }
  }
  return out;
}

/* ============================ closure ============================ */

struct ClosureCase {
  const char          *name;
  Resampler2::Mode     mode;
  unsigned             ratio;
  Resampler2::Precision prec;
  unsigned             n;
  int                  kind;
  bool                 independent;
};

static int
run_closure (const ClosureCase& tc)
{
  const auto loop = make_loop (tc.kind, tc.n);
  auto plan = make_plan (tc.mode, tc.ratio, tc.prec, { loop });

  const uint64_t P = plan->period();
  const auto first  = render_mono (*plan, size_t (P));
  const auto second = render_mono (*plan, size_t (P));
  const auto third  = render_mono (*plan, size_t (P));

  // the solved orbit must close on itself, cycle after cycle
  TASK_CHECK (max_abs_diff (first, second) <= 2e-5f);
  TASK_CHECK (max_abs_diff (first, third)  <= 2e-5f);
  for (float v : first) TASK_CHECK (std::isfinite (v));

  // match the fully converged ordinary scalar IIR resampler
  const auto ref = reference_converged (tc.mode, tc.ratio, tc.prec, loop);
  TASK_CHECK (ref.size() == first.size() ||
              ref.size() >= first.size());
  TASK_CHECK (max_abs_diff (first, ref) <= 2e-5f);

  if (tc.independent) {
    const auto ind = independent_ratio2 (tc.mode, tc.prec, loop);
    TASK_CHECK (ind.size() == first.size());
    TASK_CHECK (max_abs_diff (first, ind) <= 2e-5f);
  }
  return 0;
}

static int closure_dc1()    { ClosureCase c{ "dc_n1",     Resampler2::UP,   4, Resampler2::PREC_96DB,  1, 0, false }; return run_closure (c); }
static int closure_dc3()    { ClosureCase c{ "dc_n3",     Resampler2::DOWN, 8, Resampler2::PREC_48DB,  3, 0, false }; return run_closure (c); }
static int closure_dc17()   { ClosureCase c{ "dc_n17",    Resampler2::UP,   2, Resampler2::PREC_144DB, 17, 0, true  }; return run_closure (c); }
static int closure_dc64()   { ClosureCase c{ "dc_n64",    Resampler2::DOWN, 4, Resampler2::PREC_96DB,  64, 0, false }; return run_closure (c); }
static int closure_pulse3() { ClosureCase c{ "pulse_n3",  Resampler2::DOWN, 2, Resampler2::PREC_48DB,  3, 1, true  }; return run_closure (c); }
static int closure_pulse17(){ ClosureCase c{ "pulse_n17", Resampler2::UP,   8, Resampler2::PREC_48DB,  17, 1, false }; return run_closure (c); }
static int closure_pulse64(){ ClosureCase c{ "pulse_n64", Resampler2::DOWN, 2, Resampler2::PREC_96DB,  64, 1, true  }; return run_closure (c); }
static int closure_asym17() { ClosureCase c{ "asym_n17",  Resampler2::UP,   2, Resampler2::PREC_96DB,  17, 2, true  }; return run_closure (c); }
static int closure_asym64() { ClosureCase c{ "asym_n64",  Resampler2::DOWN, 8, Resampler2::PREC_144DB, 64, 2, false }; return run_closure (c); }
static int closure_odd_down(){ ClosureCase c{ "odd_multiloop", Resampler2::DOWN, 4, Resampler2::PREC_144DB, 3, 2, false }; return run_closure (c); }

/* ============================= orbit ============================= */

static int
run_orbit (Resampler2::Mode mode, unsigned ratio, Resampler2::Precision prec, unsigned n)
{
  const auto loop = make_loop (2, n);
  auto plan = make_plan (mode, ratio, prec, { loop });

  const uint64_t g = std::gcd (uint64_t (n), uint64_t (ratio));
  const uint64_t expect = (mode == Resampler2::UP) ? uint64_t (n) * ratio
                                                    : uint64_t (n) / g;
  TASK_CHECK (plan->period() == expect);

  const auto p0 = render_mono (*plan, size_t (expect));
  // three further periods must all equal the first (closed orbit)
  for (int cyc = 0; cyc < 3; cyc++) {
    const auto pk = render_mono (*plan, size_t (expect));
    TASK_CHECK (max_abs_diff (p0, pk) <= 2e-6f);
  }
  // seek back to the start and confirm reset reproduces the same period
  plan->reset();
  const auto pr = render_mono (*plan, size_t (expect));
  TASK_CHECK (max_abs_diff (p0, pr) <= 2e-6f);
  for (float v : p0) TASK_CHECK (std::isfinite (v));
  return 0;
}

static int orbit_2_48_up()  { return run_orbit (Resampler2::UP,   2, Resampler2::PREC_48DB,  5); }
static int orbit_2_96_dn()  { return run_orbit (Resampler2::DOWN, 2, Resampler2::PREC_96DB,  7); }
static int orbit_4_48_dn()  { return run_orbit (Resampler2::DOWN, 4, Resampler2::PREC_48DB,  3); }
static int orbit_4_144_up() { return run_orbit (Resampler2::UP,   4, Resampler2::PREC_144DB, 9); }
static int orbit_8_96_up()  { return run_orbit (Resampler2::UP,   8, Resampler2::PREC_96DB,  11); }
static int orbit_8_144_dn() { return run_orbit (Resampler2::DOWN, 8, Resampler2::PREC_144DB, 13); }
static int orbit_odd_n1()   { return run_orbit (Resampler2::DOWN, 8, Resampler2::PREC_96DB,  1); }
static int orbit_odd_n17()  { return run_orbit (Resampler2::DOWN, 2, Resampler2::PREC_144DB, 17); }

/* =========================== streaming ============================ */

static std::vector<std::vector<float>>
make_planar (unsigned channels, unsigned n, int pattern)
{
  std::vector<std::vector<float>> data (channels, std::vector<float> (n));
  for (unsigned c = 0; c < channels; c++)
    for (unsigned i = 0; i < n; i++) {
      const float base = wave_sample (2, i, n);
      if (pattern == 0)          // two channels in anti-phase
        data[c][i] = (c == 0) ? base : -base;
      else                       // three mutually distinct channels
        data[c][i] = base * (1.0f + 0.25f * c) + 0.1f * c * std::sin (i + c);
    }
  return data;
}

static int
run_streaming (unsigned channels, int pattern)
{
  const unsigned n = 23;
  const Resampler2::Mode mode = Resampler2::UP;
  const unsigned ratio = 4;
  const auto prec = Resampler2::PREC_96DB;
  auto planar = make_planar (channels, n, pattern);
  auto plan = make_plan (mode, ratio, prec, planar);
  const uint64_t P = plan->period();

  const size_t total = size_t (P) * 3 + 17;
  std::vector<std::vector<float>> whole (channels, std::vector<float> (total));
  {
    std::vector<float*> ptrs (channels);
    for (unsigned c = 0; c < channels; c++) ptrs[c] = whole[c].data();
    plan->render (ptrs.data(), total);
  }

  // render must not touch the heap regardless of block size
  static const size_t chunks[] = { 1, 7, 31 };
  for (size_t chunk : chunks) {
    plan->reset();
    std::vector<std::vector<float>> split (channels, std::vector<float> (total));
    std::vector<float*> ptrs (channels);
    size_t done = 0;
    while (done < total) {
      const size_t k = std::min (chunk, total - done);
      for (unsigned c = 0; c < channels; c++) ptrs[c] = split[c].data() + done;
      const unsigned int before = g_new_calls.load();
      plan->render (ptrs.data(), k);                 // guard wraps render only
      TASK_CHECK (g_new_calls.load() == before);     // no allocation during render
      done += k;
    }
    for (unsigned c = 0; c < channels; c++)
      TASK_CHECK (max_abs_diff (whole[c], split[c]) <= 2e-6f);
  }

  // a zero-length render does not advance the stream
  {
    std::vector<std::vector<float>> a (channels, std::vector<float> (8));
    std::vector<std::vector<float>> b (channels, std::vector<float> (8));
    std::vector<float*> ap (channels), bp (channels);
    plan->reset();
    for (unsigned c = 0; c < channels; c++) ap[c] = a[c].data();
    plan->render (ap.data(), 8);
    plan->reset();
    plan->render (nullptr, 0);                 // must be a no-op
    for (unsigned c = 0; c < channels; c++) bp[c] = b[c].data();
    plan->render (bp.data(), 8);
    for (unsigned c = 0; c < channels; c++)
      TASK_CHECK (max_abs_diff (a[c], b[c]) == 0.0f);
  }

  // each channel equals an independent single-channel plan (no cross-talk)
  for (unsigned c = 0; c < channels; c++) {
    auto solo = make_plan (mode, ratio, prec, { planar[c] });
    const auto ref = render_mono (*solo, total);
    TASK_CHECK (max_abs_diff (whole[c], ref) <= 2e-6f);
  }

  // interleaved seeks, including cursors above 2^53 reduced with integer mod
  const uint64_t big = (uint64_t (1) << 54) + 123456789ULL;
  const uint64_t positions[] = {
    0ULL, 1ULL, P - 1, P, P + 5, 2 * P + 3,
    uint64_t (1) << 53, big, big + P + 7
  };
  for (uint64_t pos : positions) {
    const uint64_t suffix = pos % P;                 // integer, never via double
    plan->seek (pos);
    const size_t k = 13;
    std::vector<std::vector<float>> got (channels, std::vector<float> (k));
    std::vector<float*> ptrs (channels);
    for (unsigned c = 0; c < channels; c++) ptrs[c] = got[c].data();
    plan->render (ptrs.data(), k);
    // compare against a fresh stream advanced exactly `suffix` frames
    auto p2 = make_plan (mode, ratio, prec, planar);
    p2->seek (suffix);
    std::vector<std::vector<float>> want (channels, std::vector<float> (k));
    std::vector<float*> wptrs (channels);
    for (unsigned c = 0; c < channels; c++) wptrs[c] = want[c].data();
    p2->render (wptrs.data(), k);
    for (unsigned c = 0; c < channels; c++)
      TASK_CHECK (max_abs_diff (got[c], want[c]) <= 2e-6f);
  }
  return 0;
}

static int streaming_stereo() { return run_streaming (2, 0); }
static int streaming_three()  { return run_streaming (3, 1); }

/* --------- in-use legacy converter must be untouched by plans --------- */

static int
legacy_undisturbed()
{
  const unsigned n = 31, blocks = 64;
  std::vector<float> in (n);
  for (unsigned i = 0; i < n; i++) in[i] = wave_sample (2, i, n);

  // R runs the whole stream uninterrupted and is the reference trajectory
  Resampler2 R (Resampler2::UP, 2, Resampler2::PREC_96DB, false, Resampler2::FILTER_IIR);
  Resampler2 A (Resampler2::UP, 2, Resampler2::PREC_96DB, false, Resampler2::FILTER_IIR);
  std::vector<std::vector<float>> ref_out (blocks, std::vector<float> (n * 2));
  std::vector<std::vector<float>> a_out (blocks, std::vector<float> (n * 2));

  for (unsigned t = 0; t < blocks; t++)
    R.process_block (in.data(), n, ref_out[t].data());

  for (unsigned t = 0; t < blocks / 2; t++)
    A.process_block (in.data(), n, a_out[t].data());
  for (unsigned t = 0; t < blocks / 2; t++)
    TASK_CHECK (max_abs_diff (ref_out[t], a_out[t]) == 0.0f);

  // build and tear down plans (and a deliberately failing construction) next
  {
    auto loop = make_loop (1, 17);
    auto p1 = make_plan (Resampler2::UP, 4, Resampler2::PREC_48DB, { loop });
    auto p2 = make_plan (Resampler2::DOWN, 8, Resampler2::PREC_144DB, { loop, loop });
    (void) render_mono (*p1, 40);
    std::vector<float> p2_0 (40), p2_1 (40);
    float *p2p[2] = { p2_0.data(), p2_1.data() };
    p2->render (p2p, 40);
  }
  bool threw = false;
  try {
    float bad[2] = { 1.0f, std::nanf ("") };
    const float *pp[1] = { bad };
    PeriodicPlan badplan (Resampler2::UP, 2, Resampler2::PREC_48DB, 1, pp, 2);
  } catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK (threw);

  // A must still follow the uninterrupted reference trajectory exactly
  for (unsigned t = blocks / 2; t < blocks; t++) {
    A.process_block (in.data(), n, a_out[t].data());
    TASK_CHECK (max_abs_diff (ref_out[t], a_out[t]) == 0.0f);
  }
  return 0;
}

/* --------------------------- invalid arguments --------------------------- */

static int
invalid_args()
{
  auto loop = make_loop (0, 8);
  const float *pp[1] = { loop.data() };
  auto expect_throw = [&] (Resampler2::Mode m, unsigned r, Resampler2::Precision pr,
                           unsigned chn, const float *const *src, size_t fr) {
    bool threw = false;
    try { PeriodicPlan q (m, r, pr, chn, src, fr); }
    catch (const std::invalid_argument&) { threw = true; }
    return threw;
  };
  TASK_CHECK (expect_throw (Resampler2::UP, 3, Resampler2::PREC_48DB, 1, pp, 8));    // bad ratio
  TASK_CHECK (expect_throw (Resampler2::UP, 2, Resampler2::PREC_72DB, 1, pp, 8));    // bad precision
  TASK_CHECK (expect_throw (Resampler2::UP, 2, Resampler2::PREC_48DB, 0, pp, 8));    // 0 channels
  TASK_CHECK (expect_throw (Resampler2::UP, 2, Resampler2::PREC_48DB, 9, pp, 8));    // >8 channels
  TASK_CHECK (expect_throw (Resampler2::UP, 2, Resampler2::PREC_48DB, 1, pp, 0));    // empty
  TASK_CHECK (expect_throw (Resampler2::UP, 2, Resampler2::PREC_48DB, 1, pp, 65537));// too long
  TASK_CHECK (expect_throw (Resampler2::UP, 2, Resampler2::PREC_48DB, 1, nullptr, 8));
  std::vector<float> nanloop (4, 1.0f);
  nanloop[2] = std::numeric_limits<float>::infinity();
  const float *np[1] = { nanloop.data() };
  TASK_CHECK (expect_throw (Resampler2::UP, 2, Resampler2::PREC_48DB, 1, np, 4));   // non-finite
  // a good construction still works after the failures
  auto good = make_plan (Resampler2::UP, 2, Resampler2::PREC_48DB, { loop });
  TASK_CHECK (good->period() == 16);
  return 0;
}

/* ============================ registration ============================ */

extern "C" const struct task_case task_cases[] = {
  { "closure", "dc_n1",        closure_dc1 },
  { "closure", "dc_n3",        closure_dc3 },
  { "closure", "dc_n17",       closure_dc17 },
  { "closure", "dc_n64",       closure_dc64 },
  { "closure", "pulse_n3",     closure_pulse3 },
  { "closure", "pulse_n17",    closure_pulse17 },
  { "closure", "pulse_n64",    closure_pulse64 },
  { "closure", "asym_n17",     closure_asym17 },
  { "closure", "asym_n64",     closure_asym64 },
  { "closure", "odd_multiloop", closure_odd_down },
  { "orbit", "r2_48_up",       orbit_2_48_up },
  { "orbit", "r2_96_down",     orbit_2_96_dn },
  { "orbit", "r4_48_down",     orbit_4_48_dn },
  { "orbit", "r4_144_up",      orbit_4_144_up },
  { "orbit", "r8_96_up",       orbit_8_96_up },
  { "orbit", "r8_144_down",    orbit_8_144_dn },
  { "orbit", "odd_n1",         orbit_odd_n1 },
  { "orbit", "odd_n17",        orbit_odd_n17 },
  { "streaming", "stereo",     streaming_stereo },
  { "streaming", "threech",    streaming_three },
  { "streaming", "legacy_undisturbed", legacy_undisturbed },
  { "streaming", "invalid_args", invalid_args },
};
extern "C" const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases[0]);
