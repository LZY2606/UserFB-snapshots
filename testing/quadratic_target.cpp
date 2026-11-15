// GL_QUADRATIC_CURVES_PGL target tests.
//
// Compiled together with task_support/driver.c.  This file defines neither
// main() nor an implementation macro.
//
// geometry   : coverage and nearest parameter against an independent
//              double-precision reference model (dense sampling plus
//              golden-section polish, with parameter-domain clipping).
// attributes : varying / depth / provoking-vertex values read from recorded
//              fragment-shader callbacks.
// submission : draw entry points, index/instance/MultiDraw plumbing,
//              scissor, blend, discard, depth, and context isolation.
#include "portablegl.h"
#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define QW 96
#define QH 72
#define QMAX_IV 16

// ---------------------------------------------------------------------------
// independent double-precision reference
// ---------------------------------------------------------------------------

typedef struct qp4 { double x, y, z, w; } qp4;
typedef struct qiv { double lo, hi; } qiv;

static void qbez(const qp4* c, double t, qp4* o)
{
	double u = 1.0 - t;
	o->x = u*u*c[0].x + 2.0*u*t*c[1].x + t*t*c[2].x;
	o->y = u*u*c[0].y + 2.0*u*t*c[1].y + t*t*c[2].y;
	o->z = u*u*c[0].z + 2.0*u*t*c[1].z + t*t*c[2].z;
	o->w = u*u*c[0].w + 2.0*u*t*c[1].w + t*t*c[2].w;
}

static double qpoly(const double* p, int n, double t)
{
	double v = 0.0;
	int i;
	for (i = n-1; i >= 0; --i)
		v = v*t + p[i];
	return v;
}

// Kept set of the quadratic p (p[0] + p[1] t + p[2] t^2 >= 0) on [0,1].
static int qref_halfspace(const double* p, qiv* out)
{
	double bounds[4];
	int nb = 0, i, n = 0;
	bounds[nb++] = 0.0;
	if (p[2] != 0.0) {
		double disc = p[1]*p[1] - 4.0*p[2]*p[0];
		if (disc >= 0.0) {
			double sq = sqrt(disc);
			double r0 = (-p[1] - sq) / (2.0*p[2]);
			double r1 = (-p[1] + sq) / (2.0*p[2]);
			if (r0 > r1) { double tmp = r0; r0 = r1; r1 = tmp; }
			if (r0 > 0.0 && r0 < 1.0) bounds[nb++] = r0;
			if (r1 > 0.0 && r1 < 1.0) bounds[nb++] = r1;
		}
	} else if (p[1] != 0.0) {
		double r0 = -p[0] / p[1];
		if (r0 > 0.0 && r0 < 1.0) bounds[nb++] = r0;
	}
	bounds[nb++] = 1.0;
	for (i = 0; i + 1 < nb; ++i) {
		double a = bounds[i], b = bounds[i+1];
		if (b - a <= 1e-14)
			continue;
		if (qpoly(p, 3, 0.5*(a+b)) >= 0.0) {
			if (n > 0 && a <= out[n-1].hi + 1e-14) {
				if (b > out[n-1].hi) out[n-1].hi = b;
			} else {
				out[n].lo = a; out[n].hi = b; ++n;
			}
		}
	}
	return n;
}

static int qref_clip(const qp4* c, qiv* out)
{
	double comp[3][3], wc[3];
	qiv cur[QMAX_IV], tmp[QMAX_IV], hs[4];
	int nc = 1, i, k, sign;

	cur[0].lo = 0.0;
	cur[0].hi = 1.0;
	for (k = 0; k < 3; ++k) {
		double v0 = (k == 0 ? c[0].x : k == 1 ? c[0].y : c[0].z);
		double v1 = (k == 0 ? c[1].x : k == 1 ? c[1].y : c[1].z);
		double v2 = (k == 0 ? c[2].x : k == 1 ? c[2].y : c[2].z);
		comp[k][0] = v0;
		comp[k][1] = 2.0*(v1-v0);
		comp[k][2] = v0 - 2.0*v1 + v2;
	}
	wc[0] = c[0].w;
	wc[1] = 2.0*(c[1].w - c[0].w);
	wc[2] = c[0].w - 2.0*c[1].w + c[2].w;

	for (k = 0; k < 3; ++k) {
		for (sign = 0; sign < 2; ++sign) {
			double p[3];
			int nh, a, b, nn = 0;
			for (i = 0; i < 3; ++i)
				p[i] = sign ? comp[k][i] + wc[i] : wc[i] - comp[k][i];
			if (p[0] == 0.0 && p[1] == 0.0 && p[2] == 0.0)
				continue;
			nh = qref_halfspace(p, hs);
			for (a = 0; a < nc; ++a) {
				for (b = 0; b < nh; ++b) {
					double lo = cur[a].lo > hs[b].lo ? cur[a].lo : hs[b].lo;
					double hi = cur[a].hi < hs[b].hi ? cur[a].hi : hs[b].hi;
					if (hi - lo > 1e-14 && nn < QMAX_IV) {
						tmp[nn].lo = lo; tmp[nn].hi = hi; ++nn;
					}
				}
			}
			nc = nn;
			for (i = 0; i < nc; ++i) cur[i] = tmp[i];
			if (!nc) return 0;
		}
	}
	for (i = 0; i < nc; ++i) out[i] = cur[i];
	return nc;
}

// Nearest point on one parameter interval: dense scan for a bracket, then
// golden-section polish of the squared distance.  Sufficient for test
// tolerances; it is independent of the implementation's quintic solver.
static double qref_interval(const qp4* c, double px, double py,
                            double ax, double bx, double ay, double by,
                            double lo, double hi, double* t_out)
{
	const int N = 3072;
	double best = 1e300, bt = lo, step;
	int i;

	for (i = 0; i <= N; ++i) {
		double t = lo + (hi-lo) * (double)i / (double)N;
		qp4 p;
		double X, Y, dx, dy, d;
		qbez(c, t, &p);
		if (!(p.w > 0.0)) continue;
		X = (ax*p.x + bx*p.w) / p.w;
		Y = (ay*p.y + by*p.w) / p.w;
		dx = X-px; dy = Y-py;
		d = dx*dx + dy*dy;
		if (d < best) { best = d; bt = t; }
	}
	step = (hi-lo)/(double)N;
	{
		double a = bt - step, b = bt + step;
		if (a < lo) a = lo;
		if (b > hi) b = hi;
		for (i = 0; i < 90; ++i) {
			double m1 = a + (b-a)/3.0, m2 = b - (b-a)/3.0;
			qp4 p1, p2;
			double X1, Y1, X2, Y2, d1, d2;
			qbez(c, m1, &p1); qbez(c, m2, &p2);
			X1 = (ax*p1.x + bx*p1.w)/p1.w; Y1 = (ay*p1.y + by*p1.w)/p1.w;
			X2 = (ax*p2.x + bx*p2.w)/p2.w; Y2 = (ay*p2.y + by*p2.w)/p2.w;
			d1 = (X1-px)*(X1-px) + (Y1-py)*(Y1-py);
			d2 = (X2-px)*(X2-px) + (Y2-py)*(Y2-py);
			if (d1 < d2) b = m2; else a = m1;
		}
		{
			double t = 0.5*(a+b);
			qp4 p;
			double X, Y, dx, dy, d;
			qbez(c, t, &p);
			X = (ax*p.x + bx*p.w)/p.w; Y = (ay*p.y + by*p.w)/p.w;
			dx = X-px; dy = Y-py;
			d = dx*dx + dy*dy;
			if (d < best) { best = d; bt = t; }
		}
	}
	*t_out = bt;
	return best;
}

