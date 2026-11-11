/*
 * Copyright (c), Recep Aslantas.
 *
 * MIT License (MIT), http://opensource.org/licenses/MIT
 * Full license can be found in the LICENSE file
 */

/*
 * Headless tests for glm_persp_oblique_rh_no() and glm_persp_oblique_rh_zo().
 * Built directly by tests/run_oblique.sh with the system C compiler, no
 * windowing or test framework required.
 */

#include <cglm/cglm.h>
#include <cglm/clipspace/persp_rh_zo.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SENTINEL 9876.54321f

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
      g_failures++;                                                          \
      printf("FAIL (line %d): ", __LINE__);                                  \
      printf(__VA_ARGS__);                                                   \
      printf("\n");                                                          \
    }                                                                        \
  } while (0)

static bool
closef(float a, float b) {
  float diff = fabsf(a - b);
  float scale = fmaxf(1.0f, fmaxf(fabsf(a), fabsf(b)));
  return diff <= 1e-5f * scale;
}

static bool
mat4_close(CGLM_CONST mat4 a, CGLM_CONST mat4 b) {
  int c, r;
  for (c = 0; c < 4; c++)
    for (r = 0; r < 4; r++)
      if (!closef(a[c][r], b[c][r]))
        return false;
  return true;
}

/* rows x, y and w must survive; only the z row (m[*][2]) is rewritten */
static bool
rows_preserved(CGLM_CONST mat4 orig, CGLM_CONST mat4 oblique) {
  int c;
  for (c = 0; c < 4; c++) {
    if (!closef(oblique[c][0], orig[c][0])
        || !closef(oblique[c][1], orig[c][1])
        || !closef(oblique[c][3], orig[c][3]))
      return false;
  }
  return true;
}

static float
clip_ndc_z(CGLM_CONST mat4 m, float x, float y, float z) {
  vec4 eye = {x, y, z, 1.0f};
  vec4 clip;
  glm_mat4_mulv(m, eye, clip);
  return clip[2] / clip[3];
}

/*
 * The far corner q = inv(proj) * (s(a), s(b), 1, 1) must stay on the far
 * plane.  It is checked homogeneously (clip.z == clip.w): q.w is allowed
 * to be zero for infinite far planes.
 */
static bool
far_corner_kept(CGLM_CONST mat4 orig, CGLM_CONST mat4 oblique,
                const vec4 plane) {
  mat4 inv;
  vec4 corner, farPoint, clip;

  glm_mat4_inv(orig, inv);
  corner[0] = plane[0] < 0.0f ? -1.0f : 1.0f;
  corner[1] = plane[1] < 0.0f ? -1.0f : 1.0f;
  corner[2] = 1.0f;
  corner[3] = 1.0f;

  glm_mat4_mulv(inv, corner, farPoint);
  glm_mat4_mulv(oblique, farPoint, clip);

  return closef(clip[2], clip[3]);
}

static bool
call_oblique(bool zo, CGLM_CONST mat4 proj, const vec4 plane, mat4 dest) {
  return zo ? glm_persp_oblique_rh_zo(proj, plane, dest)
            : glm_persp_oblique_rh_no(proj, plane, dest);
}

static void
make_proj(mat4 proj, bool zo, bool infinite, bool asymmetric) {
  if (asymmetric) {
    if (zo)
      glm_frustum_rh_zo(-0.5f, 0.8f, -0.6f, 0.7f, 1.0f, 10.0f, proj);
    else
      glm_frustum_rh_no(-0.5f, 0.8f, -0.6f, 0.7f, 1.0f, 10.0f, proj);

    if (infinite) {
      /* limit of the frustum matrix as the far distance goes to infinity */
      proj[2][2] = -1.0f;
      proj[3][2] = zo ? -1.0f : -2.0f;
    }
  } else if (infinite) {
    if (zo)
      glm_perspective_infinite_rh_zo(glm_rad(90.0f), 1.0f, 1.0f, proj);
    else
      glm_perspective_infinite_rh_no(glm_rad(90.0f), 1.0f, 1.0f, proj);
  } else {
    if (zo)
      glm_perspective_rh_zo(glm_rad(90.0f), 1.0f, 1.0f, 10.0f, proj);
    else
      glm_perspective_rh_no(glm_rad(90.0f), 1.0f, 1.0f, 10.0f, proj);
  }
}

