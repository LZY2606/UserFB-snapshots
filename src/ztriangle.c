#include "../include/zbuffer.h"
#include "msghandling.h"
#include <stdlib.h>




/* TODO: Switch from scanline rasterizer to easily parallelized cross product rasterizer.*/
static GLfloat edgeFunction(GLfloat ax, GLfloat ay, GLfloat bx, GLfloat by, GLfloat cx, GLfloat cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

#if TGL_FEATURE_RENDER_BITS == 32
#elif TGL_FEATURE_RENDER_BITS == 16
#else
#error "WRONG MODE!!!"
#endif

#if TGL_FEATURE_POLYGON_STIPPLE == 1

#define TGL_STIPPLEVARS                                                                                                                                        \
	GLubyte* zbstipplepattern = zb->stipplepattern;                                                                                                            \
	GLubyte zbdostipple = zb->dostipple;
#define THE_X ((GLint)(pp - pp1))
#define XSTIP(_a) ((THE_X + _a) & TGL_POLYGON_STIPPLE_MASK_X)
#define YSTIP (the_y & TGL_POLYGON_STIPPLE_MASK_Y)
/* NOTES                                                           Divide by 8 to get the byte        Get the actual bit*/
#define STIPBIT(_a) (zbstipplepattern[(XSTIP(_a) | (YSTIP << TGL_POLYGON_STIPPLE_POW2_WIDTH)) >> 3] & (1 << (XSTIP(_a) & 7)))

#else

#define TGL_STIPPLEVARS /* a comment */

#endif

#if TGL_FEATURE_NO_DRAW_COLOR == 1
#define NODRAWTEST(c) &&((c & TGL_COLOR_MASK) != TGL_NO_DRAW_COLOR)
#else
#define NODRAWTEST(c) /* a comment */
#endif

#if TGL_FEATURE_POLYGON_STIPPLE == 1
#define TGL_STIPPLE_PASS(_a) (!(zbdostipple && !STIPBIT(_a)))
#else
#define TGL_STIPPLE_PASS(_a) 1
#endif

/* stencil state is snapshotted at triangle entry straight from the
   ZBuffer, so display list replay and each raster fast path observe the
   state current at execution time rather than anything baked in at
   compile time */
#define TGL_STENCILVARS                                                                      \
	GLint zb_stencil = zb->stencil_test;                                                     \
	GLenum zb_sfunc = zb->stencil_func;                                                      \
	GLubyte zb_sref = zb->stencil_ref;                                                       \
	GLubyte zb_svmask = zb->stencil_value_mask;                                              \
	GLubyte zb_swmask = zb->stencil_write_mask;                                              \
	GLenum zb_sop_sfail = zb->stencil_op_sfail;                                              \
	GLenum zb_sop_dpfail = zb->stencil_op_dpfail;                                            \
	GLenum zb_sop_dppass = zb->stencil_op_dppass;

static inline GLint tgl_stencil_compare(GLenum func, GLubyte lhs, GLubyte rhs) {
	switch (func) {
	case GL_NEVER:
		return 0;
	case GL_ALWAYS:
		return 1;
	case GL_EQUAL:
		return lhs == rhs;
	case GL_NOTEQUAL:
		return lhs != rhs;
	case GL_LESS:
		return lhs < rhs;
	case GL_LEQUAL:
		return lhs <= rhs;
	case GL_GREATER:
		return lhs > rhs;
	case GL_GEQUAL:
	default:
		return lhs >= rhs;
	}
}

/* computes the full 8 bit operation result; INCR/DECR saturate at 0/255 */
static inline GLubyte tgl_stencil_op_result(GLenum op, GLubyte oldv, GLubyte refv) {
	switch (op) {
	case GL_ZERO:
		return 0;
	case GL_REPLACE:
		return refv;
	case GL_INCR:
		return (oldv >= 255) ? 255 : (GLubyte)(oldv + 1);
	case GL_DECR:
		return (oldv == 0) ? 0 : (GLubyte)(oldv - 1);
	case GL_INVERT:
		return (GLubyte)(~oldv);
	case GL_KEEP:
	default:
		return oldv;
	}
}

/* merge the operation result back through the write mask; masked bits
   keep their old value */
#define TGL_STENCIL_WRITE(ps, opv)                                                           \
	*(ps) = (GLubyte)((*(ps) & (GLubyte)(~zb_swmask)) | ((GLubyte)(opv) & zb_swmask))

/* Runs the stencil stage for one candidate fragment. The caller has
   already applied polygon stipple and the TGL_NO_DRAW_COLOR discard, and
   passes the depth outcome (a disabled depth test counts as pass). The
   color body only runs when both stencil and depth pass. */
#define TGL_STENCIL_BLOCK(_a, _depthpass, _colorbody)                                       \
	do {                                                                                     \
		if (!zb_stencil) {                                                                   \
			if (_depthpass) {                                                                \
				_colorbody                                                                   \
			}                                                                                \
		} else {                                                                             \
			GLubyte* _sp = zb->sbuf + (pz - zb->zbuf) + (_a);                                \
			GLubyte _sv = *_sp;                                                              \
			GLubyte _lhs = (GLubyte)(zb_sref & zb_svmask);                                   \
			GLubyte _rhs = (GLubyte)(_sv & zb_svmask);                                       \
			if (!tgl_stencil_compare(zb_sfunc, _lhs, _rhs)) {                                \
				TGL_STENCIL_WRITE(_sp, tgl_stencil_op_result(zb_sop_sfail, _sv, zb_sref));   \
			} else if (!(_depthpass)) {                                                      \
				TGL_STENCIL_WRITE(_sp, tgl_stencil_op_result(zb_sop_dpfail, _sv, zb_sref));  \
			} else {                                                                         \
				TGL_STENCIL_WRITE(_sp, tgl_stencil_op_result(zb_sop_dppass, _sv, zb_sref));  \
				{                                                                            \
					_colorbody                                                               \
				}                                                                            \
			}                                                                                \
		}                                                                                    \
	} while (0)


void ZB_fillTriangleFlat(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdt = zb->depth_test;
	GLubyte zbdw = zb->depth_write;
	GLuint color;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS
	TGL_STENCILVARS

#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ

#define INTERP_Z


#define DRAW_INIT()                                                                                                                                            \
	{ color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); }

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				TGL_BLEND_FUNC(color, (pp[_a])) /*pp[_a] = color;*/                                                                                            \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

void ZB_fillTriangleFlatNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
	TGL_STENCILVARS
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#define INTERP_Z

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				pp[_a] = color;                                                                                                                                \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

/*
 * Smooth filled triangle.
 * The code below is very tricky :)
 */

void ZB_fillTriangleSmooth(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS
	TGL_STENCILVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}


#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif

#include "ztriangle.h"
}

