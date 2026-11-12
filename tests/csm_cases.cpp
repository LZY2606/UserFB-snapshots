//=====================================================================
//
// csm_cases.cpp - CSM 数值检查：cascade_fit / light_depth_clip /
//                 stable_transition / camera_shadow_integration
//
// 场景全部由代码生成：地面（网格）、近/远两个接收面、一个相机外但
// 能投影进接收区的遮挡物；纹理由代码生成的棋盘格。
//
//=====================================================================
#include "checks.h"
#include "mini3d.h"
#include "csm.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

//---------------------------------------------------------------------
// 场景常量
//---------------------------------------------------------------------
static const int SCR_W = 320, SCR_H = 240, MAP_SIZE = 256;
static const float FOVY = 3.1415926f / 3.0f;	// 60 度
static const float ZN = 1.0f, ZF = 50.0f;
static const float LAMBDA = 0.5f, AMBIENT = 0.3f, BIAS = 0.2f;
static const float ASPECT = (float)SCR_W / (float)SCR_H;

static const point_t CAM_EYE = { 0.0f, 4.0f, -6.0f, 1.0f };
static const point_t CAM_AT  = { 0.0f, 0.0f,  8.0f, 1.0f };
static const point_t CAM_UP  = { 0.0f, 1.0f,  0.0f, 1.0f };
static const vector_t LIGHT_DIR = { -0.35f, -1.0f, 0.30f, 0.0f };

// 遮挡物：水平方板，位于相机视锥外（右侧），影子投进画面
static const float OC_X0 = 6.5f, OC_X1 = 9.5f;
static const float OC_Z0 = 4.5f, OC_Z1 = 7.5f;
static const float OC_Y  = 7.0f;

struct Scene {
	std::vector<vertex_t> tris;		// 主 pass 绘制（地面 + 两个接收面）
	std::vector<point_t> occluders;	// 阴影 pass（地面 + 离屏遮挡物）
};

static vertex_t make_vertex(float x, float y, float z,
	float r, float g, float b, float u, float v) {
	vertex_t vt;
	vt.pos.x = x; vt.pos.y = y; vt.pos.z = z; vt.pos.w = 1.0f;
	vt.color.r = r; vt.color.g = g; vt.color.b = b;
	vt.tc.u = u; vt.tc.v = v;
	vt.rhw = 1.0f;
	vt.world.x = 0; vt.world.y = 0; vt.world.z = 0; vt.world.w = 1;
	return vt;
}

static void push_quad(std::vector<vertex_t> &out, const vertex_t &a,
	const vertex_t &b, const vertex_t &c, const vertex_t &d) {
	out.push_back(a); out.push_back(b); out.push_back(c);
	out.push_back(a); out.push_back(c); out.push_back(d);
}

static void push_occ_quad(std::vector<point_t> &out,
	float x0, float y0, float z0, float x1, float y1, float z1,
	float x2, float y2, float z2, float x3, float y3, float z3) {
	point_t p[4];
	p[0].x = x0; p[0].y = y0; p[0].z = z0; p[0].w = 1;
	p[1].x = x1; p[1].y = y1; p[1].z = z1; p[1].w = 1;
	p[2].x = x2; p[2].y = y2; p[2].z = z2; p[2].w = 1;
	p[3].x = x3; p[3].y = y3; p[3].z = z3; p[3].w = 1;
	out.push_back(p[0]); out.push_back(p[1]); out.push_back(p[2]);
	out.push_back(p[0]); out.push_back(p[2]); out.push_back(p[3]);
}

