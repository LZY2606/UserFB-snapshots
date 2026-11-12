// target.cpp
//
// Tests for the analytic Sample3 (value + spatial gradient) path.
//
// These tests deliberately use central differences only as an external
// reference for smooth points; the production code never estimates a
// gradient that way.

#include <cmath>
#include <cstdio>
#include "check.h"
#include "noise/mathconsts.h"
#include "noise/noise.h"

using namespace noise;
using namespace noise::module;

namespace
{

  // ----------------------------------------------------------------------
  // Helpers
  // ----------------------------------------------------------------------

  const double VALUE_TOLERANCE = 1e-12;
  const double FD_ABSOLUTE_TOLERANCE = 2e-5;
  const double FD_RELATIVE_TOLERANCE = 2e-5;

  bool IsFinite (const Sample3& sample)
  {
    return std::isfinite (sample.value)
        && std::isfinite (sample.dx)
        && std::isfinite (sample.dy)
        && std::isfinite (sample.dz);
  }

  // Checks that GetSample() returns the same value as GetValue() at the
  // same point.
  void ValueMatchesGetValue (const Module& module, double x, double y,
    double z)
  {
    Sample3 sample = module.GetSample (x, y, z);
    CHECK (IsFinite (sample));
    CHECK (std::fabs (sample.value - module.GetValue (x, y, z))
      <= VALUE_TOLERANCE);
    if (!sample.differentiable) {
      CHECK (sample.dx == 0.0 && sample.dy == 0.0 && sample.dz == 0.0);
    }
  }

  // Central-difference reference gradient of a noise module.
  void ReferenceGradient (const Module& module, double x, double y, double z,
    double step, double& gx, double& gy, double& gz)
  {
    gx = (module.GetValue (x + step, y, z)
        - module.GetValue (x - step, y, z)) / (2.0 * step);
    gy = (module.GetValue (x, y + step, z)
        - module.GetValue (x, y - step, z)) / (2.0 * step);
    gz = (module.GetValue (x, y, z + step)
        - module.GetValue (x, y, z - step)) / (2.0 * step);
  }

  // Compares one analytic gradient component against a central-difference
  // reference.
  bool GradientComponentMatches (double analytic, double reference)
  {
    double error = std::fabs (analytic - reference);
    return error <= FD_ABSOLUTE_TOLERANCE
        + FD_RELATIVE_TOLERANCE * std::fabs (reference);
  }

  // Checks the gradient of a smooth, non-flat point using two central
  // difference step sizes.  The two step sizes must agree with each other,
  // which shows that the point is far enough from a crease for the reference
  // to be meaningful.
  void CheckSmoothPoint (const Module& module, double x, double y, double z)
  {
    Sample3 sample = module.GetSample (x, y, z);
    CHECK (IsFinite (sample));
    CHECK (sample.differentiable);
    CHECK (std::fabs (sample.value - module.GetValue (x, y, z))
      <= VALUE_TOLERANCE);

    double gxCoarse, gyCoarse, gzCoarse;
    double gxFine, gyFine, gzFine;
    ReferenceGradient (module, x, y, z, 2e-5, gxCoarse, gyCoarse, gzCoarse);
    ReferenceGradient (module, x, y, z, 1e-5, gxFine, gyFine, gzFine);

    CHECK (GradientComponentMatches (sample.dx, gxCoarse));
    CHECK (GradientComponentMatches (sample.dy, gyCoarse));
    CHECK (GradientComponentMatches (sample.dz, gzCoarse));
    CHECK (GradientComponentMatches (sample.dx, gxFine));
    CHECK (GradientComponentMatches (sample.dy, gyFine));
    CHECK (GradientComponentMatches (sample.dz, gzFine));

    // The two reference step sizes must not straddle a crease.
    CHECK (std::fabs (gxCoarse - gxFine) <= 1e-3);
    CHECK (std::fabs (gyCoarse - gyFine) <= 1e-3);
    CHECK (std::fabs (gzCoarse - gzFine) <= 1e-3);

    // The point must be non-flat, otherwise a zero gradient would pass
    // trivially.
    double magnitude = std::sqrt (gxFine * gxFine + gyFine * gyFine
      + gzFine * gzFine);
    CHECK (magnitude > 1e-3);
  }

  // A hand-written analytic linear module.  Its gradient is constant, so it
  // can be used to check the coordinate transformations exactly.
  class LinearModule: public Module
  {
    public:
      LinearModule (double a, double b, double c, double d):
        Module (0), m_a (a), m_b (b), m_c (c), m_d (d)
      {
      }

