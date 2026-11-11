/*
** Test driver for per-vertex attribute interpolation in libtess2.
** Built and run by Tests/check_attributes.sh.
*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tesselator.h"

#ifndef TESS_ATTR_EPS
#define TESS_ATTR_EPS 1e-4f
#endif

static int g_failures;

static void fail_report(const char *what, int vertex_index, float x, float y,
						int component, float expected, float actual)
{
	printf("FAIL [%s]: vertex #%d at (%g, %g), component %d: expected %g, got %g\n",
		   what, vertex_index, (double)x, (double)y, component,
		   (double)expected, (double)actual);
	g_failures++;
}

/* Finds the output vertex closest to (x,y); returns -1 if none is near. */
static int find_vertex(const TESSreal *verts, int vertex_count, int vertex_size,
					   float x, float y)
{
	int i, best = -1;
	float best_dist = 1e-3f;
	for (i = 0; i < vertex_count; ++i) {
		float vx = verts[i * vertex_size];
		float vy = verts[i * vertex_size + 1];
		float d = fabsf(vx - x) + fabsf(vy - y);
		if (d < best_dist) {
			best_dist = d;
			best = i;
		}
	}
	return best;
}

static void check_attrs_at(const char *what, TESStesselator *tess,
						   float x, float y, const float *expected,
						   int attr_size)
{
	const TESSreal *verts = tessGetVertices(tess);
	const TESSreal *attrs = tessGetVertexAttributes(tess);
	int n = tessGetVertexCount(tess);
	int i, vi = find_vertex(verts, n, 2, x, y);
	if (vi < 0) {
		printf("FAIL [%s]: no output vertex at (%g, %g)\n", what, (double)x, (double)y);
		g_failures++;
		return;
	}
	for (i = 0; i < attr_size; ++i) {
		float got = attrs[vi * attr_size + i];
		if (fabsf(got - expected[i]) > TESS_ATTR_EPS)
			fail_report(what, vi, verts[vi * 2], verts[vi * 2 + 1], i,
						expected[i], got);
	}
}

static float l1dist(float ax, float ay, float bx, float by)
{
	return fabsf(ax - bx) + fabsf(ay - by);
}

/* Mirror of the library's intersection weighting (VertexWeights): each of the
** two edges gets half the weight, split between its endpoints by the L1
** distance of the intersection (ix,iy) to those endpoints. Using L1 rather
** than Euclidean distance is deliberate -- it must match the tesselator
** exactly, including for non-midpoint intersections.
*/
static float expected_cross_attr(float px, float py, float pa,
								 float qx, float qy, float qa,
								 float rx, float ry, float ra,
								 float sx, float sy, float sa,
								 float ix, float iy)
{
	float dp = l1dist(px, py, ix, iy), dq = l1dist(qx, qy, ix, iy);
	float dr = l1dist(rx, ry, ix, iy), ds = l1dist(sx, sy, ix, iy);
	float e1 = (dq * pa + dp * qa) / (dp + dq);
	float e2 = (ds * ra + dr * sa) / (dr + ds);
	return 0.5f * e1 + 0.5f * e2;
}

