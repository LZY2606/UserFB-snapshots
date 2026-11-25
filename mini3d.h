//=====================================================================
//
// mini3d.h - public interface of the Mini3D software renderer core
//
// This header exposes the types and functions implemented in mini3d.c
// so that additional modules (for example radiosity.c) can reuse the
// original matrix, trapezoid and scanline pipeline.
//
// mini3d.c includes this header itself, so every declaration below is
// checked against the real definition at compile time.  The header is
// plain C99 and is guarded for use from C++.
//
//=====================================================================
#ifndef MINI3D_H
#define MINI3D_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int IUINT32;

//=====================================================================
// math
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
void matrix_set_lookat(matrix_t *m, const vector_t *eye, const vector_t *at,
	const vector_t *up);
void matrix_set_perspective(matrix_t *m, float fovy, float aspect,
	float zn, float zf);

//=====================================================================
// transforms
//=====================================================================
typedef struct {
	matrix_t world;         // world transform
	matrix_t view;          // camera transform
	matrix_t projection;    // projection transform
	matrix_t transform;     // transform = world * view * projection
	float w, h;             // screen size
} transform_t;

void transform_update(transform_t *ts);
void transform_init(transform_t *ts, int width, int height);
void transform_apply(const transform_t *ts, vector_t *y, const vector_t *x);
int transform_check_cvv(const vector_t *v);
void transform_homogenize(const transform_t *ts, vector_t *y, const vector_t *x);

//=====================================================================
// geometry: vertices, edges, trapezoids, scanlines
//=====================================================================
typedef struct { float r, g, b; } color_t;
typedef struct { float u, v; } texcoord_t;
typedef struct { point_t pos; texcoord_t tc; color_t color; float rhw; } vertex_t;

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
// render device
//=====================================================================
typedef struct {
	transform_t transform;      // transform state
	int width;                  // window width
	int height;                 // window height
	IUINT32 **framebuffer;      // framebuffer: framebuffer[y] is row y
	float **zbuffer;            // depth buffer: zbuffer[y] is row y
	IUINT32 **texture;          // texture rows
	int tex_width;              // texture width
	int tex_height;             // texture height
	float max_u;                // texture max width: tex_width - 1
	float max_v;                // texture max height: tex_height - 1
	int render_state;           // render state
	IUINT32 background;         // background color
	IUINT32 foreground;         // wireframe color
} device_t;

#define RENDER_STATE_WIREFRAME      1		// wireframe
#define RENDER_STATE_TEXTURE        2		// textured
#define RENDER_STATE_COLOR          4		// vertex color

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
