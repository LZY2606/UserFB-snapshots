/*
 *  timed_path.c
 *
 *  Regression tests for the microsecond-PTS camera path: the VSTimeBox
 *  physical-time box average, the timed text file format, PTS interpolation and
 *  end-to-end rendering through the real transform pipeline.
 *
 *  Registered through task_support/check.h in three groups:
 *    timed_math   - smoothing math and timestamp ownership/validation
 *    timed_io     - timed file round trip and malformed-input rejection
 *    timed_render - PTS queries interleaved with the legacy cursor, and pixel
 *                   comparison against an independently computed correction
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "libvidstab.h"
#include "transform.h"
#include "transformtype_operations.h"
#include "serialize.h"
#include "task_support/check.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define TASK_CASE(group, name) { (group), #name, (name) }

static int near_(double a, double b, double eps){
  return fabs(a - b) <= eps;
}

/* bit-pattern finite test, independent of the -ffast-math isfinite() fold */
static int isFinite(double v){
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return (bits & UINT64_C(0x7FF0000000000000)) != UINT64_C(0x7FF0000000000000);
}

/* the six smoothed float fields, in VSTransform declaration order */
static double getField(const VSTransform* t, int field){
  switch(field){
   case 0: return t->x;
   case 1: return t->y;
   case 2: return t->alpha;
   case 3: return t->zoom;
   case 4: return t->barrel;
   default:return t->rshutter;
  }
}

static void setField(VSTransform* t, int field, double v){
  switch(field){
   case 0: t->x = v; break;
   case 1: t->y = v; break;
   case 2: t->alpha = v; break;
   case 3: t->zoom = v; break;
   case 4: t->barrel = v; break;
   default:t->rshutter = v; break;
  }
}

static VSTransform mkTrans(double x, double y, double alpha, double zoom,
                           double barrel, double rshutter, int extra){
  VSTransform t = null_transform();
  t.x = x; t.y = y; t.alpha = alpha; t.zoom = zoom;
  t.barrel = barrel; t.rshutter = rshutter; t.extra = extra;
  return t;
}

/* builds a timed sequence that owns its arrays */
static int buildTimed(VSTransformations* tr, const VSTransform* ts,
                      const int64_t* pts, int n){
  vsTransformationsInit(tr);
  tr->ts = vs_malloc(sizeof(VSTransform) * (size_t)n);
  if(!tr->ts) return VS_ERROR;
  memcpy(tr->ts, ts, sizeof(VSTransform) * (size_t)n);
  tr->len = n;
  return vsTransformationsSetTimestamps(tr, pts, n);
}

static VSTransformConfig timedConfig(const char* name){
  VSTransformConfig c = vsTransformGetDefaultConfig(name);
  c.relative = 0;
  c.smoothing = 0;
  c.optZoom = 0;
  c.zoom = 0;
  c.crop = VSCropBorder;
  c.interpolType = VS_BiLinear;
  c.camPathAlgo = VSTimeBox;
  c.lensCorrection = VSLensCorrectOff;
  c.lensK = 0.0;
  c.fov = 0.0;
  c.smoothingTime = 100000;
  return c;
}

/* ============================ timed_math ================================== */

/* The worked example from the specification: unequal spacing, time integral
 * rather than a three-sample average. */