/*
 * Core property matrix: NO/ZO x finite/infinite x symmetric/asymmetric,
 * using the plane (0, 0, -1, -2) against a near=1 (far=10 when finite)
 * projection.
 */
static void
test_basic_matrix(bool zo, bool infinite, bool asymmetric) {
  mat4 proj, savedProj, oblique, scaledResult, aliased;
  vec4 plane    = {0.0f, 0.0f, -1.0f, -2.0f};
  vec4 savedPlane;
  vec4 scaledPlane;
  float nearBoundary = zo ? 0.0f : -1.0f;
  bool ok;
  mat4 inv;
  vec4 corner, farPoint;

  const char *tag = zo ? "ZO" : "NO";

  make_proj(proj, zo, infinite, asymmetric);
  glm_mat4_copy(proj, savedProj);
  glm_vec4_copy(plane, savedPlane);

  ok = call_oblique(zo, proj, plane, oblique);
  CHECK(ok, "%s inf=%d asym=%d: call succeeds",
        tag, infinite, asymmetric);

  /* inputs must not be modified */
  CHECK(memcmp(proj, savedProj, sizeof(mat4)) == 0,
        "%s inf=%d asym=%d: proj untouched", tag, infinite, asymmetric);
  CHECK(memcmp(plane, savedPlane, sizeof(vec4)) == 0,
        "%s inf=%d asym=%d: plane untouched", tag, infinite, asymmetric);

  CHECK(rows_preserved(proj, oblique),
        "%s inf=%d asym=%d: x/y/w rows preserved", tag, infinite, asymmetric);
  CHECK(far_corner_kept(proj, oblique, plane),
        "%s inf=%d asym=%d: far corner on z=w", tag, infinite, asymmetric);

  /* eye space z=-2 lies exactly on the new near plane */
  CHECK(closef(clip_ndc_z(oblique, 0.0f, 0.0f, -2.0f), nearBoundary),
        "%s inf=%d asym=%d: z=-2 maps to near boundary",
        tag, infinite, asymmetric);

  /* eye space z=-1 is behind the new near plane and gets clipped */
  CHECK(clip_ndc_z(oblique, 0.0f, 0.0f, -1.0f) < nearBoundary,
        "%s inf=%d asym=%d: z=-1 is clipped", tag, infinite, asymmetric);

  /* the far corner homogeneous weight behaves as advertised */
  glm_mat4_inv(proj, inv);
  corner[0] = 1.0f;
  corner[1] = 1.0f;
  corner[2] = 1.0f;
  corner[3] = 1.0f;
  glm_mat4_mulv(inv, corner, farPoint);
  if (infinite)
    CHECK(closef(farPoint[3], 0.0f),
          "%s asym=%d: infinite q.w is zero", tag, asymmetric);
  else
    CHECK(farPoint[3] > 0.0f,
          "%s asym=%d: finite q.w is positive", tag, asymmetric);

  /* scaling the plane by a positive factor must not change the result */
  glm_vec4_scale(plane, 2.5f, scaledPlane);
  ok = call_oblique(zo, proj, scaledPlane, scaledResult);
  CHECK(ok, "%s inf=%d asym=%d: scaled plane accepted",
        tag, infinite, asymmetric);
  CHECK(mat4_close(scaledResult, oblique),
        "%s inf=%d asym=%d: scaled plane identical", tag, infinite, asymmetric);

  /* dest == proj must work */
  glm_mat4_copy(proj, aliased);
  ok = call_oblique(zo, aliased, plane, aliased);
  CHECK(ok, "%s inf=%d asym=%d: aliased call accepted",
        tag, infinite, asymmetric);
  CHECK(mat4_close(aliased, oblique),
        "%s inf=%d asym=%d: aliased call identical", tag, infinite, asymmetric);
}

