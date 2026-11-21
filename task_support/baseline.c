#include "gl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static int fragments;
static void vs(float*out,float*in,void*u){(void)u;memcpy(out,in,7*sizeof(float));}
static void fs(uint32_t*out,float*in,void*u){(void)u;fragments++;*out=0xff000000u|((unsigned)(in[4]*255)<<16)|((unsigned)(in[5]*255)<<8)|(unsigned)(in[6]*255);}
int main(void){uint32_t colors[256]={0};float depths[256];for(int i=0;i<256;i++)depths[i]=2;
float pts[]={.5,-.5,0,1,0,1,0, 0,.5,0,1,1,0,0, -.5,-.5,0,1,0,0,1};
int indices[]={0,1,2};struct gl_framebuffer f={colors,depths,16,16};struct gl_pipeline p={0};p.fbuf=&f;p.vs=vs;p.fs=fs;p.pts_in=pts;p.n_pts=3;p.n_attr_in=7;p.n_attr_out=7;p.winding=GL_WINDING_ORDER_CCW;gl_render(&p,indices,3,GL_TRIANGLE_LIST);
int count=0;for(int i=0;i<256;i++)if(colors[i]){count++;if(fabsf(depths[i]-1)>1e-6)return 2;}
if(count<10||fragments!=count)return 3;uint32_t saved=colors[8*16+8];
gl_bind_framebuffer(16,16,colors,depths);gl_bind_vertices(pts,3,7);gl_bind_custom_vs(vs,7);gl_bind_custom_fs(fs);gl_renderl(indices,3,GL_TRIANGLE_LIST);if(colors[8*16+8]!=saved)return 4;
printf("STATIC pixels=%d fragments=%d center=%08x depth=1\nBASELINE_COMPLETE\n",count,fragments,saved);return 0;}
