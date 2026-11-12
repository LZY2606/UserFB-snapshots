/*******************************************************************************

"A Collection of Useful C++ Classes for Digital Signal Processing"
 By Vinnie Falco

Official project location:
https://github.com/vinniefalco/DSPFilters

See Documentation.cpp for contact information, notes, and bibliography.

--------------------------------------------------------------------------------

License: MIT License (http://www.opensource.org/licenses/mit-license.php)
Copyright (c) 2009 by Vinnie Falco

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*******************************************************************************/

#ifndef DSPFILTERS_SMOOTHEDFILTER_H
#define DSPFILTERS_SMOOTHEDFILTER_H

#include "DspFilters/Common.h"
#include "DspFilters/Filter.h"

#include <stdexcept>

namespace Dsp {

/*
 * Implements smooth modulation of time-varying filter parameters
 *
 */
template <class DesignClass,
          int Channels,
          class StateType = DirectFormII>
class SmoothedFilterDesign
  : public FilterDesign <DesignClass,
                       Channels,
                       HistoryMatchedState <StateType> >
{
public:
  // The selected realization is wrapped so its section delays can be re-seeded
  // from the canonical section history during history matched modulation.
  // With the mode disabled the wrapper is a pass-through, so the wrapped
  // StateType still produces every sample and legacy output is unchanged.
  typedef HistoryMatchedState <StateType> MatchedStateType;
  typedef FilterDesign <DesignClass, Channels, MatchedStateType> filter_type_t;

  SmoothedFilterDesign (int transitionSamples)
    : m_transitionSamples (transitionSamples)
    , m_remainingSamples (-1) // first time flag
    , m_transitionIndex (0)
    , m_historyMatched (false)
    , m_hasProcessed (false)
  {
  }

  /*
   * Enable or disable history matched modulation.
   *
   * When enabled, every second order section applies the coefficients in
   * force at each sample to that section's real last two inputs and last two
   * outputs (its direct difference equation). The internal delays of the
   * previous coefficient system are never reused as the new system's state.
   *
   * The mode may only be changed before any non-empty input has been
   * processed, or immediately after reset(); otherwise std::logic_error is
   * thrown. The default is off and the legacy modulation path is untouched.
   */
  void enableHistoryMatchedModulation (bool enabled)
  {
    if (m_hasProcessed)
      throw std::logic_error (
        "enableHistoryMatchedModulation may only be changed before processing "
        "non-empty input or after reset()");

    // Propagate first so an unsupported state type throws before the filter's
    // own mode flag changes.
    this->m_state.setHistoryMatched (enabled);
    m_historyMatched = enabled;
  }

  bool isHistoryMatchedModulationEnabled () const
  {
    return m_historyMatched;
  }

  /*
   * Clears the audio history. The modulation mode is retained. With history
   * matched modulation enabled, any unfinished transition is cancelled and the
   * filter runs stably from the parameters actually in effect at the reset
   * (the last coefficients that were applied), rather than the un-reached
   * target. With the mode disabled the legacy reset behavior is preserved.
   */
  void reset ()
  {
    // Zeroes the wrapped realization and the canonical section history while
    // preserving each section's history-matched enable flag.
    filter_type_t::reset ();
    m_hasProcessed = false;

    if (m_historyMatched && m_remainingSamples >= 0)
    {
      // Cancel the unfinished ramp and hold the last applied parameters.
      Params held = m_transitionParams;
      m_remainingSamples = 0;
      m_transitionIndex = 0;
      m_transitionStart = held;
      m_transitionParams = held;
      m_transitionFilter.setParams (held);
      // Steady block processing must use the same held coefficients.
      this->m_design.setParams (held);
    }
  }

  // Process a block of samples.
  template <typename Sample>
  void processBlock (int numSamples,
                     Sample* const* destChannelArray)
  {
    // If this goes off it means setup() was never called
    assert (m_remainingSamples >= 0);

    if (m_historyMatched)
      processBlockHistoryMatched (numSamples, destChannelArray);
    else
      processBlockLegacy (numSamples, destChannelArray);

    if (numSamples > 0)
      m_hasProcessed = true;
  }

  void process (int numSamples, float* const* arrayOfChannels)
  {
    processBlock (numSamples, arrayOfChannels);
  }

  void process (int numSamples, double* const* arrayOfChannels)
  {
    processBlock (numSamples, arrayOfChannels);
  }

protected:
  void doSetParams (const Params& parameters)
  {
    if (m_remainingSamples >= 0)
    {
      if (m_historyMatched)
      {
        // Begin a new ramp from the parameter position actually reached.
        m_transitionStart = m_transitionParams;
        m_transitionIndex = 0;
      }
      m_remainingSamples = m_transitionSamples;
    }
    else
    {
      // first time: the first parameter set takes effect immediately
      m_remainingSamples = 0;
      m_transitionParams = parameters;
      m_transitionStart = parameters;
      m_transitionIndex = 0;
    }

    filter_type_t::doSetParams (parameters);
  }

private:
  // Legacy per-block interpolation. Unchanged from the original implementation
  // so existing users keep identical behavior and output.
  template <typename Sample>
  void processBlockLegacy (int numSamples,
                           Sample* const* destChannelArray)
  {
    const int numChannels = this->getNumChannels();

    // first handle any transition samples
    int remainingSamples = std::min (m_remainingSamples, numSamples);

    if (remainingSamples > 0)
    {
      // interpolate parameters for each sample
      const double t = 1. / m_remainingSamples;
      double dp[maxParameters];
      for (int i = 0; i < DesignClass::NumParams; ++i)
        dp[i] = (this->getParams()[i] - m_transitionParams[i]) * t;

      for (int n = 0; n < remainingSamples; ++n)
      {
        for (int i = DesignClass::NumParams; --i >=0;)
          m_transitionParams[i] += dp[i];

        m_transitionFilter.setParams (m_transitionParams);

        for (int i = numChannels; --i >= 0;)
        {
          Sample* dest = destChannelArray[i]+n;
          *dest = this->m_state[i].process (*dest, m_transitionFilter);
        }
      }

      m_remainingSamples -= remainingSamples;

      if (m_remainingSamples == 0)
        m_transitionParams = this->getParams();
    }

    // do what's left
    if (numSamples - remainingSamples > 0)
    {
      // no transition
      for (int i = 0; i < numChannels; ++i)
        this->m_design.process (numSamples - remainingSamples,
                          destChannelArray[i] + remainingSamples,
                          this->m_state[i]);
    }
  }

  // History matched modulation. Coefficients are still advanced one sample at a
  // time through m_transitionParams/m_transitionFilter, but each parameter is
  // placed from an absolute ramp fraction so the trajectory is identical no
  // matter how the host splits its process blocks. Zero-length calls advance
  // nothing.
  template <typename Sample>
  void processBlockHistoryMatched (int numSamples,
                                   Sample* const* destChannelArray)
  {
    const int numChannels = this->getNumChannels();
    const int total = m_transitionSamples;

    int remainingSamples = std::min (m_remainingSamples, numSamples);

    if (remainingSamples > 0)
    {
      const Params& target = this->getParams ();

      for (int n = 0; n < remainingSamples; ++n)
      {
        ++m_transitionIndex;
        const double fraction =
          static_cast<double> (m_transitionIndex) / static_cast<double> (total);

        // First sample of a length-N ramp is at 1/N, the last is exactly 1.
        for (int i = 0; i < DesignClass::NumParams; ++i)
          m_transitionParams[i] =
            m_transitionStart[i] + (target[i] - m_transitionStart[i]) * fraction;

        m_transitionFilter.setParams (m_transitionParams);

        // Each channel/section state applies the current coefficients to its
        // own real section history; the cascade chains actual stage outputs.
        for (int i = numChannels; --i >= 0;)
        {
          Sample* dest = destChannelArray[i] + n;
          *dest = this->m_state[i].process (*dest, m_transitionFilter);
        }
      }

      m_remainingSamples -= remainingSamples;

      if (m_remainingSamples == 0)
      {
        m_transitionParams = target;
        m_transitionIndex = 0;
      }
    }

    // Steady portion: ordinary block processing. The section history keeps
    // accumulating, so a later transition starts from the real state rather
    // than a cleared one.
    if (numSamples - remainingSamples > 0)
    {
      for (int i = 0; i < numChannels; ++i)
        this->m_design.process (numSamples - remainingSamples,
                          destChannelArray[i] + remainingSamples,
                          this->m_state[i]);
    }
  }

protected:
  Params m_transitionParams;
  Params m_transitionStart;   // parameter position the current ramp began at
  DesignClass m_transitionFilter;
  int m_transitionSamples;

  int m_remainingSamples;        // remaining transition samples
  int m_transitionIndex;         // samples already advanced in this ramp

  bool m_historyMatched;
  bool m_hasProcessed;
};

}

#endif
