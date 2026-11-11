/* Local baseline witness; added locally, not an upstream test. zlib license. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FONTSTASH_IMPLEMENTATION
#include "fontstash.h"

typedef struct Recorder {
    int w,h,creates,resizes,updates,draws,vertices,deletes,full;
    unsigned char *uploaded;
    float verts[4096],uv[4096];
} Recorder;
static int create(void *p,int w,int h) {
    Recorder *r=p; r->w=w; r->h=h; r->creates++;
    r->uploaded=calloc((size_t)w*h,1); return r->uploaded!=NULL;
}
static int resize(void *p,int w,int h) {
    Recorder *r=p; free(r->uploaded); r->uploaded=NULL;
    r->w=w;r->h=h;r->resizes++;r->uploaded=calloc((size_t)w*h,1);
    return r->uploaded!=NULL;
}
static void update(void *p,int *rect,const unsigned char *data) {
    Recorder *r=p; int y;
    assert(rect[0]>=0 && rect[1]>=0 && rect[2]<=r->w && rect[3]<=r->h);
    for(y=rect[1];y<rect[3];y++)
        memcpy(r->uploaded+y*r->w+rect[0],data+y*r->w+rect[0],(size_t)(rect[2]-rect[0]));
    r->updates++;
}
static void draw(void *p,const float *v,const float *uv,const unsigned int *colors,int n) {
    Recorder *r=p; int i,ink=0; (void)colors;
    assert(n>0 && n%6==0 && (r->vertices+n)*2<=4096);
    assert(r->updates>0);
    for(i=0;i<r->w*r->h;i++)ink+=r->uploaded[i]!=0;
    if(ink<=4) fprintf(stderr,"empty-texture w=%d h=%d creates=%d resizes=%d updates=%d n=%d\n",r->w,r->h,r->creates,r->resizes,r->updates,n); assert(ink>4);
    for(i=0;i<n*2;i++){assert(isfinite(v[i]) && uv[i]>=0 && uv[i]<=1);}
    memcpy(r->verts+r->vertices*2,v,(size_t)n*2*sizeof(float));
    memcpy(r->uv+r->vertices*2,uv,(size_t)n*2*sizeof(float));
    r->vertices+=n;r->draws++;
}
static void destroy(void *p) {Recorder *r=p;r->deletes++;free(r->uploaded);r->uploaded=NULL;}
static void error(void *p,int e,int val) {Recorder *r=p;(void)val;assert(e==FONS_ATLAS_FULL);r->full++;}
static FONScontext *setup(Recorder *r,int size) {
    FONSparams p;FONScontext *s;int f;memset(&p,0,sizeof p);
    p.width=p.height=size;p.flags=FONS_ZERO_TOPLEFT;p.userPtr=r;
    p.renderCreate=create;p.renderResize=resize;p.renderUpdate=update;p.renderDraw=draw;p.renderDelete=destroy;
    s=fonsCreateInternal(&p);assert(s);f=fonsAddFont(s,"fixture","example/DroidSansJapanese.ttf");assert(f>=0);
    fonsSetFont(s,f);fonsSetSize(s,24);fonsSetErrorCallback(s,error,r);return s;
}
static void clear_draw(Recorder *r) {r->vertices=0;r->draws=0;}
static void legacy(void) {
    Recorder r={0};FONScontext *s=setup(&r,512);float end,bounds[4],old_uv[12];
    FONStextIter it;FONSquad q;int n=0,dirty[4],before,x,y,w,h;
    unsigned char *old;const unsigned char *data;
    end=fonsDrawText(s,10,40,"日本語文字漢",NULL);assert(r.vertices==36 && end>10);
    assert(fabsf(fonsTextBounds(s,10,40,"日本語文字漢",NULL,bounds)-(end-10))<1e-4f);
    assert(fonsTextIterInit(s,&it,10,40,"日本語文字漢",NULL));
    while(fonsTextIterNext(s,&it,&q)) {assert(n<6);assert(q.x0==r.verts[n*12]);assert(q.y0==r.verts[n*12+1]);n++;}
    assert(n==6 && it.nextx==end);
    before=r.updates;clear_draw(&r);fonsDrawText(s,0,32,"日",NULL);
    memcpy(old_uv,r.uv,sizeof old_uv);assert(r.updates==before);
    assert(!fonsValidateTexture(s,dirty));
    data=fonsGetTextureData(s,&w,&h);old=malloc((size_t)w*h);assert(old);memcpy(old,data,(size_t)w*h);
    assert(fonsExpandAtlas(s,1024,512));assert(r.resizes==1);
    data=fonsGetTextureData(s,&x,&y);assert(x==1024 && y==512);
    for(y=0;y<h;y++)assert(!memcmp(old+y*w,data+y*x,(size_t)w));free(old);
    clear_draw(&r);fonsDrawText(s,0,32,"日",NULL);
    assert(r.vertices==6 && fabsf(r.uv[0]*2-old_uv[0])<1e-6f && r.uv[1]==old_uv[1]);
    assert(fonsResetAtlas(s,512,512));clear_draw(&r);fonsDrawText(s,0,32,"日",NULL);
    assert(r.vertices==6 && r.resizes==2);
    fonsDeleteInternal(s);assert(r.creates==1 && r.deletes==1);
    puts("LEGACY_OK draw bounds iterator cache expand reset");
}
static void pressure(void) {
    const char *text="日本語文字漢東京大阪京都北海道青空山川海花月星金木水火土石";Recorder large={0},small={0};
    FONScontext *a=setup(&large,512),*b=setup(&small,64);
    float xa=fonsDrawText(a,0,40,text,NULL),xb=fonsDrawText(b,0,40,text,NULL);
    int va=large.vertices,vb=small.vertices;
    assert(va==29*6 && vb>0 && vb<va && small.full>0 && xb<xa);
    printf("PRESSURE largeGlyphs=%d smallGlyphs=%d atlasFull=%d advanceLarge=%.0f advanceSmall=%.0f\n",va/6,vb/6,small.full,xa,xb);
    clear_draw(&small);fonsDrawText(b,0,40,"日",NULL);assert(small.vertices==6);
    assert(small.creates==1 && small.resizes==0);
    fonsDeleteInternal(a);fonsDeleteInternal(b);
    puts("SINGLE_ATLAS_LIMIT_WITNESS_OK cached-glyph-survives=true no-resize=true");
}
int main(void) {legacy();pressure();puts("FONTSTASH_BASELINE_OK");return 0;}