static int math_unequal_spacing(void){
  VSFrameInfo fi; VSTransformData td; VSTransformations tr;
  VSTransform ts[3] = { mkTrans(0,0,0,0,0,0,0), mkTrans(10,0,0,0,0,0,0),
                        mkTrans(0,0,0,0,0,0,0) };
  int64_t pts[3] = {0, 100000, 900000};
  const double expect[3] = {-2.5, 2.8125, -0.3125};
  int i;
  TASK_CHECK(vsFrameInfoInit(&fi, 32, 24, PF_GRAY8));
  VSTransformConfig cfg = timedConfig("math_unequal");
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(buildTimed(&tr, ts, pts, 3) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  for(i = 0; i < 3; i++){
    TASK_CHECK(near_(tr.ts[i].x, expect[i], 1e-9));
    if(i == 1){
      /* a plain sample average would give 10 - 10/3 at the middle node */
      TASK_CHECK(fabs(tr.ts[i].x - (10.0 - 10.0/3.0)) > 1.0);
    }
    /* only x was driven; every other field stays zero */
    TASK_CHECK(near_(tr.ts[i].y, 0, 1e-12));
    TASK_CHECK(near_(tr.ts[i].alpha, 0, 1e-12));
    TASK_CHECK(near_(tr.ts[i].zoom, 0, 1e-12));
    TASK_CHECK(near_(tr.ts[i].barrel, 0, 1e-12));
    TASK_CHECK(near_(tr.ts[i].rshutter, 0, 1e-12));
  }
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* Relative deltas accumulate to the same absolute path, so the result must
 * match the absolute (relative=0) run field for field. */
static int math_relative_matches_absolute(void){
  VSFrameInfo fi; VSTransformData tdAbs, tdRel;
  VSTransformations absTr, relTr;
  VSTransform absTs[3] = { mkTrans(0,1,0,0,0,0,0), mkTrans(10,3,0,0,0,0,0),
                           mkTrans(0,9,0,0,0,0,0) };
  /* relative steps whose running sum is the absolute path above */
  VSTransform relTs[3] = { mkTrans(0,1,0,0,0,0,0),  mkTrans(10,2,0,0,0,0,0),
                           mkTrans(-10,6,0,0,0,0,0) };
  int64_t pts[3] = {0, 100000, 900000};
  int i;
  TASK_CHECK(vsFrameInfoInit(&fi, 32, 24, PF_GRAY8));
  VSTransformConfig cfgAbs = timedConfig("math_rel_abs");
  VSTransformConfig cfgRel = cfgAbs;
  cfgRel.relative = 1;
  TASK_CHECK(vsTransformDataInit(&tdAbs, &cfgAbs, &fi, &fi) == VS_OK);
  TASK_CHECK(vsTransformDataInit(&tdRel, &cfgRel, &fi, &fi) == VS_OK);
  TASK_CHECK(buildTimed(&absTr, absTs, pts, 3) == VS_OK);
  TASK_CHECK(buildTimed(&relTr, relTs, pts, 3) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&tdAbs, &absTr) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&tdRel, &relTr) == VS_OK);
  for(i = 0; i < 3; i++){
    TASK_CHECK(near_(relTr.ts[i].x, absTr.ts[i].x, 1e-9));
    TASK_CHECK(near_(relTr.ts[i].y, absTr.ts[i].y, 1e-9));
  }
  vsTransformationsCleanup(&absTr);
  vsTransformationsCleanup(&relTr);
  vsTransformDataCleanup(&tdAbs);
  vsTransformDataCleanup(&tdRel);
  return 0;
}

/* A window spanning several nodes of a uniform ramp.  Hand-computed box
 * averages, including the clamped endpoint tail. */