static double qref_pixel(const qp4* c, const qiv* iv, int niv,
                         double px, double py,
                         double ax, double bx, double ay, double by,
                         double* t_out)
{
	double best = 1e300, bt = 0.0;
	int i;
	for (i = 0; i < niv; ++i) {
		double t;
		double d = qref_interval(c, px, py, ax, bx, ay, by, iv[i].lo, iv[i].hi, &t);
		if (d < best) { best = d; bt = t; }
	}
	*t_out = bt;
	return best;
}

// The NDC -> pixel affine map that make_viewport_m4 builds (including its
// 0.01 right/top bias), evaluated in double.
static void qmap(int vw, int vh, double* ax, double* bx, double* ay, double* by)
{
	double l = 0.0, b = 0.0;
	double r = l + (double)vw - 0.01;
	double t = b + (double)vh - 0.01;
	*ax = (r - l) * 0.5; *bx = (l + r) * 0.5;
	*ay = (t - b) * 0.5; *by = (b + t) * 0.5;
}

// ---------------------------------------------------------------------------
// shaders / helpers
// ---------------------------------------------------------------------------

static void qgeom_vs(float* vs_output, vec4* va, Shader_Builtins* b, void* u)
{
	(void)vs_output; (void)u;
	b->gl_Position = va[0];
}

static void qgeom_fs(float* fs_input, Shader_Builtins* b, void* u)
{
	(void)fs_input;
	b->gl_FragColor = *(vec4*)u;
}

static pix_t qpix(glContext* ctx, int x, int y)
{
	return ((pix_t*)ctx->back_buffer.lastrow)[-y * ctx->back_buffer.w + x];
}

static int qset(glContext* ctx, int x, int y)
{
	Color c = PIXEL_TO_COLOR(qpix(ctx, x, y));
	return c.r > 100 || c.g > 100 || c.b > 100;
}

typedef struct qbasic {
	glContext ctx;
	pix_t* pixels;
	GLuint prog, vbo;
	vec4 color;
	float buf[64];
} qbasic;

static int qbasic_init(qbasic* s, int w, int h)
{
	memset(s, 0, sizeof(*s));
	if (!init_glContext(&s->ctx, &s->pixels, w, h))
		return 0;
	set_glContext(&s->ctx);
	s->prog = pglCreateProgram(qgeom_vs, qgeom_fs, 0, NULL, GL_FALSE);
	glUseProgram(s->prog);
	s->color = make_v4(1, 1, 1, 1);
	pglSetUniform(&s->color);
	glGenBuffers(1, &s->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, NULL);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, w, h);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	return 1;
}

static void qbasic_free(qbasic* s)
{
	free_glContext(&s->ctx);
}

static void qdraw3(qbasic* s, const float* v, float width)
{
	(void)s;
	glBufferData(GL_ARRAY_BUFFER, 12 * (GLsizeiptr)sizeof(float), v, GL_STATIC_DRAW);
	glLineWidth(width);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
}

// Compare all pixels outside the coverage-boundary band against the
// reference.  Returns the number of mismatches (0 means pass).
static int qcompare(qbasic* s, int w, int h, const float* v, float width,
                    const char* name)
{
	qp4 c[3];
	qiv iv[QMAX_IV];
	double ax, bx, ay, by, hw2;
	int niv, x, y, mism = 0, checked = 0;

	c[0] = (qp4){(double)v[0],  (double)v[1],  (double)v[2],  (double)v[3]};
	c[1] = (qp4){(double)v[4],  (double)v[5],  (double)v[6],  (double)v[7]};
	c[2] = (qp4){(double)v[8],  (double)v[9],  (double)v[10], (double)v[11]};
	niv = qref_clip(c, iv);
	qmap(w, h, &ax, &bx, &ay, &by);
	hw2 = (double)width * (double)width * 0.25;

	for (y = 0; y < h; ++y) {
		for (x = 0; x < w; ++x) {
			double px = x + 0.5, py = y + 0.5, t;
			double d;
			int refcov, gotcov;
			if (niv) {
				d = qref_pixel(c, iv, niv, px, py, ax, bx, ay, by, &t);
				if (fabs(d - hw2) < 5e-3)
					continue;
				refcov = d <= hw2;
			} else {
				refcov = 0;
			}
			gotcov = qset(&s->ctx, x, y);
			++checked;
			if (refcov != gotcov) {
				if (mism < 4)
					fprintf(stderr, "  %s (%d,%d) ref=%d got=%d\n",
					        name, x, y, refcov, gotcov);
				++mism;
			}
		}
	}
	return checked ? mism : 1;
}

// ---------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------

static int g_arch(void)
{
	qbasic s;
	float v[12] = {
		-0.9f, 0.0f, 0.0f, 1.0f,
		-0.5f, 0.9f, 0.0f, 1.0f,
		 0.9f, 0.0f, 0.0f, 1.0f,
	};
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 1.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(qcompare(&s, QW, QH, v, 1.0f, "arch") == 0);
	qbasic_free(&s);
	return 0;
}

// w varies across the triple, so the projection is genuinely projective.
static int g_varying_w(void)
{
	qbasic s;
	float v[12] = {
		-0.8f, -0.6f, 0.0f, 0.75f,
		 0.0f, -0.6f, 0.0f, 2.00f,
		 0.8f, -0.6f, 0.0f, 0.75f,
	};
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 3.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(qcompare(&s, QW, QH, v, 3.0f, "varyw") == 0);
	qbasic_free(&s);
	return 0;
}

