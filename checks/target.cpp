/*******************************************************************************

checks/target.cpp -- independent numeric regression checks for the
Butterworth filters of DSPFilters.

Two groups are provided:

  mapping : an independently derived analog-prototype / complex frequency
            reference is compared, point by point, against the production
            Cascade::response() path, together with analytic DC/Nyquist
            limits, band-region shape checks, digital pole/zero placement,
            stability, single-pole sections and transformed filter order.

  pcm     : real two-channel PCM is pushed through the public
            SimpleFilter::process() entry point using DirectFormI,
            DirectFormII and TransposedDirectFormII state, in one-shot,
            irregular-chunk and mid-stream reset runs.  Steady-state
            complex amplitudes recovered by synchronous sine/cosine
            projection are compared against the same independent complex
            response (never against another production instance).

The reference in namespace ref is self contained: it does not read any
production coefficient, does not call any production transform and never
uses a production response to manufacture an expected value.

*******************************************************************************/

#include "DspFilters/Dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <complex>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>

namespace {

typedef Dsp::complex_t cplx;

const double PI = Dsp::doublePi;

static int gcdInt (int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
static int lcmInt (int a, int b) { return a / gcdInt (a, b) * b; }

// ---------------------------------------------------------------------------
// Independent reference
// ---------------------------------------------------------------------------

namespace ref {

enum Kind { kLowPass, kHighPass, kBandPass, kBandStop };

const char* kindName (Kind k)
{
  static const char* n[] = { "lowpass", "highpass", "bandpass", "bandstop" };
  return n[k];
}

struct Spec
{
  Kind kind;
  int order;             // analog prototype order N
  double sampleRate;
  double fc;             // normalized cutoff (LP/HP) or center (BP/BS)
  double fw;             // normalized width (BP/BS), 0 for LP/HP
};

// Butterworth analog low-pass prototype poles, placed independently on the
// left-half unit circle at angles pi/2 + (2k+1) pi/(2N).
std::vector<cplx> prototypePoles (int N)
{
  std::vector<cplx> poles;
  for (int k = 0; k < N; ++k)
  {
    double theta = PI / 2.0 + (2 * k + 1) * PI / (2.0 * N);
    cplx p (std::cos (theta), std::sin (theta));
    if (p.real() < 0.0)
      poles.push_back (p);
  }
  return poles;
}

// H_a(s) of the unity-cutoff prototype, normalized so H_a(0) = 1.
cplx analogResponse (int N, cplx s)
{
  std::vector<cplx> poles = prototypePoles (N);
  cplx h (1, 0);
  for (size_t i = 0; i < poles.size(); ++i)
    h /= (s - poles[i]);
  return h;
}

// Analog (bilinear-prewarped) edge geometry for band transforms.
struct BandGeometry
{
  double omega1;         // tan(pi * lower digital edge)
  double omega2;         // tan(pi * upper digital edge)
  double omega0;         // analog geometric center sqrt(omega1*omega2)
  double bandwidth;      // omega2 - omega1
};

BandGeometry bandGeometry (const Spec& s)
{
  BandGeometry g;
  // The two digital edges are prewarped separately; the analog center is
  // their geometric mean, not the prewarp of the digital center.
  double f1 = s.fc - s.fw / 2.0;
  double f2 = s.fc + s.fw / 2.0;
  g.omega1 = std::tan (PI * f1);
  g.omega2 = std::tan (PI * f2);
  g.omega0 = std::sqrt (g.omega1 * g.omega2);
  g.bandwidth = g.omega2 - g.omega1;
  return g;
}

// Analytic endpoint limits: returns false when the limit is a hard zero.
bool analyticLimit (const Spec& s, bool nyquist, cplx& value)
{
  switch (s.kind)
  {
  case kLowPass:
    if (!nyquist) { value = cplx (1, 0); return true; }   // H(0) = 1
    value = 0; return false;                              // H(pi) = 0
  case kHighPass:
    if (nyquist)
    {
      // Fixed convention of the library: the high-pass mapping contributes a
      // global factor (-1)^N on top of the H_a(0)=1 prototype.
      value = cplx ((s.order & 1) ? -1.0 : 1.0, 0);
      return true;
    }
    value = 0; return false;
  case kBandPass:
    value = 0; return false;                              // zeros at both ends
  case kBandStop:
    value = cplx (1, 0); return true;                     // unity at both ends
  }
  value = 0;
  return false;
}

// Independent complex digital response at normalized frequency f in (0, 0.5).
cplx response (const Spec& s, double f)
{
  const int N = s.order;
  const double omega = std::tan (PI * f);      // s = j*omega on the unit circle

  cplx mapped;
  switch (s.kind)
  {
  case kLowPass:
  {
    double f0 = std::tan (PI * s.fc);
    mapped = cplx (0, omega / f0);             // p = s / f0
    return analogResponse (N, mapped);
  }
  case kHighPass:
  {
    double f0 = std::tan (PI * s.fc);
    mapped = cplx (0, -f0 / omega);            // p = f0 / s
    cplx h = analogResponse (N, mapped);
    if (N & 1)
      h = -h;                                  // fixed (-1)^N convention
    return h;
  }
  case kBandPass:
  case kBandStop:
  {
    BandGeometry g = bandGeometry (s);
    cplx h;
    if (s.kind == kBandPass)
    {
      // p = (s^2 + omega0^2) / (B s), purely imaginary on s = j*omega
      double pv = (omega * omega - g.omega0 * g.omega0)
                  / (g.bandwidth * omega);
      mapped = cplx (0, pv);
      h = analogResponse (N, mapped);
    }
    else
    {
      // p = B s / (s^2 + omega0^2)
      double pv = g.bandwidth * omega
                  / (g.omega0 * g.omega0 - omega * omega);
      mapped = cplx (0, pv);
      h = analogResponse (N, mapped);
    }
    return h;
  }
  }
  return cplx (0, 0);
}

// Independently transformed digital poles (and, where meaningful, zeros),
// obtained by mapping the prototype poles through the same s-plane algebra
// expressed directly in z.  Used to check the production pole/zero layout
// without trusting its transform code.
std::vector<cplx> digitalPoles (const Spec& s)
{
  const int N = s.order;
  std::vector<cplx> pa = prototypePoles (N);
  std::vector<cplx> out;

  if (s.kind == kLowPass || s.kind == kHighPass)
  {
    double f0 = std::tan (PI * s.fc);
    for (size_t i = 0; i < pa.size(); ++i)
    {
      cplx p = pa[i];
      cplx z;
      if (s.kind == kLowPass)
        z = (1.0 + f0 * p) / (1.0 - f0 * p);
      else
        z = (p + f0) / (p - f0);
      out.push_back (z);
    }
    return out;
  }

  BandGeometry g = bandGeometry (s);
  const double w02 = g.omega0 * g.omega0;
  const double B = g.bandwidth;
  for (size_t i = 0; i < pa.size(); ++i)
  {
    cplx p = pa[i];
    // s = (z-1)/(z+1); each analog pole yields a quadratic in z.
    cplx A, Cc, D;
    if (s.kind == kBandPass)
    {
      // p B s = s^2 + omega0^2  ->  s^2 - pB s + omega0^2 = 0
      A = 1.0 - p * B + w02;
      Cc = 2.0 * (w02 - 1.0);
      D = 1.0 + p * B + w02;
    }
    else
    {
      // p (s^2 + omega0^2) = B s  ->  p s^2 - B s + p omega0^2 = 0
      A = p * (1.0 + w02) - B;
      Cc = 2.0 * p * (w02 - 1.0);
      D = p * (1.0 + w02) + B;
    }
    cplx disc = std::sqrt (Cc * Cc - 4.0 * A * D);
    out.push_back ((-Cc + disc) / (2.0 * A));
    out.push_back ((-Cc - disc) / (2.0 * A));
  }
  return out;
}

int digitalOrder (const Spec& s)
{
  return (s.kind == kBandPass || s.kind == kBandStop) ? 2 * s.order : s.order;
}

} // namespace ref

// ---------------------------------------------------------------------------
// Runtime polymorphic wrapper around the production SimpleFilter instances
// ---------------------------------------------------------------------------

struct AnyFilter
{
  virtual ~AnyFilter () {}
  virtual cplx response (double f) const = 0;
  virtual std::vector<Dsp::PoleZeroPair> poleZeros () const = 0;
  virtual int numStages () const = 0;
  virtual void reset () = 0;
  virtual void process (int n, double* const* ch) = 0;
};

template <class FilterType>
struct HeldFilter : AnyFilter
{
  FilterType f;
  cplx response (double freq) const { return f.response (freq); }
  std::vector<Dsp::PoleZeroPair> poleZeros () const { return f.getPoleZeros (); }
  int numStages () const { return f.getNumStages (); }
  void reset () { f.reset (); }
  void process (int n, double* const* ch) { f.process (n, ch); }
};

enum StateForm { formDF1, formDF2, formTDF2 };

std::unique_ptr<AnyFilter>
makeFilter (const ref::Spec& s, StateForm form)
{
  // MaxOrder 9 reserves enough storage for every case, including the 2N=18
  // digital poles of an order-9 band transform.
  switch (form)
  {
  case formDF1:
    switch (s.kind)
    {
    case ref::kLowPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::LowPass<9>, 2, Dsp::DirectFormI> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kHighPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::HighPass<9>, 2, Dsp::DirectFormI> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kBandPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::BandPass<9>, 2, Dsp::DirectFormI> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc, s.sampleRate * s.fw);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kBandStop: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::BandStop<9>, 2, Dsp::DirectFormI> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc, s.sampleRate * s.fw);
      return std::unique_ptr<AnyFilter> (p);
    }
    }
    break;
  case formDF2:
    switch (s.kind)
    {
    case ref::kLowPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::LowPass<9>, 2, Dsp::DirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kHighPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::HighPass<9>, 2, Dsp::DirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kBandPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::BandPass<9>, 2, Dsp::DirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc, s.sampleRate * s.fw);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kBandStop: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::BandStop<9>, 2, Dsp::DirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc, s.sampleRate * s.fw);
      return std::unique_ptr<AnyFilter> (p);
    }
    }
    break;
  case formTDF2:
    switch (s.kind)
    {
    case ref::kLowPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::LowPass<9>, 2, Dsp::TransposedDirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kHighPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::HighPass<9>, 2, Dsp::TransposedDirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kBandPass: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::BandPass<9>, 2, Dsp::TransposedDirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc, s.sampleRate * s.fw);
      return std::unique_ptr<AnyFilter> (p);
    }
    case ref::kBandStop: {
      auto p = new HeldFilter<Dsp::SimpleFilter<Dsp::Butterworth::BandStop<9>, 2, Dsp::TransposedDirectFormII> > ();
      p->f.setup (s.order, s.sampleRate, s.sampleRate * s.fc, s.sampleRate * s.fw);
      return std::unique_ptr<AnyFilter> (p);
    }
    }
    break;
  }
  return std::unique_ptr<AnyFilter> ();
}

