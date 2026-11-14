//=====================================================================
//
// vsm_target.c - VSM 任务测试：矩捕获、矩过滤、相机着色三组用例
//
// 不定义 main，由 task_support/driver.c 驱动；真实数值判断一律走
// TASK_CHECK。
//
//=====================================================================
#include "../task_support/check.h"
#include "../vsm.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

static int nearly(float a, float b, float eps) {
	return fabsf(a - b) <= eps;
}

// 构造覆盖整张光图的四边形（两个三角形），深度统一为 z
static void full_quad(vertex_t *tris, float z) {
	vertex_t q[4] = {
		{ { -1, -1, z, 1 }, { 0, 0 }, { 1, 1, 1 }, 0, { 0, 0, 0, 1 } },
		{ {  1, -1, z, 1 }, { 1, 0 }, { 1, 1, 1 }, 0, { 0, 0, 0, 1 } },
		{ {  1,  1, z, 1 }, { 1, 1 }, { 1, 1, 1 }, 0, { 0, 0, 0, 1 } },
		{ { -1,  1, z, 1 }, { 0, 1 }, { 1, 1, 1 }, 0, { 0, 0, 0, 1 } },
	};
	tris[0] = q[0]; tris[1] = q[1]; tris[2] = q[2];
	tris[3] = q[0]; tris[4] = q[2]; tris[5] = q[3];
}

// 世界坐标四边形（用于相机透视场景）
static void world_quad(vertex_t *tris, float x0, float y0, float x1,
	float y1, float z, color_t color) {
	vertex_t q[4];
	int i;
	float xs[4] = { x0, x1, x1, x0 };
	float ys[4] = { y0, y0, y1, y1 };
	for (i = 0; i < 4; i++) {
		memset(&q[i], 0, sizeof(q[i]));
		q[i].pos.x = xs[i];
		q[i].pos.y = ys[i];
		q[i].pos.z = z;
		q[i].pos.w = 1.0f;
		q[i].tc.u = (i == 1 || i == 2) ? 1.0f : 0.0f;
		q[i].tc.v = (i >= 2) ? 1.0f : 0.0f;
		q[i].color = color;
		q[i].lc.w = 1.0f;
	}
	tris[0] = q[0]; tris[1] = q[1]; tris[2] = q[2];
	tris[3] = q[0]; tris[4] = q[2]; tris[5] = q[3];
}

static void draw_tris(device_t *d, const vertex_t *tris, size_t count) {
	size_t i;
	for (i = 0; i < count; i++)
		device_draw_primitive(d, &tris[i * 3], &tris[i * 3 + 1],
			&tris[i * 3 + 2]);
}

//=====================================================================
// moment_capture
//=====================================================================

// 斜平面：光图像素下 z(X,Y)=0.2+0.05X+0.025Y，手算梯度矩
static int case_sloped_plane(void) {
	// 屏幕点 (0,8)->z.4 (0,0)->z.2 (8,8)->z.8，w 恒为 1
	vertex_t tris[3];
	matrix_t m;
	vsm_map_t *map;
	float z, m2, expect_m2;
	matrix_set_identity(&m);
	memset(tris, 0, sizeof(tris));
	tris[0].pos = (point_t){ -1, -1, 0.4f, 1 };	// X=0,Y=8
	tris[1].pos = (point_t){  1, -1, 0.8f, 1 };	// X=8,Y=8
	tris[2].pos = (point_t){ -1,  1, 0.2f, 1 };	// X=0,Y=0
	tris[0].lc.w = tris[1].lc.w = tris[2].lc.w = 1.0f;

	map = vsm_create(8, 8);
	TASK_CHECK(map != NULL);
	vsm_capture(map, &m, tris, 1);

	// 生产扫描线在列 x、行中心 y+.5 处采样：(2,3) 对应 (X=2,Y=3.5)，
	// z=.2+.05*2+.025*3.5=.3875
	z = vsm_depth(map, 2, 3);
	TASK_CHECK(nearly(z, 0.3875f, 1e-5f));
	TASK_CHECK(nearly(vsm_raw_moment(map, 2, 3, 1), 0.3875f, 1e-5f));
	// M2=z^2+(dz/dx)^2/4+(dz/dy)^2/4，dz/dx=.05、dz/dy=.025
	expect_m2 = 0.3875f * 0.3875f + 0.05f * 0.05f * 0.25f +
		0.025f * 0.025f * 0.25f;
	m2 = vsm_raw_moment(map, 2, 3, 2);
	TASK_CHECK(nearly(m2, expect_m2, 1e-5f));

	// 三角形只覆盖 X<=Y 区域，(6,1) 未覆盖，保持清图值 1
	TASK_CHECK(nearly(vsm_depth(map, 6, 1), 1.0f, 1e-6f));
	TASK_CHECK(nearly(vsm_raw_moment(map, 6, 1, 1), 1.0f, 1e-6f));
	TASK_CHECK(nearly(vsm_raw_moment(map, 6, 1, 2), 1.0f, 1e-6f));
	vsm_destroy(map);
	return 0;
}

