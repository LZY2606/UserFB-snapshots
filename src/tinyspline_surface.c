#define TINYSPLINE_EXPORT
#include "tinyspline_surface.h"

#include <stdlib.h> /* malloc, free */
#include <string.h> /* memcpy */
#include <math.h>   /* isfinite */
#include <stdio.h>  /* sprintf */

/*! @name Internal Structs and Functions
 *
 * @{
 */

/** Epsilon used for knot bookkeeping (merging knot vectors etc.). */
#define TS_LOFT_KNOT_EPS ((tsReal) 1e-10)

/**
 * Stores the private data of ::tsLoftSurface.
 */
struct tsLoftSurfaceImpl
{
	size_t deg_u;     /**< Degree along the u axis. */
	size_t deg_v;     /**< Degree along the v axis. */
	size_t n_u;       /**< Number of control points along u. */
	size_t n_v;       /**< Number of control points along v. */
	size_t n_knots_u; /**< Number of u knots (n_u + deg_u + 1). */
	size_t n_knots_v; /**< Number of v knots (n_v + deg_v + 1). */
	tsReal *net;      /**< Homogeneous control net, 4 * n_u * n_v entries. */
	tsReal *knots_u;  /**< Clamped u knot vector. */
	tsReal *knots_v;  /**< Clamped v knot vector. */
};

static void
ts_int_loft_init(tsLoftSurface *surface)
{
	surface->pImpl = NULL;
}

static int
ts_int_is_finite_vec(const tsReal *values, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++)
		if (!isfinite(values[i]))
			return 0;
	return 1;
}

/* Knot equality used for bookkeeping (multiplicity and merging). */
static int
ts_int_loft_knot_eq(tsReal a, tsReal b)
{
	return fabs(a - b) <= TS_LOFT_KNOT_EPS;
}

/**
 * Validates a section curve and returns TS_SUCCESS if it can be lofted.
 */
static tsError
ts_int_loft_validate_section(const tsBSpline *section,
                             tsStatus *status)
{
	const size_t dim = ts_bspline_dimension(section);
	const size_t deg = ts_bspline_degree(section);
	const size_t n_ctrlp = ts_bspline_num_control_points(section);
	const size_t n_knots = ts_bspline_num_knots(section);
	const tsReal *ctrlp = ts_bspline_control_points_ptr(section);
	const tsReal *knots = ts_bspline_knots_ptr(section);
	const tsReal min = knots[deg];
	const tsReal max = knots[n_knots - (deg + 1)];
	size_t i, mult;

	if (dim != 3 && dim != 4)
		TS_RETURN_1(status, TS_DIM_ZERO,
		            "unsupported section dimension: %lu",
		            (unsigned long) dim)
	if (deg < 1 || deg > 3)
		TS_RETURN_1(status, TS_DEG_GE_NCTRLP,
		            "unsupported section degree: %lu (must be 1..3)",
		            (unsigned long) deg)
	if (n_ctrlp < 2)
		TS_RETURN_0(status, TS_DEG_GE_NCTRLP,
		            "section needs at least two control points")
	if (!isfinite(min) || !isfinite(max) || !(max > min))
		TS_RETURN_0(status, TS_NO_RESULT,
		            "section has an empty or non-finite parameter domain")
	if (!ts_int_is_finite_vec(knots, n_knots))
		TS_RETURN_0(status, TS_NO_RESULT,
		            "section contains non-finite knots")
	if (!ts_int_is_finite_vec(ctrlp, n_ctrlp * dim))
		TS_RETURN_0(status, TS_NO_RESULT,
		            "section contains non-finite control points")

	/* Must be clamped: the first and last order knots coincide. */
	for (i = 0; i <= deg; i++) {
		if (!ts_int_loft_knot_eq(knots[i], min))
			TS_RETURN_0(status, TS_MULTIPLICITY,
			            "section is not clamped at its first knot")
		if (!ts_int_loft_knot_eq(knots[n_knots - 1 - i], max))
			TS_RETURN_0(status, TS_MULTIPLICITY,
			            "section is not clamped at its last knot")
	}
	/* No true breaks: interior knot multiplicity must not exceed deg. */
	for (i = deg + 1; i + deg + 1 < n_knots;) {
		mult = 1;
		while (i + mult < n_knots &&
		       ts_int_loft_knot_eq(knots[i + mult], knots[i]))
			mult++;
		if (knots[i] > min && knots[i] < max && mult > deg)
			TS_RETURN_1(status, TS_MULTIPLICITY,
			            "section has a discontinuity at knot: %f",
			            (double) knots[i])
		i += mult;
	}

	if (dim == 4) {
		for (i = 0; i < n_ctrlp; i++) {
			const tsReal w = ctrlp[i * 4 + 3];
			if (!isfinite(w) || !(w > 0.0))
				TS_RETURN_0(status, TS_NO_RESULT,
				            "section has a non-positive weight")
		}
	}
	TS_RETURN_SUCCESS(status)
}