      virtual int GetSourceModuleCount () const
      {
        return 0;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        return (m_a * x) + (m_b * y) + (m_c * z) + m_d;
      }

      virtual Sample3 GetSample (double x, double y, double z) const
      {
        return Sample3 (GetValue (x, y, z), m_a, m_b, m_c, true);
      }

    private:
      double m_a, m_b, m_c, m_d;
  };

  // A user-defined module that only overrides GetValue(), as existing
  // libnoise users do.  The inherited GetSample() must still work.
  class LegacyModule: public Module
  {
    public:
      LegacyModule (): Module (0), m_value (0.0)
      {
      }

      virtual int GetSourceModuleCount () const
      {
        return 0;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        m_value = (x * 0.5) + (y * -0.25) + (z * 0.125) + 0.75;
        return m_value;
      }

    private:
      mutable double m_value;
  };

  // A module that delegates to a source module and counts how often each
  // evaluation entry point is used.
  class CountingModule: public Module
  {
    public:
      CountingModule (): Module (1), m_valueCalls (0), m_sampleCalls (0)
      {
      }

      void Reset ()
      {
        m_valueCalls = 0;
        m_sampleCalls = 0;
      }

      int GetValueCalls () const
      {
        return m_valueCalls;
      }

      int GetSampleCalls () const
      {
        return m_sampleCalls;
      }

      virtual int GetSourceModuleCount () const
      {
        return 1;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        m_valueCalls++;
        return m_pSourceModule[0]->GetValue (x, y, z);
      }

      virtual Sample3 GetSample (double x, double y, double z) const
      {
        m_sampleCalls++;
        return m_pSourceModule[0]->GetSample (x, y, z);
      }

    private:
      mutable int m_valueCalls;
      mutable int m_sampleCalls;
  };

  // The public rotation formula used by RotatePoint, duplicated here so the
  // analytic reference does not depend on the module's private matrix
  // members.
  void BuildRotation (double xAngle, double yAngle, double zAngle,
    double matrix[3][3])
  {
    double xCos = std::cos (xAngle * DEG_TO_RAD);
    double yCos = std::cos (yAngle * DEG_TO_RAD);
    double zCos = std::cos (zAngle * DEG_TO_RAD);
    double xSin = std::sin (xAngle * DEG_TO_RAD);
    double ySin = std::sin (yAngle * DEG_TO_RAD);
    double zSin = std::sin (zAngle * DEG_TO_RAD);

    matrix[0][0] = ySin * xSin * zSin + yCos * zCos;
    matrix[0][1] = xCos * zSin;
    matrix[0][2] = ySin * zCos - yCos * xSin * zSin;
    matrix[1][0] = ySin * xSin * zCos - yCos * zSin;
    matrix[1][1] = xCos * zCos;
    matrix[1][2] = -yCos * xSin * zCos - ySin * zSin;
    matrix[2][0] = -ySin * xCos;
    matrix[2][1] = xSin;
    matrix[2][2] = yCos * xCos;
  }

  // Builds the warped-ridged terrain chain used by the baseline.
  void BuildTerrainChain (Perlin& control, RidgedMulti& ridges, Const& flat,
    Select& select, Turbulence& warp, RotatePoint& rotate, ScalePoint& scale,
    Cache& cache)
  {
    control.SetSeed (17);
    control.SetOctaveCount (3);

    ridges.SetSeed (11);
    ridges.SetOctaveCount (4);

    flat.SetConstValue (0.15);

    select.SetSourceModule (0, flat);
    select.SetSourceModule (1, ridges);
    select.SetControlModule (control);
    select.SetBounds (-0.3, 0.4);
    select.SetEdgeFalloff (0.12);

    warp.SetSourceModule (0, select);
    warp.SetSeed (31);
    warp.SetPower (0.17);
    warp.SetRoughness (3);

    rotate.SetSourceModule (0, warp);
    rotate.SetAngles (19, 31, -13);

    scale.SetSourceModule (0, rotate);
    scale.SetScale (1.7, 0.6, 2.3);

    cache.SetSourceModule (0, scale);
  }

  // ----------------------------------------------------------------------
  // primitive group
  // ----------------------------------------------------------------------

