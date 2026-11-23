// This Source Code Form is licensed MPL-2.0: http://mozilla.org/MPL/2.0
//
// PeriodicPlan: periodically steady-stated, multi-channel scalar IIR
// resampling. The steady state is found with a finite periodic boundary
// calculation (a measured state-transition map plus one direct linear solve),
// never with warm-up, convergence iteration, cross-fading or table playback.
// render() always runs the real hiir FPU IIR recursion with the production
// coefficients and state topology.

#include "pandaresampler.hh"
#include "pandaresampler/hiir/Upsampler2xFpu.h"
#include "pandaresampler/hiir/Downsampler2xFpu.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace PandaResampler {

namespace {

using hiir::Downsampler2xFpu;
using hiir::StageDataFpu;
using hiir::Upsampler2xFpu;

/* ------------------------------------------------------------------ */
/* Type-erased scalar production filter stage (one hiir FPU instance) */
/* ------------------------------------------------------------------ */

struct IStage {
  virtual ~IStage() = default;
  virtual int  nmem() const = 0;
  virtual void get_mem (float *out) const = 0;
  virtual void set_mem (const float *in) = 0;
  virtual void clear() = 0;
  virtual void set_coefs (const double *coefs) = 0;
  virtual void up_step (float in, float &out0, float &out1) = 0;
  virtual float down_step (float in0, float in1) = 0;
};

template <int NC>
struct UpStage final : public IStage {
  Upsampler2xFpu<NC> f;
  int  nmem() const override { return NC + 2; }
  void get_mem (float *out) const override {
    const StageDataFpu<float> *s = f.use_stages();
    for (int i = 0; i < NC + 2; i++) out[i] = s[i]._mem;
  }
  void set_mem (const float *in) override {
    StageDataFpu<float> *s = f.use_stages();
    for (int i = 0; i < NC + 2; i++) s[i]._mem = in[i];
  }
  void clear() override { f.clear_buffers(); }
  void set_coefs (const double *coefs) override { f.set_coefs (coefs); }
  void up_step (float in, float &out0, float &out1) override {
    f.process_sample (out0, out1, in);
  }
  float down_step (float, float) override { return 0.0f; }
};

template <int NC>
struct DownStage final : public IStage {
  Downsampler2xFpu<NC> f;
  int  nmem() const override { return NC + 2; }
  void get_mem (float *out) const override {
    const StageDataFpu<float> *s = f.use_stages();
    for (int i = 0; i < NC + 2; i++) out[i] = s[i]._mem;
  }
  void set_mem (const float *in) override {
    StageDataFpu<float> *s = f.use_stages();
    for (int i = 0; i < NC + 2; i++) s[i]._mem = in[i];
  }
  void clear() override { f.clear_buffers(); }
  void set_coefs (const double *coefs) override { f.set_coefs (coefs); }
  void up_step (float, float &, float &) override { }
  float down_step (float in0, float in1) override {
    float pair[2] = { in0, in1 };
    return f.process_sample (pair);
  }
};

template <template <int> class StageT>
static std::unique_ptr<IStage>
make_n_stage (int nc, const double *coefs)
{
  switch (nc) {
    case 1: { auto p = std::make_unique<StageT<1>>(); p->set_coefs (coefs); return p; }
    case 2: { auto p = std::make_unique<StageT<2>>(); p->set_coefs (coefs); return p; }
    case 3: { auto p = std::make_unique<StageT<3>>(); p->set_coefs (coefs); return p; }
    case 4: { auto p = std::make_unique<StageT<4>>(); p->set_coefs (coefs); return p; }
    case 5: { auto p = std::make_unique<StageT<5>>(); p->set_coefs (coefs); return p; }
    case 6: { auto p = std::make_unique<StageT<6>>(); p->set_coefs (coefs); return p; }
    case 7: { auto p = std::make_unique<StageT<7>>(); p->set_coefs (coefs); return p; }
    case 8: { auto p = std::make_unique<StageT<8>>(); p->set_coefs (coefs); return p; }
    case 9: { auto p = std::make_unique<StageT<9>>(); p->set_coefs (coefs); return p; }
  }
  return nullptr;
}

static std::unique_ptr<IStage>
make_stage (bool up, int nc, const double *coefs)
{
  return up ? make_n_stage<UpStage> (nc, coefs)
            : make_n_stage<DownStage> (nc, coefs);
}

/* --------------------------- stage definition ---------------------- */

struct StageDef {
  bool                up;
  int                 ncoefs;
  std::vector<double> coefs;   // production coefficients, shared by channels
};

/* Concatenated memory size of a whole pipeline. */
static int
total_memory (const std::vector<StageDef>& defs)
{
  int k = 0;
  for (const auto& d : defs) k += d.ncoefs + 2;
  return k;
}

static std::vector<std::unique_ptr<IStage>>
build_pipeline (const std::vector<StageDef>& defs)
{
  std::vector<std::unique_ptr<IStage>> stages;
  stages.reserve (defs.size());
  for (const auto& d : defs)
    stages.push_back (make_stage (d.up, d.ncoefs, d.coefs.data()));
  return stages;
}

static void
clear_pipeline (std::vector<std::unique_ptr<IStage>>& stages)
{
  for (auto& s : stages) s->clear();
}

static void
get_state (const std::vector<std::unique_ptr<IStage>>& stages, std::vector<double>& m)
{
  size_t pos = 0;
  for (auto& s : stages) {
    std::vector<float> tmp (s->nmem());
    s->get_mem (tmp.data());
    for (int i = 0; i < s->nmem(); i++) m[pos + i] = tmp[i];
    pos += s->nmem();
  }
}

static void
set_state (std::vector<std::unique_ptr<IStage>>& stages, const std::vector<double>& m)
{
  size_t pos = 0;
  for (auto& s : stages) {
    std::vector<float> tmp (s->nmem());
    for (int i = 0; i < s->nmem(); i++) tmp[i] = float (m[pos + i]);
    s->set_mem (tmp.data());
    pos += s->nmem();
  }
}

/* Push a complete input orbit through every stage (construction time only,
 * so heap-using scratch buffers are allowed here). */
static std::vector<float>
run_pipeline (std::vector<std::unique_ptr<IStage>>& stages,
              bool                                  up,
              const std::vector<float>&             input)
{
  std::vector<float> cur = input;
  for (auto& s : stages) {
    if (up) {
      std::vector<float> nxt (cur.size() * 2);
      for (size_t i = 0; i < cur.size(); i++)
        s->up_step (cur[i], nxt[2 * i], nxt[2 * i + 1]);
      cur.swap (nxt);
    } else {
      std::vector<float> nxt (cur.size() / 2);
      for (size_t i = 0; i < nxt.size(); i++)
        nxt[i] = s->down_step (cur[2 * i], cur[2 * i + 1]);
      cur.swap (nxt);
    }
  }
  return cur;
}

/* Solve A x = b for a small dense system (Gauss-Jordan, partial pivoting).
 * mat is K x K row-major and is overwritten; b becomes the solution. */
static bool
gauss_solve (std::vector<double>& mat, std::vector<double>& b, int k)
{
  for (int col = 0; col < k; col++) {
    int piv = col;
    double best = std::fabs (mat[col * k + col]);
    for (int r = col + 1; r < k; r++) {
      double v = std::fabs (mat[r * k + col]);
      if (v > best) { best = v; piv = r; }
    }
    if (best < 1e-300) return false;
    if (piv != col) {
      for (int c = 0; c < k; c++) std::swap (mat[col * k + c], mat[piv * k + c]);
      std::swap (b[col], b[piv]);
    }
    double d = mat[col * k + col];
    for (int c = 0; c < k; c++) mat[col * k + c] /= d;
    b[col] /= d;
    for (int r = 0; r < k; r++) {
      if (r == col) continue;
      double f = mat[r * k + col];
      if (f == 0.0) continue;
      for (int c = 0; c < k; c++) mat[r * k + c] -= f * mat[col * k + c];
      b[r] -= f * b[col];
    }
  }
  return true;
}

/* ----------------------------- runtime engine ---------------------- */

/* Bounded queue between pipeline stages. The pull recursion below performs a
 * single filter step at a time, so a stage never buffers more than a handful
 * of samples (an up step emits two, a down step consumes two); this fixed
 * inline capacity always suffices and render() never touches the heap. */
static constexpr size_t RING_CAP = 32;
struct Ring {
  float  buf[RING_CAP];
  size_t head = 0;   // next write
  size_t tail = 0;   // next read
  size_t count() const { return head - tail; }
  void   reset() { head = tail = 0; }
  void   push (float v) { buf[head++ % RING_CAP] = v; }
  float  pop() { return buf[tail++ % RING_CAP]; }
};

struct ChannelRuntime {
  std::vector<std::unique_ptr<IStage>> stages;
  std::vector<std::vector<float>>      start_mem;   // solved periodic state
  std::vector<Ring>                    rings;       // one per stage
  size_t                               src_pos = 0;