// ---------------------------------------------------------------------------
// Small assertion bookkeeping
// ---------------------------------------------------------------------------

struct Checker
{
  long checks;
  long failures;
  Checker () : checks (0), failures (0) {}
  void fail (const std::string& msg)
  {
    ++failures;
    std::printf ("  FAIL %s\n", msg.c_str ());
  }
  void check (bool ok, const std::string& msg)
  {
    ++checks;
    if (!ok)
      fail (msg);
  }
};

// Compare a production complex value against the independent reference.
// Near-zero responses use an absolute tolerance; everywhere else both the
// magnitude difference and the full complex difference are asserted.
void compareComplex (Checker& chk, const char* tag,
                     cplx got, cplx expected,
                     double absTol = 2e-8, double relTol = 1e-9)
{
  double magRef = std::abs (expected);
  double cErr = std::abs (got - expected);
  double mErr = std::fabs (std::abs (got) - magRef);
  char buf[256];
  if (magRef < 1e-7)
  {
    std::snprintf (buf, sizeof (buf), "%s complex abs=%.3e tol=%.0e got=(%.6e,%.6e)",
                   tag, cErr, absTol, got.real(), got.imag());
    chk.check (cErr <= absTol, buf);
  }
  else
  {
    double cTol = std::max (absTol, relTol * magRef);
    std::snprintf (buf, sizeof (buf),
                   "%s complex err=%.3e tol=%.3e got=(%.9e,%.9e) exp=(%.9e,%.9e)",
                   tag, cErr, cTol, got.real(), got.imag(),
                   expected.real(), expected.imag());
    chk.check (cErr <= cTol, buf);
    double mTol = std::max (absTol, relTol * magRef);
    std::snprintf (buf, sizeof (buf), "%s magnitude err=%.3e tol=%.3e",
                   tag, mErr, mTol);
    chk.check (mErr <= mTol, buf);
  }
}

