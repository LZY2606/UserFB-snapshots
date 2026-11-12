#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
/* A normal serial rendering workload, not a tile implementation. */
#define W 64
#define H 48
#define TRIANGLES 132
static int vertices, fragments, foreign_threads;
static pthread_t caller;
static GLuint tex;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BASELINE_FAIL %s:%d %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static void vs(float* out, vec4* in, Shader_Builtins* b, void* u) {
 (void)u; ++vertices; out[0]=in[0].x*.25f+.5f; out[1]=in[0].y*.25f+.5f;
 b->gl_Position=in[0];
}
static void fs(float* in, Shader_Builtins* b, void* u) {
 (void)u; ++fragments;
 if (!pthread_equal(caller,pthread_self())) ++foreign_threads;
 b->gl_FragColor=texture2D(tex,in[0],in[1]);
}
int main(void) {
 glContext ctx; pix_t* pixels=NULL; caller=pthread_self();
 CHECK(init_glContext(&ctx,&pixels,W,H));
 float* p=(float*)malloc(TRIANGLES*3*4*sizeof(float)); CHECK(p);
 for (int t=0;t<TRIANGLES;++t) {
  const float v[12]={-.9f,-.9f,(t&1)?-2.f:0.f,1.f, .9f,-.9f,.25f,1.f, 0,.9f,.1f,1.f};
  memcpy(p+t*12,v,sizeof(v));
 }
 GLuint buf; glGenBuffers(1,&buf); glBindBuffer(GL_ARRAY_BUFFER,buf);
 glBufferData(GL_ARRAY_BUFFER,TRIANGLES*12*sizeof(float),p,GL_STATIC_DRAW);
 glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,0); glEnableVertexAttribArray(0);
 const Color image[4]={{255,32,0,128},{0,192,16,128},{64,0,255,128},{192,64,32,128}};
 glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,image);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 GLenum interp[]={PGL_SMOOTH,PGL_NOPERSPECTIVE};
 GLuint program=pglCreateProgram(vs,fs,2,interp,GL_FALSE); glUseProgram(program);
 glClearColor(0,0,0,1); glClearDepth(1); glClearStencil(0);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
 glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
 glDrawArrays(GL_TRIANGLES,0,TRIANGLES*3);
 CHECK(vertices==TRIANGLES*3 && fragments>0 && foreign_threads==0);
 CHECK(glGetError()==GL_NO_ERROR);
 unsigned long long digest=1469598103934665603ULL; int nonblack=0;
 for (int i=0;i<W*H;++i) {
  const unsigned char* bytes=(const unsigned char*)&pixels[i];
  for (size_t j=0;j<sizeof(pix_t);++j) digest=(digest^bytes[j])*1099511628211ULL;
  Color col=PIXEL_TO_COLOR(pixels[i]); nonblack+=(col.r||col.g||col.b);
 }
 CHECK(nonblack>0 && nonblack<W*H);
 printf("SERIAL_WITNESS vertices=%d fragments=%d foreign_threads=%d pixels=%d checksum=%llu\n",vertices,fragments,foreign_threads,nonblack,digest);
 free(p); free_glContext(&ctx); puts("BASELINE_OK"); return 0;
}
