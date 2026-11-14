#ifndef _tgl_zlog_h_
#define _tgl_zlog_h_

/*
 * Shared helpers for the optional floating point logarithmic depth buffer.
 *
 * The logarithmic channel never reuses the integer window z. Each real
 * vertex carries:
 *   winv = 1 / clip-w          (the perspective weight)
 *   dist = eye-space distance d = -eye.z
 * Perspective-correct quantities are recovered in screen space by affine
 * interpolation of 1/w and d/w, i.e. at every fragment
 *   d = (d/w) / (1/w)
 * which is the standard perspective reconstruction, not an interpolation
 * of a logarithm stored on the vertices.
 */

#include "../include/zbuffer.h"

/* The perspective weight d/w fed to the planar interpolator for a point. */
static inline GLfloat ZB_logDistOverW(const ZBufferPoint* p) { return p->dist * p->winv; }

/* Reconstruct the eye-space distance at a fragment from the interpolated
   1/w (u) and d/w (v). Returns 0 for the degenerate zero-weight case so the
   caller's range test rejects the fragment. */
static inline GLfloat ZB_logReconstruct(GLfloat u, GLfloat v) {
	if (u <= 0.0f)
		return 0.0f;
	return v / u;
}

/* Encode and submit one logarithmic depth fragment.
 *   zb     - target buffer (log_depth is known to be allocated)
 *   index  - linear pixel index into both log_depth and the color buffer
 *   d      - reconstructed eye-space distance
 * Color storage is performed by the supplied statement only when the depth
 * contract passes; the depth write mask gates the float depth store. */
#define ZB_LOG_SUBMIT(zb, index, d, COLOR_WRITE)                                                                   \
	do {                                                                                                           \
		GLfloat ZB_log_d = (d);                                                                                    \
		if (ZB_log_d > 0.0f) {                                                                                     \
			GLfloat ZB_log_D = ZB_logEncode((zb), ZB_log_d);                                                      \
			/* fragments outside [near,far] are not submitted */                                                  \
			if (ZB_log_D >= 0.0f && ZB_log_D <= 1.0f) {                                                           \
				if (!(zb)->depth_test || ZB_log_D >= (zb)->log_depth[index]) {                                    \
					COLOR_WRITE;                                                                                   \
					if ((zb)->depth_write)                                                                         \
						(zb)->log_depth[index] = ZB_log_D;                                                         \
				}                                                                                                  \
			}                                                                                                      \
		}                                                                                                          \
	} while (0)

#endif /* _tgl_zlog_h_ */
