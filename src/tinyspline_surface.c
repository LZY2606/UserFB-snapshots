#define TINYSPLINE_EXPORT
#include "tinyspline_surface.h"

#include <stdlib.h> /* malloc, free */
#include <string.h> /* memcpy */
#include <math.h>   /* isfinite, fabs */
#include <stdio.h>  /* sprintf (via tinyspline status macros) */

#ifdef TINYSPLINE_FLOAT_PRECISION
#define LOFT_KNOT_EPS ((tsReal) 1e-6)
#else
#define LOFT_KNOT_EPS ((tsReal) 1e-12)
#endif

/*! @name Internal Structs and Functions
 *
 * @{
 */
/**
 * Stores the private data of ::tsLoftSurface. A single allocation holds the
 * struct header, both knot vectors and the homogeneous control net.
 */
struct tsLoftSurfaceImpl
{
	size_t deg_u;       /**< Degree of the section (u) axis. */
	size_t deg_v;       /**< Degree of the loft (v) axis. */
	size_t n_u;         /**< Number of control points along u. */
	size_t n_v;         /**< Number of control points along v. */
	size_t n_knots_u;   /**< Number of knots of the u axis. */
	size_t n_knots_v;   /**< Number of knots of the v axis. */
	/* The allocation continues with:
	 *   tsReal knots_u[n_knots_u]
	 *   tsReal knots_v[n_knots_v]
	 *   tsReal net[4 * n_u * n_v]
	 */
};

static tsReal *
loft_access_knots_u(const tsLoftSurface *surface)
{
	return (tsReal *) (& surface->pImpl[1]);
}

static tsReal *
loft_access_knots_v(const tsLoftSurface *surface)
{
	return loft_access_knots_u(surface) + surface->pImpl->n_knots_u;
}

static tsReal *
loft_access_net(const tsLoftSurface *surface)
{
	return loft_access_knots_v(surface) + surface->pImpl->n_knots_v;
}

static size_t
loft_sof_state(const tsLoftSurface *surface)
{
	const struct tsLoftSurfaceImpl *impl = surface->pImpl;
	return sizeof(struct tsLoftSurfaceImpl) +
	       (impl->n_knots_u + impl->n_knots_v +
	        4 * impl->n_u * impl->n_v) * sizeof(tsReal);
}

/**
 * Cox-de Boor recursion for a single basis function N_{i, degree}(u). When
 * \p at_end is non-zero, the domain maximum is evaluated: by convention only
 * the very last degree zero basis function is one there. Zero-length knot
 * spans contribute nothing, which is the standard convention at repeated
 * knots.
 */
static tsReal
loft_basis(const tsReal *knots,
           size_t i,
           size_t degree,
           tsReal u,
           int at_end,
           size_t last_index)
{
	if (degree == 0) {
		if (u >= knots[i] && u < knots[i + 1])
			return (tsReal) 1.0;
		if (at_end && i == last_index)
			return (tsReal) 1.0;
		return (tsReal) 0.0;
	} else {
		tsReal left = (tsReal) 0.0;
		tsReal right = (tsReal) 0.0;
		const tsReal d_left = knots[i + degree] - knots[i];
		const tsReal d_right = knots[i + degree + 1] - knots[i + 1];
		if (d_left > (tsReal) 0.0)
			left = (u - knots[i]) / d_left *
			       loft_basis(knots, i, degree - 1, u, at_end,
			                  last_index);
		if (d_right > (tsReal) 0.0)
			right = (knots[i + degree + 1] - u) / d_right *
			        loft_basis(knots, i + 1, degree - 1, u, at_end,
			                   last_index);
		return left + right;
	}
}

/**
 * First derivative of N_{i, degree}(u), obtained from the degree - 1 basis
 * functions. Terms with zero knot span are omitted.
 */
