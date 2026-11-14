//=====================================================================
//
// mini3d.h - Mini Software Render core interface (C99, headless safe)
//
// The window layer (screen_*) stays in mini3d.c and is disabled by
// defining MINI3D_HEADLESS. Everything below is the reusable math,
// transform, trapezoid/scanline rasterization and device interface.
//
//=====================================================================
#ifndef MINI3D_H
#define MINI3D_H

#include <stddef.h>

typedef unsigned int IUINT32;

//=====================================================================
// 数学库：矩阵、矢量
//=====================================================================
typedef struct { float m[4][4]; } matrix_t;
typedef struct { float x, y, z, w; } vector_t;
typedef vector_t point_t;

int CMID(int x, int min, int max);
float interp(float x1, float x2, float t);

float vector_length(const vector_t *v);
void vector_add(vector_t *z, const vector_t *x, const vector_t *y);
void vector_sub(vector_t *z, const vector_t *x, const vector_t *y);
float vector_dotproduct(const vector_t *x, const vector_t *y);
void vector_crossproduct(vector_t *z, const vector_t *x, const vector_t *y);
void vector_interp(vector_t *z, const vector_t *x1, const vector_t *x2, float t);
void vector_normalize(vector_t *v);

void matrix_add(matrix_t *c, const matrix_t *a, const matrix_t *b);
void matrix_sub(matrix_t *c, const matrix_t *a, const matrix_t *b);
void matrix_mul(matrix_t *c, const matrix_t *a, const matrix_t *b);
void matrix_scale(matrix_t *c, const matrix_t *a, float f);
void matrix_apply(vector_t *y, const vector_t *x, const matrix_t *m);
void matrix_set_identity(matrix_t *m);
void matrix_set_zero(matrix_t *m);
void matrix_set_translate(matrix_t *m, float x, float y, float z);
void matrix_set_scale(matrix_t *m, float x, float y, float z);
void matrix_set_rotate(matrix_t *m, float x, float y, float z, float theta);
void matrix_set_lookat(matrix_t *m, const vector_t *eye,
	const vector_t *at, const vector_t *up);
void matrix_set_perspective(matrix_t *m, float fovy, float aspect,
	float zn, float zf);

//=====================================================================
// 坐标变换
//=====================================================================
typedef struct {
	matrix_t world;         // 世界坐标变换
	matrix_t view;          // 摄影机坐标变换
	matrix_t projection;    // 投影变换
	matrix_t transform;     // transform = world * view * projection
	float w, h;             // 屏幕大小
}	transform_t;

void transform_update(transform_t *ts);
void transform_init(transform_t *ts, int width, int height);
void transform_apply(const transform_t *ts, vector_t *y, const vector_t *x);
int transform_check_cvv(const vector_t *v);
void transform_homogenize(const transform_t *ts, vector_t *y, const vector_t *x);

//=====================================================================
// 几何计算：顶点、扫描线、边缘、矩形、步长计算
//=====================================================================
typedef struct { float r, g, b; } color_t;
typedef struct { float u, v; } texcoord_t;

// lc 为物体坐标经 object-to-light 变换后的光空间齐次坐标，
// vertex_rhw_init 后按主相机 w 透视除法存储，随梯形/扫描线插值
typedef struct vertex_t {
	point_t pos;
	texcoord_t tc;
	color_t color;
	float rhw;
	point_t lc;
}	vertex_t;

typedef struct { vertex_t v, v1, v2; } edge_t;
typedef struct { float top, bottom; edge_t left, right; } trapezoid_t;
typedef struct { vertex_t v, step; int x, y, w; } scanline_t;

void vertex_rhw_init(vertex_t *v);
void vertex_interp(vertex_t *y, const vertex_t *x1, const vertex_t *x2, float t);
void vertex_division(vertex_t *y, const vertex_t *x1, const vertex_t *x2, float w);
void vertex_add(vertex_t *y, const vertex_t *x);

int trapezoid_init_triangle(trapezoid_t *trap, const vertex_t *p1,
	const vertex_t *p2, const vertex_t *p3);
void trapezoid_edge_interp(trapezoid_t *trap, float y);
void trapezoid_init_scan_line(const trapezoid_t *trap, scanline_t *scanline, int y);

//=====================================================================
// 渲染设备
//=====================================================================
typedef struct {
	transform_t transform;      // 坐标变换器
	int width;                  // 窗口宽度
	int height;                 // 窗口高度
	IUINT32 **framebuffer;      // 像素缓存：framebuffer[y] 代表第 y行
	float **zbuffer;            // 深度缓存：zbuffer[y] 为第y行指针
	IUINT32 **texture;          // 纹理：同样是每行索引
	int tex_width;              // 纹理宽度
	int tex_height;             // 纹理高度
	float max_u;                // 纹理最大宽度：tex_width - 1
	float max_v;                // 纹理最大高度：tex_height - 1
	int render_state;           // 渲染状态
	IUINT32 background;         // 背景颜色
	IUINT32 foreground;         // 线框颜色

	// VSM 阴影绑定：vsm_user 非 NULL 时扫描线走阴影调制路径
	const void *vsm_user;       // vsm_map_t 指针（vsm.c 内部解释）
	float (*vsm_sample)(const void *map, float u, float v, float t,
		float bias, float min_variance, float bleed);
	matrix_t vsm_light;         // 当前物体到光源矩阵
	float vsm_bias;             // 接收深度偏移，>= 0
	float vsm_min_variance;     // 最小方差，> 0
	float vsm_bleed;            // 漏光抑制，[0, 1)
	float vsm_ambient;          // 环境光，[0, 1]

	// 阴影图捕获复用生产光栅路径时的逐像素回调；为 NULL 时正常着色
	void (*shadow_pixel_cb)(void *user, int x, int y, const vertex_t *v);
	void *shadow_user;
}	device_t;

#define RENDER_STATE_WIREFRAME      1		// 渲染线框
#define RENDER_STATE_TEXTURE        2		// 渲染纹理
#define RENDER_STATE_COLOR          4		// 渲染颜色

void device_init(device_t *device, int width, int height, void *fb);
void device_destroy(device_t *device);
void device_set_texture(device_t *device, void *bits, long pitch, int w, int h);
void device_clear(device_t *device, int mode);
void device_pixel(device_t *device, int x, int y, IUINT32 color);
void device_draw_line(device_t *device, int x1, int y1, int x2, int y2, IUINT32 c);
IUINT32 device_texture_read(const device_t *device, float u, float v);

void device_draw_scanline(device_t *device, scanline_t *scanline);
void device_render_trap(device_t *device, trapezoid_t *trap);
void device_draw_primitive(device_t *device, const vertex_t *v1,
	const vertex_t *v2, const vertex_t *v3);

#endif /* MINI3D_H */
