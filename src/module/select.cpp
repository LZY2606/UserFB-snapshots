// select.cpp
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

#include "interp.h"
#include "module/select.h"

using namespace noise::module;

Select::Select ():
  Module (GetSourceModuleCount ()),
  m_edgeFalloff (DEFAULT_SELECT_EDGE_FALLOFF),
  m_lowerBound (DEFAULT_SELECT_LOWER_BOUND),
  m_upperBound (DEFAULT_SELECT_UPPER_BOUND)
{
}

double Select::GetValue (double x, double y, double z) const
{
  assert (m_pSourceModule[0] != NULL);
  assert (m_pSourceModule[1] != NULL);
  assert (m_pSourceModule[2] != NULL);

  double controlValue = m_pSourceModule[2]->GetValue (x, y, z);
  double alpha;
  if (m_edgeFalloff > 0.0) {
    if (controlValue < (m_lowerBound - m_edgeFalloff)) {
      // The output value from the control module is below the selector
      // threshold; return the output value from the first source module.
      return m_pSourceModule[0]->GetValue (x, y, z);

    } else if (controlValue < (m_lowerBound + m_edgeFalloff)) {
      // The output value from the control module is near the lower end of the
      // selector threshold and within the smooth curve. Interpolate between
      // the output values from the first and second source modules.
      double lowerCurve = (m_lowerBound - m_edgeFalloff);
      double upperCurve = (m_lowerBound + m_edgeFalloff);
      alpha = SCurve3 (
        (controlValue - lowerCurve) / (upperCurve - lowerCurve));
      return LinearInterp (m_pSourceModule[0]->GetValue (x, y, z),
        m_pSourceModule[1]->GetValue (x, y, z),
        alpha);

    } else if (controlValue < (m_upperBound - m_edgeFalloff)) {
      // The output value from the control module is within the selector
      // threshold; return the output value from the second source module.
      return m_pSourceModule[1]->GetValue (x, y, z);

    } else if (controlValue < (m_upperBound + m_edgeFalloff)) {
      // The output value from the control module is near the upper end of the
      // selector threshold and within the smooth curve. Interpolate between
      // the output values from the first and second source modules.
      double lowerCurve = (m_upperBound - m_edgeFalloff);
      double upperCurve = (m_upperBound + m_edgeFalloff);
      alpha = SCurve3 (
        (controlValue - lowerCurve) / (upperCurve - lowerCurve));
      return LinearInterp (m_pSourceModule[1]->GetValue (x, y, z),
        m_pSourceModule[0]->GetValue (x, y, z),
        alpha);

    } else {
      // Output value from the control module is above the selector threshold;
      // return the output value from the first source module.
      return m_pSourceModule[0]->GetValue (x, y, z);
    }
  } else {
    if (controlValue < m_lowerBound || controlValue > m_upperBound) {
      return m_pSourceModule[0]->GetValue (x, y, z);
    } else {
      return m_pSourceModule[1]->GetValue (x, y, z);
    }
  }
}

void Select::SetBounds (double lowerBound, double upperBound)
{
  assert (lowerBound < upperBound);

  m_lowerBound = lowerBound;
  m_upperBound = upperBound;

  // Make sure that the edge falloff curves do not overlap.
  SetEdgeFalloff (m_edgeFalloff);
}

