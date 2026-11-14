/*
 * Self contained scanline rasterizer for the optional logarithmic depth
 * buffer. The integer path in ztriangle.c is left untouched so the default
 * depth mode renders exactly as before.
 *
 * Depth is never taken from the integer window z. Each vertex carries
 * winv = 1/clip-w and dist = eye distance d=-eye.z; the planar screen
 * interpolants are u = 1/w and v = d/w, and every fragment reconstructs
 * d = v/u before encoding the logarithmic value.
 *
 * Textured triangles deliberately reproduce the legacy sampling position
 * bit for bit: perspective correction keeps interpolating s*z and t*z
 * against the integer window z with the same block walk as ztriangle.c.
 * That window z is a texture coordinate channel only; the depth contract
 * still reconstructs distance solely from u and v.
 */

#include "../include/zbuffer.h"
#include "msghandling.h"
#include "zlog.h"
#include <stdlib.h>

#define LOG_NB_INTERP 8

#if TGL_FEATURE_POLYGON_STIPPLE == 1
#define LOG_STIPPLE_VARS                                                                          \
	GLubyte* zbstipplepattern = zb->stipplepattern;                                               \
	GLubyte zbdostipple = zb->dostipple;
#define LOG_STIPPLE_TEST(the_x, the_y)                                                           \
	(!(zbdostipple && !(zbstipplepattern[(((the_x) & TGL_POLYGON_STIPPLE_MASK_X) |               \
	                                      (((the_y) & TGL_POLYGON_STIPPLE_MASK_Y)                \
	                                       << TGL_POLYGON_STIPPLE_POW2_WIDTH)) >>                 \
	                                      3] &                                                    \
	                              (1 << ((the_x) & 7)))))
#else
#define LOG_STIPPLE_VARS
#define LOG_STIPPLE_TEST(the_x, the_y) 1
#endif

typedef struct {
	GLfloat u, v;    /* 1/w and d/w log-depth interpolants */
	GLint r, g, b;   /* smooth color interpolants */
	GLint z;         /* legacy integer window z (texture perspective only) */
	GLfloat sz, tz;  /* legacy s*z and t*z texture interpolants */
} LogSpan;

