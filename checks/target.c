// Tile worker backend test entry point.
//
// Three groups, selected by unit_run_group():
//   geometry - serial vs bucketed coverage, varyings and vertex shader counts
//   ordering - full color/depth/stencil buffers, blending, discard, FBO
//   workers  - real overlap, batch sequencing, context isolation and reconfig
//
// Every positive assertion goes through UNIT_CHECK in suite.h. Fragment
// callbacks run on worker threads, so every record they touch is guarded by
// the same mutex the assertions use on the calling thread after draw returns.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

// Batch event hooks: record only, never call back into the GL API.
#define HOOK_CAP 1024
static pthread_mutex_t hook_lock = PTHREAD_MUTEX_INITIALIZER;
static int hook_ready[HOOK_CAP];
static int hook_retired[HOOK_CAP];
static int hook_nready;
static int hook_nretired;
static int hook_max_inflight;

static void hook_batch_ready(int seq)
{
	pthread_mutex_lock(&hook_lock);
	if (hook_nready < HOOK_CAP)
		hook_ready[hook_nready++] = seq;
	int inflight = hook_nready - hook_nretired;
	if (inflight > hook_max_inflight)
		hook_max_inflight = inflight;
	pthread_mutex_unlock(&hook_lock);
}

static void hook_batch_retired(int seq)
{
	pthread_mutex_lock(&hook_lock);
	if (hook_nretired < HOOK_CAP)
		hook_retired[hook_nretired++] = seq;
	pthread_mutex_unlock(&hook_lock);
}

#define PGL_TILE_BATCH_READY(sequence) hook_batch_ready(sequence)
#define PGL_TILE_BATCH_RETIRED(sequence) hook_batch_retired(sequence)

#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include "suite.h"

#define TEST_W 48
#define TEST_H 48
#define OVERLAP_TIMEOUT_SEC 5

// Shared fragment-callback records, all under rec_lock.
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rec_cond = PTHREAD_COND_INITIALIZER;
static int vs_calls;
static int fs_calls;
static int fs_foreign;
static pthread_t caller_thread;

// Overlap handshake state.
static pthread_t overlap_tid[4];
static int overlap_count;
static int overlap_released;
static int overlap_distinct;

// Pause gate for the batch sequencing test.
static int fs_paused;
static int fs_active;

static GLuint g_tex;

// Timed wait helper: every blocked wait has a timeout so a deadlock fails the
// test instead of hanging it. Returns 0 on timeout.
static int wait_while_locked(int (*pred)(void*), void* arg, int seconds)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += seconds;
	while (!pred(arg)) {
		int rc = pthread_cond_timedwait(&rec_cond, &rec_lock, &ts);
		if (rc == ETIMEDOUT)
			return pred(arg) ? 1 : 0;
	}
	return 1;
}

static void test_vs(float* out, vec4* in, Shader_Builtins* b, void* u)
{
	(void)u;
	++vs_calls;
	// Three varyings: smooth UV pair plus a perspective-independent value.
	out[0] = in[0].x * 0.5f + 0.5f;
	out[1] = in[0].y * 0.5f + 0.5f;
	out[2] = in[0].z;
	b->gl_Position = in[0];
}

static void test_fs(float* in, Shader_Builtins* b, void* u)
{
	(void)u;
	pthread_mutex_lock(&rec_lock);
	++fs_calls;
	if (!pthread_equal(pthread_self(), caller_thread))
		++fs_foreign;
	pthread_mutex_unlock(&rec_lock);

	vec4 t = texture2D(g_tex, in[0], in[1]);
	b->gl_FragColor.x = t.x * (0.5f + 0.5f * in[2]);
	b->gl_FragColor.y = t.y;
	b->gl_FragColor.z = t.z;
	b->gl_FragColor.w = t.w;
}

// Discard a repeating subset and rewrite depth on the surviving fragments.
static void discard_fs(float* in, Shader_Builtins* b, void* u)
{
	(void)u;
	pthread_mutex_lock(&rec_lock);
	++fs_calls;
	if (!pthread_equal(pthread_self(), caller_thread))
		++fs_foreign;
	pthread_mutex_unlock(&rec_lock);

	int cell = ((int)(in[0] * 13.0f) + (int)(in[1] * 7.0f)) % 3;
	if (cell == 0) {
		b->discard = GL_TRUE;
		return;
	}
	b->gl_FragDepth = b->gl_FragCoord.z * 0.5f;
	b->gl_FragColor = make_v4(in[0], in[1], 0.3f, 0.75f);
}