  void PrimitiveLinearCoordinateChain ()
  {
    // value(p) = g * (R * S * p) with S a non-uniform scale and R a
    // multi-axis rotation, then scaled and biased, and added to a second
    // linear branch.  The expected gradient is analytic.
    LinearModule linear (0.7, -1.3, 0.4, 2.0);
    ScalePoint scale;
    scale.SetSourceModule (0, linear);
    scale.SetScale (2.5, -1.5, 0.75);

    RotatePoint rotate;
    rotate.SetSourceModule (0, scale);
    rotate.SetAngles (19.0, 31.0, -13.0);

    ScaleBias bias;
    bias.SetSourceModule (0, rotate);
    bias.SetScale (-3.0);
    bias.SetBias (0.4);

    LinearModule second (-0.2, 0.9, 1.1, -0.6);
    Add add;
    add.SetSourceModule (0, bias);
    add.SetSourceModule (1, second);

    double x = 0.37, y = -0.62, z = 0.18;
    Sample3 sample = add.GetSample (x, y, z);
    CHECK (IsFinite (sample));
    CHECK (sample.differentiable);
    CHECK (std::fabs (sample.value - add.GetValue (x, y, z))
      <= VALUE_TOLERANCE);

    // d/dp of g * R * S * p is the transpose of the scale * transpose of
    // the rotation applied to g.
    double matrix[3][3];
    BuildRotation (19.0, 31.0, -13.0, matrix);
    const double gradient[3] = { 0.7, -1.3, 0.4 };
    double scaledGradient[3] = {
      2.5 * gradient[0], -1.5 * gradient[1], 0.75 * gradient[2]
    };
    double expected[3] = { 0.0, 0.0, 0.0 };
    for (int input = 0; input < 3; input++) {
      for (int rotated = 0; rotated < 3; rotated++) {
        expected[input] += matrix[rotated][input] * scaledGradient[rotated];
      }
      expected[input] *= -3.0;
    }
    expected[0] += -0.2;
    expected[1] += 0.9;
    expected[2] += 1.1;

    CHECK (std::fabs (sample.dx - expected[0]) <= 1e-12);
    CHECK (std::fabs (sample.dy - expected[1]) <= 1e-12);
    CHECK (std::fabs (sample.dz - expected[2]) <= 1e-12);
  }

  void PrimitiveConstGradient ()
  {
    Const constant;
    constant.SetConstValue (12.5);
    ValueMatchesGetValue (constant, 0.3, -0.2, 0.7);
    Sample3 sample = constant.GetSample (0.3, -0.2, 0.7);
    CHECK (sample.differentiable);
    CHECK (sample.dx == 0.0 && sample.dy == 0.0 && sample.dz == 0.0);
  }

  void PrimitiveLegacyModuleFallback ()
  {
    LegacyModule legacy;
    ValueMatchesGetValue (legacy, 0.11, 0.53, -0.29);
    Sample3 sample = legacy.GetSample (0.11, 0.53, -0.29);
    CHECK (!sample.differentiable);
    CHECK (sample.dx == 0.0 && sample.dy == 0.0 && sample.dz == 0.0);
    CHECK (std::fabs (sample.value - legacy.GetValue (0.11, 0.53, -0.29))
      <= VALUE_TOLERANCE);
  }

  void PrimitivePerlinGradient ()
  {
    Perlin perlin;
    perlin.SetSeed (5);
    perlin.SetOctaveCount (5);
    perlin.SetFrequency (1.7);
    perlin.SetPersistence (0.6);
    perlin.SetLacunarity (2.3);
    perlin.SetNoiseQuality (QUALITY_STD);
    CheckSmoothPoint (perlin, 0.313, -0.427, 0.591);

    Perlin best;
    best.SetSeed (5);
    best.SetOctaveCount (4);
    best.SetNoiseQuality (QUALITY_BEST);
    CheckSmoothPoint (best, 0.271, 0.384, -0.517);

    Perlin fast;
    fast.SetSeed (5);
    fast.SetOctaveCount (3);
    fast.SetNoiseQuality (QUALITY_FAST);
    CheckSmoothPoint (fast, 0.413, -0.191, 0.627);
  }

  void PrimitiveRidgedMultiGradient ()
  {
    RidgedMulti ridged;
    ridged.SetSeed (9);
    ridged.SetOctaveCount (5);
    ridged.SetFrequency (1.3);
    ridged.SetLacunarity (2.1);
    CheckSmoothPoint (ridged, 0.313, -0.427, 0.591);
  }