// 穿越左右裁剪面的三角形不能被整面丢掉
static int case_clip_planes(void) {
	vertex_t tris[3];
	matrix_t m;
	vsm_map_t *map;
	int y;
	matrix_set_identity(&m);
	memset(tris, 0, sizeof(tris));
	tris[0].pos = (point_t){ -2.0f, -1.0f, 0.5f, 1 };
	tris[1].pos = (point_t){  2.0f, -1.0f, 0.5f, 1 };
	tris[2].pos = (point_t){ -2.0f,  1.0f, 0.5f, 1 };
	tris[0].lc.w = tris[1].lc.w = tris[2].lc.w = 1.0f;

	map = vsm_create(8, 8);
	vsm_capture(map, &m, tris, 1);
	// 裁出的可见部分没有被整面丢掉：每行最左列都保留，
	// 底部两行铺满整个光图宽度，深度为三角形平面的 0.5
	for (y = 2; y < 8; y++) {
		TASK_CHECK(nearly(vsm_depth(map, 0, y), 0.5f, 1e-6f));
		TASK_CHECK(nearly(vsm_raw_moment(map, 0, y, 1), 0.5f, 1e-6f));
	}
	TASK_CHECK(nearly(vsm_depth(map, 7, 7), 0.5f, 1e-6f));
	TASK_CHECK(nearly(vsm_depth(map, 7, 6), 0.5f, 1e-6f));
	// 顶部两行确实在裁剪多边形之外，保持清图值
	TASK_CHECK(nearly(vsm_depth(map, 0, 0), 1.0f, 1e-6f));
	TASK_CHECK(nearly(vsm_depth(map, 0, 1), 1.0f, 1e-6f));
	vsm_destroy(map);
	return 0;
}

// 不同深度遮挡者：保留最小 z，且与绘制顺序无关；输入不被改写
static int case_occluder_depths(void) {
	vertex_t far_q[6], near_q[6], far_copy[6];
	vertex_t tris[12];
	matrix_t m;
	vsm_map_t *map;
	matrix_set_identity(&m);
	full_quad(far_q, 0.7f);
	full_quad(near_q, 0.3f);
	memcpy(far_copy, far_q, sizeof(far_q));

	// 先远后近
	map = vsm_create(4, 4);
	memcpy(tris, far_q, sizeof(far_q));
	memcpy(tris + 6, near_q, sizeof(near_q));
	vsm_capture(map, &m, tris, 4);
	TASK_CHECK(nearly(vsm_depth(map, 1, 1), 0.3f, 1e-6f));
	TASK_CHECK(nearly(vsm_raw_moment(map, 2, 2, 1), 0.3f, 1e-6f));
	TASK_CHECK(nearly(vsm_raw_moment(map, 2, 2, 2), 0.09f, 1e-6f));
	vsm_destroy(map);

	// 先近后远，结论不变
	map = vsm_create(4, 4);
	memcpy(tris, near_q, sizeof(near_q));
	memcpy(tris + 6, far_q, sizeof(far_q));
	vsm_capture(map, &m, tris, 4);
	TASK_CHECK(nearly(vsm_depth(map, 1, 1), 0.3f, 1e-6f));
	vsm_destroy(map);

	// 输入三角形未被改写
	TASK_CHECK(memcmp(far_copy, far_q, sizeof(far_q)) == 0);

	// 清图复位为 1
	map = vsm_create(4, 4);
	vsm_capture(map, &m, near_q, 2);
	vsm_clear(map);
	TASK_CHECK(nearly(vsm_depth(map, 0, 0), 1.0f, 1e-6f));
	TASK_CHECK(nearly(vsm_raw_moment(map, 0, 0, 1), 1.0f, 1e-6f));
	TASK_CHECK(nearly(vsm_raw_moment(map, 0, 0, 2), 1.0f, 1e-6f));
	vsm_destroy(map);
	return 0;
}