static void build_scene(Scene &sc) {
	float gx, gz;
	// 地面：2 单位网格，y=0，颜色均匀，UV 平铺
	for (gx = -40.0f; gx < 40.0f; gx += 2.0f) {
		for (gz = -20.0f; gz < 80.0f; gz += 2.0f) {
			vertex_t v[4];
			float x0 = gx, x1 = gx + 2.0f, z0 = gz, z1 = gz + 2.0f;
			v[0] = make_vertex(x0, 0, z0, 0.6f, 0.6f, 0.6f,
				(x0 + 40.0f) / 160.0f, (z0 + 20.0f) / 200.0f);
			v[1] = make_vertex(x1, 0, z0, 0.6f, 0.6f, 0.6f,
				(x1 + 40.0f) / 160.0f, (z0 + 20.0f) / 200.0f);
			v[2] = make_vertex(x1, 0, z1, 0.6f, 0.6f, 0.6f,
				(x1 + 40.0f) / 160.0f, (z1 + 20.0f) / 200.0f);
			v[3] = make_vertex(x0, 0, z1, 0.6f, 0.6f, 0.6f,
				(x0 + 40.0f) / 160.0f, (z1 + 20.0f) / 200.0f);
			push_quad(sc.tris, v[0], v[1], v[2], v[3]);
			push_occ_quad(sc.occluders,
				x0, 0, z0, x1, 0, z0, x1, 0, z1, x0, 0, z1);
		}
	}
	// 近接收面：z=3 竖直四边形（级联 0）
	push_quad(sc.tris,
		make_vertex(-3, 0, 3, 0.8f, 0.3f, 0.3f, 0, 0),
		make_vertex(-1, 0, 3, 0.8f, 0.3f, 0.3f, 1, 0),
		make_vertex(-1, 3, 3, 0.8f, 0.3f, 0.3f, 1, 1),
		make_vertex(-3, 3, 3, 0.8f, 0.3f, 0.3f, 0, 1));
	// 远接收面：z=30 竖直四边形（级联 2）
	push_quad(sc.tris,
		make_vertex(-4, 0, 30, 0.3f, 0.8f, 0.3f, 0, 0),
		make_vertex(-1, 0, 30, 0.3f, 0.8f, 0.3f, 1, 0),
		make_vertex(-1, 4, 30, 0.3f, 0.8f, 0.3f, 1, 1),
		make_vertex(-4, 4, 30, 0.3f, 0.8f, 0.3f, 0, 1));
	// 离屏遮挡物：只进阴影 pass，不进主 pass
	push_occ_quad(sc.occluders,
		OC_X0, OC_Y, OC_Z0, OC_X1, OC_Y, OC_Z0,
		OC_X1, OC_Y, OC_Z1, OC_X0, OC_Y, OC_Z1);
}

//---------------------------------------------------------------------
// 工具
//---------------------------------------------------------------------
static void setup_camera(device_t *d, const point_t &eye, const point_t &at) {
	matrix_set_identity(&d->transform.world);
	matrix_set_lookat(&d->transform.view, &eye, &at, &CAM_UP);
	matrix_set_perspective(&d->transform.projection, FOVY, ASPECT, ZN, ZF);
	transform_update(&d->transform);
}

static void setup_csm(csm_t *csm, const Scene &sc, const matrix_t *view) {
	vector_t L = LIGHT_DIR;
	vector_normalize(&L);
	csm_update(csm, view, FOVY, ASPECT, ZN, ZF, &L, LAMBDA,
		sc.occluders.data(), (int)sc.occluders.size() / 3);
	csm_clear(csm);
	csm_render(csm, sc.occluders.data(), (int)sc.occluders.size() / 3);
}

static void draw_scene(device_t *d, const Scene &sc) {
	size_t i;
	for (i = 0; i + 2 < sc.tris.size(); i += 3)
		device_draw_primitive(d, &sc.tris[i], &sc.tris[i + 1], &sc.tris[i + 2]);
}

static void project_point(const device_t *d, const point_t &p,
	float &sx, float &sy) {
	vector_t c, s;
	matrix_apply(&c, &p, &d->transform.transform);
	transform_homogenize(&d->transform, &s, &c);
	sx = s.x; sy = s.y;
}

static float view_depth(const device_t *d, const point_t &p) {
	const matrix_t &v = d->transform.view;
	return p.x * v.m[0][2] + p.y * v.m[1][2] + p.z * v.m[2][2] + v.m[3][2];
}

// 解析阴影判定：从点向光源方向（-L）与遮挡板求交
static bool analytic_shadowed(float x, float y, float z) {
	vector_t L = LIGHT_DIR;
	float t, hx, hz;
	vector_normalize(&L);
	t = (y - OC_Y) / L.y;	// L.y < 0，y < OC_Y 时 t > 0
	if (t <= 0.0f) return false;
	hx = x - t * L.x;
	hz = z - t * L.z;
	return hx >= OC_X0 && hx <= OC_X1 && hz >= OC_Z0 && hz <= OC_Z1;
}

static void check_pixel(Checks &c, const device_t *d,
	float wx, float wy, float wz, int er, int eg, int eb, int tol) {
	point_t p;
	float sx, sy;
	int px, py, r, g, b;
	IUINT32 got;
	p.x = wx; p.y = wy; p.z = wz; p.w = 1.0f;
	project_point(d, p, sx, sy);
	px = (int)sx; py = (int)sy;
	c.require(px >= 0 && px < d->width && py >= 0 && py < d->height);
	got = d->framebuffer[py][px];
	r = (got >> 16) & 0xff; g = (got >> 8) & 0xff; b = got & 0xff;
	c.require(std::abs(r - er) <= tol);
	c.require(std::abs(g - eg) <= tol);
	c.require(std::abs(b - eb) <= tol);
}

static void write_ppm(const char *path, int w, int h, const IUINT32 *px) {
	FILE *f = std::fopen(path, "wb");
	int i;
	if (!f) return;
	std::fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (i = 0; i < w * h; i++) {
		unsigned char c[3];
		c[0] = (unsigned char)((px[i] >> 16) & 0xff);
		c[1] = (unsigned char)((px[i] >> 8) & 0xff);
		c[2] = (unsigned char)(px[i] & 0xff);
		std::fwrite(c, 1, 3, f);
	}
	std::fclose(f);
}

