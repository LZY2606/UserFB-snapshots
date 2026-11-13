// Oblique, perspective-foreshortened checkerboard sampled with the analytic
// interpolation derivatives exposed through pglTexture2DVarying.  The near
// edge is magnified (base mip) while the far edge is strongly minified and
// walks up the mip chain, so the level tint bands are visible in the PPM.
//
// Usage: varying_mip <output.ppm>
//
// The implementation is compiled in via task_support/library.cpp; this file
// only includes the header.
#include <portablegl.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int W = 256;
constexpr int H = 256;
constexpr int TEX_BASE = 64;
constexpr int TEX_LEVELS = 7; // 64,32,16,8,4,2,1

// Distinct tint per level so the chosen LOD is visible while a checker
// pattern survives at the larger levels.
const u8 tint[TEX_LEVELS][3] = {
	{ 235, 235, 235 }, // L0 near-neutral (checkerboard lives here)
	{ 180, 220, 255 }, // L1 pale blue
	{ 160, 255, 200 }, // L2 pale green
	{ 255, 230, 150 }, // L3 pale yellow
	{ 255, 190, 170 }, // L4 pale red
	{ 220, 180, 255 }, // L5 pale violet
	{ 120, 220, 220 }, // L6 teal (single texel)
};

GLuint g_tex;

void vs(float* out, vec4* a, Shader_Builtins* b, void*)
{
	b->gl_Position = a[0];
	out[0] = a[1].x; // U, smooth
	out[1] = a[1].y; // V, smooth
}

void fs(float* in, Shader_Builtins* b, void*)
{
	// The material chooses the UV pair explicitly and lets the analytic
	// derivatives drive the Grad LOD decision.
	b->gl_FragColor = pglTexture2DVarying(g_tex, in, b, 0, 1);
}

// One RGBA8 mip level.  L0 is a checkerboard (the magnified near end always
// samples it); every higher level is a solid tint so the foreshortened far
// region shows one clear, distinct color per selected mip level.
std::vector<Color> make_level(int level, int dim)
{
	std::vector<Color> img(dim * dim);
	if (level == 0) {
		int cell_px = dim / 8; // eight checker cells across
		for (int y = 0; y < dim; ++y)
			for (int x = 0; x < dim; ++x) {
				bool dark = (((x / cell_px) + (y / cell_px)) & 1) != 0;
				u8 v = dark ? 40 : 245;
				img[y * dim + x] = make_Color(v, v, v, 255);
			}
		return img;
	}
	for (Color& c4 : img)
		c4 = make_Color(tint[level][0], tint[level][1], tint[level][2], 255);
	return img;
}

GLuint build_texture(glContext& ctx)
{
	set_glContext(&ctx);
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	// Crisp, integer level selection so the foreshortening bands are obvious.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	int dim = TEX_BASE;
	for (int lvl = 0; lvl < TEX_LEVELS; ++lvl) {
		std::vector<Color> img = make_level(lvl, dim);
		glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, dim, dim, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, img.data());
		dim /= 2;
	}
	return tex;
}

void write_ppm(const char* path, glContext& ctx)
{
	FILE* f = fopen(path, "wb");
	if (!f) {
		std::fprintf(stderr, "cannot open %s\n", path);
		std::exit(1);
	}
	std::fprintf(f, "P6\n%d %d\n255\n", W, H);
	// Logical fragment row y lives at lastrow[-y*W+x]; emit top row first.
	pix_t* lastrow = (pix_t*)ctx.back_buffer.lastrow;
	for (int y = H - 1; y >= 0; --y) {
		for (int x = 0; x < W; ++x) {
			Color c = PIXEL_TO_COLOR(lastrow[-(size_t)y * W + x]);
			u8 rgb[3] = { c.r, c.g, c.b };
			std::fwrite(rgb, 1, 3, f);
		}
	}
	std::fclose(f);
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
		return 1;
	}

	glContext ctx;
	pix_t* pixels = nullptr;
	if (!init_glContext(&ctx, &pixels, W, H))
		return 1;
	set_glContext(&ctx);
	glDisable(GL_DEBUG_OUTPUT);

	g_tex = build_texture(ctx);
	GLenum interp[2] = { PGL_SMOOTH, PGL_SMOOTH };
	glUseProgram(pglCreateProgram(vs, fs, 2, interp, GL_FALSE));

	// Trapezoid in clip space: wide near edge (small w), narrow far edge
	// (large w).  Perspective divide compresses the far end while the same UV
	// range spans it, so the analytic Grad LOD walks steadily from the base
	// level up the mip chain toward the horizon instead of jumping.
	const float w_near = 1.0f;
	const float w_far = 3.0f;
	// Far clip x = ±0.0586 with w=3 -> NDC ±0.0195 -> ~10 px screen width, so
	// UV 0..4 over 10 px gives rho ~ 25 (roughly level 5) at the far tip while
	// the full-width near edge magnifies to level 0; the width changes
	// gradually so each intermediate level gets a broad horizontal band.
	float pos[16] = {
		-1.0f, -0.95f, 0.0f, w_near,   // near-left
		 1.0f, -0.95f, 0.0f, w_near,   // near-right
		-0.0586f, 0.95f, 0.0f, w_far,  // far-left
		 0.0586f, 0.95f, 0.0f, w_far,  // far-right
	};
	float uv[8] = {
		0.0f, 0.0f,
		4.0f, 0.0f,
		0.0f, 4.0f,
		4.0f, 4.0f,
	};

	GLuint bufs[2];
	glGenBuffers(2, bufs);
	glBindBuffer(GL_ARRAY_BUFFER, bufs[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
	glBindBuffer(GL_ARRAY_BUFFER, bufs[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(uv), uv, GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);

	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	write_ppm(argv[1], ctx);
	free_glContext(&ctx);
	return 0;
}