static tsReal
loft_basis_derivative(const tsReal *knots,
                      size_t i,
                      size_t degree,
                      tsReal u,
                      int at_end,
                      size_t last_index)
{
	const tsReal d_left = knots[i + degree] - knots[i];
	const tsReal d_right = knots[i + degree + 1] - knots[i + 1];
	tsReal result = (tsReal) 0.0;
	if (d_left > (tsReal) 0.0)
		result += (tsReal) degree / d_left *
		          loft_basis(knots, i, degree - 1, u, at_end,
		                     last_index);
	if (d_right > (tsReal) 0.0)
		result -= (tsReal) degree / d_right *
		          loft_basis(knots, i + 1, degree - 1, u, at_end,
		                     last_index);
	return result;
}

/**
 * Finds the span index k such that u is in [knots[k], knots[k+1]). This is
 * the clamped knot vector span search from "The NURBS Book" (Piegl and
 * Tiller), which also terminates on zero length knot spans. The domain
 * maximum maps to the last non-trivial span n, where n is the index of the
 * last control point.
 */
static size_t
loft_find_span(const tsReal *knots,
               size_t degree,
               size_t num_knots,
               tsReal u)
{
	const size_t n = num_knots - degree - 2; /* last control point index */
	size_t low, high, mid;
	if (u >= knots[n + 1])
		return n;
	if (u <= knots[degree])
		return degree;
	low = degree;
	high = n + 1;
	mid = (low + high) / 2;
	while (u < knots[mid] || u >= knots[mid + 1]) {
		if (u < knots[mid])
			high = mid;
		else
			low = mid;
		mid = (low + high) / 2;
	}
	return mid;
}

/**
 * Validates one section curve and returns TS_SUCCESS if it can be lofted.
 */