/* Hard-coded regression values for the symmetric near=1, far=10 case. */
static void
test_reference_numbers(void) {
  mat4 proj, oblique;
  vec4 plane = {0.0f, 0.0f, -1.0f, -2.0f};

  /* NO; cglm stores m[col][row], so element m{col}{row} lives at [col][row] */
  glm_perspective_rh_no(glm_rad(90.0f), 1.0f, 1.0f, 10.0f, proj);
  CHECK(closef(proj[0][0], 1.0f), "NO original m00");
  CHECK(closef(proj[1][1], 1.0f), "NO original m11");
  CHECK(closef(proj[2][3], -1.0f), "NO original m23");
  CHECK(closef(proj[3][2], -20.0f / 9.0f), "NO original m32");

  CHECK(glm_persp_oblique_rh_no(proj, plane, oblique), "NO reference call");
  CHECK(closef(oblique[0][2], 0.0f), "NO reference m20");
  CHECK(closef(oblique[1][2], 0.0f), "NO reference m21");
  CHECK(closef(oblique[2][2], -1.5f), "NO reference m22");
  CHECK(closef(oblique[3][2], -5.0f), "NO reference m32'");
  CHECK(closef(oblique[2][3], -1.0f), "NO original m23 retained");

  /* ZO */
  glm_perspective_rh_zo(glm_rad(90.0f), 1.0f, 1.0f, 10.0f, proj);
  CHECK(closef(proj[2][3], -1.0f), "ZO original m23");
  CHECK(closef(proj[3][2], -10.0f / 9.0f), "ZO original m32");
  CHECK(glm_persp_oblique_rh_zo(proj, plane, oblique), "ZO reference call");
  CHECK(closef(oblique[2][2], -1.25f), "ZO reference m22");
  CHECK(closef(oblique[3][2], -2.5f), "ZO reference m32'");
  CHECK(closef(oblique[2][3], -1.0f), "ZO original m23 retained");

  /* infinite reference values */
  glm_perspective_infinite_rh_no(glm_rad(90.0f), 1.0f, 1.0f, proj);
  CHECK(closef(proj[3][2], -2.0f), "NO infinite original m32");
  CHECK(glm_persp_oblique_rh_no(proj, plane, oblique),
        "NO infinite reference call");
  CHECK(closef(oblique[2][2], -1.0f), "NO infinite reference m22");
  CHECK(closef(oblique[3][2], -4.0f), "NO infinite reference m32'");

  glm_perspective_infinite_rh_zo(glm_rad(90.0f), 1.0f, 1.0f, proj);
  CHECK(closef(proj[3][2], -1.0f), "ZO infinite original m32");
  CHECK(glm_persp_oblique_rh_zo(proj, plane, oblique),
        "ZO infinite reference call");
  CHECK(closef(oblique[3][2], -2.0f), "ZO infinite reference m32'");
}

/*
 * A plane with x/y components on an asymmetric frustum: three non-collinear
 * points on the plane, one point on each side, and the untouched rows.
 */
static void
test_tilted_plane(bool zo, bool infinite) {
  mat4 proj, oblique;
  /* 0.3x - 0.2y - z - 1.5 = 0, i.e. z = 0.3x - 0.2y - 1.5 */
  vec4 plane = {0.3f, -0.2f, -1.0f, -1.5f};
  vec3 onPlane[3] = {
    {0.0f, 0.0f, -1.5f},
    {2.0f, 0.0f, -0.9f},
    {0.0f, 2.0f, -1.9f}
  };
  float nearBoundary = zo ? 0.0f : -1.0f;
  int i;
  const char *tag = zo ? "ZO" : "NO";

  make_proj(proj, zo, infinite, true);
  CHECK(call_oblique(zo, proj, plane, oblique),
        "%s tilted inf=%d: call succeeds", tag, infinite);
  CHECK(rows_preserved(proj, oblique),
        "%s tilted inf=%d: rows preserved", tag, infinite);

  for (i = 0; i < 3; i++) {
    CHECK(closef(clip_ndc_z(oblique, onPlane[i][0], onPlane[i][1],
                            onPlane[i][2]), nearBoundary),
          "%s tilted inf=%d: plane point %d on near boundary",
          tag, infinite, i);
  }

  /* (0,0,-2): plane value +0.5, kept; (0,0,-1): plane value -0.5, culled */
  CHECK(clip_ndc_z(oblique, 0.0f, 0.0f, -2.0f) > nearBoundary,
        "%s tilted inf=%d: kept-side point survives", tag, infinite);
  CHECK(clip_ndc_z(oblique, 0.0f, 0.0f, -1.0f) < nearBoundary,
        "%s tilted inf=%d: culled-side point rejected", tag, infinite);

  CHECK(far_corner_kept(proj, oblique, plane),
        "%s tilted inf=%d: far corner on z=w", tag, infinite);
}

