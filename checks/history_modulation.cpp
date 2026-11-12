/*******************************************************************************

History matched modulation check.

The reference below is an independent, double precision implementation of the
same filter designs and the same canonical (Direct Form I) per-section
difference equations. It advances its own parameter trajectory and keeps its
own per-channel, per-section input/output history. The library filters under
test are only compared against this reference; matching three library
instances against each other is not sufficient.

Tolerances:
  double : max absolute error <= 1e-6
  float  : max absolute error <= 2e-5
The residual is explained by float quantization and the anti-denormal bias
(an alternating +/-1e-8 term applied once at the first section output).

*******************************************************************************/

#include "DspFilters/Dsp.h"

#include <cassert>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace Dsp;

static const double PI = 3.1415926535897932384626433832795;
static const double VSA = 1e-8;
static const int TRANSITION = 128;
static const double SAMPLE_RATE = 48000.0;

// Call counters proving the selected State::process1 really runs per section.
long long g_callsDFI = 0;
long long g_callsDFII = 0;
long long g_callsTDFII = 0;

struct CountingDirectFormI : DirectFormI
{
  template <typename Sample>
  inline Sample process1 (const Sample in, const BiquadBase& s, const double vsa)
  {
    ++g_callsDFI;
    return DirectFormI::process1 (in, s, vsa);
  }
};

struct CountingDirectFormII : DirectFormII
{
  template <typename Sample>
  inline Sample process1 (const Sample in, const BiquadBase& s, const double vsa)
  {
    ++g_callsDFII;
    return DirectFormII::process1 (in, s, vsa);
  }
};

struct CountingTransposedDirectFormII : TransposedDirectFormII
{
  template <typename Sample>
  inline Sample process1 (const Sample in, const BiquadBase& s, const double vsa)
  {
    ++g_callsTDFII;
    return TransposedDirectFormII::process1 (in, s, vsa);
  }
};

//------------------------------------------------------------------------------
// Independent coefficient designs (normalized so a0 == 1).
//------------------------------------------------------------------------------

struct Coeff
{
  double a1, a2, b0, b1, b2;
};

static Coeff refRbjLowPass (double sr, double fc, double Q)
{
  const double w0 = 2 * PI * fc / sr;
  const double cs = std::cos (w0);
  const double sn = std::sin (w0);
  const double AL = sn / (2 * Q);
  const double a0 = 1 + AL, a1 = -2 * cs, a2 = 1 - AL;
  const double b0 = (1 - cs) / 2, b1 = 1 - cs, b2 = (1 - cs) / 2;
  return Coeff { a1 / a0, a2 / a0, b0 / a0, b1 / a0, b2 / a0 };
}

static Coeff refRbjHighPass (double sr, double fc, double Q)
{
  const double w0 = 2 * PI * fc / sr;
  const double cs = std::cos (w0);
  const double sn = std::sin (w0);
  const double AL = sn / (2 * Q);
  const double a0 = 1 + AL, a1 = -2 * cs, a2 = 1 - AL;
  const double b0 = (1 + cs) / 2, b1 = -(1 + cs), b2 = (1 + cs) / 2;
  return Coeff { a1 / a0, a2 / a0, b0 / a0, b1 / a0, b2 / a0 };
}

// Fixed fourth order Butterworth low pass: two biquads, dc gain normalized on
// the first section only, stage order matching the library cascade.
static std::vector<Coeff> refButterworthLowPass4 (double sr, double fc)
{
  const int order = 4;
  const double f = std::tan (PI * fc / sr);
  std::vector<Coeff> stages;
  const int pairs = order / 2;
  for (int i = 0; i < pairs; ++i)
  {
    const double theta = PI / 2 + (2 * i + 1) * PI / (2 * order);
    const std::complex<double> sp = std::polar (1.0, theta);
    const std::complex<double> z = (1.0 + f * sp) / (1.0 - f * sp);
    stages.push_back (Coeff { -2 * z.real (), std::norm (z), 1.0, 2.0, 1.0 });
  }
  double dc = 1.0;
  for (std::size_t i = 0; i < stages.size (); ++i)
  {
    const Coeff& c = stages[i];
    dc *= (c.b0 + c.b1 + c.b2) / (1 + c.a1 + c.a2);
  }
  const double scale = 1.0 / dc;
  stages[0].b0 *= scale;
  stages[0].b1 *= scale;
  stages[0].b2 *= scale;
  return stages;
}

