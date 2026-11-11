/*
 * Contract tests for the spherical-triangulation interpolation API.
 *
 * Usage: spherical_contract <geometry|audio|lifetime|malformed>
 *
 * The expected weights in the geometry group are derived analytically from
 * regular polyhedra and from an independent ray/triangle reconstruction;
 * they are never produced by the implementation under test.
 */

#include "mysofa.h"
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *group_name = "?";
static unsigned cases = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    cases++;                                                                   \
    if (!(cond)) {                                                             \
      printf("FAIL %s: ", group_name);                                         \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static void near_value(double got, double want, double tol, const char *what) {
  cases++;
  if (!(fabs(got - want) <= tol)) {
    printf("FAIL %s: %s got=%.12g want=%.12g tol=%.12g\n", group_name, what,
           got, want, tol);
    exit(1);
  }
}

static struct MYSOFA_ATTRIBUTE CARTESIAN = {NULL, (char *)"Type",
                                            (char *)"cartesian"};
static struct MYSOFA_ATTRIBUTE SPHERICAL = {NULL, (char *)"Type",
                                            (char *)"spherical"};

static void build_hrtf(struct MYSOFA_HRTF *h, unsigned M, unsigned N,
                       float *pos, float *ir, float *delay,
                       unsigned delay_elements) {
  memset(h, 0, sizeof(*h));
  h->I = 1;
  h->C = 3;
  h->R = 2;
  h->E = 1;
  h->N = N;
  h->M = M;
  h->SourcePosition.values = pos;
  h->SourcePosition.elements = M * 3;
  h->SourcePosition.attributes = &CARTESIAN;
  h->DataIR.values = ir;
  h->DataIR.elements = M * 2 * N;
  h->DataDelay.values = delay;
  h->DataDelay.elements = delay_elements;
}

static int get_dir(MYSOFA_SPHERICAL *s, double x, double y, double z,
                   float *left, float *right, float *delay, unsigned *idx,
                   float *w) {
  float dir[3];
  dir[0] = (float)x;
  dir[1] = (float)y;
  dir[2] = (float)z;
  return mysofa_spherical_getfilter(s, dir, left, right, delay, idx, w);
}

static void check_weights(const unsigned idx[3], const float w[3],
                          const char *what) {
  double sum = 0.0;
  int k;
  CHECK(idx[0] != idx[1] && idx[0] != idx[2] && idx[1] != idx[2],
        "%s: duplicate indices %u %u %u", what, idx[0], idx[1], idx[2]);
  for (k = 0; k < 3; k++) {
    CHECK(w[k] >= -1e-5f, "%s: negative weight %g", what, (double)w[k]);
    sum += w[k];
  }
  CHECK(fabs(sum - 1.0) <= 1e-5, "%s: weights sum %.12g", what, sum);
}

static double weight_of(const unsigned idx[3], const float w[3],
                        unsigned label) {
  int k;
  for (k = 0; k < 3; k++)
    if (idx[k] == label)
      return w[k];
  return 0.0;
}

/*
 * Compare one three-vertex result against analytic labels/weights.  `lab`
 * names the original measurement indices and `want` gives their weights.
 */
static void expect_triple(const unsigned idx[3], const float w[3],
                          const int lab[3], const double want[3], double tol,
                          const char *what) {
  int k, m;
  check_weights(idx, w, what);
  for (k = 0; k < 3; k++) {
    int found = -1;
    for (m = 0; m < 3; m++)
      if ((int)idx[m] == lab[k])
        found = m;
    CHECK(found >= 0, "%s: expected index %d missing (got %u %u %u)", what,
          lab[k], idx[0], idx[1], idx[2]);
    near_value(w[found], want[k], tol, what);
  }
}

static void expect_vertex(MYSOFA_SPHERICAL *s, const float pos[3],
                          unsigned vertex, const char *what) {
  float left[16], right[16], delay[2], w[3];
  unsigned idx[3];
  int ret = get_dir(s, pos[0], pos[1], pos[2], left, right, delay, idx, w);
  CHECK(ret == MYSOFA_OK, "%s: getfilter returned %d", what, ret);
  {
    int k, found = -1;
    check_weights(idx, w, what);
    for (k = 0; k < 3; k++)
      if (idx[k] == vertex)
        found = k;
    CHECK(found >= 0, "%s: vertex %u not returned", what, vertex);
    near_value(w[found], 1.0, 1e-5, what);
  }
}

/* ------------------------------------------------------------------ */
/* geometry                                                            */
/* ------------------------------------------------------------------ */

static void normalize3(double v[3]) {
  double r = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  v[0] /= r;
  v[1] /= r;
  v[2] /= r;
}

static void unit_of(const float p[3], double out[3]) {
  out[0] = p[0];
  out[1] = p[1];
  out[2] = p[2];
  normalize3(out);
}

/*
 * Independent ray/triangle intersection used by the general point-set check.
 * Returns 1 when the ray hits the triangle with non-negative barycentrics.
 */
static int ray_tri(const double a[3], const double b[3], const double c[3],
                   const double ray[3], double *wa, double *wb, double *wc,
                   double *tout) {
  double u[3], v[3], n[3], nn, d, den, t, p[3];
  double d00, d01, d11, d20, d21, det, uu, vv;
  int k;
  for (k = 0; k < 3; k++) {
    u[k] = b[k] - a[k];
    v[k] = c[k] - a[k];
  }
  n[0] = u[1] * v[2] - u[2] * v[1];
  n[1] = u[2] * v[0] - u[0] * v[2];
  n[2] = u[0] * v[1] - u[1] * v[0];
  nn = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (nn < 1e-14)
    return 0;
  for (k = 0; k < 3; k++)
    n[k] /= nn;
  d = n[0] * a[0] + n[1] * a[1] + n[2] * a[2];
  den = n[0] * ray[0] + n[1] * ray[1] + n[2] * ray[2];
  if (fabs(den) <= 1e-12)
    return 0;
  t = d / den;
  if (!(t > 0) || !isfinite(t))
    return 0;
  for (k = 0; k < 3; k++)
    p[k] = t * ray[k];
  d00 = u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
  d01 = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
  d11 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  d20 = (p[0] - a[0]) * u[0] + (p[1] - a[1]) * u[1] + (p[2] - a[2]) * u[2];
  d21 = (p[0] - a[0]) * v[0] + (p[1] - a[1]) * v[1] + (p[2] - a[2]) * v[2];
  det = d00 * d11 - d01 * d01;
  if (det <= 1e-18)
    return 0;
  uu = (d11 * d20 - d01 * d21) / det;
  vv = (d00 * d21 - d01 * d20) / det;
  *wa = 1.0 - uu - vv;
  *wb = uu;
  *wc = vv;
  *tout = t;
  return (*wa >= -1e-6 && *wb >= -1e-6 && *wc >= -1e-6);
}

static void geometry_tetra(void) {
  float a = (float)(1.0 / sqrt(3.0));
  float pos[12] = {a, a, a, a, -a, -a, -a, a, -a, -a, -a, a};
  float ir[8] = {0};
  float delay[2] = {0, 0};
  struct MYSOFA_HRTF h;
  MYSOFA_SPHERICAL *s;
  int err = 0;
  int t;

  build_hrtf(&h, 4, 1, pos, ir, delay, 2);
  s = mysofa_spherical_init(&h, &err);
  CHECK(s != NULL, "tetra init failed err=%d", err);

  for (t = 0; t < 4; t++)
    expect_vertex(s, pos + t * 3, (unsigned)t, "tetra vertex");

  /* every triple of a regular tetrahedron is one face; its centroid has
   * barycentric weights 1/3, 1/3, 1/3. */
  {
    int triples[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (t = 0; t < 4; t++) {
      double d[3];
      float left[1], right[1], del[2], w[3];
      unsigned idx[3];
      int ret;
      int k;
      d[0] = d[1] = d[2] = 0.0;
      for (k = 0; k < 3; k++) {
        d[0] += pos[triples[t][k] * 3 + 0];
        d[1] += pos[triples[t][k] * 3 + 1];
        d[2] += pos[triples[t][k] * 3 + 2];
      }
      normalize3(d);
      ret = get_dir(s, d[0], d[1], d[2], left, right, del, idx, w);
      CHECK(ret == MYSOFA_OK, "tetra centroid ret=%d", ret);
      {
        double want[3] = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
        expect_triple(idx, w, triples[t], want, 1e-5, "tetra centroid");
      }
    }
  }

  mysofa_spherical_free(s);
}

static void geometry_octa(void) {
  float pos[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1};
  float ir[12] = {0};
  float delay[2] = {0, 0};
  struct MYSOFA_HRTF h;
  MYSOFA_SPHERICAL *s;
  int err = 0;

  build_hrtf(&h, 6, 1, pos, ir, delay, 2);
  s = mysofa_spherical_init(&h, &err);
  CHECK(s != NULL, "octa init failed err=%d", err);

  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    int lab[3] = {0, 2, 4};
    double want[3] = {0.5, 0.25, 0.25};
    CHECK(get_dir(s, 2, 1, 1, l, r, d, idx, w) == MYSOFA_OK, "octa 2,1,1");
    expect_triple(idx, w, lab, want, 1e-5, "octa (2,1,1)");
  }
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    int lab[3] = {0, 2, 5};
    double want[3] = {0.25, 0.25, 0.5};
    CHECK(get_dir(s, 1, 1, -2, l, r, d, idx, w) == MYSOFA_OK, "octa 1,1,-2");
    expect_triple(idx, w, lab, want, 1e-5, "octa (1,1,-2)");
  }
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    int lab[3] = {1, 2, 4};
    double want[3] = {0.5, 0.25, 0.25};
    CHECK(get_dir(s, -2, 1, 1, l, r, d, idx, w) == MYSOFA_OK, "octa -2,1,1");
    expect_triple(idx, w, lab, want, 1e-5, "octa (-2,1,1)");
  }

  /* approach a shared edge from both sides */
  {
    float l1[1], r1[1], d1[2], w1[3], l2[1], r2[1], d2[2], w2[3];
    unsigned i1[3], i2[3];
    double eps = 1e-4;
    double t1 = 1.0 / (2.0 + eps);
    double t2 = 1.0 / (2.0 + eps);
    CHECK(get_dir(s, 1, 1, eps, l1, r1, d1, i1, w1) == MYSOFA_OK,
          "octa edge +eps");
    CHECK(get_dir(s, 1, 1, -eps, l2, r2, d2, i2, w2) == MYSOFA_OK,
          "octa edge -eps");
    {
      int labA[3] = {0, 2, 4};
      double wantA[3] = {t1, t1, eps * t1};
      int labB[3] = {0, 2, 5};
      double wantB[3] = {t2, t2, eps * t2};
      expect_triple(i1, w1, labA, wantA, 1e-5, "octa edge +eps");
      expect_triple(i2, w2, labB, wantB, 1e-5, "octa edge -eps");
    }
    cases++;
    if (!(fabs(weight_of(i1, w1, 0) - weight_of(i2, w2, 0)) < 1e-3 &&
          fabs(weight_of(i1, w1, 2) - weight_of(i2, w2, 2)) < 1e-3))
      CHECK(0, "octa edge continuity");
  }

  /* pole: exact vertex and a direction approaching it */
  expect_vertex(s, pos + 4 * 3, 4, "octa pole vertex");
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    double eps = 1e-4;
    CHECK(get_dir(s, eps, 0, 1, l, r, d, idx, w) == MYSOFA_OK,
          "octa near pole");
    {
      int lab[3] = {0, 2, 4};
      double want[3] = {eps / (1.0 + eps), 0.0, 1.0 / (1.0 + eps)};
      expect_triple(idx, w, lab, want, 1e-5, "octa near pole");
    }
  }

  /* seam at azimuth pi: the two sides are analytically known */
  {
    float la[1], ra[1], da[2], wa[3], lb[1], rb[1], db[2], wb[3];
    unsigned ia[3], ib[3];
    double eps = 1e-4;
    double tp = 1.0 / (2.0 + eps);
    double tm = 1.0 / (2.0 + eps);
    CHECK(get_dir(s, -1, eps, 1, la, ra, da, ia, wa) == MYSOFA_OK,
          "octa seam +");
    CHECK(get_dir(s, -1, -eps, 1, lb, rb, db, ib, wb) == MYSOFA_OK,
          "octa seam -");
    {
      int labP[3] = {1, 2, 4};
      double wantP[3] = {tp, eps * tp, tp};
      int labM[3] = {1, 3, 4};
      double wantM[3] = {tm, eps * tm, tm};
      expect_triple(ia, wa, labP, wantP, 1e-5, "octa seam +");
      expect_triple(ib, wb, labM, wantM, 1e-5, "octa seam -");
    }
  }

  mysofa_spherical_free(s);
}