// First two distinct worker threads to enter the callback register here; the
// first one then releases the lock and waits so the second can actually enter
// (holding the lock across the wait would self-deadlock).
static void handshake_fs(float* in, Shader_Builtins* b, void* u)
{
	(void)in; (void)u;
	pthread_mutex_lock(&rec_lock);
	++fs_calls;
	if (!pthread_equal(pthread_self(), caller_thread))
		++fs_foreign;
	pthread_t self = pthread_self();
	int known = 0;
	for (int i = 0; i < overlap_count; ++i) {
		if (pthread_equal(overlap_tid[i], self))
			known = 1;
	}
	if (!known && overlap_count < 4) {
		overlap_tid[overlap_count] = self;
		int mine = ++overlap_count;
		pthread_cond_broadcast(&rec_cond);
		pthread_mutex_unlock(&rec_lock);

		if (mine == 1) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += OVERLAP_TIMEOUT_SEC;
			pthread_mutex_lock(&rec_lock);
			while (overlap_count < 2 && !overlap_released) {
				if (pthread_cond_timedwait(&rec_cond, &rec_lock, &ts) == ETIMEDOUT)
					break;
			}
			if (overlap_count >= 2 && !pthread_equal(overlap_tid[0], overlap_tid[1]))
				overlap_distinct = 1;
			pthread_mutex_unlock(&rec_lock);
		}
	} else {
		pthread_mutex_unlock(&rec_lock);
	}

	b->gl_FragColor = make_v4(0.2f, 0.4f, 0.8f, 1.0f);
}

// Parks inside the callback while fs_paused is set, letting the caller thread
// publish later batches while earlier tiles are still being shaded.
static void pause_fs(float* in, Shader_Builtins* b, void* u)
{
	(void)u;
	pthread_mutex_lock(&rec_lock);
	++fs_calls;
	if (!pthread_equal(pthread_self(), caller_thread))
		++fs_foreign;
	++fs_active;
	pthread_cond_broadcast(&rec_cond);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += 10;
	while (fs_paused) {
		if (pthread_cond_timedwait(&rec_cond, &rec_lock, &ts) == ETIMEDOUT)
			break;
	}
	--fs_active;
	pthread_mutex_unlock(&rec_lock);

	b->gl_FragColor = make_v4(in[0], in[1], 0.5f, 0.6f);
}

static void reset_records(void)
{
	pthread_mutex_lock(&rec_lock);
	vs_calls = 0;
	fs_calls = 0;
	fs_foreign = 0;
	pthread_mutex_unlock(&rec_lock);
}

static int active_ge1(void* ignored) { (void)ignored; return fs_active >= 1; }

static void reset_hooks(void)
{
	pthread_mutex_lock(&hook_lock);
	hook_nready = 0;
	hook_nretired = 0;
	hook_max_inflight = 0;
	pthread_mutex_unlock(&hook_lock);
}

static GLuint make_checker_texture(int size, int seed)
{
	Color* image = (Color*)malloc((size_t)size * size * sizeof(Color));
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			int on = (((x + seed) / 2) + ((y + seed) / 2)) & 1;
			image[y * size + x] = on ? make_Color(240, 200, 40, 200)
			                         : make_Color(30, 70, 220, 200);
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	free(image);
	return tex;
}

static GLuint make_mip_texture(int size, int seed)
{
	Color* image = (Color*)malloc((size_t)size * size * sizeof(Color));
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			image[y * size + x] = make_Color((unsigned char)((x * 17 + seed) & 255),
			                                 (unsigned char)((y * 29 + seed * 3) & 255),
			                                 (unsigned char)((x * 7 + y * 11 + seed * 5) & 255),
			                                 255);
		}
	}
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, image);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glGenerateMipmap(GL_TEXTURE_2D);
	free(image);
	return tex;
}

static GLuint make_program(void (*fs)(float*, Shader_Builtins*, void*),
                           GLboolean fragdepth_or_discard)
{
	GLenum interp[] = { PGL_SMOOTH, PGL_SMOOTH, PGL_NOPERSPECTIVE };
	return pglCreateProgram(test_vs, fs, 3, interp, fragdepth_or_discard);
}

static void upload_vertices(const float* v, int ncomp, int nverts)
{
	GLuint buf;
	glGenBuffers(1, &buf);
	glBindBuffer(GL_ARRAY_BUFFER, buf);
	glBufferData(GL_ARRAY_BUFFER, (size_t)nverts * ncomp * sizeof(float), v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, ncomp, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);
}

static void clear_surface(void)
{
	glClearColor(0.05f, 0.07f, 0.09f, 1.0f);
	glClearDepth(1.0f);
#ifndef PGL_NO_STENCIL
	glClearStencil(7);
#endif
	glClear(GL_COLOR_BUFFER_BIT
#ifndef PGL_NO_DEPTH_NO_STENCIL
	        | GL_DEPTH_BUFFER_BIT
#  ifndef PGL_NO_STENCIL
	        | GL_STENCIL_BUFFER_BIT
#  endif
#endif
	        );
}

typedef struct {
	glContext ctx;
	pix_t* pixels;
	GLuint program;
	GLuint discard_program;
	GLuint handshake_program;
	GLuint pause_program;
	GLuint tex_name;
} renderer;