noise::Sample3 Select::GetSample (double x, double y, double z) const
{
  assert (m_pSourceModule[0] != NULL);
  assert (m_pSourceModule[1] != NULL);
  assert (m_pSourceModule[2] != NULL);

  // Sample the control module once.  Its value is identical to the value
  // GetValue() would return at this point, so it can also drive the branch
  // selection.
  Sample3 control = m_pSourceModule[2]->GetSample (x, y, z);
  double controlValue = control.value;

  if (m_edgeFalloff > 0.0) {
    // Decide whether the control value lies inside one of the two smooth
    // transition bands, and calculate the interpolation weight if it does.
    bool inLowerTransition = false;
    double lowerCurve = 0.0;
    double upperCurve = 0.0;

    if (controlValue < (m_lowerBound - m_edgeFalloff)) {
      return m_pSourceModule[0]->GetSample (x, y, z);
    } else if (controlValue < (m_lowerBound + m_edgeFalloff)) {
      lowerCurve = m_lowerBound - m_edgeFalloff;
      upperCurve = m_lowerBound + m_edgeFalloff;
      inLowerTransition = true;
    } else if (controlValue < (m_upperBound - m_edgeFalloff)) {
      return m_pSourceModule[1]->GetSample (x, y, z);
    } else if (controlValue < (m_upperBound + m_edgeFalloff)) {
      lowerCurve = m_upperBound - m_edgeFalloff;
      upperCurve = m_upperBound + m_edgeFalloff;
    } else {
      return m_pSourceModule[0]->GetSample (x, y, z);
    }

    // Both source branches and the control module influence the result in
    // the transition band, so all three are evaluated.  A branch that has no
    // gradient of its own makes the blend not differentiable.
    double t = (controlValue - lowerCurve) / (upperCurve - lowerCurve);
    double alpha = SCurve3 (t);
    double alphaDerivative = SCurve3Derivative (t) / (upperCurve - lowerCurve);

    Sample3 sample0 = m_pSourceModule[0]->GetSample (x, y, z);
    Sample3 sample1 = m_pSourceModule[1]->GetSample (x, y, z);

    // The lower transition blends from source 0 to source 1; the upper
    // transition blends from source 1 to source 0.
    double valueFrom, valueTo, delta;
    double dxFrom, dyFrom, dzFrom, dxTo, dyTo, dzTo;
    if (inLowerTransition) {
      valueFrom = sample0.value;
      valueTo = sample1.value;
      dxFrom = sample0.dx; dyFrom = sample0.dy; dzFrom = sample0.dz;
      dxTo = sample1.dx; dyTo = sample1.dy; dzTo = sample1.dz;
    } else {
      valueFrom = sample1.value;
      valueTo = sample0.value;
      dxFrom = sample1.dx; dyFrom = sample1.dy; dzFrom = sample1.dz;
      dxTo = sample0.dx; dyTo = sample0.dy; dzTo = sample0.dz;
    }
    delta = valueTo - valueFrom;

    double value = LinearInterp (valueFrom, valueTo, alpha);
    if (!sample0.differentiable || !sample1.differentiable
      || !control.differentiable) {
      return Sample3::NotDifferentiable (value);
    }

    double dx = (1.0 - alpha) * dxFrom + alpha * dxTo
              + delta * alphaDerivative * control.dx;
    double dy = (1.0 - alpha) * dyFrom + alpha * dyTo
              + delta * alphaDerivative * control.dy;
    double dz = (1.0 - alpha) * dzFrom + alpha * dzTo
              + delta * alphaDerivative * control.dz;
    return Sample3 (value, dx, dy, dz, true);
  }

  // There is no transition band.  Only the selected source module is
  // evaluated, so an unused branch cannot affect the result.  The selection
  // is a step function of the control value, so it is not differentiable
  // when the control value lies exactly on a selection boundary.
  if (controlValue < m_lowerBound || controlValue > m_upperBound) {
    return m_pSourceModule[0]->GetSample (x, y, z);
  }
  Sample3 sample = m_pSourceModule[1]->GetSample (x, y, z);
  if (controlValue == m_lowerBound || controlValue == m_upperBound) {
    return Sample3::NotDifferentiable (sample.value);
  }
  return sample;
}

void Select::SetEdgeFalloff (double edgeFalloff)
{
  // Make sure that the edge falloff curves do not overlap.
  double boundSize = m_upperBound - m_lowerBound;
  m_edgeFalloff = (edgeFalloff > boundSize / 2)? boundSize / 2: edgeFalloff;
}