static void geometry_cube(void) {
  float a = (float)(1.0 / sqrt(3.0));
  float pos[24] = {a,  a,  a,  a,  a,  -a, a,  -a, a,  a,  -a, -a,
                   -a, a,  a,  -a, a,  -a, -a, -a, a,  -a, -a, -a};
  float ir[16] = {0};
  float delay[2] = {0, 0};
  struct MYSOFA_HRTF h;
  MYSOFA_SPHERICAL *s;
  int err = 0;

  build_hrtf(&h, 8, 1, pos, ir, delay, 2);
  s = mysofa_spherical_init(&h, &err);
  CHECK(s != NULL, "cube init failed err=%d", err);

  /* Face x=+a.  Lexicographically smallest vertex is index 3 = (a,-a,-a);
   * its fan diagonal ends at index 0 = (a,a,a). */
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    int lab[3] = {3, 0, 1}; /* third vertex gets weight zero at face center */
    double want[3] = {0.5, 0.5, 0.0};
    CHECK(get_dir(s, 1, 0, 0, l, r, d, idx, w) == MYSOFA_OK,
          "cube face center");
    expect_triple(idx, w, lab, want, 1e-5, "cube face center");
  }
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    /* direction (1, 0.5, 0) hits (a, 0.5a, 0) on the lower fan triangle */
    int lab[3] = {3, 1, 0};
    double want[3] = {0.25, 0.25, 0.5};
    CHECK(get_dir(s, 1, 0.5f, 0, l, r, d, idx, w) == MYSOFA_OK,
          "cube off diagonal");
    expect_triple(idx, w, lab, want, 1e-5, "cube off diagonal");
  }
  /* Face x=-a: smallest vertex is index 7 = (-a,-a,-a); opposite is
   * index 4 = (-a,a,a). */
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    int lab[3] = {7, 5, 4};
    double want[3] = {0.25, 0.25, 0.5};
    CHECK(get_dir(s, -1, 0.5f, 0, l, r, d, idx, w) == MYSOFA_OK,
          "cube opposite face");
    expect_triple(idx, w, lab, want, 1e-5, "cube opposite face");
  }
  /* moving along the fan diagonal gives linear weights */
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    int lab[3] = {3, 0, 1};
    double want[3] = {0.35, 0.65, 0.0};
    CHECK(get_dir(s, 1, 0.3f, 0.3f, l, r, d, idx, w) == MYSOFA_OK,
          "cube diagonal");
    expect_triple(idx, w, lab, want, 1e-5, "cube diagonal");
  }
  /* crossing the diagonal: both sides are continuous */
  {
    float l1[1], r1[1], d1[2], w1[3], l2[1], r2[1], d2[2], w2[3];
    unsigned i1[3], i2[3];
    int k;
    CHECK(get_dir(s, 1, 0.2f, 0.2f + 1e-4f, l1, r1, d1, i1, w1) == MYSOFA_OK,
          "cube diagonal +");
    CHECK(get_dir(s, 1, 0.2f, 0.2f - 1e-4f, l2, r2, d2, i2, w2) == MYSOFA_OK,
          "cube diagonal -");
    for (k = 0; k < 3; k++) {
      cases++;
      /* both fan triangles share the root (3) and opposite corner (0) */
      if (fabs(weight_of(i1, w1, 3) - weight_of(i2, w2, 3)) > 2e-3 ||
          fabs(weight_of(i1, w1, 0) - weight_of(i2, w2, 0)) > 2e-3) {
        printf("FAIL %s: cube diagonal discontinuity\n", group_name);
        exit(1);
      }
    }
  }

  mysofa_spherical_free(s);
}