static void write_device_ppm(const char *path, const device_t *d) {
	std::vector<IUINT32> flat((size_t)d->width * d->height);
	int y;
	for (y = 0; y < d->height; y++)
		std::memcpy(&flat[(size_t)y * d->width], d->framebuffer[y],
			sizeof(IUINT32) * (size_t)d->width);
	write_ppm(path, d->width, d->height, flat.data());
}

static void write_depth_ppm(const char *path, const float *depth, int size) {
	std::vector<IUINT32> gray((size_t)size * size);
	int i;
	for (i = 0; i < size * size; i++) {
		float v = depth[i];
		int g;
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		g = (int)(v * 255.0f);
		gray[i] = (IUINT32)((g << 16) | (g << 8) | g);
	}
	write_ppm(path, size, size, gray.data());
}

// 视锥段角点（世界空间），与 csm_update 内部算法一致的独立实现
static void frustum_corners(const matrix_t *view, float d0, float d1,
	point_t out[8]) {
	// 从 lookat 矩阵提取相机基（列向量）与位置
	vector_t xa = { view->m[0][0], view->m[1][0], view->m[2][0], 0 };
	vector_t ya = { view->m[0][1], view->m[1][1], view->m[2][1], 0 };
	vector_t za = { view->m[0][2], view->m[1][2], view->m[2][2], 0 };
	point_t eye;
	float tan_half = (float)std::tan(FOVY * 0.5f);
	int k;
	eye.x = -(view->m[3][0] * xa.x + view->m[3][1] * ya.x + view->m[3][2] * za.x);
	eye.y = -(view->m[3][0] * xa.y + view->m[3][1] * ya.y + view->m[3][2] * za.y);
	eye.z = -(view->m[3][0] * xa.z + view->m[3][1] * ya.z + view->m[3][2] * za.z);
	eye.w = 1.0f;
	for (k = 0; k < 8; k++) {
		float d = (k & 1) ? d1 : d0;
		float hh = d * tan_half, hw = hh * ASPECT;
		float sx = (k & 2) ? hw : -hw;
		float sy = (k & 4) ? hh : -hh;
		out[k].x = eye.x + sx * xa.x + sy * ya.x + d * za.x;
		out[k].y = eye.y + sx * xa.y + sy * ya.y + d * za.y;
		out[k].z = eye.z + sx * xa.z + sy * ya.z + d * za.z;
		out[k].w = 1.0f;
	}
}