  void PrimitiveTransformsGradient ()
  {
    Perlin perlin;
    perlin.SetSeed (5);
    perlin.SetOctaveCount (4);
    perlin.SetFrequency (1.4);
    perlin.SetPersistence (0.55);

    ScalePoint scale;
    scale.SetSourceModule (0, perlin);
    scale.SetScale (0.3, 2.0, -1.5);
    CheckSmoothPoint (scale, 0.313, -0.427, 0.591);

    RotatePoint rotate;
    rotate.SetSourceModule (0, perlin);
    rotate.SetAngles (19.0, 31.0, -13.0);
    CheckSmoothPoint (rotate, 0.313, -0.427, 0.591);

    // A combination of a non-uniform scale and a multi-axis rotation.
    RotatePoint combined;
    combined.SetSourceModule (0, scale);
    combined.SetAngles (-27.0, 14.0, 63.0);
    CheckSmoothPoint (combined, 0.313, -0.427, 0.591);

    LinearModule linear (0.7, -1.3, 0.4, 2.0);
    ScaleBias bias;
    bias.SetSourceModule (0, linear);
    bias.SetScale (3.0);
    bias.SetBias (-2.0);
    Sample3 sample = bias.GetSample (0.2, 0.5, -0.1);
    CHECK (sample.differentiable);
    CHECK (std::fabs (sample.dx - 2.1) <= VALUE_TOLERANCE);
    CHECK (std::fabs (sample.dy - -3.9) <= VALUE_TOLERANCE);
    CHECK (std::fabs (sample.dz - 1.2) <= VALUE_TOLERANCE);
  }

  void PrimitiveSeamDifferentiable ()
  {
    // Standard and best quality noise have continuous derivatives at
    // integer lattice seams; only QUALITY_FAST is rejected there.
    Perlin standard;
    standard.SetNoiseQuality (QUALITY_STD);
    standard.SetOctaveCount (2);
    Sample3 standardSample = standard.GetSample (1.0, 2.0, -3.0);
    ValueMatchesGetValue (standard, 1.0, 2.0, -3.0);
    CHECK (standardSample.differentiable);

    Perlin best;
    best.SetNoiseQuality (QUALITY_BEST);
    best.SetOctaveCount (3);
    Sample3 bestSample = best.GetSample (1.0, 2.0, -3.0);
    ValueMatchesGetValue (best, 1.0, 2.0, -3.0);
    CHECK (bestSample.differentiable);

    // A positive integer, a negative integer and a zero on each axis are all
    // lattice seams for QUALITY_FAST.
    Perlin fast;
    fast.SetNoiseQuality (QUALITY_FAST);
    fast.SetOctaveCount (2);
    double seamPoints[6][3] = {
      { 1.0, 0.37, 0.51 },
      { -3.0, 0.37, 0.51 },
      { 0.37, 2.0, 0.51 },
      { 0.37, -2.0, 0.51 },
      { 0.37, 0.51, 3.0 },
      { 0.0, 0.51, -7.0 }
    };
    for (int i = 0; i < 6; i++) {
      double x = seamPoints[i][0];
      double y = seamPoints[i][1];
      double z = seamPoints[i][2];
      ValueMatchesGetValue (fast, x, y, z);
      Sample3 fastSample = fast.GetSample (x, y, z);
      CHECK (!fastSample.differentiable);
      CHECK (fastSample.dx == 0.0 && fastSample.dy == 0.0
        && fastSample.dz == 0.0);
    }
  }

  void PrimitiveFastOffLattice ()
  {
    Perlin fast;
    fast.SetNoiseQuality (QUALITY_FAST);
    fast.SetOctaveCount (2);
    CheckSmoothPoint (fast, 1.413, 2.219, -3.627);
  }

  // ----------------------------------------------------------------------
  // graph group
  // ----------------------------------------------------------------------

  void GraphTurbulenceGradient ()
  {
    RidgedMulti ridges;
    ridges.SetSeed (9);
    ridges.SetOctaveCount (4);
    ridges.SetFrequency (1.3);

    Turbulence warp;
    warp.SetSourceModule (0, ridges);
    warp.SetSeed (31);
    warp.SetPower (0.17);
    warp.SetRoughness (3);
    CheckSmoothPoint (warp, 0.221, -0.337, 0.529);
  }