  explicit ChannelRuntime (const std::vector<StageDef>& defs)
  {
    stages = build_pipeline (defs);
    rings.resize (defs.size());
    start_mem.resize (defs.size());
    for (size_t i = 0; i < defs.size(); i++)
      start_mem[i].resize (defs[i].ncoefs + 2);
  }
  int nstages() const { return int (stages.size()); }
};

} // anonymous namespace

/* ============================== Impl =============================== */

struct PeriodicPlan::Impl {
  Resampler2::Mode      mode;
  uint                  ratio;
  Resampler2::Precision precision;
  uint                  channels;
  size_t                n;                          // source frames N
  uint64_t              period_out;                 // P
  bool                  up;
  size_t                orbit_src;                  // source frames per closed orbit

  std::vector<StageDef>              defs;          // shared coefficients
  std::vector<std::vector<float>>    source;        // [channel][N], copied
  std::vector<ChannelRuntime>        rt;            // one runtime per channel

  Impl (Resampler2::Mode m, uint r, Resampler2::Precision p,
        uint chn, const float *const *src, size_t frames);

  float pull_input (uint ch, ChannelRuntime& c, int stage_index);
  void  produce (uint ch, ChannelRuntime& c, int stage_index);
  void  reset_runtime (ChannelRuntime& c);
  void  advance (uint ch, ChannelRuntime& c, uint64_t frames);
};