static void renderer_init(renderer* r, int mip, int seed)
{
	memset(r, 0, sizeof(*r));
	if (!init_glContext(&r->ctx, &r->pixels, TEST_W, TEST_H)) {
		fprintf(stderr, "init_glContext failed\n");
		exit(1);
	}
	set_glContext(&r->ctx);
	r->program = make_program(test_fs, GL_FALSE);
	r->discard_program = make_program(discard_fs, GL_TRUE);
	GLenum interp[] = { PGL_SMOOTH, PGL_SMOOTH, PGL_NOPERSPECTIVE };
	r->handshake_program = pglCreateProgram(test_vs, handshake_fs, 3, interp, GL_FALSE);
	r->pause_program = pglCreateProgram(test_vs, pause_fs, 3, interp, GL_FALSE);
	r->tex_name = mip ? make_mip_texture(32, seed) : make_checker_texture(8, seed);
}

static void renderer_free(renderer* r)
{
	free_glContext(&r->ctx);
	memset(r, 0, sizeof(*r));
}

static void common_state(GLboolean blend, GLboolean depth, GLboolean stencil)
{
	if (blend) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}
	if (depth) {
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
		glDepthMask(GL_TRUE);
	} else {
		glDisable(GL_DEPTH_TEST);
	}
#ifndef PGL_NO_STENCIL
	if (stencil) {
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_ALWAYS, 1, 0xFF);
		glStencilOp(GL_KEEP, GL_INCR, GL_INCR_WRAP);
	} else {
		glDisable(GL_STENCIL_TEST);
	}
#endif
}

// Depth/stencil comparison that matches whichever storage format the build
// selected (packed D24S8 u32, separate D16 u16 + u8 stencil, or neither).
static int depth_stencil_equal(renderer* a, renderer* b)
{
#ifdef PGL_NO_DEPTH_NO_STENCIL
	(void)a; (void)b;
	return 1;
#else
	int n = TEST_W * TEST_H;
#  ifdef PGL_D16
	u16* za = (u16*)a->ctx.zbuf.buf;
	u16* zb = (u16*)b->ctx.zbuf.buf;
	for (int i = 0; i < n; ++i) {
		if (za[i] != zb[i])
			return 0;
	}
#  ifndef PGL_NO_STENCIL
	u8* sa = (u8*)a->ctx.stencil_buf.buf;
	u8* sb = (u8*)b->ctx.stencil_buf.buf;
	for (int i = 0; i < n; ++i) {
		if (sa[i] != sb[i])
			return 0;
	}
#  endif
#  else
	u32* za = (u32*)a->ctx.zbuf.buf;
	u32* zb = (u32*)b->ctx.zbuf.buf;
	for (int i = 0; i < n; ++i) {
		if (za[i] != zb[i])
			return 0;
	}
#  endif
	return 1;
#endif
}

// Whole-surface comparison: default color plus the configured depth/stencil
// storage.
static int surfaces_equal(renderer* a, renderer* b)
{
	pix_t* pa = (pix_t*)a->ctx.back_buffer.buf;
	pix_t* pb = (pix_t*)b->ctx.back_buffer.buf;
	for (int i = 0; i < TEST_W * TEST_H; ++i) {
		if (pa[i] != pb[i])
			return 0;
	}
	return depth_stencil_equal(a, b);
}

typedef struct {
	GLenum mode;
	const float* verts;
	int nverts;
	int ncomp;
	const GLuint* indices;
	int index_count;
	int instances;
} scene;

static void run_scene(renderer* r, const scene* s, GLsizei workers,
                      GLuint program, GLboolean blend)
{
	set_glContext(&r->ctx);
	pglSetTileWorkers(workers);
	g_tex = r->tex_name;
	glUseProgram(program);
	upload_vertices(s->verts, s->ncomp, s->nverts);
	clear_surface();
	common_state(blend, GL_TRUE, GL_TRUE);
	if (s->indices) {
		if (s->instances > 1)
			glDrawElementsInstanced(s->mode, s->index_count, GL_UNSIGNED_INT,
			                        s->indices, s->instances);
		else
			glDrawElements(s->mode, s->index_count, GL_UNSIGNED_INT, s->indices);
	} else {
		glDrawArrays(s->mode, 0, s->nverts);
	}
}

// ---------------------------------------------------------------------------
// geometry group
// ---------------------------------------------------------------------------

// Quad split on a diagonal crossing the 16px tile boundaries.
static const float shared_edge_verts[] = {
	-0.9f, -0.8f, 0.2f, 1.f,   0.9f, -0.8f, 0.1f, 1.f,
	 0.9f,  0.8f, 0.0f, 1.f,  -0.9f, -0.8f, 0.2f, 1.f,
	 0.9f,  0.8f, 0.0f, 1.f,  -0.9f,  0.8f,-0.1f, 1.f,
};

// Different w/depth across the face so perspective interpolation differs.
static const float perspective_verts[] = {
	-0.95f, -0.9f, 0.55f, 1.f,  0.95f, -0.9f, 0.45f, 1.f,
	 0.0f,   0.9f,-0.55f, 1.f,
};

// One vertex behind the eye: the near plane clipper must produce fragments.
static const float near_clip_verts[] = {
	-0.9f, -0.9f,-2.0f, 1.f,  0.9f, -0.9f, 0.25f, 1.f,
	 0.0f,  0.9f, 0.1f, 1.f,  0.2f, -0.2f, 0.6f, 1.f,
	 0.95f, 0.2f,-0.4f, 1.f, -0.85f, 0.3f, 0.3f, 1.f,
};