double nearestDistance (cplx v, const std::vector<cplx>& set)
{
  double best = 1e300;
  for (size_t i = 0; i < set.size(); ++i)
    best = std::min (best, std::abs (v - set[i]));
  return best;
}

// ---------------------------------------------------------------------------
// mapping group
// ---------------------------------------------------------------------------

// Frequencies chosen so both narrow low-frequency bands and the strongly
// non-linear mapping close to Nyquist are exercised.  Normalized-only
// grids would miss the tan warping, so a log grid is merged with a dense
// linear grid plus band-specific feature points.
std::vector<double> sweepFrequencies (const ref::Spec& s)
{
  std::vector<double> f;
  for (int i = 0; i <= 28; ++i)
  {
    double v = std::pow (10.0, -6.0 + 6.0 * i / 28.0); // 1e-6 .. 1
    if (v < 0.499)
      f.push_back (v);
  }
  for (int i = 1; i <= 34; ++i)
    f.push_back (0.001 + 0.497 * i / 34.0);

  if (s.kind == ref::kLowPass || s.kind == ref::kHighPass)
  {
    f.push_back (s.fc * 0.25);
    f.push_back (s.fc * 0.5);
    f.push_back (s.fc * 0.9);
    f.push_back (s.fc);
    f.push_back (s.fc * 1.1);
    f.push_back (s.fc * 2.0);
  }
  else
  {
    double f1 = s.fc - s.fw / 2.0;
    double f2 = s.fc + s.fw / 2.0;
    f.push_back (f1);
    f.push_back (f1 * 0.5);
    f.push_back (s.fc);
    f.push_back (f2);
    f.push_back (f2 * 1.25);
    // digital frequency corresponding to the analog geometric center
    ref::BandGeometry g = ref::bandGeometry (s);
    f.push_back (std::atan (g.omega0) / PI);
  }

  std::sort (f.begin(), f.end());
  f.erase (std::unique (f.begin(), f.end(),
                        [](double a, double b) { return std::fabs (a - b) < 1e-12; }),
           f.end());
  return f;
}

