//=====================================================================
//
// radiosity.c - progressive radiosity with a hemicube visibility pass
//
// The transfer matrix is built by projecting the scene onto the five
// faces of a hemicube placed at the area centroid of every source
// patch.  The projection reuses the original Mini3D transform,
// trapezoid split and scanline machinery (mini3d.h) but writes into
// private primitive-ID and projected-depth targets instead of the
// caller's framebuffer, so no main-camera buffer is ever borrowed.
//
//=====================================================================
#include "radiosity.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RADIOSITY_PI 3.14159265358979323846f

struct radiosity_scene {
	size_t count;               // number of patches
	radiosity_patch *patches;   // deep copy of the input geometry
	point_t *normals;           // unit outward normal of every patch
	point_t *centroids;         // area centroid of every patch
	float *areas;               // triangle area of every patch
	float *F;                   // count * count transfer matrix, row = source
	color_t *B;                 // accumulated radiosity per patch
	color_t *U;                 // unshot radiosity per patch
	size_t last;                // last shooting patch (SIZE_MAX: none)
};

//---------------------------------------------------------------------
// small vector helpers (the public ones are non-static in mini3d)
//---------------------------------------------------------------------
static point_t vec3(float x, float y, float z) {
	point_t p;
	p.x = x; p.y = y; p.z = z; p.w = 1.0f;
	return p;
}

static point_t vec_add(const point_t *a, const point_t *b) {
	return vec3(a->x + b->x, a->y + b->y, a->z + b->z);
}

static point_t vec_sub(const point_t *a, const point_t *b) {
	return vec3(a->x - b->x, a->y - b->y, a->z - b->z);
}

static point_t vec_scale(const point_t *a, float s) {
	return vec3(a->x * s, a->y * s, a->z * s);
}

static float vec_dot(const point_t *a, const point_t *b) {
	return a->x * b->x + a->y * b->y + a->z * b->z;
}

static point_t vec_cross(const point_t *a, const point_t *b) {
	return vec3(a->y * b->z - a->z * b->y,
		a->z * b->x - a->x * b->z,
		a->x * b->y - a->y * b->x);
}

static float vec_len(const point_t *a) {
	return (float)sqrt((double)vec_dot(a, a));
}

static point_t vec_norm(const point_t *a) {
	float l = vec_len(a);
	if (l > 0.0f) return vec_scale(a, 1.0f / l);
	return vec3(0.0f, 0.0f, 0.0f);
}

//---------------------------------------------------------------------
// hemicube targets: private ID / projected-depth buffers
//---------------------------------------------------------------------
typedef struct {
	int res;
	int *id;        // -1 means background
	float *depth;   // rhw = 1/w, larger is closer
} hemi_target;

static void hemi_clear(hemi_target *t) {
	size_t n = (size_t)t->res * (size_t)t->res;
	size_t k;
	for (k = 0; k < n; k++) {
		t->id[k] = -1;
		t->depth[k] = 0.0f;
	}
}

// Camera-space clip planes, evaluated as f >= 0 for inside:
//   0 near:  z - zn
//   1 far:   zf - z
//   2 left:  x + z
//   3 right: z - x
//   4 bottom:y + z
//   5 top:   z - y
static float clip_eval(const vector_t *v, int plane, float zn, float zf) {
	switch (plane) {
	case 0: return v->z - zn;
	case 1: return zf - v->z;
	case 2: return v->x + v->z;
	case 3: return v->z - v->x;
	case 4: return v->y + v->z;
	default: return v->z - v->y;
	}
}

// Sutherland-Hodgman clip of one convex polygon against one plane.
static int clip_plane(const vector_t *in, int n, vector_t *out,
		int plane, float zn, float zf) {
	int a, m = 0;
	for (a = 0; a < n; a++) {
		const vector_t *p = &in[a];
		const vector_t *q = &in[(a + 1) % n];
		float fp = clip_eval(p, plane, zn, zf);
		float fq = clip_eval(q, plane, zn, zf);
		int ip = (fp >= 0.0f);
		int iq = (fq >= 0.0f);
		if (ip) out[m++] = *p;
		if (ip != iq) {
			float t = fp / (fp - fq);
			out[m].x = p->x + (q->x - p->x) * t;
			out[m].y = p->y + (q->y - p->y) * t;
			out[m].z = p->z + (q->z - p->z) * t;
			out[m].w = 1.0f;
			m++;
		}
	}
	return m;
}

