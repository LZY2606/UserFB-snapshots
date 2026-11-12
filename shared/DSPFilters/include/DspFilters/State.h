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

#ifndef DSPFILTERS_STATE_H
#define DSPFILTERS_STATE_H

#include "DspFilters/Common.h"
#include "DspFilters/Biquad.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Dsp {

/*
 * Various forms of state information required to
 * process channels of actual sample data.
 *
 */

//------------------------------------------------------------------------------

/*
 * State for applying a second order section to a sample using Direct Form I
 *
 * Difference equation:
 *
 *  y[n] = (b0/a0)*x[n] + (b1/a0)*x[n-1] + (b2/a0)*x[n-2]
 *                      - (a1/a0)*y[n-1] - (a2/a0)*y[n-2]  
 */
class DirectFormI
{
public:
  DirectFormI ()
  {
    reset();
  }

  void reset ()
  {
    m_x1 = 0;
    m_x2 = 0;
    m_y1 = 0;
    m_y2 = 0;
  }

  template <typename Sample>
  inline Sample process1 (const Sample in,
                          const BiquadBase& s,
                          const double vsa) // very small amount
  {
    double out = s.m_b0*in + s.m_b1*m_x1 + s.m_b2*m_x2
                           - s.m_a1*m_y1 - s.m_a2*m_y2
                           + vsa;
    m_x2 = m_x1;
    m_y2 = m_y1;
    m_x1 = in;
    m_y1 = out;

    return static_cast<Sample> (out);
  }

  // History matched modulation support.
  // Direct Form I keeps the canonical section input/output history directly,
  // so adopting the history is a plain assignment of the four delay values.
  void loadHistoryMatched (double x1, double x2,
                           double y1, double y2,
                           const BiquadBase& /*s*/)
  {
    m_x1 = x1;
    m_x2 = x2;
    m_y1 = y1;
    m_y2 = y2;
  }

protected:
  double m_x2; // x[n-2]
  double m_y2; // y[n-2]
  double m_x1; // x[n-1]
  double m_y1; // y[n-1]
};

//------------------------------------------------------------------------------

/*
 * State for applying a second order section to a sample using Direct Form II
 *
 * Difference equation:
 *
 *  v[n] =         x[n] - (a1/a0)*v[n-1] - (a2/a0)*v[n-2]
 *  y(n) = (b0/a0)*v[n] + (b1/a0)*v[n-1] + (b2/a0)*v[n-2]
 *
 */
class DirectFormII
{
public:
  DirectFormII ()
  {
    reset ();
  }

  void reset ()
  {
    m_v1 = 0;
    m_v2 = 0;
  }

  template <typename Sample>
  Sample process1 (const Sample in,
                   const BiquadBase& s,
                   const double vsa)
  {
    double w   = in - s.m_a1*m_v1 - s.m_a2*m_v2 + vsa;
    double out =      s.m_b0*w    + s.m_b1*m_v1 + s.m_b2*m_v2;

    m_v2 = m_v1;
    m_v1 = w;

    return static_cast<Sample> (out);
  }

  // History matched modulation support.
  // The canonical (input/output) history does not live in the Direct Form II
  // delays, so the internal v1/v2 are reconstructed by inverting the
  // observability transform that maps the delays onto the canonical history.
  // The transform is well conditioned for the supported filter designs.
  void loadHistoryMatched (double x1, double x2,
                           double y1, double y2,
                           const BiquadBase& s)
  {
    // Observable quantities derived from the canonical difference equation.
    const double o1 = s.m_b1*x1 - s.m_a1*y1
                    + s.m_b2*x2 - s.m_a2*y2;
    const double o2 = s.m_b2*x1 - s.m_a2*y1;

    const double c1 = s.m_b1 - s.m_b0*s.m_a1;
    const double c2 = s.m_b2 - s.m_b0*s.m_a2;
    const double d2 = s.m_b2*s.m_a1 - s.m_a2*s.m_b1;
    const double det = c1*d2 - c2*c2;

    m_v1 = (o1*d2 - c2*o2) / det;
    m_v2 = (c1*o2 - c2*o1) / det;
  }

private:
  double m_v1; // v[-1]
  double m_v2; // v[-2]
};

//------------------------------------------------------------------------------

/*
 * Transposed Direct Form I and II
 * by lubomir i. ivanov (neolit123 [at] gmail)
 *
 * Reference:
 * http://www.kvraudio.com/forum/viewtopic.php?p=4430351
 *
 */

// I think this one is broken
class TransposedDirectFormI
{
public:
  TransposedDirectFormI ()
  {
    reset ();
  }

