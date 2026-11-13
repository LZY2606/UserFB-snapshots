/**
 * @file tinyspline_surface.h
 *
 * @brief Tensor product loft surfaces built from TinySpline section curves.
 *
 * A ::tsLoftSurface is an owning, rational tensor product B-Spline surface.
 * It is obtained by lofting a sequence of clamped section curves (see
 * ::ts_loft_surface_new). The surface stores its own homogeneous control net
 * as well as the knot vectors and degrees of both parameter axes. The input
 * sections are neither retained nor referenced: releasing or editing them
 * after the surface has been created does not affect the surface.
 *
 * @section layout Control Net, Knot and Parameter Layout
 *
 * Let
 *
 *     n_u = ts_loft_surface_num_control_points_u()
 *     n_v = ts_loft_surface_num_control_points_v()
 *
 * be the number of control points along the section (u) and loft (v) axis.
 * Every control point is stored as a homogeneous coordinate
 * (wx, wy, wz, w) with strictly positive weight w. The control net is a
 * row-major, u-major array of <tt>4 * n_u * n_v</tt> ::tsReal values returned
 * by ::ts_loft_surface_control_points_ptr. Control point (i, q), where
 * <tt>0 <= i < n_u</tt> indexes the u axis and <tt>0 <= q < n_v</tt> indexes
 * the v axis, starts at offset
 *
 *     (i * n_v + q) * 4
 *
 * and consists of the four consecutive components (wx, wy, wz, w).
 *
 * The u axis has degree
 * <tt>p_u = ts_loft_surface_degree_u()</tt> (1, 2 or 3) and a clamped knot
 * vector of length <tt>n_u + p_u + 1</tt> on the domain [0, 1], accessible via
 * ::ts_loft_surface_knots_u_ptr. The v axis always has degree 3 and a clamped,
 * piecewise Bezier knot vector of length <tt>n_v + 4</tt> on the domain
 * [0, 1], accessible via ::ts_loft_surface_knots_v_ptr. The v axis passes
 * through section j (0 based) at <tt>v = j / (count - 1)</tt>.
 *
 * Both parameter domains are [0, 1]. Evaluation outside [0, 1] fails with
 * ::TS_U_UNDEFINED. At interior knots the right hand derivative is returned;
 * at u = 1 (or v = 1) the left hand derivative is returned.
 */
#ifndef TINYSPLINE_SURFACE_H
#define TINYSPLINE_SURFACE_H

#include "tinyspline.h"

#if !defined(TINYSPLINE_SURFACE_API)
#define TINYSPLINE_SURFACE_API TINYSPLINE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An owning rational tensor product loft surface. The actual data is hidden
 * behind a PIMPL pointer. A null instance is obtained with
 * ::ts_loft_surface_init and released with ::ts_loft_surface_free.
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
TINYSPLINE_SURFACE_API tsLoftSurface
ts_loft_surface_init(void);

/**
 * Lofts the given section curves into a rational tensor product surface.
 *
 * The sections are clamped B-Spline curves of degree 1, 2 or 3 with at least
 * two control points. Three dimensional control points describe non-rational
 * curves, four dimensional control points describe rational curves whose
 * last component is the homogeneous weight w. Every weight must be a positive,
 * finite value. The caller is responsible for the orientation and the
 * correspondence of the start points of the sections.
 *
 * For every section the effective parameter domain is first normalized to
 * [0, 1] and, for rational sections, all homogeneous control points are scaled
 * by the inverse weight of the section's first control point. All sections are
 * then rebased onto one common u basis (common degree and one common clamped
 * knot vector) using degree elevation and knot insertion only, so the geometry
 * of every section is preserved. For each coincident homogeneous control
 * position, the four components are interpolated along the v axis with a
 * natural cubic spline using the uniform parameter convention of
 * ::ts_bspline_interpolate_cubic_natural. The v axis passes through section j
 * at <tt>v = j / (count - 1)</tt>. For <tt>count == 2</tt> the same convention
 * yields the ruled (linear) interpolation between the two sections.
 *
 * The resulting surface owns its tensor product control net, the knot vectors
 * and degrees of both axes. It is independent of \p sections, which may be
 * released or edited once this function returns.
 *
 * @param[in] sections
 * 	The section curves. Must not be NULL and must contain at least two
 * 	curves.
 * @param[in] count
 * 	The number of section curves. Must be at least two.
 * @param[out] out
 * 	The surface that receives the result. On failure, the data of \p out
 * 	is left unchanged. On success, any previously owned data is released.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_NUM_POINTS
 * 	If \p count is less than two.
 * @return TS_DIM_ZERO
 * 	If a section has a dimensionality other than 3 or 4.
 * @return TS_MULTIPLICITY
 * 	If a section contains a true break, i.e. an interior knot with
 * 	multiplicity greater than its degree.
 * @return TS_U_UNDEFINED
 * 	If a section has an empty parameter domain.
 * @return TS_NO_RESULT
 * 	If a section is not clamped, contains non-finite data, a rational
 * 	section has a non-positive or non-finite weight, or the resulting
 * 	control net contains a non-positive homogeneous weight.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
TINYSPLINE_SURFACE_API tsError
ts_loft_surface_new(const tsBSpline *sections,
                    size_t count,
                    tsLoftSurface *out,
                    tsStatus *status);

/**
 * Creates a deep copy of \p src and stores the result in \p dest. The copy
 * owns its own control net and knot vectors, so \p src may be released after
 * the copy without affecting \p dest.
 *
 * @param[in] src
 * 	The surface to be copied. Must not be NULL.
 * @param[out] dest
 * 	The output surface. On failure, the data of \p dest is left unchanged.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_MALLOC
 * 	If allocating memory failed.
 */