/**
 * Creates an independent, [0, 1]-normalized, homogeneous copy of a section.
 * Non-rational sections are lifted with weight 1. Rational sections are
 * scaled by the inverse weight of their first control point.
 */
static tsError
ts_int_loft_normalize_section(const tsBSpline *section,
                              tsBSpline *normalized,
                              tsStatus *status)
{
	const size_t dim = ts_bspline_dimension(section);
	const size_t deg = ts_bspline_degree(section);
	const size_t n_ctrlp = ts_bspline_num_control_points(section);
	const size_t n_knots = ts_bspline_num_knots(section);
	const tsReal *src_ctrlp = ts_bspline_control_points_ptr(section);
	const tsReal *src_knots = ts_bspline_knots_ptr(section);
	const tsReal min = src_knots[deg];
	const tsReal max = src_knots[n_knots - (deg + 1)];
	const tsReal span = max - min;
	tsReal *ctrlp, *knots, scale;
	size_t i, c;
	tsError err;

	TS_CALL_ROE(err, ts_bspline_new(n_ctrlp, 4, deg, TS_OPENED,
	                                normalized, status))
	ctrlp = (tsReal *) malloc(n_ctrlp * 4 * sizeof(tsReal));
	knots = (tsReal *) malloc(n_knots * sizeof(tsReal));
	if (!ctrlp || !knots) {
		free(ctrlp);
		free(knots);
		ts_bspline_free(normalized);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}

	scale = 1.0;
	if (dim == 4)
		scale = 1.0 / src_ctrlp[3];

	for (i = 0; i < n_ctrlp; i++) {
		if (dim == 4) {
			for (c = 0; c < 4; c++)
				ctrlp[i * 4 + c] =
					scale * src_ctrlp[i * 4 + c];
		} else {
			for (c = 0; c < 3; c++)
				ctrlp[i * 4 + c] = src_ctrlp[i * 3 + c];
			ctrlp[i * 4 + 3] = 1.0;
		}
	}
	for (i = 0; i < n_knots; i++)
		knots[i] = (src_knots[i] - min) / span;

	TS_TRY(try, err, status)
		TS_CALL(try, err, ts_bspline_set_control_points(
		        normalized, ctrlp, status))
		TS_CALL(try, err, ts_bspline_set_knots(
		        normalized, knots, status))
	TS_FINALLY
		free(ctrlp);
		free(knots);
	TS_END_TRY_RETURN(err)
}

static int
ts_int_loft_compare_real(const void *a, const void *b)
{
	const tsReal x = *(const tsReal *) a;
	const tsReal y = *(const tsReal *) b;
	if (x < y)
		return -1;
	if (x > y)
		return 1;
	return 0;
}

/**
 * Merges the knot vectors of all (already degree aligned) sections into one
 * sorted clamped common knot vector, preserving the maximum multiplicity of
 * every knot. Knots closer than TS_LOFT_KNOT_EPS are treated as equal. The
 * resulting vector is written to \p common and its length to \p n_common.
 */
