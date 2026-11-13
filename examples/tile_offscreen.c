// Offscreen tile-worker rendering example.
//
// Renders a 64x64 image entirely with real glDraw* calls (no windowing
// library) and writes a binary P6 Netpbm file:
//
//   P6
//   64 64
//   255
//   <64*64 RGB bytes>
//
// Build (from the repository root), with the threaded backend enabled:
//   cc -std=c11 -O2 -DPGL_TILE_THREADS -I . examples/tile_offscreen.c \
//      -pthread -lm -o tile_offscreen
//
// Run:
//   ./tile_offscreen out.ppm
//
// Without PGL_TILE_THREADS the same program builds and runs on the serial
// pipeline; pglSetTileWorkers() then reports GL_INVALID_OPERATION and the
// draw simply stays single-threaded.

#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_W 64
#define IMG_H 64

static GLuint checker_tex;

static void offscreen_vs(float* out, vec4* in, Shader_Builtins* b, void* u)
{
	(void)u;
	// Pass a repeating UV through the first two varying components.
	out[0] = in[0].x * 2.0f;
	out[1] = in[0].y * 2.0f;
	b->gl_Position = in[0];
}

static void offscreen_fs(float* in, Shader_Builtins* b, void* u)
{
	(void)u;
	// Read-only texture sampling is allowed from worker fragment shaders.
	b->gl_FragColor = texture2D(checker_tex, in[0], in[1]);
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
		return 2;
	}

	glContext ctx;
	pix_t* pixels = NULL;
	if (!init_glContext(&ctx, &pixels, IMG_W, IMG_H)) {
		fprintf(stderr, "init_glContext failed\n");
		return 1;
	}
	set_glContext(&ctx);

	// Two persistent worker threads. On a serial-only build this is rejected
	// and rendering proceeds single-threaded.
	if (!pglSetTileWorkers(2))
		glGetError();

	// A small RGBA8 checker texture on the default texture unit.
	Color image[16];
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			int on = (x + y) & 1;
			image[y * 4 + x] = on ? make_Color(228, 96, 40, 255)
			                      : make_Color(40, 96, 228, 255);
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	checker_tex = tex;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	GLenum interp[] = { PGL_SMOOTH, PGL_SMOOTH };
	GLuint program = pglCreateProgram(offscreen_vs, offscreen_fs, 2, interp, GL_FALSE);
	glUseProgram(program);

	// Two overlapping translucent triangle fans plus a triangle strip, all
	// produced by real draw calls and spread across the 16px tile grid.
	static const float fan_a[] = {
		-0.2f, -0.2f, 0.3f, 1.f,
		-0.95f, -0.95f, 0.3f, 1.f,
		 0.55f, -0.95f, 0.3f, 1.f,
		 0.55f,  0.55f, 0.3f, 1.f,
		-0.95f,  0.55f, 0.3f, 1.f,
		-0.95f, -0.95f, 0.3f, 1.f,
	};
	static const float fan_b[] = {
		 0.25f,  0.25f, 0.1f, 1.f,
		 0.95f,  0.95f, 0.1f, 1.f,
		-0.55f,  0.95f, 0.1f, 1.f,
		-0.55f, -0.55f, 0.1f, 1.f,
		 0.95f, -0.55f, 0.1f, 1.f,
		 0.95f,  0.95f, 0.1f, 1.f,
	};
	static const float strip[] = {
		-1.0f, 0.85f, 0.2f, 1.f, -1.0f, 1.0f, 0.2f, 1.f,
		-0.5f, 0.85f, 0.2f, 1.f, -0.5f, 1.0f, 0.2f, 1.f,
		 0.0f, 0.85f, 0.2f, 1.f,  0.0f, 1.0f, 0.2f, 1.f,
		 0.5f, 0.85f, 0.2f, 1.f,  0.5f, 1.0f, 0.2f, 1.f,
		 1.0f, 0.85f, 0.2f, 1.f,  1.0f, 1.0f, 0.2f, 1.f,
	};

	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(fan_a) + sizeof(fan_b) + sizeof(strip),
	             NULL, GL_STATIC_DRAW);
	GLsizeiptr off = 0;
	glBufferSubData(GL_ARRAY_BUFFER, off, sizeof(fan_a), fan_a); off += sizeof(fan_a);
	glBufferSubData(GL_ARRAY_BUFFER, off, sizeof(fan_b), fan_b); off += sizeof(fan_b);
	glBufferSubData(GL_ARRAY_BUFFER, off, sizeof(strip), strip);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)(sizeof(fan_a) / (4 * sizeof(float))));
	glDrawArrays(GL_TRIANGLE_FAN, (GLint)(sizeof(fan_a) / (4 * sizeof(float))),
	             (GLsizei)(sizeof(fan_b) / (4 * sizeof(float))));
	glDrawArrays(GL_TRIANGLE_STRIP,
	             (GLint)((sizeof(fan_a) + sizeof(fan_b)) / (4 * sizeof(float))),
	             (GLsizei)(sizeof(strip) / (4 * sizeof(float))));

	if (glGetError() != GL_NO_ERROR) {
		fprintf(stderr, "GL error during rendering\n");
		free_glContext(&ctx);
		return 1;
	}

	FILE* f = fopen(argv[1], "wb");
	if (!f) {
		perror("fopen");
		free_glContext(&ctx);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", IMG_W, IMG_H);
	// Memory row 0 holds the top screen row (writes index from lastrow), so a
	// flat walk already produces PPM's top-to-bottom row order.
	for (int i = 0; i < IMG_W * IMG_H; ++i) {
		Color col = PIXEL_TO_COLOR(pixels[i]);
		unsigned char rgb[3] = { col.r, col.g, col.b };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);

	free_glContext(&ctx);
	return 0;
}