// Depth resolution.  `rhw` is 1/w and is monotonically increasing as the
// fragment gets nearer, so a larger value wins.  When two fragments are
// at the same depth, the smaller original patch index wins; a tiny
// relative epsilon absorbs the float noise between coplanar surfaces.
static int hemi_depth_wins(float rhw, int id, float depth, int cur) {
	float eps = 1e-6f * (fabsf(rhw) + fabsf(depth)) + 1e-9f;
	if (rhw > depth + eps) return 1;
	if (rhw < depth - eps) return 0;
	return (cur < 0) || (id < cur);
}

// Rasterize one trapezoid into the ID / depth target, sampling at pixel
// centers.  The trapezoid split and the edge interpolation come from
// the original Mini3D chain.
static void hemi_render_trap(hemi_target *t, const trapezoid_t *trap, int id) {
	int res = t->res;
	int j0 = (int)ceilf(trap->top - 0.5f);
	int j1 = (int)ceilf(trap->bottom - 0.5f);
	int j;
	for (j = j0; j < j1; j++) {
		float y = (float)j + 0.5f;
		trapezoid_t row;
		float lx, rx;
		int x0, x1, x;
		if (j < 0 || j >= res) continue;
		if (y < trap->top || y >= trap->bottom) continue;
		row = *trap;
		trapezoid_edge_interp(&row, y);
		lx = row.left.v.pos.x;
		rx = row.right.v.pos.x;
		if (!(rx > lx)) continue;
		x0 = (int)ceilf(lx - 0.5f);
		x1 = (int)ceilf(rx - 0.5f);
		for (x = x0; x < x1; x++) {
			float px = (float)x + 0.5f;
			float s, rhw;
			size_t k;
			if (x < 0 || x >= res) continue;
			if (px < lx || px >= rx) continue;
			s = (px - lx) / (rx - lx);
			rhw = row.left.v.rhw + (row.right.v.rhw - row.left.v.rhw) * s;
			k = (size_t)j * (size_t)res + (size_t)x;
			// larger rhw is nearer; on an exact depth tie the smaller
			// original patch index wins.
			if (hemi_depth_wins(rhw, id, t->depth[k], t->id[k])) {
				t->depth[k] = rhw;
				t->id[k] = id;
			}
		}
	}
}

// Clip, project and rasterize one world-space triangle.
static void hemi_draw_triangle(hemi_target *t, const transform_t *ts,
		const point_t tri[3], int id, float zn, float zf) {
	vector_t poly[16], tmp[16];
	vertex_t vv[16];
	int n = 3, plane, k;
	for (k = 0; k < 3; k++) {
		matrix_apply(&poly[k], &tri[k], &ts->view);
		poly[k].w = 1.0f;
	}
	for (plane = 0; plane < 6 && n >= 3; plane++) {
		n = clip_plane(poly, n, tmp, plane, zn, zf);
		memcpy(poly, tmp, sizeof(vector_t) * (size_t)n);
	}
	if (n < 3) return;
	for (k = 0; k < n; k++) {
		vector_t clip, scr;
		matrix_apply(&clip, &poly[k], &ts->projection);
		if (!(clip.w > 0.0f)) return;
		transform_homogenize(ts, &scr, &clip);
		vv[k].pos = scr;
		vv[k].pos.w = clip.w;
		vv[k].tc.u = 0.0f;
		vv[k].tc.v = 0.0f;
		vv[k].color.r = 0.0f;
		vv[k].color.g = 0.0f;
		vv[k].color.b = 0.0f;
		vv[k].rhw = 1.0f / clip.w;
	}
	for (k = 1; k + 1 < n; k++) {
		trapezoid_t traps[2];
		int nt = trapezoid_init_triangle(traps, &vv[0], &vv[k], &vv[k + 1]);
		if (nt >= 1) hemi_render_trap(t, &traps[0], id);
		if (nt >= 2) hemi_render_trap(t, &traps[1], id);
	}
}

//---------------------------------------------------------------------
// hemicube weights
//---------------------------------------------------------------------
// The top face covers x,y in [-1,1]; a side face covers x in [-1,1] and
// z in [0,1] (the upper half of the projected square).  Every weight of
// all five faces is normalized by one common discrete total.
static double hemi_total_weight(int res) {
	double dx = 2.0 / (double)res;
	double sum = 0.0;
	int px, py;
	for (py = 0; py < res; py++) {
		double y = 1.0 - (2.0 * (double)py + 1.0) * dx * 0.5;
		for (px = 0; px < res; px++) {
			double x = -1.0 + (2.0 * (double)px + 1.0) * dx * 0.5;
			double d = 1.0 + x * x + y * y;
			sum += dx * dx / (M_PI * d * d);
		}
	}
	{
		double side = 0.0;
		int half = res / 2;
		for (py = 0; py < half; py++) {
			double z = 1.0 - (2.0 * (double)py + 1.0) * dx * 0.5;
			for (px = 0; px < res; px++) {
				double x = -1.0 + (2.0 * (double)px + 1.0) * dx * 0.5;
				double d = 1.0 + x * x + z * z;
				side += dx * dx * z / (M_PI * d * d);
			}
		}
		sum += 4.0 * side;
	}
	return sum;
}

