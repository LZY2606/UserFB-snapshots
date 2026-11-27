/*
 * Lofting Example
 *
 * Lofts a set of clamped section curves into a rational tensor product
 * B-Spline surface using ts_loft_surface_new, samples the resulting surface
 * on a regular grid together with analytic normals, and writes the mesh to
 * "<output directory>/result.obj".
 *
 * Usage:
 *     loft <output directory>
 */
#include "tinyspline.h"
#include "tinyspline_surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define LOFT_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define LOFT_MKDIR(path) mkdir((path), 0755)
#endif

#define GRID_U 24
#define GRID_V 16
#define NUM_SECTIONS 4
#define RING_POINTS 7

int
main(int argc, char **argv)
{
	tsStatus status;
	tsBSpline sections[NUM_SECTIONS];
	tsLoftSurface surface = ts_loft_surface_init();
	tsReal control_points[NUM_SECTIONS][RING_POINTS][4];
	tsReal knots[RING_POINTS + 4]; /* 7 control points, degree 3. */
	tsReal vertices[GRID_U][GRID_V][3];
	tsReal normals[GRID_U][GRID_V][3];
	char path[1024];
	FILE *obj;
	size_t s, i, j, k, d;
	tsError err;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
		return 2;
	}

	/* Create the output directory if it does not exist yet. An
	 * "already exists" result is harmless; the fopen below is the
	 * authoritative check that the directory is writable. */
	(void) LOFT_MKDIR(argv[1]);

	/* Clamped cubic knot vector with domain [0, 1]. */
	knots[0] = knots[1] = knots[2] = knots[3] = 0.0;
	knots[4] = 1.0 / 4.0;
	knots[5] = 2.0 / 4.0;
	knots[6] = 3.0 / 4.0;
	knots[7] = knots[8] = knots[9] = knots[10] = 1.0;

	/* Build wavy ring sections. The first and last control point of each
	 * ring coincide so that every section describes a closed loop. */
	for (s = 0; s < NUM_SECTIONS; s++) {
		const tsReal twist = (tsReal) s * 0.35f;
		const tsReal radius = 1.0f + 0.35f * (tsReal) s;
		const tsReal z = (tsReal) s - 1.5f;
		for (i = 0; i < RING_POINTS; i++) {
			const tsReal angle = twist +
				(tsReal) i * 2.0f * (tsReal) TS_PI /
				(tsReal) (RING_POINTS - 1);
			const tsReal wave = 1.0f +
				0.15f * (tsReal) sin(3.0 * angle + s);
			control_points[s][i][0] =
				radius * wave * (tsReal) cos(angle);
			control_points[s][i][1] =
				radius * wave * (tsReal) sin(angle);
			control_points[s][i][2] = z;
			control_points[s][i][3] = 1.0f;
		}
		/* Make the second section rational with varied weights. */
		if (s == 1) {
			const tsReal weights[RING_POINTS] =
				{1.0f, 1.6f, 0.8f, 1.6f, 0.8f, 1.6f, 1.0f};
			for (i = 0; i < RING_POINTS; i++) {
				for (d = 0; d < 3; d++)
					control_points[s][i][d] *=
						weights[i];
				control_points[s][i][3] = weights[i];
			}
		}
		/* Close the loop. */
		for (d = 0; d < 4; d++)
			control_points[s][i - 1][d] =
				control_points[s][0][d];

		sections[s] = ts_bspline_init();
		err = ts_bspline_new(RING_POINTS, s == 1 ? 4 : 3, 3,
		                     TS_OPENED, &sections[s], &status);
		if (err) {
			fprintf(stderr, "ts_bspline_new: %s\n",
			        status.message);
			return 1;
		}
		err = ts_bspline_set_control_points(
			&sections[s], &control_points[s][0][0], &status);
		if (err) {
			fprintf(stderr, "set_control_points: %s\n",
			        status.message);
			return 1;
		}
		err = ts_bspline_set_knots(&sections[s], knots, &status);
		if (err) {
			fprintf(stderr, "set_knots: %s\n",
			        status.message);
			return 1;
		}
	}

	err = ts_loft_surface_new(sections, NUM_SECTIONS,
	                          &surface, &status);
	for (s = 0; s < NUM_SECTIONS; s++)
		ts_bspline_free(&sections[s]);
	if (err) {
		fprintf(stderr, "ts_loft_surface_new: %s\n",
		        status.message);
		return 1;
	}

	/* Sample the surface and compute analytic normals. */
	for (i = 0; i < GRID_U; i++) {
		const tsReal u = (tsReal) i / (tsReal) (GRID_U - 1);
		for (j = 0; j < GRID_V; j++) {
			tsReal point[3], du[3], dv[3];
			tsReal nx, ny, nz, length;
			const tsReal v =
				(tsReal) j / (tsReal) (GRID_V - 1);
			err = ts_loft_surface_eval(&surface, u, v,
			                           point, du, dv, &status);
			if (err) {
				fprintf(stderr, "eval: %s\n",
				        status.message);
				return 1;
			}
			for (k = 0; k < 3; k++)
				vertices[i][j][k] = point[k];
			nx = du[1] * dv[2] - du[2] * dv[1];
			ny = du[2] * dv[0] - du[0] * dv[2];
			nz = du[0] * dv[1] - du[1] * dv[0];
			length = (tsReal) sqrt(nx * nx + ny * ny +
			                        nz * nz);
			if (length == 0.0f)
				length = 1.0f;
			normals[i][j][0] = nx / length;
			normals[i][j][1] = ny / length;
			normals[i][j][2] = nz / length;
		}
	}

	snprintf(path, sizeof(path), "%s/result.obj", argv[1]);
	obj = fopen(path, "w");
	if (!obj) {
		fprintf(stderr, "cannot open %s for writing\n", path);
		return 1;
	}
	fprintf(obj, "# Lofted NURBS surface sampled with TinySpline\n");
	for (i = 0; i < GRID_U; i++) {
		for (j = 0; j < GRID_V; j++) {
			fprintf(obj, "v %.9f %.9f %.9f\n",
			        (double) vertices[i][j][0],
			        (double) vertices[i][j][1],
			        (double) vertices[i][j][2]);
		}
	}
	for (i = 0; i < GRID_U; i++) {
		for (j = 0; j < GRID_V; j++) {
			fprintf(obj, "vn %.9f %.9f %.9f\n",
			        (double) normals[i][j][0],
			        (double) normals[i][j][1],
			        (double) normals[i][j][2]);
		}
	}
	/* Quad faces, split into triangles. OBJ indices are one-based. */
	for (i = 0; i + 1 < GRID_U; i++) {
		for (j = 0; j + 1 < GRID_V; j++) {
			const size_t a = i * GRID_V + j + 1;
			const size_t b = i * GRID_V + (j + 1) + 1;
			const size_t c = (i + 1) * GRID_V + (j + 1) + 1;
			const size_t d2 = (i + 1) * GRID_V + j + 1;
			fprintf(obj, "f %zu//%zu %zu//%zu %zu//%zu\n",
			        a, a, b, b, c, c);
			fprintf(obj, "f %zu//%zu %zu//%zu %zu//%zu\n",
			        a, a, c, c, d2, d2);
		}
	}
	fclose(obj);

	printf("wrote %s\n", path);
	printf("grid=%zux%zu vertices=%zu normals=%zu faces=%zu "
	       "u_degree=%zu v_degree=%zu control_net=%zux%zu\n",
	       (size_t) GRID_U, (size_t) GRID_V,
	       (size_t) GRID_U * GRID_V,
	       (size_t) GRID_U * GRID_V,
	       (size_t) 2 * (GRID_U - 1) * (GRID_V - 1),
	       ts_loft_surface_u_degree(&surface),
	       ts_loft_surface_v_degree(&surface),
	       ts_loft_surface_num_u_control_points(&surface),
	       ts_loft_surface_num_v_control_points(&surface));

	ts_loft_surface_free(&surface);
	return 0;
}