void ZB_fillTriangleSmoothNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
	TGL_STENCILVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}

#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			/*c = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                               \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#endif

#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif
/* End of 16 bit mode stuff*/
#include "ztriangle.h"
}

/*


			TEXTURE MAPPED TRIANGLES
               Section_Header




*/
void ZB_setTexture(ZBuffer* zb, PIXEL* texture) { zb->current_texture = texture; }


#if 1

#define DRAW_LINE_TRI_TEXTURED()                                                                                                                               \
	{                                                                                                                                                          \
		register GLushort* pz;                                                                                                                                 \
		register PIXEL* pp;                                                                                                                                    \
		register GLuint s, t, z;                                                                                                                               \
		register GLint n;                                                                                                                                      \
		OR1OG1OB1DECL                                                                                                                                          \
		GLfloat sz, tz, fzl, zinv;                                                                                                                             \
		n = (x2 >> 16) - x1;                                                                                                                                   \
		fzl = (GLfloat)z1;                                                                                                                                     \
		zinv = 1.0 / fzl;                                                                                                                                      \
		pp = (PIXEL*)((GLbyte*)pp1 + x1 * PSZB);                                                                                                               \
		pz = pz1 + x1;                                                                                                                                         \
		z = z1;                                                                                                                                                \
		sz = sz1;                                                                                                                                              \
		tz = tz1;                                                                                                                                              \
		while (n >= (NB_INTERP - 1)) {                                                                                                                         \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				GLfloat ss, tt;                                                                                                                                \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			fzl += fndzdx;                                                                                                                                     \
			zinv = 1.0 / fzl;                                                                                                                                  \
			PUT_PIXEL(0); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(1); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(2); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(3); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(4); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(5); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(6); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(7); /*the_x-=7;*/                                                                                                                        \
			pz += NB_INTERP;                                                                                                                                   \
			pp += NB_INTERP; /*the_x+=NB_INTERP * PSZB;*/                                                                                                      \
			n -= NB_INTERP;                                                                                                                                    \
			sz += ndszdx;                                                                                                                                      \
			tz += ndtzdx;                                                                                                                                      \
		}                                                                                                                                                      \
		{                                                                                                                                                      \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				GLfloat ss, tt;                                                                                                                                \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			while (n >= 0) {                                                                                                                                   \
				PUT_PIXEL(0);                                                                                                                                  \
				pz += 1;                                                                                                                                       \
				/*pp = (PIXEL*)((GLbyte*)pp + PSZB);*/                                                                                                         \
				pp++;                                                                                                                                          \
				n -= 1;                                                                                                                                        \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
	} 

void ZB_fillTriangleMappingPerspective(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS
	TGL_STENCILVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB


#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				/*pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));*/                                                                       \
				TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, (TEXTURE_SAMPLE(texture, s, t))), (pp[_a]));                                                        \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		PIXEL c = TEXTURE_SAMPLE(texture, s, t);                                                                                                               \
		/* polygon stipple and the TGL_NO_DRAW_COLOR texel discard run before the stencil test */                                                               \
		if (TGL_STIPPLE_PASS(_a) NODRAWTEST(c)) {                                                                                                              \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));                                                                                      \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }

#include "ztriangle.h"
}

void ZB_fillTriangleMappingPerspectiveNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
	TGL_STENCILVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		if (TGL_STIPPLE_PASS(_a)) {                                                                                                                            \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));                                                                           \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		PIXEL c = TEXTURE_SAMPLE(texture, s, t);                                                                                                               \
		/* polygon stipple and the TGL_NO_DRAW_COLOR texel discard run before the stencil test */                                                               \
		if (TGL_STIPPLE_PASS(_a) NODRAWTEST(c)) {                                                                                                              \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			GLint dpass = (!zbdt) || (zz >= pz[_a]);                                                                                                           \
			TGL_STENCIL_BLOCK(_a, dpass, {                                                                                                                     \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, c);                                                                                                       \
				/*TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));*/                                                                                  \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			});                                                                                                                                                \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }
#include "ztriangle.h"
}

#endif
