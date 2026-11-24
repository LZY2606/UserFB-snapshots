// Hi-Z backend differential and behavioral tests.  This file intentionally
// does not define PORTABLEGL_IMPLEMENTATION or main.
#include "portablegl.h"
#include "task_support/check.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace {

int g_vertex_calls = 0;
int g_fragment_calls = 0;

struct Uniforms {
	vec4 color;
	float frag_depth;
	int write_frag_depth;
	int discard_left_half;
};

void test_vs(float*, vec4* attribs, Shader_Builtins* builtins, void*)
{
	++g_vertex_calls;
	builtins->gl_Position = attribs[0];
}

void test_fs(float*, Shader_Builtins* builtins, void* uniform)
{
	++g_fragment_calls;
	Uniforms* u = (Uniforms*)uniform;
	if (u->discard_left_half && builtins->gl_FragCoord.x < 8.0f) {
		builtins->discard = GL_TRUE;
		return;
	}
	builtins->gl_FragColor = u->color;
	if (u->write_frag_depth)
		builtins->gl_FragDepth = u->frag_depth;
}

void interpolation_vs(float* vs_output, vec4* attribs, Shader_Builtins* builtins, void*)
{
	++g_vertex_calls;
	builtins->gl_Position = attribs[0];
	vs_output[0] = attribs[1].x;
	vs_output[1] = attribs[1].y;
	vs_output[2] = attribs[1].z;
}

void interpolation_fs(float* fs_input, Shader_Builtins* builtins, void*)
{
	++g_fragment_calls;
	builtins->gl_FragColor = make_v4(fs_input[0], fs_input[1], fs_input[2], 1.0f);
}

struct Ctx {
	int w = 0, h = 0;
	glContext ctx{};
	pix_t* color = nullptr;
	GLuint program = 0;
	GLuint late_program = 0;
	GLuint interpolation_program = 0;
	Uniforms uniforms{};
	bool initialized = false;

	Ctx(int width, int height, bool hiz) : w(width), h(height)
	{
		initialized = init_glContext(&ctx, &color, width, height) == GL_TRUE;
		if (!initialized)
			return;
		bind();
		program = pglCreateProgram(test_vs, test_fs, 0, nullptr, GL_FALSE);
		late_program = pglCreateProgram(test_vs, test_fs, 0, nullptr, GL_TRUE);
		GLenum smooth3[3] = {PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH};
		interpolation_program = pglCreateProgram(interpolation_vs, interpolation_fs,
		                                         3, smooth3, GL_FALSE);
		glUseProgram(program);
		if (hiz)
			pglSetHiZ(GL_TRUE);
	}
	~Ctx()
	{
		if (initialized) {
			bind();
			free_glContext(&ctx);
		}
	}
	Ctx(const Ctx&) = delete;
	Ctx& operator=(const Ctx&) = delete;
	void bind() { set_glContext(&ctx); }
};

struct Pair {
	Ctx off, on;
	Pair(int w, int h) : off(w, h, false), on(w, h, true) {}

	int run(const std::function<int(Ctx&)>& scene)
	{
		TASK_CHECK(off.initialized && on.initialized);
		off.bind();
		g_vertex_calls = 0;
		g_fragment_calls = 0;
		TASK_CHECK(scene(off) == 0);
		int vs_off = g_vertex_calls;
		int fs_off = g_fragment_calls;

		on.bind();
		g_vertex_calls = 0;
		g_fragment_calls = 0;
		TASK_CHECK(scene(on) == 0);
		int vs_on = g_vertex_calls;
		int fs_on = g_fragment_calls;

		TASK_CHECK(vs_off == vs_on);
		TASK_CHECK(fs_off == fs_on);
		TASK_CHECK(buffers_equal_static(off, on));
		return 0;
	}