// Both endpoints outside the frustum, middle visible.
static int g_clip_ends(void)
{
	qbasic s;
	float v[12] = {
		-2.5f, -0.2f, 0.0f, 1.0f,
		 0.0f,  2.2f, 0.0f, 1.0f,
		 2.5f, -0.2f, 0.0f, 1.0f,
	};
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 2.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(qcompare(&s, QW, QH, v, 2.0f, "clipends") == 0);
	qbasic_free(&s);
	return 0;
}

// Leaves the frustum through x=w in the middle and returns: two disjoint
// visible parameter pieces.
static int g_two_pieces(void)
{
	qbasic s;
	float v[12] = {
		-0.9f, 0.5f, 0.0f, 1.0f,
		 3.0f, 0.5f, 0.0f, 1.0f,
		-0.9f, 0.5f, 0.0f, 1.0f,
	};
	{
		qp4 c[3];
		qiv iv[QMAX_IV];
		c[0] = (qp4){-0.9, 0.5, 0.0, 1.0};
		c[1] = (qp4){ 3.0, 0.5, 0.0, 1.0};
		c[2] = (qp4){-0.9, 0.5, 0.0, 1.0};
		TASK_CHECK(qref_clip(c, iv) >= 2);
	}
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 1.5f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(qcompare(&s, QW, QH, v, 1.5f, "twopieces") == 0);
	qbasic_free(&s);
	return 0;
}

// x(t) = 1 + 4(t-0.5)^2 with w = 1: the curve touches x = w exactly at
// t = 0.5 and is outside everywhere else.  The retained set is the single
// parameter 0.5, an isolated tangency, and nothing may be drawn.
static int g_interior_tangent(void)
{
	qbasic s;
	float v[12] = {
		2.0f, -0.2f, 0.0f, 1.0f,
		0.0f,  0.2f, 0.0f, 1.0f,
		2.0f, -0.2f, 0.0f, 1.0f,
	};
	{
		qp4 c[3];
		qiv iv[QMAX_IV];
		c[0] = (qp4){2.0, -0.2, 0.0, 1.0};
		c[1] = (qp4){0.0,  0.2, 0.0, 1.0};
		c[2] = (qp4){2.0, -0.2, 0.0, 1.0};
		// x(t) = 4t^2 - 4t + 2 = 1 + 4(t-0.5)^2, so the reference keeps
		// nothing.
		TASK_CHECK(qref_clip(c, iv) == 0);
	}
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 6.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	{
		int x, y, lit = 0;
		for (y = 0; y < QH; ++y)
			for (x = 0; x < QW; ++x)
				lit += qset(&s.ctx, x, y);
		TASK_CHECK(lit == 0);
	}
	qbasic_free(&s);
	return 0;
}

// Same polynomial but the curve is inside except for a tangency at x = w:
// the whole [0,1] must stay, and the double root must not split the range.
static int g_interior_touch(void)
{
	qbasic s;
	float v[12] = {
		0.0f, -0.7f, 0.0f, 1.0f,
		2.0f,  0.0f, 0.0f, 1.0f,
		0.0f,  0.7f, 0.0f, 1.0f,
	};
	{
		qp4 c[3];
		qiv iv[QMAX_IV];
		int n;
		c[0] = (qp4){0.0, -0.7, 0.0, 1.0};
		c[1] = (qp4){2.0,  0.0, 0.0, 1.0};
		c[2] = (qp4){0.0,  0.7, 0.0, 1.0};
		n = qref_clip(c, iv);
		TASK_CHECK(n == 1);
	}
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 1.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(qcompare(&s, QW, QH, v, 1.0f, "touch") == 0);
	qbasic_free(&s);
	return 0;
}

// Control points outside the near/far planes, middle inside: the retained
// pieces are the parts of t where |Cz| <= Cw.
static int g_clip_z(void)
{
	qbasic s;
	float v[12] = {
		-0.8f, 0.0f, -3.0f, 1.0f,
		 0.0f, 0.7f,  3.0f, 1.0f,
		 0.8f, 0.0f, -3.0f, 1.0f,
	};
	qp4 c[3];
	qiv iv[QMAX_IV];
	TASK_CHECK(qbasic_init(&s, QW, QH));
	c[0] = (qp4){-0.8, 0.0, -3.0, 1.0};
	c[1] = (qp4){ 0.0, 0.7,  3.0, 1.0};
	c[2] = (qp4){ 0.8, 0.0, -3.0, 1.0};
	TASK_CHECK(qref_clip(c, iv) >= 1);
	glLineWidth(2.0f);
	qdraw3(&s, v, 2.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	/* Depth test off, so the whole visible parameter range paints even
	 * though the depth values are clipped at the z planes. */
	TASK_CHECK(qcompare(&s, QW, QH, v, 2.0f, "clipz") == 0);
	qbasic_free(&s);
	return 0;
}

// Fold: the projection retraces itself through the middle control point.
// Coverage must still be judged on the curve, and one pixel still gets one
// callback.
static int g_fold(void)
{
	qbasic s;
	float v[12] = {
		-1.5f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.0f, 0.0f, 1.0f,
		 1.5f, 0.0f, 0.0f, 1.0f,
	};
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 4.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(qcompare(&s, QW, QH, v, 4.0f, "fold") == 0);
	qbasic_free(&s);
	return 0;
}

// Constant point: all three control points coincide, leaving a round dot of
// diameter glLineWidth.
static int g_const_point(void)
{
	qbasic s;
	float v[12] = {
		0.25f, -0.1f, 0.0f, 1.0f,
		0.25f, -0.1f, 0.0f, 1.0f,
		0.25f, -0.1f, 0.0f, 1.0f,
	};
	TASK_CHECK(qbasic_init(&s, QW, QH));
	qdraw3(&s, v, 6.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	TASK_CHECK(qcompare(&s, QW, QH, v, 6.0f, "point") == 0);
	qbasic_free(&s);
	return 0;
}

// ---------------------------------------------------------------------------
// attributes
// ---------------------------------------------------------------------------

typedef struct qpxrec {
	int set;
	float v[4];
	float depth;
	float w;
} qpxrec;

static qpxrec g_px[QW * QH];
static int g_calls;

static void qrec_vs(float* vs_output, vec4* va, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = va[0];
	vs_output[0] = va[1].x;
	vs_output[1] = va[1].y;
	vs_output[2] = va[1].z;
	vs_output[3] = va[1].w;
}

static void qrec_fs(float* fs_input, Shader_Builtins* b, void* u)
{
	int ix, iy;
	(void)u;
	++g_calls;
	ix = (int)b->gl_FragCoord.x;
	iy = (int)b->gl_FragCoord.y;
	if (ix >= 0 && ix < QW && iy >= 0 && iy < QH) {
		qpxrec* r = &g_px[iy*QW + ix];
		r->set = 1;
		r->v[0] = fs_input[0];
		r->v[1] = fs_input[1];
		r->v[2] = fs_input[2];
		r->v[3] = fs_input[3];
		r->depth = b->gl_FragCoord.z;
		r->w = b->gl_FragCoord.w;
	}
	b->gl_FragColor = make_v4(1, 1, 1, 1);
}

static int qrec_init(qbasic* s, const GLenum* interp, int n)
{
	memset(s, 0, sizeof(*s));
	if (!init_glContext(&s->ctx, &s->pixels, QW, QH))
		return 0;
	set_glContext(&s->ctx);
	s->prog = pglCreateProgram(qrec_vs, qrec_fs, n, (GLenum*)interp, GL_FALSE);
	glUseProgram(s->prog);
	glGenBuffers(1, &s->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, s->vbo);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float),
	                      (void*)(4*sizeof(float)));
	glEnableVertexAttribArray(1);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	return 1;
}