//=====================================================================
// moment_filter
//=====================================================================

// 测试侧独立 [1,2,1]/4 横纵卷积，越界 tap 取 1
static void independent_convolve(const float *src, float *dst, int w, int h) {
	float *tmp = (float*)malloc((size_t)w * (size_t)h * sizeof(float));
	int x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			float l = (x > 0) ? src[y * w + x - 1] : 1.0f;
			float c = src[y * w + x];
			float r = (x + 1 < w) ? src[y * w + x + 1] : 1.0f;
			tmp[y * w + x] = (l + 2.0f * c + r) * 0.25f;
		}
	}
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			float u = (y > 0) ? tmp[(y - 1) * w + x] : 1.0f;
			float c = tmp[y * w + x];
			float d = (y + 1 < h) ? tmp[(y + 1) * w + x] : 1.0f;
			dst[y * w + x] = (u + 2.0f * c + d) * 0.25f;
		}
	}
	free(tmp);
}

static int case_filter_moments(void) {
	vertex_t tris[6];
	matrix_t m;
	vsm_map_t *map;
	float base1[16], base2[16], expect1[16], expect2[16];
	float cur1[16], cur2[16];
	int x, y, pass;
	matrix_set_identity(&m);
	full_quad(tris, 0.5f);	// 覆盖像素 raw1=.5、raw2=.25
	map = vsm_create(4, 4);
	vsm_capture(map, &m, tris, 2);

	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++) {
			base1[y * 4 + x] = vsm_raw_moment(map, x, y, 1);
			base2[y * 4 + x] = vsm_raw_moment(map, x, y, 2);
			TASK_CHECK(nearly(base1[y * 4 + x], 0.5f, 1e-6f));
		}

	// passes=0 复制原始矩
	vsm_filter(map, 0);
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++) {
			TASK_CHECK(nearly(vsm_moment(map, x, y, 1), base1[y * 4 + x], 1e-6f));
			TASK_CHECK(nearly(vsm_moment(map, x, y, 2), base2[y * 4 + x], 1e-6f));
		}

	memcpy(cur1, base1, sizeof(base1));
	memcpy(cur2, base2, sizeof(base2));
	for (pass = 1; pass <= 3; pass++) {
		independent_convolve(cur1, expect1, 4, 4);
		independent_convolve(cur2, expect2, 4, 4);
		memcpy(cur1, expect1, sizeof(cur1));
		memcpy(cur2, expect2, sizeof(cur2));
		vsm_filter(map, pass);	// 每次都从原始矩快照重新过滤
		for (y = 0; y < 4; y++)
			for (x = 0; x < 4; x++) {
				TASK_CHECK(nearly(vsm_moment(map, x, y, 1),
					cur1[y * 4 + x], 1e-6f));
				TASK_CHECK(nearly(vsm_moment(map, x, y, 2),
					cur2[y * 4 + x], 1e-6f));
			}
	}

	// 重复调用相同轮数结果一致，不继续模糊上次结果
	vsm_filter(map, 2);
	{
		float snap1[16], snap2[16];
		for (y = 0; y < 4; y++)
			for (x = 0; x < 4; x++) {
				snap1[y * 4 + x] = vsm_moment(map, x, y, 1);
				snap2[y * 4 + x] = vsm_moment(map, x, y, 2);
			}
		vsm_filter(map, 2);
		for (y = 0; y < 4; y++)
			for (x = 0; x < 4; x++) {
				TASK_CHECK(nearly(vsm_moment(map, x, y, 1),
					snap1[y * 4 + x], 1e-6f));
				TASK_CHECK(nearly(vsm_moment(map, x, y, 2),
					snap2[y * 4 + x], 1e-6f));
			}
	}
	vsm_destroy(map);
	return 0;
}