static tsError
ts_int_loft_merge_knots(const tsBSpline *sections,
                        size_t count,
                        tsReal **common,
                        size_t *n_common,
                        tsStatus *status)
{
	size_t cap = 0, len = 0, j, k;
	tsReal *vec;

	for (j = 0; j < count; j++)
		cap += ts_bspline_num_knots(&sections[j]);
	vec = (tsReal *) malloc(cap * sizeof(tsReal));
	if (!vec)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")

	for (j = 0; j < count; j++) {
		const tsReal *knots = ts_bspline_knots_ptr(&sections[j]);
		const size_t n_knots = ts_bspline_num_knots(&sections[j]);
		memcpy(vec + len, knots, n_knots * sizeof(tsReal));
		len += n_knots;
	}
	qsort(vec, len, sizeof(tsReal), ts_int_loft_compare_real);

	/* Build the union: every distinct knot keeps the maximum
	 * multiplicity it has in any single section. */
	{
		tsReal *out = (tsReal *) malloc(cap * sizeof(tsReal));
		size_t out_len = 0;
		if (!out) {
			free(vec);
			TS_RETURN_0(status, TS_MALLOC, "out of memory")
		}
		j = 0;
		while (j < len) {
			const tsReal value = vec[j];
			size_t max_mult = 0, mult, s2;
			for (s2 = 0; s2 < count; s2++) {
				const tsReal *knots =
					ts_bspline_knots_ptr(&sections[s2]);
				const size_t n_knots =
					ts_bspline_num_knots(&sections[s2]);
				mult = 0;
				for (k = 0; k < n_knots; k++) {
					if (fabs(knots[k] - value) <=
					    TS_LOFT_KNOT_EPS)
						mult++;
				}
				if (mult > max_mult)
					max_mult = mult;
			}
			for (mult = 0; mult < max_mult; mult++)
				out[out_len++] = value;
			while (j < len &&
			       fabs(vec[j] - value) <= TS_LOFT_KNOT_EPS)
				j++;
		}
		free(vec);
		*common = out;
		*n_common = out_len;
	}
	TS_RETURN_SUCCESS(status)
}

/**
 * Inserts knots into \p worker until its knot vector equals \p common, then
 * snaps every knot value to the corresponding entry of \p common so that all
 * sections share exactly the same knot vector.
 */
static tsError
ts_int_loft_refine(tsBSpline *worker,
                   const tsReal *common,
                   size_t n_common,
                   tsStatus *status)
{
	const tsReal *knots;
	size_t c, k, have, want, idx = 0;
	tsBSpline tmp = ts_bspline_init();
	tsError err;

	for (c = 0; c < n_common; c++) {
		want = 1;
		while (c + want < n_common &&
		       ts_int_loft_knot_eq(common[c + want], common[c]))
			want++;
		knots = ts_bspline_knots_ptr(worker);
		have = 0;
		for (k = 0; k < ts_bspline_num_knots(worker); k++) {
			if (fabs(knots[k] - common[c]) <= TS_LOFT_KNOT_EPS)
				have++;
		}
		for (k = have; k < want; k++) {
			err = ts_bspline_insert_knot(
				worker, common[c], 1, &tmp, &idx, status);
			if (err) {
				ts_bspline_free(&tmp);
				return err;
			}
			ts_bspline_free(worker);
			ts_bspline_move(&tmp, worker);
		}
		c += want - 1;
	}

	if (ts_bspline_num_knots(worker) != n_common)
		TS_RETURN_0(status, TS_NO_RESULT,
		            "section basis unification failed")
	TS_CALL_ROE(err, ts_bspline_set_knots(worker, common, status))
	TS_RETURN_SUCCESS(status)
}

/**
 * Finds the knot span containing \p u: the index k with
 * knots[k] <= u < knots[k + 1]. At u = 1 (the final knot) the last span is
 * returned so that the left limit is evaluated.
 */