void checkLayout (Checker& chk, const ref::Spec& s, AnyFilter& filt)
{
  std::vector<Dsp::PoleZeroPair> vpz = filt.poleZeros ();
  std::vector<cplx> poles, zeros;
  int singlePoleSections = 0;
  for (size_t i = 0; i < vpz.size(); ++i)
  {
    poles.push_back (vpz[i].poles.first);
    zeros.push_back (vpz[i].zeros.first);
    if (!vpz[i].isSinglePole ())
    {
      poles.push_back (vpz[i].poles.second);
      zeros.push_back (vpz[i].zeros.second);
    }
    else
    {
      ++singlePoleSections;
    }
  }

  const int expectedOrder = ref::digitalOrder (s);
  char tag[160];

  std::snprintf (tag, sizeof (tag), "[%s N=%d sr=%.0f] digital order",
                 ref::kindName (s.kind), s.order, s.sampleRate);
  chk.check ((int) poles.size() == expectedOrder,
             std::string (tag) + " (got " + std::to_string (poles.size()) +
             " expected " + std::to_string (expectedOrder) + ")");

  std::snprintf (tag, sizeof (tag), "[%s N=%d] stage count",
                 ref::kindName (s.kind), s.order);
  chk.check (filt.numStages () == (expectedOrder + 1) / 2,
             std::string (tag) + " got " + std::to_string (filt.numStages ()));

  // Odd-order LP/HP keep one real single-pole section; band transforms
  // always produce an even digital order with biquad sections only.
  std::snprintf (tag, sizeof (tag), "[%s N=%d] single pole section",
                 ref::kindName (s.kind), s.order);
  int expectedSingles =
    ((s.kind == ref::kLowPass || s.kind == ref::kHighPass) && (s.order & 1)) ? 1 : 0;
  chk.check (singlePoleSections == expectedSingles,
             std::string (tag) + " got " + std::to_string (singlePoleSections));

  // Every digital pole must stay strictly inside the unit circle.
  double worstRadius = 0.0;
  for (size_t i = 0; i < poles.size(); ++i)
    worstRadius = std::max (worstRadius, std::abs (poles[i]));
  std::snprintf (tag, sizeof (tag), "[%s N=%d] stability rmax=%.9e",
                 ref::kindName (s.kind), s.order, worstRadius);
  chk.check (worstRadius < 1.0, tag);

  // Independent pole placement must match the production layout.
  std::vector<cplx> refPoles = ref::digitalPoles (s);
  double poleErr = 0.0;
  for (size_t i = 0; i < refPoles.size(); ++i)
    poleErr = std::max (poleErr, nearestDistance (refPoles[i], poles));
  std::snprintf (tag, sizeof (tag), "[%s N=%d] independent pole match err=%.3e",
                 ref::kindName (s.kind), s.order, poleErr);
  chk.check (poleErr <= 1e-9, tag);
}