static void geometry_reorder(void) {
  /* Use the octahedron and reverse the measurement records.  The weights,
   * expressed by original label, must not change. */
  float pos[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1};
  float rev[18];
  float ir[12] = {0};
  float delay[2] = {0, 0};
  struct MYSOFA_HRTF h;
  MYSOFA_SPHERICAL *s;
  int err = 0;
  unsigned i;

  for (i = 0; i < 6; i++) {
    rev[i * 3 + 0] = pos[(5 - i) * 3 + 0];
    rev[i * 3 + 1] = pos[(5 - i) * 3 + 1];
    rev[i * 3 + 2] = pos[(5 - i) * 3 + 2];
  }
  build_hrtf(&h, 6, 1, rev, ir, delay, 2);
  s = mysofa_spherical_init(&h, &err);
  CHECK(s != NULL, "reordered octa init failed err=%d", err);

  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    int lab[3];
    double want[3] = {0.5, 0.25, 0.25};
    CHECK(get_dir(s, 2, 1, 1, l, r, d, idx, w) == MYSOFA_OK,
          "reordered octa query");
    lab[0] = 5 - 0; /* original 0 -> reversed 5 */
    lab[1] = 5 - 2; /* original 2 -> reversed 3 */
    lab[2] = 5 - 4; /* original 4 -> reversed 1 */
    expect_triple(idx, w, lab, want, 1e-5, "reordered octa");
  }

  mysofa_spherical_free(s);

  /* Coplanar cube face after reordering.  Face x=+a consisted of original
   * indices 0..3; after reversal they are 7..4.  The fan root is still the
   * lexicographically smallest direction (original 3, now reversed 4). */
  {
    float a = (float)(1.0 / sqrt(3.0));
    float cpos[24] = {a,  a,  a,  a,  a,  -a, a,  -a, a,  a,  -a, -a,
                      -a, a,  a,  -a, a,  -a, -a, -a, a,  -a, -a, -a};
    float crev[24];
    float cir[16] = {0};
    int lab[3];
    double want[3] = {0.5, 0.5, 0.0};
    for (i = 0; i < 8; i++) {
      crev[i * 3 + 0] = cpos[(7 - i) * 3 + 0];
      crev[i * 3 + 1] = cpos[(7 - i) * 3 + 1];
      crev[i * 3 + 2] = cpos[(7 - i) * 3 + 2];
    }
    build_hrtf(&h, 8, 1, crev, cir, delay, 2);
    s = mysofa_spherical_init(&h, &err);
    CHECK(s != NULL, "reordered cube init failed err=%d", err);
    {
      float l[1], r[1], d[2], w[3];
      unsigned idx[3];
      CHECK(get_dir(s, 1, 0, 0, l, r, d, idx, w) == MYSOFA_OK,
            "reordered cube query");
      lab[0] = 7 - 3; /* original 3 -> reversed 4 (root) */
      lab[1] = 7 - 0; /* original 0 -> reversed 7 (opposite) */
      lab[2] = 7 - 1; /* original 1 -> reversed 6 */
      expect_triple(idx, w, lab, want, 1e-5, "reordered cube");
    }
    mysofa_spherical_free(s);
  }
}