static size_t
ts_int_loft_find_span(const tsReal *knots, size_t n, size_t deg, tsReal u)
{
	size_t low, high, mid;
	if (u >= knots[n])
		return n - 1;
	low = deg;
	high = n;
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
 * Evaluates the non-zero basis functions of degree \p deg at \p u. The
 * \p window array must have room for deg + 1 entries; window[r] holds the
 * basis function with global index (span - deg + r). Repeated knots are
 * handled by treating zero knot differences as zero coefficients.
 */
static void
ts_int_loft_basis_funs(const tsReal *knots, size_t span, size_t deg,
                       tsReal u, tsReal *window)
{
	size_t j, r;

	window[0] = 1.0;
	for (j = 1; j <= deg; j++) {
		tsReal saved = 0.0;
		for (r = 0; r < j; r++) {
			const tsReal den =
				knots[span + 1 + r] -
				knots[span + 1 - j + r];
			tsReal temp = 0.0;
			if (den > 0.0)
				temp = window[r] / den;
			window[r] = saved +
			            (knots[span + 1 + r] - u) * temp;
			saved = (u - knots[span + 1 - j + r]) * temp;
		}
		window[j] = saved;
	}
}

static tsError
ts_int_loft_basis(const tsReal *knots,
                  size_t n,
                  size_t deg,
                  tsReal u,
                  tsReal *values,
                  tsReal *derivs,
                  tsStatus *status)
{
	const size_t span = ts_int_loft_find_span(knots, n, deg, u);
	tsReal *window = NULL;
	tsReal *lower = NULL;
	size_t i;

	for (i = 0; i < n; i++) {
		values[i] = 0.0;
		derivs[i] = 0.0;
	}
	window = (tsReal *) malloc((deg + 1) * sizeof(tsReal));
	if (!window)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	ts_int_loft_basis_funs(knots, span, deg, u, window);
	for (i = 0; i <= deg; i++)
		values[span - deg + i] = window[i];

	if (deg >= 1) {
		/* First derivatives need the degree (deg - 1) basis. */
		lower = (tsReal *) malloc(deg * sizeof(tsReal));
		if (!lower) {
			free(window);
			TS_RETURN_0(status, TS_MALLOC, "out of memory")
		}
		ts_int_loft_basis_funs(knots, span, deg - 1, u, lower);
		for (i = 0; i < n; i++) {
			const tsReal den_a = knots[i + deg] - knots[i];
			const tsReal den_b =
				knots[i + deg + 1] - knots[i + 1];
			tsReal na = 0.0, nb = 0.0;
			/* lower[r] has global index span-deg+1+r. */
			if (i >= span - deg + 1 && i <= span)
				na = lower[i - (span - deg + 1)];
			if (i + 1 >= span - deg + 1 && i + 1 <= span)
				nb = lower[(i + 1) - (span - deg + 1)];
			if (den_a > 0.0)
				derivs[i] += (tsReal) deg * na / den_a;
			if (den_b > 0.0)
				derivs[i] -= (tsReal) deg * nb / den_b;
		}
	}
	free(window);
	free(lower);
	TS_RETURN_SUCCESS(status)
}
/*! @} */



/**
 * Uniform natural cubic interpolation in full ::tsReal precision, following
 * exactly the convention of ts_bspline_interpolate_cubic_natural: the data
 * points are the junction control points of a sequence of clamped cubic
 * Bezier segments. The production implementation hard-codes single-precision
 * constants (e.g. 1.f/6.f) and float-rounded Bezier knots, which would keep
 * the lofted surface from exactly reproducing its input sections; this
 * helper reproduces the same spline with exact fractions.
 *
 * @param points
 * 	\p n data points with \p dim components each.
 * @param n
 * 	Number of data points (>= 3).
 * @param dim
 * 	Number of components per point.
 * @param ctrlp
 * 	Output array with (n - 1) * 4 * dim entries.
 * @param knots
 * 	Output array with 4 * n clamped Bezier knots (n knot groups of
 * 	multiplicity four).
 */
static tsError
ts_int_loft_natural_cubic(const tsReal *points,
                          size_t n,
                          size_t dim,
                          tsReal *ctrlp,
                          tsReal *knots,
                          tsStatus *status)
{
	const size_t m = n - 2; /* Number of interior unknowns. */
	tsReal *q = NULL;       /* Solved S values, one per node. */
	tsReal *cprime = NULL;
	tsReal *work = NULL;
	size_t g, i, d, seg;

	q = (tsReal *) malloc(n * dim * sizeof(tsReal));
	cprime = (tsReal *) malloc(m * sizeof(tsReal));
	work = (tsReal *) malloc(m * sizeof(tsReal));
	if (!q || !cprime || !work) {
		free(q);
		free(cprime);
		free(work);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}

	/* Thomas algorithm coefficients of the constant tri-diagonal
	 * system Q_{i-1} + 4 Q_i + Q_{i+1} = 6 b_i. */
	cprime[0] = 1.0 / 4.0;
	for (i = 1; i < m; i++)
		cprime[i] = 1.0 / (4.0 - cprime[i - 1]);

	for (d = 0; d < dim; d++) {
		q[d] = points[d];                 /* S_0 = b_0 */
		q[(n - 1) * dim + d] =
			points[(n - 1) * dim + d];     /* S_{n-1} = b_{n-1} */
		/* Build the full right-hand side first so that the single
		 * interior-unknown case (m == 1) subtracts both end
		 * contributions. */
		for (i = 0; i < m; i++)
			work[i] = 6.0 * points[(i + 1) * dim + d];
		work[0] -= points[d];
		work[m - 1] -= points[(n - 1) * dim + d];
		/* Forward sweep of the Thomas algorithm. */
		work[0] /= 4.0;
		for (i = 1; i < m; i++)
			work[i] = (work[i] - work[i - 1]) * cprime[i];
		/* Back substitution. */
		q[(n - 2) * dim + d] = work[m - 1];
		for (i = m - 1; i >= 1; i--)
			q[i * dim + d] =
				work[i - 1] - cprime[i - 1] * q[(i + 1) * dim + d];
	}

	/* Clamped Bezier knot vector: n groups of four equal knots at the
	 * uniform parameters g / (n - 1). */
	for (g = 0; g < n; g++) {
		const tsReal knot = (tsReal) g / (tsReal) (n - 1);
		size_t r;
		for (r = 0; r < 4; r++)
			knots[g * 4 + r] = knot;
	}

	for (seg = 0; seg < n - 1; seg++) {
		for (d = 0; d < dim; d++) {
			tsReal s0, s1;
			if (seg == 0)
				s0 = q[d];
			else
				s0 = (q[(seg - 1) * dim + d] +
				      4.0 * q[seg * dim + d] +
				      q[(seg + 1) * dim + d]) / 6.0;
			if (seg == n - 2)
				s1 = q[(n - 1) * dim + d];
			else
				s1 = (q[seg * dim + d] +
				      4.0 * q[(seg + 1) * dim + d] +
				      q[(seg + 2) * dim + d]) / 6.0;
			ctrlp[(seg * 4 + 0) * dim + d] = s0;
			ctrlp[(seg * 4 + 1) * dim + d] =
				(2.0 * q[seg * dim + d] +
				 q[(seg + 1) * dim + d]) / 3.0;
			ctrlp[(seg * 4 + 2) * dim + d] =
				(q[seg * dim + d] +
				 2.0 * q[(seg + 1) * dim + d]) / 3.0;
			ctrlp[(seg * 4 + 3) * dim + d] = s1;
		}
	}
	free(q);
	free(cprime);
	free(work);
	TS_RETURN_SUCCESS(status)
}

tsLoftSurface
ts_loft_surface_init(void)
{
	tsLoftSurface surface;
	ts_int_loft_init(&surface);
	return surface;
}

tsError
ts_loft_surface_new(const tsBSpline *sections,
                    size_t count,
                    tsLoftSurface *out,
                    tsStatus *status)
{
	tsBSpline *normalized = NULL; /* Per-section homogeneous copies. */
	tsReal *common = NULL;        /* Common u knot vector. */
	size_t n_common = 0;
	size_t common_deg = 0;
	size_t j, i, c;
	tsError err;

	tsReal *column = NULL;
	tsReal *v_ctrlp = NULL;
	tsLoftSurface built = ts_loft_surface_init();
	tsReal *net = NULL;
	tsReal *knots_u = NULL;
	tsReal *knots_v = NULL;
	size_t n_u, n_v, n_knots_v;

	if (status != NULL) {
		status->code = TS_SUCCESS;
		status->message[0] = '\0';
	}
	if (count < 2)
		TS_RETURN_1(status, TS_NUM_POINTS,
		            "num(sections) (%lu) < 2",
		            (unsigned long) count)
	if (sections == NULL)
		TS_RETURN_0(status, TS_NUM_POINTS, "sections is NULL")

	normalized = (tsBSpline *) calloc(count, sizeof(tsBSpline));
	if (!normalized)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	for (j = 0; j < count; j++)
		normalized[j] = ts_bspline_init();

	TS_TRY(try, err, status)
		/* Validate and normalize every section. */
		for (j = 0; j < count; j++) {
			TS_CALL(try, err, ts_int_loft_validate_section(
			        &sections[j], status))
			TS_CALL(try, err, ts_int_loft_normalize_section(
			        &sections[j], &normalized[j], status))
			if (ts_bspline_degree(&normalized[j]) > common_deg)
				common_deg = ts_bspline_degree(&normalized[j]);
		}

		/* Unify the degree along u. */
		for (j = 0; j < count; j++) {
			if (ts_bspline_degree(&normalized[j]) < common_deg) {
				tsBSpline elevated = ts_bspline_init();
				TS_CALL(try, err, ts_bspline_elevate_degree(
				        &normalized[j],
				        common_deg -
				        ts_bspline_degree(&normalized[j]),
				        TS_LOFT_KNOT_EPS, &elevated, status))
				ts_bspline_free(&normalized[j]);
				normalized[j] = elevated;
			}
		}

		/* Unify the u knot vectors. */
		TS_CALL(try, err, ts_int_loft_merge_knots(
		        normalized, count, &common, &n_common, status))
		for (j = 0; j < count; j++) {
			TS_CALL(try, err, ts_int_loft_refine(
			        &normalized[j], common, n_common, status))
		}

		n_u = ts_bspline_num_control_points(&normalized[0]);
		for (j = 1; j < count; j++) {
			if (ts_bspline_num_control_points(&normalized[j]) != n_u)
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "section basis unification failed")
		}

		/* Arrange the section control points as u rows. The net first
		 * stores `count' v columns; v interpolation happens below. */
		net = (tsReal *) malloc(
			n_u * count * 4 * sizeof(tsReal));
		if (!net)
			TS_THROW_0(try, err, status, TS_MALLOC,
			           "out of memory")
		for (j = 0; j < count; j++) {
			const tsReal *ctrlp =
				ts_bspline_control_points_ptr(&normalized[j]);
			for (i = 0; i < n_u; i++)
				for (c = 0; c < 4; c++)
					net[(i * count + j) * 4 + c] =
						ctrlp[i * 4 + c];
		}

		knots_u = (tsReal *) malloc(n_common * sizeof(tsReal));
		if (!knots_u)
			TS_THROW_0(try, err, status, TS_MALLOC,
			           "out of memory")
		memcpy(knots_u, common, n_common * sizeof(tsReal));

		/* Interpolate every homogeneous control point along v. */
		column = (tsReal *) malloc(count * 4 * sizeof(tsReal));
		if (!column)
			TS_THROW_0(try, err, status, TS_MALLOC,
			           "out of memory")

		if (count == 2) {
			/* Ruled interpolation: degree 1, knots [0,0,1,1]. */
			n_v = 2;
			n_knots_v = 4;
			knots_v = (tsReal *) malloc(
				n_knots_v * sizeof(tsReal));
			if (!knots_v)
				TS_THROW_0(try, err, status, TS_MALLOC,
				           "out of memory")
			knots_v[0] = knots_v[1] = 0.0;
			knots_v[2] = knots_v[3] = 1.0;
		} else {
			/* Natural cubic interpolation: (count - 1) clamped
			 * Bezier segments, each contributing four control
			 * points; the knot vector holds count knot groups of
			 * multiplicity four (including the final 1.0 group). */
			n_v = (count - 1) * 4;
			n_knots_v = count * 4;
			knots_v = (tsReal *) malloc(
				n_knots_v * sizeof(tsReal));
			if (!knots_v)
				TS_THROW_0(try, err, status, TS_MALLOC,
				           "out of memory")
		}

		v_ctrlp = (tsReal *) malloc(n_u * n_v * 4 * sizeof(tsReal));
		if (!v_ctrlp)
			TS_THROW_0(try, err, status, TS_MALLOC,
			           "out of memory")

		for (i = 0; i < n_u; i++) {
			for (j = 0; j < count; j++)
				for (c = 0; c < 4; c++)
					column[j * 4 + c] =
						net[(i * count + j) * 4 + c];

			if (count == 2) {
				for (c = 0; c < 4; c++) {
					v_ctrlp[(i * n_v + 0) * 4 + c] =
						column[c];
					v_ctrlp[(i * n_v + 1) * 4 + c] =
						column[4 + c];
				}
			} else {
				TS_CALL(try, err, ts_int_loft_natural_cubic(
				        column, count, 4,
				        v_ctrlp + i * n_v * 4,
				        knots_v, status))
			}
		}
		free(net);
		net = v_ctrlp;
		v_ctrlp = NULL;

		/* The weights of the final net must stay positive. */
		for (i = 0; i < n_u * n_v; i++) {
			const tsReal w = net[i * 4 + 3];
			if (!isfinite(w) || !(w > 0.0))
				TS_THROW_0(try, err, status, TS_NO_RESULT,
				           "lofted control net has a non-positive "
				           "weight: denominator pole")
			for (c = 0; c < 3; c++) {
				if (!isfinite(net[i * 4 + c]))
					TS_THROW_0(try, err, status, TS_NO_RESULT,
					           "lofted control net is not finite")
			}
		}

		built.pImpl = (struct tsLoftSurfaceImpl *)
			malloc(sizeof(struct tsLoftSurfaceImpl));
		if (!built.pImpl)
			TS_THROW_0(try, err, status, TS_MALLOC,
			           "out of memory")
		built.pImpl->deg_u = common_deg;
		built.pImpl->deg_v = count == 2 ? 1 : 3;
		built.pImpl->n_u = n_u;
		built.pImpl->n_v = n_v;
		built.pImpl->n_knots_u = n_common;
		built.pImpl->n_knots_v = n_knots_v;
		built.pImpl->net = net;
		built.pImpl->knots_u = knots_u;
		built.pImpl->knots_v = knots_v;
		net = NULL;
		knots_u = NULL;
		knots_v = NULL;

		/* Success: replace any data previously held by `out`. */
		ts_loft_surface_free(out);
		*out = built;
		ts_int_loft_init(&built);
	TS_CATCH(err)
		(void) 0;
	TS_FINALLY
		if (normalized) {
			for (j = 0; j < count; j++)
				ts_bspline_free(&normalized[j]);
			free(normalized);
		}
		free(common);
		free(column);
		free(net);
		free(knots_u);
		free(knots_v);
		free(v_ctrlp);
		ts_loft_surface_free(&built);
	TS_END_TRY_RETURN(err)
}