static const GLuint indexed_ids[] = { 0, 1, 2, 2, 1, 3 };
static const float indexed_verts[] = {
	-0.9f, -0.9f, 0.2f, 1.f,  0.9f, -0.9f, 0.1f, 1.f,
	-0.4f,  0.9f, 0.0f, 1.f,  0.7f,  0.8f,-0.2f, 1.f,
};

static const float strip_verts[] = {
	-0.9f, -0.8f, 0.2f, 1.f, -0.9f, 0.8f, 0.1f, 1.f,
	-0.3f, -0.8f, 0.0f, 1.f, -0.3f, 0.8f,-0.1f, 1.f,
	 0.3f, -0.8f, 0.3f, 1.f,  0.3f, 0.8f, 0.2f, 1.f,
	 0.9f, -0.8f, 0.1f, 1.f,  0.9f, 0.8f, 0.0f, 1.f,
};

static const float fan_verts[] = {
	 0.0f,  0.0f, 0.2f, 1.f,
	 0.8f,  0.0f, 0.1f, 1.f,
	 0.55f, 0.6f, 0.0f, 1.f,
	-0.2f,  0.8f,-0.1f, 1.f,
	-0.85f, 0.3f, 0.2f, 1.f,
	-0.7f, -0.5f, 0.3f, 1.f,
	 0.4f, -0.7f, 0.1f, 1.f,
};

static int run_geometry_group(void)
{
	int checks = 0;
	#define GCHECK(x) do { UNIT_CHECK(x); ++checks; } while (0)

	scene scenes[] = {
		{ GL_TRIANGLES, shared_edge_verts, 6, 4, NULL, 0, 1 },
		{ GL_TRIANGLES, perspective_verts, 3, 4, NULL, 0, 1 },
		{ GL_TRIANGLES, near_clip_verts, 6, 4, NULL, 0, 1 },
		{ GL_TRIANGLES, indexed_verts, 4, 4, indexed_ids, 6, 1 },
		{ GL_TRIANGLE_STRIP, strip_verts, 8, 4, NULL, 0, 1 },
		{ GL_TRIANGLE_FAN, fan_verts, 7, 4, NULL, 0, 1 },
		{ GL_TRIANGLES, indexed_verts, 4, 4, indexed_ids, 6, 2 },
	};
	const GLsizei worker_counts[] = { 1, 2, 4 };

	for (unsigned si = 0; si < sizeof(scenes) / sizeof(scenes[0]); ++si) {
		for (unsigned wi = 0; wi < 3; ++wi) {
			renderer serial_r, tile_r;
			renderer_init(&serial_r, 0, 0);
			renderer_init(&tile_r, 0, 0);

			reset_records();
			run_scene(&serial_r, &scenes[si], 0, serial_r.program, GL_TRUE);
			int serial_vs = vs_calls, serial_fs = fs_calls;

			reset_records();
			run_scene(&tile_r, &scenes[si], worker_counts[wi],
			          tile_r.program, GL_TRUE);
			int tile_vs = vs_calls, tile_fs = fs_calls, tile_foreign = fs_foreign;

			GCHECK(tile_vs == serial_vs);
			GCHECK(tile_fs == serial_fs && tile_fs > 0);
			GCHECK(surfaces_equal(&serial_r, &tile_r));
			GCHECK(tile_foreign > 0);
			GCHECK(glGetError() == GL_NO_ERROR);

			renderer_free(&serial_r);
			renderer_free(&tile_r);
		}
	}
	return checks;
	#undef GCHECK
}

// ---------------------------------------------------------------------------
// ordering group
// ---------------------------------------------------------------------------

// Many translucent triangles all covering the center pixels; with
// PGL_CHUNK_PRIMS=64 this spans several batches and every center pixel sees
// primitives from different batches in emission order.
static float* make_overlap_tris(int* out_nverts)
{
	int ntris = 200;
	float* v = (float*)malloc((size_t)ntris * 3 * 4 * sizeof(float));
	for (int t = 0; t < ntris; ++t) {
		float jitter = ((t % 7) - 3) * 0.01f;
		float z = 0.6f - (t % 11) * 0.05f;
		float tri[12] = {
			-0.7f + jitter, -0.7f, z, 1.f,
			 0.7f + jitter, -0.7f, z, 1.f,
			 0.0f,          0.7f, z - 0.1f, 1.f,
		};
		memcpy(v + t * 12, tri, sizeof(tri));
	}
	*out_nverts = ntris * 3;
	return v;
}