static void geometry_asymmetric(void) {
  /* The positions are normalized below; all have the same radius. */
  float pos[24] = {0.0f,   0.0f,   1.0f,   0.1f,   -0.05f, -0.99f,
                   1.0f,   0.05f,  0.02f,  -0.98f, 0.1f,   -0.1f,
                   0.4f,   0.85f,  0.2f,   -0.3f,  -0.9f,  0.15f,
                   0.2f,   -0.2f,  0.96f,  -0.5f,  0.4f,   -0.75f};
  float ir[16] = {0};
  float delay[2] = {0, 0};
  struct MYSOFA_HRTF h;
  MYSOFA_SPHERICAL *s;
  double P[8][3];
  int err = 0;
  unsigned i, a, b, c;

  for (i = 0; i < 8; i++) {
    unit_of(pos + i * 3, P[i]);
    pos[i * 3 + 0] = (float)P[i][0];
    pos[i * 3 + 1] = (float)P[i][1];
    pos[i * 3 + 2] = (float)P[i][2];
  }

  build_hrtf(&h, 8, 1, pos, ir, delay, 2);
  s = mysofa_spherical_init(&h, &err);
  CHECK(s != NULL, "asymmetric init failed err=%d", err);

  /* A generic direction must be reconstructed from the returned triangle. */
  {
    double ray[3] = {0.3, -0.7, 0.5};
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    double wa, wb, wc, t;
    double found_t = -1.0;
    int matches = 0, best_tri[3] = {-1, -1, -1};
    double best_w[3] = {0, 0, 0};
    int k;
    normalize3(ray);
    CHECK(get_dir(s, ray[0], ray[1], ray[2], l, r, d, idx, w) == MYSOFA_OK,
          "asymmetric generic query");
    check_weights(idx, w, "asymmetric generic");
    /*
     * Enumerate all triangles and keep those that are supporting planes and
     * that the ray actually hits.  The boundary hit is the one with the
     * largest ray parameter.
     */
    for (a = 0; a < 8; a++)
      for (b = a + 1; b < 8; b++)
        for (c = b + 1; c < 8; c++) {
          double u[3], v[3], n[3], nn, pl;
          int above = 1, below = 1;
          for (k = 0; k < 3; k++) {
            u[k] = P[b][k] - P[a][k];
            v[k] = P[c][k] - P[a][k];
          }
          n[0] = u[1] * v[2] - u[2] * v[1];
          n[1] = u[2] * v[0] - u[0] * v[2];
          n[2] = u[0] * v[1] - u[1] * v[0];
          nn = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          if (nn < 1e-12)
            continue;
          for (k = 0; k < 3; k++)
            n[k] /= nn;
          pl = n[0] * P[a][0] + n[1] * P[a][1] + n[2] * P[a][2];
          for (i = 0; i < 8; i++) {
            double s = n[0] * P[i][0] + n[1] * P[i][1] + n[2] * P[i][2];
            if (s - pl > 1e-7)
              below = 0;
            if (s - pl < -1e-7)
              above = 0;
          }
          if (!above && !below)
            continue;
          if (ray_tri(P[a], P[b], P[c], ray, &wa, &wb, &wc, &t)) {
            if (t > found_t + 1e-12) {
              found_t = t;
              matches = 1;
              best_tri[0] = (int)a;
              best_tri[1] = (int)b;
              best_tri[2] = (int)c;
              best_w[0] = wa;
              best_w[1] = wb;
              best_w[2] = wc;
            } else if (fabs(t - found_t) <= 1e-12) {
              matches++;
            }
          }
        }
    CHECK(matches >= 1, "asymmetric reconstruction found no triangle");
    {
      int lab[3];
      int got = 0;
      for (k = 0; k < 3; k++) {
        int m;
        for (m = 0; m < 3; m++)
          if ((int)idx[m] == best_tri[k])
            got++;
      }
      CHECK(got == 3, "asymmetric returned triangle does not match ray hit");
      (void)got;
      lab[0] = best_tri[0];
      lab[1] = best_tri[1];
      lab[2] = best_tri[2];
      expect_triple(idx, w, lab, best_w, 1e-5, "asymmetric reconstruction");
    }
    /* verify the intersection point is the barycentric combination */
    {
      double p[3], q[3];
      for (k = 0; k < 3; k++) {
        p[k] = found_t * ray[k];
        q[k] = best_w[0] * P[best_tri[0]][k] +
               best_w[1] * P[best_tri[1]][k] +
               best_w[2] * P[best_tri[2]][k];
        near_value(q[k], p[k], 1e-6, "asymmetric ray reconstruction");
      }
    }
  }

  /* exact pole */
  expect_vertex(s, pos + 0, 0, "asymmetric north pole");

  /* near the pole */
  {
    float l[1], r[1], d[2], w[3];
    unsigned idx[3];
    CHECK(get_dir(s, 1e-4, 0, 1, l, r, d, idx, w) == MYSOFA_OK,
          "asymmetric near pole");
    check_weights(idx, w, "asymmetric near pole");
  }

  /* seam around azimuth pi: two nearby directions on either side */
  {
    float l1[1], r1[1], d1[2], w1[3], l2[1], r2[1], d2[2], w2[3];
    unsigned i1[3], i2[3];
    int k;
    CHECK(get_dir(s, -1, 1e-4, 0.2, l1, r1, d1, i1, w1) == MYSOFA_OK,
          "asymmetric seam +");
    CHECK(get_dir(s, -1, -1e-4, 0.2, l2, r2, d2, i2, w2) == MYSOFA_OK,
          "asymmetric seam -");
    for (k = 0; k < 8; k++) {
      cases++;
      if (fabs(weight_of(i1, w1, (unsigned)k) -
               weight_of(i2, w2, (unsigned)k)) > 2e-3) {
        printf("FAIL %s: asymmetric seam discontinuity\n", group_name);
        exit(1);
      }
    }
  }

  mysofa_spherical_free(s);
}