TINYSPLINE_SURFACE_API tsError
ts_loft_surface_copy(const tsLoftSurface *src,
                     tsLoftSurface *dest,
                     tsStatus *status);

/**
 * Releases the data of \p surface. After calling this function, the data of
 * \p surface points to NULL.
 *
 * @param[out] surface
 * 	The surface to be released. Must not be NULL.
 */
TINYSPLINE_SURFACE_API void
ts_loft_surface_free(tsLoftSurface *surface);

/**
 * Evaluates the surface at (\p u, \p v) and returns the Euclidean point as
 * well as the analytic first order partial derivatives. Rational derivatives
 * are computed with the quotient rule on the homogeneous tensor product
 * representation; they are not homogeneous derivatives divided by the weight.
 * A surface normal can be obtained from the cross product
 * <tt>du x dv</tt>.
 *
 * At interior knots the right hand derivative is returned. At \p u == 1 (or
 * \p v == 1) the left hand derivative is returned. On failure, \p point,
 * \p du and \p dv are left unchanged.
 *
 * @param[in] surface
 * 	The surface to evaluate. Must not be NULL.
 * @param[in] u
 * 	The section axis parameter, in [0, 1].
 * @param[in] v
 * 	The loft axis parameter, in [0, 1].
 * @param[out] point
 * 	The Euclidean point, an array of at least three ::tsReal values.
 * @param[out] du
 * 	The partial derivative with respect to u, an array of at least three
 * 	::tsReal values.
 * @param[out] dv
 * 	The partial derivative with respect to v, an array of at least three
 * 	::tsReal values.
 * @param[out] status
 * 	The status of this function. May be NULL.
 * @return TS_SUCCESS
 * 	On success.
 * @return TS_U_UNDEFINED
 * 	If \p u or \p v is outside [0, 1].
 * @return TS_NO_RESULT
 * 	If the homogeneous weight at (\p u, \p v) is not positive.
 */
TINYSPLINE_SURFACE_API tsError
ts_loft_surface_eval(const tsLoftSurface *surface,
                     tsReal u,
                     tsReal v,
                     tsReal point[3],
                     tsReal du[3],
                     tsReal dv[3],
                     tsStatus *status);

/**
 * Returns the degree of the section (u) axis of \p surface.
 *
 * @param[in] surface
 * 	The surface whose degree is read.
 * @return
 * 	The degree of the u axis.
 */
TINYSPLINE_SURFACE_API size_t
ts_loft_surface_degree_u(const tsLoftSurface *surface);

/**
 * Returns the degree of the loft (v) axis of \p surface. This is always 3.
 *
 * @param[in] surface
 * 	The surface whose degree is read.
 * @return
 * 	The degree of the v axis.
 */
TINYSPLINE_SURFACE_API size_t
ts_loft_surface_degree_v(const tsLoftSurface *surface);

/**
 * Returns the number of control points along the section (u) axis.
 *
 * @param[in] surface
 * 	The surface whose control point count is read.
 * @return
 * 	The number of control points along the u axis.
 */
TINYSPLINE_SURFACE_API size_t
ts_loft_surface_num_control_points_u(const tsLoftSurface *surface);

/**
 * Returns the number of control points along the loft (v) axis.
 *
 * @param[in] surface
 * 	The surface whose control point count is read.
 * @return
 * 	The number of control points along the v axis.
 */
TINYSPLINE_SURFACE_API size_t
ts_loft_surface_num_control_points_v(const tsLoftSurface *surface);

/**
 * Returns the number of knots of the section (u) axis.
 *
 * @param[in] surface
 * 	The surface whose knot count is read.
 * @return
 * 	The number of knots of the u axis.
 */
TINYSPLINE_SURFACE_API size_t
ts_loft_surface_num_knots_u(const tsLoftSurface *surface);

/**
 * Returns the number of knots of the loft (v) axis.
 *
 * @param[in] surface
 * 	The surface whose knot count is read.
 * @return
 * 	The number of knots of the v axis.
 */
TINYSPLINE_SURFACE_API size_t
ts_loft_surface_num_knots_v(const tsLoftSurface *surface);

/**
 * Returns a read-only pointer to the homogeneous control net of \p surface.
 * The layout is documented in the file header of tinyspline_surface.h.
 *
 * @param[in] surface
 * 	The surface whose control net is read.
 * @return
 * 	Pointer to the control net.
 */
TINYSPLINE_SURFACE_API const tsReal *
ts_loft_surface_control_points_ptr(const tsLoftSurface *surface);

/**
 * Returns a read-only pointer to the clamped knot vector of the section (u)
 * axis. The knot vector is defined on the domain [0, 1].
 *
 * @param[in] surface
 * 	The surface whose u knot vector is read.
 * @return
 * 	Pointer to the knot vector of the u axis.
 */
TINYSPLINE_SURFACE_API const tsReal *
ts_loft_surface_knots_u_ptr(const tsLoftSurface *surface);

/**
 * Returns a read-only pointer to the clamped knot vector of the loft (v)
 * axis. The knot vector is defined on the domain [0, 1].
 *
 * @param[in] surface
 * 	The surface whose v knot vector is read.
 * @return
 * 	Pointer to the knot vector of the v axis.
 */
TINYSPLINE_SURFACE_API const tsReal *
ts_loft_surface_knots_v_ptr(const tsLoftSurface *surface);

#ifdef __cplusplus
}
#endif

#endif /* TINYSPLINE_SURFACE_H */
