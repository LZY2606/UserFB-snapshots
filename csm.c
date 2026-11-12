//=====================================================================
//
// csm.c - Cascaded Shadow Maps (三级级联阴影) for Mini3D
//
// 实现见 csm.h。几何、矩阵与梯形扫描线复用 mini3d 核心；光源 pass
// 使用光空间真实深度（正交投影 w 恒为 1，不能沿用主相机的 rhw 比较）。
//
//=====================================================================
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "csm.h"

#define CSM_DEPTH_FAR 1.0f		/* 深度图远值（归一化光深） */

//---------------------------------------------------------------------
// 基础工具
//---------------------------------------------------------------------

// 通用 4x4 逆矩阵（Gauss-Jordan，行向量约定），返回 0 表示不可逆
static int csm_matrix_invert(matrix_t *out, const matrix_t *m) {
	double a[4][8];
	int i, j, k;
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			a[i][j] = m->m[i][j];
			a[i][j + 4] = (i == j) ? 1.0 : 0.0;
		}
	}
	for (i = 0; i < 4; i++) {
		int p = i;
		double d, f;
		for (k = i + 1; k < 4; k++)
			if (fabs(a[k][i]) > fabs(a[p][i])) p = k;
		if (fabs(a[p][i]) < 1e-12) return 0;
		if (p != i) {
			for (j = 0; j < 8; j++) {
				double t = a[i][j]; a[i][j] = a[p][j]; a[p][j] = t;
			}
		}
		d = a[i][i];
		for (j = 0; j < 8; j++) a[i][j] /= d;
		for (k = 0; k < 4; k++) {
			if (k == i) continue;
			f = a[k][i];
			for (j = 0; j < 8; j++) a[k][j] -= f * a[i][j];
		}
	}
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			out->m[i][j] = (float)a[i][j + 4];
	return 1;
}

//---------------------------------------------------------------------
// 初始化 / 销毁
//---------------------------------------------------------------------
void csm_init(csm_t *csm, int size) {
	int i;
	memset(csm, 0, sizeof(*csm));
	csm->size = size;
	csm->lambda = 0.5f;
	csm->ambient = 0.0f;
	csm->bias = 0.0f;
	for (i = 0; i < CSM_CASCADES; i++) {
		csm->depth[i] = (float*)malloc(sizeof(float) * (size_t)size * (size_t)size);
		csm->depth_owner[i] = 1;
	}
	csm_clear(csm);
}

void csm_destroy(csm_t *csm) {
	int i;
	for (i = 0; i < CSM_CASCADES; i++) {
		if (csm->depth_owner[i] && csm->depth[i]) free(csm->depth[i]);
		csm->depth[i] = NULL;
		csm->depth_owner[i] = 0;
	}
}

void csm_set_depth_map(csm_t *csm, int index, float *map) {
	if (index < 0 || index >= CSM_CASCADES) return;
	if (csm->depth_owner[index] && csm->depth[index]) free(csm->depth[index]);
	csm->depth[index] = map;
	csm->depth_owner[index] = 0;
}

void csm_clear(csm_t *csm) {
	int i;
	for (i = 0; i < CSM_CASCADES; i++) {
		int n = csm->size * csm->size, k;
		float *d = csm->depth[i];
		if (d == NULL) continue;
		for (k = 0; k < n; k++) d[k] = CSM_DEPTH_FAR;
	}
}