tsError
ts_loft_surface_copy(const tsLoftSurface *src,
                     tsLoftSurface *dest,
                     tsStatus *status)
{
	struct tsLoftSurfaceImpl *impl;
	const size_t sof_real = sizeof(tsReal);

	if (src == dest)
		TS_RETURN_SUCCESS(status)
	if (src->pImpl == NULL)
		TS_RETURN_0(status, TS_NO_RESULT, "source surface is empty")

	/* Replace any data previously owned by dest. */
	ts_loft_surface_free(dest);
	ts_int_loft_init(dest);
	impl = (struct tsLoftSurfaceImpl *)
		malloc(sizeof(struct tsLoftSurfaceImpl));
	if (!impl)
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	*impl = *src->pImpl;
	impl->net = NULL;
	impl->knots_u = NULL;
	impl->knots_v = NULL;

	impl->net = (tsReal *) malloc(
		impl->n_u * impl->n_v * 4 * sof_real);
	impl->knots_u = (tsReal *) malloc(
		impl->n_knots_u * sof_real);
	impl->knots_v = (tsReal *) malloc(
		impl->n_knots_v * sof_real);
	if (!impl->net || !impl->knots_u || !impl->knots_v) {
		free(impl->net);
		free(impl->knots_u);
		free(impl->knots_v);
		free(impl);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}
	memcpy(impl->net, src->pImpl->net,
	       impl->n_u * impl->n_v * 4 * sof_real);
	memcpy(impl->knots_u, src->pImpl->knots_u,
	       impl->n_knots_u * sof_real);
	memcpy(impl->knots_v, src->pImpl->knots_v,
	       impl->n_knots_v * sof_real);

	dest->pImpl = impl;
	TS_RETURN_SUCCESS(status)
}