  void GraphSelectTransitionGradient ()
  {
    // The control value sits in the middle of a transition band, where both
    // source branches and the control gradient influence the result.
    Perlin branch0;
    branch0.SetSeed (21);
    branch0.SetOctaveCount (3);
    branch0.SetFrequency (1.2);

    RidgedMulti branch1;
    branch1.SetSeed (11);
    branch1.SetOctaveCount (4);

    Const control;
    control.SetConstValue (0.0);

    Select select;
    select.SetSourceModule (0, branch0);
    select.SetSourceModule (1, branch1);
    select.SetControlModule (control);
    select.SetBounds (0.0, 0.5);
    select.SetEdgeFalloff (0.25);

    CheckSmoothPoint (select, 0.283, -0.419, 0.557);

    // The same construction with a live control noise, found at a point that
    // lies inside the lower transition band.
    Perlin liveControl;
    liveControl.SetSeed (17);
    liveControl.SetOctaveCount (3);

    Select live;
    live.SetSourceModule (0, branch0);
    live.SetSourceModule (1, branch1);
    live.SetControlModule (liveControl);
    live.SetBounds (-0.2, 0.35);
    live.SetEdgeFalloff (0.15);

    bool found = false;
    double fx = 0.0, fy = 0.0, fz = 0.0;
    for (int i = 0; i < 40 && !found; i++) {
      for (int j = 0; j < 40 && !found; j++) {
        double x = 0.071 + i * 0.0137;
        double y = -0.083 + j * 0.0119;
        double z = 0.211;
        double controlValue = liveControl.GetValue (x, y, z);
        if (controlValue > (-0.2 - 0.15) && controlValue < (-0.2 + 0.15)) {
          found = true;
          fx = x;
          fy = y;
          fz = z;
        }
      }
    }
    CHECK (found);
    CheckSmoothPoint (live, fx, fy, fz);
  }

  void GraphNonDifferentiablePropagation ()
  {
    // A non-differentiable dependency propagates through an ordinary
    // combiner, and the returned gradient is zero.
    Perlin fast;
    fast.SetNoiseQuality (QUALITY_FAST);
    fast.SetOctaveCount (2);

    Perlin standard;
    standard.SetSeed (4);
    standard.SetOctaveCount (2);

    Add add;
    add.SetSourceModule (0, fast);
    add.SetSourceModule (1, standard);

    Sample3 sample = add.GetSample (1.0, 2.0, 3.0);
    CHECK (IsFinite (sample));
    CHECK (!sample.differentiable);
    CHECK (sample.dx == 0.0 && sample.dy == 0.0 && sample.dz == 0.0);
    CHECK (std::fabs (sample.value - add.GetValue (1.0, 2.0, 3.0))
      <= VALUE_TOLERANCE);

    // For comparison, the same point away from the lattice seam is
    // differentiable.
    Sample3 smooth = add.GetSample (1.413, 2.219, 3.627);
    CHECK (smooth.differentiable);
  }

  void GraphCompositeGradient ()
  {
    Perlin control;
    RidgedMulti ridges;
    Const flat;
    Select select;
    Turbulence warp;
    RotatePoint rotate;
    ScalePoint scale;
    Cache cache;
    BuildTerrainChain (control, ridges, flat, select, warp, rotate, scale,
      cache);

    // Rebuild the same graph without the cache for reference values.
    ScalePoint uncachedScale;
    uncachedScale.SetSourceModule (0, rotate);
    uncachedScale.SetScale (1.7, 0.6, 2.3);

    for (int i = 0; i < 4; i++) {
      double x = 0.13 + i * 0.071;
      double y = -0.27 + i * 0.033;
      double z = 0.41 - i * 0.017;

      Sample3 cachedSample = cache.GetSample (x, y, z);
      CHECK (IsFinite (cachedSample));
      CHECK (std::fabs (cachedSample.value - uncachedScale.GetValue (x, y,
        z)) <= VALUE_TOLERANCE);

      // The point is smooth and non-flat, so the full composite gradient is
      // checked against the reference.
      CheckSmoothPoint (uncachedScale, x, y, z);
    }
  }

  void GraphRidgedMultiNotDifferentiableAtRidgeTip ()
  {
    // At an integer lattice point the first octave of ridged noise has an
    // exact zero magnitude before the absolute value is taken.
    RidgedMulti ridged;
    ridged.SetSeed (11);
    ridged.SetOctaveCount (3);
    Sample3 sample = ridged.GetSample (1.0, 2.0, 3.0);
    CHECK (IsFinite (sample));
    CHECK (!sample.differentiable);
    CHECK (std::fabs (sample.value - ridged.GetValue (1.0, 2.0, 3.0))
      <= VALUE_TOLERANCE);
    CHECK (sample.dx == 0.0 && sample.dy == 0.0 && sample.dz == 0.0);
  }