static int math_multi_node_window(void){
  enum { N = 5 };
  VSFrameInfo fi; VSTransformData td; VSTransformations tr;
  VSTransform ts[N]; int64_t pts[N]; int i;
  /* path x = 0,1,2,3,4 at 0,100000,...,400000; h = 200000 */
  for(i = 0; i < N; i++){
    ts[i] = mkTrans((double)i, 0, 0, 0, 0, 0, 0);
    pts[i] = (int64_t)i * 100000;
  }
  TASK_CHECK(vsFrameInfoInit(&fi, 32, 24, PF_GRAY8));
  VSTransformConfig cfg = timedConfig("math_multi");
  cfg.smoothingTime = 200000;
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(buildTimed(&tr, ts, pts, N) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  /* middle node t=200000: window covers the whole ramp, mean value 2, P=2 */
  TASK_CHECK(near_(tr.ts[2].x, 0.0, 1e-9));
  /* first node: window [-200000,200000], integral 200000 over 400000 -> avg .5,
     P(0)=0, so correction -0.5 */
  TASK_CHECK(near_(tr.ts[0].x, -0.5, 1e-9));
  /* last node: symmetric, P(400000)=4 minus average 3.5 -> +0.5 */
  TASK_CHECK(near_(tr.ts[4].x, 0.5, 1e-9));
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* alpha is integrated as a continuously unwrapped angle: a value beyond +/-pi
 * is smoothed as an ordinary scalar, never wrapped to mod 2pi. */
static int math_unwrapped_alpha(void){
  enum { N = 3 };
  VSFrameInfo fi; VSTransformData td; VSTransformations tr;
  VSTransform ts[N] = { mkTrans(0,0,0.0,0,0,0,0),
                        mkTrans(0,0,7.0,0,0,0,0),   /* beyond 2pi */
                        mkTrans(0,0,7.0,0,0,0,0) };
  int64_t pts[N] = {0, 100000, 900000};
  TASK_CHECK(vsFrameInfoInit(&fi, 32, 24, PF_GRAY8));
  VSTransformConfig cfg = timedConfig("math_alpha");
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(buildTimed(&tr, ts, pts, N) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  /* Hand values for the continuous (unwrapped) path 0 -> 7 -> 7, h = 100000:
   *   node 0: window mean 1.75, P=0   -> correction -1.75
   *   node 1: window mean 5.25, P=7   -> correction +1.75
   *   node 2: window mean 7,    P=7   -> correction  0
   * Wrapping 7 to 7-2pi (~0.717) first would scale every result by 0.717/7,
   * so matching these exactly is the no-wrap guarantee. */
  TASK_CHECK(near_(tr.ts[0].alpha, -1.75, 1e-9));
  TASK_CHECK(near_(tr.ts[1].alpha,  1.75, 1e-9));
  TASK_CHECK(near_(tr.ts[2].alpha,  0.0,  1e-9));
  TASK_CHECK(isFinite(tr.ts[0].alpha));
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

static int math_set_timestamps_validation(void){
  enum { N = 3 };
  VSTransform ts[N] = { mkTrans(0,0,0,0,0,0,0), mkTrans(1,0,0,0,0,0,0),
                        mkTrans(2,0,0,0,0,0,0) };
  int64_t good[N] = {0, 100000, 200000};
  int64_t neg[N]  = {-1, 100000, 200000};
  int64_t flat[N] = {0, 100000, 100000};
  int64_t scratch[N];
  VSTransformations tr;
  vsTransformationsInit(&tr);
  tr.ts = vs_malloc(sizeof(VSTransform) * N);
  memcpy(tr.ts, ts, sizeof(ts));
  tr.len = N;

  TASK_CHECK(vsTransformationsSetTimestamps(&tr, good, N) == VS_OK);
  /* count must match the non-empty length */
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, good, N - 1) == VS_ERROR);
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, NULL, N) == VS_ERROR);
  TASK_CHECK(vsTransformationsSetTimestamps(NULL, good, N) == VS_ERROR);
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, neg, N) == VS_ERROR);
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, flat, N) == VS_ERROR);
  /* failed validation must not dislodge the previously held good array */
  TASK_CHECK(tr.pts != NULL && tr.pts[1] == 100000);

  /* ownership: mutating the caller's buffer after the copy has no effect */
  memcpy(scratch, good, sizeof(good));
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, scratch, N) == VS_OK);
  scratch[1] = 999999;
  TASK_CHECK(tr.pts[1] == 100000);

  /* replacing the array frees the old one and installs the new copy */
  good[0] = 5;
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, good, N) == VS_OK);
  TASK_CHECK(tr.pts[0] == 5);

  vsTransformationsCleanup(&tr);
  TASK_CHECK(tr.pts == NULL && tr.ts == NULL);
  return 0;
}

static int math_unsupported_config(void){
  enum { N = 2 };
  VSFrameInfo fi; VSTransformData td; VSTransformations tr;
  VSTransform ts[N] = { mkTrans(0,0,0,0,0,0,0), mkTrans(1,0,0,0,0,0,0) };
  int64_t pts[N] = {0, 100000};
  VSTransformConfig cfg = timedConfig("math_unsup");
  TASK_CHECK(vsFrameInfoInit(&fi, 32, 24, PF_GRAY8));

  /* missing PTS */
  vsTransformationsInit(&tr);
  tr.ts = vs_malloc(sizeof(VSTransform) * N);
  memcpy(tr.ts, ts, sizeof(ts)); tr.len = N;
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);

  TASK_CHECK(buildTimed(&tr, ts, pts, N) == VS_OK);

  /* window must be positive */
  td.conf.smoothingTime = 0;
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_ERROR);
  td.conf.smoothingTime = 100000;

  td.conf.optZoom = 1;
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_ERROR);
  td.conf.optZoom = 0;

  td.conf.lensCorrection = VSLensCorrectWobble;
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_ERROR);
  td.conf.lensCorrection = VSLensCorrectOff;

  td.conf.fov = 60.0;
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_ERROR);
  td.conf.fov = 0.0;

  tr.ts[0].extra = 1;                    /* scene-cut marker not allowed */
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_ERROR);
  tr.ts[0].extra = 0;

  tr.ts[0].x = NAN;                      /* illegal numeric value */
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_ERROR);

  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* huge absolute PTS must still resolve the microsecond gap between frames */
