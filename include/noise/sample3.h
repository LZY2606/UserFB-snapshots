// sample3.h
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

#ifndef NOISE_SAMPLE3_H
#define NOISE_SAMPLE3_H

namespace noise
{

  /// @addtogroup libnoise
  /// @{

  /// The result of evaluating a noise module and its spatial gradient at a
  /// single input point.
  ///
  /// @a value is the same value that GetValue() would return at that point.
  ///
  /// @a dx, @a dy, and @a dz are the partial derivatives of the output value
  /// with respect to the three coordinates of the point that was passed to
  /// GetSample().  They are not gradients with respect to any internally
  /// transformed coordinate.
  ///
  /// @a differentiable is @a false when the module cannot provide a reliable
  /// gradient at that point.  When @a differentiable is @a false, @a value is
  /// still correct and @a dx, @a dy, and @a dz are all zero.
  struct Sample3
  {

    /// The output value.
    double value;

    /// The partial derivative of the output value with respect to @a x.
    double dx;

    /// The partial derivative of the output value with respect to @a y.
    double dy;

    /// The partial derivative of the output value with respect to @a z.
    double dz;

    /// Whether the gradient is valid at the sampled point.
    bool differentiable;

    /// Constructor.
    Sample3 ():
      value          (0.0),
      dx             (0.0),
      dy             (0.0),
      dz             (0.0),
      differentiable (false)
    {
    }

    /// Constructor.
    Sample3 (double sampleValue, double sampleDx, double sampleDy,
      double sampleDz, bool sampleDifferentiable):
      value          (sampleValue),
      dx             (sampleDx),
      dy             (sampleDy),
      dz             (sampleDz),
      differentiable (sampleDifferentiable)
    {
    }

    /// Creates a sample with a correct value and an unknown gradient.
    static Sample3 NotDifferentiable (double sampleValue)
    {
      return Sample3 (sampleValue, 0.0, 0.0, 0.0, false);
    }

    /// Creates a sample whose values are known and whose gradient is
    /// identically zero.
    static Sample3 Constant (double sampleValue)
    {
      return Sample3 (sampleValue, 0.0, 0.0, 0.0, true);
    }

  };

  /// @}

}

#endif
