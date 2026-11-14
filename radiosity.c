//=====================================================================
//
// radiosity.c - hemicube visibility driven progressive radiosity
//
// The five hemicube views reuse the original mini3d transform,
// trapezoid splitting and scanline stepping. Only the final pixel
// writer is replaced by independent primitive-id and projection
// depth targets, one cleared buffer pair per source patch per view.
//
//=====================================================================
#include "radiosity.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RAD_PI 3.14159265358979323846f

struct radiosity_scene {
	size_t count;
	int resolution;
	float offset;
	radiosity_patch *patches;   // deep copied input geometry
	color_t *B;                 // accumulated radiosity
	color_t *U;                 // unshot radiosity
	float *area;                // patch areas
	vector_t *normal;           // unit outward normals
	point_t *centroid;          // area centroids
	float *form;                // transport matrix, count * count, row major
	size_t last_shooter;        // most recent shooter, SIZE_MAX if none
};

//=====================================================================
// small helpers
//=====================================================================
static int rad_finite_v(const vector_t *v) {
	return isfinite(v->x) && isfinite(v->y) && isfinite(v->z) && isfinite(v->w);
}

static int rad_finite_c(const color_t *c) {
	return isfinite(c->r) && isfinite(c->g) && isfinite(c->b);
}

static void vec_set(vector_t *v, float x, float y, float z) {
	v->x = x; v->y = y; v->z = z; v->w = 1.0f;
}

//=====================================================================
// hemicube cell weights, normalized by one single discrete sum shared
// by every face of every source patch
//=====================================================================
typedef struct {
	int resolution;
	float cell;                 // common cell width 2 / resolution
	float inv_total;            // 1 / sum of every raw cell weight
} hemicube_weights;

static float raw_top(const hemicube_weights *hw, int px, int py) {
	float r = hw->resolution;
	float x = -1.0f + ((float)px + 0.5f) * hw->cell;
	float y = 1.0f - ((float)py + 0.5f) * hw->cell;
	float d = 1.0f + x * x + y * y;
	(void)r;
	return hw->cell * hw->cell / (RAD_PI * d * d);
}

static float raw_side(const hemicube_weights *hw, int px, int py) {
	float x = -1.0f + ((float)px + 0.5f) * hw->cell;
	float z = 1.0f - ((float)py + 0.5f) * hw->cell;
	float d = 1.0f + x * x + z * z;
	return hw->cell * hw->cell * z / (RAD_PI * d * d);
}

static void weights_init(hemicube_weights *hw, int resolution) {
	int half = resolution / 2;
	int px, py;
	float total = 0.0f;
	hw->resolution = resolution;
	hw->cell = 2.0f / (float)resolution;
	for (py = 0; py < resolution; py++)
		for (px = 0; px < resolution; px++)
			total += raw_top(hw, px, py);
	for (py = 0; py < half; py++)
		for (px = 0; px < resolution; px++)
			total += 4.0f * raw_side(hw, px, py);
	hw->inv_total = 1.0f / total;
}

static float weight_top(const hemicube_weights *hw, int px, int py) {
	return raw_top(hw, px, py) * hw->inv_total;
}

static float weight_side(const hemicube_weights *hw, int px, int py) {
	return raw_side(hw, px, py) * hw->inv_total;
}

//=====================================================================
// homogeneous clip-space polygon clipping (Sutherland-Hodgman) so
// triangles crossing the near or side frustum planes keep their
// visible fragment instead of being discarded wholesale
//=====================================================================
typedef struct { float x, y, z, w; } clip_vertex;

// plane codes, each keeps the half space a*x + b*y + c*z + d*w >= 0
enum {
	CLIP_LEFT = 0,      // x + w >= 0
	CLIP_RIGHT,         // w - x >= 0
	CLIP_BOTTOM,        // y + w >= 0
	CLIP_TOP,           // w - y >= 0
	CLIP_NEAR,          // z >= 0
	CLIP_FAR            // w - z >= 0
};

static float clip_distance(const clip_vertex *v, int plane) {
	switch (plane) {
	case CLIP_LEFT:   return v->x + v->w;
	case CLIP_RIGHT:  return v->w - v->x;
	case CLIP_BOTTOM: return v->y + v->w;
	case CLIP_TOP:    return v->w - v->y;
	case CLIP_NEAR:   return v->z;
	default:          return v->w - v->z;
	}
}