//---------------------------------------------------------------------
// 级联划分与光矩阵
//---------------------------------------------------------------------
void csm_update(csm_t *csm, const matrix_t *cam_view, float fovy, float aspect,
	float zn, float zf, const vector_t *light_dir, float lambda,
	const point_t *occluders, int tri_count)
{
	vector_t L = *light_dir;
	vector_t up = { 0.0f, 1.0f, 0.0f, 1.0f };
	vector_t eye = { 0.0f, 0.0f, 0.0f, 1.0f };
	vector_t at;
	matrix_t inv_view;
	float tan_half = (float)tan(fovy * 0.5f);
	int i, k;

	vector_normalize(&L);
	/* 光方向几乎平行世界 up 时换用侧向参考轴，保证基底稳定 */
	if (fabsf(vector_dotproduct(&L, &up)) > 0.99f) {
		up.x = 0.0f; up.y = 0.0f; up.z = 1.0f;
	}
	at.x = L.x; at.y = L.y; at.z = L.z; at.w = 1.0f;
	matrix_set_lookat(&csm->light_view, &eye, &at, &up);
	csm_matrix_invert(&inv_view, cam_view);

	csm->lambda = lambda;
	csm->znear = zn;
	csm->zfar = zf;
	csm->splits[0] = zn;
	for (i = 1; i <= CSM_CASCADES; i++) {
		float uni = zn + (zf - zn) * ((float)i / CSM_CASCADES);
		float logsp = zn * (float)pow(zf / zn, (double)i / CSM_CASCADES);
		csm->splits[i] = lambda * logsp + (1.0f - lambda) * uni;
	}

	for (i = 0; i < CSM_CASCADES; i++) {
		float d0 = csm->splits[i], d1 = csm->splits[i + 1];
		float cx = 0.0f, cy = 0.0f, cz = 0.0f, r = 0.0f;
		float texel0, texel, extent, half, scx, scy, zmin, zmax, pad;
		point_t corners[8];
		matrix_t proj;

		/* 视锥段 8 个角点：view 空间构造，经 inv_view 到世界再到光空间 */
		for (k = 0; k < 8; k++) {
			float d = (k & 1) ? d1 : d0;
			float hh = d * tan_half, hw = hh * aspect;
			point_t vc, wc, lc;
			vc.x = (k & 2) ? hw : -hw;
			vc.y = (k & 4) ? hh : -hh;
			vc.z = d;
			vc.w = 1.0f;
			matrix_apply(&wc, &vc, &inv_view);
			matrix_apply(&lc, &wc, &csm->light_view);
			corners[k] = lc;
			cx += lc.x; cy += lc.y; cz += lc.z;
		}
		cx *= 0.125f; cy *= 0.125f; cz *= 0.125f;
		for (k = 0; k < 8; k++) {
			float dx = corners[k].x - cx;
			float dy = corners[k].y - cy;
			float dz = corners[k].z - cz;
			float dd = (float)sqrt(dx * dx + dy * dy + dz * dz);
			if (dd > r) r = dd;
		}

		/* 方形边长对相机平移不变（包围球半径 + 一个纹素余量），
		   投影中心按世界空间纹素大小吸附，亚纹素平移不会滑动 */
		texel0 = 2.0f * r / (float)csm->size;
		extent = 2.0f * r + 2.0f * texel0;
		half = extent * 0.5f;
		texel = extent / (float)csm->size;
		scx = (float)floor(cx / texel + 0.5f) * texel;
		scy = (float)floor(cy / texel + 0.5f) * texel;

		/* 光深范围：角点包围球，再扩进能投到接收区（xy 落在方形内）
		   的离屏遮挡物 */
		zmin = cz - r;
		zmax = cz + r;
		for (k = 0; k < tri_count * 3; k++) {
			point_t lc;
			matrix_apply(&lc, &occluders[k], &csm->light_view);
			if (lc.x >= scx - half && lc.x <= scx + half &&
				lc.y >= scy - half && lc.y <= scy + half) {
				if (lc.z < zmin) zmin = lc.z;
				if (lc.z > zmax) zmax = lc.z;
			}
		}
		pad = 0.01f * (zmax - zmin) + 1e-3f;
		zmin -= pad;
		zmax += pad;

		/* 方形正交投影：xy -> [-1,1]，z -> [0,1]，w 恒为 1 */
		matrix_set_zero(&proj);
		proj.m[0][0] = 2.0f / extent;
		proj.m[1][1] = 2.0f / extent;
		proj.m[2][2] = 1.0f / (zmax - zmin);
		proj.m[3][0] = -scx * proj.m[0][0];
		proj.m[3][1] = -scy * proj.m[1][1];
		proj.m[3][2] = -zmin / (zmax - zmin);
		proj.m[3][3] = 1.0f;
		matrix_mul(&csm->light_vp[i], &csm->light_view, &proj);
		csm->light_proj[i] = proj;
		csm->ortho_center[i][0] = scx;
		csm->ortho_center[i][1] = scy;
		csm->ortho_center_raw[i][0] = cx;
		csm->ortho_center_raw[i][1] = cy;
		csm->ortho_extent[i] = extent;
		csm->depth_min[i] = zmin;
		csm->depth_max[i] = zmax;
	}
}