static int run_ordering_group(void)
{
	int checks = 0;
	#define OCHECK(x) do { UNIT_CHECK(x); ++checks; } while (0)

	const GLsizei worker_counts[] = { 1, 2, 4 };

	// 1. Translucent overlapping faces across multiple batches.
	int nverts = 0;
	float* overlap = make_overlap_tris(&nverts);
	for (unsigned wi = 0; wi < 3; ++wi) {
		renderer serial_r, tile_r;
		renderer_init(&serial_r, 0, 0);
		renderer_init(&tile_r, 0, 0);
		scene s = { GL_TRIANGLES, overlap, nverts, 4, NULL, 0, 1 };

		reset_records();
		run_scene(&serial_r, &s, 0, serial_r.program, GL_TRUE);
		int serial_fs = fs_calls;
		reset_records();
		run_scene(&tile_r, &s, worker_counts[wi], tile_r.program, GL_TRUE);
		OCHECK(fs_calls == serial_fs);
		OCHECK(surfaces_equal(&serial_r, &tile_r));

		renderer_free(&serial_r);
		renderer_free(&tile_r);
	}
	free(overlap);

	// 2. discard + shader-written depth, blended.
	for (unsigned wi = 0; wi < 3; ++wi) {
		renderer serial_r, tile_r;
		renderer_init(&serial_r, 0, 0);
		renderer_init(&tile_r, 0, 0);
		scene s = { GL_TRIANGLES, near_clip_verts, 6, 4, NULL, 0, 1 };

		run_scene(&serial_r, &s, 0, serial_r.discard_program, GL_TRUE);
		run_scene(&tile_r, &s, worker_counts[wi], tile_r.discard_program, GL_TRUE);
		OCHECK(surfaces_equal(&serial_r, &tile_r));

		renderer_free(&serial_r);
		renderer_free(&tile_r);
	}

	// 3. Color write mask, scissor and a reduced viewport.
	for (unsigned wi = 0; wi < 3; ++wi) {
		renderer serial_r, tile_r;
		renderer_init(&serial_r, 0, 0);
		renderer_init(&tile_r, 0, 0);
		scene s = { GL_TRIANGLE_STRIP, strip_verts, 8, 4, NULL, 0, 1 };

		for (int pass = 0; pass < 2; ++pass) {
			renderer* r = pass == 0 ? &serial_r : &tile_r;
			GLsizei workers = pass == 0 ? 0 : worker_counts[wi];
			set_glContext(&r->ctx);
			pglSetTileWorkers(workers);
			g_tex = r->tex_name;
			glUseProgram(r->program);
			upload_vertices(s.verts, s.ncomp, s.nverts);
			clear_surface();
			common_state(GL_TRUE, GL_TRUE, GL_TRUE);
			glViewport(8, 8, 32, 28);
			glEnable(GL_SCISSOR_TEST);
			glScissor(4, 4, 40, 40);
			glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
			glDrawArrays(s.mode, 0, s.nverts);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glDisable(GL_SCISSOR_TEST);
			glViewport(0, 0, TEST_W, TEST_H);
		}
		OCHECK(surfaces_equal(&serial_r, &tile_r));
		renderer_free(&serial_r);
		renderer_free(&tile_r);
	}

#if !defined(PGL_NO_DEPTH_NO_STENCIL) && !defined(PGL_NO_STENCIL)
	// 4. Front/back stencil: oppositely wound faces, separate front/back state.
	static const float wound_verts[] = {
		-0.8f, -0.8f, 0.2f, 1.f,  0.8f, -0.8f, 0.2f, 1.f,  0.0f, 0.8f, 0.2f, 1.f,
		-0.8f, -0.7f, 0.1f, 1.f,  0.0f, 0.7f, 0.1f, 1.f,  0.8f, -0.7f, 0.1f, 1.f,
	};
	for (unsigned wi = 0; wi < 3; ++wi) {
		renderer serial_r, tile_r;
		renderer_init(&serial_r, 0, 0);
		renderer_init(&tile_r, 0, 0);
		for (int pass = 0; pass < 2; ++pass) {
			renderer* r = pass == 0 ? &serial_r : &tile_r;
			GLsizei workers = pass == 0 ? 0 : worker_counts[wi];
			set_glContext(&r->ctx);
			pglSetTileWorkers(workers);
			g_tex = r->tex_name;
			glUseProgram(r->program);
			upload_vertices(wound_verts, 4, 6);
			clear_surface();
			glDisable(GL_BLEND);
			glEnable(GL_DEPTH_TEST);
			glEnable(GL_STENCIL_TEST);
			glStencilFuncSeparate(GL_FRONT, GL_ALWAYS, 3, 0xFF);
			glStencilFuncSeparate(GL_BACK, GL_ALWAYS, 5, 0xFF);
			glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_REPLACE);
			glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_INCR);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}
		OCHECK(surfaces_equal(&serial_r, &tile_r));
		int stencil_diff = 0;
		for (int i = 0; i < TEST_W * TEST_H; ++i) {
#  ifdef PGL_D16
			u8 sa = ((u8*)serial_r.ctx.stencil_buf.buf)[i];
			u8 sb = ((u8*)tile_r.ctx.stencil_buf.buf)[i];
#  else
			u8 sa = (u8)(((u32*)serial_r.ctx.zbuf.buf)[i] & 0xFF);
			u8 sb = (u8)(((u32*)tile_r.ctx.zbuf.buf)[i] & 0xFF);
#  endif
			if (sa != sb)
				++stencil_diff;
		}
		OCHECK(stencil_diff == 0);
		renderer_free(&serial_r);
		renderer_free(&tile_r);
	}