/* Collect the active production IIR stages, in processing order, together
 * with their real coefficients. A scalar Resampler2 (SSE disabled) is used
 * purely as the coefficient/topology source through its public introspection
 * helpers; no coefficient table is copied into this file. */
static std::vector<StageDef>
collect_stage_defs (Resampler2::Mode mode, uint ratio, Resampler2::Precision precision)
{
  Resampler2 rs (mode, ratio, precision, false, Resampler2::FILTER_IIR);

  uint stage_ratios[3];
  int n_stages = 0;
  if (mode == Resampler2::UP) {
    if (ratio >= 2) stage_ratios[n_stages++] = 2;
    if (ratio >= 4) stage_ratios[n_stages++] = 4;
    if (ratio >= 8) stage_ratios[n_stages++] = 8;
  } else {
    if (ratio >= 8) stage_ratios[n_stages++] = 8;
    if (ratio >= 4) stage_ratios[n_stages++] = 4;
    if (ratio >= 2) stage_ratios[n_stages++] = 2;
  }

  std::vector<StageDef> defs;
  for (int k = 0; k < n_stages; k++) {
    const uint sr = stage_ratios[k];
    const uint nc = rs.iir_stage_ncoefs (sr);
    if (nc == 0) continue;
    StageDef d;
    d.up = (mode == Resampler2::UP);
    d.ncoefs = int (nc);
    d.coefs.resize (nc);
    rs.iir_stage_coefs (sr, d.coefs.data());
    defs.push_back (std::move (d));
  }
  return defs;
}

