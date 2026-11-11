/* Independent per-pixel reference for vsDoTransformRows() (rolling shutter).
 *
 * This module re-implements the DOCUMENTED contract of the row-interpolated
 * transform from scratch, in double precision, and samples the source with
 * its own bilinear interpolator.  It deliberately calls none of:
 *
 *   - the new entry point (vsDoTransformRows),
 *   - the production warp loops (transformPlanar/transformPacked and the
 *     Rows variants), or
 *   - the production coordinate-mapping helpers (prepare_transform,
 *     transform_vec, rotation_matrix_backward, ...).
 *
 * The only shared code is frame geometry -- VSFrameInfo, the plane
 * subsampling shifts and CHROMA_SIZE -- which is data layout, not algorithm.
 * tests/rolling_rows.c compares the production output against this reference
 * and derives the tolerated fixed point error there.
 *
 * Included in the rolling_rows test binary only; nothing in src/ uses it.
 */
#ifndef ROLLING_ROWS_REF_H
#define ROLLING_ROWS_REF_H

#include "transformtype.h"
#include "frameinfo.h"

/* Exposure fraction u of destination row j of a plane whose vertical
   subsampling is 2^hsub, in a frame lumaHeight luma scanlines tall: the row
   centre maps back to luma exposure position (j+0.5)*2^hsub - 0.5, clamped
   to [0, lumaHeight-1], normalized to [0,1]; a single-row frame gets 0.5. */
double rrRefRowU(int lumaHeight, int hsub, int j);

/* The transform at exposure fraction u: x, y and zoom linear between the
   endpoints, alpha along the shortest angular arc, endpoints exactly pi
   apart resolved to the positive direction. */
VSTransform rrRefTransformAt(VSTransform top, VSTransform bottom, double u);

/* Backward source coordinate (in that plane's pixel units, double) of
   destination pixel (x,y) of the given plane under transform t, following
   the public similarity model: with dx/dy measured from the destination
   centre, the source point is z*R(alpha)*(dx,dy) + c_s - t, where the
   rotation's cross terms are converted between the plane's axis scales so
   the angle lives in the isotropic luma space. */
void rrRefSrcCoord(const VSFrameInfo* fi, int plane, int x, int y,
                   const VSTransform* t, double* xs, double* ys);

/* Full per-pixel reference warp of one frame.
   crop != 0: out-of-range samples become black (luma 0, chroma 0x80, packed
   16).  crop == 0: out-of-range samples keep the value *dest already holds,
   so the caller pre-fills dest with the previous stabilized frame. */
void rrRefWarp(const VSFrame* src, VSFrame* dest, const VSFrameInfo* fi,
               VSTransform top, VSTransform bottom, int crop);

#endif
