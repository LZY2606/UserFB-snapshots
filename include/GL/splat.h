#ifndef _tgl_splat_h_
#define _tgl_splat_h_

/*
 * CPU 3D Gaussian splat overlay for TinyGL.
 *
 * The splats are projected from a world space mean and a full 3x3
 * covariance, composited on top of an already rasterised opaque ZBuffer.
 * The depth buffer is only read (occlusion test); it is never modified.
 * See doc/SPLAT.md for the data layout and the approximation limits.
 */

#include <stddef.h>
#include "zbuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One oriented, scaled Gaussian.
 *
 * mean[3]       : centre in world space.
 * covariance[6] : symmetric 3x3 covariance in world space, stored as the
 *                 upper triangle in the order
 *                 { xx, xy, xz, yy, yz, zz }.
 * rgb[3]        : colour channels, each in [0, 1].
 * opacity       : scalar opacity in [0, 1].
 *
 * All fields are double precision. The covariance must describe a
 * symmetric positive semi-definite matrix; the renderer additionally
 * requires the camera/projection space covariance to be invertible.
 */
typedef struct TGLSplat {
	double mean[3];
	double covariance[6];
	double rgb[3];
	double opacity;
} TGLSplat;

/*
 * View description. Both matrices are 4x4 and stored in OpenGL column
 * major order (the element at column c, row r is at index 4*c + r).
 *
 * view       : rigid body world-to-camera transform, right handed, with
 *              the camera looking down -Z.
 * projection : a finite perspective or orthographic projection that maps
 *              eye space to NDC with z in [-1, 1]. The current GL matrix
 *              state is not consulted.
 * x,y        : lower left corner of the viewport, in target pixels.
 * width,height: viewport size in pixels. The viewport must lie entirely
 *              inside the destination ZBuffer.
 */
typedef struct TGLSplatView {
	double view[16];
	double projection[16];
	int x, y, width, height;
} TGLSplatView;

/*
 * Optional instrumentation. Every field is written when stats != NULL.
 *
 * projected_count : points that pass the centre rule (positive clip w and
 *                   centre NDC z strictly inside (-1, 1)).
 * tile_refs       : number of (point, tile) pair references generated.
 * pixel_tests     : number of Mahalanobis q evaluations actually run.
 */
typedef struct TGLSplatStats {
	size_t projected_count;
	size_t tile_refs;
	size_t pixel_tests;
} TGLSplatStats;

/*
 * Render the splats into dst.
 *
 * Returns 0 on success and a non zero value on failure. On failure the
 * destination colour and depth buffers are left untouched. count == 0 is
 * legal (points may then be NULL) and leaves the output unchanged.
 *
 * dst and view must be non NULL. When count > 0 points must be non NULL.
 * Neither the input arrays nor the depth buffer are modified.
 */
int tglRenderSplats(ZBuffer *dst, const TGLSplat *points, size_t count,
                    const TGLSplatView *view, TGLSplatStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* _tgl_splat_h_ */