PeriodicPlan::Impl::Impl (Resampler2::Mode      m,
                          uint                  r,
                          Resampler2::Precision p,
                          uint                  chn,
                          const float *const   *src,
                          size_t                frames)
{
  /* ---- validate everything before mutating any persistent state ---- */
  if (r != 2 && r != 4 && r != 8)
    throw std::invalid_argument ("PeriodicPlan: ratio must be 2, 4 or 8");
  if (p != Resampler2::PREC_48DB &&
      p != Resampler2::PREC_96DB &&
      p != Resampler2::PREC_144DB)
    throw std::invalid_argument ("PeriodicPlan: precision must be PREC_48DB, PREC_96DB or PREC_144DB");
  if (chn < 1 || chn > 8)
    throw std::invalid_argument ("PeriodicPlan: channels must be in 1..8");
  if (frames < 1 || frames > 65536)
    throw std::invalid_argument ("PeriodicPlan: loop length N must be in 1..65536");
  if (src == nullptr)
    throw std::invalid_argument ("PeriodicPlan: null source pointer");
  for (uint c = 0; c < chn; c++) {
    if (src[c] == nullptr)
      throw std::invalid_argument ("PeriodicPlan: null source channel pointer");
    for (size_t i = 0; i < frames; i++) {
      if (!std::isfinite (src[c][i]))
        throw std::invalid_argument ("PeriodicPlan: non-finite source sample");
    }
  }

  mode = m;
  ratio = r;
  precision = p;
  channels = chn;
  n = frames;
  up = (mode == Resampler2::UP);

  const uint64_t g = std::gcd (uint64_t (n), uint64_t (ratio));
  const uint64_t loops = uint64_t (ratio) / g;       // source loops to close (down)
  if (up) {
    period_out = uint64_t (n) * ratio;               // N*R
    orbit_src = n;                                    // one source loop
  } else {
    period_out = uint64_t (n) / g;                    // N/gcd(N,R)
    orbit_src = size_t (uint64_t (n) * loops);        // R/gcd source loops
  }

  defs = collect_stage_defs (mode, ratio, precision);
  const int K = total_memory (defs);

  /* copy the source loop once */
  source.assign (channels, std::vector<float> (n));
  for (uint c = 0; c < chn; c++)
    for (size_t i = 0; i < n; i++)
      source[c][i] = src[c][i];

  /* ---- measure the shared linear state-transition map over one orbit.
   * With zero forcing, final_state = A * initial_state. A is a property of
   * the coefficients/topology only and is therefore shared by all channels. */
  auto model = build_pipeline (defs);
  std::vector<double> b0 (K), col (K);
  std::vector<double> mat (size_t (K) * K, 0.0);     // I - A
  std::vector<float>  zero_orbit (orbit_src, 0.0f);

  clear_pipeline (model);
  run_pipeline (model, up, zero_orbit);
  get_state (model, b0);                              // homogeneous offset baseline

  for (int j = 0; j < K; j++) {
    clear_pipeline (model);
    std::vector<float> e (K, 0.0f);
    e[j] = 1.0f;
    set_state (model, std::vector<double> (e.begin(), e.end()));
    run_pipeline (model, up, zero_orbit);
    get_state (model, col);
    for (int i = 0; i < K; i++) {
      double aij = col[i] - b0[i];                    // column j of A
      mat[size_t (i) * K + j] = (i == j ? 1.0 : 0.0) - aij;
    }
  }

  /* factorize once: (I-A) is small and depends only on the shared
   * coefficients/topology; it is reused unchanged for every channel below. */

  /* per-channel forcing and periodic fixed point */
  rt.reserve (channels);
  for (uint c = 0; c < channels; c++) {
    rt.emplace_back (defs);
    ChannelRuntime& cr = rt.back();

    std::vector<float> orbit_in (orbit_src);
    for (size_t t = 0; t < orbit_src; t++)
      orbit_in[t] = source[c][t % n];

    /* forcing term b: orbit response from the zero state */
    clear_pipeline (cr.stages);
    run_pipeline (cr.stages, up, orbit_in);
    std::vector<double> b (K);
    get_state (cr.stages, b);

    std::vector<double> work = mat;                   // I - A
    std::vector<double> sol = b;
    if (!gauss_solve (work, sol, K))
      throw std::invalid_argument ("PeriodicPlan: periodic state solve failed");

    /* install the solved steady state and snapshot it */
    set_state (cr.stages, sol);
    size_t pos = 0;
    for (int s = 0; s < cr.nstages(); s++) {
      for (int i = 0; i < cr.stages[s]->nmem(); i++)
        cr.start_mem[s][i] = float (sol[pos + i]);
      pos += cr.stages[s]->nmem();
    }
    for (auto& ring : cr.rings) ring.reset();
    cr.src_pos = 0;
  }
}

