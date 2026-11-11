/*
MATHC skinning helper: CPU vertex skinning without any graphics context.

This software is provided 'as-is', without any express or implied warranty. In
no event will the authors be held liable for any damages arising from the use of
this software.

Permission is granted to anyone to use this software for any purpose, including
commercial applications, and to alter it and redistribute it freely, subject to
the following restrictions:

  1. The origin of this software must not be misrepresented; you must not claim
     that you wrote the original software. If you use this software in a
     product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#ifndef MATHC_SKIN_H
#define MATHC_SKIN_H

#include <stdbool.h>
#include <stddef.h>

#include "mathc.h"

#if !defined(MATHC_USE_FLOATING_POINT)
#error "mathc_skin requires MATHC floating point support (do not define MATHC_NO_FLOATING_POINT)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
Skin a single vertex with a joint matrix palette, in mesh local space.

`palette` is an array of `palette_count` column-major 4x4 affine matrices
(16 mfloat_t each, same layout as mathc mat4). Every palette entry is assumed
to already contain its inverse-bind matrix, so the skinned vertex stays in
mesh local space; no extra inverse-bind or world matrix is applied here.

The four weights do not need to be normalized and several weights may refer
to the same joint. Non-negative finite weights are normalized by their sum
and the joint matrices are blended element-wise. Joints with a zero weight
are never accessed, so their indices may be out of range. The function
returns false (writing nothing) when a joint with a positive weight is out
of range or when the weight sum is zero.

The position is transformed by the blended matrix. The normal is transformed
by the inverse-transpose of the blended 3x3 linear part and normalized. The
tangent xyz is transformed by the blended linear part, orthogonalized
against the output normal (Gram-Schmidt) and normalized; the tangent w is
the input w (which must be +1 or -1) multiplied by the sign of the
determinant of the blended linear part, so that callers can reconstruct the
bitangent as cross(normal, tangent.xyz) * tangent.w. The translation of the
blended matrix never affects the normal or the tangent.

All used input components must be finite, the normal and the tangent xyz
must be non-zero and not parallel, the tangent w must be +1 or -1, and every
participating palette entry must be an affine matrix (last row 0 0 0 1).
The blended linear part A is rejected as degenerate when s == 0 or
abs(det(A)) <= 64 * MFLT_EPSILON * s^3, with s = max(abs(Aij)). The
orthogonalized tangent is rejected when its length is not greater than
64 * MFLT_EPSILON times the transformed (pre-orthogonalization) tangent
length. On any failure the outputs are left completely untouched.

Each output may be exactly the same array as its corresponding input
(out_position == position, out_normal == normal, out_tangent == tangent);
other forms of aliasing are not supported.
*/
bool mathc_skin_vertex(mfloat_t out_position[3], mfloat_t out_normal[3],
	mfloat_t out_tangent[4], const mfloat_t position[3],
	const mfloat_t normal[3], const mfloat_t tangent[4],
	const unsigned joints[4], const mfloat_t weights[4],
	const mfloat_t *palette, size_t palette_count);

#ifdef __cplusplus
}
#endif

#endif
