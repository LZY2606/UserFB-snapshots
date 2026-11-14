/*
 * timed_path.c -- regression tests for time-based camera-path smoothing.
 *
 * Covers three areas:
 *   timed_math    the VSTimeBox integral itself, relative/absolute input,
 *                 multi-node windows, continuous angles, rejected configs
 *   timed_io      the VID.STAB.TIMED text format, timestamp ownership and
 *                 the PTS query interface
 *   timed_render  the queried corrections fed through the real transform
 *                 pipeline, compared pixel-for-pixel against independently
 *                 computed corrections
 */

/* Relative path so the file builds both inside the tree's CMake test build
   and with the task runner, which only adds -I. and -Isrc. */
#include "../task_support/check.h"
#include "libvidstab.h"
#include "transform.h"
#include "transformtype_operations.h"
#include "serialize.h"
#include "frameinfo.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TP_W 32
#define TP_H 24

/* ---- helpers --------------------------------------------------------- */

static int seqAlloc(VSTransformations* tr, int n){
  vsTransformationsInit(tr);
  tr->ts = (VSTransform*)vs_malloc(sizeof(VSTransform) * n);
  if(!tr->ts) return VS_ERROR;
  tr->len = n;
  for(int i=0; i<n; i++) tr->ts[i] = null_transform();
  return VS_OK;
}

/* A transform data object configured for the time-box mode.  Everything the
   mode does not support is switched off explicitly, like a caller who wants
   only the time smoothing would. */
static int makeTimedTd(VSTransformData* td, VSTransformConfig* cfg,
                       const VSFrameInfo* fi, int relative, int64_t h){
  *cfg = vsTransformGetDefaultConfig("timed-path-test");
  cfg->relative            = relative;
  cfg->smoothingTime       = h;
  cfg->camPathAlgo         = VSTimeBox;
  cfg->optZoom             = 0;
  cfg->lensCorrection      = VSLensCorrectOff;
  cfg->estimateLensDistortion = 0;
  cfg->fov                 = 0.0;
  cfg->invert              = 0;
  cfg->maxShift            = -1;
  cfg->maxAngle            = -1.0;
  cfg->zoom                = 0.0;
  cfg->crop                = VSCropBorder;
  return vsTransformDataInit(td, cfg, fi, fi);
}

/* ---- timed_math ------------------------------------------------------ */

/* The example from the design: PTS 0/100000/900000, x 0/10/0, absolute
   input, h = 100000.  The window mean is the exact time integral, not the
   mean of the three samples. */
static int tpTimeBoxExample(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  const int64_t pts[3] = {0, 100000, 900000};
  const double ex[3] = {-2.5, 2.8125, -0.3125};

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 100000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, 3) == VS_OK);
  tr.ts[0].x = 0.0;
  tr.ts[1].x = 10.0;
  tr.ts[2].x = 0.0;
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 3) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  for(int i=0; i<3; i++){
    TASK_CHECK(fabs(tr.ts[i].x - ex[i]) < 1e-12);
    /* all other fields stay zero */
    TASK_CHECK(tr.ts[i].y == 0.0 && tr.ts[i].alpha == 0.0);
    TASK_CHECK(tr.ts[i].zoom == 0.0 && tr.ts[i].barrel == 0.0);
    TASK_CHECK(tr.ts[i].rshutter == 0.0 && tr.ts[i].extra == 0);
  }
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* relative=1: the first transform is the absolute starting point and later
   ones are deltas.  x deltas 0,10,-10 integrate to the same absolute path as
   the example, so the corrections are the same. */
static int tpTimeBoxRelative(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  const int64_t pts[3] = {0, 100000, 900000};
  const double ex[3] = {-2.5, 2.8125, -0.3125};

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 1, 100000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, 3) == VS_OK);
  tr.ts[0].x = 0.0;
  tr.ts[1].x = 10.0;    /* relative motion 0 -> 10 */
  tr.ts[2].x = -10.0;   /* relative motion 10 -> 0 */
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 3) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  for(int i=0; i<3; i++)
    TASK_CHECK(fabs(tr.ts[i].x - ex[i]) < 1e-12);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* A window spanning several nodes.  PTS 0..400000 in 100000 steps, x equal
   to the frame index, h = 150000.  Expected values are the exact
   piecewise-linear window means worked out by hand (endpoint values held
   outside the sequence). */
static int tpTimeBoxMultiNode(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  const int64_t pts[5] = {0, 100000, 200000, 300000, 400000};
  const double ex[5] = {-0.375, -0.041666666666666666, 0.0,
                        0.041666666666666666, 0.375};

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 150000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, 5) == VS_OK);
  for(int i=0; i<5; i++) tr.ts[i].x = (double)i;
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 5) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  for(int i=0; i<5; i++)
    TASK_CHECK(fabs(tr.ts[i].x - ex[i]) < 1e-12);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* alpha is a continuous angle: an absolute input of 3.0 followed by the
   same angle written as -3.0 (i.e. 3.283 = -3 + 2*pi) must not average to a
   half turn.  h = 100000, single linear segment; expected corrections are
   +-0.5*delta*... as computed from the trapezoid. */
