//=====================================================================
//
// vsm.c - Variance Shadow Map 实现
//
// 捕获：光源齐次空间六面裁剪（Sutherland-Hodgman），裁出的三角形
// 交给 mini3d 既有的梯形拆分/扫描线光栅路径，逐像素回调里保留
// 最小 z 并写入一、二阶矩。过滤与采样只接触矩，不做硬阴影比较。
//
//=====================================================================
#include "vsm.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

struct vsm_map {
	int width;
	int height;
	float *depth;		// 每个光图像素的最小 z，未覆盖为 1
	float *raw1;		// 原始一阶矩 M1
	float *raw2;		// 原始二阶矩 M2
	float *m1;			// 过滤后一阶矩
	float *m2;			// 过滤后二阶矩
	float *tmp1;		// 过滤用乒乓缓冲
	float *tmp2;
	float *work1;
	float *work2;
};

// 捕获上下文：挂在内部光栅设备的逐像素回调上
struct capture_ctx {
	vsm_map_t *map;
	float dzdx;			// 当前面深度平面在光图像素下的梯度
	float dzdy;
};

#define PLANE_COUNT 6

vsm_map_t *vsm_create(int width, int height) {
	vsm_map_t *map;
	size_t n;
	if (width <= 0 || height <= 0) return NULL;
	n = (size_t)width * (size_t)height;
	map = (vsm_map_t*)calloc(1, sizeof(vsm_map_t));
	if (map == NULL) return NULL;
	map->width = width;
	map->height = height;
	map->depth = (float*)malloc(n * sizeof(float) * 9);
	if (map->depth == NULL) {
		free(map);
		return NULL;
	}
	map->raw1 = map->depth + n;
	map->raw2 = map->raw1 + n;
	map->m1 = map->raw2 + n;
	map->m2 = map->m1 + n;
	map->tmp1 = map->m2 + n;
	map->tmp2 = map->tmp1 + n;
	map->work1 = map->tmp2 + n;
	map->work2 = map->work1 + n;
	vsm_clear(map);
	return map;
}

void vsm_destroy(vsm_map_t *map) {
	if (map == NULL) return;
	free(map->depth);
	free(map);
}

void vsm_clear(vsm_map_t *map) {
	size_t n = (size_t)map->width * (size_t)map->height;
	size_t i;
	if (map == NULL) return;
	for (i = 0; i < n; i++) {
		map->depth[i] = 1.0f;
		map->raw1[i] = 1.0f;
		map->raw2[i] = 1.0f;
		map->m1[i] = 1.0f;
		map->m2[i] = 1.0f;
		map->tmp1[i] = 1.0f;
		map->tmp2[i] = 1.0f;
		map->work1[i] = 1.0f;
		map->work2[i] = 1.0f;
	}
}

//------------------------------------------------------------
// 光源齐次空间六面裁剪：CVV 为 x/y∈[-w,w]、z∈[0,w]
//------------------------------------------------------------
static int clip_flags(const point_t *p) {
	int f = 0;
	if (p->z < 0.0f) f |= 1;
	if (p->z > p->w) f |= 2;
	if (p->x < -p->w) f |= 4;
	if (p->x > p->w) f |= 8;
	if (p->y < -p->w) f |= 16;
	if (p->y > p->w) f |= 32;
	return f;
}

// 点到六个裁剪面的有向“距离”，>=0 表示在面内
static float plane_distance(const point_t *p, int plane) {
	switch (plane) {
	case 0: return p->z;			// z >= 0
	case 1: return p->w - p->z;		// z <= w
	case 2: return p->x + p->w;		// x >= -w
	case 3: return p->w - p->x;		// x <= w
	case 4: return p->y + p->w;		// y >= -w
	default: return p->w - p->y;	// y <= w
	}
}

static point_t clip_interp(const point_t *a, const point_t *b,
	int plane) {
	float da = plane_distance(a, plane);
	float db = plane_distance(b, plane);
	float t = da / (da - db);
	point_t r;
	r.x = a->x + (b->x - a->x) * t;
	r.y = a->y + (b->y - a->y) * t;
	r.z = a->z + (b->z - a->z) * t;
	r.w = a->w + (b->w - a->w) * t;
	return r;
}