/* Every failure path must leave the destination bytes exactly as they were. */
static void
expect_failure(bool zo, CGLM_CONST mat4 proj, const vec4 plane,
               const char *name) {
  mat4 sentinel, dest;
  mat4 savedProj;
  vec4 savedPlane;
  int c, r;

  for (c = 0; c < 4; c++)
    for (r = 0; r < 4; r++)
      sentinel[c][r] = SENTINEL;

  memcpy(dest, sentinel, sizeof(sentinel));
  glm_mat4_copy(proj, savedProj);
  glm_vec4_copy(plane, savedPlane);

  CHECK(!call_oblique(zo, proj, plane, dest), "%s: rejected", name);
  CHECK(memcmp(dest, sentinel, sizeof(sentinel)) == 0,
        "%s: destination sentinel untouched", name);
  CHECK(memcmp(proj, savedProj, sizeof(mat4)) == 0,
        "%s: proj untouched", name);
  CHECK(memcmp(plane, savedPlane, sizeof(vec4)) == 0,
        "%s: plane untouched", name);
}

static void
test_failures(void) {
  mat4 proj, singular, nonFinite;
  vec4 plane = {0.0f, 0.0f, -1.0f, -2.0f};
  vec4 nanPlane = {0.0f, 0.0f, -1.0f, NAN};
  vec4 zeroNormal = {0.0f, 0.0f, 0.0f, -2.0f};
  vec4 dZero = {0.0f, 0.0f, -1.0f, 0.0f};
  vec4 dPositive = {0.0f, 0.0f, -1.0f, 0.5f};
  vec4 facingAway = {0.0f, 0.0f, 1.0f, -2.0f};

  glm_perspective_rh_no(glm_rad(90.0f), 1.0f, 1.0f, 10.0f, proj);

  expect_failure(false, proj, nanPlane, "NO non-finite plane component");
  expect_failure(true,  proj, nanPlane, "ZO non-finite plane component");

  glm_mat4_copy(proj, nonFinite);
  nonFinite[2][2] = INFINITY;
  expect_failure(false, nonFinite, plane, "non-finite matrix component");

  glm_mat4_zero(singular);
  expect_failure(false, singular, plane, "singular matrix");

  expect_failure(false, proj, zeroNormal, "zero-length normal");
  expect_failure(false, proj, dZero, "d equal to zero");
  expect_failure(false, proj, dPositive, "positive d");
  expect_failure(false, proj, facingAway, "plane.q dot not positive");

  /* failure with dest == proj: the projection itself must survive intact */
  {
    mat4 aliased, original;
    glm_mat4_copy(proj, original);
    glm_mat4_copy(proj, aliased);
    CHECK(!glm_persp_oblique_rh_no(aliased, zeroNormal, aliased),
          "aliased zero-normal call rejected");
    CHECK(memcmp(aliased, original, sizeof(mat4)) == 0,
          "aliased failure leaves proj untouched");
  }

  /* sign rule at exactly zero: a zero x/y component must select +1 */
  {
    mat4 oblique;
    vec4 axisPlane = {0.0f, 0.0f, -1.0f, -1.5f};
    CHECK(glm_persp_oblique_rh_no(proj, axisPlane, oblique),
          "plane with zero x/y components accepted");
    CHECK(closef(oblique[0][2], 0.0f) && closef(oblique[1][2], 0.0f),
          "zero x/y components give zero x/y z-row entries");
  }
}

int
main(void) {
  int zo, infinite, asymmetric;

  for (zo = 0; zo <= 1; zo++)
    for (infinite = 0; infinite <= 1; infinite++)
      for (asymmetric = 0; asymmetric <= 1; asymmetric++)
        test_basic_matrix(zo != 0, infinite != 0, asymmetric != 0);

  test_reference_numbers();

  for (zo = 0; zo <= 1; zo++) {
    test_tilted_plane(zo != 0, false);
    test_tilted_plane(zo != 0, true);
  }

  test_failures();

  printf("oblique tests: %d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
