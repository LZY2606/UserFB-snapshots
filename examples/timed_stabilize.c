/*
 * timed_stabilize.c -- end-to-end example for time-based smoothing.
 *
 * Generates eight 32x24 grayscale frames, derives the camera corrections with
 * VSTimeBox at a 125 ms frame interval, queries the smoothed correction for
 * each frame by PTS, and writes the stabilised frames as raw gray8 to the
 * file named on the command line.  Eight 32x24 frames are 6144 bytes.
 *
 * Build (from the source tree, after building the library):
 *   cc -std=c99 -DVIDSTAB_STATIC_DEFINE -I. -Isrc -I<build> \
 *      examples/timed_stabilize.c <build>/libvidstab.a -lm -o timed_stabilize
 *
 * Run:
 *   ./timed_stabilize out.gray
 */

#include "libvidstab.h"
#include "transform.h"
#include "transformtype_operations.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIDTH     32
#define HEIGHT    24
#define NFRAMES   8
#define FRAME_US  125000   /* 8 fps */

/* A synthetic shaky camera path: a deliberate jolt in the middle that the
   time window should smooth away. */
static double shakeX(int i){
  static const double dx[NFRAMES] = {0, 3, -4, 9, -2, 1, -1, 0};
  return dx[i];
}
static double shakeY(int i){
  static const double dy[NFRAMES] = {0, -1, 2, -5, 1, 0, 2, 0};
  return dy[i];
}

static void fillFrame(VSFrame* f, const VSFrameInfo* fi, int i){
  for(int y=0; y<fi->height; y++)
    for(int x=0; x<fi->width; x++){
      /* a moving pattern so a shifted frame visibly differs from the source */
      int v = (x*7 + y*13 + i*23) % 251;
      if(((x/4) + (y/4) + i) % 2) v = 255 - v;
      f->data[0][y*f->linesize[0] + x] = (uint8_t)v;
    }
}

int main(int argc, char** argv){
  VSFrameInfo fi;
  VSTransformData td;
  VSTransformConfig cfg;
  VSTransformations trans;
  VSFrame src, dst;
  int64_t pts[NFRAMES];
  FILE* out;
  int i;

  if(argc != 2){
    fprintf(stderr, "usage: %s <output.rawvideo>\n", argv[0]);
    return 2;
  }
  if(!vsFrameInfoInit(&fi, WIDTH, HEIGHT, PF_GRAY8)){
    fprintf(stderr, "cannot initialise frame info\n");
    return 3;
  }

  /* Time-box mode: absolute transforms (the "camera path" is the input),
     a 250 ms half window on either side of each frame. */
  cfg = vsTransformGetDefaultConfig("timed_stabilize");
  cfg.relative        = 0;
  cfg.smoothing       = 0;
  cfg.smoothingTime   = 250000;   /* microseconds */
  cfg.camPathAlgo     = VSTimeBox;
  cfg.optZoom         = 0;
  cfg.zoom            = 0.0;
  cfg.lensCorrection  = VSLensCorrectOff;
  cfg.estimateLensDistortion = 0;
  cfg.fov             = 0.0;
  cfg.invert          = 0;
  cfg.crop            = VSCropBorder;
  cfg.maxShift        = -1;
  cfg.maxAngle        = -1.0;

  if(vsTransformDataInit(&td, &cfg, &fi, &fi) != VS_OK){
    fprintf(stderr, "cannot initialise the transform pass\n");
    return 4;
  }
  vsTransformationsInit(&trans);
  trans.ts = (VSTransform*)vs_malloc(sizeof(VSTransform) * NFRAMES);
  if(!trans.ts){
    fprintf(stderr, "out of memory\n");
    vsTransformDataCleanup(&td);
    return 5;
  }
  trans.len = NFRAMES;
  for(i=0; i<NFRAMES; i++){
    trans.ts[i] = null_transform();
    trans.ts[i].x = shakeX(i);
    trans.ts[i].y = shakeY(i);
    pts[i] = (int64_t)i * FRAME_US;
  }
  if(vsTransformationsSetTimestamps(&trans, pts, NFRAMES) != VS_OK){
    fprintf(stderr, "timestamp array rejected\n");
    vsTransformationsCleanup(&trans);
    vsTransformDataCleanup(&td);
    return 6;
  }
  if(vsPreprocessTransforms(&td, &trans) != VS_OK){
    fprintf(stderr, "preprocessing failed\n");
    vsTransformationsCleanup(&trans);
    vsTransformDataCleanup(&td);
    return 7;
  }

  out = fopen(argv[1], "wb");
  if(!out){
    fprintf(stderr, "cannot open %s\n", argv[1]);
    vsTransformationsCleanup(&trans);
    vsTransformDataCleanup(&td);
    return 8;
  }

  vsFrameAllocate(&src, &fi);
  vsFrameAllocate(&dst, &fi);
  for(i=0; i<NFRAMES; i++){
    VSTransform correction;
    /* Take the smoothed correction for this frame's PTS.  This is the
       variable-rate path: the output PTS is what selects the correction. */
    if(vsGetTransformAtPTS(&td, &trans, pts[i], &correction) != VS_OK){
      fprintf(stderr, "query at %lld failed\n", (long long)pts[i]);
      break;
    }
    fillFrame(&src, &fi, i);
    if(vsTransformPrepare(&td, &src, &dst) != VS_OK ||
       vsDoTransform(&td, correction) != VS_OK ||
       vsTransformFinish(&td) != VS_OK){
      fprintf(stderr, "rendering frame %d failed\n", i);
      break;
    }
    if(fwrite(dst.data[0], 1, (size_t)WIDTH*HEIGHT, out) !=
       (size_t)WIDTH*HEIGHT){
      fprintf(stderr, "write failed\n");
      break;
    }
    printf("frame %d pts=%lld correction=(%.3f, %.3f, %.4f)\n",
           i, (long long)pts[i], correction.x, correction.y, correction.alpha);
  }

  fclose(out);
  vsFrameFree(&src);
  vsFrameFree(&dst);
  vsTransformationsCleanup(&trans);
  vsTransformDataCleanup(&td);
  return i == NFRAMES ? 0 : 9;
}