//------------------------------------------------------------------------------
// Independent canonical reference filter with its own ramp state machine.
//------------------------------------------------------------------------------

enum RefDesign
{
  refRbjLP = 0,
  refRbjHP = 1,
  refBw4LP = 2
};

struct RefSection
{
  double x1, x2, y1, y2;
  RefSection () : x1 (0), x2 (0), y1 (0), y2 (0) {}
  void reset () { x1 = x2 = y1 = y2 = 0; }

  double step (double x, const Coeff& c, double vsa)
  {
    const double y = c.b0 * x + c.b1 * x1 + c.b2 * x2
                   - c.a1 * y1 - c.a2 * y2 + vsa;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }
};

class RefModel
{
public:
  RefModel (RefDesign design, int transitionSamples)
    : m_design (design)
    , m_transition (transitionSamples)
    , m_remaining (-1)
    , m_index (0)
    , m_vsaSign (-1.0) // library's first ac() returns -anti_denormal_vsa
  {
    m_start.clear ();
    m_target.clear ();
    m_applied.clear ();
  }

  int numStages () const { return m_design == refBw4LP ? 2 : 1; }

  void setParams (const Params& p)
  {
    if (m_remaining < 0)
    {
      m_remaining = 0;
      m_start = p;
      m_target = p;
      m_applied = p;
    }
    else
    {
      if (m_remaining > 0)
      {
        const double frac = double (m_index) / double (m_transition);
        for (int i = 0; i < maxParameters; ++i)
          m_start[i] = m_start[i] + (m_target[i] - m_start[i]) * frac;
      }
      else
      {
        m_start = m_target;
      }
      m_target = p;
      m_remaining = m_transition;
      m_index = 0;
    }
  }

  void reset ()
  {
    for (std::size_t i = 0; i < m_sections.size (); ++i)
      m_sections[i].reset ();
    m_sections.clear ();
    // Unfinished transition is cancelled; hold the last parameters that were
    // actually applied (the filter's in-use position at the reset).
    m_start = m_applied;
    m_target = m_applied;
    m_remaining = 0;
    m_index = 0;
    // The anti-denormal alternation is intentionally not restarted.
  }

  double process (double in)
  {
    Params cur = m_target;
    if (m_remaining > 0)
    {
      ++m_index;
      const double frac = double (m_index) / double (m_transition);
      for (int i = 0; i < maxParameters; ++i)
        cur[i] = m_start[i] + (m_target[i] - m_start[i]) * frac;
      --m_remaining;
      if (m_remaining == 0)
        m_index = 0;
    }
    m_applied = cur;

    std::vector<Coeff> stages = designCoeffs (cur);
    if (int (m_sections.size ()) != int (stages.size ()))
      m_sections.resize (stages.size ());

    // The bias alternates every sample and is injected at the first section
    // output only, mirroring the library cascade/biquad denormal prevention.
    const double vsa = m_vsaSign * VSA;
    m_vsaSign = -m_vsaSign;

    double out = in;
    for (std::size_t k = 0; k < stages.size (); ++k)
      out = m_sections[k].step (out, stages[k], k == 0 ? vsa : 0.0);
    return out;
  }

private:
  std::vector<Coeff> designCoeffs (const Params& p) const
  {
    std::vector<Coeff> v;
    if (m_design == refRbjLP)
      v.push_back (refRbjLowPass (p[0], p[1], p[2]));
    else if (m_design == refRbjHP)
      v.push_back (refRbjHighPass (p[0], p[1], p[2]));
    else
      v = refButterworthLowPass4 (p[0], p[2]);
    return v;
  }