static int clip_plane(clip_vertex *out, const clip_vertex *in, int n, int plane) {
	int i, count = 0;
	for (i = 0; i < n; i++) {
		const clip_vertex *a = &in[i];
		const clip_vertex *b = &in[(i + 1) % n];
		float da = clip_distance(a, plane);
		float db = clip_distance(b, plane);
		int ina = da >= 0.0f;
		int inb = db >= 0.0f;
		if (ina != inb) {
			float t = da / (da - db);
			clip_vertex q;
			q.x = a->x + (b->x - a->x) * t;
			q.y = a->y + (b->y - a->y) * t;
			q.z = a->z + (b->z - a->z) * t;
			q.w = a->w + (b->w - a->w) * t;
			out[count++] = q;
		}
		if (inb) out[count++] = *b;
	}
	return count;
}

//=====================================================================
// hemicube render target: independent id and depth buffers
//=====================================================================
typedef struct {
	int resolution;
	IUINT32 *id;              // UINT32_MAX = background
	float *depth;             // rhw = 1 / w, larger is nearer
	transform_t transform;    // reused original transform chain
} hemicube_target;

static void target_clear(hemicube_target *ht) {
	memset(ht->id, 0xff, sizeof(IUINT32) * (size_t)ht->resolution * ht->resolution);
	memset(ht->depth, 0, sizeof(float) * (size_t)ht->resolution * ht->resolution);
}

// pixel writer mirroring device_draw_scanline, but filling the
// independent id/depth targets; equal depth keeps the smaller index
static void target_scanline(hemicube_target *ht, scanline_t *scanline, IUINT32 id) {
	int x = scanline->x;
	int w = scanline->w;
	int y = scanline->y;
	int width = ht->resolution;
	if (y < 0 || y >= width) return;
	for (; w > 0; x++, w--) {
		if (x >= 0 && x < width) {
			float rhw = scanline->v.rhw;
			float old = ht->depth[y * width + x];
			IUINT32 old_id = ht->id[y * width + x];
			if (rhw > old || (rhw == old && id < old_id)) {
				ht->depth[y * width + x] = rhw;
				ht->id[y * width + x] = id;
			}
		}
		vertex_add(&scanline->v, &scanline->step);
		if (x >= width) break;
	}
}

static void target_render_trap(hemicube_target *ht, trapezoid_t *trap, IUINT32 id) {
	scanline_t scanline;
	int j, top, bottom;
	top = (int)(trap->top + 0.5f);
	bottom = (int)(trap->bottom + 0.5f);
	for (j = top; j < bottom; j++) {
		if (j >= 0 && j < ht->resolution) {
			trapezoid_edge_interp(trap, (float)j + 0.5f);
			trapezoid_init_scan_line(trap, &scanline, j);
			target_scanline(ht, &scanline, id);
		}
		if (j >= ht->resolution) break;
	}
}

// draw one clipped polygon (fan) through the original trapezoid chain
static void target_draw_polygon(hemicube_target *ht, const clip_vertex *poly,
	int n, IUINT32 id) {
	int k;
	vertex_t verts[12];
	if (n < 3 || n > 12) return;
	for (k = 0; k < n; k++) {
		point_t clip, screen;
		clip.x = poly[k].x; clip.y = poly[k].y;
		clip.z = poly[k].z; clip.w = poly[k].w;
		transform_homogenize(&ht->transform, &screen, &clip);
		memset(&verts[k], 0, sizeof(vertex_t));
		verts[k].pos = screen;
		verts[k].pos.w = poly[k].w;
		vertex_rhw_init(&verts[k]);
	}
	for (k = 1; k + 1 < n; k++) {
		trapezoid_t traps[2];
		int m = trapezoid_init_triangle(traps, &verts[0], &verts[k], &verts[k + 1]);
		if (m >= 1) target_render_trap(ht, &traps[0], id);
		if (m >= 2) target_render_trap(ht, &traps[1], id);
	}
}