/*
** Minimal example from the API design: bowtie (0,0),(4,4),(0,4),(4,0),
** attributes 0,0,0,8, odd rule. The intersection at (2,2) must get the
** value 2: each edge interpolates 0 and 4 at the midpoint, then the two
** edges are averaged.
*/
static void test_bowtie_basic_intersection(void)
{
	static const TESSreal coords[] = {0, 0, 4, 4, 0, 4, 4, 0};
	static const TESSreal attrs[] = {0, 0, 0, 8};
	TESStesselator *tess = tessNewTess(NULL);
	const TESSindex *indices;
	int rc, vi;

	rc = tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
									  1, attrs, sizeof(TESSreal));
	if (rc != 1) {
		printf("FAIL [bowtie]: tessAddContourWithAttributes returned %d, status %d\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [bowtie]: tessTesselate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}

	if (tessGetVertexAttributeSize(tess) != 1) {
		printf("FAIL [bowtie]: attribute size %d, expected 1\n",
			   tessGetVertexAttributeSize(tess));
		g_failures++;
	}

	check_attrs_at("bowtie-original-0", tess, 0, 0, (float[]){0}, 1);
	check_attrs_at("bowtie-original-1", tess, 4, 4, (float[]){0}, 1);
	check_attrs_at("bowtie-original-2", tess, 0, 4, (float[]){0}, 1);
	check_attrs_at("bowtie-original-3", tess, 4, 0, (float[]){8}, 1);
	check_attrs_at("bowtie-intersection", tess, 2, 2, (float[]){2}, 1);

	/* The old convention is untouched: new intersections report TESS_UNDEF. */
	indices = tessGetVertexIndices(tess);
	vi = find_vertex(tessGetVertices(tess), tessGetVertexCount(tess), 2, 2, 2);
	if (vi < 0 || indices[vi] != TESS_UNDEF) {
		printf("FAIL [bowtie]: intersection vertex index is %d, expected TESS_UNDEF\n",
			   vi < 0 ? -1 : (int)indices[vi]);
		g_failures++;
	}

	tessDeleteTess(tess);
}

/*
** One long edge cut by several intersection points in sequence. Two crossing
** contours (a bar and a triangle) produce four crossings, each cutting an edge
** that may already have been split. The first crossing on an edge has an exact
** closed-form value; later crossings must carry the already-blended attributes
** of earlier intersection vertices forward rather than recomputing from the
** original endpoints. All results stay convex combinations of their two
** contributing edge endpoints.
*/
static void test_edge_split_multiple_times(void)
{
	static const TESSreal bar[] = {0, 0, 10, 0, 10, 2, 0, 2};
	static const TESSreal bar_attr[] = {10, 20, 30, 40};
	static const TESSreal tri[] = {2, -1, 8, -1, 5, 3};
	static const TESSreal tri_attr[] = {1, 5, 9};
	TESStesselator *tess = tessNewTess(NULL);
	const TESSreal *verts, *attrs;
	const TESSindex *indices;
	int n, i, undef_count = 0;
	float expect_first;

	tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
								 1, bar_attr, sizeof(TESSreal));
	tessAddContourWithAttributes(tess, 2, tri, 2 * sizeof(TESSreal), 3,
								 1, tri_attr, sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [multi-split]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}

	verts = tessGetVertices(tess);
	attrs = tessGetVertexAttributes(tess);
	indices = tessGetVertexIndices(tess);
	n = tessGetVertexCount(tess);

	/* Every generated intersection keeps the TESS_UNDEF index convention. */
	for (i = 0; i < n; ++i)
		if (indices[i] == TESS_UNDEF)
			undef_count++;
	if (undef_count != 4) {
		printf("FAIL [multi-split]: expected 4 intersection vertices, got %d\n", undef_count);
		g_failures++;
	}

	/* Original vertices retain their own attributes untouched. */
	check_attrs_at("multi-split-bar0", tess, 0, 0, (float[]){10}, 1);
	check_attrs_at("multi-split-bar1", tess, 10, 0, (float[]){20}, 1);
	check_attrs_at("multi-split-bar2", tess, 10, 2, (float[]){30}, 1);
	check_attrs_at("multi-split-bar3", tess, 0, 2, (float[]){40}, 1);
	check_attrs_at("multi-split-tri0", tess, 2, -1, (float[]){1}, 1);
	check_attrs_at("multi-split-tri1", tess, 8, -1, (float[]){5}, 1);
	check_attrs_at("multi-split-tri2", tess, 5, 3, (float[]){9}, 1);

	/* First crossing (2.75,0): both contributing edges are unsplit, so the
	** value is the exact two-edge L1 weighted average = 7.875. */
	expect_first = expected_cross_attr(0, 0, 10, 10, 0, 20,
									   2, -1, 1, 5, 3, 9, 2.75f, 0);
	check_attrs_at("multi-split-first", tess, 2.75f, 0, &expect_first, 1);

	/* A later crossing (7.25,0) sits on an edge already split by the first
	** intersection, so its value must carry the earlier intersection's blended
	** attribute rather than recomputing from the original endpoints. There is
	** no simple closed form (the exact contributing sub-edge depends on the
	* internal mesh topology), so verify it deterministically differs from the
	* naive original-endpoint value while remaining a convex combination.
	* Exact per-vertex correctness on this same geometry is covered by the
	* affine-plane test below. */
	{
		float naive_later = expected_cross_attr(0, 0, 10, 10, 0, 20,
											  8, -1, 5, 5, 3, 9, 7.25f, 0);
		float got_later = attrs[find_vertex(verts, n, 2, 7.25f, 0)];
		if (fabsf(got_later - naive_later) <= 1e-3f) {
			printf("FAIL [multi-split-propagation]: crossing at (7.25,0) reused original-endpoint value %g instead of propagating the prior intersection blend\n",
				   (double)naive_later);
			g_failures++;
		}
		if (got_later < 1.0f - TESS_ATTR_EPS || got_later > 20.0f + TESS_ATTR_EPS) {
			printf("FAIL [multi-split-bounds]: crossing at (7.25,0) attr %g outside convex range [1,20]\n",
				   (double)got_later);
			g_failures++;
		}
	}

	/* The two top-edge crossings likewise stay within their contributing range. */
	{
		int vi;
		vi = find_vertex(verts, n, 2, 4.25f, 2);
		if (vi >= 0 && (attrs[vi] < 1.0f - TESS_ATTR_EPS || attrs[vi] > 40.0f + TESS_ATTR_EPS)) {
			printf("FAIL [multi-split-bounds]: crossing at (4.25,2) attr %g outside [1,40]\n",
				   (double)attrs[vi]);
			g_failures++;
		}
		vi = find_vertex(verts, n, 2, 5.75f, 2);
		if (vi >= 0 && (attrs[vi] < 1.0f - TESS_ATTR_EPS || attrs[vi] > 40.0f + TESS_ATTR_EPS)) {
			printf("FAIL [multi-split-bounds]: crossing at (5.75,2) attr %g outside [1,40]\n",
				   (double)attrs[vi]);
			g_failures++;
		}
	}

	tessDeleteTess(tess);
}