#endif

#ifdef PGL_D24S8
	// 5. FBO: RGBA8 color texture plus a packed depth/stencil renderbuffer.
	for (unsigned wi = 0; wi < 3; ++wi) {
		Color fbo_pix_a[TEST_W * TEST_H];
		Color fbo_pix_b[TEST_W * TEST_H];
		memset(fbo_pix_a, 0, sizeof(fbo_pix_a));
		memset(fbo_pix_b, 0, sizeof(fbo_pix_b));

		renderer serial_r, tile_r;
		renderer_init(&serial_r, 0, 0);
		renderer_init(&tile_r, 0, 0);

		renderer* rs[2] = { &serial_r, &tile_r };
		Color* storage[2] = { fbo_pix_a, fbo_pix_b };
		GLsizei workers[2] = { 0, worker_counts[wi] };

		for (int pass = 0; pass < 2; ++pass) {
			renderer* r = rs[pass];
			// Object names are per-context, so make this context current
			// before generating any of its object names.
			set_glContext(&r->ctx);
			GLuint fbo, ctex, rb;
			glGenFramebuffers(1, &fbo);
			glGenTextures(1, &ctex);
			glGenRenderbuffers(1, &rb);

			pglSetTileWorkers(workers[pass]);
			glBindTexture(GL_TEXTURE_2D, ctex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEST_W, TEST_H, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, storage[pass]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

			glBindRenderbuffer(GL_RENDERBUFFER, rb);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
			                      TEST_W, TEST_H);

			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			                       GL_TEXTURE_2D, ctex, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			                          GL_RENDERBUFFER, rb);
			OCHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

			g_tex = r->tex_name;
			glUseProgram(r->discard_program);
			upload_vertices(near_clip_verts, 4, 6);
			clear_surface();
			common_state(GL_TRUE, GL_TRUE, GL_TRUE);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}

		OCHECK(memcmp(fbo_pix_a, fbo_pix_b, sizeof(fbo_pix_a)) == 0);

		set_glContext(&tile_r.ctx);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		renderer_free(&serial_r);
		renderer_free(&tile_r);
	}
#endif // PGL_D24S8

	return checks;
	#undef OCHECK
}

// ---------------------------------------------------------------------------
// workers group
// ---------------------------------------------------------------------------

static int overlap_pred(void* ignored) { (void)ignored; return overlap_count >= 2; }

struct pause_ctx {
	int observed_two_ready;
	int timed_out;
};

static void* pause_controller(void* arg)
{
	struct pause_ctx* pc = (struct pause_ctx*)arg;

	// Wait until a worker is parked inside the first batch's fragment shader.
	pthread_mutex_lock(&rec_lock);
	if (!wait_while_locked(active_ge1, NULL, OVERLAP_TIMEOUT_SEC)) {
		pc->timed_out = 1;
		pthread_mutex_unlock(&rec_lock);
		return NULL;
	}

	// Then wait for the caller to publish a second batch before the first one
	// can retire (workers are parked). Poll the hook records with a timeout.
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += OVERLAP_TIMEOUT_SEC;
	for (;;) {
		pthread_mutex_lock(&hook_lock);
		int ready2 = hook_nready >= 2;
		int retired_lags = hook_nretired < hook_nready;
		pthread_mutex_unlock(&hook_lock);
		if (ready2 && retired_lags) {
			pc->observed_two_ready = 1;
			break;
		}
		struct timespec now;
		clock_gettime(CLOCK_REALTIME, &now);
		if (now.tv_sec > ts.tv_sec ||
		    (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec)) {
			pc->timed_out = 1;
			break;
		}
		struct timespec step = { 0, 2 * 1000 * 1000 };
		nanosleep(&step, NULL);
	}

	// Release the parked workers so the draw can finish.
	fs_paused = 0;
	pthread_cond_broadcast(&rec_cond);
	pthread_mutex_unlock(&rec_lock);
	return NULL;
}