// “先比较再过滤”与“先过滤矩再算概率”在边界处必然不同
static int case_compare_first_differs(void) {
	// 4x4 光图，左半列放置 z=.2 的遮挡者，右半保持空 (1,1)
	vertex_t tris[6];
	matrix_t m;
	vsm_map_t *map;
	float depth[16], hard[16], hard_f[16], m1f[16], m2f[16];
	float raw1[16], raw2[16];
	int x, y;
	float u, v, t, min_var, p_hard, p_vsm, m1, m2, variance, fx, fy, gx, gy;
	int x0, y0;

	matrix_set_identity(&m);
	memset(tris, 0, sizeof(tris));
	tris[0].pos = (point_t){ -1, -1, 0.2f, 1 };
	tris[1].pos = (point_t){  0, -1, 0.2f, 1 };
	tris[2].pos = (point_t){  0,  1, 0.2f, 1 };
	tris[3] = tris[0];
	tris[4] = tris[2];
	tris[5].pos = (point_t){ -1,  1, 0.2f, 1 };
	tris[5].lc.w = 1.0f;
	tris[0].lc.w = tris[1].lc.w = tris[2].lc.w = tris[3].lc.w =
		tris[4].lc.w = 1.0f;

	map = vsm_create(4, 4);
	vsm_capture(map, &m, tris, 2);
	vsm_filter(map, 1);

	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++) {
			depth[y * 4 + x] = vsm_depth(map, x, y);
			raw1[y * 4 + x] = vsm_raw_moment(map, x, y, 1);
			raw2[y * 4 + x] = vsm_raw_moment(map, x, y, 2);
		}

	// 硬阴影先比较：lit = t<=depth，再做同一轮卷积
	t = 0.6f;
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			hard[y * 4 + x] = (t <= depth[y * 4 + x]) ? 1.0f : 0.0f;
	independent_convolve(hard, hard_f, 4, 4);
	independent_convolve(raw1, m1f, 4, 4);
	independent_convolve(raw2, m2f, 4, 4);

	// 在边界纹素之间双线性采样
	u = 0.5f;	// 光图 u=.5 对应列分界 X=2
	v = 0.5f;
	min_var = 1e-4f;
	gx = u * 4.0f - 0.5f;
	gy = v * 4.0f - 0.5f;
	x0 = (int)floorf(gx);
	y0 = (int)floorf(gy);
	fx = gx - (float)x0;
	fy = gy - (float)y0;

	{
		float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
		float w01 = (1 - fx) * fy, w11 = fx * fy;
		int taps[4][2] = { { x0, y0 }, { x0 + 1, y0 },
			{ x0, y0 + 1 }, { x0 + 1, y0 + 1 } };
		float ws[4] = { w00, w10, w01, w11 };
		int k;
		p_hard = 0.0f; m1 = 0.0f; m2 = 0.0f;
		for (k = 0; k < 4; k++) {
			int tx = taps[k][0], ty = taps[k][1];
			float hv = (tx < 0 || tx >= 4 || ty < 0 || ty >= 4) ?
				1.0f : hard_f[ty * 4 + tx];
			float a = (tx < 0 || tx >= 4 || ty < 0 || ty >= 4) ?
				1.0f : m1f[ty * 4 + tx];
			float b = (tx < 0 || tx >= 4 || ty < 0 || ty >= 4) ?
				1.0f : m2f[ty * 4 + tx];
			p_hard += ws[k] * hv;
			m1 += ws[k] * a;
			m2 += ws[k] * b;
		}
	}
	variance = m2 - m1 * m1;
	if (variance < min_var) variance = min_var;
	p_vsm = variance / (variance + (t - m1) * (t - m1));

	// 库接口必须与独立 VSM 计算一致
	TASK_CHECK(nearly(vsm_sample(map, u, v, t, 0.0f, min_var, 0.0f),
		p_vsm, 1e-5f));
	// 两种路径给出可观测的不同结果
	TASK_CHECK(fabsf(p_hard - p_vsm) > 0.05f);
	vsm_destroy(map);
	return 0;
}