static void log_triangle(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2, int textured, int smooth, PIXEL flat_color) {
	GLfloat fdx1, fdy1, fdx2, fdy2, fz;
	GLint part;
	GLint dx1, dy1, dx2, dy2;
	GLint error, derror;
	GLint x1, dxdy_min, dxdy_max;
	GLint x2 = 0, dx2dy2 = 0;
	GLint the_y;

	GLfloat dudx, dudy, dvdx, dvdy;
	GLint drdx = 0, drdy = 0, dgdx = 0, dgdy = 0, dbdx = 0, dbdy = 0;
	/* legacy texture perspective gradients, matching ztriangle.h names */
	GLint dzdx = 0, dzdy = 0;
	GLfloat dszdx = 0, dszdy = 0, dtzdx = 0, dtzdy = 0;
	GLfloat fdzdx = 0, fndzdx = 0, ndszdx = 0, ndtzdx = 0;

	PIXEL* texture = zb->current_texture;
	LOG_STIPPLE_VARS
	TGL_BLEND_VARS

	/* sort vertices by increasing y, exactly like ztriangle.h */
	if (p1->y < p0->y) {
		ZBufferPoint* t = p0;
		p0 = p1;
		p1 = t;
	}
	if (p2->y < p0->y) {
		ZBufferPoint* t = p2;
		p2 = p1;
		p1 = p0;
		p0 = t;
	} else if (p2->y < p1->y) {
		ZBufferPoint* t = p1;
		p1 = p2;
		p2 = t;
	}

	fdx1 = p1->x - p0->x;
	fdy1 = p1->y - p0->y;
	fdx2 = p2->x - p0->x;
	fdy2 = p2->y - p0->y;
	fz = fdx1 * fdy2 - fdx2 * fdy1;
	if (fz == 0.0f)
		return;
	fz = 1.0f / fz;
	fdx1 *= fz;
	fdy1 *= fz;
	fdx2 *= fz;
	fdy2 *= fz;

	{
		GLfloat d1, d2;
		d1 = p1->winv - p0->winv;
		d2 = p2->winv - p0->winv;
		dudx = fdy2 * d1 - fdy1 * d2;
		dudy = fdx1 * d2 - fdx2 * d1;
		d1 = ZB_logDistOverW(p1) - ZB_logDistOverW(p0);
		d2 = ZB_logDistOverW(p2) - ZB_logDistOverW(p0);
		dvdx = fdy2 * d1 - fdy1 * d2;
		dvdy = fdx1 * d2 - fdx2 * d1;
	}
	if (smooth) {
		GLint d1, d2;
		d1 = p1->r - p0->r;
		d2 = p2->r - p0->r;
		drdx = (GLint)(fdy2 * d1 - fdy1 * d2);
		drdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		d1 = p1->g - p0->g;
		d2 = p2->g - p0->g;
		dgdx = (GLint)(fdy2 * d1 - fdy1 * d2);
		dgdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		d1 = p1->b - p0->b;
		d2 = p2->b - p0->b;
		dbdx = (GLint)(fdy2 * d1 - fdy1 * d2);
		dbdy = (GLint)(fdx1 * d2 - fdx2 * d1);
	}
	if (textured) {
		GLint d1, d2;
		GLfloat fd1, fd2;
		/* legacy window-z perspective texture correction */
		d1 = p1->z - p0->z;
		d2 = p2->z - p0->z;
		dzdx = (GLint)(fdy2 * d1 - fdy1 * d2);
		dzdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		fd1 = (GLfloat)p1->s * (GLfloat)p1->z - (GLfloat)p0->s * (GLfloat)p0->z;
		fd2 = (GLfloat)p2->s * (GLfloat)p2->z - (GLfloat)p0->s * (GLfloat)p0->z;
		dszdx = fdy2 * fd1 - fdy1 * fd2;
		dszdy = fdx1 * fd2 - fdx2 * fd1;
		fd1 = (GLfloat)p1->t * (GLfloat)p1->z - (GLfloat)p0->t * (GLfloat)p0->z;
		fd2 = (GLfloat)p2->t * (GLfloat)p2->z - (GLfloat)p0->t * (GLfloat)p0->z;
		dtzdx = fdy2 * fd1 - fdy1 * fd2;
		dtzdy = fdx1 * fd2 - fdx2 * fd1;
		/* legacy DRAW_INIT() constants */
		fdzdx = (GLfloat)dzdx;
		fndzdx = LOG_NB_INTERP * fdzdx;
		ndszdx = LOG_NB_INTERP * dszdx;
		ndtzdx = LOG_NB_INTERP * dtzdx;
	}

	the_y = p0->y;
	for (part = 0; part < 2; part++) {
		GLint nb_lines;
		GLint update_left, update_right;
		ZBufferPoint *pr1, *pr2, *l1, *l2;
		LogSpan left;

		if (part == 0) {
			if (fz > 0) {
				update_left = 1;
				update_right = 1;
				l1 = p0;
				l2 = p2;
				pr1 = p0;
				pr2 = p1;
			} else {
				update_left = 1;
				update_right = 1;
				l1 = p0;
				l2 = p1;
				pr1 = p0;
				pr2 = p2;
			}
			nb_lines = p1->y - p0->y;
		} else {
			if (fz > 0) {
				update_left = 0;
				update_right = 1;
				pr1 = p1;
				pr2 = p2;
			} else {
				update_left = 1;
				update_right = 0;
				l1 = p1;
				l2 = p2;
				pr1 = p1;
				pr2 = p2;
			}
			nb_lines = p2->y - p1->y + 1;
		}

		if (update_left) {
			GLint tmp;
			dy1 = l2->y - l1->y;
			dx1 = l2->x - l1->x;
			if (dy1 > 0)
				tmp = (dx1 << 16) / dy1;
			else
				tmp = 0;
			x1 = l1->x;
			error = 0;
			derror = tmp & 0x0000ffff;
			dxdy_min = tmp >> 16;
			dxdy_max = dxdy_min + 1;

			left.u = l1->winv;
			left.v = ZB_logDistOverW(l1);
			if (smooth) {
				left.r = l1->r;
				left.g = l1->g;
				left.b = l1->b;
			}
			if (textured) {
				left.z = l1->z;
				left.sz = (GLfloat)l1->s * (GLfloat)l1->z;
				left.tz = (GLfloat)l1->t * (GLfloat)l1->z;
			}
		}
		if (update_right) {
			dy2 = pr2->y - pr1->y;
			dx2 = pr2->x - pr1->x;
			if (dy2 > 0)
				dx2dy2 = (dx2 << 16) / dy2;
			else
				dx2dy2 = 0;
			x2 = pr1->x << 16;
		}

		while (nb_lines > 0) {
			GLint n = (x2 >> 16) - x1;
			GLfloat uu = left.u;
			GLfloat vv = left.v;
			GLint or1 = left.r;
			GLint og1 = left.g;
			GLint ob1 = left.b;
			GLint px = x1;
			GLint base = the_y * zb->xsize;

			if (textured) {
				/* exact legacy perspective texture walk (block of 8) */
				GLfloat sz = left.sz;
				GLfloat tz = left.tz;
				GLfloat fzl = (GLfloat)left.z;
				GLfloat zinv = 1.0f / fzl;

				while (n >= (LOG_NB_INTERP - 1)) {
					GLint dsdx, dtdx, k;
					GLuint s, t;
					GLfloat ss = (sz * zinv);
					GLfloat tt = (tz * zinv);
					s = (GLint)ss;
					t = (GLint)tt;
					dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);
					dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);
					fzl += fndzdx;
					zinv = 1.0f / fzl;
					for (k = 0; k < LOG_NB_INTERP; k++) {
						if (px >= 0 && the_y >= 0 && px < zb->xsize && the_y < zb->ysize && LOG_STIPPLE_TEST(px, the_y)) {
							GLint index = base + px;
							PIXEL* pp = (PIXEL*)((GLbyte*)zb->pbuf + zb->linesize * the_y + px * PSZB);
							GLfloat d = ZB_logReconstruct(uu, vv);
#if TGL_FEATURE_LIT_TEXTURES == 1
							PIXEL texel = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));
#else
							PIXEL texel = TEXTURE_SAMPLE(texture, s, t);
#endif
#if TGL_FEATURE_NO_DRAW_COLOR == 1
							if ((texel & TGL_COLOR_MASK) != TGL_NO_DRAW_COLOR)
#endif
							{
#if TGL_FEATURE_BLEND == 1
								if (!zb->enable_blend)
									ZB_LOG_SUBMIT(zb, index, d, *pp = texel);
								else
									ZB_LOG_SUBMIT(zb, index, d, TGL_BLEND_FUNC(texel, (*pp)));
#else
								ZB_LOG_SUBMIT(zb, index, d, *pp = texel);
#endif
							}
						}
						s += dsdx;
						t += dtdx;
#if TGL_FEATURE_LIT_TEXTURES == 1
						or1 += drdx;
						og1 += dgdx;
						ob1 += dbdx;
#endif
						uu += dudx;
						vv += dvdx;
						px++;
						n--;
					}
					sz += ndszdx;
					tz += ndtzdx;
				}
				{
					GLfloat ss = (sz * zinv);
					GLfloat tt = (tz * zinv);
					GLuint s = (GLint)ss;
					GLuint t = (GLint)tt;
					GLint dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);
					GLint dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);
					while (n >= 0) {
						if (px >= 0 && the_y >= 0 && px < zb->xsize && the_y < zb->ysize && LOG_STIPPLE_TEST(px, the_y)) {
							GLint index = base + px;
							PIXEL* pp = (PIXEL*)((GLbyte*)zb->pbuf + zb->linesize * the_y + px * PSZB);
							GLfloat d = ZB_logReconstruct(uu, vv);
#if TGL_FEATURE_LIT_TEXTURES == 1
							PIXEL texel = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));
