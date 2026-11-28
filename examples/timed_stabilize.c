/*
 *  timed_stabilize.c
 *
 *  Minimal end-to-end demonstration of variable-frame-rate stabilization:
 *  an absolute camera path carrying microsecond PTS is smoothed with the
 *  VSTimeBox (physical-time box average) algorithm, then eight output frames
 *  are produced at a fixed 125000 us cadence by querying the preprocessed
 *  correction at each output PTS with vsGetTransformAtPTS().
 *
 *  The output is raw 8-bit grayscale (PF_GRAY8), 32x24 per frame, written back
 *  to back with no header: 8 * 32 * 24 = 6144 bytes.  Play it back with e.g.
 *      ffplay -f rawvideo -pixel_format gray -video_size 32x24 out.gray
 *
 *  Usage: timed_stabilize <output.raw>
 *
 *  Pure C, no dependencies beyond the vid.stab library and libm.
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "libvidstab.h"
#include "transform.h"
#include "transformtype_operations.h"
#include "serialize.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define FRAME_W   32
#define FRAME_H   24
#define N_FRAMES  8
#define OUT_PERIOD_US 125000          /* output frame cadence */

/* input camera path nodes (absolute, relative=0) and their microsecond PTS */
#define N_NODES 4

int main(int argc, char** argv){
  static const int64_t nodePts[N_NODES] = {0, 250000, 600000, 1000000};
  /* a shaky absolute path: alternating translations in x and y */
  static const double nodeX[N_NODES] = { 3.0, -4.0, 5.0, -2.0};
  static const double nodeY[N_NODES] = {-2.0,  3.0,-4.0, 2.0};

  VSFrameInfo fi;
  VSTransformData td;
  VSTransformations tr;
  VSTransformConfig cfg;
  VSFrame src, dst;
  FILE* out;
  int i;

  if(argc != 2){
    fprintf(stderr, "usage: %s <output.raw>\n", argv[0]);
    return 2;
  }
  out = fopen(argv[1], "wb");
  if(!out){
    fprintf(stderr, "cannot open %s for writing\n", argv[1]);
    return 2;
  }

  if(vsFrameInfoInit(&fi, FRAME_W, FRAME_H, PF_GRAY8) != 1) return 3;

  /* Time-box smoothing over a +/- 150 ms window; plain similarity transform,
   * which is the only configuration VSTimeBox accepts. */
  cfg = vsTransformGetDefaultConfig("timed_stabilize");
  cfg.relative      = 0;
  cfg.smoothing     = 0;
  cfg.optZoom       = 0;
  cfg.zoom          = 0;
  cfg.crop          = VSCropBorder;
  cfg.interpolType  = VS_BiLinear;
  cfg.camPathAlgo   = VSTimeBox;
  cfg.lensCorrection = VSLensCorrectOff;
  cfg.lensK         = 0.0;
  cfg.fov           = 0.0;
  cfg.smoothingTime = 150000;        /* half-window h, in microseconds */
  if(vsTransformDataInit(&td, &cfg, &fi, &fi) != VS_OK) return 4;

  vsTransformationsInit(&tr);
  tr.ts = vs_malloc(sizeof(VSTransform) * N_NODES);
  tr.len = N_NODES;
  for(i = 0; i < N_NODES; i++){
    VSTransform node = null_transform();
    node.x = nodeX[i];
    node.y = nodeY[i];
    tr.ts[i] = node;
  }
  if(vsTransformationsSetTimestamps(&tr, nodePts, N_NODES) != VS_OK) return 5;

  /* one O(n) pass: relative accumulation (none here), time-box smoothing,
   * invert/maxShift/zoom stages.  After this the sequence is queryable. */
  if(vsPreprocessTransforms(&td, &tr) != VS_OK) return 6;

  vsFrameAllocate(&src, &fi);
  vsFrameAllocate(&dst, &fi);

  for(i = 0; i < N_FRAMES; i++){
    int64_t pts = (int64_t)i * OUT_PERIOD_US;
    VSTransform corr;
    int x, y;
    /* synthesize a deterministic source frame: a moving diagonal ramp so the
     * warp has real content to resample */
    for(y = 0; y < fi.height; y++)
      for(x = 0; x < fi.width; x++)
        src.data[0][y*src.linesize[0]+x] =
          (uint8_t)((x*7 + y*11 + i*13) % 251);

    if(vsGetTransformAtPTS(&td, &tr, pts, &corr) != VS_OK) return 7;
    if(vsTransformPrepare(&td, &src, &dst) != VS_OK) return 8;
    if(vsDoTransform(&td, corr) != VS_OK) return 9;
    if(vsTransformFinish(&td) != VS_OK) return 10;

    if((int)fwrite(dst.data[0], 1, (size_t)fi.width * fi.height, out)
       != fi.width * fi.height) return 11;
  }

  fclose(out);
  vsFrameFree(&src);
  vsFrameFree(&dst);
  vsTransformationsCleanup(&tr);
  vsTransformDataCleanup(&td);
  return 0;
}