static int math_large_pts_gap(void){
  enum { N = 2 };
  VSTransformations tr;
  VSTransform ts[N] = { mkTrans(0,0,0,0,0,0,0), mkTrans(10,0,0,0,0,0,0) };
  const int64_t base = INT64_C(9000000000000000);
  int64_t pts[N] = {base, base + 100000};
  VSTransform out;
  TASK_CHECK(buildTimed(&tr, ts, pts, N) == VS_OK);
  TASK_CHECK(vsGetTransformAtPTS(NULL, &tr, base + 50000, &out) == VS_OK);
  TASK_CHECK(near_(out.x, 5.0, 1e-12));      /* exactly the midpoint */
  TASK_CHECK(vsGetTransformAtPTS(NULL, &tr, base, &out) == VS_OK);
  TASK_CHECK(near_(out.x, 0.0, 1e-12));
  TASK_CHECK(vsGetTransformAtPTS(NULL, &tr, base + 100000, &out) == VS_OK);
  TASK_CHECK(near_(out.x, 10.0, 1e-12));
  /* outside the range the endpoint is held */
  TASK_CHECK(vsGetTransformAtPTS(NULL, &tr, base + 5000000, &out) == VS_OK);
  TASK_CHECK(near_(out.x, 10.0, 1e-12));
  vsTransformationsCleanup(&tr);
  return 0;
}

/* ============================ timed_io =================================== */

static int io_roundtrip_doubles(void){
  enum { N = 4 };
  VSTransformations in, out;
  VSTransform ts[N] = {
    mkTrans(0.1, -0.2, 0.3, 0.4, 0.5, 0.6, 0),
    mkTrans(1.0/3.0, -1.0/7.0, 1e-12, 1e12, -1234567.89, 3.141592653589793, 0),
    mkTrans(-0.0, 2.5, -2.5, 100.0, -100.0, 0.0, 1),
    mkTrans(7.5, 8.25, 16.5, 32.125, 64.0625, 128.03125, 0)
  };
  int64_t pts[N] = {0, 100000, 250000, 900000};
  FILE* f = tmpfile();
  int i;
  TASK_CHECK(f != NULL);
  TASK_CHECK(buildTimed(&in, ts, pts, N) == VS_OK);
  TASK_CHECK(vsWriteTimedTransforms(f, &in) == VS_OK);
  rewind(f);
  vsTransformationsInit(&out);
  TASK_CHECK(vsReadTimedTransforms(f, &out) == VS_OK);
  TASK_CHECK(out.len == N && out.pts != NULL);
  for(i = 0; i < N; i++){
    TASK_CHECK(out.pts[i] == pts[i]);
    /* 17-digit text encoding must round-trip every double bit-for-bit */
    TASK_CHECK(memcmp(&out.ts[i].x, &in.ts[i].x, sizeof(double)) == 0);
    TASK_CHECK(memcmp(&out.ts[i].y, &in.ts[i].y, sizeof(double)) == 0);
    TASK_CHECK(memcmp(&out.ts[i].alpha, &in.ts[i].alpha, sizeof(double)) == 0);
    TASK_CHECK(memcmp(&out.ts[i].zoom, &in.ts[i].zoom, sizeof(double)) == 0);
    TASK_CHECK(memcmp(&out.ts[i].barrel, &in.ts[i].barrel, sizeof(double)) == 0);
    TASK_CHECK(memcmp(&out.ts[i].rshutter, &in.ts[i].rshutter, sizeof(double)) == 0);
    TASK_CHECK(out.ts[i].extra == in.ts[i].extra);
  }
  fclose(f);
  vsTransformationsCleanup(&in);
  vsTransformationsCleanup(&out);
  TASK_CHECK(out.pts == NULL && out.ts == NULL);
  return 0;
}