  RefDesign m_design;
  int m_transition;
  int m_remaining;
  int m_index;
  Params m_start;
  Params m_target;
  Params m_applied;   // last parameter set actually applied, used by reset()
  std::vector<RefSection> m_sections;
  double m_vsaSign;
};

//------------------------------------------------------------------------------
// Scenario: parameter events at absolute sample positions.
//------------------------------------------------------------------------------

struct Event
{
  int at;
  Params params;
};

static Params makeRbjParams (double fc, double Q)
{
  Params p;
  p.clear ();
  p[0] = SAMPLE_RATE;
  p[1] = fc;
  p[2] = Q;
  return p;
}

static Params makeBwParams (double fc)
{
  Params p;
  p.clear ();
  p[0] = SAMPLE_RATE;
  p[1] = 4;
  p[2] = fc;
  return p;
}

static std::vector<Event> makeEvents (bool butterworth)
{
  std::vector<Event> e;
  Event ev;

  // Static warm-up, then a sweep to the upper frequency (and, for RBJ, Q)
  // boundary.
  ev.at = 150;
  ev.params = butterworth ? makeBwParams (6000) : makeRbjParams (6000, 2.0);
  e.push_back (ev);

  // Reverse/redirect the target halfway through that sweep, down to the lower
  // frequency (and, for RBJ, Q) boundary.
  ev.at = 150 + 60;
  ev.params = butterworth ? makeBwParams (500) : makeRbjParams (500, 0.7);
  e.push_back (ev);

  // After a long constant hold, sweep again.
  ev.at = 700;
  ev.params = butterworth ? makeBwParams (4000) : makeRbjParams (4000, 1.9);
  e.push_back (ev);

  // One more sweep after the previous one completes.
  ev.at = 1000;
  ev.params = butterworth ? makeBwParams (900) : makeRbjParams (900, 0.75);
  e.push_back (ev);

  return e;
}

static double inputSample (int channel, int n)
{
  if (channel == 0)
    return 0.45 * std::sin (0.11 * n) + 0.25 * std::sin (0.29 * n)
         + (n % 173 == 0 ? 0.5 : 0.0);
  return 0.40 * std::sin (0.07 * n + 1.7)
       + 0.20 * std::sin (0.43 * n + 0.5)
       + (n % 211 == 0 ? -0.45 : 0.0);
}

//------------------------------------------------------------------------------
// Drive a library filter through a scenario with a given chunk pattern.
// Events always land on block boundaries; zero-length calls are sprinkled in.
//------------------------------------------------------------------------------

template <typename Sample>
static std::vector<std::vector<Sample> >
driveLibrary (Filter& filter,
              const Params& initial,
              const std::vector<Event>& events,
              const std::vector<Sample>& in0,
              const std::vector<Sample>& in1,
              int channels,
              const std::vector<int>& chunkPattern,
              bool sprinkleZeroLength)
{
  const int N = int (in0.size ());
  std::vector<std::vector<Sample> > buf (channels);
  for (int c = 0; c < channels; ++c)
    buf[c] = (c == 0 ? in0 : in1);

  filter.setParams (initial);
  int pos = 0, ci = 0, ei = 0;

  auto applyEvents = [&] ()
  {
    while (ei < int (events.size ()) && events[ei].at <= pos)
    {
      filter.setParams (events[ei].params);
      ++ei;
    }
  };

  while (pos < N)
  {
    applyEvents ();
    int nextEvent = N;
    if (ei < int (events.size ()))
      nextEvent = events[ei].at;

    int sz = chunkPattern[ci++ % int (chunkPattern.size ())];
    sz = std::min (sz, nextEvent - pos);
    sz = std::min (sz, N - pos);

    if (sprinkleZeroLength && (ci % 3 == 0))
    {
      // A zero-length call must not advance the parameter trajectory.
      Sample* z[2] = { buf[0].data () + pos, buf[channels - 1].data () + pos };
      filter.process (0, z);
    }

    Sample* ch[2] = { buf[0].data () + pos, buf[channels - 1].data () + pos };
    filter.process (sz, ch);
    pos += sz;
  }
  return buf;
}