//---------------------------------------------------------------------
// cascade_fit：划分公式、角点包围、纹素吸附、过渡带覆盖、up 平行稳定
//---------------------------------------------------------------------
void cascade_fit(Checks &c) {
	Scene sc;
	device_t dev;
	csm_t csm;
	int i, k;
	build_scene(sc);
	device_init(&dev, SCR_W, SCR_H, NULL);
	setup_camera(&dev, CAM_EYE, CAM_AT);
	csm_init(&csm, MAP_SIZE);
	{
		vector_t L = LIGHT_DIR;
		vector_normalize(&L);
		csm_update(&csm, &dev.transform.view, FOVY, ASPECT, ZN, ZF, &L,
			LAMBDA, sc.occluders.data(), (int)sc.occluders.size() / 3);
	}

	// 分界公式：lambda 混合对数/均匀划分
	c.require(std::fabs(csm.splits[0] - ZN) < 1e-4f);
	for (i = 1; i <= 3; i++) {
		float uni = ZN + (ZF - ZN) * i / 3.0f;
		float logsp = ZN * std::pow(ZF / ZN, i / 3.0f);
		float expect = LAMBDA * logsp + (1.0f - LAMBDA) * uni;
		c.require(std::fabs(csm.splits[i] - expect) < 1e-3f);
	}
	c.require(std::fabs(csm.splits[3] - ZF) < 1e-3f);

	// 每级角点被方形正交投影包住；xy 等比（方形）；中心按纹素吸附
	for (i = 0; i < CSM_CASCADES; i++) {
		point_t corners[8];
		float texel = csm.ortho_extent[i] / MAP_SIZE;
		frustum_corners(&dev.transform.view, csm.splits[i], csm.splits[i + 1],
			corners);
		for (k = 0; k < 8; k++) {
			vector_t clip;
			matrix_apply(&clip, &corners[k], &csm.light_vp[i]);
			c.require(std::fabs(clip.x) <= 1.0f + 1e-3f);
			c.require(std::fabs(clip.y) <= 1.0f + 1e-3f);
			c.require(clip.z >= -1e-3f && clip.z <= 1.0f + 1e-3f);
		}
		c.require(std::fabs(csm.light_proj[i].m[0][0] -
			csm.light_proj[i].m[1][1]) < 1e-6f);	// 方形
		c.require(csm.ortho_extent[i] > 0.0f);
		{
			float qx = csm.ortho_center[i][0] / texel;
			float qy = csm.ortho_center[i][1] / texel;
			c.require(std::fabs(qx - std::floor(qx + 0.5f)) < 1e-3f);
			c.require(std::fabs(qy - std::floor(qy + 0.5f)) < 1e-3f);
		}
		c.require(csm.depth_max[i] > csm.depth_min[i]);
	}

	// 相邻级拟合范围覆盖过渡带：本段最后 10% 的角点落在下一级图内
	for (i = 0; i + 1 < CSM_CASCADES; i++) {
		float seg = csm.splits[i + 1] - csm.splits[i];
		float db = csm.splits[i + 1] - 0.1f * seg;
		point_t corners[8];
		frustum_corners(&dev.transform.view, db, db, corners);
		for (k = 0; k < 8; k++) {
			vector_t clip;
			matrix_apply(&clip, &corners[k], &csm.light_vp[i + 1]);
			c.require(std::fabs(clip.x) <= 1.0f + 1e-3f);
			c.require(std::fabs(clip.y) <= 1.0f + 1e-3f);
		}
	}

	// 光深范围包含能投影进接收区的离屏遮挡物（遮挡板全部顶点）
	{
		vector_t L = LIGHT_DIR;
		point_t occ[4];
		vector_normalize(&L);
		occ[0].x = OC_X0; occ[0].y = OC_Y; occ[0].z = OC_Z0; occ[0].w = 1;
		occ[1].x = OC_X1; occ[1].y = OC_Y; occ[1].z = OC_Z0; occ[1].w = 1;
		occ[2].x = OC_X1; occ[2].y = OC_Y; occ[2].z = OC_Z1; occ[2].w = 1;
		occ[3].x = OC_X0; occ[3].y = OC_Y; occ[3].z = OC_Z1; occ[3].w = 1;
		for (k = 0; k < 4; k++) {
			point_t lc;
			matrix_apply(&lc, &occ[k], &csm.light_view);
			// 遮挡板影子落在级联 1 范围，其光深必须被该级深度范围包含
			c.require(lc.z >= csm.depth_min[1] - 1e-3f);
			c.require(lc.z <= csm.depth_max[1] + 1e-3f);
		}
	}

	// 光方向几乎平行世界 up：基底仍稳定（正交归一、角点仍被包住）
	{
		csm_t cs2;
		vector_t L2 = { 0.0001f, -1.0f, 0.0001f, 0.0f };
		point_t corners[8];
		csm_init(&cs2, MAP_SIZE);
		csm_update(&cs2, &dev.transform.view, FOVY, ASPECT, ZN, ZF, &L2,
			LAMBDA, sc.occluders.data(), (int)sc.occluders.size() / 3);
		{
			vector_t r0 = { cs2.light_view.m[0][0], cs2.light_view.m[0][1],
				cs2.light_view.m[0][2], 0 };
			vector_t r1 = { cs2.light_view.m[1][0], cs2.light_view.m[1][1],
				cs2.light_view.m[1][2], 0 };
			vector_t r2 = { cs2.light_view.m[2][0], cs2.light_view.m[2][1],
				cs2.light_view.m[2][2], 0 };
			c.require(std::fabs(vector_length(&r0) - 1.0f) < 1e-3f);
			c.require(std::fabs(vector_length(&r1) - 1.0f) < 1e-3f);
			c.require(std::fabs(vector_length(&r2) - 1.0f) < 1e-3f);
			c.require(std::fabs(vector_dotproduct(&r0, &r1)) < 1e-3f);
			c.require(std::fabs(vector_dotproduct(&r0, &r2)) < 1e-3f);
			c.require(std::fabs(vector_dotproduct(&r1, &r2)) < 1e-3f);
		}
		frustum_corners(&dev.transform.view, cs2.splits[0], cs2.splits[1],
			corners);
		for (k = 0; k < 8; k++) {
			vector_t clip;
			matrix_apply(&clip, &corners[k], &cs2.light_vp[0]);
			c.require(clip.x == clip.x && clip.y == clip.y && clip.z == clip.z);
			c.require(std::fabs(clip.x) <= 1.0f + 1e-3f);
			c.require(std::fabs(clip.y) <= 1.0f + 1e-3f);
		}
		csm_destroy(&cs2);
	}

	// lambda 端点：0 为均匀划分，1 为对数划分
	{
		csm_t cs3;
		vector_t L = LIGHT_DIR;
		vector_normalize(&L);
		csm_init(&cs3, MAP_SIZE);
		csm_update(&cs3, &dev.transform.view, FOVY, ASPECT, ZN, ZF, &L,
			0.0f, sc.occluders.data(), (int)sc.occluders.size() / 3);
		c.require(std::fabs(cs3.splits[1] - (ZN + (ZF - ZN) / 3.0f)) < 1e-3f);
		csm_update(&cs3, &dev.transform.view, FOVY, ASPECT, ZN, ZF, &L,
			1.0f, sc.occluders.data(), (int)sc.occluders.size() / 3);
		c.require(std::fabs(cs3.splits[1] -
			ZN * std::pow(ZF / ZN, 1.0f / 3.0f)) < 1e-3f);
		csm_destroy(&cs3);
	}

	csm_destroy(&csm);
	device_destroy(&dev);
}

