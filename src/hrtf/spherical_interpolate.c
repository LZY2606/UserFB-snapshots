/*
 * Spherical triangulation interpolation for single-radius HRTF data.
 *
 * The measurement directions are normalized and their three-dimensional
 * convex hull is built once in mysofa_spherical_init().  A query casts a
 * ray from the origin, finds the hull triangle that it hits and uses the
 * triangle's barycentric weights to blend the original FIR filters.
 *
 * The implementation deliberately keeps the legacy lookup/neighborhood
 * code untouched.  It is a separate, additional path.
 */

#include "mysofa.h"
#include "mysofa_export.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef SPH_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...)
#endif

/* ---------------------------------------------------------------------
 * small vector helpers (all geometry is done in double precision)
 * --------------------------------------------------------------------- */

typedef struct {
  double x, y, z;
} vec3;

static vec3 vec_sub(const vec3 a, const vec3 b) {
  vec3 r;
  r.x = a.x - b.x;
  r.y = a.y - b.y;
  r.z = a.z - b.z;
  return r;
}

static double vec_dot(const vec3 a, const vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3 vec_cross(const vec3 a, const vec3 b) {
  vec3 r;
  r.x = a.y * b.z - a.z * b.y;
  r.y = a.z * b.x - a.x * b.z;
  r.z = a.x * b.y - a.y * b.x;
  return r;
}

static double vec_len(const vec3 a) { return sqrt(vec_dot(a, a)); }

static int vec_is_finite(const vec3 a) {
  return isfinite(a.x) && isfinite(a.y) && isfinite(a.z);
}

/* ---------------------------------------------------------------------
 * opaque context
 * --------------------------------------------------------------------- */

struct MYSOFA_SPHERICAL {
  unsigned M;
  unsigned N;
  unsigned R;

  /* 1 if DataDelay contains M*R values, 0 if it contains R global values */
  int delay_per_measurement;

  /* normalized measurement directions, M * 3 */
  double *dir;

  /* private snapshot of the input data */
  float *fir;   /* M * R * N */
  float *delay; /* R or M * R */

  /* closed triangle mesh, indices into the original measurement arrays */
  unsigned triangles;
  unsigned *triangle;
};

/* =====================================================================
 * convex hull construction
 * ===================================================================== */

typedef struct {
  unsigned v[3];
  vec3 n;   /* unit outward normal */
  double d; /* plane is n . x = d */
} hull_face;

typedef struct {
  unsigned index;
  double s, t;
} plane_point;

typedef struct {
  vec3 n;
  double d;
} plane_group;

static double cross2(double ax, double ay, double bx, double by) {
  return ax * by - ay * bx;
}

static int compare_plane_point(const void *pa, const void *pb) {
  const plane_point *a = (const plane_point *)pa;
  const plane_point *b = (const plane_point *)pb;
  if (a->s < b->s)
    return -1;
  if (a->s > b->s)
    return 1;
  if (a->t < b->t)
    return -1;
  if (a->t > b->t)
    return 1;
  return 0;
}

/*
 * Append one outward-facing triangle.  `interior` is known to be inside the
 * hull, which makes the orientation independent of the vertex order passed
 * in.  Degenerate triangles (for example a horizon edge collinear with the
 * new point) are silently dropped.
 */
static int add_face(hull_face *faces, size_t *count, size_t capacity,
                    const vec3 *p, unsigned v0, unsigned v1, unsigned v2,
                    vec3 interior) {
  unsigned a = v0, b = v1, c = v2;
  vec3 pa = p[a];
  vec3 u = vec_sub(p[b], pa);
  vec3 v = vec_sub(p[c], pa);
  vec3 n = vec_cross(u, v);
  double l = vec_len(n);
  hull_face tmp;

  if (l < 1e-14)
    return 0;
  n.x /= l;
  n.y /= l;
  n.z /= l;
  if (vec_dot(n, interior) - vec_dot(n, pa) > 0.0) {
    unsigned swap = b;
    b = c;
    c = swap;
    u = vec_sub(p[b], pa);
    v = vec_sub(p[c], pa);
    n = vec_cross(u, v);
    l = vec_len(n);
    if (l < 1e-14)
      return 0;
    n.x /= l;
    n.y /= l;
    n.z /= l;
  }
  if (*count >= capacity)
    return -1;
  tmp.v[0] = a;
  tmp.v[1] = b;
  tmp.v[2] = c;
  tmp.n = n;
  tmp.d = vec_dot(n, pa);
  faces[*count] = tmp;
  DBG("  add face %u %u %u d=%g n=%g,%g,%g (count %zu)\n", tmp.v[0], tmp.v[1],
      tmp.v[2], tmp.d, tmp.n.x, tmp.n.y, tmp.n.z, *count);
  (*count)++;
  return 1;
}

/*
 * Build the closed triangular hull of the unit directions p[0..M-1].  The
 * mesh is stored in ctx->triangle as indices into the original measurement
 * arrays.
 *
 * The hull is grown incrementally.  Coplanar hull facets are merged by plane
 * and retriangulated as a fan around the facet vertex with the lexicographically
 * smallest direction, so the geometric partition does not depend on the order
 * in which the measurements are supplied.
 */
static int build_hull(struct MYSOFA_SPHERICAL *ctx, const vec3 *p, int *err) {
  size_t M = ctx->M;
  size_t i, j, i1 = 0, i2 = 0, i3 = 0;
  size_t f, ng, g, fcount, pcount, tcount;
  double best, area2, vol;
  hull_face *faces = NULL;
  unsigned char *visible = NULL;
  unsigned *vlist = NULL;
  unsigned *horizon = NULL;
  plane_group *groups = NULL;
  unsigned *face_group = NULL;
  plane_point *pp = NULL;
  plane_point *hull = NULL;
  unsigned *tris = NULL;
  size_t max_faces = 2 * M + 8;
  size_t max_horizon = 6 * max_faces + 8;
  size_t max_tris = 3 * (4 * M + 16); /* generous triangle vertex entries */
  vec3 interior;

  /* --- pick four non-coplanar seed points ----------------------------- */
  best = -1.0;
  for (i = 0; i < M; i++) {
    for (j = i + 1; j < M; j++) {
      vec3 v = vec_sub(p[j], p[i]);
      double l2 = vec_dot(v, v);
      if (l2 > best) {
        best = l2;
        i1 = j;
      }
    }
  }
  if (best < 1e-18) {
    DBG("hull: degenerate best=%g\n", best);
    goto degenerate;
  }

  area2 = 0.0;
  {
    vec3 v01 = vec_sub(p[i1], p[0]);
    for (f = 0; f < M; f++) {
      vec3 v0f, c;
      double l2;
      if (f == 0 || f == i1)
        continue;
      v0f = vec_sub(p[f], p[0]);
      c = vec_cross(v01, v0f);
      l2 = vec_dot(c, c);
      if (l2 > area2) {
        area2 = l2;
        i2 = f;
      }
    }
  }
  if (area2 < 1e-24) {
    DBG("hull: degenerate area2=%g\n", area2);
    goto degenerate;
  }

  vol = 0.0;
  {
    vec3 v01 = vec_sub(p[i1], p[0]);
    vec3 v02 = vec_sub(p[i2], p[0]);
    vec3 n = vec_cross(v01, v02);
    for (f = 0; f < M; f++) {
      double v;
      if (f == 0 || f == i1 || f == i2)
        continue;
      v = fabs(vec_dot(n, vec_sub(p[f], p[0])));
      if (v > vol) {
        vol = v;
        i3 = f;
      }
    }
  }
  if (vol < 1e-14) {
    DBG("hull: degenerate vol=%g\n", vol);
    goto degenerate;
  }

  DBG("seed %zu %zu %zu %zu vol=%g area2=%g\n", (size_t)0, i1, i2, i3, vol,
      area2);

  interior.x = (p[0].x + p[i1].x + p[i2].x + p[i3].x) / 4.0;
  interior.y = (p[0].y + p[i1].y + p[i2].y + p[i3].y) / 4.0;
  interior.z = (p[0].z + p[i1].z + p[i2].z + p[i3].z) / 4.0;

  faces = (hull_face *)malloc(max_faces * sizeof(hull_face));
  visible = (unsigned char *)calloc(max_faces, sizeof(unsigned char));
  vlist = (unsigned *)malloc(max_faces * sizeof(unsigned));
  horizon = (unsigned *)malloc(max_horizon * sizeof(unsigned));
  groups = (plane_group *)malloc(max_faces * sizeof(plane_group));
  face_group = (unsigned *)malloc(max_faces * sizeof(unsigned));
  tris = (unsigned *)malloc(max_tris * sizeof(unsigned));
  if (!faces || !visible || !vlist || !horizon || !groups || !face_group ||
      !tris) {
    *err = MYSOFA_NO_MEMORY;
    goto fail;
  }

  /* --- seed tetrahedron ------------------------------------------------- */
  fcount = 0;
  if (add_face(faces, &fcount, max_faces, p, 0, (unsigned)i1, (unsigned)i2,
               interior) <= 0 ||
      add_face(faces, &fcount, max_faces, p, 0, (unsigned)i1, (unsigned)i3,
               interior) <= 0 ||
      add_face(faces, &fcount, max_faces, p, 0, (unsigned)i2, (unsigned)i3,
               interior) <= 0 ||
      add_face(faces, &fcount, max_faces, p, (unsigned)i1, (unsigned)i2,
               (unsigned)i3, interior) <= 0)
    goto degenerate;

  /* --- add remaining points --------------------------------------------- */
  for (f = 0; f < M; f++) {
    size_t vn = 0, hn = 0, w, e, h;
    vec3 pi;
    if (f == 0 || f == i1 || f == i2 || f == i3)
      continue;

    pi = p[f];
    for (w = 0; w < fcount; w++) {
      double s = vec_dot(faces[w].n, pi) - faces[w].d;
      visible[w] = (s > 1e-10) ? 1 : 0;
      if (visible[w])
        vlist[vn++] = (unsigned)w;
    }
    if (vn == 0)
      continue;

    /*
     * Horizon = directed edges of visible faces that are not shared with
     * another visible face in the opposite direction.
     */
    for (e = 0; e < vn; e++) {
      unsigned fi = vlist[e];
      int k;
      for (k = 0; k < 3; k++) {
        unsigned a = faces[fi].v[k];
        unsigned b = faces[fi].v[(k + 1) % 3];
        int shared = 0;
        size_t e2;
        for (e2 = 0; e2 < vn && !shared; e2++) {
          unsigned fj = vlist[e2];
          int m;
          if (fj == fi)
            continue;
          for (m = 0; m < 3; m++) {
            if (faces[fj].v[m] == b &&
                faces[fj].v[(m + 1) % 3] == a) {
              shared = 1;
              break;
            }
          }
        }
        if (!shared) {
          int already = 0;
          size_t q;
          for (q = 0; q + 1 < hn; q += 2) {
            if ((horizon[q] == a && horizon[q + 1] == b) ||
                (horizon[q] == b && horizon[q + 1] == a)) {
              already = 1;
              break;
            }
          }
          if (!already) {
            if (hn + 2 > max_horizon)
              goto degenerate;
            horizon[hn++] = a;
            horizon[hn++] = b;
          }
        }
      }
    }

    /* drop the visible faces */
    w = 0;
    for (e = 0; e < fcount; e++) {
      if (!visible[e])
        faces[w++] = faces[e];
    }
    fcount = w;

    /* cone the horizon to the new point */
    for (h = 0; h + 1 < hn; h += 2) {
      int r = add_face(faces, &fcount, max_faces, p, horizon[h],
                       horizon[h + 1], (unsigned)f, interior);
      if (r < 0) {
        *err = MYSOFA_NO_MEMORY;
        goto fail;
      }
    }
  }

  /* --- merge coplanar triangles into facets ----------------------------- */
  DBG("faces=%zu\n", fcount);
  ng = 0;
  for (f = 0; f < fcount; f++) {
    size_t q;
    int found = 0;
    for (q = 0; q < ng; q++) {
      double dotn = vec_dot(faces[f].n, groups[q].n);
      if (dotn > 1.0 - 1e-6 && fabs(faces[f].d - groups[q].d) <= 1e-6) {
        face_group[f] = (unsigned)q;
        found = 1;
        break;
      }
    }
    if (!found) {
      groups[ng].n = faces[f].n;
      groups[ng].d = faces[f].d;
      face_group[f] = (unsigned)ng;
      ng++;
    }
  }
  if (ng < 4) {
    DBG("hull: only %zu facets\n", ng);
    goto degenerate;
  }

  /* every measurement must be inside every supporting plane */
  for (g = 0; g < ng; g++) {
    for (i = 0; i < M; i++) {
      double s = vec_dot(groups[g].n, p[i]) - groups[g].d;
      if (s > 1e-5) {
        DBG("outside g=%zu i=%zu s=%g n=%g,%g,%g d=%g\n", g, i, s,
            groups[g].n.x, groups[g].n.y, groups[g].n.z, groups[g].d);
        goto degenerate;
      }
    }
  }

  pp = (plane_point *)malloc(M * sizeof(plane_point));
  /* monotone chain temporarily stores lower and upper hulls separately */
  hull = (plane_point *)malloc((2 * M + 4) * sizeof(plane_point));
  if (!pp || !hull) {
    *err = MYSOFA_NO_MEMORY;
    goto fail;
  }

  tcount = 0;
  for (g = 0; g < ng; g++) {
    vec3 n = groups[g].n;
    double d = groups[g].d;
    vec3 e1, e2, helper;
    size_t k, kk, hk;
    int edge;

    if (d <= 1e-6) {
      DBG("origin too close g=%zu d=%g n=%g,%g,%g\n", g, d, n.x, n.y, n.z);
      goto degenerate;
    } /* supporting plane too close to the origin */
    /*
     * Deterministic in-plane basis with e1 x e2 = n.  It depends only on n,
     * so it is stable under measurement reordering.
     */
    if (fabs(n.z) < 0.9) {
      helper.x = 0.0;
      helper.y = 0.0;
      helper.z = 1.0;
    } else {
      helper.x = 1.0;
      helper.y = 0.0;
      helper.z = 0.0;
    }
    e1 = vec_cross(helper, n);
    {
      double l = vec_len(e1);
      if (l < 1e-12)
        goto degenerate;
      e1.x /= l;
      e1.y /= l;
      e1.z /= l;
    }
    e2 = vec_cross(n, e1);

    /*
     * Collect the union of the vertices of all triangles on this plane.  The
     * convex hull triangulation already contains every measurement vertex,
     * so no extra plane-distance search is needed and no vertex can be
     * dropped from the mesh.
     */
    pcount = 0;
    for (kk = 0; kk < fcount; kk++) {
      if (face_group[kk] != (unsigned)g)
        continue;
      for (edge = 0; edge < 3; edge++) {
        unsigned vi = faces[kk].v[edge];
        size_t q;
        int seen = 0;
        for (q = 0; q < pcount; q++)
          if (pp[q].index == vi)
            seen = 1;
        if (!seen) {
          pp[pcount].index = vi;
          pp[pcount].s = vec_dot(p[vi], e1);
          pp[pcount].t = vec_dot(p[vi], e2);
          pcount++;
        }
      }
    }
    if (pcount < 3) {
      DBG("hull: facet g=%zu pcount=%zu d=%g\n", g, pcount, d);
      goto degenerate;
    }

    qsort(pp, pcount, sizeof(plane_point), compare_plane_point);

    /* monotone-chain 2d hull, counter-clockwise in (s,t) */
    hk = 0;
    for (k = 0; k < pcount; k++) {
      while (hk >= 2 &&
             cross2(hull[hk - 1].s - hull[hk - 2].s,
                    hull[hk - 1].t - hull[hk - 2].t,
                    pp[k].s - hull[hk - 2].s,
                    pp[k].t - hull[hk - 2].t) <= 1e-14)
        hk--;
      hull[hk++] = pp[k];
    }
    {
      size_t lower = hk;
      for (k = pcount - 1; k-- > 0;) {
        while (hk > lower &&
               cross2(hull[hk - 1].s - hull[hk - 2].s,
                      hull[hk - 1].t - hull[hk - 2].t,
                      pp[k].s - hull[hk - 2].s,
                      pp[k].t - hull[hk - 2].t) <= 1e-14)
          hk--;
        hull[hk++] = pp[k];
      }
      hk--; /* last point repeats the first */
    }
    if (hk < 3) {
      DBG("hull: 2d hull g=%zu hk=%zu pcount=%zu\n", g, hk, pcount);
      goto degenerate;
    }

    /* rotate the polygon so the lexicographically smallest vertex is first */
    {
      size_t root = 0, r;
      for (r = 1; r < hk; r++) {
        unsigned ia = hull[r].index, ib = hull[root].index;
        if (p[ia].x < p[ib].x ||
            (p[ia].x == p[ib].x && p[ia].y < p[ib].y) ||
            (p[ia].x == p[ib].x && p[ia].y == p[ib].y &&
             p[ia].z < p[ib].z))
          root = r;
      }
      if (root != 0) {
        for (r = 0; r < hk; r++)
          pp[r] = hull[(r + root) % hk];
        for (r = 0; r < hk; r++)
          hull[r] = pp[r];
      }
    }

    /*
     * The polygon is counter-clockwise as seen from outside.  Fan
     * triangulation from hull[0] yields outward-facing triangles.
     */
    for (k = 1; k + 1 < hk; k++) {
      if (tcount + 3 > max_tris) {
        DBG("hull: triangle capacity g=%zu tcount=%zu max=%zu\n", g, tcount,
            max_tris);
        goto degenerate;
      }
      tris[tcount++] = hull[0].index;
      tris[tcount++] = hull[k].index;
      tris[tcount++] = hull[k + 1].index;
    }
  }

  free(faces);
  free(visible);
  free(vlist);
  free(horizon);
  free(groups);
  free(face_group);
  free(pp);
  free(hull);

  ctx->triangle = tris;
  ctx->triangles = (unsigned)(tcount / 3);
  DBG("hull: success ng=%zu triangles=%u\n", ng, ctx->triangles);
  return MYSOFA_OK;

degenerate:
  DBG("hull: degenerate label reached\n");
  *err = MYSOFA_INVALID_FORMAT;
fail:
  free(faces);
  free(visible);
  free(vlist);
  free(horizon);
  free(groups);
  free(face_group);
  free(pp);
  free(hull);
  free(tris);
  return *err;
}

/* =====================================================================
 * public API
 * ===================================================================== */

MYSOFA_EXPORT MYSOFA_SPHERICAL *
mysofa_spherical_init(const struct MYSOFA_HRTF *hrtf, int *err) {
  MYSOFA_SPHERICAL *ctx = NULL;
  vec3 *points = NULL;
  size_t i, j, count;
  double r0;
  unsigned delay_elements;
  char *type;

  if (err)
    *err = MYSOFA_OK;
  if (!hrtf) {
    if (err)
      *err = MYSOFA_INTERNAL_ERROR;
    return NULL;
  }

  type = mysofa_getAttribute(hrtf->SourcePosition.attributes, "Type");
  if (!type || strcmp(type, "cartesian") != 0) {
    DBG("init: type '%s'\n", type ? type : "(null)");
    if (err)
      *err = MYSOFA_INVALID_COORDINATE_TYPE;
    return NULL;
  }

  if (hrtf->C != 3 || hrtf->R != 2 || hrtf->M < 4 || hrtf->N == 0) {
    DBG("init: dims C=%u R=%u M=%u N=%u\n", hrtf->C, hrtf->R, hrtf->M,
        hrtf->N);
    if (err)
      *err = MYSOFA_INVALID_DIMENSIONS;
    return NULL;
  }

  if (!hrtf->SourcePosition.values || !hrtf->DataIR.values ||
      !hrtf->DataDelay.values) {
    if (err)
      *err = MYSOFA_INVALID_FORMAT;
    return NULL;
  }

  {
    size_t pos_count = (size_t)hrtf->M * 3;
    size_t ir_count = (size_t)hrtf->M * hrtf->R * hrtf->N;
    size_t delay_count = (size_t)hrtf->M * hrtf->R;
    if (pos_count > UINT_MAX || ir_count > UINT_MAX)
      goto bad_dimensions;
    if ((size_t)hrtf->SourcePosition.elements != pos_count ||
        (size_t)hrtf->DataIR.elements != ir_count) {
    bad_dimensions:
      if (err)
        *err = MYSOFA_INVALID_DIMENSIONS;
      return NULL;
    }
    if ((size_t)hrtf->DataDelay.elements != hrtf->R &&
        (size_t)hrtf->DataDelay.elements != delay_count)
      goto bad_dimensions;
  }

  delay_elements = hrtf->DataDelay.elements;

  /* all input values must be finite */
  for (i = 0; i < (size_t)hrtf->M * 3; i++) {
    if (!isfinite(hrtf->SourcePosition.values[i])) {
      if (err)
        *err = MYSOFA_INVALID_FORMAT;
      return NULL;
    }
  }
  for (i = 0; i < (size_t)hrtf->M * hrtf->R * hrtf->N; i++) {
    if (!isfinite(hrtf->DataIR.values[i])) {
      if (err)
        *err = MYSOFA_INVALID_FORMAT;
      return NULL;
    }
  }
  for (i = 0; i < delay_elements; i++) {
    if (!isfinite(hrtf->DataDelay.values[i])) {
      if (err)
        *err = MYSOFA_INVALID_FORMAT;
      return NULL;
    }
  }

  points = (vec3 *)malloc((size_t)hrtf->M * sizeof(vec3));
  if (!points) {
    if (err)
      *err = MYSOFA_NO_MEMORY;
    return NULL;
  }

  /* radius of the first measurement */
  r0 = sqrt((double)hrtf->SourcePosition.values[0] *
                hrtf->SourcePosition.values[0] +
            (double)hrtf->SourcePosition.values[1] *
                hrtf->SourcePosition.values[1] +
            (double)hrtf->SourcePosition.values[2] *
                hrtf->SourcePosition.values[2]);
  if (!isfinite(r0) || r0 == 0.0) {
    free(points);
    if (err)
      *err = MYSOFA_INVALID_FORMAT;
    return NULL;
  }

  for (i = 0; i < hrtf->M; i++) {
    float *v = hrtf->SourcePosition.values + i * 3;
    double r = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] +
                    (double)v[2] * v[2]);
    if (!isfinite(r) || r == 0.0 || fabs(r - r0) > 2e-5 * fabs(r0)) {
      DBG("init: radius i=%zu r=%g r0=%g diff=%g\n", i, r, r0,
          fabs(r - r0));
      free(points);
      if (err)
        *err = MYSOFA_INVALID_FORMAT;
      return NULL;
    }
    points[i].x = (double)v[0] / r;
    points[i].y = (double)v[1] / r;
    points[i].z = (double)v[2] / r;
    if (!vec_is_finite(points[i])) {
      free(points);
      if (err)
        *err = MYSOFA_INVALID_FORMAT;
      return NULL;
    }
  }

  /* reject duplicate directions */
  for (i = 0; i < hrtf->M; i++) {
    for (j = i + 1; j < hrtf->M; j++) {
      vec3 d = vec_sub(points[i], points[j]);
      if (vec_dot(d, d) <= 1e-12) {
        DBG("init: duplicate i=%zu j=%zu d2=%g\n", i, j, vec_dot(d, d));
        free(points);
        if (err)
          *err = MYSOFA_INVALID_FORMAT;
        return NULL;
      }
    }
  }

  ctx = (MYSOFA_SPHERICAL *)malloc(sizeof(MYSOFA_SPHERICAL));
  if (!ctx) {
    free(points);
    if (err)
      *err = MYSOFA_NO_MEMORY;
    return NULL;
  }
  memset(ctx, 0, sizeof(*ctx));
  ctx->M = hrtf->M;
  ctx->N = hrtf->N;
  ctx->R = hrtf->R;
  ctx->delay_per_measurement =
      ((size_t)delay_elements == (size_t)hrtf->M * hrtf->R) ? 1 : 0;
  ctx->dir = (double *)malloc((size_t)hrtf->M * 3 * sizeof(double));
  ctx->fir = (float *)malloc((size_t)hrtf->M * hrtf->R * hrtf->N *
                             sizeof(float));
  ctx->delay = (float *)malloc(delay_elements * sizeof(float));
  if (!ctx->dir || !ctx->fir || !ctx->delay) {
    free(points);
    mysofa_spherical_free(ctx);
    if (err)
      *err = MYSOFA_NO_MEMORY;
    return NULL;
  }

  for (i = 0; i < hrtf->M; i++) {
    ctx->dir[i * 3 + 0] = points[i].x;
    ctx->dir[i * 3 + 1] = points[i].y;
    ctx->dir[i * 3 + 2] = points[i].z;
  }
  count = (size_t)hrtf->M * hrtf->R * hrtf->N;
  memcpy(ctx->fir, hrtf->DataIR.values, count * sizeof(float));
  memcpy(ctx->delay, hrtf->DataDelay.values, delay_elements * sizeof(float));

  {
    int hull_err = MYSOFA_OK;
    if (build_hull(ctx, points, &hull_err) != MYSOFA_OK) {
      free(points);
      mysofa_spherical_free(ctx);
      if (err)
        *err = hull_err;
      return NULL;
    }
  }

  free(points);
  return ctx;
}