//=====================================================================
// build one hemicube view and integrate its cell weights
//=====================================================================
static void render_view(hemicube_target *ht, const radiosity_scene *rs,
	size_t source, const point_t *eye, const vector_t *facing,
	const vector_t *up, int side, const hemicube_weights *hw,
	const char *front_flag, float *row) {
	int res = ht->resolution;
	int half = res / 2;
	int j, px, py, plane;
	clip_vertex input[3], tmp[12], out[12];

	matrix_set_identity(&ht->transform.world);
	matrix_set_lookat(&ht->transform.view, eye,
		&(point_t){ eye->x + facing->x, eye->y + facing->y,
			eye->z + facing->z, 1.0f }, up);
	{
		float zn = rs->offset * 0.25f;
		float zf = rs->offset;
		for (j = 0; j < (int)rs->count; j++) {
			int k;
			if ((size_t)j == source) continue;
			for (k = 0; k < 3; k++) {
				float dx = rs->patches[j].v[k].x - eye->x;
				float dy = rs->patches[j].v[k].y - eye->y;
				float dz = rs->patches[j].v[k].z - eye->z;
				float d = sqrtf(dx * dx + dy * dy + dz * dz);
				if (d > zf) zf = d;
			}
		}
		zf += rs->offset;
		matrix_set_perspective(&ht->transform.projection, RAD_PI * 0.5f, 1.0f, zn, zf);
	}
	ht->transform.w = (float)ht->resolution;
	ht->transform.h = (float)ht->resolution;
	transform_update(&ht->transform);
	target_clear(ht);

	for (j = 0; j < (int)rs->count; j++) {
		int n;
		if ((size_t)j == source) continue;      // source never occludes itself
		for (px = 0; px < 3; px++) {
			point_t clip;
			transform_apply(&ht->transform, &clip, &rs->patches[j].v[px]);
			input[px].x = clip.x; input[px].y = clip.y;
			input[px].z = clip.z; input[px].w = clip.w;
		}
		n = 3;
		memcpy(tmp, input, sizeof(input));
		for (plane = 0; plane < 6; plane++) {
			n = clip_plane(out, tmp, n, plane);
			if (n < 3) break;
			memcpy(tmp, out, sizeof(clip_vertex) * (size_t)n);
		}
		if (n >= 3) target_draw_polygon(ht, tmp, n, (IUINT32)j);
	}

	// integrate: only front facing hits receive weight, back faces and
	// background cells simply drop their weight
	for (py = 0; py < (side ? half : res); py++) {
		for (px = 0; px < res; px++) {
			IUINT32 id = ht->id[py * res + px];
			float wgt;
			if (id == UINT32_MAX) continue;
			if (!front_flag[id]) continue;
			wgt = side ? weight_side(hw, px, py) : weight_top(hw, px, py);
			row[id] += wgt;
		}
	}
}