//---------------------------------------------------------------------
// light_depth_clip：正交不同深度、跨光图边界裁剪、z 裁剪、属性连续
//---------------------------------------------------------------------
void light_depth_clip(Checks &c) {
	csm_t csm;
	int i;
	csm_init(&csm, 16);
	// 手工正交：xy 直接落 [-1,1]，z∈[0,4] -> [0,1]
	for (i = 0; i < CSM_CASCADES; i++) {
		matrix_set_identity(&csm.light_vp[i]);
		csm.light_vp[i].m[2][2] = 0.25f;
		csm.depth_min[i] = 0.0f;
		csm.depth_max[i] = 4.0f;
	}
	csm.bias = 0.0f;

	// 正交不同深度：同一区域两个三角形，靠近光源者（z 小）保留，
	// 与提交顺序无关
	{
		point_t a1 = { -0.6f, -0.6f, 2.0f, 1 }, a2 = { 0.6f, -0.6f, 2.0f, 1 },
			a3 = { 0.0f, 0.6f, 2.0f, 1 };
		point_t b1 = { -0.6f, -0.6f, 1.0f, 1 }, b2 = { 0.6f, -0.6f, 1.0f, 1 },
			b3 = { 0.0f, 0.6f, 1.0f, 1 };
		csm_clear(&csm);
		csm_render_triangle(&csm, &a1, &a2, &a3);	// 远（z=2 -> 0.5）
		csm_render_triangle(&csm, &b1, &b2, &b3);	// 近（z=1 -> 0.25）
		c.require(std::fabs(csm.depth[0][8 * 16 + 8] - 0.25f) < 1e-4f);
		csm_clear(&csm);
		csm_render_triangle(&csm, &b1, &b2, &b3);	// 交换顺序
		csm_render_triangle(&csm, &a1, &a2, &a3);
		c.require(std::fabs(csm.depth[0][8 * 16 + 8] - 0.25f) < 1e-4f);
	}

	// 跨光图边界三角形：一个顶点在 x>1 之外，可绘部分必须保留，
	// 裁出的顶点光深连续（平面 z = 1 + 0.4*(x+0.5)）
	{
		point_t t1 = { -0.5f, -0.5f, 1.0f, 1 };
		point_t t2 = {  2.0f, -0.5f, 2.0f, 1 };	// 越界顶点
		point_t t3 = { -0.5f,  0.5f, 1.0f, 1 };
		csm_clear(&csm);
		csm_render_triangle(&csm, &t1, &t2, &t3);
		// 像素 (15,11)：ndc≈(0.9375,-0.4375)，z=1.575 -> 0.39375
		c.require(csm.depth[0][11 * 16 + 15] < 1.0f);	// 未被整面丢掉
		c.require(std::fabs(csm.depth[0][11 * 16 + 15] - 0.39375f) < 0.03f);
		// 三角形未覆盖处保持远值
		c.require(csm.depth[0][0 * 16 + 15] == 1.0f);
		c.require(csm.depth[0][8 * 16 + 2] == 1.0f);
	}

	// 完全在图外的三角形：不产生任何写入
	{
		point_t o1 = { 2.0f, 2.0f, 1.0f, 1 };
		point_t o2 = { 3.0f, 2.0f, 1.0f, 1 };
		point_t o3 = { 2.0f, 3.0f, 1.0f, 1 };
		csm_clear(&csm);
		csm_render_triangle(&csm, &o1, &o2, &o3);
		for (i = 0; i < 16 * 16; i++)
			if (csm.depth[0][i] != 1.0f) break;
		c.require(i == 16 * 16);
	}

	// 穿过 z=0 齐次平面的三角形：裁出部分光深从 0 连续插值
	{
		point_t z1 = { -0.5f, -0.5f, -1.0f, 1 };	// 光近平面之后
		point_t z2 = {  0.5f, -0.5f,  1.0f, 1 };
		point_t z3 = {  0.0f,  0.5f,  1.0f, 1 };
		csm_clear(&csm);
		csm_render_triangle(&csm, &z1, &z2, &z3);
		// 裁剪后平面 z = 2x + y + 0.5；像素 (8,11) 处 ndc≈0.0156
		c.require(csm.depth[0][11 * 16 + 8] < 0.03f);
		// 像素 (8,5)：ndc≈(0,0.3125)，z=0.8125 -> 0.203
		c.require(std::fabs(csm.depth[0][5 * 16 + 8] - 0.203f) < 0.03f);
	}
	csm_destroy(&csm);
}