// 对单个多边形依次过六个面，结果写回 poly，scratch 为备用缓冲
static int clip_polygon(point_t *poly, int nin, point_t *scratch) {
	point_t *src = poly;
	point_t *dst = scratch;
	int sn = nin;
	int k;
	for (k = 0; k < PLANE_COUNT; k++) {
		int dn = 0, i;
		point_t *tmp;
		for (i = 0; i < sn; i++) {
			const point_t *a = &src[i];
			const point_t *b = &src[(i + 1) % sn];
			int ina = plane_distance(a, k) >= 0.0f;
			int inb = plane_distance(b, k) >= 0.0f;
			if (ina) {
				dst[dn++] = *a;
				if (!inb) dst[dn++] = clip_interp(a, b, k);
			} else if (inb) {
				dst[dn++] = clip_interp(a, b, k);
			}
		}
		tmp = src;
		src = dst;
		dst = tmp;
		sn = dn;
		if (sn == 0) return 0;
	}
	if (src != poly) memcpy(poly, src, (size_t)sn * sizeof(point_t));
	return sn;
}

//------------------------------------------------------------
// 光栅回调：复用生产扫描线，只在光图上保留最小 z 与矩
//------------------------------------------------------------
static void capture_pixel(void *user, int x, int y, const vertex_t *v) {
	struct capture_ctx *ctx = (struct capture_ctx*)user;
	vsm_map_t *map = ctx->map;
	if ((unsigned)x >= (unsigned)map->width ||
		(unsigned)y >= (unsigned)map->height) return;
	{
		size_t idx = (size_t)y * (size_t)map->width + (size_t)x;
		float z = v->lc.z;		// rhw_init 已除过光空间 w
		if (!isfinite(z)) return;
		if (z < map->depth[idx]) {
			float gx = ctx->dzdx, gy = ctx->dzdy;
			map->depth[idx] = z;
			map->raw1[idx] = z;
			map->raw2[idx] = z * z + gx * gx * 0.25f + gy * gy * 0.25f;
		}
	}
}

// 由当前面三个光图屏幕顶点解析深度平面 z=a*X+b*Y+c
static void depth_plane_gradient(const vertex_t *t1, const vertex_t *t2,
	const vertex_t *t3, float *dzdx, float *dzdy) {
	float x0 = t1->pos.x, y0 = t1->pos.y, z0 = t1->lc.z;
	float x1 = t2->pos.x, y1 = t2->pos.y, z1 = t2->lc.z;
	float x2 = t3->pos.x, y2 = t3->pos.y, z2 = t3->lc.z;
	float det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	if (det == 0.0f || !isfinite(det)) {
		*dzdx = 0.0f;
		*dzdy = 0.0f;
		return;
	}
	*dzdx = ((z1 - z0) * (y2 - y0) - (z2 - z0) * (y1 - y0)) / det;
	*dzdy = ((x1 - x0) * (z2 - z0) - (x2 - x0) * (z1 - z0)) / det;
}