/* feed one malformed blob; the read must fail and leave the target clean */
static int expectReadFailure(const char* blob){
  VSTransformations tr;
  FILE* f = tmpfile();
  int rc;
  if(!f) return 0;
  fputs(blob, f);
  rewind(f);
  vsTransformationsInit(&tr);
  rc = vsReadTimedTransforms(f, &tr);
  fclose(f);
  if(rc != VS_ERROR) return 0;
  if(tr.len != 0 || tr.ts != NULL || tr.pts != NULL) return 0;
  vsTransformationsCleanup(&tr); /* must be safe after a failed read */
  return 1;
}

static int io_reject_malformed(void){
  TASK_CHECK(expectReadFailure("GARBAGE 1 1\n0 0 0 0 0 0 0 0\n"));          /* magic */
  TASK_CHECK(expectReadFailure("VID.STAB.TIMED 2 1\n0 0 0 0 0 0 0 0\n"));   /* version */
  TASK_CHECK(expectReadFailure("VID.STAB.TIMED 1 1 extra\n0 0 0 0 0 0 0 0\n")); /* hdr extra */
  TASK_CHECK(expectReadFailure("VID.STAB.TIMED 1 2\n0 0 0 0 0 0 0 0\n"));   /* missing line */
  TASK_CHECK(expectReadFailure(
    "VID.STAB.TIMED 1 1\n0 0 0 0 0 0 0 0\n1 0 0 0 0 0 0 0\n"));             /* extra line */
  TASK_CHECK(expectReadFailure(
    "VID.STAB.TIMED 1 1\n0 xx 0 0 0 0 0 0\n"));                             /* bad number */
  TASK_CHECK(expectReadFailure(
    "VID.STAB.TIMED 1 2\n0 0 0 0 0 0 0 0\n0 1 0 0 0 0 0 0\n"));            /* pts flat */
  TASK_CHECK(expectReadFailure(
    "VID.STAB.TIMED 1 2\n-5 0 0 0 0 0 0 0\n0 1 0 0 0 0 0 0\n"));           /* pts negative */
  TASK_CHECK(expectReadFailure(
    "VID.STAB.TIMED 1 1\n0 nan 0 0 0 0 0 0\n"));                           /* nan field */
  TASK_CHECK(expectReadFailure(
    "VID.STAB.TIMED 1 1\n0 inf 0 0 0 0 0 0\n"));                           /* inf field */
  TASK_CHECK(expectReadFailure(
    "VID.STAB.TIMED 1 1\n0 0 0 0 0 0 0 0 9\n"));                           /* extra column */
  TASK_CHECK(expectReadFailure("VID.STAB.TIMED 1 1\n"));                   /* no data line */
  return 0;
}

static int io_read_into_nonempty(void){
  enum { N = 2 };
  VSTransformations tr;
  VSTransform ts[N] = { mkTrans(0,0,0,0,0,0,0), mkTrans(1,0,0,0,0,0,0) };
  int64_t pts[N] = {0, 100000};
  FILE* f = tmpfile();
  TASK_CHECK(f != NULL);
  TASK_CHECK(buildTimed(&tr, ts, pts, N) == VS_OK);
  fputs("VID.STAB.TIMED 1 1\n0 0 0 0 0 0 0 0\n", f);
  rewind(f);
  /* reading into an object that already holds data is refused, not merged */
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  TASK_CHECK(tr.len == N); /* original content untouched */
  fclose(f);
  vsTransformationsCleanup(&tr);
  return 0;
}

/* ============================ timed_render =============================== */

/* independent piecewise-linear, end-clamped value of one field */
static double refFieldAt(const VSTransform* ts, const int64_t* pts, int n,
                         int field, int64_t t){
  int i;
  if(t <= pts[0]) return getField(&ts[0], field);
  if(t >= pts[n-1]) return getField(&ts[n-1], field);
  for(i = 0; i < n - 1; i++){
    if(t >= pts[i] && t <= pts[i+1]){
      double a = getField(&ts[i], field);
      double b = getField(&ts[i+1], field);
      double frac = (double)(t - pts[i]) / (double)(pts[i+1] - pts[i]);
      return a + frac * (b - a);
    }
  }
  return 0.0;
}

/* independent box integral of one field over [a,b], mirroring the library's
 * trapezoid arithmetic on int64 time differences */