//---------------------------------------------------------------------
// stable_transition：亚纹素相机平移不滑动 + 级联过渡线性混合
//---------------------------------------------------------------------
void stable_transition(Checks &c) {
	// 过渡混合：手工构造相邻两级结论相反的深度图，验证最后 10%
	// 视深范围内可见度线性混合，段内其余部分不混合，末级以外无遮挡
	{
		csm_t csm;
		point_t p = { 0.0f, 0.0f, 0.5f, 1.0f };	// 落在 texel (4,4)
		point_t far_away = { 100.0f, 0.0f, 0.5f, 1.0f };
		int i;
		csm_init(&csm, 8);
		csm.splits[0] = 1.0f; csm.splits[1] = 10.0f;
		csm.splits[2] = 20.0f; csm.splits[3] = 30.0f;
		for (i = 0; i < CSM_CASCADES; i++) {
			matrix_set_identity(&csm.light_vp[i]);
			csm.depth_min[i] = 0.0f;
			csm.depth_max[i] = 1.0f;
		}
		csm.bias = 0.0f;
		csm_clear(&csm);
		csm.depth[0][4 * 8 + 4] = 0.0f;	// 级联 0：阴影
		csm.depth[1][4 * 8 + 4] = 1.0f;	// 级联 1：受光
		csm.depth[2][4 * 8 + 4] = 1.0f;
		// 段前 90%：纯级联 0
		c.require(std::fabs(csm_visibility(&csm, &p, 9.0f) - 0.0f) < 1e-4f);
		// 最后 10% 中点：t=0.5，线性混合
		c.require(std::fabs(csm_visibility(&csm, &p, 9.55f) - 0.5f) < 1e-4f);
		// 分界处 t=1：完全取下一级
		c.require(std::fabs(csm_visibility(&csm, &p, 10.0f) - 1.0f) < 1e-4f);
		c.require(std::fabs(csm_visibility(&csm, &p, 10.4f) - 1.0f) < 1e-4f);
		// 第二段 -> 第三段同样混合
		csm.depth[1][4 * 8 + 4] = 0.0f;
		c.require(std::fabs(csm_visibility(&csm, &p, 19.5f) - 0.5f) < 1e-4f);
		// 最后一级不混合；最后一级以外保留无遮挡
		csm.depth[2][4 * 8 + 4] = 0.0f;
		c.require(std::fabs(csm_visibility(&csm, &p, 25.0f) - 0.0f) < 1e-4f);
		c.require(std::fabs(csm_visibility(&csm, &p, 31.0f) - 1.0f) < 1e-4f);
		// 深度图外取样视为无遮挡
		c.require(std::fabs(csm_visibility(&csm, &far_away, 5.0f) - 1.0f) < 1e-4f);
		csm_destroy(&csm);
	}

	// 亚纹素相机平移：先把相机对齐到纹素中心，再沿光空间切向做
	// < 1 纹素的微移，吸附中心、光矩阵与深度图必须保持不动
	{
		Scene sc;
		device_t dev;
		matrix_t view0;
		int ci, j;
		build_scene(sc);
		device_init(&dev, SCR_W, SCR_H, NULL);
		setup_camera(&dev, CAM_EYE, CAM_AT);
		view0 = dev.transform.view;

		for (ci = 0; ci < CSM_CASCADES; ci++) {
			csm_t cb, cc;
			point_t eyeC, atC;
			float dx, dy, wx, wy, wz, texel;
			vector_t jx, jy;
			csm_init(&cb, MAP_SIZE);
			setup_csm(&cb, sc, &view0);
			// 把未吸附中心平移到吸附网格中心（光空间 delta 转世界）
			dx = cb.ortho_center[ci][0] - cb.ortho_center_raw[ci][0];
			dy = cb.ortho_center[ci][1] - cb.ortho_center_raw[ci][1];
			wx = dx * cb.light_view.m[0][0] + dy * cb.light_view.m[0][1];
			wy = dx * cb.light_view.m[1][0] + dy * cb.light_view.m[1][1];
			wz = dx * cb.light_view.m[2][0] + dy * cb.light_view.m[2][1];
			eyeC = CAM_EYE; atC = CAM_AT;
			eyeC.x += wx; eyeC.y += wy; eyeC.z += wz;
			atC.x += wx; atC.y += wy; atC.z += wz;
			setup_camera(&dev, eyeC, atC);
			csm_init(&cc, MAP_SIZE);
			setup_csm(&cc, sc, &dev.transform.view);
			texel = cc.ortho_extent[ci] / MAP_SIZE;
			// 确认已对齐纹素中心
			c.require(std::fabs(cc.ortho_center_raw[ci][0] -
				cc.ortho_center[ci][0]) < 0.05f * texel);
			c.require(std::fabs(cc.ortho_center_raw[ci][1] -
				cc.ortho_center[ci][1]) < 0.05f * texel);
			jx.x = cc.light_view.m[0][0]; jx.y = cc.light_view.m[1][0];
			jx.z = cc.light_view.m[2][0]; jx.w = 0;
			jy.x = cc.light_view.m[0][1]; jy.y = cc.light_view.m[1][1];
			jy.z = cc.light_view.m[2][1]; jy.w = 0;
			for (j = 0; j < 4; j++) {
				float sx = (j == 0) ? 0.3f : ((j == 1) ? -0.3f : 0.0f);
				float sy = (j == 2) ? 0.25f : ((j == 3) ? -0.25f : 0.0f);
				point_t eyeJ = eyeC, atJ = atC;
				csm_t cj;
				int k;
				float maxd = 0.0f;
				eyeJ.x += (sx * jx.x + sy * jy.x) * texel;
				eyeJ.y += (sx * jx.y + sy * jy.y) * texel;
				eyeJ.z += (sx * jx.z + sy * jy.z) * texel;
				atJ.x += (sx * jx.x + sy * jy.x) * texel;
				atJ.y += (sx * jx.y + sy * jy.y) * texel;
				atJ.z += (sx * jx.z + sy * jy.z) * texel;
				setup_camera(&dev, eyeJ, atJ);
				csm_init(&cj, MAP_SIZE);
				setup_csm(&cj, sc, &dev.transform.view);
				// 吸附中心与方形边长不动（允许浮点舍入）
				c.require(std::fabs(cj.ortho_center[ci][0] -
					cc.ortho_center[ci][0]) < 0.001f * texel);
				c.require(std::fabs(cj.ortho_center[ci][1] -
					cc.ortho_center[ci][1]) < 0.001f * texel);
				c.require(std::fabs(cj.ortho_extent[ci] -
					cc.ortho_extent[ci]) < 0.001f * texel);
				// 深度图不随亚纹素平移滑动
				for (k = 0; k < MAP_SIZE * MAP_SIZE; k++) {
					float d = std::fabs(cj.depth[ci][k] - cc.depth[ci][k]);
					if (d > maxd) maxd = d;
				}
				c.require(maxd < 1e-3f);
				csm_destroy(&cj);
			}
			csm_destroy(&cb);
			csm_destroy(&cc);
		}
		device_destroy(&dev);
	}
}