void checkBandShape (Checker& chk, const ref::Spec& s)
{
  char tag[160];
  std::snprintf (tag, sizeof (tag), "[%s N=%d fc=%.2f fw=%.2f] shape",
                 ref::kindName (s.kind), s.order, s.fc, s.fw);

  if (s.kind == ref::kLowPass || s.kind == ref::kHighPass)
  {
    // The prewarped cutoff maps exactly onto the prototype -3 dB point.
    cplx hc = ref::response (s, s.fc);
    chk.check (std::fabs (std::abs (hc) - 1.0 / std::sqrt (2.0)) <= 1e-12,
               std::string (tag) + " cutoff -3dB");

    // Passband, two transition-band points and stopband, evaluated in order
    // so the full magnitude chain (not just isolated regions) is checked.
    double fp = s.fc / 8.0;                    // well inside the passband
    double ft1 = s.fc / 2.0;                   // lower transition band
    double ft2 = std::min (s.fc * 2.0, 0.48);  // upper transition band
    double fs = std::min (s.fc * 4.0, 0.49);   // well into the stopband
    double mags[5] = {
      std::abs (ref::response (s, fp)),
      std::abs (ref::response (s, ft1)),
      std::abs (hc),
      std::abs (ref::response (s, ft2)),
      std::abs (ref::response (s, fs))
    };

    if (s.kind == ref::kLowPass)
    {
      chk.check (mags[0] > 0.95, std::string (tag) + " LP passband");
      chk.check (mags[4] < 0.30, std::string (tag) + " LP stopband");
      bool ordered = true;
      for (int i = 1; i < 5; ++i)
        ordered = ordered && (mags[i - 1] > mags[i]);
      chk.check (ordered, std::string (tag) +
                 " LP pass/transition/stop monotone chain");
    }
    else
    {
      chk.check (mags[0] < 0.30, std::string (tag) + " HP stopband");
      chk.check (mags[4] > 0.95, std::string (tag) + " HP passband");
      bool ordered = true;
      for (int i = 1; i < 5; ++i)
        ordered = ordered && (mags[i - 1] < mags[i]);
      chk.check (ordered, std::string (tag) +
                 " HP stop/transition/pass monotone chain");
    }
  }
  else
  {
    double f1 = s.fc - s.fw / 2.0;
    double f2 = s.fc + s.fw / 2.0;
    cplx he1 = ref::response (s, f1);
    cplx he2 = ref::response (s, f2);
    chk.check (std::fabs (std::abs (he1) - 1.0 / std::sqrt (2.0)) <= 1e-12,
               std::string (tag) + " lower edge -3dB");
    chk.check (std::fabs (std::abs (he2) - 1.0 / std::sqrt (2.0)) <= 1e-12,
               std::string (tag) + " upper edge -3dB");

    cplx hc = ref::response (s, s.fc);
    // Stopband probes are pushed a full octave beyond the edges, since a
    // first-order band transform only gives shallow attenuation one edge
    // width away.
    double flow = std::max (1e-5, f1 / 8.0);
    double fhigh = std::min (0.499, f2 * 2.0);
    // Transition-band points sit halfway between each -3dB edge and the
    // corresponding far stop/outer-pass probe.
    double ftl = 0.5 * (flow + f1);
    double fth = 0.5 * (f2 + fhigh);
    cplx hl = ref::response (s, flow);
    cplx hh = ref::response (s, fhigh);
    cplx htl = ref::response (s, ftl);
    cplx hth = ref::response (s, fth);
    if (s.kind == ref::kBandPass)
    {
      chk.check (std::abs (hc) > 0.99, std::string (tag) + " BP passband");
      chk.check (std::abs (hl) < 0.10 && std::abs (hh) < 0.10,
                 std::string (tag) + " BP stopbands");
      // Full chains through both transition bands: stop < transition < edge
      // < passband on the low side and mirrored on the high side.
      bool lowChain = std::abs (hl) < std::abs (htl) &&
                      std::abs (htl) < std::abs (he1) &&
                      std::abs (he1) < std::abs (hc);
      bool highChain = std::abs (hh) < std::abs (hth) &&
                       std::abs (hth) < std::abs (he2) &&
                       std::abs (he2) < std::abs (hc);
      chk.check (lowChain, std::string (tag) + " BP low transition chain");
      chk.check (highChain, std::string (tag) + " BP high transition chain");
    }
    else
    {
      // A first-order notch is shallow (about 0.09 at the geometric center),
      // so the notch threshold is order-tolerant; higher orders go to ~0.
      chk.check (std::abs (hc) < 0.15, std::string (tag) + " BS notch");
      chk.check (std::abs (hl) > 0.90 && std::abs (hh) > 0.90,
                 std::string (tag) + " BS outer passbands");
      // Outer pass > transition > -3dB edge > notch on each side.
      bool lowChain = std::abs (hl) > std::abs (htl) &&
                      std::abs (htl) > std::abs (he1) &&
                      std::abs (he1) > std::abs (hc);
      bool highChain = std::abs (hh) > std::abs (hth) &&
                       std::abs (hth) > std::abs (he2) &&
                       std::abs (he2) > std::abs (hc);
      chk.check (lowChain, std::string (tag) + " BS low transition chain");
      chk.check (highChain, std::string (tag) + " BS high transition chain");
    }
  }
}