//---------------------------------------------------------------------
// 光源 pass：齐次裁剪 + 光空间深度光栅化
//---------------------------------------------------------------------

// 齐次裁剪平面：返回 >= 0 表示在 cvv 内
static float csm_plane_dist(const vector_t *p, int plane) {
	switch (plane) {
	case 0: return p->z;			/* z >= 0 */
	case 1: return p->w - p->z;		/* z <= w */
	case 2: return p->x + p->w;		/* x >= -w */
	case 3: return p->w - p->x;		/* x <= w */
	case 4: return p->y + p->w;		/* y >= -w */
	default: return p->w - p->y;	/* y <= w */
	}
}

// Sutherland-Hodgman：用单个齐次平面裁凸多边形，返回新顶点数。
// 裁剪出的顶点属性（含光深 pos.z）按线性插值保持连续。
static int csm_clip_plane(vertex_t *out, const vertex_t *in, int n, int plane) {
	int m = 0, i;
	for (i = 0; i < n; i++) {
		const vertex_t *a = &in[i];
		const vertex_t *b = &in[(i + 1) % n];
		float da = csm_plane_dist(&a->pos, plane);
		float db = csm_plane_dist(&b->pos, plane);
		if (da >= 0.0f) out[m++] = *a;
		if ((da >= 0.0f) != (db >= 0.0f)) {
			float t = da / (da - db);
			vertex_interp(&out[m], a, b, t);
			m++;
		}
	}
	return m;
}

// 光深度扫描线：z 小（更靠近光源）者保留；正交 w 恒为 1，不用 rhw 比较
static void csm_light_scanline(float *depth, int size, scanline_t *scanline) {
	int x = scanline->x;
	int w = scanline->w;
	float *row = depth + scanline->y * size;
	for (; w > 0; x++, w--) {
		if (x >= 0 && x < size) {
			float z = scanline->v.pos.z;
			if (z < row[x]) row[x] = z;
		}
		vertex_add(&scanline->v, &scanline->step);
		if (x >= size) break;
	}
}

static void csm_light_trap(float *depth, int size, trapezoid_t *trap) {
	scanline_t scanline;
	int j, top, bottom;
	top = (int)(trap->top + 0.5f);
	bottom = (int)(trap->bottom + 0.5f);
	for (j = top; j < bottom; j++) {
		if (j >= 0 && j < size) {
			trapezoid_edge_interp(trap, (float)j + 0.5f);
			trapezoid_init_scan_line(trap, &scanline, j);
			csm_light_scanline(depth, size, &scanline);
		}
		if (j >= size) break;
	}
}

static void csm_light_triangle(csm_t *csm, int cascade, const vertex_t *v1,
	const vertex_t *v2, const vertex_t *v3)
{
	vertex_t t1 = *v1, t2 = *v2, t3 = *v3;
	trapezoid_t traps[2];
	transform_t ts;
	int n;
	memset(&ts, 0, sizeof(ts));
	ts.w = ts.h = (float)csm->size;
	transform_homogenize(&ts, &t1.pos, &v1->pos);
	transform_homogenize(&ts, &t2.pos, &v2->pos);
	transform_homogenize(&ts, &t3.pos, &v3->pos);
	t1.pos.w = v1->pos.w;
	t2.pos.w = v2->pos.w;
	t3.pos.w = v3->pos.w;
	vertex_rhw_init(&t1);
	vertex_rhw_init(&t2);
	vertex_rhw_init(&t3);
	n = trapezoid_init_triangle(traps, &t1, &t2, &t3);
	if (n >= 1) csm_light_trap(csm->depth[cascade], csm->size, &traps[0]);
	if (n >= 2) csm_light_trap(csm->depth[cascade], csm->size, &traps[1]);
}

