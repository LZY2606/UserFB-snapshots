#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include <cstdio>
#include <cstring>
struct Material { float z; vec4 color; };
static int calls;
static void vs(float*, vec4* a, Shader_Builtins* b, void* u) {
 b->gl_Position = a[0]; b->gl_Position.z = ((Material*)u)->z;
}
static void fs(float*, Shader_Builtins* b, void* u) {
 ++calls; b->gl_FragColor = ((Material*)u)->color;
}
int main() {
 glContext ctx; pix_t* back = nullptr;
 if (!init_glContext(&ctx, &back, 32, 24)) return 2;
 float points[] = {-.8f,-.7f,0, -.7f,.7f,0, 0,-.6f,0, .6f,.7f,0, .8f,-.7f,0};
 GLuint vbo; glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
 glBufferData(GL_ARRAY_BUFFER,sizeof(points),points,GL_STATIC_DRAW);
 glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,0); glEnableVertexAttribArray(0);
 GLuint p=pglCreateProgram(vs,fs,0,nullptr,GL_FALSE); glUseProgram(p);
 Material m={-.5f,make_v4(1,0,0,1)}; pglSetUniform(&m);
 GLubyte indices[]={0,1,2,3,4};
 glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
 glClearColor(0,0,0,1); glClearDepth(1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
 glDrawElements(GL_TRIANGLE_STRIP,5,GL_UNSIGNED_BYTE,indices);
 int near_calls=calls;
 pix_t color[32*24]; u32 depth[32*24];
 memcpy(color,back,sizeof(color)); memcpy(depth,ctx.zbuf.buf,sizeof(depth));
 m.z=.5f; m.color=make_v4(0,1,0,1); calls=0;
 glDrawElements(GL_TRIANGLE_STRIP,5,GL_UNSIGNED_BYTE,indices);
 int far_calls=calls;
 if (memcmp(color,back,sizeof(color)) || memcmp(depth,ctx.zbuf.buf,sizeof(depth))) return 3;
 glEnable(GL_SCISSOR_TEST); glScissor(9,6,7,5); glClear(GL_DEPTH_BUFFER_BIT);
 calls=0; glDrawElements(GL_TRIANGLE_STRIP,5,GL_UNSIGNED_BYTE,indices);
 int reopened=calls;
 GLenum err=glGetError();
 printf("SEED_DEPTH near=%d hidden=%d reopened=%d error=%u\n",near_calls,far_calls,reopened,(unsigned)err);
 free_glContext(&ctx);
 return near_calls==365 && far_calls==0 && reopened>0 && reopened<=35 && err==GL_NO_ERROR ? 0 : 4;
}