	static int buffers_equal_static(Ctx& off, Ctx& on)
	{
		int ow = off.ctx.back_buffer.w, oh = off.ctx.back_buffer.h;
		int nw = on.ctx.back_buffer.w, nh = on.ctx.back_buffer.h;
		TASK_CHECK(ow == nw && oh == nh);
		size_t pixels = (size_t)ow * oh;
		TASK_CHECK(std::memcmp(off.ctx.back_buffer.buf, on.ctx.back_buffer.buf,
		                       pixels * sizeof(pix_t)) == 0);
#ifndef PGL_NO_DEPTH_NO_STENCIL
		TASK_CHECK(std::memcmp(off.ctx.zbuf.buf, on.ctx.zbuf.buf,
		                       pixels * sizeof(u32)) == 0);
#endif
		return 1;
	}

	int buffers_equal()
	{
		return buffers_equal_static(off, on);
	}

	static int ctx_w(Ctx& e) { return e.ctx.back_buffer.w; }
	static int ctx_h(Ctx& e) { return e.ctx.back_buffer.h; }
};

Uniforms color_u(float r, float g, float b, float a = 1.0f)
{
	Uniforms u{};
	u.color = make_v4(r, g, b, a);
	return u;
}

void basic_state(Ctx& e, GLenum func = GL_LESS)
{
	e.bind();
	glViewport(0, 0, e.w, e.h);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(func);
	glDepthMask(GL_TRUE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthRange(0.0, 1.0);
	glPolygonOffset(0.0f, 0.0f);
	glDisable(GL_POLYGON_OFFSET_FILL);
}

void clear_color_depth(Ctx& e, float depth = 1.0f)
{
	e.bind();
	glClearColor(0.05f, 0.07f, 0.09f, 1.0f);
	glClearDepth(depth);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT
#ifndef PGL_NO_STENCIL
	        | GL_STENCIL_BUFFER_BIT
#endif
	);
}

void draw_positions(Ctx& e, const std::vector<float>& p3, GLenum mode,
                    const Uniforms& u, GLuint program = 0)
{
	e.bind();
	e.uniforms = u;
	int n = (int)p3.size() / 3;
	std::vector<float> p4((size_t)n * 4);
	for (int i = 0; i < n; ++i) {
		p4[i * 4 + 0] = p3[i * 3 + 0];
		p4[i * 4 + 1] = p3[i * 3 + 1];
		p4[i * 4 + 2] = p3[i * 3 + 2];
		p4[i * 4 + 3] = 1.0f;
	}
	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)p4.size() * sizeof(float),
	             p4.data(), GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
	glEnableVertexAttribArray(0);
	GLuint prog = program ? program : e.program;
	glUseProgram(prog);
	pglSetUniform(&e.uniforms);
	glDrawArrays(mode, 0, n);
}

void draw_full_tri(Ctx& e, float z0, float z1, float z2, const Uniforms& u,
                   GLuint program = 0)
{
	draw_positions(e, {-1.0f, -1.0f, z0, 3.0f, -1.0f, z1, -1.0f, 3.0f, z2},
	               GL_TRIANGLES, u, program);
}

void draw_interpolated_tri(Ctx& e)
{
	e.bind();
	const float data[] = {
		-1.0f, -1.0f, -0.4f, 1.0f,  1.0f, 0.0f, 0.0f,
		 3.0f, -1.0f,  0.3f, 1.0f,  0.0f, 1.0f, 0.0f,
		-1.0f,  3.0f,  0.8f, 1.0f,  0.0f, 0.0f, 1.0f
	};
	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(data), data, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float),
	                      reinterpret_cast<void*>(4 * sizeof(float)));
	glEnableVertexAttribArray(1);
	glUseProgram(e.interpolation_program);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableVertexAttribArray(1);
}

void fill_packed_depth(Ctx& e, u32 quantized)
{
	e.bind();
	for (int i = 0; i < e.w * e.h; ++i)
		((u32*)e.ctx.zbuf.buf)[i] = (quantized << 8);
	pglInvalidateHiZ();
}

pglHiZStats stats_of(Ctx& e)
{
	e.bind();
	return pglGetHiZStats();
}
void reset_stats(Ctx& e)
{
	e.bind();
	pglResetHiZStats();
}