int runMapping ()
{
  Checker chk;
  const int orders[] = { 1, 3, 6, 9 };
  const double rates[] = { 8000.0, 48000.0, 96000.0 };
  const double cutoffs[] = { 0.07, 0.31 };
  const double bands[][2] = { { 0.18, 0.12 }, { 0.34, 0.08 } };

  long configs = 0;
  long sweepPoints = 0;
  double globalErr = 0.0;

  for (size_t oi = 0; oi < 4; ++oi)
  {
    for (size_t ri = 0; ri < 3; ++ri)
    {
      for (size_t ci = 0; ci < 2; ++ci)
      {
        ref::Spec specs[4];
        specs[0] = (ref::Spec) { ref::kLowPass,  orders[oi], rates[ri], cutoffs[ci], 0 };
        specs[1] = (ref::Spec) { ref::kHighPass, orders[oi], rates[ri], cutoffs[ci], 0 };
        specs[2] = (ref::Spec) { ref::kBandPass, orders[oi], rates[ri], bands[ci][0], bands[ci][1] };
        specs[3] = (ref::Spec) { ref::kBandStop, orders[oi], rates[ri], bands[ci][0], bands[ci][1] };
        const int nspec = 4;

        for (int si = 0; si < nspec; ++si)
        {
          ref::Spec s = specs[si];
          std::unique_ptr<AnyFilter> filt = makeFilter (s, formDF2);
          ++configs;

          // Analytic DC and Nyquist limits, handled separately because the
          // tan mapping is singular there.
          cplx lim;
          cplx gotDc = filt->response (0.0);
          if (ref::analyticLimit (s, false, lim))
            compareComplex (chk, "DC unity limit", gotDc, lim, 1e-12);
          else
          {
            ++chk.checks;
            if (std::abs (gotDc) > 1e-8)
              chk.fail ("DC zero limit residual too large");
          }
          cplx gotNy = filt->response (0.5);
          if (ref::analyticLimit (s, true, lim))
            compareComplex (chk, "Nyquist unity/sign limit", gotNy, lim, 1e-12);
          else
          {
            ++chk.checks;
            if (std::abs (gotNy) > 1e-8)
              chk.fail ("Nyquist zero limit residual too large");
          }

          // Interior sweep: independent complex reference vs production
          // response path.
          std::vector<double> freqs = sweepFrequencies (s);
          sweepPoints += static_cast<long> (freqs.size());
          double cfgErr = 0.0;
          for (size_t fi = 0; fi < freqs.size(); ++fi)
          {
            double f = freqs[fi];
            cplx expected = ref::response (s, f);
            cplx got = filt->response (f);
            cfgErr = std::max (cfgErr, std::abs (got - expected));
            char tag[128];
            std::snprintf (tag, sizeof (tag),
                           "%s N=%d sr=%.0f f=%.6g",
                           ref::kindName (s.kind), s.order, s.sampleRate, f);
            compareComplex (chk, tag, got, expected);
          }
          globalErr = std::max (globalErr, cfgErr);

          checkLayout (chk, s, *filt);
          checkBandShape (chk, s);
        }
      }
    }
  }

  std::printf ("mapping configs=%ld sweep_points=%ld max_complex_error=%.3e\n",
               configs, sweepPoints, globalErr);
  std::printf ("mapping stability/poles/order/shape/limits covered independently\n");
  if (chk.failures != 0)
  {
    std::printf ("mapping FAILED checks=%ld failures=%ld\n", chk.checks, chk.failures);
    return 1;
  }
  std::printf ("PASS mapping checks=%ld\n", chk.checks);
  return 0;
}

// ---------------------------------------------------------------------------
// pcm group
// ---------------------------------------------------------------------------

enum RunMode { modeWhole, modeChunk1, modeChunk7, modeChunk31, modeReset };

const char* modeName (RunMode m)
{
  static const char* n[] = { "whole", "chunk1", "chunk7", "chunk31", "reset" };
  return n[m];
}

const char* formName (StateForm f)
{
  static const char* n[] = { "DirectFormI", "DirectFormII", "TransposedDirectFormII" };
  return n[f];
}

// Slowest pole radius from the production layout; used only to size the
// settling time.  Expected amplitudes still come from the independent
// complex response.
double slowestRadius (AnyFilter& filt)
{
  std::vector<Dsp::PoleZeroPair> vpz = filt.poleZeros ();
  double r = 0.0;
  for (size_t i = 0; i < vpz.size(); ++i)
  {
    r = std::max (r, std::abs (vpz[i].poles.first));
    if (!vpz[i].isSinglePole ())
      r = std::max (r, std::abs (vpz[i].poles.second));
  }
  return r;
}

struct Probe
{
  double targetF;       // requested normalized frequency
  double actualF;       // frequency actually used (integer period)
  int period;           // samples per period
  double amplitude;
  double phase;
};

// Synchronous cosine/sine projection over an integer number of periods.
// For y[n] = Re(K e^{jwn}), the recovered coefficient is K = Ccos - j Csin.
cplx project (const std::vector<double>& y, int start, int length, double w)
{
  double cc = 0, cs = 0;
  for (int n = 0; n < length; ++n)
  {
    double c = std::cos (w * (start + n));
    double si = std::sin (w * (start + n));
    cc += y[start + n] * c;
    cs += y[start + n] * si;
  }
  cc *= 2.0 / length;
  cs *= 2.0 / length;
  return cplx (cc, -cs);
}