//=====================================================================
// camera_shading
//=====================================================================

// 透视相机（非等 w）下的纹理优先级、光坐标插值与后方遮挡
static int case_perspective_shading(void) {
	device_t d;
	vertex_t recv[6], rear[6], occ[6];
	matrix_t light;
	vsm_map_t *map;
	IUINT32 tex[4] = { 0x808080, 0x808080, 0x808080, 0x808080 };
	point_t c1, c2, c3;
	unsigned center, lit, rear_pixel;

	device_init(&d, 32, 24, NULL);
	transform_init(&d.transform, 32, 24);	// 透视投影，w=z 非 1
	world_quad(recv, -3, -3, 3, 3, 4.0f, (color_t){ 1, 1, 1 });
	world_quad(rear, -3, -3, 3, 3, 6.0f, (color_t){ 1, 0, 0 });
	world_quad(occ, -1.5f, -1.5f, 1.5f, 1.5f, 2.0f, (color_t){ 1, 1, 1 });

	// 确认相机齐次 w 非 1 且顶点间不等
	transform_apply(&d.transform, &c1, &recv[0].pos);
	transform_apply(&d.transform, &c2, &occ[0].pos);
	transform_apply(&d.transform, &c3, &recv[1].pos);
	TASK_CHECK(nearly(c1.w, 4.0f, 1e-4f));
	TASK_CHECK(nearly(c2.w, 2.0f, 1e-4f));
	TASK_CHECK(fabsf(c1.w - 1.0f) > 1e-3f);

	// 光矩阵：lx=.5x、ly=.5y、lz=.2z+.1
	matrix_set_scale(&light, 0.5f, 0.5f, 0.2f);
	light.m[3][2] = 0.1f;

	map = vsm_create(16, 16);
	vsm_capture(map, &light, occ, 2);
	vsm_filter(map, 2);

	d.background = 0;
	device_clear(&d, 0);
	device_set_texture(&d, tex, 8, 2, 2);
	d.render_state = RENDER_STATE_TEXTURE | RENDER_STATE_COLOR;
	device_bind_vsm(&d, map, &light, 0.0f, 1e-4f, 0.0f, 0.0f);

	// 空光图区域全亮时纹理优先，顶点底色不参与
	draw_tris(&d, recv, 2);
	lit = d.framebuffer[12][8];
	TASK_CHECK(lit == 0x808080);

	// 中心落在遮挡者阴影里，纹理被压暗
	center = d.framebuffer[12][16];
	TASK_CHECK(center < 0x808080);
	TASK_CHECK((center & 0xff) < 0x80);

	// 后方物体不能通过主相机深度测试
	draw_tris(&d, rear, 2);
	TASK_CHECK(d.framebuffer[12][16] == center);

	// 清掉接收面后，后方红色物体自己可以绘制
	device_clear(&d, 0);
	d.render_state = RENDER_STATE_COLOR;
	draw_tris(&d, rear, 2);
	rear_pixel = d.framebuffer[12][16];
	TASK_CHECK((rear_pixel >> 16) > (rear_pixel & 0xff));

	// 解绑后恢复全亮调制：白色接收面量化为纯白
	device_unbind_vsm(&d);
	device_clear(&d, 0);
	draw_tris(&d, recv, 2);
	TASK_CHECK(d.framebuffer[12][16] == 0xffffff);
	TASK_CHECK(d.framebuffer[12][8] == 0xffffff);

	vsm_destroy(map);
	device_destroy(&d);
	return 0;
}