void vsm_capture(vsm_map_t *map, const matrix_t *object_to_light,
	const vertex_t *triangles, size_t count) {
	device_t device;
	struct capture_ctx ctx;
	size_t tri;
	if (map == NULL || object_to_light == NULL || triangles == NULL) return;

	vsm_clear(map);		// 每次 capture 重新开始
	ctx.map = map;
	ctx.dzdx = 0.0f;
	ctx.dzdy = 0.0f;

	// 光图尺寸的内部光栅设备：变换置单位阵，屏幕坐标即光图像素
	device_init(&device, map->width, map->height, NULL);
	matrix_set_identity(&device.transform.transform);
	device.transform.w = (float)map->width;
	device.transform.h = (float)map->height;
	device.render_state = RENDER_STATE_COLOR;
	device.shadow_pixel_cb = capture_pixel;
	device.shadow_user = &ctx;

	for (tri = 0; tri < count; tri++) {
		const vertex_t *src = triangles + tri * 3;
		point_t light[3];
		point_t clipped[16];
		point_t scratch[16];
		int nout = 0;
		int k;

		matrix_apply(&light[0], &src[0].pos, object_to_light);
		matrix_apply(&light[1], &src[1].pos, object_to_light);
		matrix_apply(&light[2], &src[2].pos, object_to_light);

		// 整面在 CVV 内则跳过裁剪；否则六面裁出可见部分，不整面丢弃
		if (clip_flags(&light[0]) == 0 && clip_flags(&light[1]) == 0 &&
			clip_flags(&light[2]) == 0) {
			clipped[0] = light[0];
			clipped[1] = light[1];
			clipped[2] = light[2];
			nout = 3;
		} else if ((clip_flags(&light[0]) & clip_flags(&light[1]) &
			clip_flags(&light[2])) != 0) {
			continue;	// 三个顶点被同一面同时剔除，多边形无可见部分
		} else {
			memcpy(clipped, light, sizeof(light));
			nout = clip_polygon(clipped, 3, scratch);
			if (nout < 3) continue;
		}

		// 扇形拆为三角形，逐个走生产梯形/扫描线路径
		for (k = 1; k + 1 < nout; k++) {
			vertex_t t1, t2, t3;
			trapezoid_t traps[2];
			int n;

			memset(&t1, 0, sizeof(t1));
			memset(&t2, 0, sizeof(t2));
			memset(&t3, 0, sizeof(t3));
			t1.lc = clipped[0];
			t2.lc = clipped[k];
			t3.lc = clipped[k + 1];

			transform_homogenize(&device.transform, &t1.pos, &clipped[0]);
			transform_homogenize(&device.transform, &t2.pos, &clipped[k]);
			transform_homogenize(&device.transform, &t3.pos, &clipped[k + 1]);
			t1.pos.w = clipped[0].w;
			t2.pos.w = clipped[k].w;
			t3.pos.w = clipped[k + 1].w;

			vertex_rhw_init(&t1);
			vertex_rhw_init(&t2);
			vertex_rhw_init(&t3);

			// 梯度来自当前面深度平面（透视除法后的 z），单位为光图像素
			depth_plane_gradient(&t1, &t2, &t3, &ctx.dzdx, &ctx.dzdy);

			n = trapezoid_init_triangle(traps, &t1, &t2, &t3);
			if (n >= 1) device_render_trap(&device, &traps[0]);
			if (n >= 2) device_render_trap(&device, &traps[1]);
		}
	}

	device_destroy(&device);
}

//------------------------------------------------------------
// 盒式过滤：每轮横向、纵向各一次 [1,2,1]/4，两矩独立
//------------------------------------------------------------
static void convolve_pass(float *src, float *dst, int w, int h) {
	int y, x;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			float l = (x > 0) ? src[y * w + x - 1] : 1.0f;
			float c = src[y * w + x];
			float r = (x + 1 < w) ? src[y * w + x + 1] : 1.0f;
			dst[y * w + x] = (l + 2.0f * c + r) * 0.25f;
		}
	}
	for (x = 0; x < w; x++) {
		for (y = 0; y < h; y++) {
			float u = (y > 0) ? dst[(y - 1) * w + x] : 1.0f;
			float c = dst[y * w + x];
			float d = (y + 1 < h) ? dst[(y + 1) * w + x] : 1.0f;
			src[y * w + x] = (u + 2.0f * c + d) * 0.25f;
		}
	}
}

void vsm_filter(vsm_map_t *map, int passes) {
	size_t n = (size_t)map->width * (size_t)map->height;
	int i;
	if (map == NULL || passes < 0) return;
	// 始终从原始矩快照开始，重复调用相同轮数结果一致
	memcpy(map->tmp1, map->raw1, n * sizeof(float));
	memcpy(map->tmp2, map->raw2, n * sizeof(float));
	for (i = 0; i < passes; i++) {
		convolve_pass(map->tmp1, map->work1, map->width, map->height);
		convolve_pass(map->tmp2, map->work2, map->width, map->height);
	}
	memcpy(map->m1, map->tmp1, n * sizeof(float));
	memcpy(map->m2, map->tmp2, n * sizeof(float));
}

float vsm_depth(const vsm_map_t *map, int x, int y) {
	if (map == NULL || (unsigned)x >= (unsigned)map->width ||
		(unsigned)y >= (unsigned)map->height) return 1.0f;
	return map->depth[y * map->width + x];
}

float vsm_raw_moment(const vsm_map_t *map, int x, int y, int moment) {
	const float *base;
	if (map == NULL || (unsigned)x >= (unsigned)map->width ||
		(unsigned)y >= (unsigned)map->height) return 1.0f;
	base = (moment == 2) ? map->raw2 : map->raw1;
	return base[y * map->width + x];
}

float vsm_moment(const vsm_map_t *map, int x, int y, int moment) {
	const float *base;
	if (map == NULL || (unsigned)x >= (unsigned)map->width ||
		(unsigned)y >= (unsigned)map->height) return 1.0f;
	base = (moment == 2) ? map->m2 : map->m1;
	return base[y * map->width + x];
}