static int tpTimeBoxContinuousAngle(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  const int64_t pts[2] = {0, 100000};
  const double a0 = 3.0;
  const double a1 = -3.0 + 2.0*M_PI;
  double ex0, ex1;

  /* node 0: left half held at a0, right half linear 0..100000 */
  ex0 = a0 - (a0*100000.0 + 0.5*(a0+a1)*100000.0)/200000.0;
  /* node 1: linear 0..100000, right half held at a1 */
  ex1 = a1 - (0.5*(a0+a1)*100000.0 + a1*100000.0)/200000.0;
  TASK_CHECK(fabs(ex0) < 0.5 && fabs(ex1) < 0.5);

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 100000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, 2) == VS_OK);
  tr.ts[0].alpha = 3.0;
  tr.ts[1].alpha = -3.0;      /* the continuously unwrapped value is a1 */
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 2) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  TASK_CHECK(fabs(tr.ts[0].alpha - ex0) < 1e-12);
  TASK_CHECK(fabs(tr.ts[1].alpha - ex1) < 1e-12);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* All six fields are smoothed with the same rule.  A single node exercised
   per field with a known window mean would be trivial, so use two nodes and
   give every field a distinct discontinuity. */
static int tpTimeBoxSixFields(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  const int64_t pts[2] = {0, 100000};
  double vals[6];

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 100000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, 2) == VS_OK);
  for(int i=0; i<2; i++){
    tr.ts[i].x        = i ? 4.0 : 0.0;
    tr.ts[i].y        = i ? -2.0 : 0.0;
    tr.ts[i].alpha    = i ? 0.2 : 0.0;
    tr.ts[i].zoom     = i ? 1.0 : 0.0;
    tr.ts[i].barrel   = i ? 0.5 : 0.0;
    tr.ts[i].rshutter = i ? 0.3 : 0.0;
  }
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 2) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  vals[0] = tr.ts[0].x;  vals[1] = tr.ts[0].y;  vals[2] = tr.ts[0].alpha;
  vals[3] = tr.ts[0].zoom; vals[4] = tr.ts[0].barrel; vals[5] = tr.ts[0].rshutter;
  /* node 0: window [-h,h] holds 0 for the left half and averages 0..v over
     the right half -> correction -v/4.  Node 1: mirror -> +v/4. */
  TASK_CHECK(fabs(vals[0] - (-1.0)) < 1e-12);
  TASK_CHECK(fabs(vals[1] - ( 0.5)) < 1e-12);
  TASK_CHECK(fabs(vals[2] - (-0.05)) < 1e-12);
  TASK_CHECK(fabs(vals[3] - (-0.25)) < 1e-12);
  TASK_CHECK(fabs(vals[4] - (-0.125)) < 1e-12);
  TASK_CHECK(fabs(vals[5] - (-0.075)) < 1e-12);
  TASK_CHECK(fabs(tr.ts[1].x - 1.0) < 1e-12);
  TASK_CHECK(fabs(tr.ts[1].y - (-0.5)) < 1e-12);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* Unsupported configurations and missing timestamps are rejected outright. */
static int tpTimeBoxRejects(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  const int64_t pts[2] = {0, 100000};
  int rc;

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));

#define EXPECT_REJECT(modify, h)                                        \
  do {                                                                  \
    cfg = vsTransformGetDefaultConfig("timed-reject");                  \
    cfg.relative = 0; cfg.smoothingTime = (h); cfg.camPathAlgo = VSTimeBox; \
    cfg.optZoom = 0; cfg.lensCorrection = VSLensCorrectOff;             \
    cfg.estimateLensDistortion = 0; cfg.fov = 0.0;                      \
    cfg.maxShift = -1; cfg.maxAngle = -1.0; cfg.zoom = 0.0;             \
    modify;                                                             \
    TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);      \
    rc = vsPreprocessTransforms(&td, &tr);                              \
    vsTransformDataCleanup(&td);                                        \
    TASK_CHECK(rc == VS_ERROR);                                         \
  } while(0)

  TASK_CHECK(seqAlloc(&tr, 2) == VS_OK);
  tr.ts[0].x = 0.0; tr.ts[1].x = 1.0;
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 2) == VS_OK);

  EXPECT_REJECT((void)0, 0);                       /* no positive window   */
  EXPECT_REJECT(cfg.optZoom = 1, 100000);          /* optZoom              */
  EXPECT_REJECT(cfg.lensCorrection = VSLensCorrectWobble, 100000);
  EXPECT_REJECT(cfg.fov = 30.0, 100000);           /* rotational model     */
  EXPECT_REJECT({ tr.ts[0].extra = 1; }, 100000);  /* scene cut marker     */
  EXPECT_REJECT({ tr.ts[0].x = NAN; }, 100000);    /* non-finite value     */
#undef EXPECT_REJECT

  /* No PTS at all. */
  {
    VSTransformations bare;
    TASK_CHECK(seqAlloc(&bare, 2) == VS_OK);
    bare.ts[0].x = 0.0; bare.ts[1].x = 1.0;
    cfg = vsTransformGetDefaultConfig("timed-reject");
    cfg.relative = 0; cfg.smoothingTime = 100000; cfg.camPathAlgo = VSTimeBox;
    cfg.optZoom = 0; cfg.lensCorrection = VSLensCorrectOff;
    cfg.estimateLensDistortion = 0; cfg.fov = 0.0;
    TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
    TASK_CHECK(vsPreprocessTransforms(&td, &bare) == VS_ERROR);
    vsTransformationsCleanup(&bare);
    vsTransformDataCleanup(&td);
  }
  vsTransformationsCleanup(&tr);
  return 0;
}

/* The old frame-order algorithms keep their behaviour when a timestamp
   array is present but not used: a smooth input path must come out of
   VSGaussian/VSAvg unchanged in the way those algorithms already produce,
   and smoothingTime alone switches nothing on. */
