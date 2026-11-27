/** @file */

#ifndef TINYSPLINE_SURFACE_H
#define TINYSPLINE_SURFACE_H

#include "tinyspline.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Lofted NURBS Surface
 *
 * A ::tsLoftSurface stores a rational tensor product B-Spline surface that
 * lofts (skins) a set of section curves. All data is owned by the instance
 * itself: the input sections are copied during construction, so releasing or
 * editing them after ::ts_loft_surface_init (or ::ts_loft_surface_new) has no
 * effect on the resulting surface.
 *
 * ## Control net layout
 *
 * The homogeneous control net is a regular tensor product with
 *
 *     n_u = ::ts_loft_surface_num_u_control_points
 *     n_v = ::ts_loft_surface_num_v_control_points
 *
 * control points along the u and v axis, respectively. Every control point has
 * four components (wx, wy, wz, w) and is stored in row-major order:
 *
 *     index(i, j) = 4 * (i * n_v + j)
 *
 * where i ranges over [0, n_u) (along the section curves) and j ranges over
 * [0, n_v) (along the lofting direction). In other words, all v control points
 * of u row i are stored consecutively. The weight component w is guaranteed to
 * be positive.
 *
 * ## Knot vectors and degrees
 *
 * Both knot vectors are clamped and normalized to the domain [0, 1]. The
 * u knot vector (::ts_loft_surface_u_knots_ptr) is shared by all sections and
 * results from making their u basis functions identical (common knot vector
 * plus common degree, not just a common number of control points). It has
 * n_u + degree_u + 1 knots. The v knot vector
 * (::ts_loft_surface_v_knots_ptr) has n_v + degree_v + 1 knots:
 *
 * - with two sections, v is a degree 1 ruled interpolation with knots
 *   [0, 0, 1, 1],
 * - with three or more sections, v is a degree 3 clamped B-Spline following
 *   the same uniform natural-cubic convention as
 *   ::ts_bspline_interpolate_cubic_natural (computed in full ::tsReal
 *   precision so that the input sections are reproduced exactly), so the
 *   surface passes through section j at v = j / (count - 1).
 *
 * ## Evaluation
 *
 * ::ts_loft_surface_eval evaluates the rational surface at (u, v) in [0, 1]
 * and returns the Euclidean point together with the analytic first order
 * partial derivatives. A unit normal can be obtained as the normalized cross
 * product of the u derivative over the v derivative (du x dv).
 *
 * @{
 */

/**
 * A rational tensor product B-Spline surface created by lofting section
 * curves. Use ::ts_loft_surface_init to create an empty instance.
 */
typedef struct
{
	struct tsLoftSurfaceImpl *pImpl; /**< The actual implementation. */
} tsLoftSurface;

/**
 * Creates a new surface whose data points to NULL.
 *
 * @return
 * 	A new surface whose data points to NULL.
 */
tsLoftSurface TINYSPLINE_API
ts_loft_surface_init(void);

/**
 * Lofts \p count section curves into a rational tensor product B-Spline
 * surface.
 *
 * The sections must be clamped B-Splines of degree 1, 2, or 3 with at least
 * two sections in total. Each section must have three-dimensional control
 * points (non-rational curves) or four-dimensional control points of the form
 * (wx, wy, wz, w) (rational curves). In the rational case, all weights must be
 * positive and finite. The degrees, numbers of control points, interior
 * knots, and parameter domains of the sections may differ: the parameter
 * domain of every section is normalized to [0, 1], the weights of each section
 * are scaled by the inverse weight of its first control point, and the u
 * basis functions are made identical by degree elevation and knot insertion.
 *
 * The v axis passes through section j at v = j / (\p count - 1). For each
 * homogeneous control point position, the four components are interpolated
 * along v with the uniform natural cubic spline convention used by
 * ::ts_bspline_interpolate_cubic_natural. With two sections, the result is the
 * corresponding ruled (linear) interpolation.
 *
 * @param[in] sections
 * 	The section curves to loft. Must contain at least two entries. All
 * 	entries must be valid (initialized) splines.
 * @param[in] count
 * 	The number of section curves.
 * @param[out] out
 * 	The resulting surface. On failure, \p out is left untouched.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_DIM_ZERO
 * 	If a section has neither 3 nor 4 dimensions.
 * @return TS_DEG_GE_NCTRLP
 * 	If a section has degree 0 or a degree above 3 (the supported
 * 	clamped section degrees are 1, 2, and 3), or if it has fewer than
 * 	two control points.
 * @return TS_NUM_POINTS
 * 	If \p count is smaller than 2.
 * @return TS_MULTIPLICITY
 * 	If a section is not clamped or contains a knot whose multiplicity
 * 	exceeds its order (a true discontinuity).
 * @return TS_NO_RESULT
 * 	If an input contains non-finite data or non-positive weights, or if
 * 	the resulting control net contains a non-positive homogeneous
 * 	weight (which would cause denominator poles at interior parameters).
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_loft_surface_new(const tsBSpline *sections,
                    size_t count,
                    tsLoftSurface *out,
                    tsStatus *status);

/**
 * Creates a deep copy of \p src and stores the copied data in \p dest.
 *
 * @param[in] src
 * 	The surface to be deep copied.
 * @param[out] dest
 * 	The output surface.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_loft_surface_copy(const tsLoftSurface *src,
                     tsLoftSurface *dest,
                     tsStatus *status);

/**
 * Releases the data of \p surface. After calling this function, the data of
 * \p surface points to NULL.
 *
 * @param[out] surface
 * 	The surface to be released.
 */
void TINYSPLINE_API
ts_loft_surface_free(tsLoftSurface *surface);

/**
 * Evaluates the rational surface at (\p u, \p v) and stores the Euclidean
 * point and the analytic first order partial derivatives in \p point, \p du,
 * and \p dv, respectively. Each output array must have room for three
 * components. At interior knots, the right-sided derivatives are returned; at
 * u = 1 (or v = 1), the left-sided derivatives are returned. The derivatives
 * are obtained from the homogeneous representation using the quotient rule,
 * that is, dividing the homogeneous derivative by its weight alone is not
 * sufficient.
 *
 * @param[in] surface
 * 	The surface to be evaluated.
 * @param[in] u
 * 	The u parameter. Must be within [0, 1].
 * @param[in] v
 * 	The v parameter. Must be within [0, 1].
 * @param[out] point
 * 	The Euclidean point at (\p u, \p v).
 * @param[out] du
 * 	The partial derivative with respect to u at (\p u, \p v).
 * @param[out] dv
 * 	The partial derivative with respect to v at (\p u, \p v).
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_U_UNDEFINED
 * 	If \p u or \p v is outside of [0, 1].
 * @return TS_NO_RESULT
 * 	If the denominator of the rational evaluation is not positive (this
 * 	must not happen for surfaces created successfully).
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
tsError TINYSPLINE_API
ts_loft_surface_eval(const tsLoftSurface *surface,
                     tsReal u,
                     tsReal v,
                     tsReal point[3],
                     tsReal du[3],
                     tsReal dv[3],
                     tsStatus *status);

/**
 * Returns the degree of the surface along the u axis.
 */
size_t TINYSPLINE_API
ts_loft_surface_u_degree(const tsLoftSurface *surface);

/**
 * Returns the degree of the surface along the v axis.
 */
size_t TINYSPLINE_API
ts_loft_surface_v_degree(const tsLoftSurface *surface);

/**
 * Returns the number of control points along the u axis.
 */
size_t TINYSPLINE_API
ts_loft_surface_num_u_control_points(const tsLoftSurface *surface);

/**
 * Returns the number of control points along the v axis.
 */
size_t TINYSPLINE_API
ts_loft_surface_num_v_control_points(const tsLoftSurface *surface);

/**
 * Returns the number of knots of the u knot vector.
 */
size_t TINYSPLINE_API
ts_loft_surface_num_u_knots(const tsLoftSurface *surface);

/**
 * Returns the number of knots of the v knot vector.
 */
size_t TINYSPLINE_API
ts_loft_surface_num_v_knots(const tsLoftSurface *surface);

/**
 * Returns a read-only pointer to the homogeneous control net. The array has
 * 4 * num_u * num_v entries and uses the layout described above.
 */
const tsReal TINYSPLINE_API *
ts_loft_surface_control_points_ptr(const tsLoftSurface *surface);

/**
 * Returns a read-only pointer to the (clamped, [0, 1]-normalized) u knot
 * vector.
 */
const tsReal TINYSPLINE_API *
ts_loft_surface_u_knots_ptr(const tsLoftSurface *surface);

/**
 * Returns a read-only pointer to the (clamped, [0, 1]-normalized) v knot
 * vector.
 */
const tsReal TINYSPLINE_API *
ts_loft_surface_v_knots_ptr(const tsLoftSurface *surface);
/*! @} */

#ifdef __cplusplus
}
#endif

#endif /* TINYSPLINE_SURFACE_H */
