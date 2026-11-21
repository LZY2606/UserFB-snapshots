// CPU silhouette (contour) extraction for NURBS surfaces under an
// orthographic camera.
//
// This file is an addition to Geometric Tools.  It is distributed under the
// same license as the rest of the repository (see LICENSE-BOOST and
// LICENSE-MIT at the repository root).
//
// ---------------------------------------------------------------------------
// Overview
// ---------------------------------------------------------------------------
// Given an owning list of NURBS surfaces and an orthographic camera whose
// object-to-eye direction is the unit vector D, the silhouette of a surface
// S(u,v) is the zero set of
//
//     f(u,v) = (S_u(u,v) x S_v(u,v)) . D
//
// in the parametric domain of the surface.  Surface boundary curves are NOT
// automatically silhouettes; only the interior zero set of f is extracted.
// Every surface participates in occlusion, and the front part of a surface
// can occlude parts of itself.
//
// Supported input scope (validated at construction time):
//   * 3D surfaces (N = 3) with double-precision controls,
//   * degree 2 or 3 independently in u and v,
//   * clamped (open), non-periodic knot vectors,
//   * interior knot multiplicity at most degree-1 (the surface is C1),
//   * strictly positive, finite weights,
//   * trimmed surfaces are not supported.
// The surface is also required to be regular (S_u x S_v nonzero).  This
// cannot be checked everywhere at construction time; a numerically vanishing
// surface normal detected during extraction is reported as an unresolved
// region instead of being silently accepted.
//
// ---------------------------------------------------------------------------
// Extraction and connection rules
// ---------------------------------------------------------------------------
// The zero set of f is found on the true parametric surface, not on a fixed
// sampled grid:
//   1. Each surface is split into its Bezier elements using the knot
//      partition.  On an element, w^2*f (w = weight function) is a polynomial
//      whose Bernstein coefficients are computed exactly from the homogeneous
//      control net.  The convex-hull property of the Bernstein form is used
//      to discard parameter regions that cannot contain a zero.
//   2. Regions that cannot be excluded are refined by quadtree subdivision
//      (de Casteljau splits, so the coefficient bounds stay exact) until a
//      Newton iteration on f converges to a seed point on the zero set.
//   3. From each seed the branch is traced with a predictor-corrector method
//      in the parameter domain.  The parametric trace direction is
//      (-f_v, f_u), mapped to 3D by the surface first derivative.  Branches
//      end on the domain boundary (open branch), on the seed they started
//      from (closed loop), or at an explicitly unresolved region.
//   4. Seeds that lie on the same traced branch (including the same
//      parametric event seen from two adjacent knot elements) are consumed,
//      so each parametric event is connected exactly once.  Connection is
//      decided in the parameter domain of each surface; different parameters
//      that map to the same spatial point are never welded together.
// Interior closed loops are preserved; a loop is found whenever the Bernstein
// exclusion test cannot discard its region, independent of any boundary.
//
// ---------------------------------------------------------------------------
// Tolerances
// ---------------------------------------------------------------------------
//   * spatialTolerance: distances in 3D.  A ray-surface intersection only
//     occludes when its distance along +D exceeds this tolerance, so the
//     silhouette point itself never counts as its own occluder.
//   * parameterTolerance: resolution in the (u,v) domain; bounds the
//     smallest region and the trace step.
//   * angleTolerance: every reported sample satisfies
//     |Dot(N,D)|/|N| <= angleTolerance with N = S_u x S_v evaluated at the
//     sample parameters.
//   * regionBudget: maximum number of region nodes (exclusion quadtrees and
//     ray-cast quadtrees together).  Exhaustion is reported, never silently
//     ignored.
// Every reported position is the surface evaluated at the reported (u,v), so
// back-substitution into the original surface reproduces the output.
//
// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------
// Visibility is decided along the continuous parameter of each curve, not
// per polyline segment: occlusion state changes between samples are located
// by bisection along the traced branch and the intervals are split there.
// A silhouette point X is occluded when the ray X + t*D meets some surface
// (including its own) at a distance t > spatialTolerance.  Hidden parts of a
// curve are kept in the output; visible and hidden intervals never join
// across an occluded gap.
//
// ---------------------------------------------------------------------------
// Unresolved states
// ---------------------------------------------------------------------------
// Extraction is conservative: a region that cannot be excluded and cannot be
// resolved is reported, never treated as empty.  Result::complete is true
// only when no unresolved regions remain.  UnresolvedRegion carries the
// surface index, a UV box, and a reason:
//   * ZeroSetGradientVanishes - the gradient of f vanishes on (or next to)
//     the zero set, so branches cannot be traced reliably.
//   * NonIsolatedZeroSet - f vanishes on a whole region (for example a plane
//     viewed along its own tangent direction); the silhouette is not an
//     isolated curve there.
//   * BudgetExhausted - the region work budget ran out before this region
//     could be excluded or resolved.
//   * RayUnresolved - a visibility ray could not be classified.
//   * OcclusionBoundaryUnresolved - an occlusion state change could not be
//     located; the neighboring interval is marked Unknown.
//   * SurfaceDegenerate - the surface normal vanished (input outside the
//     supported regular-C1 scope).
// Visibility intervals have a state Visible, Hidden or Unknown; Unknown is
// used instead of guessing when a ray or an occlusion boundary is
// unresolved.
//
// The curve parameter t used by VisibilityInterval is the normalized
// polyline chord-length parameter of the curve samples: t=0 at the first
// sample, t=1 at the last (for closed curves the polyline wraps from the
// last sample back to the first).  On closed curves a single interval may
// wrap across t=1/t=0, in which case t0 > t1.
//
// All output containers own their data; the result remains valid after the
// input surfaces and this object are destroyed.