  void reset ()
  {
    m_v = 0;
    m_s1 = 0;
    m_s1_1 = 0;
    m_s2 = 0;
    m_s2_1 = 0;
    m_s3 = 0;
    m_s3_1 = 0;
    m_s4 = 0;
    m_s4_1 = 0;
  }

  template <typename Sample>
  inline Sample process1 (const Sample in,
                          const BiquadBase& s,
                          const double vsa)
  {
    double out;

    // can be: in += m_s1_1;
    m_v = in + m_s1_1;
    out = s.m_b0*m_v + m_s3_1;
    m_s1 = m_s2_1 - s.m_a1*m_v;
    m_s2 = -s.m_a2*m_v;
    m_s3 = s.m_b1*m_v + m_s4_1;
    m_s4 = s.m_b2*m_v; 

    m_s4_1 = m_s4;
    m_s3_1 = m_s3;
    m_s2_1 = m_s2;
    m_s1_1 = m_s1;

    return static_cast<Sample> (out);
  }

private:
  double m_v;
  double m_s1;
  double m_s1_1;
  double m_s2;
  double m_s2_1;
  double m_s3;
  double m_s3_1;
  double m_s4;
  double m_s4_1;
};

//------------------------------------------------------------------------------

class TransposedDirectFormII
{
public:
  TransposedDirectFormII ()
  {
    reset ();
  }

  void reset ()
  {
    m_s1 = 0;
    m_s1_1 = 0;
    m_s2 = 0;
    m_s2_1 = 0;
  }

  template <typename Sample>
  inline Sample process1 (const Sample in,
                          const BiquadBase& s,
                          const double vsa)
  {
    double out;

    out = m_s1_1 + s.m_b0*in + vsa;
    m_s1 = m_s2_1 + s.m_b1*in - s.m_a1*out;
    m_s2 = s.m_b2*in - s.m_a2*out;
    m_s1_1 = m_s1;
    m_s2_1 = m_s2;

    return static_cast<Sample> (out);
  }

  // History matched modulation support.
  // The two stored delays are directly observable from the canonical section
  // input/output history, so they can be seeded without solving a system.
  void loadHistoryMatched (double x1, double x2,
                           double y1, double y2,
                           const BiquadBase& s)
  {
    m_s1_1 = s.m_b1*x1 - s.m_a1*y1
           + s.m_b2*x2 - s.m_a2*y2;
    m_s2_1 = s.m_b2*x1 - s.m_a2*y1;
  }

private:
  double m_s1;
  double m_s1_1;
  double m_s2;
  double m_s2_1;
};

//------------------------------------------------------------------------------

/*
 * Compile-time marker for which realizations support history matched
 * modulation. A state is matchable when it provides loadHistoryMatched(), so
 * the three supported forms (and subclasses such as instrumented test states)
 * qualify. Any other state type still compiles and runs through the normal
 * (legacy) path, but enabling the mode for it is rejected.
 */
template <class StateType>
struct IsHistoryMatchable
{
  template <class U>
  static auto test (int)
    -> decltype (std::declval<U> ().loadHistoryMatched (
                   std::declval<double> (), std::declval<double> (),
                   std::declval<double> (), std::declval<double> (),
                   std::declval<const BiquadBase&> ()),
                 void (), std::true_type ());

  template <class>
  static std::false_type test (...);

  static const bool value = decltype (test<StateType> (0))::value;
};

template <bool Matched>
struct HistoryMatchedDispatchTag
{
};

/*
 * HistoryMatchedState
 *
 * Wraps any of the supported section state types (DirectFormI, DirectFormII,
 * TransposedDirectFormII) so that time-varying coefficients always act on the
 * true canonical history of the section: its last two inputs and last two
 * outputs, evaluated with the section's own direct difference equation.
 *
 * The wrapped StateType still computes every output through its own
 * process1(). Before each output its internal delays are re-seeded from the
 * shared canonical history for the coefficients currently in force, so simply
 * carrying the previous coefficient system's delay variables into the new
 * system never happens. When history matched modulation is disabled the
 * wrapper is a pass-through and behaves exactly like the wrapped StateType.
 *
 * Storage is fixed size; there are no heap allocations on the audio path.
 */
template <class StateType>
class HistoryMatchedState
{
public:
  HistoryMatchedState ()
    : m_x1 (0)
    , m_x2 (0)
    , m_y1 (0)
    , m_y2 (0)
    , m_enabled (false)
  {
    // m_state is default-constructed already reset; reset() again to keep the
    // same initialization contract as the wrapped state types.
    m_state.reset ();
  }

