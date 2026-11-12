/* conservative_raster.c
 *
 * Windowless demo of pglConservativeRaster().  Renders the same tiny scene
 * twice -- once with the normal center-sample rasterizer and once with
 * conservative rasterization enabled -- and writes both side by side to a
 * PPM image (conservative_raster.ppm, normal left / conservative right).
 *
 * The scene contains a tapered sliver triangle well under a pixel wide that
 * threads its way between pixel centers.  The normal rasterizer only shades
 * pixels whose *center* falls inside the triangle, so the sliver mostly
 * vanishes.  Conservative mode shades every pixel whose [x,x+1]x[y,y+1]
 * square the triangle overlaps with positive area, so the sliver shows up
 * as a continuous streak.
 *
 * Build (from this directory, no window system or SDL needed):
 *   cc -O2 conservative_raster.c -o conservative_raster
 * or from the repo root:
 *   cc -O2 -I. examples/original/conservative_raster.c -o conservative_raster
 *
 * ---------------------------------------------------------------------------
 * Conservative depth vs. the attribute sample point
 * ---------------------------------------------------------------------------
 * Each conservative fragment carries two different geometric quantities:
 *
 *   gl_FragCoord.xy  -- the *area centroid* of the (clipped) triangle's
 *                       overlap with the pixel square.  gl_FragCoord.w and
 *                       all smooth/noperspective varyings are evaluated at
 *                       this same point, with the usual perspective rules.
 *                       Flat varyings still come from the primitive's
 *                       provoking vertex.
 *
 *   gl_FragCoord.z   -- the *minimum* window depth over the whole overlap,
 *   (and the default    NOT the depth at the centroid.  Depth is affine
 *    gl_FragDepth)     across the projected triangle, so the minimum sits
 *                       at a vertex of the overlap polygon; it is not
 *                       perspective-interpolated.  This is what you want
 *                       when the pass records nearest coverage (e.g. for
 *                       offline geometry-visibility preprocessing): the
 *                       stored depth is the closest the triangle gets to
 *                       the viewer anywhere inside that pixel.
 *
 * If the fragment shader writes gl_FragDepth itself, that value is used
 * instead, exactly as in normal mode.
 */

#define PGL_EXCLUDE_STUBS
#define PORTABLEGL_IMPLEMENTATION
#include "../../portablegl.h"

#include <stdio.h>

#define SZ 160          /* each half is SZ x SZ */
#define OUT_W (2*SZ)
#define OUT_H SZ

pix_t* bbufpix[2];
glContext the_Context[2];

typedef struct My_Uniforms
{
	vec4 v_color;
} My_Uniforms;

My_Uniforms the_uniforms;

void smooth_vs(float* vs_output, vec4* vertex_attribs, Shader_Builtins* builtins, void* uniforms);
void smooth_fs(float* fs_input, Shader_Builtins* builtins, void* uniforms);
void sliver_vs(float* vs_output, vec4* vertex_attribs, Shader_Builtins* builtins, void* uniforms);
void sliver_fs(float* fs_input, Shader_Builtins* builtins, void* uniforms);

void render_scene(int conservative);
int write_ppm(const char* filename);

int main(int argc, char** argv)
{
	PGL_UNUSED(argc);
	PGL_UNUSED(argv);

	for (int i = 0; i < 2; ++i) {
		bbufpix[i] = (pix_t*)malloc(SZ * SZ * sizeof(pix_t));
		if (!bbufpix[i] || !init_glContext(&the_Context[i], &bbufpix[i], SZ, SZ)) {
			fprintf(stderr, "context setup failed\n");
			return 1;
		}
	}

	render_scene(0);
	render_scene(1);

	if (!write_ppm("conservative_raster.ppm"))
		return 1;

	printf("wrote conservative_raster.ppm (left: normal, right: conservative)\n");
	printf("note how the sub-pixel sliver only survives conservative rasterization\n");
	printf("and each of its fragments pairs a centroid-sampled color (gl_FragCoord.xy)\n");
	printf("with the pixel's nearest overlap depth (gl_FragCoord.z / gl_FragDepth)\n");

	free_glContext(&the_Context[0]);
	free_glContext(&the_Context[1]);
	free(bbufpix[0]);
	free(bbufpix[1]);
	return 0;
}