/*
** Affine attribute f(x,y) = 2x - 3y + 7 must be recovered exactly at every
** output vertex, including all generated intersections.
*/
static void test_affine_plane_recovered(void)
{
	static const TESSreal bar[] = {0, 0, 10, 0, 10, 2, 0, 2};
	static const TESSreal tri[] = {2, -1, 8, -1, 5, 3};
	TESSreal bar_attr[4], tri_attr[3];
	TESStesselator *tess = tessNewTess(NULL);
	const TESSreal *verts, *attrs;
	int i, n;

	for (i = 0; i < 4; ++i)
		bar_attr[i] = 2 * bar[i * 2] - 3 * bar[i * 2 + 1] + 7;
	for (i = 0; i < 3; ++i)
		tri_attr[i] = 2 * tri[i * 2] - 3 * tri[i * 2 + 1] + 7;

	tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
								 1, bar_attr, sizeof(TESSreal));
	tessAddContourWithAttributes(tess, 2, tri, 2 * sizeof(TESSreal), 3,
								 1, tri_attr, sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [affine]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}

	verts = tessGetVertices(tess);
	attrs = tessGetVertexAttributes(tess);
	n = tessGetVertexCount(tess);
	for (i = 0; i < n; ++i) {
		float x = verts[i * 2], y = verts[i * 2 + 1];
		float expect = 2 * x - 3 * y + 7;
		if (fabsf(attrs[i] - expect) > TESS_ATTR_EPS)
			fail_report("affine-plane", i, x, y, 0, expect, attrs[i]);
	}

	tessDeleteTess(tess);
}

/*
** Non-affine attribute g(x,y) = x^2 + y^2. At the bowtie midpoint (2,2),
** rebuilding the attribute from the coordinates would give 8, while the
** required edge-weight interpolation gives 16. This prevents an
** implementation from simply reconstructing attributes from positions.
*/
static void test_non_affine_attribute(void)
{
	static const TESSreal coords[] = {0, 0, 4, 4, 0, 4, 4, 0};
	TESSreal attrs[4];
	TESStesselator *tess = tessNewTess(NULL);
	int i;

	for (i = 0; i < 4; ++i)
		attrs[i] = coords[i * 2] * coords[i * 2] + coords[i * 2 + 1] * coords[i * 2 + 1];

	tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
								 1, attrs, sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [non-affine]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}

	/* Interpolation: 0.5 * midpoint(0,32) + 0.5 * midpoint(16,16) = 16. */
	check_attrs_at("non-affine-intersection", tess, 2, 2, (float[]){16}, 1);
	/* The coordinate-rebuilt value would be 8; make sure they differ. */
	{
		const TESSreal *out = tessGetVertexAttributes(tess);
		int vi = find_vertex(tessGetVertices(tess), tessGetVertexCount(tess), 2, 2, 2);
		if (vi >= 0 && fabsf(out[vi] - 8.0f) < 1e-3f) {
			printf("FAIL [non-affine]: intersection attribute was rebuilt from coordinates (8)\n");
			g_failures++;
		}
	}

	tessDeleteTess(tess);
}