struct ChannelRecord
{
  double freq;          // observed (integer-period) normalized frequency
  cplx expected;        // independent analytic steady-state complex value
  cplx observed;        // value recovered by the synchronous projection
  double maxError;      // largest error over every run mode
  double tol;
};

struct PcmRecord
{
  int order;
  ref::Kind kind;
  double sampleRate;
  StateForm form;
  ChannelRecord ch[2];
};

PcmRecord runPcmCase (Checker& chk, const ref::Spec& s, StateForm form,
                      const Probe& pr0, const Probe& pr1)
{
  PcmRecord rec;
  rec.order = s.order;
  rec.kind = s.kind;
  rec.sampleRate = s.sampleRate;
  rec.form = form;
  for (int c = 0; c < 2; ++c)
  {
    rec.ch[c].maxError = 0.0;
    rec.ch[c].observed = cplx (0, 0);
    rec.ch[c].expected = cplx (0, 0);
    rec.ch[c].tol = 0.0;
    rec.ch[c].freq = 0.0;
  }

  const int periods = 8;
  std::unique_ptr<AnyFilter> filt = makeFilter (s, form);

  double r = slowestRadius (*filt);
  int settle;
  if (r >= 0.999999)
    settle = static_cast<int> (std::ceil (std::log (1e-3) / std::log (r)));
  else
    settle = static_cast<int> (std::ceil (std::log (1e-4) / std::log (r)));

  // Window covers an integer number of periods for both channels at once,
  // so each synchronous projection is exactly whole-period.
  int jointPeriod = lcmInt (pr0.period, pr1.period);
  int win = periods * jointPeriod;
  settle = std::max (settle, 4 * jointPeriod);

  int total = settle + win;

  std::vector<double> in0 (total), in1 (total);
  std::vector<double> out0 (total), out1 (total);
  double w0 = 2 * PI * pr0.actualF;
  double w1 = 2 * PI * pr1.actualF;
  for (int n = 0; n < total; ++n)
  {
    in0[n] = pr0.amplitude * std::cos (w0 * n + pr0.phase);
    in1[n] = pr1.amplitude * std::cos (w1 * n + pr1.phase);
  }

  RunMode modes[] = { modeWhole, modeChunk1, modeChunk7, modeChunk31, modeReset };
  for (size_t mi = 0; mi < 5; ++mi)
  {
    RunMode mode = modes[mi];
    out0 = in0;   // fresh, pristine playback buffers for every run
    out1 = in1;
    filt->reset ();

    if (mode == modeReset)
    {
      // Play half the stream, reset the state, then replay the full stream
      // from pristine input; the post-settle window must again match the
      // analytic steady state, proving reset() clears every section.
      int half = total / 2;
      double* ch[2] = { out0.data(), out1.data() };
      filt->process (half, ch);
      filt->reset ();
      out0 = in0;
      out1 = in1;
      double* ch2[2] = { out0.data(), out1.data() };
      filt->process (total, ch2);
    }
    else if (mode == modeWhole)
    {
      double* ch[2] = { out0.data(), out1.data() };
      filt->process (total, ch);
    }
    else
    {
      int chunk = (mode == modeChunk1) ? 1 : (mode == modeChunk7) ? 7 : 31;
      int pos = 0;
      while (pos < total)
      {
        int n = std::min (chunk, total - pos);
        double* ch[2] = { out0.data() + pos, out1.data() + pos };
        filt->process (n, ch);
        pos += n;
      }
    }

    for (int c = 0; c < 2; ++c)
    {
      const Probe& pr = (c == 0) ? pr0 : pr1;
      double w = (c == 0) ? w0 : w1;
      const std::vector<double>& out = (c == 0) ? out0 : out1;

      for (int n = 0; n < total; ++n)
      {
        ++chk.checks;
        if (!std::isfinite (out[n]))
        {
          chk.fail ("non-finite PCM output");
          break;
        }
      }

      cplx observed = project (out, settle, win, w);
      cplx expected = pr.amplitude * std::polar (1.0, pr.phase)
                      * ref::response (s, pr.actualF);
      double err = std::abs (observed - expected);
      double tol;
      // Stopband bins with negligible expected amplitude use an absolute
      // threshold that covers floating point noise and the library's
      // 1e-8 per-section anti-denormal injection accumulated through the
      // cascade and the projection window.
      if (std::abs (expected) < 1e-5)
        tol = 5e-6;
      else
        tol = std::max (2e-6, 5e-4 * std::abs (expected));

      rec.ch[c].freq = pr.actualF;
      rec.ch[c].expected = expected;
      rec.ch[c].tol = tol;
      if (err > rec.ch[c].maxError)
      {
        // Keep the observed value from the run that produced the max error.
        rec.ch[c].maxError = err;
        rec.ch[c].observed = observed;
      }

      char tag[256];
      std::snprintf (tag, sizeof (tag),
                     "pcm %s %s %s ch%d f=%.6g exp=(%.6e,%.6e) got=(%.6e,%.6e) err=%.3e tol=%.3e",
                     ref::kindName (s.kind), formName (form), modeName (mode), c,
                     pr.actualF, expected.real(), expected.imag(),
                     observed.real(), observed.imag(), err, tol);
      chk.check (err <= tol, tag);
    }
  }
  return rec;
}