static tsError
loft_validate_section(const tsBSpline *section,
                      tsStatus *status)
{
	const size_t dim = ts_bspline_dimension(section);
	const size_t deg = ts_bspline_degree(section);
	const size_t order = deg + 1;
	const size_t num_knots = ts_bspline_num_knots(section);
	const size_t num_ctrlp = ts_bspline_num_control_points(section);
	const tsReal *knots = ts_bspline_knots_ptr(section);
	const tsReal *ctrlp = ts_bspline_control_points_ptr(section);
	tsReal lo, hi;
	size_t i;

	if (dim != 3 && dim != 4)
		TS_RETURN_0(status, TS_DIM_ZERO,
		            "section dimension must be 3 or 4")
	if (deg < 1 || deg > 3)
		TS_RETURN_1(status, TS_NO_RESULT,
		            "unsupported section degree: %lu",
		            (unsigned long) deg)
	if (num_ctrlp < 2)
		TS_RETURN_0(status, TS_NO_RESULT,
		            "section needs at least two control points")

	lo = knots[deg];
	hi = knots[num_knots - order];
	if (!(hi > lo))
		TS_RETURN_0(status, TS_U_UNDEFINED,
		            "section has an empty parameter domain")

	for (i = 0; i < order; i++) {
		if (fabs(knots[i] - lo) > LOFT_KNOT_EPS)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "section is not clamped at the domain minimum")
		if (fabs(knots[num_knots - 1 - i] - hi) > LOFT_KNOT_EPS)
			TS_RETURN_0(status, TS_NO_RESULT,
			            "section is not clamped at the domain maximum")
	}

	for (i = 0; i < num_knots; i++) {
		if (!isfinite(knots[i]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "section contains a non-finite knot")
		if (i > 0 && knots[i] < knots[i - 1])
			TS_RETURN_0(status, TS_KNOTS_DECR,
			            "section has a decreasing knot vector")
	}

	/* Interior knots must not exceed the degree in multiplicity: a full
	 * multiplicity knot is a true break and cannot be rebased onto a
	 * common continuous basis. */
	i = order;
	while (i < num_knots - order) {
		size_t mult = 1;
		while (i + mult < num_knots - order &&
		       fabs(knots[i + mult] - knots[i]) <= LOFT_KNOT_EPS)
			mult++;
		if (mult > deg)
			TS_RETURN_1(status, TS_MULTIPLICITY,
			            "section has a break at knot: %f",
			            (double) knots[i])
		i += mult;
	}

	for (i = 0; i < num_ctrlp * dim; i++) {
		if (!isfinite(ctrlp[i]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "section contains non-finite control point data")
	}
	if (dim == 4) {
		for (i = 0; i < num_ctrlp; i++) {
			const tsReal w = ctrlp[i * 4 + 3];
			if (!isfinite(w) || w <= (tsReal) 0.0)
				TS_RETURN_0(status, TS_NO_RESULT,
				            "section has a non-positive weight")
		}
	}
	TS_RETURN_SUCCESS(status)
}

/**
 * Copies a section into a four dimensional, normalized worker. The parameter
 * domain is mapped to [0, 1] and the homogeneous components of a rational
 * section are scaled so that the weight of its first control point is one.
 */
static tsError
loft_normalize_section(const tsBSpline *section,
                       tsBSpline *worker,
                       tsStatus *status)
{
	const size_t dim = ts_bspline_dimension(section);
	const size_t deg = ts_bspline_degree(section);
	const size_t num_ctrlp = ts_bspline_num_control_points(section);
	const size_t num_knots = ts_bspline_num_knots(section);
	const tsReal *src_knots = ts_bspline_knots_ptr(section);
	const tsReal *src_ctrlp = ts_bspline_control_points_ptr(section);
	tsReal *dst_knots = NULL;
	tsReal *dst_ctrlp = NULL;
	tsReal lo, hi, span, scale;
	size_t i;
	tsError err;

	TS_TRY(try, err, status)
		lo = src_knots[deg];
		hi = src_knots[num_knots - (deg + 1)];
		span = hi - lo;

		TS_CALL(try, err, ts_bspline_new(
		        num_ctrlp, 4, deg, TS_OPENED, worker, status))
		dst_knots = (tsReal *) malloc(num_knots * sizeof(tsReal));
		dst_ctrlp = (tsReal *) malloc(num_ctrlp * 4 * sizeof(tsReal));
		if (!dst_knots || !dst_ctrlp)
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")

		for (i = 0; i < num_knots; i++)
			dst_knots[i] = (src_knots[i] - lo) / span;
		TS_CALL(try, err, ts_bspline_set_knots(
		        worker, dst_knots, status))

		scale = (tsReal) 1.0;
		if (dim == 4)
			scale = (tsReal) 1.0 / src_ctrlp[3];
		for (i = 0; i < num_ctrlp; i++) {
			if (dim == 4) {
				dst_ctrlp[i * 4 + 0] = scale * src_ctrlp[i * 4 + 0];
				dst_ctrlp[i * 4 + 1] = scale * src_ctrlp[i * 4 + 1];
				dst_ctrlp[i * 4 + 2] = scale * src_ctrlp[i * 4 + 2];
				dst_ctrlp[i * 4 + 3] = scale * src_ctrlp[i * 4 + 3];
			} else {
				dst_ctrlp[i * 4 + 0] = src_ctrlp[i * 3 + 0];
				dst_ctrlp[i * 4 + 1] = src_ctrlp[i * 3 + 1];
				dst_ctrlp[i * 4 + 2] = src_ctrlp[i * 3 + 2];
				dst_ctrlp[i * 4 + 3] = (tsReal) 1.0;
			}
		}
		TS_CALL(try, err, ts_bspline_set_control_points(
		        worker, dst_ctrlp, status))
	TS_CATCH(err)
		ts_bspline_free(worker);
	TS_FINALLY
		free(dst_knots);
		free(dst_ctrlp);
	TS_END_TRY_RETURN(err)
}

/**
 * Counts how often the knot at \p index is repeated in \p knots.
 */
static size_t
loft_knot_multiplicity(const tsReal *knots,
                       size_t num_knots,
                       size_t index)
{
	size_t mult = 1;
	while (index + mult < num_knots &&
	       fabs(knots[index + mult] - knots[index]) <= LOFT_KNOT_EPS)
		mult++;
	return mult;
}

/**
 * Rebases all normalized workers onto one common clamped u basis: the maximum
 * section degree and the union of all interior knots with their maximum
 * multiplicity. Only degree elevation and knot insertion are used, so the
 * geometry of every worker is preserved.
 */
static tsError
loft_unify_basis(tsBSpline *workers,
                 size_t count,
                 size_t *degree_out,
                 tsStatus *status)
{
	size_t max_deg = 0;
	size_t total_interior = 0;
	size_t cap, union_count = 0;
	tsReal *union_knots = NULL;
	size_t *union_mult = NULL;
	tsReal *canonical = NULL;
	size_t q, j, k;
	tsError err;

	TS_TRY(try, err, status)
		for (q = 0; q < count; q++) {
			const size_t deg = ts_bspline_degree(&workers[q]);
			if (deg > max_deg)
				max_deg = deg;
		}

		/* Elevate every worker to the common degree first. Degree
		 * elevation (via the piecewise Bezier decomposition) may
		 * increase the multiplicity of interior knots, so the knot
		 * union must be collected afterwards. */
		for (q = 0; q < count; q++) {
			const size_t deg = ts_bspline_degree(&workers[q]);
			if (deg < max_deg) {
				tsBSpline elevated = ts_bspline_init();
				TS_CALL(try, err, ts_bspline_elevate_degree(
				        &workers[q], max_deg - deg,
				        (tsReal) 0.0, &elevated, status))
				ts_bspline_free(&workers[q]);
				workers[q] = elevated;
			}
		}

		for (q = 0; q < count; q++) {
			total_interior += ts_bspline_num_knots(&workers[q]) -
			                  2 * (max_deg + 1);
		}
		cap = total_interior;
		if (cap == 0)
			cap = 1;
		union_knots = (tsReal *) malloc(cap * sizeof(tsReal));
		union_mult = (size_t *) malloc(cap * sizeof(size_t));
		if (!union_knots || !union_mult)
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")

		/* Collect the union of the interior knots. */
		for (q = 0; q < count; q++) {
			const size_t num_knots = ts_bspline_num_knots(&workers[q]);
			const tsReal *knots = ts_bspline_knots_ptr(&workers[q]);
			size_t i = max_deg + 1;
			while (i < num_knots - (max_deg + 1)) {
				const tsReal knot = knots[i];
				const size_t mult = loft_knot_multiplicity(
				        knots, num_knots, i);
				size_t found = union_count;
				for (k = 0; k < union_count; k++) {
					if (fabs(union_knots[k] - knot) <= LOFT_KNOT_EPS) {
						found = k;
						break;
					}
				}
				if (k == union_count) {
					union_knots[union_count] = knot;
					union_mult[union_count] = mult;
					union_count++;
				} else if (mult > union_mult[found]) {
					union_mult[found] = mult;
				}
				i += mult;
			}
		}

		/* Order the union knots (simple insertion sort; the vectors
		 * involved are small). */
		for (j = 1; j < union_count; j++) {
			const tsReal kv = union_knots[j];
			const size_t mv = union_mult[j];
			size_t m = j;
			while (m > 0 && union_knots[m - 1] > kv) {
				union_knots[m] = union_knots[m - 1];
				union_mult[m] = union_mult[m - 1];
				m--;
			}
			union_knots[m] = kv;
			union_mult[m] = mv;
		}

		/* Insert the union knots until the multiplicities match. When a
		 * worker already carries a near equal knot, its own knot value
		 * is used for the insertion so that no second, nearly coincident
		 * knot is created. */
		for (j = 0; j < union_count; j++) {
			for (q = 0; q < count; q++) {
				const tsReal *knots =
				        ts_bspline_knots_ptr(&workers[q]);
				const size_t num_knots =
				        ts_bspline_num_knots(&workers[q]);
				size_t current = 0;
				size_t inserted = 0;
				tsReal insert_value = union_knots[j];
				for (k = max_deg + 1;
				     k < num_knots - (max_deg + 1); k++) {
					if (fabs(knots[k] - union_knots[j]) <=
					    LOFT_KNOT_EPS) {
						current++;
						insert_value = knots[k];
					}
				}
				if (current < union_mult[j]) {
					TS_CALL(try, err, ts_bspline_insert_knot(
					        &workers[q], insert_value,
					        union_mult[j] - current,
					        &workers[q], &inserted, status))
				}
			}
		}

		/* All workers now have the same knot structure. Force one
		 * exact, canonical clamped knot vector so that the common
		 * basis is bitwise identical for every section. */
		{
			size_t n_knots = ts_bspline_num_knots(&workers[0]);
			size_t pos = 0;
			canonical = (tsReal *) malloc(n_knots * sizeof(tsReal));
			if (!canonical)
				TS_THROW_0(try, err, status, TS_MALLOC,
				           "out of memory")
			for (k = 0; k <= max_deg; k++)
				canonical[pos++] = (tsReal) 0.0;
			for (j = 0; j < union_count; j++) {
				for (k = 0; k < union_mult[j]; k++)
					canonical[pos++] = union_knots[j];
			}
			for (k = 0; k <= max_deg; k++)
				canonical[pos++] = (tsReal) 1.0;
			if (pos != n_knots)
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "could not unify the section bases")
			for (q = 0; q < count; q++) {
				if (ts_bspline_num_knots(&workers[q]) != n_knots)
					TS_THROW_0(try, err, status, TS_NO_RESULT,
					           "could not unify the section bases")
				TS_CALL(try, err, ts_bspline_set_knots(
				        &workers[q], canonical, status))
			}
		}

		/* All workers must now share the exact same basis. */
		for (q = 1; q < count; q++) {
			if (ts_bspline_num_control_points(&workers[q]) !=
			    ts_bspline_num_control_points(&workers[0]))
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "could not unify the section bases")
		}
		*degree_out = max_deg;
	TS_FINALLY
		free(union_knots);
		free(union_mult);
		free(canonical);
	TS_END_TRY_RETURN(err)
}
/*! @} */