/*
** The same attributed input through all three output layouts: attributes must
** line up item-by-item with tessGetVertices() in each of them, including the
** boundary contour layout which uses its own [base, count] vertex numbering.
*/
static void test_three_output_layouts(void)
{
	static const TESSreal bar[] = {0, 0, 10, 0, 10, 2, 0, 2};
	static const TESSreal bar_attr[][2] = {{10, -1}, {20, -2}, {30, -3}, {40, -4}};
	static const TESSreal tri[] = {2, -1, 8, -1, 5, 3};
	static const TESSreal tri_attr[][2] = {{1, 7}, {5, 7}, {9, 7}};
	static const struct { int element_type; const char *name; } layouts[] = {
		{TESS_POLYGONS, "polygons"},
		{TESS_CONNECTED_POLYGONS, "connected-polygons"},
		{TESS_BOUNDARY_CONTOURS, "boundary-contours"},
	};
	int li;

	for (li = 0; li < 3; ++li) {
		TESStesselator *tess = tessNewTess(NULL);
		char label[64];
		tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
									 2, bar_attr, 2 * sizeof(TESSreal));
		tessAddContourWithAttributes(tess, 2, tri, 2 * sizeof(TESSreal), 3,
									 2, tri_attr, 2 * sizeof(TESSreal));
		if (!tessTesselate(tess, TESS_WINDING_ODD, layouts[li].element_type, 3, 2, NULL)) {
			printf("FAIL [layout-%s]: tessellate failed, status %d\n",
				   layouts[li].name, (int)tessGetStatus(tess));
			g_failures++;
			tessDeleteTess(tess);
			continue;
		}
		if (tessGetVertexAttributeSize(tess) != 2 ||
			tessGetVertexAttributes(tess) == NULL) {
			printf("FAIL [layout-%s]: attribute query empty or wrong size\n", layouts[li].name);
			g_failures++;
		}

		snprintf(label, sizeof(label), "layout-%s-bar00", layouts[li].name);
		check_attrs_at(label, tess, 0, 0, (float[]){10, -1}, 2);
		snprintf(label, sizeof(label), "layout-%s-tri-apex", layouts[li].name);
		check_attrs_at(label, tess, 5, 3, (float[]){9, 7}, 2);
		snprintf(label, sizeof(label), "layout-%s-cross", layouts[li].name);
		{
			float e0 = expected_cross_attr(0, 0, 10, 10, 0, 20,
										   2, -1, 1, 5, 3, 9, 2.75f, 0);
			float e1 = expected_cross_attr(0, 0, -1, 10, 0, -2,
										   2, -1, 7, 5, 3, 7, 2.75f, 0);
			check_attrs_at(label, tess, 2.75f, 0, (float[]){e0, e1}, 2);
		}

		if (layouts[li].element_type == TESS_BOUNDARY_CONTOURS) {
			/* Every [base,count] range must address emitted, attribute-carrying
			* vertices rather than polygon-style indices. */
			const TESSindex *elems = tessGetElements(tess);
			int nelems = tessGetElementCount(tess);
			int e, total = 0;
			for (e = 0; e < nelems; ++e) {
				if (elems[e * 2] < 0 ||
					elems[e * 2] + elems[e * 2 + 1] > tessGetVertexCount(tess)) {
					printf("FAIL [layout-boundary]: contour range [%d,%d] out of bounds\n",
						   (int)elems[e * 2], (int)elems[e * 2 + 1]);
					g_failures++;
				}
				total += elems[e * 2 + 1];
			}
			if (total != tessGetVertexCount(tess)) {
				printf("FAIL [layout-boundary]: contour vertices %d != output vertices %d\n",
					   total, tessGetVertexCount(tess));
				g_failures++;
			}
		}
		tessDeleteTess(tess);
	}
}

/* Object reuse: successive cycles may change attributeSize, with no residue;
** a legacy-only cycle reports empty attribute queries. */
static void test_object_reuse_and_legacy(void)
{
	static const TESSreal coords[] = {0, 0, 4, 0, 4, 4, 0, 4};
	static const TESSreal attrs2[][2] = {{1, 9}, {2, 8}, {3, 7}, {4, 6}};
	static const TESSreal attrs1[] = {5, 6, 7, 8};
	TESStesselator *tess = tessNewTess(NULL);

	/* Cycle 1: two components. */
	tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
								 2, attrs2, 2 * sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [reuse-cycle1]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
	}
	if (tessGetVertexAttributeSize(tess) != 2) {
		printf("FAIL [reuse-cycle1]: attribute size %d, expected 2\n",
			   tessGetVertexAttributeSize(tess));
		g_failures++;
	}

	/* Cycle 2: one component -- previous configuration must not linger. */
	tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
								 1, attrs1, sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [reuse-cycle2]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
	}
	if (tessGetVertexAttributeSize(tess) != 1) {
		printf("FAIL [reuse-cycle2]: attribute size %d, expected 1\n",
			   tessGetVertexAttributeSize(tess));
		g_failures++;
	}
	check_attrs_at("reuse-cycle2-corner", tess, 4, 0, (float[]){6}, 1);

	/* Cycle 3: legacy entry point only -- queries are empty. */
	tessAddContour(tess, 2, coords, 2 * sizeof(TESSreal), 4);
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [reuse-cycle3]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
	}
	if (tessGetVertexAttributeSize(tess) != 0 ||
		tessGetVertexAttributes(tess) != NULL) {
		printf("FAIL [reuse-cycle3]: legacy cycle exposed attributes (size=%d, ptr=%p)\n",
			   tessGetVertexAttributeSize(tess), (const void *)tessGetVertexAttributes(tess));
		g_failures++;
	}

	tessDeleteTess(tess);
}