//---------------------------------------------------------------------
// camera_shadow_integration：完整场景两台设备交错绘制，解析点核对
//---------------------------------------------------------------------
void camera_shadow_integration(Checks &c) {
	Scene sc;
	device_t A, B;		// A：顶点色模式；B：纹理模式，交错绘制
	csm_t csm;
	static IUINT32 tex[64][64];
	int i, j;
	build_scene(sc);

	// 代码生成棋盘纹理（8 像素格子）
	for (j = 0; j < 64; j++)
		for (i = 0; i < 64; i++)
			tex[j][i] = (((i / 8) + (j / 8)) & 1) ? 0xffffff : 0x336699;

	device_init(&A, SCR_W, SCR_H, NULL);
	device_init(&B, SCR_W, SCR_H, NULL);
	setup_camera(&A, CAM_EYE, CAM_AT);
	setup_camera(&B, CAM_EYE, CAM_AT);
	A.render_state = RENDER_STATE_COLOR;
	B.render_state = RENDER_STATE_TEXTURE;
	device_set_texture(&B, tex, 64 * 4, 64, 64);

	csm_init(&csm, MAP_SIZE);
	setup_csm(&csm, sc, &A.transform.view);
	csm_bind(&csm, &A, AMBIENT, BIAS);
	csm_bind(&csm, &B, AMBIENT, BIAS);

	device_clear(&A, 0);
	device_clear(&B, 0);
	// 两台设备交错绘制同一场景
	for (i = 0; i + 2 < (int)sc.tris.size(); i += 3) {
		device_draw_primitive(&A, &sc.tris[i], &sc.tris[i + 1], &sc.tris[i + 2]);
		device_draw_primitive(&B, &sc.tris[i], &sc.tris[i + 1], &sc.tris[i + 2]);
	}

	// 解析阴影判定与 csm_visibility 直接核对
	{
		point_t ps1 = { 5.5f, 0.0f, 8.0f, 1.0f };	// 影内
		point_t ps2 = { 5.0f, 0.0f, 7.5f, 1.0f };	// 影内
		point_t pl1 = { 0.0f, 0.0f, 8.0f, 1.0f };	// 受光
		point_t pl2 = { 5.5f, 0.0f, 11.0f, 1.0f };	// 受光（影区后方）
		point_t pout = { 200.0f, 0.0f, 200.0f, 1.0f };// 深度图外
		c.require(analytic_shadowed(5.5f, 0.0f, 8.0f));
		c.require(analytic_shadowed(5.0f, 0.0f, 7.5f));
		c.require(!analytic_shadowed(0.0f, 0.0f, 8.0f));
		c.require(!analytic_shadowed(5.5f, 0.0f, 11.0f));
		c.require(csm_visibility(&csm, &ps1, view_depth(&A, ps1)) < 0.01f);
		c.require(csm_visibility(&csm, &ps2, view_depth(&A, ps2)) < 0.01f);
		c.require(csm_visibility(&csm, &pl1, view_depth(&A, pl1)) > 0.99f);
		c.require(csm_visibility(&csm, &pl2, view_depth(&A, pl2)) > 0.99f);
		c.require(csm_visibility(&csm, &pout, view_depth(&A, pout)) > 0.99f);
	}

	// 顶点色模式：地面受光 153，影内 = 153*0.3 ≈ 45
	check_pixel(c, &A, 0.0f, 0.0f, 8.0f, 153, 153, 153, 4);
	check_pixel(c, &A, 2.0f, 0.0f, 3.0f, 153, 153, 153, 4);
	check_pixel(c, &A, 5.5f, 0.0f, 8.0f, 45, 45, 45, 4);
	check_pixel(c, &A, 5.0f, 0.0f, 7.5f, 45, 45, 45, 4);
	check_pixel(c, &A, 5.5f, 0.0f, 11.0f, 153, 153, 153, 4);
	// 接收面：近（级联 0）与远（级联 2）均受光；主相机深度遮挡保持
	check_pixel(c, &A, -2.0f, 1.5f, 3.0f, 204, 76, 76, 4);
	check_pixel(c, &A, -2.5f, 2.0f, 30.0f, 76, 204, 76, 4);

	// 纹理模式：同一批解析点按棋盘格期望纹素 * 可见度调制
	{
		// (0,0,8)：u=0.25,v=0.14 -> 格(2,1) -> 0xffffff，受光
		check_pixel(c, &B, 0.0f, 0.0f, 8.0f, 255, 255, 255, 4);
		// (5.5,0,8)：格(2,1)，影内 -> 0xffffff * 0.3
		check_pixel(c, &B, 5.5f, 0.0f, 8.0f, 76, 76, 76, 4);
		// (2,0,3)：u=0.2625,v=0.115 -> 格(2,0) -> 0x336699，受光
		check_pixel(c, &B, 2.0f, 0.0f, 3.0f, 0x33, 0x66, 0x99, 4);
		// 接收面 UV 中心 -> 格(4,4) -> 0x336699，受光
		check_pixel(c, &B, -2.0f, 1.5f, 3.0f, 0x33, 0x66, 0x99, 4);
		check_pixel(c, &B, -2.5f, 2.0f, 30.0f, 0x33, 0x66, 0x99, 4);
	}

	// 保存深度图与最终 PPM
	write_depth_ppm("csm_depth0.ppm", csm.depth[0], MAP_SIZE);
	write_depth_ppm("csm_depth1.ppm", csm.depth[1], MAP_SIZE);
	write_depth_ppm("csm_depth2.ppm", csm.depth[2], MAP_SIZE);
	write_device_ppm("csm_final_color.ppm", &A);
	write_device_ppm("csm_final_texture.ppm", &B);

	// 不启用阴影时输出保持：解绑后与从未绑定的设备逐像素一致
	{
		device_t C, D;
		device_init(&C, SCR_W, SCR_H, NULL);
		device_init(&D, SCR_W, SCR_H, NULL);
		setup_camera(&C, CAM_EYE, CAM_AT);
		setup_camera(&D, CAM_EYE, CAM_AT);
		C.render_state = RENDER_STATE_COLOR;
		D.render_state = RENDER_STATE_COLOR;
		csm_bind(&csm, &D, AMBIENT, BIAS);
		csm_unbind(&D);	// 绑定后解绑，回到无阴影状态
		device_clear(&C, 0);
		device_clear(&D, 0);
		draw_scene(&C, sc);
		draw_scene(&D, sc);
		for (j = 0; j < SCR_H; j++)
			for (i = 0; i < SCR_W; i++)
				c.require(C.framebuffer[j][i] == D.framebuffer[j][i]);
		// 无阴影时影内解析点恢复全亮
		check_pixel(c, &C, 5.5f, 0.0f, 8.0f, 153, 153, 153, 4);
		device_destroy(&C);
		device_destroy(&D);
	}

	csm_unbind(&A);
	csm_unbind(&B);
	csm_destroy(&csm);
	device_destroy(&A);
	device_destroy(&B);
}