static double refIntegral(const VSTransform* ts, const int64_t* pts, int n,
                          int field, int64_t a, int64_t b){
  double area = 0.0;
  int i;
  if(a < pts[0]){
    int64_t end = b < pts[0] ? b : pts[0];
    if(end > a) area += getField(&ts[0], field) * (double)(end - a);
  }
  for(i = 0; i < n - 1; i++){
    int64_t lo = pts[i], hi = pts[i+1];
    int64_t s = a > lo ? a : lo;
    int64_t e = b < hi ? b : hi;
    if(s < e){
      double v0 = getField(&ts[i], field);
      double v1 = getField(&ts[i+1], field);
      double d = (double)(hi - lo);
      double vs = v0 + (v1 - v0) * ((double)(s - lo) / d);
      double ve = v0 + (v1 - v0) * ((double)(e - lo) / d);
      area += 0.5 * (vs + ve) * (double)(e - s);
    }
  }
  if(b > pts[n-1]){
    int64_t start = a > pts[n-1] ? a : pts[n-1];
    if(b > start) area += getField(&ts[n-1], field) * (double)(b - start);
  }
  return area;
}

static int render_query_cursor_interleave(void){
  enum { N = 4 };
  VSFrameInfo fi; VSTransformData td; VSTransformations tr;
  VSTransform ts[N] = { mkTrans(0,0,0,0,0,0,0), mkTrans(4,1,0,0,0,0,0),
                        mkTrans(2,5,0,0,0,0,0), mkTrans(6,2,0,0,0,0,0) };
  int64_t pts[N] = {0, 100000, 300000, 700000};
  VSTransform out;
  TASK_CHECK(vsFrameInfoInit(&fi, 32, 24, PF_GRAY8));
  VSTransformConfig cfg = timedConfig("render_cursor");
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(buildTimed(&tr, ts, pts, N) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);

  /* error conditions of the query interface */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, -1, &out) == VS_ERROR);
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 100000, NULL) == VS_ERROR);
  {
    VSTransformations empty;
    vsTransformationsInit(&empty);
    TASK_CHECK(vsGetTransformAtPTS(&td, &empty, 0, &out) == VS_ERROR);
    vsTransformationsCleanup(&empty);
  }

  /* reverse-order and repeated random queries do not move the cursor */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 700000, &out) == VS_OK);
  TASK_CHECK(near_(out.x, tr.ts[3].x, 1e-12));
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 50000, &out) == VS_OK);
  TASK_CHECK(near_(out.x, refFieldAt(tr.ts, pts, N, 0, 50000), 1e-12));
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 50000, &out) == VS_OK); /* repeat */

  /* interleave with the legacy cursor; PTS queries leave current untouched */
  TASK_CHECK(vsGetNextTransform(&td, &tr).x == tr.ts[0].x);
  TASK_CHECK(tr.current == 1);
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 200000, &out) == VS_OK);
  TASK_CHECK(near_(out.x, refFieldAt(tr.ts, pts, N, 0, 200000), 1e-12));
  TASK_CHECK(tr.current == 1);
  TASK_CHECK(vsGetNextTransform(&td, &tr).x == tr.ts[1].x);
  TASK_CHECK(tr.current == 2);

  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

static int framesPixelEqual(const VSFrame* a, const VSFrame* b, int w, int h){
  int x, y;
  for(y = 0; y < h; y++)
    for(x = 0; x < w; x++)
      if(a->data[0][y*a->linesize[0]+x] != b->data[0][y*b->linesize[0]+x])
        return 0;
  return 1;
}

/* The queried correction drives the real Prepare/DoTransform/Finish pipeline
 * and must render identically to an independently computed correction. */