/* Strided input with padding between both coordinates and attributes, plus
** four attribute components. */
static void test_strided_padded_input(void)
{
	/* 3 floats of padding per coordinate set, 2 per attribute set. */
	static const TESSreal coords[4][4] = {
		{0, 0, 99, 99}, {4, 0, 99, 99}, {4, 4, 99, 99}, {0, 4, 99, 99}};
	static const TESSreal attrs[4][6] = {
		{1, 2, 3, 4, 99, 99}, {5, 6, 7, 8, 99, 99},
		{9, 10, 11, 12, 99, 99}, {13, 14, 15, 16, 99, 99}};
	TESStesselator *tess = tessNewTess(NULL);

	tessAddContourWithAttributes(tess, 2, coords, sizeof(coords[0]), 4,
								 4, attrs, sizeof(attrs[0]));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [strided]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}
	if (tessGetVertexAttributeSize(tess) != 4) {
		printf("FAIL [strided]: attribute size %d, expected 4\n",
			   tessGetVertexAttributeSize(tess));
		g_failures++;
	}
	check_attrs_at("strided-corner", tess, 4, 0, (float[]){5, 6, 7, 8}, 4);

	tessDeleteTess(tess);
}

/* Custom allocator: counts every allocation and can be forced to fail the
** attribute-specific allocations. */
typedef struct
{
	int allocs;
	int reallocs;
	int frees;
	int fail_realloc;
	size_t fail_alloc_size; /* 0 disables size-based failure */
	int fail_size_attempts; /* number of allocations matching fail_alloc_size */
} AllocCtl;

static void *ctl_alloc(void *user_data, unsigned int size)
{
	AllocCtl *ctl = (AllocCtl *)user_data;
	void *p;
	if (ctl->fail_alloc_size != 0 && (size_t)size == ctl->fail_alloc_size) {
		ctl->fail_size_attempts++;
		return NULL;
	}
	ctl->allocs++;
	p = malloc(size);
	return p;
}

static void *ctl_realloc(void *user_data, void *ptr, unsigned int size)
{
	AllocCtl *ctl = (AllocCtl *)user_data;
	void *p;
	if (ctl->fail_realloc)
		return NULL;
	ctl->reallocs++;
	p = realloc(ptr, size);
	if (p != NULL) {
		/* A successful realloc consumes the old block (realloc(NULL,...)
		* behaves like malloc and consumes nothing) and yields a new one. */
		if (ptr != NULL)
			ctl->frees++;
		ctl->allocs++;
	}
	return p;
}

static void ctl_free(void *user_data, void *ptr)
{
	AllocCtl *ctl = (AllocCtl *)user_data;
	ctl->frees++;
	free(ptr);
}

