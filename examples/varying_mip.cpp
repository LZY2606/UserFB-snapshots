// varying_mip.cpp
//
// Oblique perspective checkerboard rendered headlessly to a binary PPM.
//
// The checker UVs are perspective-correct vertex outputs sampled with
// pglTexture2DVarying(). Unlike texture2D(), whose mip LOD is one constant
// per triangle (estimated from the first non-flat UV pair), this uses the
// analytic per-fragment pgl_Ddx/pgl_Ddy derivatives of the two UV slots, so
// the near edge selects coarse magnification and the far edge walks up the
// mip chain within the same triangle.
//
// Usage: varying_mip [output.ppm]

#include <portablegl.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static constexpr int W = 512;
static constexpr int H = 512;
static constexpr int TEX = 128;
static constexpr int CHECKS = 8; // checker cells per texture edge

// UV slots 0 and 1 are a single smooth vec2. The derivatives come from the
// Shader_Builtins the triangle rasterizer fills for this fragment.
static void mip_fs(float* fs_input, Shader_Builtins* b, void* uniforms)
{
	GLuint tex = *static_cast<GLuint*>(uniforms);
	b->gl_FragColor = pglTexture2DVarying(tex, fs_input, b, 0, 1);
}

static void uv_vs(float* out, vec4* a, Shader_Builtins* b, void*)
{
	b->gl_Position = a[0];
	out[0] = a[1].x;
	out[1] = a[1].y;
}

// Box-downsample the RGBA checker into a full mip chain so the distance
// converges to average gray instead of sparkling.
static GLuint make_checker_mip_chain()
{
	std::vector<unsigned char> rgba((size_t)TEX * TEX * 4);
	for (int y = 0; y < TEX; ++y) {
		for (int x = 0; x < TEX; ++x) {
			int cx = x / (TEX / CHECKS);
			int cy = y / (TEX / CHECKS);
			unsigned char v = ((cx + cy) & 1) ? 225 : 28;
			size_t i = ((size_t)y * TEX + x) * 4;
			rgba[i + 0] = v;
			rgba[i + 1] = v;
			rgba[i + 2] = v;
			rgba[i + 3] = 255;
		}
	}

	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	int size = TEX;
	int level = 0;
	std::vector<std::vector<unsigned char> > chain;
	chain.push_back(rgba);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, chain[0].data());

	while (size > 1) {
		int nsize = size / 2;
		std::vector<unsigned char> next((size_t)nsize * nsize * 4);
		const unsigned char* prev = chain.back().data();
		for (int y = 0; y < nsize; ++y) {
			for (int x = 0; x < nsize; ++x) {
				for (int ch = 0; ch < 4; ++ch) {
					int sum = 0;
					for (int yy = 0; yy < 2; ++yy)
						for (int xx = 0; xx < 2; ++xx)
							sum += prev[((size_t)(2 * y + yy) * size + (2 * x + xx)) * 4 + ch];
					next[((size_t)y * nsize + x) * 4 + ch] =
						(unsigned char)(sum / 4);
				}
			}
		}
		chain.push_back(next);
		size = nsize;
		++level;
		glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, size, size, 0, GL_RGBA,
		             GL_UNSIGNED_BYTE, chain.back().data());
	}
	return tex;
}

int main(int argc, char** argv)
{
	const char* out_path = (argc > 1) ? argv[1] : "varying_mip.ppm";

	glContext ctx;
	pix_t* pixels = nullptr;
	if (!init_glContext(&ctx, &pixels, W, H)) {
		std::fprintf(stderr, "init_glContext failed\n");
		return 1;
	}
	ctx.dbg_output = GL_FALSE;

	glViewport(0, 0, W, H);
	glClearColor(0.12f, 0.14f, 0.18f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	GLuint tex = make_checker_mip_chain();

	// Oblique ground quad. Far vertices carry a large w, near vertices w=1,
	// so perspective-correct UVs compress strongly toward the top of the image
	// and the analytic V derivative grows with distance.
	// 6 floats/vertex: clip.xyzw then uv.
	const float UV = 4.0f; // repeat the checker along each axis
	float verts[4][6] = {
		// x_clip, y_clip, z, w,   u,    v
		{ -1.00f, -1.00f, 0.0f,  1.0f, 0.0f, 0.0f }, // near left
		{  1.00f, -1.00f, 0.0f,  1.0f, UV,   0.0f }, // near right
		{ -3.50f,  9.20f, 0.0f, 10.0f, 0.0f, UV   }, // far left  (ndc -.35,.92)
		{  3.50f,  9.20f, 0.0f, 10.0f, UV,   UV   }  // far right (ndc  .35,.92)
	};
	// Two CCW triangles: 0,1,2 and 1,3,2.
	unsigned int idx[6] = { 0, 1, 2, 1, 3, 2 };

	GLenum interp[2] = { PGL_SMOOTH, PGL_SMOOTH };
	GLuint prog = pglCreateProgram(uv_vs, mip_fs, 2, interp, GL_FALSE);
	glUseProgram(prog);
	GLuint uniforms_mem = tex;
	pglSetUniform(&uniforms_mem);

	GLuint vbo, ebo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
	                      (void*)(4 * sizeof(float)));

	glGenBuffers(1, &ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);

	std::vector<unsigned char> rgba((size_t)W * H * 4);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

	std::FILE* f = std::fopen(out_path, "wb");
	if (!f) {
		std::fprintf(stderr, "could not open %s for writing\n", out_path);
		free_glContext(&ctx);
		return 1;
	}
	std::fprintf(f, "P6\n%d %d\n255\n", W, H);
	// glReadPixels is bottom-row-first; PPM is top-row-first.
	std::vector<unsigned char> rgb((size_t)W * 3);
	for (int y = H - 1; y >= 0; --y) {
		for (int x = 0; x < W; ++x) {
			const unsigned char* p = &rgba[((size_t)y * W + x) * 4];
			rgb[x * 3 + 0] = p[0];
			rgb[x * 3 + 1] = p[1];
			rgb[x * 3 + 2] = p[2];
		}
		std::fwrite(rgb.data(), 1, rgb.size(), f);
	}
	std::fclose(f);

	std::printf("wrote oblique varying-mip checkerboard to %s\n", out_path);
	free_glContext(&ctx);
	return 0;
}