// Drive the independent reference through the same scenario sample by sample.
// Each channel gets its own model so the per-channel anti-denormal alternator
// and section history stay independent, mirroring the library's per-channel
// cascade state.
static std::vector<std::vector<double> >
driveReference (RefDesign designKind,
                int transitionSamples,
                const Params& initial,
                const std::vector<Event>& events,
                const std::vector<double>& in0,
                const std::vector<double>& in1,
                int channels)
{
  const int N = int (in0.size ());
  std::vector<std::vector<double> > out (channels, std::vector<double> (N));
  std::vector<RefModel> refs;
  for (int c = 0; c < channels; ++c)
  {
    refs.emplace_back (designKind, transitionSamples);
    refs[c].setParams (initial);
  }
  int ei = 0;
  for (int n = 0; n < N; ++n)
  {
    while (ei < int (events.size ()) && events[ei].at == n)
    {
      for (int c = 0; c < channels; ++c)
        refs[c].setParams (events[ei].params);
      ++ei;
    }
    for (int c = 0; c < channels; ++c)
      out[c][n] = refs[c].process (c == 0 ? in0[n] : in1[n]);
  }
  return out;
}

//------------------------------------------------------------------------------
// Per-design/state matrix.
//------------------------------------------------------------------------------

struct ComboResult
{
  double worstError;
  double chunkDiff;
};

template <class FilterType, typename Sample>
static ComboResult
runOne (RefDesign designKind,
        const Params& initial,
        const std::vector<Event>& events,
        const std::vector<Sample>& in0,
        const std::vector<Sample>& in1,
        int channels,
        double tolerance)
{
  const int N = int (in0.size ());

  std::vector<double> din0 (N), din1 (N);
  for (int n = 0; n < N; ++n)
  {
    din0[n] = double (in0[n]);
    // For a mono filter the second input placeholder is never actually used.
    din1[n] = (channels > 1) ? double (in1[n]) : 0.0;
  }

  // Independent reference trajectory.
  std::vector<std::vector<double> > refOut =
    driveReference (designKind, TRANSITION, initial, events,
                    din0, din1, channels);

  // Library filter, whole block.
  FilterType whole (TRANSITION);
  whole.enableHistoryMatchedModulation (true);
  std::vector<std::vector<Sample> > outWhole =
    driveLibrary (whole, initial, events, in0, in1, channels,
                  std::vector<int> (1, N), false);

  // Library filter, irregular 1/7/113 chunking with zero-length calls.
  FilterType split (TRANSITION);
  split.enableHistoryMatchedModulation (true);
  std::vector<std::vector<Sample> > outSplit =
    driveLibrary (split, initial, events, in0, in1, channels,
                  std::vector<int> { 1, 7, 113 }, true);

  double worst = 0, chunk = 0;
  for (int c = 0; c < channels; ++c)
  {
    for (int n = 0; n < N; ++n)
    {
      assert (std::isfinite (double (outWhole[c][n])));
      assert (std::isfinite (double (outSplit[c][n])));
      worst = std::max (worst, std::fabs (double (outWhole[c][n]) - refOut[c][n]));
      worst = std::max (worst, std::fabs (double (outSplit[c][n]) - refOut[c][n]));
      chunk = std::max (chunk,
                        std::fabs (double (outWhole[c][n]) - double (outSplit[c][n])));
    }
  }

  // Every design/state combination gets a real numerical assertion.
  assert (worst <= tolerance);
  // The parameter trajectory must be identical regardless of block splitting.
  assert (chunk == 0.0);

  ComboResult r;
  r.worstError = worst;
  r.chunkDiff = chunk;
  return r;
}