// 两台设备绑定不同阴影图，状态互不串扰
static int case_two_devices(void) {
	device_t a, b;
	vertex_t recv[6], occ[6];
	matrix_t light;
	vsm_map_t *map_shadow, *map_empty;

	device_init(&a, 32, 24, NULL);
	device_init(&b, 32, 24, NULL);
	transform_init(&a.transform, 32, 24);
	transform_init(&b.transform, 32, 24);
	world_quad(recv, -3, -3, 3, 3, 4.0f, (color_t){ 1, 1, 1 });
	world_quad(occ, -1.5f, -1.5f, 1.5f, 1.5f, 2.0f, (color_t){ 1, 1, 1 });
	matrix_set_scale(&light, 0.5f, 0.5f, 0.2f);
	light.m[3][2] = 0.1f;

	map_shadow = vsm_create(16, 16);
	vsm_capture(map_shadow, &light, occ, 2);
	vsm_filter(map_shadow, 2);
	map_empty = vsm_create(16, 16);	// 只有清图值
	vsm_filter(map_empty, 2);

	a.background = b.background = 0;
	a.render_state = b.render_state = RENDER_STATE_COLOR;
	device_bind_vsm(&a, map_shadow, &light, 0.0f, 1e-4f, 0.2f, 0.1f);
	device_bind_vsm(&b, map_empty, &light, 0.0f, 1e-4f, 0.0f, 0.0f);

	device_clear(&a, 0);
	device_clear(&b, 0);
	draw_tris(&a, recv, 2);
	draw_tris(&b, recv, 2);

	// A 中心有阴影；B 绑定空图，全亮
	TASK_CHECK(a.framebuffer[12][16] < 0xffffff);
	TASK_CHECK(b.framebuffer[12][16] == 0xffffff);
	// A 的绑定参数没有污染 B：B 中心严格等于纯白而非环境光抬升
	TASK_CHECK((b.framebuffer[12][16] & 0xff) == 255);

	// 解绑 A 不影响 B
	device_unbind_vsm(&a);
	TASK_CHECK(a.vsm_user == NULL);
	TASK_CHECK(b.vsm_user == map_empty);
	device_clear(&b, 0);
	draw_tris(&b, recv, 2);
	TASK_CHECK(b.framebuffer[12][16] == 0xffffff);

	vsm_destroy(map_shadow);
	vsm_destroy(map_empty);
	device_destroy(&a);
	device_destroy(&b);
	return 0;
}

// 越界与无效参数按全亮处理，ambient 抬升生效
static int case_sampling_edges(void) {
	vsm_map_t *map = vsm_create(4, 4);
	vsm_filter(map, 0);
	TASK_CHECK(nearly(vsm_sample(map, 0.5f, 0.5f, 0.5f, 0.0f, 1e-4f, 0.0f),
		1.0f, 1e-6f));	// 空图 M1=1，t<=M1 全亮
	TASK_CHECK(nearly(vsm_sample(map, -0.1f, 0.5f, 0.5f, 0.0f, 1e-4f, 0.0f),
		1.0f, 1e-6f));	// 光图外
	TASK_CHECK(nearly(vsm_sample(map, 0.5f, 0.5f, 1.2f, 0.0f, 1e-4f, 0.0f),
		1.0f, 1e-6f));	// 深度域外
	TASK_CHECK(nearly(vsm_sample(NULL, 0.5f, 0.5f, 0.5f, 0.0f, 1e-4f, 0.0f),
		1.0f, 1e-6f));
	vsm_destroy(map);
	return 0;
}

const struct task_case task_cases[] = {
	{ "moment_capture", "sloped_plane", case_sloped_plane },
	{ "moment_capture", "clip_planes", case_clip_planes },
	{ "moment_capture", "occluder_depths", case_occluder_depths },
	{ "moment_filter", "filter_moments", case_filter_moments },
	{ "moment_filter", "compare_first_differs", case_compare_first_differs },
	{ "camera_shading", "perspective_shading", case_perspective_shading },
	{ "camera_shading", "two_devices", case_two_devices },
	{ "camera_shading", "sampling_edges", case_sampling_edges },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
