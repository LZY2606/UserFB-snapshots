#include "tinyspline.h"
#include "tinyspline_surface.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define LOFT_MKDIR(path) _mkdir(path)
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#define LOFT_MKDIR(path) mkdir((path), 0755)
#endif

/*
 * Loft example: builds a handful of clamped section curves with different
 * degrees and knot vectors, lofts them into a tensor product surface and
 * samples the surface together with its analytic normals into a Wavefront
 * OBJ file. The example uses the C99 standard library and libm only.
 */

#define NUM_SECTIONS 5
#define GRID_U 24
#define GRID_V 24

static tsBSpline
make_section(size_t index)
{
	/* Five non-planar sections of degree 1, 2 and 3 with deliberately
	 * different interior knots and parameter domains. */
	static const tsReal ctrlp[5][21] = {
		{0, 0, 0,  2, 1, 1,  4, 0, 2}, /* degree 2, 3 controls */
		{0, 1, 1,  1.5, 2, 0,  3, 1, -1,  4, 2, 1,  4.5, 1, 2},
		{0, 2, 2,  4, 3, 0},           /* degree 1, 2 controls */
		{0, 3, 1,  1, 4, 2,  3, 2, 0,  4, 3, 2,  4.5, 4, 1},
		{0, 4, 0,  1.5, 5, 2,  3, 4.5, 1,  4, 4, 1} /* degree 2, 4 */
	};
	static const size_t num_ctrlp[5] = {3, 5, 2, 5, 4};
	static const size_t degree[5] = {2, 3, 1, 3, 2};
	static const tsReal knots[5][11] = {
		{0, 0, 0, 1, 1, 1},                /* 6 knots,  [0, 1] */
		{2, 2, 2, 2, 4.25, 5, 5, 5, 5},    /* 9 knots,  [2, 5] */
		{-1, -1, 3, 3},                    /* 4 knots,  [-1, 3] */
		{0, 0, 0, 0, .5, 1, 1, 1, 1},      /* 9 knots,  interior .5 */
		{0, 0, 0, .7, 1, 1, 1}             /* 7 knots,  interior .7 */
	};

	tsBSpline section = ts_bspline_init();
	tsStatus status;
	tsError err = ts_bspline_new(num_ctrlp[index], 3, degree[index],
	                             TS_OPENED, &section, &status);
	if (err != TS_SUCCESS) {
		fprintf(stderr, "ts_bspline_new failed: %s\n", status.message);
		exit(1);
	}
	err = ts_bspline_set_knots(&section, knots[index], &status);
	if (err != TS_SUCCESS) {
		fprintf(stderr, "ts_bspline_set_knots failed: %s\n",
		        status.message);
		exit(1);
	}
	err = ts_bspline_set_control_points(&section, ctrlp[index], &status);
	if (err != TS_SUCCESS) {
		fprintf(stderr, "ts_bspline_set_control_points failed: %s\n",
		        status.message);
		exit(1);
	}
	return section;
}

int
main(int argc, char **argv)
{
	tsBSpline sections[NUM_SECTIONS];
	tsLoftSurface surface = ts_loft_surface_init();
	tsStatus status;
	char path[1024];
	FILE *file;
	size_t i, j;
	int err;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
		return 2;
	}

	for (i = 0; i < NUM_SECTIONS; i++)
		sections[i] = make_section(i);

	if (ts_loft_surface_new(sections, NUM_SECTIONS, &surface,
	                        &status) != TS_SUCCESS) {
		fprintf(stderr, "ts_loft_surface_new failed: %s\n",
		        status.message);
		return 1;
	}
	for (i = 0; i < NUM_SECTIONS; i++)
		ts_bspline_free(&sections[i]);

	err = snprintf(path, sizeof(path), "%s/result.obj", argv[1]);
	if (err <= 0 || (size_t) err >= sizeof(path)) {
		fprintf(stderr, "output path is too long\n");
		return 1;
	}
	/* Create the output directory if it does not exist yet. An
	 * already existing directory (EEXIST) is not an error. */
#if defined(_WIN32)
	LOFT_MKDIR(argv[1]);
#else
	if (LOFT_MKDIR(argv[1]) != 0 && errno != EEXIST) {
		fprintf(stderr, "cannot create directory %s\n", argv[1]);
		return 1;
	}
#endif
	file = fopen(path, "w");
	if (!file) {
		fprintf(stderr, "cannot open %s for writing\n", path);
		return 1;
	}

	fprintf(file, "# Loft surface sampled with tinyspline_surface\n");
	fprintf(file, "# grid: %d x %d\n", GRID_U, GRID_V);

	/* Vertices and analytic normals (du x dv). */
	for (j = 0; j < GRID_V; j++) {
		const tsReal v = (tsReal) j / (tsReal) (GRID_V - 1);
		for (i = 0; i < GRID_U; i++) {
			tsReal point[3], du[3], dv[3];
			tsReal nx, ny, nz, len;
			const tsReal u = (tsReal) i / (tsReal) (GRID_U - 1);
			if (ts_loft_surface_eval(&surface, u, v, point, du, dv,
			                         &status) != TS_SUCCESS) {
				fprintf(stderr, "eval failed: %s\n",
				        status.message);
				fclose(file);
				return 1;
			}
			nx = du[1] * dv[2] - du[2] * dv[1];
			ny = du[2] * dv[0] - du[0] * dv[2];
			nz = du[0] * dv[1] - du[1] * dv[0];
			len = (tsReal) sqrt((double) (nx * nx + ny * ny +
			                              nz * nz));
			fprintf(file, "v %.9f %.9f %.9f\n",
			        (double) point[0], (double) point[1],
			        (double) point[2]);
			fprintf(file, "vn %.9f %.9f %.9f\n",
			        (double) (nx / len), (double) (ny / len),
			        (double) (nz / len));
		}
	}

	/* Quad connectivity; OBJ indices are 1 based. */
	for (j = 0; j + 1 < GRID_V; j++) {
		for (i = 0; i + 1 < GRID_U; i++) {
			const size_t a = j * GRID_U + i + 1;
			const size_t b = a + 1;
			const size_t c = a + GRID_U;
			const size_t d = c + 1;
			fprintf(file, "f %zu//%zu %zu//%zu %zu//%zu %zu//%zu\n",
			        a, a, b, b, d, d, c, c);
		}
	}

	fclose(file);
	printf("wrote %s\n", path);
	printf("sections=%d grid_u=%d grid_v=%d vertices=%d faces=%d "
	       "u_controls=%zu v_controls=%zu\n",
	       NUM_SECTIONS, GRID_U, GRID_V, GRID_U * GRID_V,
	       (GRID_U - 1) * (GRID_V - 1),
	       ts_loft_surface_num_control_points_u(&surface),
	       ts_loft_surface_num_control_points_v(&surface));
	ts_loft_surface_free(&surface);
	return 0;
}