static int tpOldModesIgnoreTimestamps(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  const int64_t pts[5] = {0, 1000, 5000, 9000, 20000};
  VSTransform in[5];
  double expect[5];
  int i;

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  for(i=0; i<5; i++){
    in[i] = null_transform();
    in[i].x = (double)i;
  }
  /* VSGaussian with relative=0 and a linear path: the gaussian average of a
     straight line is the line, so x only needs to be shifted to the local
     mean.  The key point is that this path does not use the microsecond
     timestamps at all: the same input over completely different timebases
     must give the same result. */
  cfg = vsTransformGetDefaultConfig("timed-old");
  cfg.relative = 0; cfg.smoothingTime = 100000; cfg.camPathAlgo = VSGaussian;
  cfg.optZoom = 0; cfg.lensCorrection = VSLensCorrectOff;
  cfg.estimateLensDistortion = 0;
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, 5) == VS_OK);
  for(i=0; i<5; i++) tr.ts[i] = in[i];
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 5) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  for(i=0; i<5; i++) expect[i] = tr.ts[i].x;

  /* different, even irregular timebase; same frame order -> same answer */
  {
    const int64_t pts2[5] = {5, 6, 7, 8, 9};
    VSTransformations tr2;
    TASK_CHECK(seqAlloc(&tr2, 5) == VS_OK);
    for(i=0; i<5; i++) tr2.ts[i] = in[i];
    TASK_CHECK(vsTransformationsSetTimestamps(&tr2, pts2, 5) == VS_OK);
    TASK_CHECK(vsPreprocessTransforms(&td, &tr2) == VS_OK);
    for(i=0; i<5; i++) TASK_CHECK(tr2.ts[i].x == expect[i]);
    vsTransformationsCleanup(&tr2);
  }

  /* and VSAvg, with a timestamp array present, must also ignore it: equal
     output over a different timebase (the two algorithms differ from each
     other, so compare each against itself). */
  {
    VSTransformations tr3, tr4;
    const int64_t pts2[5] = {1000000, 1000001, 1000002, 1000003, 1000004};
    cfg.camPathAlgo = VSAvg;
    vsTransformDataCleanup(&td);
    TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
    TASK_CHECK(seqAlloc(&tr3, 5) == VS_OK);
    for(i=0; i<5; i++) tr3.ts[i] = in[i];
    TASK_CHECK(vsTransformationsSetTimestamps(&tr3, pts, 5) == VS_OK);
    TASK_CHECK(vsPreprocessTransforms(&td, &tr3) == VS_OK);
    TASK_CHECK(seqAlloc(&tr4, 5) == VS_OK);
    for(i=0; i<5; i++) tr4.ts[i] = in[i];
    TASK_CHECK(vsTransformationsSetTimestamps(&tr4, pts2, 5) == VS_OK);
    TASK_CHECK(vsPreprocessTransforms(&td, &tr4) == VS_OK);
    for(i=0; i<5; i++) TASK_CHECK(tr3.ts[i].x == tr4.ts[i].x);
    vsTransformationsCleanup(&tr3);
    vsTransformationsCleanup(&tr4);
  }

  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* ---- query interface ------------------------------------------------- */