Probe makeProbe (double targetF, double amplitude, double phase)
{
  Probe p;
  p.targetF = targetF;
  // Snap to a frequency whose period is an integer number of samples, so a
  // whole-period projection window exists.  Period 2 would land exactly on
  // Nyquist (f = 0.5), where the tan mapping is singular, so it is excluded.
  p.period = std::max (3, static_cast<int> (std::lround (1.0 / targetF)));
  p.actualF = 1.0 / p.period;
  p.amplitude = amplitude;
  p.phase = phase;
  return p;
}

int runPcm ()
{
  Checker chk;

  // Representative configurations spanning all four responses, prototype
  // orders 1/3/6/9 and all three sample rates somewhere in the set.
  ref::Spec specs[] = {
    { ref::kLowPass,  3, 48000.0, 0.07, 0 },
    { ref::kLowPass,  9, 96000.0, 0.31, 0 },
    { ref::kHighPass, 1, 8000.0,  0.07, 0 },
    { ref::kHighPass, 3, 48000.0, 0.31, 0 },
    { ref::kBandPass, 3, 48000.0, 0.18, 0.12 },
    { ref::kBandPass, 6, 96000.0, 0.34, 0.08 },
    { ref::kBandStop, 3, 48000.0, 0.34, 0.08 },
    { ref::kBandStop, 9, 8000.0,  0.18, 0.12 },
  };

  StateForm forms[] = { formDF1, formDF2, formTDF2 };
  long cases = 0;
  std::vector<PcmRecord> records;

  for (size_t si = 0; si < sizeof (specs) / sizeof (specs[0]); ++si)
  {
    ref::Spec s = specs[si];

    // One passband probe and one stopband probe per configuration; each
    // channel gets its own frequency, amplitude and phase.
    double pf0, pf1;
    if (s.kind == ref::kLowPass)
    { pf0 = s.fc * 0.4; pf1 = std::min (0.45, s.fc * 4.0); }
    else if (s.kind == ref::kHighPass)
    { pf0 = std::min (0.45, s.fc * 3.0); pf1 = s.fc * 0.25; }
    else if (s.kind == ref::kBandPass)
    { pf0 = s.fc; pf1 = std::max (1e-3, (s.fc - s.fw / 2.0) * 0.25); }
    else
    {
      ref::BandGeometry g = ref::bandGeometry (s);
      pf0 = std::atan (g.omega0) / PI;                 // notch frequency
      pf1 = std::min (0.45, (s.fc + s.fw / 2.0) * 1.6);
    }
    Probe p0 = makeProbe (pf0, 0.7, 0.3);
    Probe p1 = makeProbe (pf1, 0.4, 1.1);

    for (size_t fi = 0; fi < 3; ++fi)
    {
      records.push_back (runPcmCase (chk, s, forms[fi], p0, p1));
      ++cases;
    }
  }

  // Per-case record: observed frequency, expected/actual complex steady
  // state and the largest projection error across all five run modes.
  for (size_t i = 0; i < records.size(); ++i)
  {
    const PcmRecord& r = records[i];
    for (int c = 0; c < 2; ++c)
    {
      std::printf ("record %s N=%d sr=%.0f %s ch%d f=%.6g "
                   "expected=(%.6e,%.6e) observed=(%.6e,%.6e) maxerr=%.3e\n",
                   ref::kindName (r.kind), r.order, r.sampleRate,
                   formName (r.form), c, r.ch[c].freq,
                   r.ch[c].expected.real(), r.ch[c].expected.imag(),
                   r.ch[c].observed.real(), r.ch[c].observed.imag(),
                   r.ch[c].maxError);
    }
  }

  std::printf ("pcm cases=%ld forms=3 modes=5 channels=2\n", cases);
  if (chk.failures != 0)
  {
    std::printf ("pcm FAILED checks=%ld failures=%ld\n", chk.checks, chk.failures);
    return 1;
  }
  std::printf ("PASS pcm checks=%ld\n", chk.checks);
  return 0;
}

} // namespace

int main (int argc, char** argv)
{
  if (argc != 2)
  {
    std::fprintf (stderr, "usage: %s mapping|pcm\n", argv[0]);
    return 2;
  }
  std::string group = argv[1];
  if (group == "mapping")
    return runMapping ();
  if (group == "pcm")
    return runPcm ();
  std::fprintf (stderr, "unknown group '%s'\n", argv[1]);
  return 2;
}