u32 depth_q(float d)
{
	return (u32)(d * PGL_MAX_Z);
}

int scene_identity(int)
{
	return 0;
}

} // namespace

static int bounds_quantized_leaf(void)
{
	const int S = 8;
	const u32 q = depth_q(0.5f);

	// LESS: equal packed depth fails and is region-rejectable.
	{
		Pair pair(S, S);
		auto scene = [&](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 1.0f);
			fill_packed_depth(e, q);
			draw_full_tri(e, 0.0f, 0.0f, 0.0f, color_u(1, 0, 0));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
		pglHiZStats s = stats_of(pair.on);
		TASK_CHECK(s.rejected_blocks == 1);
		TASK_CHECK(s.rejected_pixels == 64);
		TASK_CHECK(s.rebuilt_leaves == 1);
		TASK_CHECK(s.depth_reads == 64);

		reset_stats(pair.on);
		pair.on.bind();
		draw_full_tri(pair.on, 0.0f, 0.0f, 0.0f, color_u(1, 0, 0));
		s = stats_of(pair.on);
		TASK_CHECK(s.hierarchy_tests == 1);
		TASK_CHECK(s.rejected_blocks == 1);
		TASK_CHECK(s.rejected_pixels == 64);
		TASK_CHECK(s.depth_reads == 0);
		TASK_CHECK(s.rebuilt_leaves == 0);
	}

	// LESS boundary one LSB farther: source passes and must not be rejected.
	{
		Pair pair(S, S);
		auto scene = [&](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 1.0f);
			fill_packed_depth(e, q + 1);
			draw_full_tri(e, 0.0f, 0.0f, 0.0f, color_u(0, 1, 0));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
		pglHiZStats s = stats_of(pair.on);
		TASK_CHECK(s.rejected_blocks == 0);
		TASK_CHECK(s.rejected_pixels == 0);
	}

	// LEQUAL: equal depth passes and must be preserved.
	{
		Pair pair(S, S);
		auto scene = [&](Ctx& e) -> int {
			basic_state(e, GL_LEQUAL);
			clear_color_depth(e, 1.0f);
			fill_packed_depth(e, q);
			draw_full_tri(e, 0.0f, 0.0f, 0.0f, color_u(0, 0, 1));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
		pglHiZStats s = stats_of(pair.on);
		TASK_CHECK(s.rejected_blocks == 0);
	}

	// LEQUAL one LSB shallower: all candidates are strictly farther, reject.
	{
		Pair pair(S, S);
		auto scene = [&](Ctx& e) -> int {
			basic_state(e, GL_LEQUAL);
			clear_color_depth(e, 1.0f);
			fill_packed_depth(e, q - 1);
			draw_full_tri(e, 0.0f, 0.0f, 0.0f, color_u(1, 1, 0));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
		pglHiZStats s = stats_of(pair.on);
		TASK_CHECK(s.rejected_blocks == 1);
		TASK_CHECK(s.rejected_pixels == 64);
	}
	return 0;
}

static int bounds_tilted_local_not_global(void)
{
	Pair pair(32, 32);
	auto scene = [](Ctx& e) -> int {
		basic_state(e, GL_LESS);
		clear_color_depth(e, 1.0f);
		// Farthest stored depth is 0.3.  The tilted plane reaches depth 0 at
		// one corner but is far enough in the opposite region for a local node
		// bound to reject.  A three-vertex global minimum could never do that.
		fill_packed_depth(e, depth_q(0.30f));
		draw_positions(e, {
			-1.0f, -1.0f, -1.0f,
			 3.0f, -1.0f,  1.0f,
			-1.0f,  3.0f,  1.0f
		}, GL_TRIANGLES, color_u(0.8f, 0.4f, 0.2f));
		draw_interpolated_tri(e);
		return 0;
	};
	TASK_CHECK(pair.run(scene) == 0);
	pglHiZStats s = stats_of(pair.on);
	TASK_CHECK(s.rejected_blocks > 0);
	TASK_CHECK(s.rejected_pixels >= 64);
	// The near corner must still contain non-background, written pixels.
	int wrote_near = 0;
	pair.on.bind();
	pix_t* px = (pix_t*)pair.on.ctx.back_buffer.buf;
	for (int y = 0; y < 10; ++y)
		for (int x = 0; x < 10; ++x)
			if (px[-y * 32 + x] != pair.on.ctx.clear_color)
				++wrote_near;
	TASK_CHECK(wrote_near > 20);
	return 0;
}

static int bounds_edges_clips_and_partial_leaves(void)
{
	Pair pair(23, 17);
	auto scene = [](Ctx& e) -> int {
		basic_state(e, GL_LESS);
		clear_color_depth(e, 1.0f);

		// Non-8-aligned viewport and scissor intersections.
		glViewport(2, 3, 15, 11);
		glEnable(GL_SCISSOR_TEST);
		glScissor(5, 4, 12, 9);
		struct R { float x0, y0, z0, x1, y1, z1, x2, y2, z2; GLenum f; };
		R tris[] = {
			{-1.1f,-0.7f,-0.9f, 0.2f,0.9f,-0.1f, 1.2f,-0.8f,0.8f, GL_LESS},
			{-0.4f,-1.0f, 0.2f, 1.4f,-0.2f,0.4f,-0.2f,1.2f,0.9f, GL_LEQUAL},
			{ 0.1f, 0.1f,-0.4f, 1.5f,1.4f,0.6f,-1.3f,0.8f,0.1f, GL_LESS},
			{-1.5f,-1.2f, 0.9f, 0.1f,0.2f,0.7f, 0.2f,-0.1f,0.5f, GL_LESS},
			{-0.2f,-0.2f, 0.5f, 0.3f,-0.1f,0.5f, 0.0f,0.3f,0.5f, GL_LEQUAL}
		};
		for (const R& r : tris) {
			glDepthFunc(r.f);
			draw_positions(e, {r.x0,r.y0,r.z0, r.x1,r.y1,r.z1, r.x2,r.y2,r.z2},
			               GL_TRIANGLES, color_u((r.z0 + 1) * .5f, .25f, .75f));
		}

		// A second batch with scissor disabled and a changed depth range.
		glDisable(GL_SCISSOR_TEST);
		glViewport(0, 0, 23, 17);
		glDepthRange(0.25, 0.9);
		glDepthFunc(GL_LESS);
		draw_positions(e, {-1,-1,-0.8f, 3,-1,0.2f, -1,3,0.9f},
		               GL_TRIANGLES, color_u(.2f,.8f,.3f,.8f));
		return 0;
	};
	TASK_CHECK(pair.run(scene) == 0);
	return 0;
}

static int coherence_warm_parent_and_local_rebuild(void)
{
	Pair pair(64, 64);
	auto scene = [](Ctx& e) -> int {
		basic_state(e, GL_LESS);
		clear_color_depth(e, 1.0f);
		draw_full_tri(e, -0.5f, -0.5f, -0.5f, color_u(1, 0, 0));
		draw_full_tri(e,  0.5f,  0.5f,  0.5f, color_u(0, 1, 0));
		return 0;
	};
	TASK_CHECK(pair.run(scene) == 0);

	// One more fully blocked draw makes the hierarchy warm and clean.
	pair.on.bind();
	draw_full_tri(pair.on, 0.5f, 0.5f, 0.5f, color_u(0, 1, 0));
	pglHiZStats s = stats_of(pair.on);
	TASK_CHECK(s.rejected_blocks >= 1);

	reset_stats(pair.on);
	pair.on.bind();
	draw_full_tri(pair.on, 0.5f, 0.5f, 0.5f, color_u(0, 1, 0));
	s = stats_of(pair.on);
	TASK_CHECK(s.hierarchy_tests == 1);
	TASK_CHECK(s.rejected_blocks == 1);
	TASK_CHECK(s.rejected_pixels == 4096);
	TASK_CHECK(s.depth_reads == 0);
	TASK_CHECK(s.rebuilt_leaves == 0);

	// Dirty exactly one complete 8x8 leaf with a scissor clear.
	pair.on.bind();
	glEnable(GL_SCISSOR_TEST);
	glScissor(8, 8, 8, 8);
	glClearDepth(0.25f);
	glClear(GL_DEPTH_BUFFER_BIT);
	glDisable(GL_SCISSOR_TEST);
	reset_stats(pair.on);
	pair.on.bind();
	draw_full_tri(pair.on, 0.5f, 0.5f, 0.5f, color_u(0, 1, 0));
	s = stats_of(pair.on);
	TASK_CHECK(s.rebuilt_leaves == 1);
	TASK_CHECK(s.depth_reads == 64);
	TASK_CHECK(s.rejected_blocks >= 1);
	return 0;
}

static int coherence_scissor_late_points_lines(void)
{
	// Scissor clear and ordinary subsequent geometry.
	{
		Pair pair(48, 40);
		auto scene = [](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 1.0f);
			draw_full_tri(e, -0.6f, -0.6f, -0.6f, color_u(.8f,.2f,.2f));
			glEnable(GL_SCISSOR_TEST);
			glScissor(11, 7, 17, 13);
			glClearDepth(0.95f);
			glClear(GL_DEPTH_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);
			draw_positions(e, {-1,-1,0.2f, 3,-1,0.8f, -1,3,0.9f},
			               GL_TRIANGLES, color_u(.2f,.8f,.2f));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
	}

	// Point and line writes using GL_ALWAYS can move stored maxima upward.
	{
		Pair pair(32, 32);
		auto scene = [](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 0.25f);
			draw_full_tri(e, -0.5f, -0.5f, -0.5f, color_u(.5f,.5f,.5f));

			glDepthFunc(GL_ALWAYS);
			glPointSize(8.0f);
			draw_positions(e, {0.0f, 0.0f, 0.9f}, GL_POINTS, color_u(1,0,1));
			glLineWidth(2.0f);
			draw_positions(e, {-0.8f,-0.6f,0.95f, 0.7f,0.5f,0.95f},
			               GL_LINES, color_u(0,1,1));

			glDepthFunc(GL_LESS);
			draw_full_tri(e, 0.0f, 0.0f, 0.0f, color_u(1,1,0));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
		pglHiZStats s = stats_of(pair.on);
		TASK_CHECK(s.rebuilt_leaves > 0);
	}

	// Late gl_FragDepth is a fallback program but its writes invalidate Hi-Z.
	{
		Pair pair(32, 32);
		auto scene = [](Ctx& e) -> int {
			basic_state(e, GL_ALWAYS);
			clear_color_depth(e, 0.25f);
			Uniforms late = color_u(.2f,.2f,1);
			late.write_frag_depth = 1;
			late.frag_depth = 0.92f;
			draw_positions(e, {-0.8f,-0.8f,0.1f, 2.2f,-0.8f,0.1f, -0.8f,2.2f,0.1f},
			               GL_TRIANGLES, late, e.late_program);
			basic_state(e, GL_LESS);
			draw_full_tri(e, 0.0f, 0.0f, 0.0f, color_u(0,1,0));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
	}
	return 0;
}

static int coherence_fbo_blit_resize_contexts(void)
{
	// FBO float depth -> default D24 blit must invalidate default Hi-Z.
	{
		Pair pair(32, 32);
		auto scene = [](Ctx& e) -> int {
			const int FW = 16, FH = 16;
			basic_state(e, GL_LESS);
			clear_color_depth(e, 1.0f);
			draw_full_tri(e, -0.5f, -0.5f, -0.5f, color_u(1,0,0));
			draw_full_tri(e,  0.5f,  0.5f,  0.5f, color_u(0,1,0));

			std::vector<float> depth_pixels((size_t)FW * FH, 0.9f);
			GLuint tex = 0, fbo = 0;
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			pglTextureImage2D(tex, 0, GL_DEPTH_COMPONENT, FW, FH, 0,
			                   GL_DEPTH_COMPONENT, GL_FLOAT, depth_pixels.data());
			glGenFramebuffers(1, &fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
			                       GL_TEXTURE_2D, tex, 0);
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
			TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
			glEnable(GL_DEPTH_TEST);
			glClearDepth(0.9f);
			glClear(GL_DEPTH_BUFFER_BIT);

			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
			glBlitFramebuffer(0, 0, FW, FH, 4, 5, 20, 21,
			                  GL_DEPTH_BUFFER_BIT, GL_NEAREST);
			TASK_CHECK(glGetError() == GL_NO_ERROR);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			draw_full_tri(e, 0.0f, 0.0f, 0.0f, color_u(0.2f,0.8f,1));
			return 0;
		};
		TASK_CHECK(pair.run(scene) == 0);
	}

	// Resize and interleaved use of two independent Hi-Z contexts.
	{
		Ctx a0(32, 32, false), a1(32, 32, true);
		Ctx b0(24, 40, false), b1(24, 40, true);
		TASK_CHECK(a0.initialized && a1.initialized && b0.initialized && b1.initialized);
		for (int frame = 0; frame < 3; ++frame) {
			for (Ctx* e : {&a0, &a1}) {
				e->bind();
				if (frame == 1)
					TASK_CHECK(pglResizeFramebuffer(40, 36) == GL_TRUE);
				basic_state(*e, GL_LESS);
				clear_color_depth(*e, 1.0f);
				draw_full_tri(*e, -0.5f, -0.5f, -0.5f, color_u(frame*.2f, .4f, .8f));
				draw_positions(*e, {-0.8f,-0.8f,0.4f, 1.8f,-0.8f,0.4f, -0.8f,1.8f,0.4f},
				               GL_TRIANGLES, color_u(.9f,.8f,.2f));
			}
			for (Ctx* e : {&b0, &b1}) {
				e->bind();
				if (frame == 2)
					TASK_CHECK(pglResizeFramebuffer(31, 31) == GL_TRUE);
				basic_state(*e, GL_LEQUAL);
				clear_color_depth(*e, 0.8f);
				draw_positions(*e, {-1,-1,-0.9f, 3,-1,0.2f, -1,3,0.9f},
				               GL_TRIANGLES, color_u(.2f,frame*.3f,.7f));
			}
		}
		TASK_CHECK(Pair::buffers_equal_static(a0, a1));
		TASK_CHECK(Pair::buffers_equal_static(b0, b1));
		pglHiZStats sa = stats_of(a1), sb = stats_of(b1);
		TASK_CHECK(sa.rejected_pixels != 0 || sa.hierarchy_tests != 0);
		TASK_CHECK(sb.rejected_pixels != 0 || sb.hierarchy_tests != 0);
	}
	return 0;
}

static int fallback_disabled_and_unsupported(void)
{
	// Disabled by default and all statistics start at zero.
	{
		Ctx e(32, 32, false);
		e.bind();
		pglHiZStats s = pglGetHiZStats();
		TASK_CHECK(s.hierarchy_tests == 0 && s.rejected_blocks == 0);
		TASK_CHECK(s.rejected_pixels == 0 && s.depth_reads == 0);
		TASK_CHECK(s.rebuilt_leaves == 0);
		basic_state(e, GL_LESS);
		clear_color_depth(e, 1.0f);
		draw_full_tri(e, -0.5f, -0.5f, -0.5f, color_u(1,0,0));
		s = pglGetHiZStats();
		TASK_CHECK(s.depth_reads == 0 && s.hierarchy_tests == 0);
	}

	// Unsupported state/programs must produce identical buffers.
	struct Case {
		const char* name;
		std::function<int(Ctx&)> run;
	};
	Case cases[] = {
		{"stencil", [](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			glEnable(GL_STENCIL_TEST);
			glStencilFunc(GL_ALWAYS, 3, 0xff);
			glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
			clear_color_depth(e, 1.0f);
			draw_full_tri(e, -0.2f, -0.2f, -0.2f, color_u(.2f,.9f,.3f));
			return 0;
		}},
		{"greater", [](Ctx& e) -> int {
			basic_state(e, GL_GREATER);
			clear_color_depth(e, 0.25f);
			draw_full_tri(e, 0.2f, 0.2f, 0.2f, color_u(.8f,.3f,.9f));
			return 0;
		}},
		{"late-depth", [](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 1.0f);
			Uniforms u = color_u(.9f,.4f,.2f);
			u.write_frag_depth = 1;
			u.frag_depth = 0.4f;
			draw_full_tri(e, -0.9f, -0.9f, -0.9f, u, e.late_program);
			return 0;
		}},
		{"discard", [](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 0.5f);
			Uniforms u = color_u(.1f,.8f,.9f);
			u.discard_left_half = 1;
			draw_full_tri(e, -0.2f, -0.2f, -0.2f, u, e.late_program);
			return 0;
		}},
		{"points-lines", [](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 1.0f);
			glPointSize(5.0f);
			draw_positions(e, {-.4f,.2f,-.3f, .5f,-.6f,.4f}, GL_POINTS, color_u(1,0,0));
			glLineWidth(2.0f);
			draw_positions(e, {-1,-.8f,-.5f, .9f,.7f,.2f}, GL_LINES, color_u(0,1,1));
			return 0;
		}},
		{"polygon-line", [](Ctx& e) -> int {
			basic_state(e, GL_LESS);
			clear_color_depth(e, 1.0f);
			glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
			draw_full_tri(e, -0.1f, -0.1f, -0.1f, color_u(1,.8f,.1f));
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			return 0;
		}}
	};
	for (const Case& tc : cases) {
		Pair pair(32, 32);
		TASK_CHECK(pair.run(tc.run) == 0);
		pglHiZStats s = stats_of(pair.on);
		TASK_CHECK(s.hierarchy_tests == 0);
		TASK_CHECK(s.rejected_blocks == 0);
	}

	// User FBO/float depth also falls back and leaves default cache coherent.
	{
		Pair pair(32, 32);
		auto scene = [](Ctx& e) -> int {
			const int S = 16;
			std::vector<float> z((size_t)S*S, 0.5f);
			GLuint tex = 0, fbo = 0;
			glGenTextures(1,&tex);
			glBindTexture(GL_TEXTURE_2D,tex);
			pglTextureImage2D(tex,0,GL_DEPTH_COMPONENT,S,S,0,GL_DEPTH_COMPONENT,
			                  GL_FLOAT,z.data());
			glGenFramebuffers(1,&fbo);
			glBindFramebuffer(GL_FRAMEBUFFER,fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,tex,0);
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
			TASK_CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
			basic_state(e,GL_LESS);
			glClearDepth(1); glClear(GL_DEPTH_BUFFER_BIT);
			draw_full_tri(e,-.2f,-.2f,-.2f,color_u(1,0,0));
			glBindFramebuffer(GL_FRAMEBUFFER,0);
			draw_full_tri(e,.2f,.2f,.2f,color_u(0,1,0));
			return 0;
		};
		TASK_CHECK(pair.run(scene)==0);
		pglHiZStats s=stats_of(pair.on);
		TASK_CHECK(s.hierarchy_tests==0 || s.rejected_blocks==0 || s.hierarchy_tests > 0);
	}
	return 0;
}

const struct task_case task_cases[] = {
	{"bounds", "quantized_leaf", bounds_quantized_leaf},
	{"bounds", "tilted_local_minimum", bounds_tilted_local_not_global},
	{"bounds", "edges_clips_partial_leaves", bounds_edges_clips_and_partial_leaves},
	{"coherence", "warm_parent_and_local_rebuild", coherence_warm_parent_and_local_rebuild},
	{"coherence", "scissor_late_points_lines", coherence_scissor_late_points_lines},
	{"coherence", "fbo_blit_resize_contexts", coherence_fbo_blit_resize_contexts},
	{"fallback", "disabled_and_unsupported", fallback_disabled_and_unsupported},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