//------------------------------------------------------------------------------
// Reset/reuse and interface contract checks.
//------------------------------------------------------------------------------

template <class FilterType, typename Sample>
static void testResetReuse (RefDesign designKind,
                            const Params& initial,
                            const Params& redirected,
                            const std::vector<Sample>& in0,
                            const std::vector<Sample>& in1,
                            int channels,
                            double tolerance)
{
  const int N = int (in0.size ());
  const int warm = 40;        // steady samples before the ramp
  const int partial = 37;     // advance only partway into the 128-sample ramp

  // Independent per-channel references mirror every call, including the
  // partial ramp that is later cancelled by reset().
  std::vector<RefModel> ref;
  for (int c = 0; c < channels; ++c)
    ref.emplace_back (designKind, TRANSITION);

  FilterType f (TRANSITION);
  f.enableHistoryMatchedModulation (true);
  f.setParams (initial);
  for (int c = 0; c < channels; ++c)
    ref[c].setParams (initial);

  std::vector<std::vector<Sample> > buf (channels);
  buf[0] = in0;
  if (channels > 1) buf[1] = in1;

  // Steady warm-up, then a ramp which is only partially advanced.
  auto step = [&] (int n)
  {
    Sample* ch[2] = { buf[0].data () + n, buf[channels - 1].data () + n };
    f.process (1, ch);
    ref[0].process (double (in0[n]));
    if (channels > 1)
      ref[1].process (double (in1[n]));
  };

  for (int n = 0; n < warm; ++n)
    step (n);
  f.setParams (redirected);
  for (int c = 0; c < channels; ++c)
    ref[c].setParams (redirected);
  for (int n = warm; n < warm + partial; ++n)
    step (n);

  // Reset mid-ramp: history is cleared and the unfinished ramp is cancelled,
  // leaving the last actually applied parameters held on both sides.
  f.reset ();
  for (int c = 0; c < channels; ++c)
    ref[c].reset ();

  // Mode survives reset.
  assert (f.isHistoryMatchedModulationEnabled ());
  // Toggling is permitted again after reset.
  f.enableHistoryMatchedModulation (false);
  f.enableHistoryMatchedModulation (true);

  double worst = 0;
  for (int n = warm + partial; n < N; ++n)
  {
    Sample* ch[2] = { buf[0].data () + n, buf[channels - 1].data () + n };
    f.process (1, ch);
    const double e0 = ref[0].process (double (in0[n]));
    worst = std::max (worst, std::fabs (double (buf[0][n]) - e0));
    if (channels > 1)
    {
      const double e1 = ref[1].process (double (in1[n]));
      worst = std::max (worst, std::fabs (double (buf[1][n]) - e1));
    }
  }
  assert (worst <= tolerance);
}

// Toggle guard, zero-length handling and the very first parameter set.
// Uses a mono filter so a single channel pointer is valid.
static void testInterfaceContract ()
{
  typedef SmoothedFilterDesign<RBJ::Design::LowPass, 1, DirectFormI> Filt;
  Filt f (TRANSITION);

  // Allowed before any non-empty input, including after only zero-length calls.
  f.enableHistoryMatchedModulation (true);
  f.setParams (makeRbjParams (800, 0.8));
  std::vector<float> dummy (8, 0.0f);
  float* p = dummy.data ();
  f.process (0, &p);
  f.enableHistoryMatchedModulation (false); // still allowed: nothing processed
  f.enableHistoryMatchedModulation (true);

  f.process (8, &p);
  bool threw = false;
  try
  {
    f.enableHistoryMatchedModulation (false);
  }
  catch (const std::logic_error&)
  {
    threw = true;
  }
  assert (threw);

  f.reset ();
  bool threwAfterReset = false;
  try
  {
    f.enableHistoryMatchedModulation (false);
  }
  catch (const std::logic_error&)
  {
    threwAfterReset = true;
  }
  assert (!threwAfterReset);

  // Default is off and legacy mode reports as such.
  Filt legacy (TRANSITION);
  assert (!legacy.isHistoryMatchedModulationEnabled ());

  // A state realization without a history-matched transform still compiles and
  // runs in legacy mode, but enabling the mode for it is rejected.
  SmoothedFilterDesign<RBJ::Design::LowPass, 1, TransposedDirectFormI>
      unsupported (TRANSITION);
  unsupported.setParams (makeRbjParams (800, 0.8));
  bool threwUnsupported = false;
  try
  {
    unsupported.enableHistoryMatchedModulation (true);
  }
  catch (const std::logic_error&)
  {
    threwUnsupported = true;
  }
  assert (threwUnsupported);
}