/* --------------------------------------------------------------------- */

MYSOFA_EXPORT int mysofa_spherical_getfilter(
    const MYSOFA_SPHERICAL *spherical, const float direction[3], float *left,
    float *right, float delays[2], unsigned indices[3], float weights[3]) {

  double ray[3], ray_len;
  size_t t;
  int found = 0;
  double w0 = 0.0, w1 = 0.0, w2 = 0.0;
  unsigned i0 = 0, i1 = 0, i2 = 0;

  if (!spherical || !direction || !left || !right || !delays || !indices ||
      !weights)
    return MYSOFA_INTERNAL_ERROR;

  ray[0] = direction[0];
  ray[1] = direction[1];
  ray[2] = direction[2];
  if (!isfinite(ray[0]) || !isfinite(ray[1]) || !isfinite(ray[2]))
    return MYSOFA_INTERNAL_ERROR;

  ray_len = sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
  if (!isfinite(ray_len) || ray_len == 0.0)
    return MYSOFA_INTERNAL_ERROR;
  ray[0] /= ray_len;
  ray[1] /= ray_len;
  ray[2] /= ray_len;

  /*
   * Exact measurement hits return that measurement directly.  This makes the
   * vertex result independent of which adjacent triangle the ray would hit
   * first (and of any harmless numerical clamping at a vertex).
   */
  {
    size_t v;
    for (v = 0; v < spherical->M; v++) {
      const double *dv = spherical->dir + v * 3;
      double dx = ray[0] - dv[0];
      double dy = ray[1] - dv[1];
      double dz = ray[2] - dv[2];
      if (dx * dx + dy * dy + dz * dz <= 1e-14) {
        i0 = (unsigned)v;
        i1 = (unsigned)((v + 1) % spherical->M);
        i2 = (unsigned)((v + 2) % spherical->M);
        w0 = 1.0;
        w1 = 0.0;
        w2 = 0.0;
        found = 1;
        break;
      }
    }
  }

  if (found)
    goto mix;

  for (t = 0; t < spherical->triangles; t++) {
    const double *da = spherical->dir + spherical->triangle[t * 3 + 0] * 3;
    const double *db = spherical->dir + spherical->triangle[t * 3 + 1] * 3;
    const double *dc = spherical->dir + spherical->triangle[t * 3 + 2] * 3;
    vec3 a, b, c, u, v, n, ap;
    double nl, plane_d, denom, tt, px, py, pz;
    double d00, d01, d11, d20, d21, det, uu, vv, wa, wb, wc, sum;

    a.x = da[0];
    a.y = da[1];
    a.z = da[2];
    b.x = db[0];
    b.y = db[1];
    b.z = db[2];
    c.x = dc[0];
    c.y = dc[1];
    c.z = dc[2];

    u = vec_sub(b, a);
    v = vec_sub(c, a);
    n = vec_cross(u, v);
    nl = vec_len(n);
    if (nl < 1e-14)
      continue;
    n.x /= nl;
    n.y /= nl;
    n.z /= nl;

    plane_d = vec_dot(n, a);
    {
      vec3 rayv;
      rayv.x = ray[0];
      rayv.y = ray[1];
      rayv.z = ray[2];
      denom = vec_dot(n, rayv);
    }
    if (denom <= 1e-12)
      continue;

    tt = plane_d / denom;
    if (!(tt > 0.0) || !isfinite(tt))
      continue;

    px = tt * ray[0];
    py = tt * ray[1];
    pz = tt * ray[2];
    ap.x = px - a.x;
    ap.y = py - a.y;
    ap.z = pz - a.z;

    /* barycentric coordinates inside the triangle */
    d00 = vec_dot(u, u);
    d01 = vec_dot(u, v);
    d11 = vec_dot(v, v);
    d20 = vec_dot(ap, u);
    d21 = vec_dot(ap, v);
    det = d00 * d11 - d01 * d01;
    if (det <= 1e-18)
      continue;
    uu = (d11 * d20 - d01 * d21) / det;
    vv = (d00 * d21 - d01 * d20) / det;
    wa = 1.0 - uu - vv;
    wb = uu;
    wc = vv;

    if (wa < -1e-6 || wb < -1e-6 || wc < -1e-6)
      continue;

    if (wa < 0.0)
      wa = 0.0;
    if (wb < 0.0)
      wb = 0.0;
    if (wc < 0.0)
      wc = 0.0;
    sum = wa + wb + wc;
    if (sum <= 0.0)
      continue;
    wa /= sum;
    wb /= sum;
    wc /= sum;

    /* restore an exact vertex hit */
    if (wa >= 1.0 - 1e-6) {
      wa = 1.0;
      wb = 0.0;
      wc = 0.0;
    } else if (wb >= 1.0 - 1e-6) {
      wb = 1.0;
      wa = 0.0;
      wc = 0.0;
    } else if (wc >= 1.0 - 1e-6) {
      wc = 1.0;
      wa = 0.0;
      wb = 0.0;
    }

    w0 = wa;
    w1 = wb;
    w2 = wc;
    i0 = spherical->triangle[t * 3 + 0];
    i1 = spherical->triangle[t * 3 + 1];
    i2 = spherical->triangle[t * 3 + 2];
    found = 1;
    break;
  }

  if (!found)
    return MYSOFA_INTERNAL_ERROR;

mix:
  {
    float fw[3];
    unsigned fi[3];
    size_t c;
    unsigned k;
    fw[0] = (float)w0;
    fw[1] = (float)w1;
    fw[2] = (float)w2;
    fi[0] = i0;
    fi[1] = i1;
    fi[2] = i2;

    for (c = 0; c < spherical->N; c++) {
      double l = 0.0, r = 0.0;
      for (k = 0; k < 3; k++) {
        const float *fir =
            spherical->fir + (size_t)fi[k] * spherical->R * spherical->N;
        l += (double)fw[k] * fir[c];
        r += (double)fw[k] * fir[spherical->N + c];
      }
      left[c] = (float)l;
      right[c] = (float)r;
    }

    if (spherical->delay_per_measurement) {
      double dl = 0.0, dr = 0.0;
      for (k = 0; k < 3; k++) {
        const float *delay = spherical->delay + (size_t)fi[k] * spherical->R;
        dl += (double)fw[k] * delay[0];
        dr += (double)fw[k] * delay[1];
      }
      delays[0] = (float)dl;
      delays[1] = (float)dr;
    } else {
      /* global delay: return the original values unchanged */
      delays[0] = spherical->delay[0];
      delays[1] = spherical->delay[1];
    }

    indices[0] = fi[0];
    indices[1] = fi[1];
    indices[2] = fi[2];
    weights[0] = fw[0];
    weights[1] = fw[1];
    weights[2] = fw[2];
  }

  return MYSOFA_OK;
}

MYSOFA_EXPORT void mysofa_spherical_free(MYSOFA_SPHERICAL *spherical) {
  if (!spherical)
    return;
  free(spherical->dir);
  free(spherical->fir);
  free(spherical->delay);
  free(spherical->triangle);
  free(spherical);
}