static int render_pixel_match(void){
  enum { N = 3 };
  const int64_t h = 100000;
  VSFrameInfo fi; VSTransformData td; VSTransformations tr;
  VSTransform raw[N] = { mkTrans(0,0,0,0,0,0,0), mkTrans(10,4,0,0,0,0,0),
                         mkTrans(0,2,0,0,0,0,0) };
  VSTransform path[N]; /* absolute input path P (relative=0) */
  int64_t pts[N] = {0, 100000, 900000};
  VSFrame src, dstQuery, dstRef;
  int x, y, field;
  TASK_CHECK(vsFrameInfoInit(&fi, 32, 24, PF_GRAY8));
  VSTransformConfig cfg = timedConfig("render_pixel");
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(buildTimed(&tr, raw, pts, N) == VS_OK);
  memcpy(path, raw, sizeof(raw)); /* keep P before preprocessing overwrites ts */
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);

  vsFrameAllocate(&src, &fi);
  vsFrameAllocate(&dstQuery, &fi);
  vsFrameAllocate(&dstRef, &fi);
  for(y = 0; y < fi.height; y++)
    for(x = 0; x < fi.width; x++)
      src.data[0][y*src.linesize[0]+x] = (uint8_t)((x*7 + y*11) % 251);

  /* (1) node query: independent box average of the raw path P, recomputed from
   * scratch, must equal the library's stored node and the queried value. */
  {
    int64_t qt = pts[1];
    VSTransform tq, tInd = null_transform();
    TASK_CHECK(vsGetTransformAtPTS(&td, &tr, qt, &tq) == VS_OK);
    for(field = 0; field < 6; field++){
      double avg = refIntegral(path, pts, N, field, qt - h, qt + h)
                   / (double)(2 * h);
      setField(&tInd, field, getField(&path[1], field) - avg);
    }
    for(field = 0; field < 6; field++){
      TASK_CHECK(near_(getField(&tq, field), getField(&tr.ts[1], field), 1e-12));
      TASK_CHECK(near_(getField(&tq, field), getField(&tInd, field), 1e-9));
    }
    TASK_CHECK(vsTransformPrepare(&td, &src, &dstQuery) == VS_OK);
    TASK_CHECK(vsDoTransform(&td, tq) == VS_OK);
    TASK_CHECK(vsTransformFinish(&td) == VS_OK);
    TASK_CHECK(vsTransformPrepare(&td, &src, &dstRef) == VS_OK);
    TASK_CHECK(vsDoTransform(&td, tInd) == VS_OK);
    TASK_CHECK(vsTransformFinish(&td) == VS_OK);
    TASK_CHECK(framesPixelEqual(&dstQuery, &dstRef, fi.width, fi.height));
  }

  /* (2) between-node query: independent linear interpolation of the
   * preprocessed nodes, rendered through the same pipeline, pixel-identical */
  {
    int64_t qt = 200000;
    VSTransform tq, tref = null_transform();
    TASK_CHECK(vsGetTransformAtPTS(&td, &tr, qt, &tq) == VS_OK);
    for(field = 0; field < 6; field++)
      setField(&tref, field, refFieldAt(tr.ts, pts, N, field, qt));
    for(field = 0; field < 6; field++)
      TASK_CHECK(getField(&tq, field) == getField(&tref, field));
    /* a genuinely non-identity correction, so the warp really happened */
    TASK_CHECK(fabs(tq.x) > 1e-9 || fabs(tq.y) > 1e-9);

    TASK_CHECK(vsTransformPrepare(&td, &src, &dstQuery) == VS_OK);
    TASK_CHECK(vsDoTransform(&td, tq) == VS_OK);
    TASK_CHECK(vsTransformFinish(&td) == VS_OK);
    TASK_CHECK(vsTransformPrepare(&td, &src, &dstRef) == VS_OK);
    TASK_CHECK(vsDoTransform(&td, tref) == VS_OK);
    TASK_CHECK(vsTransformFinish(&td) == VS_OK);
    TASK_CHECK(framesPixelEqual(&dstQuery, &dstRef, fi.width, fi.height));
    TASK_CHECK(!framesPixelEqual(&dstQuery, &src, fi.width, fi.height));
  }

  vsFrameFree(&src); vsFrameFree(&dstQuery); vsFrameFree(&dstRef);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

const struct task_case task_cases[] = {
  TASK_CASE("timed_math",   math_unequal_spacing),
  TASK_CASE("timed_math",   math_relative_matches_absolute),
  TASK_CASE("timed_math",   math_multi_node_window),
  TASK_CASE("timed_math",   math_unwrapped_alpha),
  TASK_CASE("timed_math",   math_set_timestamps_validation),
  TASK_CASE("timed_math",   math_unsupported_config),
  TASK_CASE("timed_math",   math_large_pts_gap),
  TASK_CASE("timed_io",     io_roundtrip_doubles),
  TASK_CASE("timed_io",     io_reject_malformed),
  TASK_CASE("timed_io",     io_read_into_nonempty),
  TASK_CASE("timed_render", render_query_cursor_interleave),
  TASK_CASE("timed_render", render_pixel_match),
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