static int tpQueryErrors(void){
  VSTransformData td;
  VSTransformConfig cfg;
  VSFrameInfo fi;
  VSTransformations tr, empty;
  VSTransform out;
  const int64_t pts[2] = {0, 100};

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  cfg = vsTransformGetDefaultConfig("timed-query");
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);

  vsTransformationsInit(&empty);
  TASK_CHECK(vsGetTransformAtPTS(&td, &empty, 0, &out) == VS_ERROR);

  TASK_CHECK(seqAlloc(&tr, 2) == VS_OK);
  tr.ts[0].x = 0.0; tr.ts[1].x = 10.0;
  /* no timestamps yet */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 0, &out) == VS_ERROR);
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 2) == VS_OK);
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, -1, &out) == VS_ERROR);
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 50, NULL) == VS_ERROR);
  TASK_CHECK(vsGetTransformAtPTS(&td, NULL, 50, &out) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* Endpoints, interpolation, descending/repeated queries, the untouched
   cursor, and a large absolute PTS whose small offsets still matter. */
static int tpQueryInterpolation(void){
  VSTransformData td;
  VSTransformConfig cfg;
  VSFrameInfo fi;
  VSTransformations tr;
  VSTransform q, seq;
  const int64_t base = (int64_t)1000 * 1000 * 1000 * 1000 * 10; /* 1e16 us */
  int64_t pts[4];
  int i;

  pts[0] = base;
  pts[1] = base + 1000;      /* 1 ms later */
  pts[2] = base + 3000;
  pts[3] = base + 7000;

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  cfg = vsTransformGetDefaultConfig("timed-query");
  TASK_CHECK(vsTransformDataInit(&td, &cfg, &fi, &fi) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, 4) == VS_OK);
  for(i=0; i<4; i++){
    tr.ts[i] = null_transform();
    tr.ts[i].x        = 2.0*i;
    tr.ts[i].y        = -1.0*i;
    tr.ts[i].alpha    = 0.01*i;
    tr.ts[i].zoom     = 0.5*i;
    tr.ts[i].barrel   = 0.02*i;
    tr.ts[i].rshutter = 0.03*i;
    tr.ts[i].extra    = i;   /* copied from the lower node */
  }
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 4) == VS_OK);

  /* before the first and after the last: endpoint values */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, 0, &q) == VS_OK);
  TASK_CHECK(q.x == 0.0 && q.alpha == 0.0 && q.extra == 0);
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, base + 100000, &q) == VS_OK);
  TASK_CHECK(q.x == 6.0 && q.y == -3.0 && q.zoom == 1.5 && q.extra == 3);

  /* exactly on a node */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, pts[2], &q) == VS_OK);
  TASK_CHECK(q.x == 4.0 && q.y == -2.0 && q.barrel == 0.04 && q.extra == 2);

  /* halfway through the 1 ms interval: exactly the average, even though
     base is 1e16 and the interesting part is 500 us */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, base + 500, &q) == VS_OK);
  TASK_CHECK(fabs(q.x - 1.0) < 1e-12);
  TASK_CHECK(fabs(q.y - (-0.5)) < 1e-12);
  TASK_CHECK(fabs(q.alpha - 0.005) < 1e-15);
  TASK_CHECK(fabs(q.zoom - 0.25) < 1e-12);
  TASK_CHECK(fabs(q.barrel - 0.01) < 1e-15);
  TASK_CHECK(fabs(q.rshutter - 0.015) < 1e-15);

  /* 1100 us into the 2000 us interval [pts1,pts2] -> weight 0.55 */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, base + 2100, &q) == VS_OK);
  TASK_CHECK(fabs(q.x - (2.0 + 2.0*0.55)) < 1e-12);

  /* The cursor starts at 0 and queries do not move it.  Interleave them:
     two sequential reads, then a query in the middle of the sequence, then
     the third sequential read must still be the next one. */
  TASK_CHECK(tr.current == 0);
  seq = vsGetNextTransform(&td, &tr);
  TASK_CHECK(tr.current == 1 && seq.x == 0.0);
  seq = vsGetNextTransform(&td, &tr);
  TASK_CHECK(tr.current == 2 && seq.x == 2.0);
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, pts[3], &q) == VS_OK);
  TASK_CHECK(tr.current == 2);
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, pts[0], &q) == VS_OK);
  TASK_CHECK(tr.current == 2);
  seq = vsGetNextTransform(&td, &tr);
  TASK_CHECK(tr.current == 3 && seq.x == 4.0);
  /* repeated query gives the same answer */
  TASK_CHECK(vsGetTransformAtPTS(&td, &tr, base + 500, &q) == VS_OK);
  TASK_CHECK(fabs(q.x - 1.0) < 1e-12);

  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* ---- timestamp ownership --------------------------------------------- */

static int tpTimestampOwnership(void){
  VSTransformations tr;
  int64_t pts[3] = {0, 100, 200};
  VSTransform q;

  TASK_CHECK(seqAlloc(&tr, 3) == VS_OK);
  tr.ts[0].x = 0.0; tr.ts[1].x = 1.0; tr.ts[2].x = 2.0;

  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 2) == VS_ERROR); /* count */
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, NULL, 3) == VS_ERROR); /* pts */
  pts[0] = -1;
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 3) == VS_ERROR); /* negative */
  pts[0] = 0; pts[1] = 0;
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 3) == VS_ERROR); /* not strict */
  pts[1] = 100;

  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 3) == VS_OK);
  /* The caller may reuse its buffer; the sequence must have its own copy. */
  pts[0] = 500; pts[1] = 600; pts[2] = 700;
  TASK_CHECK(vsGetTransformAtPTS(NULL, &tr, 50, &q) == VS_OK);
  TASK_CHECK(fabs(q.x - 0.5) < 1e-12);
  /* 500 is past the stored last PTS (200), not the caller's rewritten 700:
     the endpoint value is what comes back */
  TASK_CHECK(vsGetTransformAtPTS(NULL, &tr, 500, &q) == VS_OK);
  TASK_CHECK(q.x == 2.0);

  /* Replacing the array is allowed and releases the previous one. */
  {
    const int64_t pts2[3] = {1000, 1100, 1300};
    TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts2, 3) == VS_OK);
    TASK_CHECK(vsGetTransformAtPTS(NULL, &tr, 1050, &q) == VS_OK);
    TASK_CHECK(fabs(q.x - 0.5) < 1e-12);
  }

  vsTransformationsCleanup(&tr);
  TASK_CHECK(tr.ts == NULL && tr.pts == NULL && tr.len == 0);
  return 0;
}

/* ---- timed_io -------------------------------------------------------- */

/* Round trip of every double field bit-exactly, including values whose
   decimal form is long. */
