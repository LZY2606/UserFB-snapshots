#include "libvidstab.h"
#include "transform.h"
#include "serialize.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint32_t checksum(const VSFrame* f, int w, int h) {
 uint32_t sum=2166136261u;
 for(int y=0;y<h;y++) for(int x=0;x<w;x++) sum=(sum^f->data[0][y*f->linesize[0]+x])*16777619u;
 return sum;
}
int main(void) {
 VSFrameInfo fi; VSTransformData td; VSTransformations tr;
 VSFrame src,dst; FILE *f=tmpfile(); if(!f)return 2;
 if(!vsFrameInfoInit(&fi,32,24,PF_GRAY8))return 3;
 VSTransformConfig cfg=vsTransformGetDefaultConfig("seed-timed-baseline");
 cfg.relative=0;cfg.smoothing=0;cfg.optZoom=0;cfg.zoom=0;cfg.crop=VSCropBorder;cfg.camPathAlgo=VSGaussian;
 if(vsTransformDataInit(&td,&cfg,&fi,&fi)!=VS_OK)return 4;
 vsTransformationsInit(&tr);
 fputs("0 0 0 0 0 0\n100000 2 0 0 0 0\n900000 4 0 0 0 0\n",f);rewind(f);
 int count=vsReadOldTransforms(&td,f,&tr);fclose(f);
 if(count!=3 || tr.len!=3 || tr.ts[1].x!=2 || vsPreprocessTransforms(&td,&tr)!=VS_OK)return 5;
 vsFrameAllocate(&src,&fi);vsFrameAllocate(&dst,&fi);
 for(int y=0;y<fi.height;y++)for(int x=0;x<fi.width;x++)src.data[0][y*src.linesize[0]+x]=(uint8_t)((x*7+y*11)%251);
 uint32_t sums[2];
 for(int i=0;i<2;i++){
  VSTransform t=vsGetNextTransform(&td,&tr);if(t.x!=i*2)return 6;
  if(vsTransformPrepare(&td,&src,&dst)!=VS_OK || vsDoTransform(&td,t)!=VS_OK || vsTransformFinish(&td)!=VS_OK)return 7;
  sums[i]=checksum(&dst,fi.width,fi.height);
 }
 if(sums[0]!=checksum(&src,fi.width,fi.height) || sums[1]==sums[0])return 8;
 printf("BASELINE_OK parsed=%d cursor=%d first_x=0 second_x=2\n",count,tr.current);
 printf("RENDER_OK identity=%u shifted=%u\n",sums[0],sums[1]);
 puts("TIMING_CAPABILITY=frame-order-only; legacy-time-column-not-retained");
 vsFrameFree(&src);vsFrameFree(&dst);vsTransformationsCleanup(&tr);vsTransformDataCleanup(&td);
 return 0;
}