static int run_workers_group(void)
{
	int checks = 0;
	#define WCHECK(x) do { UNIT_CHECK(x); ++checks; } while (0)

	caller_thread = pthread_self();

	// 1. Two distinct worker threads really do run fragment callbacks at the
	// same time. A wide triangle with 2 workers, parked on a handshake.
	{
		renderer r;
		renderer_init(&r, 0, 0);
		pthread_mutex_lock(&rec_lock);
		overlap_count = 0;
		overlap_released = 0;
		overlap_distinct = 0;
		pthread_mutex_unlock(&rec_lock);
		reset_records();

		set_glContext(&r.ctx);
		pglSetTileWorkers(2);
		g_tex = r.tex_name;
		glUseProgram(r.handshake_program);
		upload_vertices(shared_edge_verts, 4, 6);
		clear_surface();
		common_state(GL_FALSE, GL_FALSE, GL_FALSE);
		glDrawArrays(GL_TRIANGLES, 0, 6);

		pthread_mutex_lock(&rec_lock);
		int ok = wait_while_locked(overlap_pred, NULL, OVERLAP_TIMEOUT_SEC);
		overlap_released = 1;
		pthread_cond_broadcast(&rec_cond);
		pthread_mutex_unlock(&rec_lock);
		WCHECK(ok);
		WCHECK(overlap_distinct);
		WCHECK(fs_foreign > 0);
		renderer_free(&r);
	}

	// 2. Batch sequencing: second batch is published while the first is still
	// active, and in-flight batches never exceed two.
	{
		renderer r;
		renderer_init(&r, 0, 0);
		int nverts = 0;
		float* overlap = make_overlap_tris(&nverts);

		reset_hooks();
		pthread_mutex_lock(&rec_lock);
		fs_paused = 1;
		fs_active = 0;
		pthread_mutex_unlock(&rec_lock);

		struct pause_ctx pc = { 0, 0 };
		pthread_t controller;
		int rc = pthread_create(&controller, NULL, pause_controller, &pc);
		WCHECK(rc == 0);

		set_glContext(&r.ctx);
		pglSetTileWorkers(2);
		g_tex = r.tex_name;
		glUseProgram(r.pause_program);
		upload_vertices(overlap, 4, nverts);
		clear_surface();
		common_state(GL_TRUE, GL_TRUE, GL_FALSE);
		glDrawArrays(GL_TRIANGLES, 0, nverts);
		pthread_join(controller, NULL);

		WCHECK(!pc.timed_out);
		WCHECK(pc.observed_two_ready);

		pthread_mutex_lock(&hook_lock);
		int nready = hook_nready, nretired = hook_nretired, maxflight = hook_max_inflight;
		int continuous = 1;
		for (int i = 0; i < nready; ++i) {
			if (hook_ready[i] != i)
				continuous = 0;
		}
		int paired = (nready == nretired && nready >= 2);
		int order_ok = 1;
		for (int i = 0; i < nretired; ++i) {
			if (hook_retired[i] != i)
				order_ok = 0;
		}
		pthread_mutex_unlock(&hook_lock);

		WCHECK(continuous);
		WCHECK(paired);
		WCHECK(order_ok);
		WCHECK(maxflight == 2);
		WCHECK(hook_max_inflight <= 2);

		free(overlap);
		renderer_free(&r);
	}

	// 3. Two contexts with different textures and mip footprints, interleaved.
	{
		renderer a, b;
		renderer_init(&a, 1, 11);
		renderer_init(&b, 1, 77);
		scene sa = { GL_TRIANGLES, perspective_verts, 3, 4, NULL, 0, 1 };
		scene sb = { GL_TRIANGLE_FAN, fan_verts, 7, 4, NULL, 0, 1 };
		Color ref_a[TEST_W * TEST_H];
		Color ref_b[TEST_W * TEST_H];

		set_glContext(&a.ctx);
		pglSetTileWorkers(0);
		run_scene(&a, &sa, 0, a.program, GL_FALSE);
		memcpy(ref_a, a.ctx.back_buffer.buf, sizeof(ref_a));
		set_glContext(&b.ctx);
		pglSetTileWorkers(0);
		run_scene(&b, &sb, 0, b.program, GL_FALSE);
		memcpy(ref_b, b.ctx.back_buffer.buf, sizeof(ref_b));

		run_scene(&b, &sb, 4, b.program, GL_FALSE);
		run_scene(&a, &sa, 4, a.program, GL_FALSE);
		run_scene(&b, &sb, 2, b.program, GL_FALSE);
		run_scene(&a, &sa, 1, a.program, GL_FALSE);

		WCHECK(memcmp(ref_a, a.ctx.back_buffer.buf, sizeof(ref_a)) == 0);
		WCHECK(memcmp(ref_b, b.ctx.back_buffer.buf, sizeof(ref_b)) == 0);
		renderer_free(&a);
		renderer_free(&b);
	}

	// 4. Reconfiguration: negative values fail GL_INVALID_VALUE and leave the
	// current configuration in place; switching worker counts keeps working.
	{
		renderer r;
		renderer_init(&r, 0, 0);
		scene s = { GL_TRIANGLES, shared_edge_verts, 6, 4, NULL, 0, 1 };

		WCHECK(pglSetTileWorkers(2) == GL_TRUE);
		WCHECK(pglSetTileWorkers(-1) == GL_FALSE);
		WCHECK(glGetError() == GL_INVALID_VALUE);
		reset_records();
		run_scene(&r, &s, 2, r.program, GL_FALSE);
		WCHECK(fs_foreign > 0);

		WCHECK(pglSetTileWorkers(4) == GL_TRUE);
		reset_records();
		run_scene(&r, &s, 4, r.program, GL_FALSE);
		WCHECK(fs_foreign > 0);

		WCHECK(pglSetTileWorkers(1) == GL_TRUE);
		reset_records();
		run_scene(&r, &s, 1, r.program, GL_FALSE);
		WCHECK(fs_foreign > 0);

		WCHECK(pglSetTileWorkers(0) == GL_TRUE);
		reset_records();
		run_scene(&r, &s, 0, r.program, GL_FALSE);
		WCHECK(fs_foreign == 0);
		renderer_free(&r);
	}

	// 5. Points and lines stay on the serial (caller) thread even with workers.
	{
		renderer r;
		renderer_init(&r, 0, 0);
		static const float pts[] = {
			-0.5f, -0.5f, 0.2f, 1.f, 0.5f, -0.5f, 0.2f, 1.f,
			 0.5f,  0.5f, 0.2f, 1.f, -0.5f, 0.5f, 0.2f, 1.f,
		};
		set_glContext(&r.ctx);
		pglSetTileWorkers(4);
		g_tex = r.tex_name;
		glUseProgram(r.program);
		upload_vertices(pts, 4, 4);
		clear_surface();
		common_state(GL_FALSE, GL_FALSE, GL_FALSE);
		reset_records();
		glDrawArrays(GL_POINTS, 0, 4);
		glDrawArrays(GL_LINE_STRIP, 0, 4);
		WCHECK(fs_calls > 0);
		WCHECK(fs_foreign == 0);
		renderer_free(&r);
	}

	// 6. Free with live workers, rebuild, and the sequence restarts at 0 in
	// the new context; within one context it stays monotonic across toggles.
	{
		renderer r;
		renderer_init(&r, 0, 0);
		set_glContext(&r.ctx);
		WCHECK(pglSetTileWorkers(2) == GL_TRUE);
		scene s = { GL_TRIANGLES, shared_edge_verts, 6, 4, NULL, 0, 1 };
		reset_hooks();
		run_scene(&r, &s, 2, r.program, GL_FALSE);
		pthread_mutex_lock(&hook_lock);
		int first_seq = hook_nready > 0 ? hook_ready[0] : -1;
		pthread_mutex_unlock(&hook_lock);
		WCHECK(first_seq == 0);

		// Turn the backend off and back on: sequences keep advancing, they do
		// not restart while the context lives.
		WCHECK(pglSetTileWorkers(0) == GL_TRUE);
		run_scene(&r, &s, 0, r.program, GL_FALSE);
		WCHECK(pglSetTileWorkers(2) == GL_TRUE);
		reset_hooks();
		run_scene(&r, &s, 2, r.program, GL_FALSE);
		pthread_mutex_lock(&hook_lock);
		int resumed = hook_nready > 0 ? hook_ready[0] : -1;
		pthread_mutex_unlock(&hook_lock);
		WCHECK(resumed > 0);
		renderer_free(&r);

		renderer r2;
		renderer_init(&r2, 0, 0);
		set_glContext(&r2.ctx);
		WCHECK(pglSetTileWorkers(2) == GL_TRUE);
		reset_hooks();
		run_scene(&r2, &s, 2, r2.program, GL_FALSE);
		pthread_mutex_lock(&hook_lock);
		int rebuilt_seq = hook_nready > 0 ? hook_ready[0] : -1;
		pthread_mutex_unlock(&hook_lock);
		WCHECK(rebuilt_seq == 0);
		renderer_free(&r2);
	}

	// 7. Resizing the framebuffer while a worker pool exists keeps serial and
	// threaded output identical and leaves no stale tile geometry behind.
	{
		renderer serial_r, tile_r;
		renderer_init(&serial_r, 0, 0);
		renderer_init(&tile_r, 0, 0);
		set_glContext(&serial_r.ctx);
		WCHECK(pglResizeFramebuffer(TEST_W + 16, TEST_H - 8) == GL_TRUE);
		set_glContext(&tile_r.ctx);
		WCHECK(pglSetTileWorkers(4) == GL_TRUE);
		WCHECK(pglResizeFramebuffer(TEST_W + 16, TEST_H - 8) == GL_TRUE);

		scene s = { GL_TRIANGLES, shared_edge_verts, 6, 4, NULL, 0, 1 };
		reset_records();
		run_scene(&serial_r, &s, 0, serial_r.program, GL_TRUE);
		int serial_fs = fs_calls;
		reset_records();
		run_scene(&tile_r, &s, 4, tile_r.program, GL_TRUE);
		WCHECK(fs_calls == serial_fs);
		// Same dimensions as the resized contexts.
		int rw = TEST_W + 16, rh = TEST_H - 8;
		pix_t* pa = (pix_t*)serial_r.ctx.back_buffer.buf;
		pix_t* pb = (pix_t*)tile_r.ctx.back_buffer.buf;
		int color_diff = 0;
		for (int i = 0; i < rw * rh; ++i) {
			if (pa[i] != pb[i])
				++color_diff;
		}
		WCHECK(color_diff == 0);
		renderer_free(&serial_r);
		renderer_free(&tile_r);
	}

	return checks;
	#undef WCHECK
}

int unit_run_group(const char* group)
{
	if (!strcmp(group, "geometry"))
		return run_geometry_group();
	if (!strcmp(group, "ordering"))
		return run_ordering_group();
	if (!strcmp(group, "workers"))
		return run_workers_group();
	return 0;
}