/*! @name Public API
 *
 * @{
 */
tsLoftSurface
ts_loft_surface_init(void)
{
	tsLoftSurface surface;
	surface.pImpl = NULL;
	return surface;
}

tsError
ts_loft_surface_new(const tsBSpline *sections,
                    size_t count,
                    tsLoftSurface *out,
                    tsStatus *status)
{
	tsBSpline *workers = NULL;
	tsBSpline v_spline = ts_bspline_init();
	tsReal *section_points = NULL;
	tsReal *knots_u = NULL;
	tsReal *knots_v = NULL;
	tsReal *net = NULL;
	tsLoftSurface built;
	size_t common_deg = 0;
	size_t n_u, n_v, n_knots_u, n_knots_v;
	size_t q, i, c;
	tsError err;

	built.pImpl = NULL;

	if (sections == NULL || out == NULL)
		TS_RETURN_0(status, TS_NO_RESULT, "invalid NULL argument")
	if (count < 2)
		TS_RETURN_1(status, TS_NUM_POINTS,
		            "need at least two sections, got %lu",
		            (unsigned long) count)

	TS_TRY(try, err, status)
		for (q = 0; q < count; q++) {
			TS_CALL(try, err, loft_validate_section(
			        &sections[q], status))
		}

		workers = (tsBSpline *) calloc(count, sizeof(tsBSpline));
		if (!workers)
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")
		for (q = 0; q < count; q++)
			workers[q] = ts_bspline_init();

		for (q = 0; q < count; q++) {
			TS_CALL(try, err, loft_normalize_section(
			        &sections[q], &workers[q], status))
		}

		TS_CALL(try, err, loft_unify_basis(
		        workers, count, &common_deg, status))

		n_u = ts_bspline_num_control_points(&workers[0]);
		n_knots_u = ts_bspline_num_knots(&workers[0]);

		/* Layout of the interpolation data: point q contains the four
		 * homogeneous components of all u control positions of
		 * section q. */
		section_points = (tsReal *) malloc(
		        count * n_u * 4 * sizeof(tsReal));
		if (!section_points)
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")
		for (q = 0; q < count; q++) {
			const tsReal *ctrlp =
			        ts_bspline_control_points_ptr(&workers[q]);
			memcpy(section_points + q * n_u * 4,
			       ctrlp, n_u * 4 * sizeof(tsReal));
		}

		TS_CALL(try, err, ts_bspline_interpolate_cubic_natural(
		        section_points, count, n_u * 4,
		        &v_spline, status))

		/* The natural cubic construction stores each interior
		 * interpolation point twice, as the joining control points of
		 * two adjacent Bezier segments. In exact arithmetic these
		 * joins equal the input points; force them to do so here. This
		 * keeps the uniform parameter convention and the natural cubic
		 * tangents (all non-joining control points are untouched) while
		 * guaranteeing that the v axis passes through every section
		 * exactly, independently of the precision of the constants used
		 * inside the production interpolation routine. */
		for (q = 1; q + 1 < count; q++) {
			const size_t join_a = 4 * q - 1;
			const size_t join_b = 4 * q;
			TS_CALL(try, err, ts_bspline_set_control_point_at(
			        &v_spline, join_a,
			        section_points + q * (n_u * 4), status))
			TS_CALL(try, err, ts_bspline_set_control_point_at(
			        &v_spline, join_b,
			        section_points + q * (n_u * 4), status))
		}

		n_v = ts_bspline_num_control_points(&v_spline);
		n_knots_v = ts_bspline_num_knots(&v_spline);

		knots_u = (tsReal *) malloc(n_knots_u * sizeof(tsReal));
		knots_v = (tsReal *) malloc(n_knots_v * sizeof(tsReal));
		net = (tsReal *) malloc(4 * n_u * n_v * sizeof(tsReal));
		if (!knots_u || !knots_v || !net)
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")

		memcpy(knots_u, ts_bspline_knots_ptr(&workers[0]),
		       n_knots_u * sizeof(tsReal));

		/* Rebuild the v knot vector from the exact uniform station
		 * values j / (count - 1) in full precision. The production
		 * interpolation stores its piecewise Bezier knots through
		 * single precision domain constants, which would otherwise put
		 * a mathematically uniform station just left of its knot and
		 * spoil section reproduction. The uniform parameter convention
		 * of the natural cubic interpolation is unchanged. */
		{
			size_t pos = 0;
			for (i = 0; i <= 3; i++)
				knots_v[pos++] = (tsReal) 0.0;
			for (q = 1; q + 1 < count; q++) {
				const tsReal station =
				        (tsReal) q / (tsReal) (count - 1);
				for (i = 0; i < 4; i++)
					knots_v[pos++] = station;
			}
			for (i = 0; i <= 3; i++)
				knots_v[pos++] = (tsReal) 1.0;
			if (pos != n_knots_v)
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "inconsistent v knot vector")
		}

		/* Transpose the v interpolation result into the u-major
		 * tensor product net. */
		{
			const tsReal *v_ctrlp =
			        ts_bspline_control_points_ptr(&v_spline);
			for (i = 0; i < n_u; i++) {
				for (q = 0; q < n_v; q++) {
					for (c = 0; c < 4; c++) {
						net[(i * n_v + q) * 4 + c] =
						        v_ctrlp[q * (n_u * 4) +
						                i * 4 + c];
					}
				}
			}
		}

		/* The net must stay free of pole producing weights. */
		for (i = 0; i < n_u * n_v; i++) {
			const tsReal w = net[i * 4 + 3];
			if (!isfinite(w) || w <= (tsReal) 0.0)
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "lofted control net has a non-positive "
				           "weight")
			for (c = 0; c < 3; c++) {
				if (!isfinite(net[i * 4 + c]))
					TS_THROW_0(try, err, status, TS_NO_RESULT,
					           "lofted control net is not finite")
			}
		}

		built.pImpl = (struct tsLoftSurfaceImpl *) malloc(
		        sizeof(struct tsLoftSurfaceImpl) +
		        (n_knots_u + n_knots_v + 4 * n_u * n_v) *
		        sizeof(tsReal));
		if (!built.pImpl)
			TS_THROW_0(try, err, status, TS_MALLOC, "out of memory")
		built.pImpl->deg_u = common_deg;
		built.pImpl->deg_v = ts_bspline_degree(&v_spline);
		built.pImpl->n_u = n_u;
		built.pImpl->n_v = n_v;
		built.pImpl->n_knots_u = n_knots_u;
		built.pImpl->n_knots_v = n_knots_v;
		memcpy(loft_access_knots_u(&built), knots_u,
		       n_knots_u * sizeof(tsReal));
		memcpy(loft_access_knots_v(&built), knots_v,
		       n_knots_v * sizeof(tsReal));
		memcpy(loft_access_net(&built), net,
		       4 * n_u * n_v * sizeof(tsReal));
	TS_CATCH(err)
		ts_loft_surface_free(&built);
	TS_FINALLY
		if (workers) {
			for (q = 0; q < count; q++)
				ts_bspline_free(&workers[q]);
			free(workers);
		}
		ts_bspline_free(&v_spline);
		free(section_points);
		free(knots_u);
		free(knots_v);
		free(net);
	TS_END_TRY
	if (err)
		return err;

	/* Commit only after everything succeeded so that `out` remains
	 * untouched on failure. */
	ts_loft_surface_free(out);
	*out = built;
	TS_RETURN_SUCCESS(status)
}

