#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include <stdio.h>
#include <assert.h>
static int verts,frags;
static void vs(float *out,vec4 *in,Shader_Builtins *b,void *u){
 (void)u;verts++;out[0]=in[0].x;b->gl_Position=in[0];
}
static void fs(float *in,Shader_Builtins *b,void *u){
 (void)in;(void)u;frags++;b->gl_FragColor=make_v4(1,0,0,1);
}
int main(void){
 glContext c;pix_t *pixels=NULL;assert(init_glContext(&c,&pixels,16,16));
 float p[]={-.8f,-.8f,0,1,.8f,-.8f,0,1,0,.8f,0,1};
 GLuint v;glGenBuffers(1,&v);glBindBuffer(GL_ARRAY_BUFFER,v);
 glBufferData(GL_ARRAY_BUFFER,sizeof p,p,GL_STATIC_DRAW);
 glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,0);glEnableVertexAttribArray(0);
 GLenum interp[]={PGL_SMOOTH};GLuint prog=pglCreateProgram(vs,fs,1,interp,GL_FALSE);
 glUseProgram(prog);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
 unsigned ix[]={0,1,2};glDrawElementsInstanced(GL_TRIANGLES,3,GL_UNSIGNED_INT,ix,2);
 assert(verts==6 && frags>0 && glGetError()==GL_NO_ERROR);
 printf("DRAW_COUNTS vertices=%d fragments=%d\n",verts,frags);
 free_glContext(&c);puts("BASELINE_OK");return 0;
}