// Verifies smooth, noperspective, flat and depth against the curve parameter
// selected by the independent reference.  Also requires exactly one callback
// per covered pixel.
static int a_values(void)
{
	qbasic s;
	static const GLenum interp[4] = {
		PGL_SMOOTH, PGL_NOPERSPECTIVE, PGL_SMOOTH, PGL_FLAT
	};
	float v[24] = {
		// position                    varying (4)
		-0.7f, -0.5f, 0.25f, 1.0f,   1.0f, 3.0f, 0.0f, 1.0f,
		 0.0f,  0.8f, 0.60f, 2.0f,   2.0f, 0.0f, 4.0f, 1.0f,
		 0.7f, -0.5f, 0.25f, 1.0f,   5.0f, 3.0f, 8.0f, 1.0f,
	};
	qp4 c[3];
	qiv iv[QMAX_IV];
	double ax, bx, ay, by;
	int niv, x, y, checked = 0, bad = 0;

	c[0] = (qp4){(double)v[0], (double)v[1], (double)v[2], (double)v[3]};
	c[1] = (qp4){(double)v[8], (double)v[9], (double)v[10], (double)v[11]};
	c[2] = (qp4){(double)v[16], (double)v[17], (double)v[18], (double)v[19]};
	niv = qref_clip(c, iv);
	TASK_CHECK(niv >= 1);
	qmap(QW, QH, &ax, &bx, &ay, &by);

	memset(g_px, 0, sizeof(g_px));
	g_calls = 0;
	TASK_CHECK(qrec_init(&s, interp, 4));
	// Default provoking vertex is GL_LAST_VERTEX_CONVENTION: varying 0 of
	// control point 2.
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glLineWidth(6.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(glGetError() == GL_NO_ERROR);

	for (y = 0; y < QH; ++y) {
		for (x = 0; x < QW; ++x) {
			qpxrec* r = &g_px[y*QW + x];
			double px = x + 0.5, py = y + 0.5, t, d;
			double b0, b1, b2, W, es, en, ez;

			if (!niv) {
				if (r->set) ++bad;
				continue;
			}
			d = qref_pixel(c, iv, niv, px, py, ax, bx, ay, by, &t);
			if (fabs(d - 9.0) < 5e-3)
				continue; /* half width 3 */
			if (d > 9.0) {
				if (r->set) ++bad;
				continue;
			}
			++checked;
			if (!r->set) { ++bad; continue; }

			b0 = (1.0-t)*(1.0-t);
			b1 = 2.0*t*(1.0-t);
			b2 = t*t;
			W = b0*c[0].w + b1*c[1].w + b2*c[2].w;
			// slot 0 smooth: 1,2,5
			es = b0*1.0 + b1*2.0 + b2*5.0;
			if (fabs(r->v[0] - es) > 2e-3) ++bad;
			// slot 1 noperspective: (sum Bi*wi*fi)/Cw, f = 3,0,3
			en = (b0*1.0*3.0 + b1*2.0*0.0 + b2*1.0*3.0) / W;
			if (fabs(r->v[1] - en) > 2e-3) ++bad;
			// slot 2 smooth: 0,4,8
			ez = b0*0.0 + b1*4.0 + b2*8.0;
			if (fabs(r->v[2] - ez) > 2e-3) ++bad;
			// slot 3 flat: provoking (last) vertex's fourth varying (1.0)
			if (fabs(r->v[3] - v[23]) > 1e-4) ++bad;
			// depth: Cz/Cw mapped from [-1,1] to the default [0,1]
			{
				double Z = b0*c[0].z + b1*c[1].z + b2*c[2].z;
				double dep = 0.5*(Z/W) + 0.5;
				if (fabs(r->depth - dep) > 2e-3) ++bad;
			}
			// gl_FragCoord.w = 1/Cw
			if (fabs(r->w - 1.0/W) > 2e-3) ++bad;
		}
	}
	TASK_CHECK(checked > 20);
	TASK_CHECK(bad == 0);
	// exactly one callback per covered pixel (no duplicates, no misses)
	TASK_CHECK(g_calls == checked);
	qbasic_free(&s);
	return 0;
}

// Smooth varying 0 carries the parameter itself.  Control points tag 0, 0.5
// and 1, so the Bernstein blend is 2t(1-t)*0.5 + t^2 = t and the recorded
// value recovers the implementation's selected parameter.
static int a_parameter(void)
{
	qbasic s;
	static const GLenum interp[4] = {
		PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH
	};
	float v[24] = {
		// position                     tag: f1 = 0.5, f2 = 1 -> result = t
		-0.8f, -0.4f, 0.30f, 1.00f,   0.00f, 0.0f, 0.0f, 1.0f,
		 0.0f,  0.9f, 0.50f, 1.50f,   0.50f, 0.0f, 0.0f, 1.0f,
		 0.8f, -0.4f, 0.30f, 1.00f,   1.00f, 0.0f, 0.0f, 1.0f,
	};
	qp4 c[3];
	qiv iv[QMAX_IV];
	double ax, bx, ay, by;
	int niv, x, y, checked = 0, bad = 0;

	c[0] = (qp4){(double)v[0], (double)v[1], (double)v[2], (double)v[3]};
	c[1] = (qp4){(double)v[8], (double)v[9], (double)v[10], (double)v[11]};
	c[2] = (qp4){(double)v[16], (double)v[17], (double)v[18], (double)v[19]};
	niv = qref_clip(c, iv);
	TASK_CHECK(niv >= 1);
	qmap(QW, QH, &ax, &bx, &ay, &by);

	memset(g_px, 0, sizeof(g_px));
	g_calls = 0;
	TASK_CHECK(qrec_init(&s, interp, 4));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glLineWidth(5.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(glGetError() == GL_NO_ERROR);

	for (y = 0; y < QH; ++y) {
		for (x = 0; x < QW; ++x) {
			qpxrec* r = &g_px[y*QW + x];
			double px = x + 0.5, py = y + 0.5, t, d;
			if (!r->set)
				continue;
			d = qref_pixel(c, iv, niv, px, py, ax, bx, ay, by, &t);
			/* Only pixels the reference places comfortably inside the
			 * stroke are compared; the contract excludes the boundary band. */
			if (d > 6.25 - 2e-3) {
				++bad;
				continue;
			}
			if (fabs(r->v[0] - t) > 1e-6) {
				if (bad < 5)
					fprintf(stderr, "  t mismatch (%d,%d) ref=%.9f got=%.9f\n",
					        x, y, t, r->v[0]);
				++bad;
			}
			++checked;
		}
	}
	TASK_CHECK(checked > 20);
	/* 1e-6 absolute parameter tolerance as the contract demands. */
	TASK_CHECK(bad == 0);
	qbasic_free(&s);
	return 0;
}

typedef struct qflatcnt { int lo, hi; int seen_lo, seen_hi; } qflatcnt;
static qflatcnt g_flat;

static void qflat_vs(float* vs_output, vec4* va, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = va[0];
	vs_output[0] = va[1].x;
}

static void qflat_fs(float* fs_input, Shader_Builtins* b, void* u)
{
	(void)u;
	if (fabs(fs_input[0] - 7.0f) < 1e-3) g_flat.seen_lo = 1;
	if (fabs(fs_input[0] - 11.0f) < 1e-3) g_flat.seen_hi = 1;
	b->gl_FragColor = make_v4(1, 1, 1, 1);
}

// A clipped-away control point still supplies flat values: provoking is a
// property of the submitted triple, not of what survived clipping.
static int a_flat_provoke(void)
{
	glContext ctx;
	pix_t* pixels = NULL;
	static const GLenum interp[1] = { PGL_FLAT };
	float v[24] = {
		-3.0f, 0.0f, 0.0f, 1.0f,   7.0f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.8f, 0.0f, 1.0f,   9.0f, 0.0f, 0.0f, 1.0f,
		 3.0f, 0.0f, 0.0f, 1.0f,  11.0f, 0.0f, 0.0f, 1.0f,
	};
	GLuint prog, vbo;
	int x, y, lit;

	if (!init_glContext(&ctx, &pixels, QW, QH)) return 1;
	set_glContext(&ctx);
	prog = pglCreateProgram(qflat_vs, qflat_fs, 1, (GLenum*)interp, GL_FALSE);
	glUseProgram(prog);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float),
	                      (void*)(4*sizeof(float)));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glLineWidth(2.0f);

	// FIRST: the first control point is off-screen but must still provide the
	// flat value.
	g_flat.seen_lo = g_flat.seen_hi = 0;
	glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(g_flat.seen_lo);
	TASK_CHECK(!g_flat.seen_hi);
	lit = 0;
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x)
			lit += qset(&ctx, x, y);
	TASK_CHECK(lit > 0);

	// LAST: the third control point, also off-screen near the rim.
	g_flat.seen_lo = g_flat.seen_hi = 0;
	glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(g_flat.seen_hi);
	TASK_CHECK(!g_flat.seen_lo);

	free_glContext(&ctx);
	return 0;
}