static int tioRoundTrip(void){
  VSTransformations tr, rd;
  const int64_t pts[4] = {0, 33333, 900001, 1234567890123LL};
  const double vals[4][6] = {
    {0.1, -1.0/3.0, 1e-300, 1e300, -0.0, 2.2250738585072014e-308},
    {7.0/9.0, 12345.678901234567, 0.30000000000000004, -1e-12, 1.0, -2.5},
    {M_PI, -M_PI, 1.7976931348623157e308, 5e-324, 0.125, -0.0625},
    {42.0, -42.0, 0.0, 1.0, -1.0, 0.33333333333333331}
  };
  FILE* f = tmpfile();
  TASK_CHECK(f != NULL);

  TASK_CHECK(seqAlloc(&tr, 4) == VS_OK);
  for(int i=0; i<4; i++){
    tr.ts[i].x        = vals[i][0];
    tr.ts[i].y        = vals[i][1];
    tr.ts[i].alpha    = vals[i][2];
    tr.ts[i].zoom     = vals[i][3];
    tr.ts[i].barrel   = vals[i][4];
    tr.ts[i].rshutter = vals[i][5];
    tr.ts[i].extra    = i;
  }
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 4) == VS_OK);
  TASK_CHECK(vsWriteTimedTransforms(f, &tr) == VS_OK);

  /* header is exactly VID.STAB.TIMED 1 N */
  rewind(f);
  {
    char line[128];
    TASK_CHECK(fgets(line, sizeof(line), f) != NULL);
    TASK_CHECK(strcmp(line, "VID.STAB.TIMED 1 4\n") == 0);
  }
  rewind(f);

  vsTransformationsInit(&rd);
  TASK_CHECK(vsReadTimedTransforms(f, &rd) == VS_OK);
  TASK_CHECK(rd.len == 4);
  for(int i=0; i<4; i++){
    TASK_CHECK(rd.pts[i] == pts[i]);
    TASK_CHECK(memcmp(&rd.ts[i].x,        &tr.ts[i].x,        sizeof(double)) == 0);
    TASK_CHECK(memcmp(&rd.ts[i].y,        &tr.ts[i].y,        sizeof(double)) == 0);
    TASK_CHECK(memcmp(&rd.ts[i].alpha,    &tr.ts[i].alpha,    sizeof(double)) == 0);
    TASK_CHECK(memcmp(&rd.ts[i].zoom,     &tr.ts[i].zoom,     sizeof(double)) == 0);
    TASK_CHECK(memcmp(&rd.ts[i].barrel,   &tr.ts[i].barrel,   sizeof(double)) == 0);
    TASK_CHECK(memcmp(&rd.ts[i].rshutter, &tr.ts[i].rshutter, sizeof(double)) == 0);
    TASK_CHECK(rd.ts[i].extra == tr.ts[i].extra);
  }
  fclose(f);
  vsTransformationsCleanup(&tr);
  vsTransformationsCleanup(&rd);
  return 0;
}

/* A timed file can drive preprocessing and queries, including after the
   transforms that produced it have gone. */
static int tioFileDrivesSmoothing(void){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr, rd;
  const int64_t pts[3] = {0, 100000, 900000};
  VSTransform q;
  FILE* f = tmpfile();
  TASK_CHECK(f != NULL);

  TASK_CHECK(seqAlloc(&tr, 3) == VS_OK);
  tr.ts[0].x = 0.0; tr.ts[1].x = 10.0; tr.ts[2].x = 0.0;
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, 3) == VS_OK);
  TASK_CHECK(vsWriteTimedTransforms(f, &tr) == VS_OK);
  rewind(f);
  vsTransformationsInit(&rd);
  TASK_CHECK(vsReadTimedTransforms(f, &rd) == VS_OK);
  vsTransformationsCleanup(&tr);

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 100000) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &rd) == VS_OK);
  TASK_CHECK(fabs(rd.ts[0].x - (-2.5)) < 1e-12);
  TASK_CHECK(fabs(rd.ts[1].x - 2.8125) < 1e-12);
  TASK_CHECK(fabs(rd.ts[2].x - (-0.3125)) < 1e-12);
  TASK_CHECK(vsGetTransformAtPTS(&td, &rd, 450000, &q) == VS_OK);
  TASK_CHECK(fabs(q.x - (2.8125 + (-0.3125 - 2.8125)*350000.0/800000.0)) < 1e-12);

  fclose(f);
  vsTransformationsCleanup(&rd);
  vsTransformDataCleanup(&td);
  return 0;
}

/* Every kind of malformed input is an error, and the object remains safe to
   clean up afterwards. */
static int tioRejects(void){
  VSTransformations tr;
  FILE* f;

  /* bad version */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 2 1\n0 1 2 3 4 5 6 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  TASK_CHECK(tr.len == 0 && tr.ts == NULL && tr.pts == NULL);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* missing record */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 3\n0 1 0 0 0 0 0 0\n100 2 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  TASK_CHECK(tr.len == 0);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* one record too many */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 2\n0 1 0 0 0 0 0 0\n100 2 0 0 0 0 0 0\n"
        "900 3 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* non-numeric field */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 2\n0 1 0 0 0 0 0 0\n100 x 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* PTS not strictly increasing, and negative */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 2\n100 1 0 0 0 0 0 0\n100 2 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 1\n-1 1 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* missing count in the header */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1\n0 1 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* a line with trailing garbage */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 1\n0 1 0 0 0 0 0 0 junk\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* trailing garbage or a wrong prefix in the header */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 1 x\n0 1 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED1 1 1\n0 1 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* a non-finite value is rejected even though strtod reads "nan" */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 2\n0 1 0 0 0 0 0 0\n100 nan 0 0 0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* a line that is not a line (record split over two lines) */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 1\n0 1 0 0 0\n0 0 0 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* blank lines after the records are tolerated */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 1\n0 1 0 0 0 0 0 0\n\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_OK);
  TASK_CHECK(tr.len == 1 && tr.pts[0] == 0);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* reading into a non-empty sequence is refused */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 1\n0 1 0 0 0 0 0 0\n", f);
  rewind(f);
  TASK_CHECK(seqAlloc(&tr, 1) == VS_OK);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* the writer refuses a sequence with no timestamps */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  TASK_CHECK(seqAlloc(&tr, 2) == VS_OK);
  TASK_CHECK(vsWriteTimedTransforms(f, &tr) == VS_ERROR);
  vsTransformationsCleanup(&tr);
  fclose(f);

  /* empty sequence with zero records is legal */
  f = tmpfile();
  TASK_CHECK(f != NULL);
  fputs("VID.STAB.TIMED 1 0\n", f);
  rewind(f);
  vsTransformationsInit(&tr);
  TASK_CHECK(vsReadTimedTransforms(f, &tr) == VS_OK);
  TASK_CHECK(tr.len == 0 && tr.ts == NULL && tr.pts == NULL);
  vsTransformationsCleanup(&tr);
  fclose(f);
  return 0;
}


