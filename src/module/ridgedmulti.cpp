// ridgedmulti.cpp
//
// Copyright (C) 2003, 2004 Jason Bevins
//
// This library is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation; either version 2.1 of the License, or (at
// your option) any later version.
//
// This library is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
// License (COPYING.txt) for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this library; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// The developer's email is jlbezigvins@gmzigail.com (for great email, take
// off every 'zig'.)
//

#include "module/ridgedmulti.h"

using namespace noise::module;

RidgedMulti::RidgedMulti ():
  Module (GetSourceModuleCount ()),
  m_frequency    (DEFAULT_RIDGED_FREQUENCY   ),
  m_lacunarity   (DEFAULT_RIDGED_LACUNARITY  ),
  m_noiseQuality (DEFAULT_RIDGED_QUALITY     ),
  m_octaveCount  (DEFAULT_RIDGED_OCTAVE_COUNT),
  m_seed         (DEFAULT_RIDGED_SEED)
{
  CalcSpectralWeights ();
}

// Calculates the spectral weights for each octave.
void RidgedMulti::CalcSpectralWeights ()
{
  // This exponent parameter should be user-defined; it may be exposed in a
  // future version of libnoise.
  double h = 1.0;

  double frequency = 1.0;
  for (int i = 0; i < RIDGED_MAX_OCTAVE; i++) {
    // Compute weight for each frequency.
    m_pSpectralWeights[i] = pow (frequency, -h);
    frequency *= m_lacunarity;
  }
}

// Multifractal code originally written by F. Kenton "Doc Mojo" Musgrave,
// 1998.  Modified by jas for use with libnoise.
double RidgedMulti::GetValue (double x, double y, double z) const
{
  x *= m_frequency;
  y *= m_frequency;
  z *= m_frequency;

  double signal = 0.0;
  double value  = 0.0;
  double weight = 1.0;

  // These parameters should be user-defined; they may be exposed in a
  // future version of libnoise.
  double offset = 1.0;
  double gain = 2.0;

  for (int curOctave = 0; curOctave < m_octaveCount; curOctave++) {

    // Make sure that these floating-point values have the same range as a 32-
    // bit integer so that we can pass them to the coherent-noise functions.
    double nx, ny, nz;
    nx = MakeInt32Range (x);
    ny = MakeInt32Range (y);
    nz = MakeInt32Range (z);

    // Get the coherent-noise value.
    int seed = (m_seed + curOctave) & 0x7fffffff;
    signal = GradientCoherentNoise3D (nx, ny, nz, seed, m_noiseQuality);

    // Make the ridges.
    signal = fabs (signal);
    signal = offset - signal;

    // Square the signal to increase the sharpness of the ridges.
    signal *= signal;

    // The weighting from the previous octave is applied to the signal.
    // Larger values have higher weights, producing sharp points along the
    // ridges.
    signal *= weight;

    // Weight successive contributions by the previous signal.
    weight = signal * gain;
    if (weight > 1.0) {
      weight = 1.0;
    }
    if (weight < 0.0) {
      weight = 0.0;
    }

    // Add the signal to the output value.
    value += (signal * m_pSpectralWeights[curOctave]);

    // Go to the next octave.
    x *= m_lacunarity;
    y *= m_lacunarity;
    z *= m_lacunarity;
  }

  return (value * 1.25) - 1.0;
}