// Legacy behavior is unchanged and still differs across realizations.
static void testLegacyUnchanged ()
{
  typedef RBJ::Design::LowPass LP;
  SmoothedFilterDesign<LP, 1, DirectFormI> dfi (TRANSITION);
  SmoothedFilterDesign<LP, 1, DirectFormII> dfii (TRANSITION);
  Params p0 = makeRbjParams (800, 0.8);
  dfi.setParams (p0);
  dfii.setParams (p0);

  const int N = 512;
  std::vector<double> x (N), y (N);
  for (int n = 0; n < N; ++n)
    x[n] = y[n] = 0.3 * std::sin (0.07 * n);
  double* xp = x.data ();
  double* yp = y.data ();
  dfi.process (N, &xp);
  dfii.process (N, &yp);
  double fixedDiff = 0;
  for (int n = 0; n < N; ++n)
    fixedDiff = std::max (fixedDiff, std::fabs (x[n] - y[n]));
  assert (fixedDiff < 1e-6);

  Params p1 = makeRbjParams (6000, 1.7);
  dfi.setParams (p1);
  dfii.setParams (p1);
  for (int n = 0; n < N; ++n)
    x[n] = y[n] = 0.3 * std::sin (0.07 * (n + N));
  dfi.process (N, &xp);
  dfii.process (N, &yp);
  double movingDiff = 0;
  for (int n = 0; n < N; ++n)
    movingDiff = std::max (movingDiff, std::fabs (x[n] - y[n]));
  // The legacy modulation difference that history matched mode removes.
  assert (movingDiff > 1e-3);
}

//------------------------------------------------------------------------------
// Matrix driver over the three state forms for one design.
//------------------------------------------------------------------------------

template <class Design, int Channels>
static void runMatrix (const char* designName,
                       RefDesign designKind,
                       const Params& initial,
                       const std::vector<Event>& events,
                       double& globalWorstDouble,
                       double& globalWorstFloat)
{
  const int N = 1200;
  std::vector<float> f0 (N), f1 (N);
  std::vector<double> d0 (N), d1 (N);
  for (int n = 0; n < N; ++n)
  {
    f0[n] = float (inputSample (0, n));
    f1[n] = float (inputSample (1, n));
    d0[n] = inputSample (0, n);
    d1[n] = inputSample (1, n);
  }

  typedef SmoothedFilterDesign<Design, Channels, DirectFormI> TFI;
  typedef SmoothedFilterDesign<Design, Channels, DirectFormII> TFII;
  typedef SmoothedFilterDesign<Design, Channels,
                               TransposedDirectFormII> TTFII;
  const char* names[3] =
    { "DirectFormI", "DirectFormII", "TransposedDirectFormII" };

  ComboResult rD[3], rF[3];

  rD[0] = runOne<TFI, double> (designKind, initial, events, d0, d1, Channels, 1e-6);
  rD[1] = runOne<TFII, double> (designKind, initial, events, d0, d1, Channels, 1e-6);
  rD[2] = runOne<TTFII, double> (designKind, initial, events, d0, d1, Channels, 1e-6);

  rF[0] = runOne<TFI, float> (designKind, initial, events, f0, f1, Channels, 2e-5);
  rF[1] = runOne<TFII, float> (designKind, initial, events, f0, f1, Channels, 2e-5);
  rF[2] = runOne<TTFII, float> (designKind, initial, events, f0, f1, Channels, 2e-5);

  for (int i = 0; i < 3; ++i)
  {
    globalWorstDouble = std::max (globalWorstDouble, rD[i].worstError);
    globalWorstFloat = std::max (globalWorstFloat, rF[i].worstError);
    std::printf ("  %-22s %-24s dblErr=%.3e fltErr=%.3e chunkDiff=%.1e\n",
                 designName, names[i],
                 rD[i].worstError, rF[i].worstError,
                 std::max (rD[i].chunkDiff, rF[i].chunkDiff));
  }

  // Reset/reuse on the double DFI instantiation.
  testResetReuse<TFI, double> (
    designKind, initial,
    designKind == refBw4LP ? makeBwParams (5000) : makeRbjParams (5000, 1.4),
    d0, d1, Channels, 1e-6);
}

