/*******************************************************************************

History matched modulation: parameter automation (sweep) example.

Shows how to run a time-varying cutoff with history matched modulation so the
automation has the same meaning regardless of the selected state realization.
Two filters are driven with identical parameter events but different block
sizes; because the parameter trajectory is placed from an absolute ramp
fraction and the per-section history is preserved, their outputs are identical.

Build (the checks script does this for you):
  c++ -std=c++11 -O2 -Ishared/DSPFilters/include
      checks/history_modulation_sweep_example.cpp
      shared/DSPFilters/source/ (all .cpp translation units)

*******************************************************************************/

#include "DspFilters/Dsp.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace Dsp;

static const int kChannels = 2;
static const int kTransitionSamples = 128;
static const double kSampleRate = 48000.0;

static Dsp::Params lowPassParams (double cutoffHz, double Q)
{
  Dsp::Params p;
  p.clear ();
  p[0] = kSampleRate;   // sample rate
  p[1] = cutoffHz;      // cutoff frequency
  p[2] = Q;             // Q
  return p;
}

// Drive one filter over a fixed schedule. Parameter changes are issued at
// absolute sample positions, independent of how process() blocks are split.
static std::vector<std::vector<float> >
runSweep (const std::vector<float>& inputL,
          const std::vector<float>& inputR,
          const std::vector<int>& blockPattern)
{
  typedef Dsp::SmoothedFilterDesign<
      Dsp::RBJ::Design::LowPass, kChannels, Dsp::DirectFormII> Filter;

  Filter filter (kTransitionSamples);

  // The mode must be selected before any non-empty input is processed.
  filter.enableHistoryMatchedModulation (true);

  // The first parameter set takes effect immediately (no ramp).
  filter.setParams (lowPassParams (800.0, 0.8));

  const int numSamples = static_cast<int> (inputL.size ());
  std::vector<std::vector<float> > buf (kChannels);
  buf[0] = inputL;
  buf[1] = inputR;

  // Schedule (in samples): warm up, sweep up, retarget halfway, hold, sweep.
  struct Event { int at; double cutoff; double Q; };
  const Event events[] = {
    { 128, 6000.0, 1.7 },  // begin a 128-sample upward sweep
    { 188, 1500.0, 1.1 },  // retarget from the actual mid-ramp position
    { 600, 4000.0, 1.9 },  // after a constant hold, sweep again
  };
  const int numEvents = int (sizeof (events) / sizeof (events[0]));
  int nextEvent = 0;

  int pos = 0, blockIndex = 0;
  while (pos < numSamples)
  {
    while (nextEvent < numEvents && events[nextEvent].at <= pos)
    {
      filter.setParams (lowPassParams (events[nextEvent].cutoff,
                                       events[nextEvent].Q));
      ++nextEvent;
    }

    // Never stride past a pending parameter event: it must land on a boundary.
    int limit = numSamples;
    if (nextEvent < numEvents)
      limit = events[nextEvent].at;

    int block = blockPattern[blockIndex++ % int (blockPattern.size ())];
    block = std::min (block, limit - pos);
    block = std::min (block, numSamples - pos);

    float* channels[kChannels] =
      { buf[0].data () + pos, buf[1].data () + pos };
    filter.process (block, channels);
    pos += block;
  }

  return buf;
}

int main ()
{
  const int numSamples = 900;
  std::vector<float> inL (numSamples), inR (numSamples);
  for (int n = 0; n < numSamples; ++n)
  {
    inL[n] = 0.4f * std::sin (0.11f * n) + 0.2f * std::sin (0.29f * n);
    inR[n] = 0.35f * std::sin (0.07f * n + 1.7f)
           + 0.15f * std::sin (0.43f * n + 0.5f);
  }

  // Same schedule, two very different blockings.
  std::vector<std::vector<float> > whole =
    runSweep (inL, inR, std::vector<int> (1, numSamples));
  std::vector<std::vector<float> > split =
    runSweep (inL, inR, std::vector<int> { 1, 7, 113 });

  float largestDifference = 0.0f;
  for (int c = 0; c < kChannels; ++c)
    for (int n = 0; n < numSamples; ++n)
      largestDifference =
        std::max (largestDifference, std::fabs (whole[c][n] - split[c][n]));

  std::printf ("sweep example: largest whole-vs-split output difference = %.3g\n",
               largestDifference);
  assert (largestDifference == 0.0f);
  std::printf ("SWEEP_EXAMPLE_OK (block-independent parameter trajectory)\n");
  return 0;
}