static void run_geometry(void) {
  geometry_tetra();
  geometry_octa();
  geometry_cube();
  geometry_reorder();
  geometry_asymmetric();
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */

#define AUDIO_N 5

static void audio_case(int per_measurement) {
  float pos[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1};
  float ir[6 * 2 * AUDIO_N];
  float delay[6 * 2];
  float global_delay[2] = {0.75f, 1.25f};
  struct MYSOFA_HRTF h;
  MYSOFA_SPHERICAL *s;
  int err = 0;
  unsigned m, c;
  float maxabs = 0.0f;

  memset(ir, 0, sizeof(ir));
  for (m = 0; m < 6; m++) {
    /* deliberately sparse and different for the two ears */
    ir[m * 2 * AUDIO_N + (m % AUDIO_N)] = (float)(m + 1) * 1.5f;
    ir[m * 2 * AUDIO_N + AUDIO_N + ((m + 3) % AUDIO_N)] =
        (float)(m + 1) * 0.75f;
    delay[m * 2 + 0] = (float)m * 0.37f + 0.1f;
    delay[m * 2 + 1] = (float)m * 0.41f + 0.2f;
  }
  for (m = 0; m < 6 * 2 * AUDIO_N; m++)
    if (fabsf(ir[m]) > maxabs)
      maxabs = fabsf(ir[m]);
  for (m = 0; m < 12; m++)
    if (fabsf(delay[m]) > maxabs)
      maxabs = fabsf(delay[m]);
  {
    float scale = maxabs > 0.0f ? maxabs : 1.0f;
    double reltol = 2e-5 * (double)scale;

    build_hrtf(&h, 6, AUDIO_N, pos, ir,
               per_measurement ? delay : global_delay,
               per_measurement ? 12u : 2u);
    s = mysofa_spherical_init(&h, &err);
    CHECK(s != NULL, "audio init failed err=%d", err);

    {
      struct {
        double x, y, z;
        int lab[3];
        double w[3];
      } dirs[3] = {{2, 1, 1, {0, 2, 4}, {0.5, 0.25, 0.25}},
                   {-2, 1, 1, {1, 2, 4}, {0.5, 0.25, 0.25}},
                   {1, 1, -2, {0, 2, 5}, {0.25, 0.25, 0.5}}};
      int q;
      for (q = 0; q < 3; q++) {
        float left[AUDIO_N], right[AUDIO_N], del[2], w[3];
        unsigned idx[3];
        int k, ch;
        CHECK(get_dir(s, dirs[q].x, dirs[q].y, dirs[q].z, left, right, del,
                      idx, w) == MYSOFA_OK,
              "audio query %d", q);
        check_weights(idx, w, "audio query");
        for (ch = 0; ch < 2; ch++) {
          for (c = 0; c < AUDIO_N; c++) {
            double want = 0.0;
            for (k = 0; k < 3; k++) {
              const float *fir = ir + (size_t)dirs[q].lab[k] * 2 * AUDIO_N;
              want += dirs[q].w[k] * fir[ch * AUDIO_N + c];
            }
            cases++;
            if (fabs((ch == 0 ? left[c] : right[c]) - want) > reltol) {
              printf("FAIL %s: audio q=%d ch=%d c=%u got=%.9g want=%.9g\n",
                     group_name, q, ch, c,
                     (double)(ch == 0 ? left[c] : right[c]), want);
              exit(1);
            }
          }
        }
        if (per_measurement) {
          for (ch = 0; ch < 2; ch++) {
            double want = 0.0;
            for (k = 0; k < 3; k++)
              want += dirs[q].w[k] *
                      (double)delay[dirs[q].lab[k] * 2 + ch];
            cases++;
            if (fabs((double)del[ch] - want) > reltol) {
              printf("FAIL %s: per-measurement delay q=%d ch=%d got=%.9g "
                     "want=%.9g\n",
                     group_name, q, ch, (double)del[ch], want);
              exit(1);
            }
          }
        } else {
          cases++;
          if (fabs((double)del[0] - (double)global_delay[0]) > reltol ||
              fabs((double)del[1] - (double)global_delay[1]) > reltol) {
            printf("FAIL %s: global delay changed %.9g %.9g\n", group_name,
                   (double)del[0], (double)del[1]);
            exit(1);
          }
        }
      }
    }

    /* exact measurement vertex must reproduce the stored FIR and delay */
    {
      float left[AUDIO_N], right[AUDIO_N], del[2], w[3];
      unsigned idx[3];
      unsigned v;
      for (v = 0; v < 6; v++) {
        int ret = get_dir(s, pos[v * 3], pos[v * 3 + 1], pos[v * 3 + 2],
                          left, right, del, idx, w);
        CHECK(ret == MYSOFA_OK, "audio vertex query");
        for (c = 0; c < AUDIO_N; c++) {
          CHECK(left[c] == ir[v * 2 * AUDIO_N + c],
                "audio vertex left differs");
          CHECK(right[c] == ir[v * 2 * AUDIO_N + AUDIO_N + c],
                "audio vertex right differs");
        }
        if (per_measurement) {
          CHECK(del[0] == delay[v * 2 + 0], "audio vertex delay left");
          CHECK(del[1] == delay[v * 2 + 1], "audio vertex delay right");
        } else {
          CHECK(del[0] == global_delay[0] && del[1] == global_delay[1],
                "audio vertex global delay");
        }
      }
    }
    mysofa_spherical_free(s);
  }
}

static void run_audio(void) {
  audio_case(1);
  audio_case(0);
}

/* ------------------------------------------------------------------ */
/* lifetime                                                            */
/* ------------------------------------------------------------------ */

static struct MYSOFA_ATTRIBUTE *heap_attr(const char *name,
                                          const char *value) {
  struct MYSOFA_ATTRIBUTE *a =
      (struct MYSOFA_ATTRIBUTE *)malloc(sizeof(struct MYSOFA_ATTRIBUTE));
  a->next = NULL;
  a->name = (char *)malloc(strlen(name) + 1);
  a->value = (char *)malloc(strlen(value) + 1);
  strcpy(a->name, name);
  strcpy(a->value, value);
  return a;
}

static struct MYSOFA_HRTF *heap_hrtf(unsigned M, unsigned N) {
  struct MYSOFA_HRTF *h = (struct MYSOFA_HRTF *)malloc(sizeof(*h));
  float *pos = (float *)malloc((size_t)M * 3 * sizeof(float));
  float *ir = (float *)malloc((size_t)M * 2 * N * sizeof(float));
  float *delay = (float *)malloc((size_t)M * 2 * sizeof(float));
  float *rate = (float *)malloc(sizeof(float));
  unsigned m, c;
  memset(h, 0, sizeof(*h));
  {
    float vectors[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1,
                         0, 0, 0, 1, 0, 0, -1};
    memcpy(pos, vectors, 18 * sizeof(float));
  }
  for (m = 0; m < M; m++) {
    for (c = 0; c < N; c++) {
      ir[m * 2 * N + c] = (float)(m * N + c + 1) * 0.125f;
      ir[m * 2 * N + N + c] = (float)(m * N + c + 1) * 0.0625f;
    }
    delay[m * 2 + 0] = (float)m + 0.5f;
    delay[m * 2 + 1] = (float)m + 10.5f;
  }
  *rate = 48000.0f;
  h->I = 1;
  h->C = 3;
  h->R = 2;
  h->E = 1;
  h->N = N;
  h->M = M;
  h->SourcePosition.values = pos;
  h->SourcePosition.elements = M * 3;
  h->SourcePosition.attributes = heap_attr("Type", "cartesian");
  h->DataIR.values = ir;
  h->DataIR.elements = M * 2 * N;
  h->DataDelay.values = delay;
  h->DataDelay.elements = M * 2;
  h->DataSamplingRate.values = rate;
  h->DataSamplingRate.elements = 1;
  return h;
}

static void snapshot_query(MYSOFA_SPHERICAL *s, float *l, float *r, float *d,
                           unsigned *idx, float *w, int *ok) {
  *ok = get_dir(s, 0.3, -0.7, 0.5, l, r, d, idx, w);
}

#define LIFETIME_THREADS 4
#define LIFETIME_ITERS 200

struct lifetime_job {
  MYSOFA_SPHERICAL *s;
  float dir[3];
  int ok;
  unsigned i1[3];
  float w1[3];
};

static void *lifetime_worker(void *arg) {
  struct lifetime_job *j = (struct lifetime_job *)arg;
  int it;
  for (it = 0; it < LIFETIME_ITERS; it++) {
    float l[4], r[4], d[2], w[3];
    unsigned idx[3];
    int ret = get_dir(j->s, j->dir[0], j->dir[1], j->dir[2], l, r, d, idx, w);
    if (ret != MYSOFA_OK || fabs(w[0] + w[1] + w[2] - 1.0) > 1e-4) {
      j->ok = 0;
      return NULL;
    }
    if (it == 0) {
      unsigned k;
      for (k = 0; k < 3; k++) {
        j->i1[k] = idx[k];
        j->w1[k] = w[k];
      }
    } else {
      unsigned k;
      for (k = 0; k < 3; k++)
        if (j->i1[k] != idx[k] || fabs(j->w1[k] - w[k]) > 1e-5)
          j->ok = 0;
    }
  }
  return NULL;
}

static void compare_snapshot(const char *what, const float *l1,
                             const float *r1, const float *d1,
                             const unsigned *i1, const float *w1,
                             const float *l2, const float *r2, const float *d2,
                             const unsigned *i2, const float *w2,
                             unsigned N) {
  unsigned c;
  int k;
  CHECK(i1[0] == i2[0] && i1[1] == i2[1] && i1[2] == i2[2],
        "%s: indices changed", what);
  for (k = 0; k < 3; k++)
    near_value(w1[k], w2[k], 1e-6, what);
  for (c = 0; c < N; c++) {
    near_value(l1[c], l2[c], 1e-6, what);
    near_value(r1[c], r2[c], 1e-6, what);
  }
  near_value(d1[0], d2[0], 1e-6, what);
  near_value(d1[1], d2[1], 1e-6, what);
}

static void run_lifetime(void) {
  float l1[8], r1[8], d1[2], w1[3];
  unsigned i1[3];

  /* 1. freeing the source HRTF must not affect the context */
  {
    struct MYSOFA_HRTF *h = heap_hrtf(6, 4);
    MYSOFA_SPHERICAL *s;
    int err = 0, ok;
    h->DataDelay.elements = 12;
    s = mysofa_spherical_init(h, &err);
    CHECK(s != NULL, "lifetime free init err=%d", err);
    snapshot_query(s, l1, r1, d1, i1, w1, &ok);
    CHECK(ok == MYSOFA_OK, "lifetime initial query");
    mysofa_free(h);
    {
      float l2[8], r2[8], d2[2], w2[3];
      unsigned i2[3];
      snapshot_query(s, l2, r2, d2, i2, w2, &ok);
      CHECK(ok == MYSOFA_OK, "lifetime query after free");
      compare_snapshot("after free", l1, r1, d1, i1, w1, l2, r2, d2, i2, w2,
                       4);
    }
    mysofa_spherical_free(s);
  }

  /* 2. overwriting the source arrays must not affect the context */
  {
    struct MYSOFA_HRTF *h = heap_hrtf(6, 4);
    MYSOFA_SPHERICAL *s;
    int err = 0, ok;
    unsigned m;
    s = mysofa_spherical_init(h, &err);
    CHECK(s != NULL, "lifetime modify init err=%d", err);
    snapshot_query(s, l1, r1, d1, i1, w1, &ok);
    CHECK(ok == MYSOFA_OK, "lifetime modify initial query");
    for (m = 0; m < 6 * 3; m++)
      h->SourcePosition.values[m] = 0.0f;
    for (m = 0; m < 6 * 2 * 4; m++)
      h->DataIR.values[m] = -12345.0f;
    for (m = 0; m < 12; m++)
      h->DataDelay.values[m] = 99.0f;
    {
      float l2[8], r2[8], d2[2], w2[3];
      unsigned i2[3];
      snapshot_query(s, l2, r2, d2, i2, w2, &ok);
      CHECK(ok == MYSOFA_OK, "lifetime query after modify");
      compare_snapshot("after modify", l1, r1, d1, i1, w1, l2, r2, d2, i2,
                       w2, 4);
    }
    mysofa_spherical_free(s);
    mysofa_free(h);
  }

  /* 3. resampling the source HRTF must not affect the context */
  {
    struct MYSOFA_HRTF *h = heap_hrtf(6, 4);
    MYSOFA_SPHERICAL *s;
    int err = 0, ok, rerr;
    s = mysofa_spherical_init(h, &err);
    CHECK(s != NULL, "lifetime resample init err=%d", err);
    snapshot_query(s, l1, r1, d1, i1, w1, &ok);
    CHECK(ok == MYSOFA_OK, "lifetime resample initial query");
    rerr = mysofa_resample(h, 96000.0f);
    CHECK(rerr == MYSOFA_OK, "lifetime mysofa_resample failed %d", rerr);
    CHECK(h->N != 4, "lifetime resample did not change N");
    {
      float l2[8], r2[8], d2[2], w2[3];
      unsigned i2[3];
      snapshot_query(s, l2, r2, d2, i2, w2, &ok);
      CHECK(ok == MYSOFA_OK, "lifetime query after resample");
      compare_snapshot("after resample", l1, r1, d1, i1, w1, l2, r2, d2, i2,
                       w2, 4);
    }
    mysofa_spherical_free(s);
    mysofa_free(h);
  }

  /* 4. concurrent queries with independent output buffers */
  {
    struct MYSOFA_HRTF *h = heap_hrtf(6, 4);
    MYSOFA_SPHERICAL *s;
    int err = 0;
    s = mysofa_spherical_init(h, &err);
    CHECK(s != NULL, "lifetime threads init err=%d", err);
    {
      struct lifetime_job jobs[LIFETIME_THREADS];
      pthread_t tid[LIFETIME_THREADS];
      int q;
      for (q = 0; q < LIFETIME_THREADS; q++) {
        jobs[q].s = s;
        jobs[q].dir[0] = (float)(q + 1);
        jobs[q].dir[1] = (float)(q + 2) * 0.5f;
        jobs[q].dir[2] = (float)(q + 1) * 0.25f;
        jobs[q].ok = 1;
      }
      for (q = 0; q < LIFETIME_THREADS; q++)
        CHECK(pthread_create(&tid[q], NULL, lifetime_worker, &jobs[q]) == 0,
              "pthread_create failed");
      for (q = 0; q < LIFETIME_THREADS; q++)
        CHECK(pthread_join(tid[q], NULL) == 0, "pthread_join failed");
      for (q = 0; q < LIFETIME_THREADS; q++)
        CHECK(jobs[q].ok == 1, "concurrent query %d disagreed", q);
    }
    mysofa_spherical_free(s);
    mysofa_free(h);
  }
}

/* ------------------------------------------------------------------ */
/* malformed                                                           */
/* ------------------------------------------------------------------ */

static int init_err(struct MYSOFA_HRTF *h) {
  int err = 0;
  MYSOFA_SPHERICAL *s = mysofa_spherical_init(h, &err);
  cases++;
  if (s) {
    mysofa_spherical_free(s);
    printf("FAIL %s: malformed input was accepted (M=%u C=%u R=%u N=%u "
           "pos=%u ir=%u delay=%u)\n",
           group_name, h->M, h->C, h->R, h->N, h->SourcePosition.elements,
           h->DataIR.elements, h->DataDelay.elements);
    exit(1);
  }
  return err;
}

static void expect_unchanged(const float *before, const float *after,
                             size_t n, const char *what) {
  cases++;
  if (memcmp(before, after, n * sizeof(float)) != 0) {
    printf("FAIL %s: %s wrote to output buffer\n", group_name, what);
    exit(1);
  }
}

static void run_malformed(void) {
  float delay[2] = {0, 0};
  struct MYSOFA_HRTF h;
  int e;

  /* hemisphere: all z >= 0, the hull touches the origin */
  {
    float hemi[21] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0,
                      0, 0, 1, 0.5f, 0.5f, 0.7071f, -0.5f, 0.5f, 0.7071f};
    float hir[14] = {0};
    build_hrtf(&h, 7, 1, hemi, hir, delay, 2);
    e = init_err(&h);
    CHECK(e != 0, "hemisphere error code not set");
  }

  /* degenerate, all points on one great circle */
  {
    float line[15] = {1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0, 0.5f, 0.5f, 0};
    float lir[10] = {0};
    build_hrtf(&h, 5, 1, line, lir, delay, 2);
    e = init_err(&h);
    CHECK(e != 0, "coplanar error code not set");
  }

  /* duplicate normalized direction */
  {
    float dup[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0,
                     0, -1, 0, 0, 0, 1, 0, 0, 1};
    float dir[12] = {0};
    build_hrtf(&h, 6, 1, dup, dir, delay, 2);
    e = init_err(&h);
    CHECK(e != 0, "duplicate direction error code not set");
  }

  /* bad dimensions and array sizes */
  {
    float p4[12] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0};
    float i4[8] = {0};
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.C = 2;
    e = init_err(&h);
    CHECK(e != 0, "C=2 error code not set");
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.R = 1;
    e = init_err(&h);
    CHECK(e != 0, "R=1 error code not set");
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.M = 3;
    e = init_err(&h);
    CHECK(e != 0, "M=3 error code not set");
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.N = 0;
    e = init_err(&h);
    CHECK(e != 0, "N=0 error code not set");
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.SourcePosition.elements = 11;
    e = init_err(&h);
    CHECK(e != 0, "SourcePosition size error code not set");
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.DataIR.elements = 7;
    e = init_err(&h);
    CHECK(e != 0, "DataIR size error code not set");
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.DataDelay.elements = 3;
    e = init_err(&h);
    CHECK(e != 0, "DataDelay size error code not set");
  }

  /* non-finite input values */
  {
    float p4[12] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0};
    float i4[8] = {0};
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    p4[0] = NAN;
    e = init_err(&h);
    CHECK(e != 0, "NaN coordinate error code not set");
    p4[0] = 1;
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    i4[0] = INFINITY;
    e = init_err(&h);
    CHECK(e != 0, "Inf FIR error code not set");
    i4[0] = 0;
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    {
      float bad[2] = {NAN, 0};
      struct MYSOFA_ARRAY old = h.DataDelay;
      h.DataDelay.values = bad;
      e = init_err(&h);
      h.DataDelay = old;
      CHECK(e != 0, "NaN delay error code not set");
    }
  }

  /* zero radius */
  {
    float p4[12] = {0, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0};
    float i4[8] = {0};
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    e = init_err(&h);
    CHECK(e != 0, "zero radius error code not set");
  }

  /* radius spread beyond 2e-5 */
  {
    float p4[12] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0};
    float i4[8] = {0};
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    p4[3] = -1.0001f;
    e = init_err(&h);
    CHECK(e != 0, "radius spread error code not set");
  }

  /* wrong coordinate type / missing attribute */
  {
    float p4[12] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0};
    float i4[8] = {0};
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.SourcePosition.attributes = &SPHERICAL;
    e = init_err(&h);
    CHECK(e != 0, "spherical type error code not set");
    build_hrtf(&h, 4, 1, p4, i4, delay, 2);
    h.SourcePosition.attributes = NULL;
    e = init_err(&h);
    CHECK(e != 0, "missing Type error code not set");
  }

  /* NULL HRTF */
  {
    int err = 0;
    MYSOFA_SPHERICAL *s = mysofa_spherical_init(NULL, &err);
    cases++;
    CHECK(s == NULL && err != 0, "NULL HRTF error code not set");
  }

  /* query failures must not write the output buffers */
  {
    float p6[18] = {1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1,
                    0, 0, 0, 1, 0, 0, -1};
    float i6[12] = {0};
    MYSOFA_SPHERICAL *s;
    int err = 0;
    build_hrtf(&h, 6, 1, p6, i6, delay, 2);
    s = mysofa_spherical_init(&h, &err);
    CHECK(s != NULL, "query failure setup err=%d", err);

    {
      float sl[1] = {11.0f}, sr[1] = {12.0f}, sd[2] = {13.0f, 14.0f};
      unsigned si[3] = {18u, 19u, 20u};
      float sw[3] = {15.0f, 16.0f, 17.0f};
      float l[1], r[1], d[2], w[3], dir[3] = {0, 0, 0};
      unsigned idx[3];
      int ret;
      memcpy(l, sl, sizeof(l));
      memcpy(r, sr, sizeof(r));
      memcpy(d, sd, sizeof(d));
      memcpy(w, sw, sizeof(w));
      memcpy(idx, si, sizeof(idx));
      ret = mysofa_spherical_getfilter(s, dir, l, r, d, idx, w);
      cases++;
      CHECK(ret != MYSOFA_OK, "zero direction accepted");
      expect_unchanged(sl, l, 1, "zero direction left");
      expect_unchanged(sr, r, 1, "zero direction right");
      expect_unchanged(sd, d, 2, "zero direction delay");
      expect_unchanged(sw, w, 3, "zero direction weights");
      cases++;
      CHECK(idx[0] == 18u && idx[1] == 19u && idx[2] == 20u,
            "zero direction indices");
    }
    {
      float sl[1] = {11.0f}, sr[1] = {12.0f}, sd[2] = {13.0f, 14.0f};
      unsigned si[3] = {18u, 19u, 20u};
      float sw[3] = {15.0f, 16.0f, 17.0f};
      float l[1], r[1], d[2], w[3], dir[3] = {NAN, 0, 1};
      unsigned idx[3];
      int ret;
      memcpy(l, sl, sizeof(l));
      memcpy(r, sr, sizeof(r));
      memcpy(d, sd, sizeof(d));
      memcpy(w, sw, sizeof(w));
      memcpy(idx, si, sizeof(idx));
      ret = mysofa_spherical_getfilter(s, dir, l, r, d, idx, w);
      cases++;
      CHECK(ret != MYSOFA_OK, "NaN direction accepted");
      expect_unchanged(sl, l, 1, "NaN direction left");
      expect_unchanged(sr, r, 1, "NaN direction right");
      expect_unchanged(sd, d, 2, "NaN direction delay");
      expect_unchanged(sw, w, 3, "NaN direction weights");
    }
    {
      float sl[1] = {11.0f}, sr[1] = {12.0f}, sd[2] = {13.0f, 14.0f};
      unsigned si[3] = {18u, 19u, 20u};
      float sw[3] = {15.0f, 16.0f, 17.0f};
      float l[1], r[1], d[2], w[3], dir[3] = {INFINITY, 0, 1};
      unsigned idx[3];
      int ret;
      memcpy(l, sl, sizeof(l));
      memcpy(r, sr, sizeof(r));
      memcpy(d, sd, sizeof(d));
      memcpy(w, sw, sizeof(w));
      memcpy(idx, si, sizeof(idx));
      ret = mysofa_spherical_getfilter(s, dir, l, r, d, idx, w);
      cases++;
      CHECK(ret != MYSOFA_OK, "Inf direction accepted");
      expect_unchanged(sl, l, 1, "Inf direction left");
    }
    {
      float sl[1] = {11.0f}, sr[1] = {12.0f};
      float l[1], r[1], dir[3] = {1, 2, 3};
      int ret;
      memcpy(l, sl, sizeof(l));
      memcpy(r, sr, sizeof(r));
      ret = mysofa_spherical_getfilter(NULL, dir, l, r, NULL, NULL, NULL);
      cases++;
      CHECK(ret != MYSOFA_OK, "NULL context accepted");
      expect_unchanged(sl, l, 1, "NULL context left");
      expect_unchanged(sr, r, 1, "NULL context right");
    }
    {
      float sr[1] = {12.0f}, sd[2] = {13.0f, 14.0f};
      unsigned si[3] = {18u, 19u, 20u};
      float sw[3] = {15.0f, 16.0f, 17.0f};
      float r[1], d[2], w[3], dir[3] = {1, 2, 3};
      unsigned idx[3];
      int ret;
      memcpy(r, sr, sizeof(r));
      memcpy(d, sd, sizeof(d));
      memcpy(w, sw, sizeof(w));
      memcpy(idx, si, sizeof(idx));
      ret = mysofa_spherical_getfilter(s, dir, NULL, r, d, idx, w);
      cases++;
      CHECK(ret != MYSOFA_OK, "NULL left accepted");
      expect_unchanged(sr, r, 1, "NULL left right buffer");
      expect_unchanged(sd, d, 2, "NULL left delay buffer");
      expect_unchanged(sw, w, 3, "NULL left weight buffer");
    }
    {
      float sl[1] = {11.0f}, sr[1] = {12.0f}, sd[2] = {13.0f, 14.0f};
      float sw[3] = {15.0f, 16.0f, 17.0f};
      float l[1], r[1], d[2], w[3], dir[3] = {1, 2, 3};
      int ret;
      memcpy(l, sl, sizeof(l));
      memcpy(r, sr, sizeof(r));
      memcpy(d, sd, sizeof(d));
      memcpy(w, sw, sizeof(w));
      ret = mysofa_spherical_getfilter(s, dir, l, r, d, NULL, w);
      cases++;
      CHECK(ret != MYSOFA_OK, "NULL indices accepted");
      expect_unchanged(sl, l, 1, "NULL indices left buffer");
      expect_unchanged(sr, r, 1, "NULL indices right buffer");
      expect_unchanged(sd, d, 2, "NULL indices delay buffer");
      expect_unchanged(sw, w, 3, "NULL indices weights");
    }
    {
      float sl[1] = {11.0f}, sr[1] = {12.0f}, sd[2] = {13.0f, 14.0f};
      unsigned si[3] = {18u, 19u, 20u};
      float l[1], r[1], d[2], dir[3] = {1, 2, 3};
      unsigned idx[3];
      int ret;
      memcpy(l, sl, sizeof(l));
      memcpy(r, sr, sizeof(r));
      memcpy(d, sd, sizeof(d));
      memcpy(idx, si, sizeof(idx));
      ret = mysofa_spherical_getfilter(s, dir, l, r, d, idx, NULL);
      cases++;
      CHECK(ret != MYSOFA_OK, "NULL weights accepted");
      expect_unchanged(sl, l, 1, "NULL weights left buffer");
      expect_unchanged(sr, r, 1, "NULL weights right buffer");
      expect_unchanged(sd, d, 2, "NULL weights delay buffer");
      cases++;
      CHECK(idx[0] == 18u && idx[1] == 19u && idx[2] == 20u,
            "NULL weights indices");
    }
    {
      float sl[1] = {11.0f}, sr[1] = {12.0f};
      unsigned si[3] = {18u, 19u, 20u};
      float l[1], r[1], dir[3] = {1, 2, 3};
      unsigned idx[3];
      int ret;
      memcpy(l, sl, sizeof(l));
      memcpy(r, sr, sizeof(r));
      memcpy(idx, si, sizeof(idx));
      ret = mysofa_spherical_getfilter(s, dir, l, r, NULL, idx, NULL);
      cases++;
      CHECK(ret != MYSOFA_OK, "NULL delays accepted");
      expect_unchanged(sl, l, 1, "NULL delays left buffer");
      expect_unchanged(sr, r, 1, "NULL delays right buffer");
      cases++;
      CHECK(idx[0] == 18u && idx[1] == 19u && idx[2] == 20u,
            "NULL delays indices");
    }
    {
      float sl[1] = {11.0f};
      float l[1];
      memcpy(l, sl, sizeof(l));
      cases++;
      CHECK(mysofa_spherical_getfilter(s, NULL, l, l, NULL, NULL, NULL) ==
                MYSOFA_INTERNAL_ERROR,
            "NULL direction accepted");
      expect_unchanged(sl, l, 1, "NULL direction buffer");
    }

    /* any finite, non-zero direction must be accepted; only its length
     * matters for normalization */
    {
      float l[1], r[1], d[2], w[3];
      unsigned idx[3];
      float big[3] = {1e30f, 2e30f, 3e30f};
      float small[3] = {1e-30f, 2e-30f, 3e-30f};
      float one[3] = {1, 2, 3};
      int ret;
      ret = mysofa_spherical_getfilter(s, big, l, r, d, idx, w);
      cases++;
      CHECK(ret == MYSOFA_OK, "large finite direction rejected %d", ret);
      {
        float w2[3];
        unsigned idx2[3];
        float l2[1], r2[1], d2[2];
        ret = mysofa_spherical_getfilter(s, one, l2, r2, d2, idx2, w2);
        cases++;
        CHECK(ret == MYSOFA_OK, "unit direction rejected");
        cases++;
        CHECK(idx[0] == idx2[0] && idx[1] == idx2[1] &&
                  idx[2] == idx2[2],
              "direction length changed indices");
        near_value(w[0], w2[0], 1e-5, "direction length");
        near_value(w[1], w2[1], 1e-5, "direction length");
        near_value(w[2], w2[2], 1e-5, "direction length");
      }
      ret = mysofa_spherical_getfilter(s, small, l, r, d, idx, w);
      cases++;
      CHECK(ret == MYSOFA_OK, "small finite direction rejected %d", ret);
    }

    mysofa_spherical_free(s);
  }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <geometry|audio|lifetime|malformed>\n",
            argv[0]);
    return 2;
  }
  group_name = argv[1];
  cases = 0;
  if (strcmp(argv[1], "geometry") == 0)
    run_geometry();
  else if (strcmp(argv[1], "audio") == 0)
    run_audio();
  else if (strcmp(argv[1], "lifetime") == 0)
    run_lifetime();
  else if (strcmp(argv[1], "malformed") == 0)
    run_malformed();
  else {
    fprintf(stderr, "unknown group %s\n", argv[1]);
    return 2;
  }
  printf("PASS %s cases=%u\n", argv[1], cases);
  return 0;
}
