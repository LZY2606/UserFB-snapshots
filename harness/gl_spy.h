/* Minimal synchronous GL2 recorder, no graphics driver. Added here, zlib license. */
#ifndef CHECK_GL_SPY_H
#define CHECK_GL_SPY_H
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
enum {GL_TEXTURE_2D=1,GL_ALPHA,GL_UNSIGNED_BYTE,GL_TEXTURE_MIN_FILTER,GL_LINEAR,
      GL_CLIENT_PIXEL_STORE_BIT,GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,
      GL_UNPACK_SKIP_PIXELS,GL_UNPACK_SKIP_ROWS,GL_VERTEX_ARRAY,GL_TEXTURE_COORD_ARRAY,
      GL_COLOR_ARRAY,GL_FLOAT,GL_TRIANGLES};
typedef struct SpyTexture {int alive,w,h,updates;unsigned char *pixels;} SpyTexture;
typedef struct SpyDraw {GLuint texture;int nverts;float verts[4096],uv[4096];} SpyDraw;
static struct {
    SpyTexture textures[128];GLuint next,bound;
    int creates,deletes,draw_count;
    int rowlen,skipx,skipy,alignment,saved[4];
    const float *verts,*uv;
    SpyDraw draws[128];
    /* Failure injection: when >0 the next glGenTextures call simulates a
       driver failure (id 0) and the counter is decremented. */
    int fail_create_remaining;
} spy;
static void glGenTextures(GLsizei n,GLuint *out) {
    int i;for(i=0;i<n;i++){
        if(spy.fail_create_remaining>0){spy.fail_create_remaining--;out[i]=0;spy.creates++;continue;}
        GLuint id=++spy.next;assert(id<128);out[i]=id;spy.textures[id].alive=1;spy.creates++;}
}
static void glDeleteTextures(GLsizei n,const GLuint *ids) {
    int i;for(i=0;i<n;i++){SpyTexture *t=&spy.textures[ids[i]];assert(t->alive);free(t->pixels);memset(t,0,sizeof *t);spy.deletes++;}
}
static void glBindTexture(GLenum target,GLuint id) {(void)target;assert(id<128 && spy.textures[id].alive);spy.bound=id;}
static void glTexImage2D(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const void *data) {
    SpyTexture *t=&spy.textures[spy.bound];(void)target;(void)level;(void)internal;(void)border;(void)format;(void)type;
    assert(t->alive && w>0 && h>0);free(t->pixels);t->pixels=calloc((size_t)w*h,1);assert(t->pixels);t->w=w;t->h=h;
    if(data)memcpy(t->pixels,data,(size_t)w*h);
}
static void glTexParameteri(GLenum a,GLenum b,GLint c) {(void)a;(void)b;(void)c;}
static void glPushClientAttrib(GLenum a) {(void)a;spy.saved[0]=spy.rowlen;spy.saved[1]=spy.skipx;spy.saved[2]=spy.skipy;spy.saved[3]=spy.alignment;}
static void glPopClientAttrib(void) {spy.rowlen=spy.saved[0];spy.skipx=spy.saved[1];spy.skipy=spy.saved[2];spy.alignment=spy.saved[3];}
static void glPixelStorei(GLenum key,GLint value) {
    if(key==GL_UNPACK_ALIGNMENT)spy.alignment=value;
    else if(key==GL_UNPACK_ROW_LENGTH)spy.rowlen=value;
    else if(key==GL_UNPACK_SKIP_PIXELS)spy.skipx=value;
    else if(key==GL_UNPACK_SKIP_ROWS)spy.skipy=value;
    else assert(0);
}
static void glTexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,const void *data) {
    int row,stride=spy.rowlen?spy.rowlen:w;SpyTexture *t=&spy.textures[spy.bound];const unsigned char *src=data;
    (void)target;(void)level;(void)format;(void)type;
    assert(t->alive && x>=0 && y>=0 && x+w<=t->w && y+h<=t->h);
    for(row=0;row<h;row++)memcpy(t->pixels+(y+row)*t->w+x,src+(spy.skipy+row)*stride+spy.skipx,(size_t)w);
    t->updates++;
}
static void glEnable(GLenum a) {(void)a;}
static void glDisable(GLenum a) {(void)a;}
static void glEnableClientState(GLenum a) {(void)a;}
static void glDisableClientState(GLenum a) {(void)a;}
static void glVertexPointer(GLint n,GLenum t,GLsizei stride,const void *p) {(void)t;(void)stride;assert(n==2);spy.verts=p;}
static void glTexCoordPointer(GLint n,GLenum t,GLsizei stride,const void *p) {(void)t;(void)stride;assert(n==2);spy.uv=p;}
static void glColorPointer(GLint n,GLenum t,GLsizei stride,const void *p) {(void)n;(void)t;(void)stride;(void)p;}
static void glDrawArrays(GLenum mode,GLint start,GLsizei n) {
    SpyTexture *t=&spy.textures[spy.bound];SpyDraw *d;
    assert(mode==GL_TRIANGLES && start==0 && n>0 && n*2<=4096 && spy.draw_count<128);
    assert(t->alive && t->updates>0);d=&spy.draws[spy.draw_count++];d->texture=spy.bound;d->nverts=n;
    memcpy(d->verts,spy.verts,(size_t)n*2*sizeof(float));memcpy(d->uv,spy.uv,(size_t)n*2*sizeof(float));
}
#endif