void
ts_loft_surface_free(tsLoftSurface *surface)
{
	if (surface == NULL || surface->pImpl == NULL)
		return;
	free(surface->pImpl->net);
	free(surface->pImpl->knots_u);
	free(surface->pImpl->knots_v);
	free(surface->pImpl);
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
	tsReal *nu = NULL, *dnu = NULL, *nv = NULL, *dnv = NULL;
	tsReal homo[4], homo_u[4], homo_v[4];
	tsReal w;
	size_t i, j, c;
	tsError err;

	if (status != NULL) {
		status->code = TS_SUCCESS;
		status->message[0] = '\0';
	}
	if (surface == NULL || surface->pImpl == NULL)
		TS_RETURN_0(status, TS_NO_RESULT, "surface is empty")
	impl = surface->pImpl;
	if (!(u >= 0.0 && u <= 1.0))
		TS_RETURN_1(status, TS_U_UNDEFINED,
		            "u (%f) outside of [0, 1]", (double) u)
	if (!(v >= 0.0 && v <= 1.0))
		TS_RETURN_1(status, TS_U_UNDEFINED,
		            "v (%f) outside of [0, 1]", (double) v)

	nu = (tsReal *) malloc(impl->n_u * sizeof(tsReal));
	dnu = (tsReal *) malloc(impl->n_u * sizeof(tsReal));
	nv = (tsReal *) malloc(impl->n_v * sizeof(tsReal));
	dnv = (tsReal *) malloc(impl->n_v * sizeof(tsReal));
	if (!nu || !dnu || !nv || !dnv) {
		free(nu);
		free(dnu);
		free(nv);
		free(dnv);
		TS_RETURN_0(status, TS_MALLOC, "out of memory")
	}
	err = ts_int_loft_basis(impl->knots_u, impl->n_u, impl->deg_u,
	                        u, nu, dnu, status);
	if (err)
		goto eval_cleanup;
	err = ts_int_loft_basis(impl->knots_v, impl->n_v, impl->deg_v,
	                        v, nv, dnv, status);
	if (err)
		goto eval_cleanup;

	for (c = 0; c < 4; c++) {
		homo[c] = homo_u[c] = homo_v[c] = 0.0;
	}
	for (i = 0; i < impl->n_u; i++) {
		for (j = 0; j < impl->n_v; j++) {
			const tsReal *p = impl->net +
			                  (i * impl->n_v + j) * 4;
			const tsReal f = nu[i] * nv[j];
			const tsReal fu = dnu[i] * nv[j];
			const tsReal fv = nu[i] * dnv[j];
			for (c = 0; c < 4; c++) {
				homo[c] += f * p[c];
				homo_u[c] += fu * p[c];
				homo_v[c] += fv * p[c];
			}
		}
	}
	free(nu);
	free(dnu);
	free(nv);
	free(dnv);
	goto eval_after_basis;

eval_cleanup:
	free(nu);
	free(dnu);
	free(nv);
	free(dnv);
	return err;

eval_after_basis:
	w = homo[3];
	if (!isfinite(w) || !(w > 0.0))
		TS_RETURN_0(status, TS_NO_RESULT,
		            "rational denominator is not positive")

	/* Quotient rule: S = H/w, dS = (dH - S * dw) / w. */
	for (c = 0; c < 3; c++) {
		point[c] = homo[c] / w;
		du[c] = (homo_u[c] - point[c] * homo_u[3]) / w;
		dv[c] = (homo_v[c] - point[c] * homo_v[3]) / w;
		if (!isfinite(point[c]) ||
		    !isfinite(du[c]) || !isfinite(dv[c]))
			TS_RETURN_0(status, TS_NO_RESULT,
			            "surface evaluation is not finite")
	}
	TS_RETURN_SUCCESS(status)
}

size_t
ts_loft_surface_u_degree(const tsLoftSurface *surface)
{
	return surface->pImpl->deg_u;
}

size_t
ts_loft_surface_v_degree(const tsLoftSurface *surface)
{
	return surface->pImpl->deg_v;
}

size_t
ts_loft_surface_num_u_control_points(const tsLoftSurface *surface)
{
	return surface->pImpl->n_u;
}

size_t
ts_loft_surface_num_v_control_points(const tsLoftSurface *surface)
{
	return surface->pImpl->n_v;
}

size_t
ts_loft_surface_num_u_knots(const tsLoftSurface *surface)
{
	return surface->pImpl->n_knots_u;
}

size_t
ts_loft_surface_num_v_knots(const tsLoftSurface *surface)
{
	return surface->pImpl->n_knots_v;
}

const tsReal *
ts_loft_surface_control_points_ptr(const tsLoftSurface *surface)
{
	return surface->pImpl->net;
}

const tsReal *
ts_loft_surface_u_knots_ptr(const tsLoftSurface *surface)
{
	return surface->pImpl->knots_u;
}

const tsReal *
ts_loft_surface_v_knots_ptr(const tsLoftSurface *surface)
{
	return surface->pImpl->knots_v;
}