static void test_allocation_failures(void)
{
	static const TESSreal bar[] = {0, 0, 10, 0, 10, 2, 0, 2};
	static const TESSreal bar_attr2[][2] = {{1, 9}, {2, 8}, {3, 7}, {4, 6}};
	static const TESSreal bar_attr3[][3] = {{1, 9, 2}, {2, 8, 3}, {3, 7, 4}, {4, 6, 5}};
	static const TESSreal tri[] = {2, -1, 8, -1, 5, 3, 7, 0, 3, 1, 1, 4, 6, 2, 9, 5};
	static const TESSreal tri_attr2[8][2] = {0};
	static const TESSreal tri_attr3[8][3] = {0};
	TESSalloc allocator;
	AllocCtl ctl;
	TESStesselator *tess;
	int output_vertex_count, rc;
	size_t output_attr_bytes;

	memset(&allocator, 0, sizeof(allocator));
	memset(&ctl, 0, sizeof(ctl));
	allocator.memalloc = ctl_alloc;
	allocator.memrealloc = ctl_realloc;
	allocator.memfree = ctl_free;
	allocator.userData = &ctl;

	/* Reference run to learn the output vertex count, and thus the exact size
	** of the output attribute allocation (3 components keeps it distinct from
	** the vertex-index array size). */
	{
		TESStesselator *ref = tessNewTess(NULL);
		tessAddContourWithAttributes(ref, 2, bar, 2 * sizeof(TESSreal), 4,
									 3, bar_attr3, 3 * sizeof(TESSreal));
		tessAddContourWithAttributes(ref, 2, tri, 2 * sizeof(TESSreal), 8,
									 3, tri_attr3, 3 * sizeof(TESSreal));
		if (!tessTesselate(ref, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
			printf("FAIL [alloc-ref]: reference tessellate failed, status %d\n",
				   (int)tessGetStatus(ref));
			g_failures++;
			tessDeleteTess(ref);
			return;
		}
		output_vertex_count = tessGetVertexCount(ref);
		tessDeleteTess(ref);
	}
	output_attr_bytes = (size_t)output_vertex_count * 3 * sizeof(TESSreal);

	/* Failure 1: the input attribute buffer growth (memrealloc). The first
	** contour add must fail cleanly and leave no half-exposed attributes. */
	tess = tessNewTess(&allocator);
	ctl.fail_realloc = 1;
	rc = tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
									  2, bar_attr2, 2 * sizeof(TESSreal));
	if (rc != 0 || tessGetStatus(tess) != TESS_STATUS_OUT_OF_MEMORY) {
		printf("FAIL [alloc-input-growth]: rc=%d status=%d, expected failure/OOM\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
	}
	if (tessGetVertexAttributes(tess) != NULL ||
		tessGetVertexAttributeSize(tess) != 0) {
		printf("FAIL [alloc-input-growth]: attributes exposed after failure\n");
		g_failures++;
	}
	if (tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL) != 0) {
		printf("FAIL [alloc-input-growth]: tessellate succeeded after failed add\n");
		g_failures++;
	}
	tessDeleteTess(tess);
	if (ctl.allocs != ctl.frees) {
		printf("FAIL [alloc-input-growth]: alloc/free imbalance %d vs %d\n",
			   ctl.allocs, ctl.frees);
		g_failures++;
	}

	/* Failure 2: the output attribute array (memalloc of the exact size).
	** fail_size_attempts must be exactly one, proving this allocation is the
	** attribute output and nothing else. */
	memset(&ctl, 0, sizeof(ctl));
	ctl.fail_alloc_size = output_attr_bytes;
	tess = tessNewTess(&allocator);
	tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
								 3, bar_attr3, 3 * sizeof(TESSreal));
	tessAddContourWithAttributes(tess, 2, tri, 2 * sizeof(TESSreal), 8,
								 3, tri_attr3, 3 * sizeof(TESSreal));
	rc = tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL);
	if (rc != 0 || tessGetStatus(tess) != TESS_STATUS_OUT_OF_MEMORY) {
		printf("FAIL [alloc-output]: rc=%d status=%d, expected failure/OOM\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
	}
	if (ctl.fail_size_attempts != 1) {
		printf("FAIL [alloc-output]: expected exactly one failed allocation of %zu bytes, got %d\n",
			   output_attr_bytes, ctl.fail_size_attempts);
		g_failures++;
	}
	if (tessGetVertexAttributes(tess) != NULL ||
		tessGetVertexAttributeSize(tess) != 0) {
		printf("FAIL [alloc-output]: attributes exposed after failure\n");
		g_failures++;
	}
	tessDeleteTess(tess);
	if (ctl.allocs != ctl.frees) {
		printf("FAIL [alloc-output]: alloc/free imbalance %d vs %d\n",
			   ctl.allocs, ctl.frees);
		g_failures++;
	}

	/* Failure 3: growth fails on a later contour -- nothing half-exposed. */
	memset(&ctl, 0, sizeof(ctl));
	tess = tessNewTess(&allocator);
	tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
								 2, bar_attr2, 2 * sizeof(TESSreal));
	ctl.fail_realloc = 1;
	rc = tessAddContourWithAttributes(tess, 2, tri, 2 * sizeof(TESSreal), 8,
									  2, tri_attr2, 2 * sizeof(TESSreal));
	if (rc != 0 || tessGetStatus(tess) != TESS_STATUS_OUT_OF_MEMORY) {
		printf("FAIL [alloc-mid-growth]: rc=%d status=%d, expected failure/OOM\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
	}
	if (tessGetVertexAttributes(tess) != NULL ||
		tessGetVertexAttributeSize(tess) != 0) {
		printf("FAIL [alloc-mid-growth]: attributes exposed after failure\n");
		g_failures++;
	}
	tessDeleteTess(tess);
	if (ctl.allocs != ctl.frees) {
		printf("FAIL [alloc-mid-growth]: alloc/free imbalance %d vs %d\n",
			   ctl.allocs, ctl.frees);
		g_failures++;
	}
}