/* ---- timed_render ---------------------------------------------------- */

/* Simple, independent correction: for each node and field, integrate the
   absolute path over [t-h, t+h] by walking every breakpoint inside the
   window, holding the endpoint values outside the measured span.  Six
   fields, alpha unwrapped continuously.  This is the reference the library
   and the rendered pixels are checked against -- no running state, no
   special-casing, just the documented definition.  O(n^2) on purpose. */
#define TP_FIELDS 6

static double tpField(const VSTransform* t, int f){
  switch(f){
   case 0: return t->x;
   case 1: return t->y;
   case 2: return t->alpha;
   case 3: return t->zoom;
   case 4: return t->barrel;
   default: return t->rshutter;
  }
}

static void tpSetField(VSTransform* t, int f, double v){
  switch(f){
   case 0: t->x = v; break;
   case 1: t->y = v; break;
   case 2: t->alpha = v; break;
   case 3: t->zoom = v; break;
   case 4: t->barrel = v; break;
   default: t->rshutter = v; break;
  }
}

/* value of a piecewise-linear, endpoint-held path at time u */
static double tpRefValue(const double* P, const int64_t* pts, int n, int64_t u){
  int lo, hi;
  if(u <= pts[0])      return P[0];
  if(u >= pts[n-1])    return P[n-1];
  lo = 0; hi = n - 1;
  while(hi - lo > 1){
    int mid = lo + (hi - lo)/2;
    if(pts[mid] <= u) lo = mid; else hi = mid;
  }
  return P[lo] + (P[hi] - P[lo]) *
         ((double)(u - pts[lo]) / (double)(pts[hi] - pts[lo]));
}

static double tpRefWindowMean(const double* P, const int64_t* pts, int n,
                              int idx, int64_t h){
  int64_t t = pts[idx];
  int64_t a = t - h, b = t + h;
  double total = 0.0;
  int i;
  if(a < pts[0]){
    total += P[0] * ((double)pts[0] - (double)a);
    a = pts[0];
  }
  if(b > pts[n-1]){
    total += P[n-1] * ((double)b - (double)pts[n-1]);
    b = pts[n-1];
  }
  if(b > a){
    /* pieces: a -> each breakpoint in (a,b], then -> b */
    int64_t prev = a;
    double vprev = tpRefValue(P, pts, n, a);
    for(i=0; i<n; i++){
      if(pts[i] > a && pts[i] <= b){
        total += 0.5 * (vprev + P[i]) * ((double)pts[i] - (double)prev);
        prev = pts[i];
        vprev = P[i];
      }
    }
    if(b > prev)
      total += 0.5 * (vprev + tpRefValue(P, pts, n, b)) * ((double)b - (double)prev);
  }
  return total / (2.0 * (double)h);
}

/* The library-free correction for the whole sequence, matching the
   documented rule: relative input is integrated field-wise, absolute input
   is used as the path.  Alpha is unwrapped continuously. */
static void tpRefCorrections(const VSTransform* in, const int64_t* pts, int n,
                             int relative, int64_t h, VSTransform* out){
  double* P[TP_FIELDS];
  int i, f;
  for(f=0; f<TP_FIELDS; f++) P[f] = (double*)vs_malloc(sizeof(double) * n);
  for(f=0; f<TP_FIELDS; f++){
    double accum = 0.0;
    for(i=0; i<n; i++){
      double v = tpField(&in[i], f);
      if(relative && i > 0){
        accum += v;
        if(f == 2){
          int64_t turns = (int64_t)floor((v + M_PI) / (2.0*M_PI));
          accum -= turns * 2.0 * M_PI;
        }
      }else{
        accum = v;
        if(f == 2 && i > 0){
          double d = accum - P[2][i-1];
          int64_t turns = (int64_t)floor((d + M_PI) / (2.0*M_PI));
          accum -= turns * 2.0 * M_PI;
        }
      }
      P[f][i] = accum;
    }
  }
  for(i=0; i<n; i++){
    out[i] = null_transform();
    for(f=0; f<TP_FIELDS; f++)
      tpSetField(&out[i], f, P[f][i] - tpRefWindowMean(P[f], pts, n, i, h));
    out[i].extra = in[i].extra;
  }
  for(f=0; f<TP_FIELDS; f++) vs_free(P[f]);
}

/* ---- rendering comparison -------------------------------------------- */

/* Pixel-for-pixel equality of two rendered frames.  A checksum would catch
 * differences as well, but this also reports the offending coordinate, which
 * is what makes a failure here diagnosable. */
static int tpFramesEqual(const VSFrame* a, const VSFrame* b, const VSFrameInfo* fi){
  for(int y=0; y<fi->height; y++)
    for(int x=0; x<fi->width; x++)
      if(a->data[0][y*a->linesize[0] + x] != b->data[0][y*b->linesize[0] + x]){
        fprintf(stderr, "pixel mismatch at (%d,%d): %u vs %u\n", x, y,
                (unsigned)a->data[0][y*a->linesize[0] + x],
                (unsigned)b->data[0][y*b->linesize[0] + x]);
        return 0;
      }
  return 1;
}