int main ()
{
  testLegacyUnchanged ();
  std::printf ("legacy modulation behavior unchanged\n");

  double worstDouble = 0, worstFloat = 0;

  // Stereo (2 channel) matrix over all three designs and three states.
  std::printf ("stereo (2 channels), float and double:\n");
  runMatrix<RBJ::Design::LowPass, 2> (
    "RBJ LowPass", refRbjLP, makeRbjParams (800, 0.8), makeEvents (false),
    worstDouble, worstFloat);
  runMatrix<RBJ::Design::HighPass, 2> (
    "RBJ HighPass", refRbjHP, makeRbjParams (900, 0.9), makeEvents (false),
    worstDouble, worstFloat);
  runMatrix<Butterworth::Design::LowPass<4>, 2> (
    "Butterworth LowPass4", refBw4LP, makeBwParams (1000), makeEvents (true),
    worstDouble, worstFloat);

  // Mono (1 channel): every design and every state form, double precision.
  std::printf ("mono (1 channel), double:\n");
  {
    const int N = 1200;
    std::vector<double> d0 (N), d1 (1, 0.0);
    for (int n = 0; n < N; ++n)
      d0[n] = inputSample (0, n);

    ComboResult m[9];
    Params initials[3] =
      { makeRbjParams (800, 0.8), makeRbjParams (900, 0.9), makeBwParams (1000) };
    int mi = 0;

    typedef SmoothedFilterDesign<RBJ::Design::LowPass, 1, DirectFormI> L1;
    typedef SmoothedFilterDesign<RBJ::Design::LowPass, 1, DirectFormII> L2;
    typedef SmoothedFilterDesign<RBJ::Design::LowPass, 1,
                                 TransposedDirectFormII> L3;
    typedef SmoothedFilterDesign<RBJ::Design::HighPass, 1, DirectFormI> H1;
    typedef SmoothedFilterDesign<RBJ::Design::HighPass, 1, DirectFormII> H2;
    typedef SmoothedFilterDesign<RBJ::Design::HighPass, 1,
                                 TransposedDirectFormII> H3;
    typedef SmoothedFilterDesign<Butterworth::Design::LowPass<4>, 1,
                                 DirectFormI> B1;
    typedef SmoothedFilterDesign<Butterworth::Design::LowPass<4>, 1,
                                 DirectFormII> B2;
    typedef SmoothedFilterDesign<Butterworth::Design::LowPass<4>, 1,
                                 TransposedDirectFormII> B3;

    m[mi++] = runOne<L1, double> (refRbjLP, initials[0], makeEvents (false),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<L2, double> (refRbjLP, initials[0], makeEvents (false),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<L3, double> (refRbjLP, initials[0], makeEvents (false),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<H1, double> (refRbjHP, initials[1], makeEvents (false),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<H2, double> (refRbjHP, initials[1], makeEvents (false),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<H3, double> (refRbjHP, initials[1], makeEvents (false),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<B1, double> (refBw4LP, initials[2], makeEvents (true),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<B2, double> (refBw4LP, initials[2], makeEvents (true),
                                  d0, d1, 1, 1e-6);
    m[mi++] = runOne<B3, double> (refBw4LP, initials[2], makeEvents (true),
                                  d0, d1, 1, 1e-6);
    for (int i = 0; i < mi; ++i)
    {
      std::printf ("  mono combo %d worstErr=%.3e chunkDiff=%.1e\n",
                   i, m[i].worstError, m[i].chunkDiff);
      worstDouble = std::max (worstDouble, m[i].worstError);
    }
  }

  // Mono float coverage for one design/state.
  {
    const int N = 1200;
    std::vector<float> f0 (N), f1 (1, 0.0f);
    for (int n = 0; n < N; ++n)
      f0[n] = float (inputSample (0, n));
    ComboResult mf =
      runOne<SmoothedFilterDesign<RBJ::Design::LowPass, 1, DirectFormI>, float> (
        refRbjLP, makeRbjParams (800, 0.8), makeEvents (false),
        f0, f1, 1, 2e-5);
    worstFloat = std::max (worstFloat, mf.worstError);
    std::printf ("  mono float RBJ LowPass DFI worstErr=%.3e\n", mf.worstError);
  }

  testInterfaceContract ();
  std::printf ("interface contract (toggle guard, zero-length, first set) ok\n");

  // Explicit per-section call counting with instrumented state types. This is
  // a runtime counter on State::process1, not a name or source-string check.
  std::printf ("state call accounting:\n");
  long long totalStateCalls = 0;
  {
    const int M = 257, channels = 2;
    std::vector<float> a (M), b (M);
    for (int n = 0; n < M; ++n)
    {
      a[n] = float (inputSample (0, n));
      b[n] = float (inputSample (1, n));
    }
    float* ch[2] = { a.data (), b.data () };

    g_callsDFI = g_callsDFII = g_callsTDFII = 0;
    SmoothedFilterDesign<RBJ::Design::LowPass, channels,
                         CountingDirectFormI> cf (TRANSITION);
    cf.setParams (makeRbjParams (800, 0.8));
    cf.enableHistoryMatchedModulation (true);
    cf.process (M, ch);
    std::printf ("  RBJ  DFI    process1 calls=%lld expected=%d\n",
                 g_callsDFI, M * channels * 1);
    assert (g_callsDFI == (long long)(M) * channels * 1);
    totalStateCalls += g_callsDFI;

    g_callsDFI = g_callsDFII = g_callsTDFII = 0;
    SmoothedFilterDesign<Butterworth::Design::LowPass<4>, channels,
                         CountingDirectFormII> cb (TRANSITION);
    cb.setParams (makeBwParams (1000));
    cb.enableHistoryMatchedModulation (true);
    cb.process (M, ch);
    std::printf ("  BW4  DFII   process1 calls=%lld expected=%d\n",
                 g_callsDFII, M * channels * 2);
    assert (g_callsDFII == (long long)(M) * channels * 2);
    totalStateCalls += g_callsDFII;

    g_callsDFI = g_callsDFII = g_callsTDFII = 0;
    SmoothedFilterDesign<RBJ::Design::HighPass, channels,
                         CountingTransposedDirectFormII> ct (TRANSITION);
    ct.setParams (makeRbjParams (900, 0.9));
    ct.enableHistoryMatchedModulation (true);
    ct.process (M, ch);
    std::printf ("  RBJ  TDFII  process1 calls=%lld expected=%d\n",
                 g_callsTDFII, M * channels * 1);
    assert (g_callsTDFII == (long long)(M) * channels * 1);
    totalStateCalls += g_callsTDFII;
  }

  std::printf ("\nSUMMARY worstDoubleErr=%.3e worstFloatErr=%.3e totalStateCalls=%lld\n",
               worstDouble, worstFloat, totalStateCalls);
  std::printf ("HISTORY_MATCHED_MODULATION_OK designs=3 states=3\n");
  return 0;
}