// Overlapping retained intervals / folds: one callback per pixel.
static int a_single_call(void)
{
	qbasic s;
	static const GLenum interp[4] = {
		PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH
	};
	float v[24] = {
		-1.5f, 0.2f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
		 0.0f, -0.9f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
		 1.5f, 0.2f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
	};
	int x, y, lit = 0;
	memset(g_px, 0, sizeof(g_px));
	g_calls = 0;
	TASK_CHECK(qrec_init(&s, interp, 4));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glLineWidth(4.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x)
			lit += qset(&s.ctx, x, y);
	TASK_CHECK(lit > 0);
	TASK_CHECK(g_calls == lit);
	qbasic_free(&s);
	return 0;
}

// Every covered pixel must get exactly one callback: a second draw of the
// same pixel is a second fragment only if it comes from a different curve.
static int a_no_duplicates(void)
{
	qbasic s;
	static const GLenum interp[4] = {
		PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH
	};
	float v[24] = {
		-0.9f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.9f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
		 0.9f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
	};
	memset(g_px, 0, sizeof(g_px));
	g_calls = 0;
	TASK_CHECK(qrec_init(&s, interp, 4));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glLineWidth(8.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(g_calls > 0);
	{
		int x, y, lit = 0;
		for (y = 0; y < QH; ++y)
			for (x = 0; x < QW; ++x)
				if (g_px[y*QW + x].set) ++lit;
		TASK_CHECK(g_calls == lit);
	}
	qbasic_free(&s);
	return 0;
}

// ---------------------------------------------------------------------------
// submission
// ---------------------------------------------------------------------------

// Indexed draw uses element order for the control triple.
static int s_indexed(void)
{
	qbasic s;
	GLuint ibo;
	unsigned indices[3] = { 2, 0, 1 };
	float v[12] = {
		-0.8f, -0.4f, 0.0f, 1.0f,
		 0.0f,  0.8f, 0.0f, 1.0f,
		 0.8f, -0.4f, 0.0f, 1.0f,
	};
	float ordered[12];
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glGenBuffers(1, &ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
	glLineWidth(1.0f);
	glDrawElements(GL_QUADRATIC_CURVES_PGL, 3, GL_UNSIGNED_INT, NULL);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	ordered[0] = v[8]; ordered[1] = v[9]; ordered[2] = v[10]; ordered[3] = v[11];
	ordered[4] = v[0]; ordered[5] = v[1]; ordered[6] = v[2]; ordered[7] = v[3];
	ordered[8] = v[4]; ordered[9] = v[5]; ordered[10] = v[6]; ordered[11] = v[7];
	TASK_CHECK(qcompare(&s, QW, QH, ordered, 1.0f, "indexed") == 0);
	qbasic_free(&s);
	return 0;
}

// Trailing vertices that do not complete a triple are ignored.
static int s_trailing(void)
{
	qbasic s;
	float v[32] = {
		-0.9f, 0.4f, 0.0f, 1.0f,  -0.4f, 0.8f, 0.0f, 1.0f,  0.1f, 0.4f, 0.0f, 1.0f,
		-0.9f, -0.6f, 0.0f, 1.0f, -0.4f, -0.2f, 0.0f, 1.0f, 0.1f, -0.6f, 0.0f, 1.0f,
		0.5f, 0.5f, 0.0f, 1.0f,   0.7f, 0.7f, 0.0f, 1.0f,
	};
	int x, y, upper, lower;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glLineWidth(1.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 8);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	upper = lower = 0;
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			if (!qset(&s.ctx, x, y)) continue;
			if (y > QH/2) ++upper; else ++lower;
		}
	TASK_CHECK(upper > 5);
	TASK_CHECK(lower > 5);
	qbasic_free(&s);
	return 0;
}

// MultiDrawArrays with two ranges: one complete curve in each.
static int s_multidraw(void)
{
	qbasic s;
	float v[24] = {
		-0.9f, 0.4f, 0.0f, 1.0f,  -0.5f, 0.9f, 0.0f, 1.0f,  0.0f, 0.4f, 0.0f, 1.0f,
		-0.9f, -0.5f, 0.0f, 1.0f, -0.5f, 0.0f, 0.0f, 1.0f,  0.0f, -0.5f, 0.0f, 1.0f,
	};
	GLint first[2] = { 0, 3 };
	GLsizei count[2] = { 3, 3 };
	int x, y, upper = 0, lower = 0;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glLineWidth(1.0f);
	glMultiDrawArrays(GL_QUADRATIC_CURVES_PGL, first, count, 2);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			if (!qset(&s.ctx, x, y)) continue;
			if (y > QH/2) ++upper; else ++lower;
		}
	TASK_CHECK(upper > 5);
	TASK_CHECK(lower > 5);
	qbasic_free(&s);
	return 0;
}