// One hemicube view: direction d, screen-right axis and screen-up axis.
typedef struct {
	point_t dir;
	point_t right;
	point_t up;
	int top;        // 1 = top face (full square), 0 = side (upper half)
} hemi_view;

//---------------------------------------------------------------------
// creation / validation
//---------------------------------------------------------------------
static int patch_valid(const radiosity_patch *p) {
	int k;
	for (k = 0; k < 3; k++) {
		if (!isfinite(p->v[k].x) || !isfinite(p->v[k].y) ||
				!isfinite(p->v[k].z))
			return 0;
		if (p->v[k].w != 1.0f) return 0;
	}
	if (!(p->reflectance.r >= 0.0f && p->reflectance.r < 1.0f)) return 0;
	if (!(p->reflectance.g >= 0.0f && p->reflectance.g < 1.0f)) return 0;
	if (!(p->reflectance.b >= 0.0f && p->reflectance.b < 1.0f)) return 0;
	if (!(p->emission.r >= 0.0f) || !isfinite(p->emission.r)) return 0;
	if (!(p->emission.g >= 0.0f) || !isfinite(p->emission.g)) return 0;
	if (!(p->emission.b >= 0.0f) || !isfinite(p->emission.b)) return 0;
	return 1;
}

static void scene_free(radiosity_scene *s) {
	if (!s) return;
	free(s->patches);
	free(s->normals);
	free(s->centroids);
	free(s->areas);
	free(s->F);
	free(s->B);
	free(s->U);
	free(s);
}

// Build the unit normal, centroid and area of every patch.  Returns 0 on
// a degenerate patch.
static int scene_prepare(radiosity_scene *s) {
	size_t i;
	for (i = 0; i < s->count; i++) {
		const point_t *a = &s->patches[i].v[0];
		const point_t *b = &s->patches[i].v[1];
		const point_t *c = &s->patches[i].v[2];
		point_t e1 = vec_sub(b, a);
		point_t e2 = vec_sub(c, a);
		point_t cr = vec_cross(&e1, &e2);
		float crl = vec_len(&cr);
		float l1 = vec_len(&e1);
		float l2 = vec_len(&e2);
		float scale = (l1 > l2) ? l1 : l2;
		if (!isfinite(crl) || !(crl > 1e-12f * scale * scale)) return 0;
		s->normals[i] = vec_scale(&cr, 1.0f / crl);
		{
			point_t sum = vec_add(a, b);
			sum = vec_add(&sum, c);
			s->centroids[i] = vec_scale(&sum, 1.0f / 3.0f);
		}
		s->areas[i] = crl * 0.5f;
	}
	return 1;
}