  void reset ()
  {
    m_state.reset ();
    m_x1 = 0;
    m_x2 = 0;
    m_y1 = 0;
    m_y2 = 0;
    // The enabled flag is deliberately preserved: reset() clears audio
    // history only, not the selected modulation mode.
  }

  // Turn history matched modulation on/off. This only changes how the wrapped
  // state's delays are adopted; the canonical history itself is preserved.
  void setHistoryMatched (bool enabled)
  {
    if (enabled && !IsHistoryMatchable<StateType>::value)
      throw std::logic_error (
        "history matched modulation is not supported for this state type");
    m_enabled = enabled;
  }

  bool isHistoryMatched () const
  {
    return m_enabled;
  }

  template <typename Sample>
  inline Sample process1 (const Sample in,
                          const BiquadBase& s,
                          const double vsa)
  {
    return process1Matched (in, s, vsa,
            HistoryMatchedDispatchTag<IsHistoryMatchable<StateType>::value> ());
  }

private:
  // Supported realization: re-seed internal delays from canonical history, then
  // let the selected state form produce the output itself.
  template <typename Sample>
  inline Sample process1Matched (const Sample in,
                                 const BiquadBase& s,
                                 const double vsa,
                                 HistoryMatchedDispatchTag<true>)
  {
    if (m_enabled)
    {
      // Adopt the canonical history into this realization using the
      // coefficients currently in force, then let the selected state form
      // produce the output through its own difference equation.
      //
      // The anti-denormal term is deliberately NOT handed to the realization:
      // the three forms inject it at different nodes (output for DFI/TDFII,
      // internal state for DFII), which would make the modulated output depend
      // on the chosen realization. Since the internal delays are re-seeded from
      // canonical history before every sample, the term is instead applied once
      // at the canonical output so every realization follows the same equation.
      m_state.loadHistoryMatched (m_x1, m_x2, m_y1, m_y2, s);
      Sample out = m_state.process1 (in, s, 0.0);
      out = static_cast<Sample> (out + static_cast<Sample> (vsa));

      // Accumulate the canonical section input/output history every sample,
      // including samples where the coefficients do not change.
      m_x2 = m_x1;
      m_x1 = in;
      m_y2 = m_y1;
      m_y1 = out;
      return out;
    }

    return m_state.process1 (in, s, vsa);
  }

  // Unsupported realization: the history matched body is never instantiated,
  // so a state without loadHistoryMatched() still compiles. It always runs the
  // normal pass-through; enabling the mode is rejected in setHistoryMatched().
  template <typename Sample>
  inline Sample process1Matched (const Sample in,
                                 const BiquadBase& s,
                                 const double vsa,
                                 HistoryMatchedDispatchTag<false>)
  {
    return m_state.process1 (in, s, vsa);
  }

private:
  StateType m_state;

  double m_x1; // section x[n-1]
  double m_x2; // section x[n-2]
  double m_y1; // section y[n-1]
  double m_y2; // section y[n-2]

  bool m_enabled;
};

//------------------------------------------------------------------------------

// Holds an array of states suitable for multi-channel processing
template <int Channels, class StateType>
class ChannelsState
{
public:
  ChannelsState ()
  {
  }

  const int getNumChannels() const
  {
    return Channels;
  }

  void reset ()
  {
    for (int i = 0; i < Channels; ++i)
      m_state[i].reset();
  }

  // Forward the history matched modulation flag to every channel. Only
  // instantiated for state types that provide setHistoryMatched().
  void setHistoryMatched (bool enabled)
  {
    for (int i = 0; i < Channels; ++i)
      m_state[i].setHistoryMatched (enabled);
  }

  StateType& operator[] (int index)
  {
    assert (index >= 0 && index < Channels);
    return m_state[index];
  }

  template <class Filter, typename Sample>
  void process (int numSamples,
                Sample* const* arrayOfChannels,
                Filter& filter)
  {
    for (int i = 0; i < Channels; ++i)
      filter.process (numSamples, arrayOfChannels[i], m_state[i]);
  }

private:
  StateType m_state[Channels];
};

// Empty state, can't process anything
template <class StateType>
class ChannelsState <0, StateType>
{
public:
  const int getNumChannels() const
  {
    return 0;
  }

  void reset ()
  {
    throw std::logic_error ("attempt to reset empty ChannelState");
  }

  template <class FilterDesign, typename Sample>
  void process (int numSamples,
                Sample* const* arrayOfChannels,
                FilterDesign& filter)
  {
    throw std::logic_error ("attempt to process empty ChannelState");
  }
};

//------------------------------------------------------------------------------

}

#endif