// MultiDrawElements with two index ranges.
static int s_multidraw_elements(void)
{
	qbasic s;
	GLuint ibo;
	unsigned idx[6] = { 0, 1, 2, 3, 4, 5 };
	float v[24] = {
		-0.9f, 0.4f, 0.0f, 1.0f,  -0.5f, 0.9f, 0.0f, 1.0f,  0.0f, 0.4f, 0.0f, 1.0f,
		-0.9f, -0.5f, 0.0f, 1.0f, -0.5f, 0.0f, 0.0f, 1.0f,  0.0f, -0.5f, 0.0f, 1.0f,
	};
	GLsizei counts[2] = { 3, 3 };
	const GLvoid* ptrs[2] = { (const GLvoid*)(size_t)0, (const GLvoid*)(3*sizeof(unsigned)) };
	int x, y, upper = 0, lower = 0;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glGenBuffers(1, &ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
	glLineWidth(1.0f);
	glMultiDrawElements(GL_QUADRATIC_CURVES_PGL, counts, GL_UNSIGNED_INT, ptrs, 2);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			if (!qset(&s.ctx, x, y)) continue;
			if (y > QH/2) ++upper; else ++lower;
		}
	TASK_CHECK(upper > 5);
	TASK_CHECK(lower > 5);
	qbasic_free(&s);
	return 0;
}

// Vertex callback count follows the existing rule: one call per submitted
// vertex, even for a trailing incomplete triple.
static int g_vs_calls;
static void qcount_vs(float* vs_output, vec4* va, Shader_Builtins* b, void* u)
{
	(void)vs_output; (void)u;
	++g_vs_calls;
	b->gl_Position = va[0];
}

static int s_vertex_calls(void)
{
	glContext ctx;
	pix_t* pixels = NULL;
	static const GLenum interp[1] = { PGL_SMOOTH };
	float v[32];
	GLuint prog, vbo;
	vec4 color = {1, 1, 1, 1};
	int i;
	for (i = 0; i < 8; ++i) {
		v[i*4+0] = -0.5f + 0.1f*(float)i;
		v[i*4+1] = 0.0f;
		v[i*4+2] = 0.0f;
		v[i*4+3] = 1.0f;
	}
	if (!init_glContext(&ctx, &pixels, QW, QH)) return 1;
	set_glContext(&ctx);
	prog = pglCreateProgram(qcount_vs, qgeom_fs, 1, (GLenum*)interp, GL_FALSE);
	glUseProgram(prog);
	pglSetUniform(&color);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);

	g_vs_calls = 0;
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 8);
	TASK_CHECK(g_vs_calls == 8);

	g_vs_calls = 0;
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 2);
	TASK_CHECK(g_vs_calls == 2);
	TASK_CHECK(glGetError() == GL_NO_ERROR);

	free_glContext(&ctx);
	return 0;
}

// Instanced draw: two copies, shifted by an instanced attribute.
static void qinst_vs(float* vs_output, vec4* va, Shader_Builtins* b, void* u)
{
	(void)u;
	b->gl_Position = va[0];
	b->gl_Position.y += va[1].x;
	vs_output[0] = (float)b->gl_InstanceID;
}

static void qinst_fs(float* fs_input, Shader_Builtins* b, void* u)
{
	(void)fs_input; (void)u;
	b->gl_FragColor = make_v4(1, 1, 1, 1);
}

static int s_instanced(void)
{
	glContext ctx;
	pix_t* pixels = NULL;
	static const GLenum interp[1] = { PGL_SMOOTH };
	float v[12] = {
		-0.5f, -0.5f, 0.0f, 1.0f,  -0.3f, 0.2f, 0.0f, 1.0f,  -0.1f, -0.5f, 0.0f, 1.0f,
	};
	float offsets[8] = { 0.0f, 0.0f, 0.0f, 0.0f,  0.7f, 0.0f, 0.0f, 0.0f };
	GLuint prog, vbo, abo;
	int x, y, upper = 0, lower = 0;

	if (!init_glContext(&ctx, &pixels, QW, QH)) return 1;
	set_glContext(&ctx);
	prog = pglCreateProgram(qinst_vs, qinst_fs, 1, (GLenum*)interp, GL_FALSE);
	glUseProgram(prog);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(0);
	glGenBuffers(1, &abo);
	glBindBuffer(GL_ARRAY_BUFFER, abo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(offsets), offsets, GL_STATIC_DRAW);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribDivisor(1, 1);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glLineWidth(1.0f);
	glDrawArraysInstanced(GL_QUADRATIC_CURVES_PGL, 0, 3, 2);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			if (!qset(&ctx, x, y)) continue;
			if (y > QH/2) ++upper; else ++lower;
		}
	TASK_CHECK(upper > 3);
	TASK_CHECK(lower > 3);
	free_glContext(&ctx);
	return 0;
}