#else
							PIXEL texel = TEXTURE_SAMPLE(texture, s, t);
#endif
#if TGL_FEATURE_NO_DRAW_COLOR == 1
							if ((texel & TGL_COLOR_MASK) != TGL_NO_DRAW_COLOR)
#endif
							{
#if TGL_FEATURE_BLEND == 1
								if (!zb->enable_blend)
									ZB_LOG_SUBMIT(zb, index, d, *pp = texel);
								else
									ZB_LOG_SUBMIT(zb, index, d, TGL_BLEND_FUNC(texel, (*pp)));
#else
								ZB_LOG_SUBMIT(zb, index, d, *pp = texel);
#endif
							}
						}
						s += dsdx;
						t += dtdx;
#if TGL_FEATURE_LIT_TEXTURES == 1
						or1 += drdx;
						og1 += dgdx;
						ob1 += dbdx;
#endif
						px++;
						n--;
						uu += dudx;
						vv += dvdx;
					}
				}
			} else {
				while (n >= 0) {
					if (px >= 0 && the_y >= 0 && px < zb->xsize && the_y < zb->ysize && LOG_STIPPLE_TEST(px, the_y)) {
						GLint index = base + px;
						PIXEL* pp = (PIXEL*)((GLbyte*)zb->pbuf + zb->linesize * the_y + px * PSZB);
						GLfloat d = ZB_logReconstruct(uu, vv);
						if (smooth) {
#if TGL_FEATURE_BLEND == 1
							if (!zb->enable_blend)
								ZB_LOG_SUBMIT(zb, index, d, *pp = RGB_TO_PIXEL(or1, og1, ob1));
							else
								ZB_LOG_SUBMIT(zb, index, d, TGL_BLEND_FUNC_RGB(or1, og1, ob1, (*pp)));
#else
							ZB_LOG_SUBMIT(zb, index, d, *pp = RGB_TO_PIXEL(or1, og1, ob1));
#endif
							or1 += drdx;
							og1 += dgdx;
							ob1 += dbdx;
						} else {
#if TGL_FEATURE_BLEND == 1
							if (!zb->enable_blend)
								ZB_LOG_SUBMIT(zb, index, d, *pp = flat_color);
							else
								ZB_LOG_SUBMIT(zb, index, d, TGL_BLEND_FUNC(flat_color, (*pp)));
#else
							ZB_LOG_SUBMIT(zb, index, d, *pp = flat_color);
#endif
						}
					} else if (smooth) {
						or1 += drdx;
						og1 += dgdx;
						ob1 += dbdx;
					}
					uu += dudx;
					vv += dvdx;
					px++;
					n--;
				}
			}

			/* advance the left edge */
			error += derror;
			if (error > 0) {
				error -= 0x10000;
				x1 += dxdy_max;
				left.u += dudy + dudx * dxdy_max;
				left.v += dvdy + dvdx * dxdy_max;
				if (smooth) {
					left.r += drdy + drdx * dxdy_max;
					left.g += dgdy + dgdx * dxdy_max;
					left.b += dbdy + dbdx * dxdy_max;
				}
				if (textured) {
					left.z += dzdy + dzdx * dxdy_max;
					left.sz += dszdy + dszdx * dxdy_max;
					left.tz += dtzdy + dtzdx * dxdy_max;
				}
			} else {
				x1 += dxdy_min;
				left.u += dudy + dudx * dxdy_min;
				left.v += dvdy + dvdx * dxdy_min;
				if (smooth) {
					left.r += drdy + drdx * dxdy_min;
					left.g += dgdy + dgdx * dxdy_min;
					left.b += dbdy + dbdx * dxdy_min;
				}
				if (textured) {
					left.z += dzdy + dzdx * dxdy_min;
					left.sz += dszdy + dszdx * dxdy_min;
					left.tz += dtzdy + dtzdx * dxdy_min;
				}
			}
			x2 += dx2dy2;
			the_y++;
			nb_lines--;
		}
	}
}

void ZB_fillTriangleFlatLog(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
	log_triangle(zb, p0, p1, p2, 0, 0, color);
}

void ZB_fillTriangleSmoothLog(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	log_triangle(zb, p0, p1, p2, 0, 1, 0);
}

void ZB_fillTriangleMappingPerspectiveLog(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	log_triangle(zb, p0, p1, p2, 1, 1, 0);
}
