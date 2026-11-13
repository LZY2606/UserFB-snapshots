#include <portablegl.h>
#include <iostream>
#include <stdexcept>
static int count=0;
static void vs(float* out,vec4* a,Shader_Builtins* b,void*){b->gl_Position=a[0];out[0]=a[0].x;out[1]=a[0].y;}
static void fs(float* in,Shader_Builtins* b,void*){++count;b->gl_FragColor=make_v4((in[0]+1)/2,(in[1]+1)/2,0,1);}
int main(){glContext c;pix_t* pixels=nullptr;if(!init_glContext(&c,&pixels,16,16))return 1;GLenum interp[]={PGL_SMOOTH,PGL_SMOOTH};auto program=pglCreateProgram(vs,fs,2,interp,GL_FALSE);glUseProgram(program);float a[]={-.75,-.75,0,1,.75,-.75,0,1,0,.75,0,1};GLuint vbo;glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(a),a,GL_STATIC_DRAW);glEnableVertexAttribArray(0);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,0);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLES,0,3);if(count<=0||glGetError()!=GL_NO_ERROR)throw std::runtime_error("draw failed");std::cout<<"real triangle fragments="<<count<<"; smooth UV callback executed\n";free_glContext(&c);}