// Scissor clips actual pixels; the stroke may extend past the center curve's
// clip edge but never past the framebuffer/scissor rectangle.
static int s_scissor(void)
{
	qbasic s;
	float v[12] = {
		-0.9f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.9f, 0.0f, 1.0f,
		 0.9f, 0.0f, 0.0f, 1.0f,
	};
	int x, y, inside = 0, outside = 0;
	int sx = QW/4, sy = QH/3, sw = QW/2, sh = QH/3;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glLineWidth(3.0f);
	glEnable(GL_SCISSOR_TEST);
	glScissor(sx, sy, sw, sh);
	glClear(GL_COLOR_BUFFER_BIT);
	qdraw3(&s, v, 3.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			int in_box = (x >= sx && x < sx+sw && y >= sy && y < sy+sh);
			int lit = qset(&s.ctx, x, y);
			if (in_box && lit) ++inside;
			if (!in_box && lit) ++outside;
		}
	TASK_CHECK(inside > 3);
	TASK_CHECK(outside == 0);
	qbasic_free(&s);
	return 0;
}

// Blending still applies per fragment, and a second curve blends over the
// first rather than replacing or double-blending within one curve.
static int s_blend(void)
{
	qbasic s;
	float v[12] = {
		-0.9f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.9f, 0.0f, 1.0f,
		 0.9f, 0.0f, 0.0f, 1.0f,
	};
	int one_r = 0, two_r = 0, x, y;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glLineWidth(8.0f);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClear(GL_COLOR_BUFFER_BIT);
	s.color = make_v4(1.0f, 0.0f, 0.0f, 0.5f);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	for (y = 0; y < QH && !one_r; ++y)
		for (x = 0; x < QW && !one_r; ++x)
			if (qset(&s.ctx, x, y))
				one_r = PIXEL_TO_COLOR(qpix(&s.ctx, x, y)).r;
	// second curve, disjoint from the first, blends over the same rule
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	for (y = 0; y < QH && !two_r; ++y)
		for (x = 0; x < QW && !two_r; ++x)
			if (qset(&s.ctx, x, y))
				two_r = PIXEL_TO_COLOR(qpix(&s.ctx, x, y)).r;
	TASK_CHECK(one_r > 100);      // 0.5 over black
	TASK_CHECK(two_r > one_r);    // 0.75 over 0.5
	qbasic_free(&s);
	return 0;
}

// Color mask is applied by the existing fragment write path.
static int s_colormask(void)
{
	qbasic s;
	float v[12] = {
		-0.9f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.9f, 0.0f, 1.0f,
		 0.9f, 0.0f, 0.0f, 1.0f,
	};
	int x, y, lit = 0;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glLineWidth(6.0f);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	qdraw3(&s, v, 6.0f);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x)
			lit += qset(&s.ctx, x, y);
	TASK_CHECK(lit == 0);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	qdraw3(&s, v, 6.0f);
	lit = 0;
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x)
			lit += qset(&s.ctx, x, y);
	TASK_CHECK(lit > 0);
	qbasic_free(&s);
	return 0;
}

// Discard leaves the cleared background untouched.
static void qdiscard_fs(float* fs_input, Shader_Builtins* b, void* u)
{
	(void)fs_input; (void)u;
	b->discard = GL_TRUE;
	b->gl_FragColor = make_v4(1, 0, 1, 1);
}

static int s_discard(void)
{
	glContext ctx;
	pix_t* pixels = NULL;
	float v[12] = {
		-0.9f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.9f, 0.0f, 1.0f,
		 0.9f, 0.0f, 0.0f, 1.0f,
	};
	GLuint prog, vbo;
	int x, y, lit = 0, bg_ok = 1;
	if (!init_glContext(&ctx, &pixels, QW, QH)) return 1;
	set_glContext(&ctx);
	prog = pglCreateProgram(qgeom_vs, qdiscard_fs, 0, NULL, GL_FALSE);
	glUseProgram(prog);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glLineWidth(6.0f);
	glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_DEPTH_TEST);
	glClear(GL_DEPTH_BUFFER_BIT);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			Color c = PIXEL_TO_COLOR(qpix(&ctx, x, y));
			if (qset(&ctx, x, y)) ++lit;
			// Background is (0.1, 0.2, 0.3); any brighter channel means a
			// fragment escaped discard.
			if (c.r > 40 || c.g > 60 || c.b > 80) bg_ok = 0;
		}
	TASK_CHECK(lit == 0);
	TASK_CHECK(bg_ok);
	free_glContext(&ctx);
	return 0;
}

// Depth test on: the near stroke wins where the two curves overlap.
static int s_depth(void)
{
	qbasic s;
	float far_v[12] = {
		-0.9f, 0.0f, 0.6f, 1.0f,  0.0f, 0.9f, 0.6f, 1.0f,  0.9f, 0.0f, 0.6f, 1.0f,
	};
	float near_v[12] = {
		-0.9f, 0.0f, -0.6f, 1.0f, 0.0f, 0.9f, -0.6f, 1.0f, 0.9f, 0.0f, -0.6f, 1.0f,
	};
	int x, y, green = 0, red = 0;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glLineWidth(5.0f);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	s.color = make_v4(1, 0, 0, 1);
	glBufferData(GL_ARRAY_BUFFER, sizeof(far_v), far_v, GL_STATIC_DRAW);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	s.color = make_v4(0, 1, 0, 1);
	glBufferData(GL_ARRAY_BUFFER, sizeof(near_v), near_v, GL_STATIC_DRAW);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			Color c = PIXEL_TO_COLOR(qpix(&s.ctx, x, y));
			if (c.g > 100 && c.r < 100) ++green;
			if (c.r > 100 && c.g < 100) ++red;
		}
	TASK_CHECK(green > 10);
	TASK_CHECK(red == 0);
	qbasic_free(&s);
	return 0;
}

// A fragment shader that writes gl_FragDepth must control the depth test.
static void qfd_vs(float* vs_output, vec4* va, Shader_Builtins* b, void* u)
{
	(void)vs_output; (void)u;
	b->gl_Position = va[0];
}

static void qfd_fs(float* fs_input, Shader_Builtins* b, void* u)
{
	(void)fs_input; (void)u;
	b->gl_FragColor = make_v4(1, 1, 0, 1);
	b->gl_FragDepth = 1.0f; /* push this stroke to the far plane */
}

static int s_shader_depth(void)
{
	glContext ctx;
	pix_t* pixels = NULL;
	float v[12] = {
		-0.9f, 0.0f, 0.0f, 1.0f,
		 0.0f, 0.9f, 0.0f, 1.0f,
		 0.9f, 0.0f, 0.0f, 1.0f,
	};
	GLuint prog, vbo;
	int x, y, red = 0, green = 0;
	if (!init_glContext(&ctx, &pixels, QW, QH)) return 1;
	set_glContext(&ctx);
	prog = pglCreateProgram(qfd_vs, qfd_fs, 0, NULL, GL_TRUE);
	glUseProgram(prog);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glLineWidth(5.0f);
	// Shader pushes depth to 1.0.
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	// Then a regular near stroke with the standard shader.
	{
		qbasic s;
		/* reuse context, just switch program */
		s.ctx = ctx;
		(void)s;
	}
	prog = pglCreateProgram(qgeom_vs, qgeom_fs, 0, NULL, GL_FALSE);
	glUseProgram(prog);
	{
		vec4 green_col = make_v4(0, 1, 0, 1);
		pglSetUniform(&green_col);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
		glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);
	}
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			Color c = PIXEL_TO_COLOR(qpix(&ctx, x, y));
			if (c.r > 100 && c.g > 100) ++red;   /* yellow from the far stroke */
			if (c.g > 100 && c.r < 100) ++green;
		}
	/* The near green stroke is in front of the shader-pushed yellow. */
	TASK_CHECK(green > 10);
	TASK_CHECK(red == 0);
	free_glContext(&ctx);
	return 0;
}