tsError
ts_loft_surface_copy(const tsLoftSurface *src,
                     tsLoftSurface *dest,
                     tsStatus *status)
{
	size_t size;
	tsLoftSurface copy;
	if (src == NULL || dest == NULL || src->pImpl == NULL)
		TS_RETURN_0(status, TS_NO_RESULT, "invalid NULL argument")
	copy.pImpl = NULL;
	size = loft_sof_state(src);
	copy.pImpl = (struct tsLoftSurfaceImpl *) malloc(size);
	if (!copy.pImpl)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	memcpy(copy.pImpl, src->pImpl, size);
	ts_loft_surface_free(dest);
	*dest = copy;
	TS_RETURN_SUCCESS(status)
}

void
ts_loft_surface_free(tsLoftSurface *surface)
{
	if (surface && surface->pImpl)
		free(surface->pImpl);
	if (surface)
		surface->pImpl = NULL;
}

tsError
ts_loft_surface_eval(const tsLoftSurface *surface,
                     tsReal u,
                     tsReal v,
                     tsReal point[3],
                     tsReal du[3],
                     tsReal dv[3],
                     tsStatus *status)
{
	const struct tsLoftSurfaceImpl *impl;
	const tsReal *ku, *kv, *net;
	size_t span_u, span_v;
	size_t i0, q0, i, q, c;
	int at_end_u, at_end_v;
	tsReal H[4], Hu[4], Hv[4];
	tsReal w;
	tsReal local_point[3], local_du[3], local_dv[3];

	if (surface == NULL || surface->pImpl == NULL)
		TS_RETURN_0(status, TS_NO_RESULT, "invalid NULL argument")
	if (!isfinite(u) || u < (tsReal) 0.0 || u > (tsReal) 1.0)
		TS_RETURN_1(status, TS_U_UNDEFINED,
		            "u (%f) outside domain [0, 1]", (double) u)
	if (!isfinite(v) || v < (tsReal) 0.0 || v > (tsReal) 1.0)
		TS_RETURN_1(status, TS_U_UNDEFINED,
		            "v (%f) outside domain [0, 1]", (double) v)

	impl = surface->pImpl;
	ku = loft_access_knots_u(surface);
	kv = loft_access_knots_v(surface);
	net = loft_access_net(surface);

	at_end_u = (u >= (tsReal) 1.0);
	at_end_v = (v >= (tsReal) 1.0);
	span_u = loft_find_span(ku, impl->deg_u, impl->n_knots_u, u);
	span_v = loft_find_span(kv, impl->deg_v, impl->n_knots_v, v);
	i0 = span_u - impl->deg_u;
	q0 = span_v - impl->deg_v;

	for (c = 0; c < 4; c++) {
		H[c] = (tsReal) 0.0;
		Hu[c] = (tsReal) 0.0;
		Hv[c] = (tsReal) 0.0;
	}

	for (i = 0; i <= impl->deg_u; i++) {
		const size_t ui = i0 + i;
		const size_t last_u = impl->n_knots_u - impl->deg_u - 2;
		const tsReal nu = loft_basis(ku, ui, impl->deg_u, u, at_end_u,
		                             last_u);
		const tsReal dnu = loft_basis_derivative(
		        ku, ui, impl->deg_u, u, at_end_u, last_u);
		tsReal B[4], Bv[4];
		for (c = 0; c < 4; c++) {
			B[c] = (tsReal) 0.0;
			Bv[c] = (tsReal) 0.0;
		}
		for (q = 0; q <= impl->deg_v; q++) {
			const size_t vq = q0 + q;
			const size_t last_v = impl->n_knots_v -
			                     impl->deg_v - 2;
			const tsReal mv = loft_basis(
			        kv, vq, impl->deg_v, v, at_end_v,
			        last_v);
			const tsReal dmv = loft_basis_derivative(
			        kv, vq, impl->deg_v, v, at_end_v,
			        last_v);
			const tsReal *p = net + (ui * impl->n_v + vq) * 4;
			for (c = 0; c < 4; c++) {
				B[c] += mv * p[c];
				Bv[c] += dmv * p[c];
			}
		}
		for (c = 0; c < 4; c++) {
			H[c] += nu * B[c];
			Hu[c] += dnu * B[c];
			Hv[c] += nu * Bv[c];
		}
	}

	w = H[3];
	if (!isfinite(w) || w <= (tsReal) 0.0)
		TS_RETURN_0(status, TS_NO_RESULT,
		            "non-positive homogeneous weight while evaluating")

	/* Quotient rule for rational surfaces:
	 * d(x/w) = (dx * w - x * dw) / w^2 */
	for (c = 0; c < 3; c++) {
		local_point[c] = H[c] / w;
		local_du[c] = (Hu[c] * w - H[c] * Hu[3]) / (w * w);
		local_dv[c] = (Hv[c] * w - H[c] * Hv[3]) / (w * w);
		if (!isfinite(local_point[c]) ||
		    !isfinite(local_du[c]) ||
		    !isfinite(local_dv[c]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "evaluation produced non-finite data")
	}

	memcpy(point, local_point, 3 * sizeof(tsReal));
	memcpy(du, local_du, 3 * sizeof(tsReal));
	memcpy(dv, local_dv, 3 * sizeof(tsReal));
	TS_RETURN_SUCCESS(status)
}

size_t
ts_loft_surface_degree_u(const tsLoftSurface *surface)
{
	return surface->pImpl->deg_u;
}

size_t
ts_loft_surface_degree_v(const tsLoftSurface *surface)
{
	return surface->pImpl->deg_v;
}

size_t
ts_loft_surface_num_control_points_u(const tsLoftSurface *surface)
{
	return surface->pImpl->n_u;
}

size_t
ts_loft_surface_num_control_points_v(const tsLoftSurface *surface)
{
	return surface->pImpl->n_v;
}

size_t
ts_loft_surface_num_knots_u(const tsLoftSurface *surface)
{
	return surface->pImpl->n_knots_u;
}

size_t
ts_loft_surface_num_knots_v(const tsLoftSurface *surface)
{
	return surface->pImpl->n_knots_v;
}

const tsReal *
ts_loft_surface_control_points_ptr(const tsLoftSurface *surface)
{
	return loft_access_net(surface);
}

const tsReal *
ts_loft_surface_knots_u_ptr(const tsLoftSurface *surface)
{
	return loft_access_knots_u(surface);
}

const tsReal *
ts_loft_surface_knots_v_ptr(const tsLoftSurface *surface)
{
	return loft_access_knots_v(surface);
}
/*! @} */