  void GraphSelectNoTransitionBoundary ()
  {
    Const branch0;
    branch0.SetConstValue (1.0);
    Const branch1;
    branch1.SetConstValue (2.0);

    // No transition band and the control value lies exactly on the lower
    // bound: the step is not differentiable, but the value is correct.
    Const control;
    control.SetConstValue (0.0);
    Select select;
    select.SetSourceModule (0, branch0);
    select.SetSourceModule (1, branch1);
    select.SetControlModule (control);
    select.SetBounds (0.0, 0.5);
    select.SetEdgeFalloff (0.0);

    Sample3 lower = select.GetSample (0.2, -0.4, 0.9);
    CHECK (!lower.differentiable);
    CHECK (std::fabs (lower.value - select.GetValue (0.2, -0.4, 0.9))
      <= VALUE_TOLERANCE);
    CHECK (lower.value == 2.0);

    // Control value exactly on the upper bound.
    Const upperControl;
    upperControl.SetConstValue (0.5);
    Select upper;
    upper.SetSourceModule (0, branch0);
    upper.SetSourceModule (1, branch1);
    upper.SetControlModule (upperControl);
    upper.SetBounds (0.0, 0.5);
    upper.SetEdgeFalloff (0.0);

    Sample3 upperSample = upper.GetSample (0.2, -0.4, 0.9);
    CHECK (!upperSample.differentiable);
    CHECK (std::fabs (upperSample.value - upper.GetValue (0.2, -0.4, 0.9))
      <= VALUE_TOLERANCE);

    // A control value strictly inside the band is differentiable.
    Const insideControl;
    insideControl.SetConstValue (0.25);
    Select inside;
    inside.SetSourceModule (0, branch0);
    inside.SetSourceModule (1, branch1);
    inside.SetControlModule (insideControl);
    inside.SetBounds (0.0, 0.5);
    inside.SetEdgeFalloff (0.0);

    Sample3 insideSample = inside.GetSample (0.2, -0.4, 0.9);
    CHECK (insideSample.differentiable);
    CHECK (insideSample.value == 2.0);
    CHECK (insideSample.dx == 0.0 && insideSample.dy == 0.0
      && insideSample.dz == 0.0);
  }

  void GraphSelectUnusedBranchNotEvaluated ()
  {
    Perlin branch0;
    branch0.SetSeed (21);
    branch0.SetOctaveCount (2);
    Const branch1;
    branch1.SetConstValue (2.0);
    Const control;
    control.SetConstValue (0.75);

    CountingModule count0;
    count0.SetSourceModule (0, branch0);
    CountingModule count1;
    count1.SetSourceModule (0, branch1);
    CountingModule countControl;
    countControl.SetSourceModule (0, control);

    // Control value 0.75 is above the upper bound, so only branch 0 is
    // selected.  The unused branch and the control are not sampled with
    // GetSample().
    Select above;
    above.SetSourceModule (0, count0);
    above.SetSourceModule (1, count1);
    above.SetControlModule (countControl);
    above.SetBounds (0.0, 0.5);
    above.SetEdgeFalloff (0.0);

    count0.Reset ();
    count1.Reset ();
    countControl.Reset ();
    above.GetSample (0.2, -0.4, 0.9);
    CHECK (count0.GetSampleCalls () == 1);
    CHECK (count1.GetSampleCalls () == 0);
    CHECK (count1.GetValueCalls () == 0);
    CHECK (countControl.GetSampleCalls () == 1);
    CHECK (countControl.GetValueCalls () == 0);

    // A transition band evaluates both branches and the control.
    Select transition;
    transition.SetSourceModule (0, count0);
    transition.SetSourceModule (1, count1);
    transition.SetControlModule (countControl);
    transition.SetBounds (0.0, 0.5);
    transition.SetEdgeFalloff (0.25);

    Const midControl;
    midControl.SetConstValue (0.0);
    CountingModule countMid;
    countMid.SetSourceModule (0, midControl);

    Select midTransition;
    midTransition.SetSourceModule (0, count0);
    midTransition.SetSourceModule (1, count1);
    midTransition.SetControlModule (countMid);
    midTransition.SetBounds (0.0, 0.5);
    midTransition.SetEdgeFalloff (0.25);

    count0.Reset ();
    count1.Reset ();
    countMid.Reset ();
    midTransition.GetSample (0.2, -0.4, 0.9);
    CHECK (count0.GetSampleCalls () == 1);
    CHECK (count1.GetSampleCalls () == 1);
    CHECK (countMid.GetSampleCalls () == 1);
    CHECK (countMid.GetValueCalls () == 0);
  }

