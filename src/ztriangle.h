/*
 * An eXtReMeLy complicated, delicate, tuned triangle rasterizer
 * Aight, so basically this is the most complicated code you'll ever read in your life.
 * The lifetime of variables has been... SUPER Optimized, that's why there's so many random ass curly braces everywhere.
 * Yes, it is necessary to do that. This code is extremely delicate
 * and even a minor fuck-up is gonna tank the framerate

Before committing any changes, run gears, model, and texture on your changed code to make sure you didn't
fuck up!

Things to keep in mind:
 1) Tight control of the lifetimes, scopes, and usage of variables lets us use registers more often and memory less
 2) Doing the same operation on multiple items is faster than doing different things on different items, generally, because
   they will be able to take advantage of any/all applicable SIMD/vector ops on your hardware.
 3) Divide operations are vastly more expensive than add/sub/bitwise/etc
 4) Bit shifting is your friend, it's the fast way to multiply or divide by 2.
 5) Fixed point math is used for the depth "z" buffer
 6) We're not just using floats for everything because this is still supposed to be fast on platforms without SSE2
 7) Fewer variables is usually better
 */

{
	GLfloat fdx1, fdx2, fdy1, fdy2;
	GLushort* pz1;
	PIXEL* pp1;

	GLint part;
	GLint dx1, dy1, dx2, dy2;
#if TGL_FEATURE_POLYGON_STIPPLE == 1
	GLint the_y;
#endif
	GLint error, derror;
	GLint x1, dxdy_min, dxdy_max;
	/* warning: x2 is multiplied by 2^16 */
	GLint x2=0, dx2dy2=0;

#ifdef INTERP_Z
	GLint z1, dzdx, dzdy, dzdl_min, dzdl_max;
#endif
#ifdef INTERP_RGB
	GLint r1, drdx, drdy, drdl_min, drdl_max;
	GLint g1, dgdx, dgdy, dgdl_min, dgdl_max;
	GLint b1, dbdx, dbdy, dbdl_min, dbdl_max;
#endif
#ifdef INTERP_ST
	GLint s1, dsdx, dsdy, dsdl_min, dsdl_max;
	GLint t1, dtdx, dtdy, dtdl_min, dtdl_max;
#endif
#ifdef INTERP_STZ
	/*
	 * Perspective-correct texture mapping.
	 *
	 * 1/w, s/w and t/w are affine functions of the integer screen
	 * coordinates, so they are evaluated at each covered pixel and the
	 * final coordinate is the exact per-pixel quotient:
	 *
	 *   s = (sum lambda_i * s_i / w_i) / (sum lambda_i / w_i)
	 *
	 * where w_i is the clip-space w of the vertex (ZBufferPoint.w).
	 * Window-space z is only used for depth test/store.
	 */
	GLdouble st_iwx, st_iwy, st_iwc;
	GLdouble st_swx, st_swy, st_swc;
	GLdouble st_twx, st_twy, st_twc;
	GLint st_x, st_y;
#endif

	/* we sort the vertex with increasing y */
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

	/* we compute dXdx and dXdy for all GLinterpolated values */
	fdx1 = p1->x - p0->x; 
	fdy1 = p1->y - p0->y; 

	fdx2 = p2->x - p0->x;
	fdy2 = p2->y - p0->y;

	GLfloat fz = fdx1 * fdy2 - fdx2 * fdy1; 
	/*
	if (fz == 0)
		return;
	fz = 1.0 / fz; 
	*/
	if (fz != 0.0)
		fz = 1.0 / fz;
	
	fdx1 *= fz;
	fdy1 *= fz;
	fdx2 *= fz;
	fdy2 *= fz;
	
	{
		GLfloat d1, d2;
#ifdef INTERP_Z
		{
			d1 = p1->z - p0->z; 
			d2 = p2->z - p0->z;
			dzdx = (GLint)(fdy2 * d1 - fdy1 * d2);
			dzdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		}
#endif

#ifdef INTERP_RGB
		{
			d1 = p1->r - p0->r;
			d2 = p2->r - p0->r;
			drdx = (GLint)(fdy2 * d1 - fdy1 * d2);
			drdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		}
		{
			d1 = p1->g - p0->g;
			d2 = p2->g - p0->g;
			dgdx = (GLint)(fdy2 * d1 - fdy1 * d2);
			dgdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		}
		{
			d1 = p1->b - p0->b;
			d2 = p2->b - p0->b;
			dbdx = (GLint)(fdy2 * d1 - fdy1 * d2);
			dbdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		}
#endif

#ifdef INTERP_ST
		{
			d1 = p1->s - p0->s;
			d2 = p2->s - p0->s;
			dsdx = (GLint)(fdy2 * d1 - fdy1 * d2);
			dsdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		}
		{
			d1 = p1->t - p0->t;
			d2 = p2->t - p0->t;
			dtdx = (GLint)(fdy2 * d1 - fdy1 * d2);
			dtdy = (GLint)(fdx1 * d2 - fdx2 * d1);
		}
#endif

#ifdef INTERP_STZ
		/*
		 * The affine coefficients for 1/w, s/w and t/w are computed by
		 * DRAW_INIT() (see ZB_fillTriangleMappingPerspective).  Only the
		 * current scanline index has to be tracked here.
		 */
#endif
	}
	/* screen coordinates */

	pp1 = (PIXEL*)(zb->pbuf) + zb->xsize * p0->y;
#if TGL_FEATURE_POLYGON_STIPPLE == 1
	the_y = p0->y;
#endif
#ifdef INTERP_STZ
	st_y = p0->y;
#endif
	pz1 = zb->zbuf + p0->y * zb->xsize;

	DRAW_INIT();
	/*
	 part used here and down.
	 TODO: #pragma omp parallel for private(a, b, c)
	 Required reading:
	 jakascorner.com/blog/2016/06/omp-data-sharing-attributes.html
	 I'd also like to figure out if the main while() loop over raster lines can be OMP parallelized, but I suspect it isn't worth it.
	*/
	ZBufferPoint *pr1, *pr2, *l1, *l2; 
	for (part = 0; part < 2; part++) {
		GLint nb_lines;
		{
			register GLint update_left, update_right; 
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
				/* second part */
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
				}
				nb_lines = p2->y - p1->y + 1;
			}
			/* compute the values for the left edge */
			/*pr1 and pr2 are not used inside this area.*/
			if (update_left) { 
				{
					register GLint tmp;
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
				}
				dxdy_max = dxdy_min + 1;
#ifdef INTERP_Z
				z1 = l1->z;
				dzdl_min = (dzdy + dzdx * dxdy_min);
				dzdl_max = dzdl_min + dzdx;
#endif
#ifdef INTERP_RGB
				r1 = l1->r;
				drdl_min = (drdy + drdx * dxdy_min);
				drdl_max = drdl_min + drdx;
				g1 = l1->g;
				dgdl_min = (dgdy + dgdx * dxdy_min);
				dgdl_max = dgdl_min + dgdx;
				b1 = l1->b;
				dbdl_min = (dbdy + dbdx * dxdy_min);
				dbdl_max = dbdl_min + dbdx;
#endif
#ifdef INTERP_ST
				s1 = l1->s;
				dsdl_min = (dsdy + dsdx * dxdy_min);
				dsdl_max = dsdl_min + dsdx;
				t1 = l1->t;
				dtdl_min = (dtdy + dtdx * dxdy_min);
				dtdl_max = dtdl_min + dtdx;
#endif
#ifdef INTERP_STZ
				/* base values at the left edge of this scanline */
#endif
			}
			/* compute values for the right edge */

			if (update_right) { /*Update right tested*/
				dx2 = (pr2->x - pr1->x);
				dy2 = (pr2->y - pr1->y); /*LAST USAGE OF PR2*/
				if (dy2 > 0)
					dx2dy2 = (dx2 << 16) / dy2;
				else
					dx2dy2 = 0;
				x2 = pr1->x << 16; /*LAST USAGE OF PR1*/
			}					   /*EOF update right*/
		}						   /*End of lifetime for ZBufferpoints*/
		/* we draw all the scan line of the part */

		while (nb_lines > 0) {
			nb_lines--;
#ifndef DRAW_LINE
			/* generic draw line */
			{
				register PIXEL* pp;
				register GLint n;
#ifdef INTERP_Z
				register GLushort* pz;
				register GLuint z;
#endif
#ifdef INTERP_RGB
				register GLint or1, og1, ob1;
#endif
#ifdef INTERP_ST
				register GLuint s, t;
#endif
#ifdef INTERP_STZ
				/* values are evaluated per pixel from the plane coefficients */
#endif

				n = (x2 >> 16) - x1;
				
				pp = (PIXEL*)pp1 + x1;
#ifdef INTERP_Z
				pz = pz1 + x1;
				z = z1;
#endif
#ifdef INTERP_RGB
				or1 = r1;
				og1 = g1;
				ob1 = b1;
#endif
#ifdef INTERP_ST
				s = s1;
				t = t1;
#endif
#ifdef INTERP_STZ
				/* values are evaluated per pixel from the plane coefficients */
#endif
				while (n >= 3) {
					PUT_PIXEL(0); /*the_x++;*/
					PUT_PIXEL(1); /*the_x++;*/
					PUT_PIXEL(2); /*the_x++;*/
					PUT_PIXEL(3); /*the_x++;*/
#ifdef INTERP_Z
					pz += 4;
#endif
					
					pp += 4;
					n -= 4;
				}
				while (n >= 0) {
					PUT_PIXEL(0); /*the_x++;*/
#ifdef INTERP_Z
					
					pz++;
#endif
					/*pp = (PIXEL*)((GLbyte*)pp + PS_ZB);*/
					pp++;
					n--;
				}
			}
#else
			DRAW_LINE();
#endif

			/* left edge */
			error += derror;
			if (error > 0) {
				error -= 0x10000;
				x1 += dxdy_max;
#ifdef INTERP_Z
				z1 += dzdl_max;
#endif
#ifdef INTERP_RGB
				r1 += drdl_max;
				g1 += dgdl_max;
				b1 += dbdl_max;
#endif
#ifdef INTERP_ST
				s1 += dsdl_max;
				t1 += dtdl_max;
#endif
			} else {
				x1 += dxdy_min;
#ifdef INTERP_Z
				z1 += dzdl_min;
#endif
#ifdef INTERP_RGB
				r1 += drdl_min;
				g1 += dgdl_min;
				b1 += dbdl_min;
#endif
#ifdef INTERP_ST
				s1 += dsdl_min;
				t1 += dtdl_min;
#endif
			}

			/* right edge */
			x2 += dx2dy2;

			/* screen coordinates */
			
			pp1 += zb->xsize;
#if TGL_FEATURE_POLYGON_STIPPLE == 1
			the_y++;
#endif
#ifdef INTERP_STZ
			st_y++;
#endif
			pz1 += zb->xsize;
		}
	}
}

#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ

#undef DRAW_INIT
#undef DRAW_LINE
#undef PUT_PIXEL