/* Invalid parameters and the unsupported same-cycle mixing of the two entry
** points must be rejected with a status readable through tessGetStatus(). */
static void test_invalid_and_mixed_usage(void)
{
	static const TESSreal coords[] = {0, 0, 4, 0, 4, 4, 0, 4};
	static const TESSreal attrs[] = {1, 2, 3, 4};
	TESStesselator *tess;
	int rc;

	/* attributeSize outside the 1..4 range. */
	tess = tessNewTess(NULL);
	rc = tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
									  0, attrs, sizeof(TESSreal));
	if (rc != 0 || tessGetStatus(tess) != TESS_STATUS_INVALID_INPUT) {
		printf("FAIL [invalid-size0]: rc=%d status=%d, expected 0/INVALID_INPUT\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
	}
	tessDeleteTess(tess);

	tess = tessNewTess(NULL);
	rc = tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
									  5, attrs, 5 * sizeof(TESSreal));
	if (rc != 0 || tessGetStatus(tess) != TESS_STATUS_INVALID_INPUT) {
		printf("FAIL [invalid-size5]: rc=%d status=%d, expected 0/INVALID_INPUT\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
	}
	tessDeleteTess(tess);

	/* Legacy first, then the attributed entry within the same cycle. */
	tess = tessNewTess(NULL);
	tessAddContour(tess, 2, coords, 2 * sizeof(TESSreal), 4);
	rc = tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
									  1, attrs, sizeof(TESSreal));
	if (rc != 0 || tessGetStatus(tess) != TESS_STATUS_INVALID_INPUT) {
		printf("FAIL [mixed-legacy-first]: rc=%d status=%d, expected 0/INVALID_INPUT\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
	}
	if (tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL) != 0) {
		printf("FAIL [mixed-legacy-first]: tessellate succeeded after rejected mixed add\n");
		g_failures++;
	}
	tessDeleteTess(tess);

	/* Attributed first, then the legacy entry within the same cycle. */
	tess = tessNewTess(NULL);
	rc = tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
									  1, attrs, sizeof(TESSreal));
	if (rc != 1) {
		printf("FAIL [mixed-attrs-first]: first add rc=%d status=%d\n",
			   rc, (int)tessGetStatus(tess));
		g_failures++;
	}
	tessAddContour(tess, 2, coords, 2 * sizeof(TESSreal), 4);
	if (tessGetStatus(tess) != TESS_STATUS_INVALID_INPUT) {
		printf("FAIL [mixed-attrs-first]: expected INVALID_INPUT after legacy add, got status %d\n",
			   (int)tessGetStatus(tess));
		g_failures++;
	}
	if (tessGetVertexAttributes(tess) != NULL ||
		tessGetVertexAttributeSize(tess) != 0) {
		printf("FAIL [mixed-attrs-first]: output attributes exposed before tessellation\n");
		g_failures++;
	}
	tessDeleteTess(tess);

	/* A rejected first contour of a new cycle must not clobber the still-valid
	** output of the previous cycle. */
	tess = tessNewTess(NULL);
	tessAddContourWithAttributes(tess, 2, coords, 2 * sizeof(TESSreal), 4,
								 1, attrs, sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [prev-output]: setup tessellate failed, status %d\n",
			   (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}
	{
		static const TESSreal bad[] = {0, 0, 4, 0, 4, 4, 1e30f, 4};
		int prev_count = tessGetVertexCount(tess);
		const TESSreal *prev_attrs = tessGetVertexAttributes(tess);
		tessAddContour(tess, 2, bad, 2 * sizeof(TESSreal), 4);
		if (tessGetStatus(tess) != TESS_STATUS_INVALID_INPUT ||
			tessGetVertexCount(tess) != prev_count ||
			tessGetVertexAttributes(tess) != prev_attrs ||
			tessGetVertexAttributeSize(tess) != 1) {
			printf("FAIL [prev-output]: failed new-cycle add disturbed previous output (status=%d)\n",
				   (int)tessGetStatus(tess));
			g_failures++;
		}
	}
	tessDeleteTess(tess);
}

/* The contour orientation option and winding rules keep working, and the
** attributes stay attached under them. (A reversed contour under the odd rule
** still fills the same region; negative winding of a single simple contour
** legitimately produces no interior, matching the legacy entry point.) */
static void test_winding_and_reverse(void)
{
	static const TESSreal bar[] = {0, 0, 10, 0, 10, 2, 0, 2};
	static const TESSreal bar_attr[] = {10, 20, 30, 40};
	TESStesselator *tess = tessNewTess(NULL);

	tessSetOption(tess, TESS_REVERSE_CONTOURS, 1);
	tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
								 1, bar_attr, sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [winding]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}
	if (tessGetElementCount(tess) < 1) {
		printf("FAIL [winding]: expected output elements, got %d\n",
			   tessGetElementCount(tess));
		g_failures++;
	}
	check_attrs_at("winding-reverse-corner", tess, 10, 2, (float[]){30}, 1);
	check_attrs_at("winding-reverse-origin", tess, 0, 0, (float[]){10}, 1);
	tessDeleteTess(tess);
}

