#include "nanovg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int updates, triangles, vertices;
static int create(void*p){(void)p;return 1;}
static int tex(void*p,int t,int w,int h,int f,const unsigned char*d){(void)p;(void)t;(void)w;(void)h;(void)f;(void)d;return 1;}
static int del(void*p,int i){(void)p;(void)i;return 1;}
static int size(void*p,int i,int*w,int*h){(void)p;(void)i;*w=*h=512;return 1;}
static int update(void*p,int i,int x,int y,int w,int h,const unsigned char*d){(void)p;(void)i;(void)x;(void)y;assert(w>0&&h>0&&d);updates++;return 1;}
static void viewport(void*p,float w,float h,float r){(void)p;(void)w;(void)h;(void)r;}
static void flush(void*p){(void)p;}
static void draw(void*p,NVGpaint*a,NVGcompositeOperationState o,NVGscissor*s,const NVGvertex*v,int n,float f){
 (void)p;(void)o;(void)f;assert(a->image==1);assert(fabsf(a->innerColor.a-.2f)<1e-6f);assert(s->extent[0]==40);
 for(int i=0;i<n;i++){assert(isfinite(v[i].x)&&isfinite(v[i].y));assert(v[i].u>=0&&v[i].u<=1);}
 triangles++;vertices+=n;
}
int main(void){
 NVGparams p={0};p.renderCreate=create;p.renderCreateTexture=tex;p.renderDeleteTexture=del;
 p.renderUpdateTexture=update;p.renderGetTextureSize=size;p.renderViewport=viewport;p.renderFlush=flush;p.renderTriangles=draw;
 NVGcontext*c=nvgCreateInternal(&p);assert(c);
 int font=nvgCreateFont(c,"roboto","example/Roboto-Regular.ttf");assert(font>=0);
 nvgBeginFrame(c,160,80,1);nvgFontFaceId(c,font);nvgFontSize(c,24);
 nvgScissor(c,0,0,80,60);nvgFillColor(c,nvgRGBAf(1,1,1,.5f));nvgGlobalAlpha(c,.4f);
 float end=nvgText(c,10,40,"AV",NULL);assert(end>10);
 assert(triangles==1&&vertices==12&&updates>0);nvgEndFrame(c);nvgDeleteInternal(c);
 printf("baseline AV: vertices=%d texture_updates=%d alpha=0.2 scissor_width=80\n",vertices,updates);
}