void smooth_vs(float* vs_output, vec4* vertex_attribs, Shader_Builtins* builtins, void* uniforms)
{
	PGL_UNUSED(uniforms);
	builtins->gl_Position = vertex_attribs[0];
	vs_output[0] = vertex_attribs[1].x;
	vs_output[1] = vertex_attribs[1].y;
	vs_output[2] = vertex_attribs[1].z;
}

void smooth_fs(float* fs_input, Shader_Builtins* builtins, void* uniforms)
{
	PGL_UNUSED(uniforms);
	builtins->gl_FragColor = make_v4(fs_input[0], fs_input[1], fs_input[2], 1.0f);
}

void sliver_vs(float* vs_output, vec4* vertex_attribs, Shader_Builtins* builtins, void* uniforms)
{
	PGL_UNUSED(uniforms);
	builtins->gl_Position = vertex_attribs[0];
}

void sliver_fs(float* fs_input, Shader_Builtins* builtins, void* uniforms)
{
	My_Uniforms* u = (My_Uniforms*)uniforms;
	PGL_UNUSED(fs_input);
	builtins->gl_FragColor = u->v_color;
}

/* window coords -> clip coords for the full SZ x SZ viewport */
static void w2c(float* out, double wx, double wy, double z)
{
	double a = (SZ - 0.01) / 2.0;
	out[0] = (float)((wx - a) / a);
	out[1] = (float)((wy - a) / a);
	out[2] = (float)z;
	out[3] = 1.0f;
}

void render_scene(int conservative)
{
	set_glContext(&the_Context[conservative]);

	GLenum interp[3] = { PGL_SMOOTH3 };
	GLuint bg_prog = pglCreateProgram(smooth_vs, smooth_fs, 3, interp, GL_FALSE);
	GLuint sl_prog = pglCreateProgram(sliver_vs, sliver_fs, 0, NULL, GL_FALSE);

	glEnable(GL_DEPTH_TEST);
	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (conservative)
		pglConservativeRaster(GL_TRUE);

	/* background: two big triangles with a smooth color gradient, z = 0.5 */
	float bg_pos[6][4] = {
		{ -1, -1, 0.5f, 1 }, { 3, -1, 0.5f, 1 }, { -1, 3, 0.5f, 1 },
		{ -1,  3, 0.5f, 1 }, { 3, -1, 0.5f, 1 }, {  3, 3, 0.5f, 1 },
	};
	float bg_col[6][4] = {
		{ 0.10f, 0.12f, 0.25f, 0 }, { 0.20f, 0.30f, 0.45f, 0 }, { 0.30f, 0.18f, 0.40f, 0 },
		{ 0.30f, 0.18f, 0.40f, 0 }, { 0.20f, 0.30f, 0.45f, 0 }, { 0.45f, 0.35f, 0.55f, 0 },
	};
	GLuint vbo[2];
	glGenBuffers(2, vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(bg_pos), bg_pos, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(bg_col), bg_col, GL_STATIC_DRAW);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(1);
	glUseProgram(bg_prog);
	glDrawArrays(GL_TRIANGLES, 0, 6);

	/* tapered sliver, ~0.35 px wide at the wide end, z = 0 (in front) */
	float sl[3][4];
	w2c(sl[0], 8.0, 12.0, 0.0);
	w2c(sl[1], 152.0, 140.0, 0.0);
	w2c(sl[2], 8.0 - 0.466, 12.0 + 0.522, 0.0);
	glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(sl), sl, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glDisableVertexAttribArray(1);
	glUseProgram(sl_prog);
	the_uniforms.v_color = make_v4(1.0f, 0.85f, 0.1f, 1.0f);
	pglSetUniform(&the_uniforms);
	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDeleteBuffers(2, vbo);
}

int write_ppm(const char* filename)
{
	FILE* f = fopen(filename, "wb");
	if (!f) {
		perror(filename);
		return 0;
	}
	fprintf(f, "P6\n%d %d\n255\n", OUT_W, OUT_H);
	for (int y = OUT_H - 1; y >= 0; --y) {          /* GL y is bottom-up */
		for (int x = 0; x < OUT_W; ++x) {
			int half = x / SZ;
			Color c = PIXEL_TO_COLOR(bbufpix[half][y * SZ + (x % SZ)]);
			fputc(c.r, f);
			fputc(c.g, f);
			fputc(c.b, f);
		}
	}
	fclose(f);
	return 1;
}
