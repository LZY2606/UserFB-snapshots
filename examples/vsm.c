//=====================================================================
//
// examples/vsm.c - 离线 VSM 预览示例（MINI3D_HEADLESS）
//
// 场景：白色接收地面 + 悬浮遮挡方块。先把遮挡者捕获进阴影图并
// 过滤，再用同一台透视相机分别在“未绑定/绑定 VSM”下渲染，
// 输出一张左右对照的二进制 PPM（P6）。
//
// build:
//   cc -std=c99 -O2 -DMINI3D_HEADLESS -I. examples/vsm.c mini3d.c \
//      vsm.c -lm -o vsm_example
//
//=====================================================================
#include "../vsm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_W 128
#define VIEW_H 96

static void make_quad(vertex_t *tris, float x0, float y0, float x1,
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

static void draw_all(device_t *d, const vertex_t *tris, size_t count) {
	size_t i;
	for (i = 0; i < count; i++)
		device_draw_primitive(d, &tris[i * 3], &tris[i * 3 + 1],
			&tris[i * 3 + 2]);
}

static void write_ppm(const char *path, const IUINT32 *left,
	const IUINT32 *right) {
	FILE *fp = fopen(path, "wb");
	int y, x;
	if (fp == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return;
	}
	fprintf(fp, "P6\n%d %d\n255\n", VIEW_W * 2, VIEW_H);
	for (y = 0; y < VIEW_H; y++) {
		for (x = 0; x < VIEW_W; x++) {
			IUINT32 c = left[y * VIEW_W + x];
			fputc((c >> 16) & 255, fp);
			fputc((c >> 8) & 255, fp);
			fputc(c & 255, fp);
		}
		for (x = 0; x < VIEW_W; x++) {
			IUINT32 c = right[y * VIEW_W + x];
			fputc((c >> 16) & 255, fp);
			fputc((c >> 8) & 255, fp);
			fputc(c & 255, fp);
		}
	}
	fclose(fp);
	printf("wrote %s (left: unbound, right: VSM)\n", path);
}

int main(int argc, char **argv) {
	const char *out = (argc > 1) ? argv[1] : "vsm_compare.ppm";
	device_t device;
	vsm_map_t *map;
	matrix_t light;
	vertex_t floor_tris[6], occluder_tris[6];
	IUINT32 *plain = (IUINT32*)malloc((size_t)VIEW_W * VIEW_H * sizeof(IUINT32));
	int y, x;

	// 正交光源齐次矩阵：lx=.5x、ly=.5y、lz=.15z，地面与遮挡者都落在 [0,1]
	matrix_set_scale(&light, 0.5f, 0.5f, 0.15f);

	make_quad(floor_tris, -3.0f, -2.2f, 3.0f, 2.2f, 5.0f,
		(color_t){ 0.95f, 0.95f, 0.9f });
	make_quad(occluder_tris, -1.0f, -1.0f, 1.0f, 1.0f, 2.5f,
		(color_t){ 0.8f, 0.8f, 0.8f });

	map = vsm_create(32, 32);
	vsm_capture(map, &light, occluder_tris, 2);
	vsm_filter(map, 3);

	device_init(&device, VIEW_W, VIEW_H, NULL);
	transform_init(&device.transform, VIEW_W, VIEW_H);
	device.background = 0x101820;
	device.render_state = RENDER_STATE_COLOR;

	// 左：未绑定阴影，地面全亮
	device_clear(&device, 0);
	draw_all(&device, floor_tris, 2);
	for (y = 0; y < VIEW_H; y++)
		for (x = 0; x < VIEW_W; x++)
			plain[y * VIEW_W + x] = device.framebuffer[y][x];

	// 右：绑定 VSM，ambient 保留环境光，bleed 抑制漏光
	device_bind_vsm(&device, map, &light, 0.0f, 1e-4f, 0.2f, 0.15f);
	device_clear(&device, 0);
	draw_all(&device, floor_tris, 2);

	write_ppm(out, plain, device.framebuffer[0]);

	free(plain);
	vsm_destroy(map);
	device_destroy(&device);
	return 0;
}
