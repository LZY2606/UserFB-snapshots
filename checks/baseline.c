#include "nanovg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int calls, verts;
static int create(void *p){(void)p;return 1;}
static int tex(void*p,int t,int w,int h,int f,const unsigned char*d){(void)p;(void)t;(void)w;(void)h;(void)f;(void)d;return 1;}
static int del(void*p,int i){(void)p;(void)i;return 1;}
static void viewport(void*p,float w,float h,float d){(void)p;(void)w;(void)h;(void)d;}
static void noop(void*p){(void)p;}
static void stroke(void*p,NVGpaint*a,NVGcompositeOperationState c,NVGscissor*s,float f,float w,const NVGpath*ps,int n){
 (void)p;(void)a;(void)c;(void)s;(void)f; assert(fabsf(w-6)<1e-5);assert(n==1);++calls;
 for(int j=0;j<n;j++)for(int k=0;k<ps[j].nstroke;k++){assert(isfinite(ps[j].stroke[k].x));assert(isfinite(ps[j].stroke[k].y));++verts;}
}
int main(void){
 NVGparams p={0};p.renderCreate=create;p.renderCreateTexture=tex;p.renderDeleteTexture=del;
 p.renderViewport=viewport;p.renderDelete=noop;p.renderFlush=noop;p.renderStroke=stroke;
 NVGcontext*c=nvgCreateInternal(&p);assert(c);nvgBeginFrame(c,128,128,1);
 nvgStrokeWidth(c,6);nvgLineJoin(c,NVG_ROUND);nvgLineCap(c,NVG_ROUND);
 nvgBeginPath(c);nvgMoveTo(c,10,10);nvgBezierTo(c,15,40,35,0,50,30);nvgLineTo(c,70,10);nvgStroke(c);
 assert(calls==1&&verts>20);nvgEndFrame(c);nvgDeleteInternal(c);puts("NANOVG_CURVE_STROKE_OK");return 0;
}
