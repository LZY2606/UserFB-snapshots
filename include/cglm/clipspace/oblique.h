/*
 * Copyright (c), Recep Aslantas.
 *
 * MIT License (MIT), http://opensource.org/licenses/MIT
 * Full license can be found in the LICENSE file
 */

/*
 Functions:
   CGLM_INLINE bool glm_persp_oblique_rh_no(CGLM_CONST mat4 proj,
                                            const vec4 plane,
                                            mat4 dest)
   CGLM_INLINE bool glm_persp_oblique_rh_zo(CGLM_CONST mat4 proj,
                                            const vec4 plane,
                                            mat4 dest)
 */

#ifndef cglm_oblique_h
#define cglm_oblique_h

#include "../common.h"
#include "../mat4.h"
#include "../plane.h"

/*!
 * @brief common worker for the right-handed oblique near plane helpers
 *
 * Replaces the third column of a perspective projection so that the eye
 * space [plane] becomes the near clipping plane, while rows x, y and w
 * keep their original values.
 *
 * The far plane is anchored through the homogeneous far corner
 *   q = inv(proj) * (s(plane.x), s(plane.y), 1, 1)
 * where s(t) is -1 for t < 0 and 1 otherwise. q is kept homogeneous:
 * its w component may be zero when [proj] has an infinite far plane.
 *
 * @param[in]  proj     right-handed, forward-depth perspective projection,
 *                      may be asymmetric or have an infinite far plane
 * @param[in]  plane    eye space plane (a, b, c, d), the half space
 *                      a*x + b*y + c*z + d >= 0 is kept
 * @param[in]  nearClip clip-space z value of the new near plane:
 *                      -1.0f for NO (z = -w), 0.0f for ZO (z = 0)
 * @param[out] dest     oblique projection; may alias [proj]
 *
 * @return true when [dest] was written, false on any invalid input or
 *         when plane . q is not strictly positive
 */
CGLM_INLINE
bool
glm_persp_oblique_rh_p(CGLM_CONST mat4 proj,
                       const vec4 plane,
                       float nearClip,
                       mat4 dest) {
  CGLM_ALIGN_MAT mat4 invProj, work;
  vec4 farCorner, farPoint;
  float dot, scale;
  int i;

  for (i = 0; i < 4; i++) {
    if (!isfinite(plane[i]))
      return false;
  }

  for (i = 0; i < 4; i++) {
    if (!isfinite(proj[i][0]) || !isfinite(proj[i][1])
        || !isfinite(proj[i][2]) || !isfinite(proj[i][3]))
      return false;
  }

  /* plane normal must be non-zero */
  if (!(plane[0] * plane[0]
      + plane[1] * plane[1]
      + plane[2] * plane[2] > 0.0f))
    return false;

  /* the camera sits at the origin, d < 0 puts it in the kept half space */
  if (!(plane[3] < 0.0f))
    return false;

  if (glm_mat4_det(proj) == 0.0f)
    return false;

  glm_mat4_inv(proj, invProj);

  farCorner[0] = plane[0] < 0.0f ? -1.0f : 1.0f;
  farCorner[1] = plane[1] < 0.0f ? -1.0f : 1.0f;
  farCorner[2] = 1.0f;
  farCorner[3] = 1.0f;

  /* do not divide by farPoint.w, it can be zero for infinite far planes */
  glm_mat4_mulv(invProj, farCorner, farPoint);

  for (i = 0; i < 4; i++) {
    if (!isfinite(farPoint[i]))
      return false;
  }

  dot = glm_vec4_dot(plane, farPoint);
  if (!(dot > 0.0f))
    return false;

  /* NO maps the near plane to z = -w, ZO maps it to z = 0 */
  scale = (1.0f - nearClip) / dot;
  if (!isfinite(scale))
    return false;

  glm_mat4_copy(proj, work);

  work[0][2] = scale * plane[0];
  work[1][2] = scale * plane[1];
  work[2][2] = scale * plane[2] - nearClip;
  work[3][2] = scale * plane[3];

  glm_mat4_copy(work, dest);

  return true;
}

/*!
 * @brief make an eye space plane the near clipping plane of a right-handed
 *        perspective projection with clip-space depth in [-1, 1] (NO)
 *
 * [proj] must be a right-handed, forward-depth perspective projection; it
 * may be asymmetric or use an infinite far plane. Orthographic projections
 * and reverse-Z are not supported.
 *
 * [plane] = (a, b, c, d) is given in eye space and the half space
 * a*x + b*y + c*z + d >= 0 is kept. The normal (a, b, c) must be non-zero
 * and d must be negative. Scaling [plane] by any positive factor does not
 * change the result.
 *
 * On success rows x, y and w of the projection are left untouched, while
 * [plane] becomes the new near plane mapped to clip-space z = -w. The
 * selected far corner of the original frustum stays on the far plane
 * (z = w).
 *
 * @param[in]  proj   perspective projection matrix
 * @param[in]  plane  eye space near plane (a, b, c, d)
 * @param[out] dest   resulting projection; may alias [proj]
 *
 * @return false without touching [dest] when any component is non-finite,
 *         [proj] is singular, the normal is zero, d >= 0 or plane . q is
 *         not strictly positive; true otherwise
 */
CGLM_INLINE
bool
glm_persp_oblique_rh_no(CGLM_CONST mat4 proj,
                        const vec4 plane,
                        mat4 dest) {
  return glm_persp_oblique_rh_p(proj, plane, -1.0f, dest);
}

/*!
 * @brief make an eye space plane the near clipping plane of a right-handed
 *        perspective projection with clip-space depth in [0, 1] (ZO)
 *
 * [proj] must be a right-handed, forward-depth perspective projection; it
 * may be asymmetric or use an infinite far plane. Orthographic projections
 * and reverse-Z are not supported.
 *
 * [plane] = (a, b, c, d) is given in eye space and the half space
 * a*x + b*y + c*z + d >= 0 is kept. The normal (a, b, c) must be non-zero
 * and d must be negative. Scaling [plane] by any positive factor does not
 * change the result.
 *
 * On success rows x, y and w of the projection are left untouched, while
 * [plane] becomes the new near plane mapped to clip-space z = 0. The
 * selected far corner of the original frustum stays on the far plane
 * (z = w).
 *
 * @param[in]  proj   perspective projection matrix
 * @param[in]  plane  eye space near plane (a, b, c, d)
 * @param[out] dest   resulting projection; may alias [proj]
 *
 * @return false without touching [dest] when any component is non-finite,
 *         [proj] is singular, the normal is zero, d >= 0 or plane . q is
 *         not strictly positive; true otherwise
 */
CGLM_INLINE
bool
glm_persp_oblique_rh_zo(CGLM_CONST mat4 proj,
                        const vec4 plane,
                        mat4 dest) {
  return glm_persp_oblique_rh_p(proj, plane, 0.0f, dest);
}

#endif /* cglm_oblique_h */