/* --------------------------- streaming render ----------------------- */

float
PeriodicPlan::Impl::pull_input (uint ch, ChannelRuntime& c, int stage_index)
{
  if (stage_index == 0) {
    const float v = source[ch][c.src_pos % n];
    c.src_pos++;
    return v;
  }
  const int prev = stage_index - 1;
  if (c.rings[prev].count() == 0)
    produce (ch, c, prev);
  return c.rings[prev].pop();
}

/* Run exactly one filter step on one stage, recursively pulling its inputs.
 * One step adds at most two samples (up) or one (down) to a ring, and the
 * recursion drains them again, so the fixed-size rings never overflow. */
void
PeriodicPlan::Impl::produce (uint ch, ChannelRuntime& c, int stage_index)
{
  Ring& ring = c.rings[stage_index];
  IStage *s = c.stages[stage_index].get();
  if (up) {
    const float in = pull_input (ch, c, stage_index);
    float a, b;
    s->up_step (in, a, b);
    ring.push (a);
    ring.push (b);
  } else {
    const float i0 = pull_input (ch, c, stage_index);
    const float i1 = pull_input (ch, c, stage_index);
    ring.push (s->down_step (i0, i1));
  }
}

void
PeriodicPlan::Impl::reset_runtime (ChannelRuntime& c)
{
  for (int s = 0; s < c.nstages(); s++)
    c.stages[s]->set_mem (c.start_mem[s].data());
  for (auto& ring : c.rings) ring.reset();
  c.src_pos = 0;
}

void
PeriodicPlan::Impl::advance (uint ch, ChannelRuntime& c, uint64_t frames)
{
  const int last = c.nstages() - 1;
  for (uint64_t done = 0; done < frames; done++) {
    if (c.rings[last].count() == 0)
      produce (ch, c, last);
    (void) c.rings[last].pop();
  }
}

/* ============================ public API ============================ */

PeriodicPlan::PeriodicPlan (Resampler2::Mode      mode,
                            uint                  ratio,
                            Resampler2::Precision  precision,
                            uint                  channels,
                            const float *const   *source,
                            size_t                frames)
  : impl_ (std::make_unique<Impl> (mode, ratio, precision, channels, source, frames))
{
}

PeriodicPlan::~PeriodicPlan() = default;

uint64_t
PeriodicPlan::period() const
{
  return impl_->period_out;
}

void
PeriodicPlan::render (float *const *output, size_t frames)
{
  if (frames == 0)
    return;
  if (output == nullptr)
    throw std::invalid_argument ("PeriodicPlan::render: null output pointer");
  for (uint c = 0; c < impl_->channels; c++) {
    if (output[c] == nullptr)
      throw std::invalid_argument ("PeriodicPlan::render: null output channel pointer");
  }

  Impl& self = *impl_;
  const int last = self.rt[0].nstages() - 1;
  for (uint c = 0; c < self.channels; c++) {
    ChannelRuntime& cr = self.rt[c];
    for (size_t i = 0; i < frames; i++) {
      if (cr.rings[last].count() == 0)
        self.produce (c, cr, last);
      output[c][i] = cr.rings[last].pop();
    }
  }
}

void
PeriodicPlan::seek (uint64_t frame)
{
  Impl& self = *impl_;
  const uint64_t r = frame % self.period_out;   // integer modulus, never via double
  for (uint c = 0; c < self.channels; c++) {
    self.reset_runtime (self.rt[c]);
    self.advance (c, self.rt[c], r);
  }
}

void
PeriodicPlan::reset()
{
  Impl& self = *impl_;
  for (uint c = 0; c < self.channels; c++)
    self.reset_runtime (self.rt[c]);
}

} // namespace PandaResampler
