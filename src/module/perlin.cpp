// perlin.cpp
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

#include "module/perlin.h"

using namespace noise::module;

Perlin::Perlin ():
  Module (GetSourceModuleCount ()),
  m_frequency    (DEFAULT_PERLIN_FREQUENCY   ),
  m_lacunarity   (DEFAULT_PERLIN_LACUNARITY  ),
  m_noiseQuality (DEFAULT_PERLIN_QUALITY     ),
  m_octaveCount  (DEFAULT_PERLIN_OCTAVE_COUNT),
  m_persistence  (DEFAULT_PERLIN_PERSISTENCE ),
  m_seed         (DEFAULT_PERLIN_SEED)
{
}

double Perlin::GetValue (double x, double y, double z) const
{
  double value = 0.0;
  double signal = 0.0;
  double curPersistence = 1.0;
  double nx, ny, nz;
  int seed;

  x *= m_frequency;
  y *= m_frequency;
  z *= m_frequency;

  for (int curOctave = 0; curOctave < m_octaveCount; curOctave++) {

    // Make sure that these floating-point values have the same range as a 32-
    // bit integer so that we can pass them to the coherent-noise functions.
    nx = MakeInt32Range (x);
    ny = MakeInt32Range (y);
    nz = MakeInt32Range (z);

    // Get the coherent-noise value from the input value and add it to the
    // final result.
    seed = (m_seed + curOctave) & 0xffffffff;
    signal = GradientCoherentNoise3D (nx, ny, nz, seed, m_noiseQuality);
    value += signal * curPersistence;

    // Prepare the next octave.
    x *= m_lacunarity;
    y *= m_lacunarity;
    z *= m_lacunarity;
    curPersistence *= m_persistence;
  }

  return value;
}

noise::Sample3 Perlin::GetSample (double x, double y, double z) const
{
  double value = 0.0;
  double dx = 0.0;
  double dy = 0.0;
  double dz = 0.0;
  double curPersistence = 1.0;
  double nX, nY, nZ;
  int seed;
  bool differentiable = true;

  // The frequency scaling applied to the input coordinates for the current
  // octave.  The coordinates stay well inside the range where MakeInt32Range
  // leaves them unchanged, so this is also the derivative of the noise
  // argument with respect to the input coordinate.
  double frequency = m_frequency;

  x *= m_frequency;
  y *= m_frequency;
  z *= m_frequency;

  for (int curOctave = 0; curOctave < m_octaveCount; curOctave++) {

    nX = MakeInt32Range (x);
    nY = MakeInt32Range (y);
    nZ = MakeInt32Range (z);

    // Get the coherent-noise value and its gradient from the input value.
    seed = (m_seed + curOctave) & 0xffffffff;
    double gx, gy, gz;
    bool octaveDifferentiable;
    double signal = GradientCoherentNoise3D (nX, nY, nZ, seed,
      m_noiseQuality, gx, gy, gz, octaveDifferentiable);
    value += signal * curPersistence;

    if (!octaveDifferentiable) {
      differentiable = false;
    }
    dx += gx * curPersistence * frequency;
    dy += gy * curPersistence * frequency;
    dz += gz * curPersistence * frequency;

    // Prepare the next octave.
    x *= m_lacunarity;
    y *= m_lacunarity;
    z *= m_lacunarity;
    curPersistence *= m_persistence;
    frequency *= m_lacunarity;
  }

  if (!differentiable) {
    return Sample3::NotDifferentiable (value);
  }
  return Sample3 (value, dx, dy, dz, true);
}