noise::Sample3 RidgedMulti::GetSample (double x, double y, double z) const
{
  x *= m_frequency;
  y *= m_frequency;
  z *= m_frequency;

  double signal = 0.0;
  double value  = 0.0;
  double weight = 1.0;

  // The partial derivatives of the weight with respect to the input
  // coordinates.  The weight starts as the constant 1.0, so its derivative
  // starts at zero.
  double weightDx = 0.0;
  double weightDy = 0.0;
  double weightDz = 0.0;

  double dx = 0.0;
  double dy = 0.0;
  double dz = 0.0;

  double offset = 1.0;
  double gain = 2.0;

  // The frequency scaling applied to the input coordinates for the current
  // octave.  The coordinates stay well inside the range where MakeInt32Range
  // leaves them unchanged, so this is also the derivative of the noise
  // argument with respect to the input coordinate.
  double frequency = m_frequency;
  bool differentiable = true;

  for (int curOctave = 0; curOctave < m_octaveCount; curOctave++) {

    double nx, ny, nz;
    nx = MakeInt32Range (x);
    ny = MakeInt32Range (y);
    nz = MakeInt32Range (z);

    int seed = (m_seed + curOctave) & 0x7fffffff;
    double gx, gy, gz;
    bool octaveDifferentiable;

    // Get the coherent-noise value and its gradient with respect to the
    // current octave coordinates.
    signal = GradientCoherentNoise3D (nx, ny, nz, seed, m_noiseQuality,
      gx, gy, gz, octaveDifferentiable);
    if (!octaveDifferentiable) {
      differentiable = false;
    }

    // The derivative of the coherent-noise value with respect to the input
    // coordinates includes the frequency scaling of this octave.
    double signalDx = gx * frequency;
    double signalDy = gy * frequency;
    double signalDz = gz * frequency;

    // Make the ridges.  The absolute-value function is not differentiable
    // when the noise signal is exactly zero.
    if (signal < 0.0) {
      signal = -signal;
      signalDx = -signalDx;
      signalDy = -signalDy;
      signalDz = -signalDz;
    } else if (signal == 0.0) {
      differentiable = false;
      signalDx = 0.0;
      signalDy = 0.0;
      signalDz = 0.0;
    }

    signal = offset - signal;

    // The derivative of (offset - |n|) is -d|n|.
    signalDx = -signalDx;
    signalDy = -signalDy;
    signalDz = -signalDz;

    // Square the signal to increase the sharpness of the ridges.  The
    // derivative of s^2 is 2 * s * ds.
    signalDx *= 2.0 * signal;
    signalDy *= 2.0 * signal;
    signalDz *= 2.0 * signal;
    signal *= signal;

    // The weighting from the previous octave is applied to the signal.  The
    // weight is itself a function of the input, so it contributes through
    // its own partial derivatives.
    double signalDWeightX = signalDx * weight + signal * weightDx;
    double signalDWeightY = signalDy * weight + signal * weightDy;
    double signalDWeightZ = signalDz * weight + signal * weightDz;
    signal *= weight;

    // Weight successive contributions by the previous signal.  Once the
    // raw weight is clamped to 0.0 or 1.0, its derivative is zero; when the
    // raw weight is exactly 0.0 or 1.0 the clamp boundary makes the
    // derivative ambiguous.
    weight = signal * gain;
    if (weight == 0.0 || weight == 1.0) {
      differentiable = false;
      weightDx = 0.0;
      weightDy = 0.0;
      weightDz = 0.0;
    } else if (weight > 1.0) {
      weight = 1.0;
      weightDx = 0.0;
      weightDy = 0.0;
      weightDz = 0.0;
    } else if (weight < 0.0) {
      weight = 0.0;
      weightDx = 0.0;
      weightDy = 0.0;
      weightDz = 0.0;
    } else {
      weightDx = signalDWeightX * gain;
      weightDy = signalDWeightY * gain;
      weightDz = signalDWeightZ * gain;
    }

    // Add the signal to the output value, and accumulate its derivative.
    value += (signal * m_pSpectralWeights[curOctave]);
    dx += signalDWeightX * m_pSpectralWeights[curOctave];
    dy += signalDWeightY * m_pSpectralWeights[curOctave];
    dz += signalDWeightZ * m_pSpectralWeights[curOctave];

    // Go to the next octave.
    x *= m_lacunarity;
    y *= m_lacunarity;
    z *= m_lacunarity;
    frequency *= m_lacunarity;
  }

  double finalValue = (value * 1.25) - 1.0;
  if (!differentiable) {
    return Sample3::NotDifferentiable (finalValue);
  }
  return Sample3 (finalValue, dx * 1.25, dy * 1.25, dz * 1.25, true);
}