// Cast the scene onto the five hemicube faces of source patch `src` and
// accumulate the transfer row.
static void scene_cast_row(radiosity_scene *s, size_t src, float offset,
		hemi_target *ht, double *acc, double total) {
	point_t n = s->normals[src];
	point_t aux, t, b, eye;
	hemi_view views[5];
	float zn = offset * 0.25f;
	float zf;
	float maxd = 0.0f;
	double dx = 2.0 / (double)ht->res;
	int v, px, py;
	size_t i, k;

	if (fabsf(n.y) < 0.9f) aux = vec3(0.0f, 1.0f, 0.0f);
	else aux = vec3(1.0f, 0.0f, 0.0f);
	{
		point_t aux_n = vec_cross(&aux, &n);
		t = vec_norm(&aux_n);
	}
	b = vec_cross(&n, &t);

	views[0].dir = n;
	views[0].right = t;
	views[0].up = b;
	views[0].top = 1;
	views[1].dir = t;
	views[2].dir = vec_scale(&t, -1.0f);
	views[3].dir = b;
	views[4].dir = vec_scale(&b, -1.0f);
	for (v = 1; v < 5; v++) {
		views[v].right = vec_cross(&n, &views[v].dir);
		views[v].up = n;
		views[v].top = 0;
	}

	{
		point_t off = vec_scale(&n, offset);
		eye = vec_add(&s->centroids[src], &off);
	}
	for (i = 0; i < s->count; i++) {
		if (i == src) continue;
		for (k = 0; k < 3; k++) {
			point_t d = vec_sub(&s->patches[i].v[k], &eye);
			float l = vec_len(&d);
			if (l > maxd) maxd = l;
		}
	}
	zf = ((maxd > offset) ? maxd : offset) + offset;

	for (v = 0; v < 5; v++) {
		transform_t ts;
		int half = ht->res / 2;
		int rows = views[v].top ? ht->res : half;

		hemi_clear(ht);

		matrix_set_identity(&ts.world);
		{
			point_t at = vec_add(&eye, &views[v].dir);
			matrix_set_lookat(&ts.view, &eye, &at, &views[v].up);
		}
		matrix_set_perspective(&ts.projection,
			RADIOSITY_PI * 0.5f, 1.0f, zn, zf);
		transform_update(&ts);
		ts.w = (float)ht->res;
		ts.h = (float)ht->res;

		for (i = 0; i < s->count; i++) {
			if (i == src) continue;
			hemi_draw_triangle(ht, &ts, s->patches[i].v, (int)i, zn, zf);
		}

		for (py = 0; py < rows; py++) {
			double second = 1.0 - (2.0 * (double)py + 1.0) * dx * 0.5;
			for (px = 0; px < ht->res; px++) {
				double first = -1.0 + (2.0 * (double)px + 1.0) * dx * 0.5;
				size_t idx = (size_t)py * (size_t)ht->res + (size_t)px;
				int id = ht->id[idx];
				double w;
				point_t dir;
				if (id < 0) continue;
				if (views[v].top) {
					double d = 1.0 + first * first + second * second;
					w = dx * dx / (M_PI * d * d);
				}	else {
					double d = 1.0 + first * first + second * second;
					w = dx * dx * second / (M_PI * d * d);
				}
				{
					point_t a1 = vec_scale(&views[v].right, (float)first);
					point_t a2 = vec_scale(&views[v].up, (float)second);
					point_t a12 = vec_add(&a1, &a2);
					dir = vec_add(&views[v].dir, &a12);
				}
				// only the front face of a receiver gets transport;
				// a nearest back face has already absorbed the ray.
				if (vec_dot(&dir, &s->normals[id]) < 0.0f)
					acc[id] += w / total;
			}
		}
	}
}

radiosity_scene *radiosity_create(const radiosity_patch *patches,
		size_t count, int resolution, float offset) {
	radiosity_scene *s;
	size_t res2, i, j;
	hemi_target ht;
	double total;
	double *acc = NULL;

	if (!patches || count == 0) return NULL;
	if (resolution < 4 || (resolution & 1)) return NULL;
	if (!(offset > 0.0f) || !isfinite(offset)) return NULL;

	for (i = 0; i < count; i++) {
		if (!patch_valid(&patches[i])) return NULL;
	}

	s = (radiosity_scene*)calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->count = count;
	s->last = (size_t)-1;
	s->patches = (radiosity_patch*)malloc(sizeof(radiosity_patch) * count);
	s->normals = (point_t*)malloc(sizeof(point_t) * count);
	s->centroids = (point_t*)malloc(sizeof(point_t) * count);
	s->areas = (float*)malloc(sizeof(float) * count);
	s->F = (float*)calloc(count * count, sizeof(float));
	s->B = (color_t*)malloc(sizeof(color_t) * count);
	s->U = (color_t*)malloc(sizeof(color_t) * count);
	if (!s->patches || !s->normals || !s->centroids || !s->areas ||
			!s->F || !s->B || !s->U) {
		scene_free(s);
		return NULL;
	}
	memcpy(s->patches, patches, sizeof(radiosity_patch) * count);
	if (!scene_prepare(s)) {
		scene_free(s);
		return NULL;
	}
	for (i = 0; i < count; i++) {
		s->B[i] = patches[i].emission;
		s->U[i] = patches[i].emission;
	}

	res2 = (size_t)resolution * (size_t)resolution;
	ht.res = resolution;
	ht.id = (int*)malloc(sizeof(int) * res2);
	ht.depth = (float*)malloc(sizeof(float) * res2);
	acc = (double*)malloc(sizeof(double) * count);
	if (!ht.id || !ht.depth || !acc) {
		free(ht.id);
		free(ht.depth);
		free(acc);
		scene_free(s);
		return NULL;
	}

	total = hemi_total_weight(resolution);
	for (i = 0; i < count; i++) {
		memset(acc, 0, sizeof(double) * count);
		scene_cast_row(s, i, offset, &ht, acc, total);
		// row sums cannot exceed one physically; guard against float
		// rounding pushing the normalized weights just above it.
		{
			double rsum = 0.0;
			for (j = 0; j < count; j++) rsum += acc[j];
			if (rsum > 1.0) {
				double inv = 1.0 / rsum;
				for (j = 0; j < count; j++) acc[j] *= inv;
			}
		}
		for (j = 0; j < count; j++) {
			s->F[i * count + j] = (j == i) ? 0.0f : (float)acc[j];
		}
	}

	free(ht.id);
	free(ht.depth);
	free(acc);
	return s;
}