#pragma once

#ifdef __cplusplus

#include <Mathematics/NURBSSurface.h>
#include <Mathematics/Vector3.h>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace gte
{
    class NURBSSilhouette
    {
    public:
        struct Options
        {
            // 3D distance tolerance used by occlusion queries.
            double spatialTolerance = 1.0e-8;

            // Parameter-domain resolution for regions, seeds and tracing.
            double parameterTolerance = 1.0e-10;

            // Bound on |Dot(N,D)|/|N| at reported samples.
            double angleTolerance = 1.0e-6;

            // Work budget counted in region nodes (exclusion and ray quadtrees).
            size_t regionBudget = 200000;
        };

        enum class IntervalState
        {
            Visible,
            Hidden,
            Unknown
        };

        enum class UnresolvedReason
        {
            ZeroSetGradientVanishes,
            NonIsolatedZeroSet,
            BudgetExhausted,
            RayUnresolved,
            OcclusionBoundaryUnresolved,
            SurfaceDegenerate
        };

        struct Sample
        {
            // Index of the surface in the input list.
            size_t surface = 0;

            // Parameters of the sample on that surface.
            double u = 0.0;
            double v = 0.0;

            // Position, exactly S(u,v) of the source surface.
            Vector3<double> position{ 0.0, 0.0, 0.0 };

            // Oriented unit tangent: the parametric direction (-f_v, f_u)
            // mapped through the surface first derivative and normalized.
            Vector3<double> tangent{ 0.0, 0.0, 0.0 };
        };

        struct VisibilityInterval
        {
            // Curve-parameter range, normalized chord length in [0,1].  On a
            // closed curve an interval may wrap: t0 > t1 means the interval
            // runs from t0 through 1 and continues from 0 to t1.
            double t0 = 0.0;
            double t1 = 0.0;
            IntervalState state = IntervalState::Unknown;
        };

        struct Curve
        {
            // Index of the surface in the input list.
            size_t surface = 0;

            // True for a closed loop, false for an open branch (ending on the
            // domain boundary or at an explicitly unresolved region).
            bool closed = false;

            // Samples in connected order along the branch.
            std::vector<Sample> samples;

            // Normalized chord-length parameter of each sample, in [0,1].
            std::vector<double> parameter;

            // Visibility intervals partitioning [0,1] (see the file header
            // for the wrapping convention on closed curves).
            std::vector<VisibilityInterval> intervals;
        };

        struct UnresolvedRegion
        {
            // Index of the surface in the input list.
            size_t surface = 0;

            // UV box of the unresolved region, inside the surface domain.
            double umin = 0.0;
            double umax = 0.0;
            double vmin = 0.0;
            double vmax = 0.0;

            UnresolvedReason reason = UnresolvedReason::BudgetExhausted;
        };

        struct Result
        {
            // True only when no unresolved regions remain.
            bool complete = false;

            // Extracted silhouette branches in connected order.
            std::vector<Curve> curves;

            // Regions that could neither be excluded nor resolved.
            std::vector<UnresolvedRegion> unresolved;
        };

        // Construction takes shared ownership of the surfaces and validates
        // the camera, the surfaces and the tolerances.  Invalid input is
        // rejected here, before any work is done; inspect IsValid() and
        // GetError().  The view direction must be a unit vector pointing from
        // the object toward the eye.
        NURBSSilhouette(
            std::vector<std::shared_ptr<NURBSSurface<3, double>>> surfaces,
            Vector3<double> const& viewDirection,
            Options const& options);

        inline bool IsValid() const
        {
            return mValid;
        }

        inline std::string const& GetError() const
        {
            return mError;
        }

        inline Options const& GetOptions() const
        {
            return mOptions;
        }

        // Computes the silhouette.  Returns false when the object was
        // constructed with invalid input; in that case 'output' is left
        // untouched.  Otherwise 'output' is replaced by the new result, whose
        // 'complete' member reports whether unresolved regions remain.
        bool Compute(Result& output) const;

    private:
        std::vector<std::shared_ptr<NURBSSurface<3, double>>> mSurfaces;
        Vector3<double> mDirection{ 0.0, 0.0, 1.0 };
        Options mOptions;
        bool mValid = false;
        std::string mError;
    };
}

#endif  // __cplusplus