/* The pure query result must equal an independently computed correction on
   a varied, irregular timebase.  This pins timed_render's correction source
   before it is fed to the warp. */
static int trQueryMatchesReference(void){
  const int n = 9;
  int64_t pts[9] = {0, 40000, 90000, 250000, 251000, 600000, 601000,
                    1300000, 3000000};
  VSTransform in[9];
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  VSTransform ref[9], q;
  int i;

  for(i=0; i<n; i++){
    in[i] = null_transform();
    in[i].x        = sin(0.7*i) * 12.0;
    in[i].y        = cos(0.45*i) * 9.0 - 0.3*i;
    in[i].alpha    = 0.05*sin(1.1*i) + 0.0007*i;
    in[i].zoom     = 0.2*i - 0.5;
    in[i].barrel   = 0.01*i;
    in[i].rshutter = -0.02*i;
  }
  tpRefCorrections(in, pts, n, 0, 250000, ref);

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 250000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, n) == VS_OK);
  for(i=0; i<n; i++) tr.ts[i] = in[i];
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, n) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  for(i=0; i<n; i++){
    TASK_CHECK(fabs(tr.ts[i].x - ref[i].x) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].y - ref[i].y) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].alpha - ref[i].alpha) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].zoom - ref[i].zoom) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].barrel - ref[i].barrel) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].rshutter - ref[i].rshutter) < 1e-12);
    /* querying exactly at a node returns the same value the sequence holds */
    TASK_CHECK(vsGetTransformAtPTS(&td, &tr, pts[i], &q) == VS_OK);
    TASK_CHECK(fabs(q.x - tr.ts[i].x) < 1e-12);
  }
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* The same reference check for relative input over an irregular timebase. */
static int trRelativeMatchesReference(void){
  const int n = 7;
  int64_t pts[7] = {100, 101000, 102000, 500000, 1500000, 1501000, 4000000};
  VSTransform in[7];
  VSTransform ref[7];
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  int i;

  for(i=0; i<n; i++){
    in[i] = null_transform();
    in[i].x        = 1.5 - 0.2*i;
    in[i].y        = 0.9*cos(0.3*i);
    in[i].alpha    = 0.004*sin(0.9*i);
    in[i].zoom     = 0.001*i;
    in[i].barrel   = 0.0005*i;
    in[i].rshutter = -0.0003*i;
  }
  tpRefCorrections(in, pts, n, 1, 300000, ref);

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 1, 300000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, n) == VS_OK);
  for(i=0; i<n; i++) tr.ts[i] = in[i];
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, n) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);
  for(i=0; i<n; i++){
    TASK_CHECK(fabs(tr.ts[i].x - ref[i].x) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].y - ref[i].y) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].alpha - ref[i].alpha) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].zoom - ref[i].zoom) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].barrel - ref[i].barrel) < 1e-12);
    TASK_CHECK(fabs(tr.ts[i].rshutter - ref[i].rshutter) < 1e-12);
  }
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* Render a frame through the real pipeline twice: once with the correction
   obtained from vsGetTransformAtPTS() on the preprocessed sequence, and once
   with the independently computed correction in ref.  The outputs must be
   bit-identical.  Repeat over several query PTS so the interpolation between
   nodes is also exercised. */
static int trRenderMatchesReference(void){
  const int n = 6;
  int64_t pts[6] = {0, 83333, 83334, 500000, 1000000, 2500000};
  VSTransform in[6];
  VSTransform ref[6];
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  VSFrame src, dstA, dstB;
  VSTransform q;
  int i;
  int64_t query[] = {0, 40000, 83333, 300000, 750000, 1200000, 2500000, 3000000};

  for(i=0; i<n; i++){
    in[i] = null_transform();
    in[i].x        = 3.0*sin(0.5*i) + 0.4*i;
    in[i].y        = 2.0*cos(0.35*i) - 0.7*i;
    in[i].alpha    = 0.008*sin(0.8*i);
    in[i].zoom     = 0.0;
    in[i].barrel   = 0.0;
    in[i].rshutter = 0.0;
  }
  tpRefCorrections(in, pts, n, 0, 120000, ref);

  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));

  /* The pipeline uses td's frame info and crop setting.  Use the same
     configuration in both runs, only the transform differs. */
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 120000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, n) == VS_OK);
  for(i=0; i<n; i++) tr.ts[i] = in[i];
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, n) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);

  vsFrameAllocate(&src, &fi);
  vsFrameAllocate(&dstA, &fi);
  vsFrameAllocate(&dstB, &fi);
  for(int y=0; y<fi.height; y++)
    for(int x=0; x<fi.width; x++)
      src.data[0][y*src.linesize[0] + x] = (uint8_t)((x*7 + y*11 + x*y*3) % 251);

  for(i=0; i<(int)(sizeof(query)/sizeof(query[0])); i++){
    /* A: what the query interface hands a caller */
    TASK_CHECK(vsGetTransformAtPTS(&td, &tr, query[i], &q) == VS_OK);
    TASK_CHECK(vsTransformPrepare(&td, &src, &dstA) == VS_OK);
    TASK_CHECK(vsDoTransform(&td, q) == VS_OK);
    TASK_CHECK(vsTransformFinish(&td) == VS_OK);

    /* B: an independent linearly-interpolated correction */
    {
      VSTransform refq;
      int lo = 0, hi = 0;
      double w;
      if(query[i] <= pts[0]){
        refq = ref[0];
      }else if(query[i] >= pts[n-1]){
        refq = ref[n-1];
      }else{
        lo = 0; hi = n-1;
        while(hi - lo > 1){
          int mid = lo + (hi - lo)/2;
          if(pts[mid] <= query[i]) lo = mid; else hi = mid;
        }
        w = (double)(query[i] - pts[lo]) / (double)(pts[hi] - pts[lo]);
        refq = null_transform();
        refq.x     = ref[lo].x     + (ref[hi].x     - ref[lo].x)*w;
        refq.y     = ref[lo].y     + (ref[hi].y     - ref[lo].y)*w;
        refq.alpha = ref[lo].alpha + (ref[hi].alpha - ref[lo].alpha)*w;
        refq.zoom  = ref[lo].zoom  + (ref[hi].zoom  - ref[lo].zoom)*w;
      }
      TASK_CHECK(vsTransformPrepare(&td, &src, &dstB) == VS_OK);
      TASK_CHECK(vsDoTransform(&td, refq) == VS_OK);
      TASK_CHECK(vsTransformFinish(&td) == VS_OK);
    }
    TASK_CHECK(tpFramesEqual(&dstA, &dstB, &fi));
  }

  vsFrameFree(&src); vsFrameFree(&dstA); vsFrameFree(&dstB);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* Interleaving the sequential cursor with PTS queries over the rendered
   sequence: each frame's sequential transform equals the query at its PTS,
   and the rendered pixels match. */