void radiosity_destroy(radiosity_scene *scene) {
	scene_free(scene);
}

//---------------------------------------------------------------------
// matrix query
//---------------------------------------------------------------------
float radiosity_factor(const radiosity_scene *scene, size_t i, size_t j) {
	if (!scene || i >= scene->count || j >= scene->count) return 0.0f;
	if (i == j) return 0.0f;
	return scene->F[i * scene->count + j];
}

//---------------------------------------------------------------------
// progressive shooting
//---------------------------------------------------------------------
size_t radiosity_step(radiosity_scene *scene, size_t shots) {
	size_t done = 0;
	if (!scene) return 0;
	while (done < shots) {
		size_t i, best = (size_t)-1;
		float best_energy = 0.0f;
		color_t shot;
		double ratio;
		for (i = 0; i < scene->count; i++) {
			float e = scene->areas[i] *
				(scene->U[i].r + scene->U[i].g + scene->U[i].b);
			if (e > best_energy) {
				best_energy = e;
				best = i;
			}
		}
		if (best == (size_t)-1 || !(best_energy > 0.0f)) break;
		shot = scene->U[best];
		scene->U[best].r = 0.0f;
		scene->U[best].g = 0.0f;
		scene->U[best].b = 0.0f;
		for (i = 0; i < scene->count; i++) {
			float f;
			double add_r, add_g, add_b;
			if (i == best) continue;
			f = scene->F[best * scene->count + i];
			if (!(f > 0.0f)) continue;
			ratio = (double)scene->areas[best] /
				(double)scene->areas[i];
			add_r = (double)scene->patches[i].reflectance.r *
				(double)shot.r * ratio * (double)f;
			add_g = (double)scene->patches[i].reflectance.g *
				(double)shot.g * ratio * (double)f;
			add_b = (double)scene->patches[i].reflectance.b *
				(double)shot.b * ratio * (double)f;
			scene->B[i].r += (float)add_r;
			scene->B[i].g += (float)add_g;
			scene->B[i].b += (float)add_b;
			scene->U[i].r += (float)add_r;
			scene->U[i].g += (float)add_g;
			scene->U[i].b += (float)add_b;
		}
		scene->last = best;
		done++;
	}
	return done;
}

int radiosity_get(const radiosity_scene *scene, size_t i,
		color_t *B, color_t *U, float *area) {
	if (!scene || i >= scene->count) return 0;
	if (B) *B = scene->B[i];
	if (U) *U = scene->U[i];
	if (area) *area = scene->areas[i];
	return 1;
}

size_t radiosity_last_shooter(const radiosity_scene *scene) {
	if (!scene) return (size_t)-1;
	return scene->last;
}

void radiosity_reset(radiosity_scene *scene) {
	size_t i;
	if (!scene) return;
	for (i = 0; i < scene->count; i++) {
		scene->B[i] = scene->patches[i].emission;
		scene->U[i] = scene->patches[i].emission;
	}
	scene->last = (size_t)-1;
}

//---------------------------------------------------------------------
// drawing
//---------------------------------------------------------------------
void radiosity_draw(const radiosity_scene *scene, device_t *device,
		float exposure) {
	transform_t saved;
	int saved_state;
	size_t i;
	if (!scene || !device) return;
	saved = device->transform;
	saved_state = device->render_state;

	matrix_set_identity(&device->transform.world);
	transform_update(&device->transform);
	device->render_state = RENDER_STATE_COLOR;
	for (i = 0; i < scene->count; i++) {
		vertex_t v[3];
		int k;
		color_t c;
		c.r = scene->B[i].r * exposure;
		c.g = scene->B[i].g * exposure;
		c.b = scene->B[i].b * exposure;
		for (k = 0; k < 3; k++) {
			v[k].pos = scene->patches[i].v[k];
			v[k].tc.u = 0.0f;
			v[k].tc.v = 0.0f;
			v[k].color = c;
			v[k].rhw = 1.0f;
		}
		device_draw_primitive(device, &v[0], &v[1], &v[2]);
	}
	device->transform = saved;
	device->render_state = saved_state;
}