void csm_render_triangle(csm_t *csm, const point_t *a, const point_t *b,
	const point_t *c)
{
	int i, p, k;
	for (i = 0; i < CSM_CASCADES; i++) {
		vertex_t poly[16], tmp[16];
		int n = 3;
		if (csm->depth[i] == NULL) continue;
		memset(poly, 0, sizeof(poly));
		matrix_apply(&poly[0].pos, a, &csm->light_vp[i]);
		matrix_apply(&poly[1].pos, b, &csm->light_vp[i]);
		matrix_apply(&poly[2].pos, c, &csm->light_vp[i]);
		poly[0].world = *a;
		poly[1].world = *b;
		poly[2].world = *c;
		/* 按 6 个齐次平面裁出可绘部分，不能整面丢弃 */
		for (p = 0; p < 6 && n > 0; p++) {
			n = csm_clip_plane(tmp, poly, n, p);
			if (n > 0) memcpy(poly, tmp, sizeof(vertex_t) * (size_t)n);
		}
		for (k = 1; k + 1 < n; k++)
			csm_light_triangle(csm, i, &poly[0], &poly[k], &poly[k + 1]);
	}
}

void csm_render(csm_t *csm, const point_t *tris, int tri_count) {
	int i;
	for (i = 0; i < tri_count; i++)
		csm_render_triangle(csm, &tris[i * 3], &tris[i * 3 + 1], &tris[i * 3 + 2]);
}

//---------------------------------------------------------------------
// 主 pass 取样
//---------------------------------------------------------------------

// 单级可见度：深度图外视为无遮挡；接收深度超过图中深度加 bias 记为阴影
static float csm_visibility_one(const csm_t *csm, int i, const point_t *world) {
	vector_t clip;
	float invw, nx, ny, nz, sx, sy, map, range, bias;
	int ix, iy;
	matrix_apply(&clip, world, &csm->light_vp[i]);
	if (clip.w == 0.0f) return 1.0f;
	invw = 1.0f / clip.w;
	nx = clip.x * invw;
	ny = clip.y * invw;
	nz = clip.z * invw;
	if (nz < 0.0f || nz > 1.0f) return 1.0f;
	sx = (nx + 1.0f) * 0.5f * (float)csm->size;
	sy = (1.0f - ny) * 0.5f * (float)csm->size;
	ix = (int)floorf(sx);
	iy = (int)floorf(sy);
	if (ix < 0 || iy < 0 || ix >= csm->size || iy >= csm->size) return 1.0f;
	map = csm->depth[i][iy * csm->size + ix];
	range = csm->depth_max[i] - csm->depth_min[i];
	bias = (range > 0.0f) ? csm->bias / range : 0.0f;
	return (nz > map + bias) ? 0.0f : 1.0f;
}

float csm_visibility(const csm_t *csm, const point_t *world, float view_z) {
	int i;
	float s0, s1, seg, t, vis;
	for (i = 0; i < CSM_CASCADES; i++) {
		if (view_z <= csm->splits[i + 1]) break;
	}
	if (i >= CSM_CASCADES) return 1.0f;		/* 最后一级以外无遮挡 */
	vis = csm_visibility_one(csm, i, world);
	s0 = csm->splits[i];
	s1 = csm->splits[i + 1];
	seg = s1 - s0;
	/* 每段最后 10% 视深范围与下一级线性混合 */
	if (i + 1 < CSM_CASCADES && seg > 0.0f) {
		t = (view_z - (s1 - 0.1f * seg)) / (0.1f * seg);
		if (t > 0.0f) {
			float next;
			if (t > 1.0f) t = 1.0f;
			next = csm_visibility_one(csm, i + 1, world);
			vis = vis + (next - vis) * t;
		}
	}
	return vis;
}

//---------------------------------------------------------------------
// 与渲染设备绑定
//---------------------------------------------------------------------
static float csm_device_sample(void *ctx, const point_t *world, float view_z) {
	return csm_visibility((const csm_t*)ctx, world, view_z);
}

void csm_bind(csm_t *csm, device_t *device, float ambient, float bias) {
	if (ambient < 0.0f) ambient = 0.0f;
	if (ambient > 1.0f) ambient = 1.0f;
	if (bias < 0.0f) bias = 0.0f;
	csm->ambient = ambient;
	csm->bias = bias;
	device->shadow_state = 1;
	device->shadow_fn = csm_device_sample;
	device->shadow_ctx = csm;
	device->shadow_ambient = ambient;
}

void csm_unbind(device_t *device) {
	device->shadow_state = 0;
	device->shadow_fn = NULL;
	device->shadow_ctx = NULL;
}