  void GraphTurbulenceWithLegacySource ()
  {
    // A source module without an analytic gradient still produces the
    // correct value; the gradient flag is simply false.
    LegacyModule legacy;
    Turbulence warp;
    warp.SetSourceModule (0, legacy);
    warp.SetSeed (31);
    warp.SetPower (0.17);
    warp.SetRoughness (2);

    Sample3 sample = warp.GetSample (0.221, -0.337, 0.529);
    CHECK (IsFinite (sample));
    CHECK (!sample.differentiable);
    CHECK (std::fabs (sample.value - warp.GetValue (0.221, -0.337, 0.529))
      <= VALUE_TOLERANCE);
    CHECK (sample.dx == 0.0 && sample.dy == 0.0 && sample.dz == 0.0);
  }

  // ----------------------------------------------------------------------
  // cache group
  // ----------------------------------------------------------------------

  void CacheScalarThenSample ()
  {
    Perlin control;
    RidgedMulti ridges;
    Const flat;
    Select select;
    Turbulence warp;
    RotatePoint rotate;
    ScalePoint scale;
    Cache cache;
    BuildTerrainChain (control, ridges, flat, select, warp, rotate, scale,
      cache);

    CountingModule counting;
    counting.SetSourceModule (0, scale);
    cache.SetSourceModule (0, counting);

    double x = 0.13, y = -0.27, z = 0.41;
    counting.Reset ();

    double value = cache.GetValue (x, y, z);
    CHECK (counting.GetValueCalls () == 1);
    CHECK (counting.GetSampleCalls () == 0);

    // A repeated scalar query is served from the cache.
    CHECK (cache.GetValue (x, y, z) == value);
    CHECK (counting.GetValueCalls () == 1);

    // A full sample after a scalar query must evaluate the source once.
    Sample3 sample = cache.GetSample (x, y, z);
    CHECK (counting.GetSampleCalls () == 1);
    CHECK (std::fabs (sample.value - value) <= VALUE_TOLERANCE);

    // Repeated full sample at the same point is a cache hit.
    Sample3 repeat = cache.GetSample (x, y, z);
    CHECK (counting.GetSampleCalls () == 1);
    CHECK (repeat.value == sample.value);
    CHECK (repeat.dx == sample.dx && repeat.dy == sample.dy
      && repeat.dz == sample.dz);
    CHECK (repeat.differentiable == sample.differentiable);

    // GetValue() after a full sample reuses the cached value.
    CHECK (cache.GetValue (x, y, z) == sample.value);
    CHECK (counting.GetValueCalls () == 1);
    CHECK (counting.GetSampleCalls () == 1);
  }

  void CacheSampleThenScalar ()
  {
    Perlin control;
    RidgedMulti ridges;
    Const flat;
    Select select;
    Turbulence warp;
    RotatePoint rotate;
    ScalePoint scale;
    Cache cache;
    BuildTerrainChain (control, ridges, flat, select, warp, rotate, scale,
      cache);

    CountingModule counting;
    counting.SetSourceModule (0, scale);
    cache.SetSourceModule (0, counting);

    double x = 0.13, y = -0.27, z = 0.41;
    counting.Reset ();

    Sample3 sample = cache.GetSample (x, y, z);
    CHECK (counting.GetSampleCalls () == 1);
    CHECK (counting.GetValueCalls () == 0);

    // A scalar query after a full sample is served from the sample.
    CHECK (cache.GetValue (x, y, z) == sample.value);
    CHECK (counting.GetValueCalls () == 0);
  }