/* Renders the triangulated output as grayscale PPM, using the interpolated
** attribute as the gray value. */
static void write_ppm(const char *path, TESStesselator *tess)
{
	static const int W = 160, H = 160;
	const TESSreal *verts = tessGetVertices(tess);
	const TESSreal *attrs = tessGetVertexAttributes(tess);
	const TESSindex *elems = tessGetElements(tess);
	int nelems = tessGetElementCount(tess);
	unsigned char *pixels;
	float min_a = 1e30f, max_a = -1e30f, xmin, xmax, ymin, ymax, range;
	int i;
	FILE *f;

	for (i = 0; i < tessGetVertexCount(tess); ++i) {
		float a = attrs[i];
		if (a < min_a) min_a = a;
		if (a > max_a) max_a = a;
	}
	range = max_a - min_a;
	if (range < 1e-6f) range = 1.0f;

	xmin = ymin = 1e30f;
	xmax = ymax = -1e30f;
	for (i = 0; i < tessGetVertexCount(tess); ++i) {
		float vx = verts[i * 2], vy = verts[i * 2 + 1];
		if (vx < xmin) xmin = vx;
		if (vx > xmax) xmax = vx;
		if (vy < ymin) ymin = vy;
		if (vy > ymax) ymax = vy;
	}

	pixels = (unsigned char *)malloc((size_t)W * H);
	memset(pixels, 255, (size_t)W * H);

	for (i = 0; i < nelems; ++i) {
		const TESSindex *tri = &elems[i * 3];
		int ia = tri[0], ib = tri[1], ic = tri[2];
		float ax = verts[ia * 2], ay = verts[ia * 2 + 1];
		float bx = verts[ib * 2], by = verts[ib * 2 + 1];
		float cx = verts[ic * 2], cy = verts[ic * 2 + 1];
		float denom = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
		int py, pxx;
		if (fabsf(denom) < 1e-12f) continue;
		for (py = 0; py < H; ++py) {
			for (pxx = 0; pxx < W; ++pxx) {
				float px = xmin + (xmax - xmin) * (pxx + 0.5f) / W;
				float pyy = ymin + (ymax - ymin) * (py + 0.5f) / H;
				float wa = ((by - cy) * (px - cx) + (cx - bx) * (pyy - cy)) / denom;
				float wb = ((cy - ay) * (px - cx) + (ax - cx) * (pyy - cy)) / denom;
				float wc = 1.0f - wa - wb;
				float gray;
				if (wa < -1e-4f || wb < -1e-4f || wc < -1e-4f) continue;
				gray = wa * attrs[ia] + wb * attrs[ib] + wc * attrs[ic];
				pixels[(H - 1 - py) * W + pxx] =
					(unsigned char)(255.0f * (gray - min_a) / range);
			}
		}
	}

	f = fopen(path, "wb");
	if (f == NULL) {
		printf("FAIL [ppm]: cannot open %s\n", path);
		g_failures++;
		free(pixels);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (i = 0; i < W * H; ++i) {
		unsigned char rgb[3] = {pixels[i], pixels[i], pixels[i]};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	free(pixels);
}

static void test_ppm_scene(const char *path)
{
	static const TESSreal bar[] = {0, 0, 10, 0, 10, 2, 0, 2};
	static const TESSreal tri[] = {2, -1, 8, -1, 5, 3};
	TESSreal bar_attr[4], tri_attr[3];
	TESStesselator *tess = tessNewTess(NULL);
	int i;

	for (i = 0; i < 4; ++i)
		bar_attr[i] = bar[i * 2] + 2 * bar[i * 2 + 1];
	for (i = 0; i < 3; ++i)
		tri_attr[i] = tri[i * 2] + 2 * tri[i * 2 + 1];

	tessAddContourWithAttributes(tess, 2, bar, 2 * sizeof(TESSreal), 4,
								 1, bar_attr, sizeof(TESSreal));
	tessAddContourWithAttributes(tess, 2, tri, 2 * sizeof(TESSreal), 3,
								 1, tri_attr, sizeof(TESSreal));
	if (!tessTesselate(tess, TESS_WINDING_ODD, TESS_POLYGONS, 3, 2, NULL)) {
		printf("FAIL [ppm]: tessellate failed, status %d\n", (int)tessGetStatus(tess));
		g_failures++;
		tessDeleteTess(tess);
		return;
	}
	write_ppm(path, tess);
	tessDeleteTess(tess);
}

int main(int argc, char **argv)
{
	const char *ppm_path = (argc > 1) ? argv[1] : "check_attributes.ppm";

	test_bowtie_basic_intersection();
	test_edge_split_multiple_times();
	test_affine_plane_recovered();
	test_non_affine_attribute();
	test_three_output_layouts();
	test_object_reuse_and_legacy();
	test_strided_padded_input();
	test_allocation_failures();
	test_invalid_and_mixed_usage();
	test_winding_and_reverse();
	test_ppm_scene(ppm_path);

	if (g_failures == 0) {
		printf("all attribute tests passed; grayscale PPM written to %s\n", ppm_path);
		return 0;
	}
	printf("%d attribute test check(s) failed\n", g_failures);
	return 1;
}
