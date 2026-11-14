#ifndef _tgl_splat_h_
#define _tgl_splat_h_

/*
 * CPU 3D Gaussian splat rendering for TinyGL.
 *
 * The splats are composited on top of whatever is already in the 32 bit
 * RGBA frame buffer of a ZBuffer (for example the opaque triangles rendered
 * through the regular TinyGL pipeline).  The depth buffer is only read for
 * the opaque occlusion test, it is never modified.
 *
 * The implementation only uses C99 and the C standard library and keeps all
 * of its working memory local to a single tglRenderSplats() call.
 */

#include <stddef.h>

#include "../zbuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A single Gaussian.
 *
 * mean[3]       world space position of the Gaussian mean.
 * covariance[6] symmetric 3x3 world space covariance, stored in the order
 *               xx, xy, xz, yy, yz, zz.  It must be positive (semi)definite.
 * rgb[3]        linear color, every component in [0,1].
 * opacity       scalar opacity in [0,1].
 *
 * All values must be finite.
 */
typedef struct TGLSplat {
	double mean[3];
	double covariance[6];
	double rgb[3];
	double opacity;
} TGLSplat;

/*
 * The view used to project the splats.  It is fully self contained and does
 * not depend on the current TinyGL matrix or viewport state.
 *
 * view[16] and projection[16] are ordinary OpenGL matrices stored in column
 * major order.  view is a rigid world-to-camera transform of a right handed
 * camera looking down -Z.  projection is a finite perspective or orthographic
 * projection with NDC z in [-1,1].
 *
 * x,y,width,height describe the viewport rectangle in target pixels.  The
 * whole rectangle must lie inside the target frame buffer.
 */
typedef struct TGLSplatView {
	double view[16];
	double projection[16];
	int x;
	int y;
	int width;
	int height;
} TGLSplatView;

/*
 * Optional run statistics.  When a non NULL pointer is passed every field is
 * overwritten by the call.
 *
 * projected_count  splats whose center passes the w>0 and NDC z rules.
 * tile_refs        number of (splat, tile) bucket references actually built.
 * pixel_tests      number of Mahalanobis q evaluations actually performed.
 */
typedef struct TGLSplatStats {
	size_t projected_count;
	size_t tile_refs;
	size_t pixel_tests;
} TGLSplatStats;

/*
 * Composite the splats onto dst.
 *
 * Returns 0 on success and a non zero value if any input is invalid, an
 * intermediate value is non finite, a covariance is not positive definite,
 * or a temporary allocation fails.  On failure the frame buffer and depth
 * buffer are left untouched.  A count of 0 is legal (points may then be NULL)
 * and leaves the target unchanged.
 */
int tglRenderSplats(ZBuffer *dst, const TGLSplat *points, size_t count,
                    const TGLSplatView *view, TGLSplatStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* _tgl_splat_h_ */