  void CacheRevisitAfterOtherPoint ()
  {
    Perlin control;
    RidgedMulti ridges;
    Const flat;
    Select select;
    Turbulence warp;
    RotatePoint rotate;
    ScalePoint scale;
    Cache cache;
    BuildTerrainChain (control, ridges, flat, select, warp, rotate, scale,
      cache);

    CountingModule counting;
    counting.SetSourceModule (0, scale);
    cache.SetSourceModule (0, counting);

    double x1 = 0.13, y1 = -0.27, z1 = 0.41;
    double x2 = 0.51, y2 = 0.17, z2 = -0.23;

    counting.Reset ();
    Sample3 first = cache.GetSample (x1, y1, z1);
    CHECK (counting.GetSampleCalls () == 1);

    Sample3 other = cache.GetSample (x2, y2, z2);
    CHECK (counting.GetSampleCalls () == 2);

    // Returning to the first point must evaluate again, and must match the
    // first result exactly.
    Sample3 revisited = cache.GetSample (x1, y1, z1);
    CHECK (counting.GetSampleCalls () == 3);
    CHECK (std::fabs (revisited.value - first.value) <= VALUE_TOLERANCE);
    CHECK (std::fabs (revisited.dx - first.dx) <= VALUE_TOLERANCE);
    CHECK (std::fabs (revisited.dy - first.dy) <= VALUE_TOLERANCE);
    CHECK (std::fabs (revisited.dz - first.dz) <= VALUE_TOLERANCE);
    CHECK (revisited.differentiable == first.differentiable);

    // The cached scalar value at the second point is also consistent with
    // the uncached graph.
    CHECK (std::fabs (other.value - scale.GetValue (x2, y2, z2))
      <= VALUE_TOLERANCE);
    CHECK (std::fabs (first.value - scale.GetValue (x1, y1, z1))
      <= VALUE_TOLERANCE);
  }

  void CacheMatchesUncachedGraph ()
  {
    Perlin control;
    RidgedMulti ridges;
    Const flat;
    Select select;
    Turbulence warp;
    RotatePoint rotate;
    ScalePoint scale;
    Cache cache;
    BuildTerrainChain (control, ridges, flat, select, warp, rotate, scale,
      cache);

    // Scalars from the cached graph are compared against the uncached
    // source module directly.
    for (int i = 0; i < 8; i++) {
      double x = 0.13 + i * 0.071;
      double y = -0.27 + i * 0.033;
      double z = 0.41 - i * 0.017;
      CHECK (std::fabs (cache.GetValue (x, y, z) - scale.GetValue (x, y, z))
        <= VALUE_TOLERANCE);
      Sample3 sample = cache.GetSample (x, y, z);
      CHECK (std::fabs (sample.value - scale.GetValue (x, y, z))
        <= VALUE_TOLERANCE);
    }
  }

}

void register_cases ()
{
  add_case ("primitive", "linear-coordinate-chain",
    PrimitiveLinearCoordinateChain);
  add_case ("primitive", "const-gradient", PrimitiveConstGradient);
  add_case ("primitive", "legacy-module-fallback",
    PrimitiveLegacyModuleFallback);
  add_case ("primitive", "perlin-gradient", PrimitivePerlinGradient);
  add_case ("primitive", "ridgedmulti-gradient",
    PrimitiveRidgedMultiGradient);
  add_case ("primitive", "transform-gradient",
    PrimitiveTransformsGradient);
  add_case ("primitive", "seam-differentiable",
    PrimitiveSeamDifferentiable);
  add_case ("primitive", "fast-off-lattice", PrimitiveFastOffLattice);

  add_case ("graph", "turbulence-gradient", GraphTurbulenceGradient);
  add_case ("graph", "select-transition-gradient",
    GraphSelectTransitionGradient);
  add_case ("graph", "composite-gradient", GraphCompositeGradient);
  add_case ("graph", "non-differentiable-propagation",
    GraphNonDifferentiablePropagation);
  add_case ("graph", "ridgedmulti-ridge-tip",
    GraphRidgedMultiNotDifferentiableAtRidgeTip);
  add_case ("graph", "select-no-transition-boundary",
    GraphSelectNoTransitionBoundary);
  add_case ("graph", "select-unused-branch",
    GraphSelectUnusedBranchNotEvaluated);
  add_case ("graph", "turbulence-legacy-source",
    GraphTurbulenceWithLegacySource);

  add_case ("cache", "scalar-then-sample", CacheScalarThenSample);
  add_case ("cache", "sample-then-scalar", CacheSampleThenScalar);
  add_case ("cache", "revisit-after-other-point",
    CacheRevisitAfterOtherPoint);
  add_case ("cache", "matches-uncached-graph", CacheMatchesUncachedGraph);
}
