//=====================================================================
//
// mini3d.h - Mini Software Render Core Interface
//
// 可复用的核心接口：数学库、坐标变换、梯形扫描线、渲染设备。
// mini3d.c 实现本头文件声明的全部函数；csm.c 与测试程序均可只
// 依赖本头文件使用渲染核心。
//
//=====================================================================
#ifndef MINI3D_H
#define MINI3D_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int IUINT32;

//=====================================================================
// 数学库
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
void matrix_set_lookat(matrix_t *m, const vector_t *eye, const vector_t *at, const vector_t *up);
void matrix_set_perspective(matrix_t *m, float fovy, float aspect, float zn, float zf);

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
// world 为插值用的世界坐标（随 rhw 一起做透视校正），供阴影等按
// 世界位置取样的效果使用；不使用阴影时无需填写。
typedef struct { point_t pos; texcoord_t tc; color_t color; float rhw; point_t world; } vertex_t;

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

// 阴影取样回调：给定透视校正后的世界坐标与正视深度（view space z），
// 返回可见度 [0,1]，1 为完全受光。由 csm 模块提供实现。
typedef float (*shadow_sample_fn)(void *ctx, const point_t *world, float view_z);

typedef struct {
	transform_t transform;      // 坐标变换器
	int width;                  // 窗口宽度
	int height;                 // 窗口高度
	IUINT32 **framebuffer;      // 像素缓存：framebuffer[y] 代表第 y行
	float **zbuffer;            // 深度缓存：zbuffer[y] 为第 y行指针
	IUINT32 **texture;          // 纹理：同样是每行索引
	int tex_width;              // 纹理宽度
	int tex_height;             // 纹理高度
	float max_u;                // 纹理最大宽度：tex_width - 1
	float max_v;                // 纹理最大高度：tex_height - 1
	int render_state;           // 渲染状态
	IUINT32 background;         // 背景颜色
	IUINT32 foreground;         // 线框颜色
	int shadow_state;           // 阴影开关：0 关闭（输出与原有一致）
	shadow_sample_fn shadow_fn; // 阴影取样回调
	void *shadow_ctx;           // 回调上下文（例如 csm_t）
	float shadow_ambient;       // 环境光分量 [0,1]
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

#ifdef __cplusplus
}
#endif

#endif /* MINI3D_H */