// Two curves submitted together: both callbacks run on pixels they share;
// depth is not used to deduplicate the two primitives.
static int s_two_curves(void)
{
	qbasic s;
	static const GLenum interp[4] = {
		PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH, PGL_SMOOTH
	};
	float v[48] = {
		-0.8f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
		-0.3f, 0.6f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
		 0.2f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
		-0.2f, 0.2f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
		 0.3f, 0.8f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
		 0.8f, 0.2f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
	};
	int i, lit = 0;
	memset(g_px, 0, sizeof(g_px));
	g_calls = 0;
	TASK_CHECK(qrec_init(&s, interp, 4));
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
	glLineWidth(3.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 6);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	for (i = 0; i < QW*QH; ++i)
		if (g_px[i].set) ++lit;
	TASK_CHECK(lit > 10);
	// Both curves overlap in the middle, so more callbacks than lit pixels.
	TASK_CHECK(g_calls > lit);
	qbasic_free(&s);
	return 0;
}

// Contexts do not share curve scratch or framebuffers.
static int s_two_contexts(void)
{
	glContext a, b;
	pix_t* pa = NULL;
	pix_t* pb = NULL;
	float va[12] = {
		-0.8f, -0.5f, 0.0f, 1.0f,  -0.2f, 0.7f, 0.0f, 1.0f,  0.4f, -0.5f, 0.0f, 1.0f,
	};
	float vb[12] = {
		-0.8f, 0.6f, 0.0f, 1.0f,   0.0f, -0.7f, 0.0f, 1.0f,  0.8f, -0.6f, 0.0f, 1.0f,
	};
	GLuint prog, vbo;
	vec4 color = {1, 1, 1, 1};
	int x, y, seta = 0, setb = 0, ub = 0;

	if (!init_glContext(&a, &pa, QW, QH)) return 1;
	if (!init_glContext(&b, &pb, QW, QH)) return 1;

	set_glContext(&a);
	prog = pglCreateProgram(qgeom_vs, qgeom_fs, 0, NULL, GL_FALSE);
	glUseProgram(prog);
	pglSetUniform(&color);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(va), va, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glLineWidth(2.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);

	set_glContext(&b);
	prog = pglCreateProgram(qgeom_vs, qgeom_fs, 0, NULL, GL_FALSE);
	glUseProgram(prog);
	pglSetUniform(&color);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vb), vb, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, QW, QH);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glLineWidth(2.0f);
	glDrawArrays(GL_QUADRATIC_CURVES_PGL, 0, 3);

	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x) {
			if (qset(&a, x, y)) ++seta;
			if (qset(&b, x, y)) {
				++setb;
				if (y > QH/2) ++ub;
			}
		}
	TASK_CHECK(seta > 10);
	TASK_CHECK(setb > 10);
	TASK_CHECK(ub > 5);
	// a's curve peaks in the upper area; b's starts upper but sweeps low.
	{
		int ua = 0;
		for (y = QH/2; y < QH; ++y)
			for (x = 0; x < QW; ++x)
				if (qset(&a, x, y)) ++ua;
		TASK_CHECK(ua > 3);
	}

	free_glContext(&a);
	free_glContext(&b);
	return 0;
}

// A mode other than the curve one still behaves as before; ordinary points,
// lines and triangles keep their paths.
static int s_other_modes(void)
{
	qbasic s;
	float line[8] = { -0.8f, 0.0f, 0.0f, 1.0f, 0.8f, 0.0f, 0.0f, 1.0f };
	float tri[12] = { -0.5f, -0.5f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 1.0f,
	                  0.0f, 0.5f, 0.0f, 1.0f };
	int x, y, lit;
	TASK_CHECK(qbasic_init(&s, QW, QH));
	glBufferData(GL_ARRAY_BUFFER, sizeof(line), line, GL_STATIC_DRAW);
	glDrawArrays(GL_LINES, 0, 2);
	lit = 0;
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x)
			lit += qset(&s.ctx, x, y);
	TASK_CHECK(lit > 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	lit = 0;
	for (y = 0; y < QH; ++y)
		for (x = 0; x < QW; ++x)
			lit += qset(&s.ctx, x, y);
	TASK_CHECK(lit > 100);
	TASK_CHECK(glGetError() == GL_NO_ERROR);
	qbasic_free(&s);
	return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
	{ "geometry",   "arch",             g_arch },
	{ "geometry",   "varying_w",        g_varying_w },
	{ "geometry",   "clip_ends",        g_clip_ends },
	{ "geometry",   "two_pieces",       g_two_pieces },
	{ "geometry",   "interior_tangent", g_interior_tangent },
	{ "geometry",   "interior_touch",   g_interior_touch },
	{ "geometry",   "clip_z",           g_clip_z },
	{ "geometry",   "fold",             g_fold },
	{ "geometry",   "const_point",      g_const_point },

	{ "attributes", "values",           a_values },
	{ "attributes", "parameter",        a_parameter },
	{ "attributes", "flat_provoke",     a_flat_provoke },
	{ "attributes", "single_call",      a_single_call },
	{ "attributes", "no_duplicates",    a_no_duplicates },

	{ "submission", "indexed",          s_indexed },
	{ "submission", "trailing",         s_trailing },
	{ "submission", "multidraw",        s_multidraw },
	{ "submission", "multidraw_elems",  s_multidraw_elements },
	{ "submission", "vertex_calls",     s_vertex_calls },
	{ "submission", "instanced",        s_instanced },
	{ "submission", "scissor",          s_scissor },
	{ "submission", "blend",            s_blend },
	{ "submission", "colormask",        s_colormask },
	{ "submission", "discard",          s_discard },
	{ "submission", "depth",            s_depth },
	{ "submission", "shader_depth",     s_shader_depth },
	{ "submission", "two_curves",       s_two_curves },
	{ "submission", "two_contexts",     s_two_contexts },
	{ "submission", "other_modes",      s_other_modes },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