static int trCursorInterleaved(void){
  const int n = 4;
  int64_t pts[4] = {0, 125000, 375000, 500000};
  VSTransform in[4];
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations tr;
  VSFrame src, dstA, dstB;
  int i;

  for(i=0; i<n; i++){
    in[i] = null_transform();
    in[i].x = 2.0*i;
    in[i].y = -1.0*i;
    in[i].alpha = 0.002*i;
  }
  TASK_CHECK(vsFrameInfoInit(&fi, TP_W, TP_H, PF_GRAY8));
  TASK_CHECK(makeTimedTd(&td, &cfg, &fi, 0, 125000) == VS_OK);
  TASK_CHECK(seqAlloc(&tr, n) == VS_OK);
  for(i=0; i<n; i++) tr.ts[i] = in[i];
  TASK_CHECK(vsTransformationsSetTimestamps(&tr, pts, n) == VS_OK);
  TASK_CHECK(vsPreprocessTransforms(&td, &tr) == VS_OK);

  vsFrameAllocate(&src, &fi);
  vsFrameAllocate(&dstA, &fi);
  vsFrameAllocate(&dstB, &fi);
  for(int y=0; y<fi.height; y++)
    for(int x=0; x<fi.width; x++)
      src.data[0][y*src.linesize[0] + x] = (uint8_t)((x*13 + y*5) % 253);

  for(i=0; i<n; i++){
    VSTransform seqT, atT;
    /* query first: it must not disturb the cursor */
    TASK_CHECK(vsGetTransformAtPTS(&td, &tr, pts[i], &atT) == VS_OK);
    seqT = vsGetNextTransform(&td, &tr);
    TASK_CHECK(fabs(seqT.x - atT.x) < 1e-12);
    TASK_CHECK(fabs(seqT.y - atT.y) < 1e-12);
    TASK_CHECK(fabs(seqT.alpha - atT.alpha) < 1e-12);

    TASK_CHECK(vsTransformPrepare(&td, &src, &dstA) == VS_OK);
    TASK_CHECK(vsDoTransform(&td, seqT) == VS_OK);
    TASK_CHECK(vsTransformFinish(&td) == VS_OK);
    TASK_CHECK(vsTransformPrepare(&td, &src, &dstB) == VS_OK);
    TASK_CHECK(vsDoTransform(&td, atT) == VS_OK);
    TASK_CHECK(vsTransformFinish(&td) == VS_OK);
    TASK_CHECK(tpFramesEqual(&dstA, &dstB, &fi));
  }

  vsFrameFree(&src); vsFrameFree(&dstA); vsFrameFree(&dstB);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}

/* ---- registration ---------------------------------------------------- */

const struct task_case task_cases[] = {
  {"timed_math", "example",              tpTimeBoxExample},
  {"timed_math", "relative",             tpTimeBoxRelative},
  {"timed_math", "multi_node",           tpTimeBoxMultiNode},
  {"timed_math", "continuous_angle",     tpTimeBoxContinuousAngle},
  {"timed_math", "six_fields",           tpTimeBoxSixFields},
  {"timed_math", "rejects",              tpTimeBoxRejects},
  {"timed_math", "old_modes_ignore",     tpOldModesIgnoreTimestamps},
  {"timed_math", "query_errors",         tpQueryErrors},
  {"timed_math", "query_interpolation",  tpQueryInterpolation},
  {"timed_math", "timestamp_ownership",  tpTimestampOwnership},
  {"timed_io",   "round_trip",           tioRoundTrip},
  {"timed_io",   "file_drives_smoothing", tioFileDrivesSmoothing},
  {"timed_io",   "rejects",              tioRejects},
  {"timed_render", "query_reference",    trQueryMatchesReference},
  {"timed_render", "relative_reference", trRelativeMatchesReference},
  {"timed_render", "render_reference",   trRenderMatchesReference},
  {"timed_render", "cursor_interleaved", trCursorInterleaved},
};
const size_t task_case_count =
    sizeof(task_cases)/sizeof(task_cases[0]);