//=====================================================================
// scene construction
//=====================================================================
radiosity_scene *radiosity_create(const radiosity_patch *patches, size_t count,
	int resolution, float offset) {
	radiosity_scene *rs;
	hemicube_weights hw;
	hemicube_target ht;
	size_t i, j;
	int view;

	if (patches == NULL || count == 0) return NULL;
	if (resolution < 4 || (resolution & 1) != 0) return NULL;
	if (!isfinite(offset) || offset <= 0.0f) return NULL;
	if (count > (size_t)UINT32_MAX) return NULL;

	for (i = 0; i < count; i++) {
		const radiosity_patch *p = &patches[i];
		int k;
		for (k = 0; k < 3; k++) {
			if (!rad_finite_v(&p->v[k])) return NULL;
			if (p->v[k].w != 1.0f) return NULL;
		}
		if (!rad_finite_c(&p->reflectance) || !rad_finite_c(&p->emission))
			return NULL;
		if (p->reflectance.r < 0.0f || p->reflectance.r >= 1.0f ||
			p->reflectance.g < 0.0f || p->reflectance.g >= 1.0f ||
			p->reflectance.b < 0.0f || p->reflectance.b >= 1.0f)
			return NULL;
		if (p->emission.r < 0.0f || p->emission.g < 0.0f ||
			p->emission.b < 0.0f)
			return NULL;
	}

	rs = (radiosity_scene *)calloc(1, sizeof(radiosity_scene));
	if (rs == NULL) return NULL;
	rs->count = count;
	rs->resolution = resolution;
	rs->offset = offset;
	rs->last_shooter = SIZE_MAX;

	rs->patches = (radiosity_patch *)malloc(sizeof(radiosity_patch) * count);
	rs->B = (color_t *)malloc(sizeof(color_t) * count);
	rs->U = (color_t *)malloc(sizeof(color_t) * count);
	rs->area = (float *)malloc(sizeof(float) * count);
	rs->normal = (vector_t *)malloc(sizeof(vector_t) * count);
	rs->centroid = (point_t *)malloc(sizeof(point_t) * count);
	rs->form = (float *)malloc(sizeof(float) * count * count);
	if (!rs->patches || !rs->B || !rs->U || !rs->area || !rs->normal ||
		!rs->centroid || !rs->form) {
		radiosity_destroy(rs);
		return NULL;
	}
	memcpy(rs->patches, patches, sizeof(radiosity_patch) * count);
	memset(rs->form, 0, sizeof(float) * count * count);

	// areas, normals, centroids; reject degenerate patches
	for (i = 0; i < count; i++) {
		const point_t *v = rs->patches[i].v;
		vector_t e1, e2, cr;
		vector_sub(&e1, &v[1], &v[0]);
		vector_sub(&e2, &v[2], &v[0]);
		vector_crossproduct(&cr, &e1, &e2);
		if (!rad_finite_v(&cr)) {
			radiosity_destroy(rs);
			return NULL;
		}
		{
			float len2 = cr.x * cr.x + cr.y * cr.y + cr.z * cr.z;
			if (!(len2 > 0.0f)) {
				radiosity_destroy(rs);
				return NULL;
			}
			rs->area[i] = 0.5f * sqrtf(len2);
			rs->normal[i] = cr;
			vector_normalize(&rs->normal[i]);
		}
		rs->centroid[i].x = (v[0].x + v[1].x + v[2].x) / 3.0f;
		rs->centroid[i].y = (v[0].y + v[1].y + v[2].y) / 3.0f;
		rs->centroid[i].z = (v[0].z + v[1].z + v[2].z) / 3.0f;
		rs->centroid[i].w = 1.0f;
		rs->B[i] = rs->patches[i].emission;
		rs->U[i] = rs->patches[i].emission;
	}

	ht.resolution = resolution;
	ht.id = (IUINT32 *)malloc(sizeof(IUINT32) * (size_t)resolution * resolution);
	ht.depth = (float *)malloc(sizeof(float) * (size_t)resolution * resolution);
	if (!ht.id || !ht.depth) {
		free(ht.id);
		free(ht.depth);
		radiosity_destroy(rs);
		return NULL;
	}
	weights_init(&hw, resolution);

	for (i = 0; i < count; i++) {
		vector_t n = rs->normal[i];
		vector_t helper, t, b;
		vector_t dirs[5], ups[5];
		point_t eye;
		char *front_flag;
		float *row = &rs->form[i * count];
		float row_sum = 0.0f;

		if (fabsf(n.y) < 0.9f)
			vec_set(&helper, 0.0f, 1.0f, 0.0f);
		else
			vec_set(&helper, 1.0f, 0.0f, 0.0f);
		vector_crossproduct(&t, &helper, &n);
		vector_normalize(&t);
		vector_crossproduct(&b, &n, &t);

		eye.x = rs->centroid[i].x + n.x * offset;
		eye.y = rs->centroid[i].y + n.y * offset;
		eye.z = rs->centroid[i].z + n.z * offset;
		eye.w = 1.0f;

		// top face along n, then the four side faces
		dirs[0] = n; ups[0] = b;
		dirs[1] = t; ups[1] = n;
		dirs[2].x = -t.x; dirs[2].y = -t.y; dirs[2].z = -t.z;
		dirs[2].w = 1.0f; ups[2] = n;
		dirs[3] = b; ups[3] = n;
		dirs[4].x = -b.x; dirs[4].y = -b.y; dirs[4].z = -b.z;
		dirs[4].w = 1.0f; ups[4] = n;

		front_flag = (char *)malloc(count);
		if (front_flag == NULL) {
			free(ht.id);
			free(ht.depth);
			radiosity_destroy(rs);
			return NULL;
		}
		for (j = 0; j < count; j++) {
			vector_t to_eye;
			float dot;
			if (j == i) { front_flag[j] = 0; continue; }
			vector_sub(&to_eye, &eye, &rs->patches[j].v[0]);
			dot = vector_dotproduct(&rs->normal[j], &to_eye);
			front_flag[j] = (dot > 0.0f) ? 1 : 0;
		}

		// top view
		render_view(&ht, rs, i, &eye, &dirs[0], &ups[0], 0, &hw,
			front_flag, row);
		// four side views: facing dir, up n, right cross(n, dir)
		for (view = 1; view < 5; view++) {
			vector_t side_up = n;
			render_view(&ht, rs, i, &eye, &dirs[view], &side_up, 1, &hw,
				front_flag, row);
		}
		free(front_flag);

		// guarantee the row sum never exceeds 1 under float rounding
		for (j = 0; j < count; j++) row_sum += row[j];
		if (row_sum > 1.0f) {
			float scale = 1.0f / row_sum;
			for (j = 0; j < count; j++) row[j] *= scale;
		}
	}

	free(ht.id);
	free(ht.depth);
	return rs;
}

