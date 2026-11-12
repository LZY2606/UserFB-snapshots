#include "portablegl.h"
#include <cassert>
#include <cstdio>
static int calls;
static void vs(float*,vec4* a,Shader_Builtins*b,void*){b->gl_Position=a[0];}
static void fs(float*,Shader_Builtins*b,void*){++calls;b->gl_FragColor=make_v4(1,0,0,1);}
int main(){glContext ctx;pix_t* pixels=nullptr;assert(init_glContext(&ctx,&pixels,8,8));set_glContext(&ctx);
 GLuint prog=pglCreateProgram(vs,fs,0,nullptr,GL_FALSE);glUseProgram(prog);
 float p[]={-.75f,-.75f,0,1,.75f,-.75f,0,1,-.75f,.75f,0,1};
 GLuint vbo;glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(p),p,GL_STATIC_DRAW);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,nullptr);glEnableVertexAttribArray(0);
 glEnable(GL_DEPTH_TEST);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);assert(calls==15);assert(glGetError()==GL_NO_ERROR);
 const int before=calls;for(int i=2;i<12;i+=4)p[i]=.8f;glBufferData(GL_ARRAY_BUFFER,sizeof(p),p,GL_STATIC_DRAW);glDrawArrays(GL_TRIANGLES,0,3);assert(calls==before);
 std::printf("PORTABLEGL_BASELINE_OK fragments=%d occluded=%d\n",before,calls-before);free_glContext(&ctx);}