//------------------------------------------------------------
// 采样：先双线性插值过滤后矩，再计算概率
//------------------------------------------------------------
static void filtered_tap(const vsm_map_t *map, int x, int y,
	float *a, float *b) {
	if ((unsigned)x >= (unsigned)map->width ||
		(unsigned)y >= (unsigned)map->height) {
		*a = 1.0f;	// 越界 tap 用 (1,1)
		*b = 1.0f;
		return;
	}
	{
		size_t idx = (size_t)y * (size_t)map->width + (size_t)x;
		*a = map->m1[idx];
		*b = map->m2[idx];
	}
}

float vsm_sample(const vsm_map_t *map, float u, float v, float t,
	float bias, float min_variance, float bleed) {
	float gx, gy, fx, fy, m1, m2, variance, p;
	int x0, y0;
	if (map == NULL) return 1.0f;
	if (!isfinite(u) || !isfinite(v) || !isfinite(t) ||
		!isfinite(bias) || !isfinite(min_variance) || !isfinite(bleed))
		return 1.0f;
	// 光图外或深度域外按全亮
	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return 1.0f;
	if (t < 0.0f || t > 1.0f) return 1.0f;

	t -= bias;
	if (t <= 0.0f) return 1.0f;

	// 纹素中心 ((x+.5)/width, (y+.5)/height)
	gx = u * (float)map->width - 0.5f;
	gy = v * (float)map->height - 0.5f;
	x0 = (int)floorf(gx);
	y0 = (int)floorf(gy);
	fx = gx - (float)x0;
	fy = gy - (float)y0;

	{
		float m00_1, m00_2, m10_1, m10_2, m01_1, m01_2, m11_1, m11_2;
		float w00 = (1.0f - fx) * (1.0f - fy);
		float w10 = fx * (1.0f - fy);
		float w01 = (1.0f - fx) * fy;
		float w11 = fx * fy;
		filtered_tap(map, x0, y0, &m00_1, &m00_2);
		filtered_tap(map, x0 + 1, y0, &m10_1, &m10_2);
		filtered_tap(map, x0, y0 + 1, &m01_1, &m01_2);
		filtered_tap(map, x0 + 1, y0 + 1, &m11_1, &m11_2);
		m1 = w00 * m00_1 + w10 * m10_1 + w01 * m01_1 + w11 * m11_1;
		m2 = w00 * m00_2 + w10 * m10_2 + w01 * m01_2 + w11 * m11_2;
	}

	if (t <= m1) return 1.0f;	// 接收面不深于阴影图分布，全亮

	variance = m2 - m1 * m1;
	if (variance < min_variance) variance = min_variance;
	p = variance / (variance + (t - m1) * (t - m1));
	if (bleed > 0.0f) p = (p - bleed) / (1.0f - bleed);
	if (p < 0.0f) p = 0.0f;
	if (p > 1.0f) p = 1.0f;
	return p;
}

// device 侧回调签名适配：vsm_user 只存指针，具体类型在此解释
static float vsm_sample_trampoline(const void *user, float u, float v,
	float t, float bias, float min_variance, float bleed) {
	return vsm_sample((const vsm_map_t*)user, u, v, t, bias,
		min_variance, bleed);
}

void device_bind_vsm(device_t *device, vsm_map_t *map,
	const matrix_t *object_to_light, float bias, float min_variance,
	float bleed, float ambient) {
	if (device == NULL || map == NULL || object_to_light == NULL) return;
	if (!isfinite(bias) || !isfinite(min_variance) ||
		!isfinite(bleed) || !isfinite(ambient)) return;
	if (bias < 0.0f || min_variance <= 0.0f ||
		bleed < 0.0f || bleed >= 1.0f || ambient < 0.0f || ambient > 1.0f)
		return;
	device->vsm_user = map;
	device->vsm_sample = vsm_sample_trampoline;
	device->vsm_light = *object_to_light;
	device->vsm_bias = bias;
	device->vsm_min_variance = min_variance;
	device->vsm_bleed = bleed;
	device->vsm_ambient = ambient;
}

void device_unbind_vsm(device_t *device) {
	if (device == NULL) return;
	device->vsm_user = NULL;
	device->vsm_sample = NULL;
	device->vsm_bias = 0.0f;
	device->vsm_min_variance = 1e-6f;
	device->vsm_bleed = 0.0f;
	device->vsm_ambient = 0.0f;
	matrix_set_zero(&device->vsm_light);
}