void radiosity_destroy(radiosity_scene *scene) {
	if (scene == NULL) return;
	free(scene->patches);
	free(scene->B);
	free(scene->U);
	free(scene->area);
	free(scene->normal);
	free(scene->centroid);
	free(scene->form);
	free(scene);
}

float radiosity_factor(const radiosity_scene *scene, size_t i, size_t j) {
	if (scene == NULL || i >= scene->count || j >= scene->count) return 0.0f;
	if (i == j) return 0.0f;
	return scene->form[i * scene->count + j];
}

//=====================================================================
// progressive radiosity
//=====================================================================
size_t radiosity_step(radiosity_scene *scene, size_t shots) {
	size_t done, i, j;
	if (scene == NULL) return 0;
	for (done = 0; done < shots; done++) {
		size_t shooter = SIZE_MAX;
		float best = 0.0f;
		for (i = 0; i < scene->count; i++) {
			float energy = scene->area[i] *
				(scene->U[i].r + scene->U[i].g + scene->U[i].b);
			if (energy > best) {
				best = energy;
				shooter = i;
			}
		}
		if (shooter == SIZE_MAX) break;      // no unshot energy left

		{
			color_t shot = scene->U[shooter];
			float ai = scene->area[shooter];
			const float *row = &scene->form[shooter * scene->count];
			scene->U[shooter].r = scene->U[shooter].g =
				scene->U[shooter].b = 0.0f;
			for (j = 0; j < scene->count; j++) {
				float f = row[j];
				float ratio, rr, gg, bb;
				if (f == 0.0f) continue;
				ratio = ai / scene->area[j] * f;
				rr = scene->patches[j].reflectance.r * shot.r * ratio;
				gg = scene->patches[j].reflectance.g * shot.g * ratio;
				bb = scene->patches[j].reflectance.b * shot.b * ratio;
				scene->B[j].r += rr; scene->B[j].g += gg; scene->B[j].b += bb;
				scene->U[j].r += rr; scene->U[j].g += gg; scene->U[j].b += bb;
			}
			scene->last_shooter = shooter;
		}
	}
	return done;
}

int radiosity_get(const radiosity_scene *scene, size_t i,
	color_t *B, color_t *U, float *area) {
	if (scene == NULL || i >= scene->count) return 0;
	if (B) *B = scene->B[i];
	if (U) *U = scene->U[i];
	if (area) *area = scene->area[i];
	return 1;
}

size_t radiosity_last_shooter(const radiosity_scene *scene) {
	if (scene == NULL) return SIZE_MAX;
	return scene->last_shooter;
}

void radiosity_reset(radiosity_scene *scene) {
	size_t i;
	if (scene == NULL) return;
	for (i = 0; i < scene->count; i++) {
		scene->B[i] = scene->patches[i].emission;
		scene->U[i] = scene->patches[i].emission;
	}
	scene->last_shooter = SIZE_MAX;
}

//=====================================================================
// drawing through the original color triangle pipeline
//=====================================================================
void radiosity_draw(const radiosity_scene *scene, device_t *device,
	float exposure) {
	transform_t saved_transform;
	int saved_state;
	size_t i;
	if (scene == NULL || device == NULL) return;

	saved_transform = device->transform;
	saved_state = device->render_state;
	matrix_set_identity(&device->transform.world);
	transform_update(&device->transform);
	device->render_state = RENDER_STATE_COLOR;

	for (i = 0; i < scene->count; i++) {
		vertex_t verts[3];
		color_t c;
		int k;
		c.r = scene->B[i].r * exposure;
		c.g = scene->B[i].g * exposure;
		c.b = scene->B[i].b * exposure;
		for (k = 0; k < 3; k++) {
			memset(&verts[k], 0, sizeof(vertex_t));
			verts[k].pos = scene->patches[i].v[k];
			verts[k].color = c;
		}
		device_draw_primitive(device, &verts[0], &verts[1], &verts[2]);
	}

	device->transform = saved_transform;
	device->render_state = saved_state;
}
