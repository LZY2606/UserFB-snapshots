#include "renderer/core/api.h"
#include <stdio.h>
#include <math.h>
static vec4_t sh(const ellipsoid_hit_t*h,void*u,int*d){(void)h;*(double*)u=*(double*)u; (void)d; return vec4_new(1,0,0,1);}
int main(void){
 for(int W=16;W<=48;W+=16){
  framebuffer_t*f=framebuffer_create(W,W);
  ellipsoid_draw_t e; e.object_to_world=mat4_identity();
  mat4_t view=mat4_lookat(vec3_new(0,0,5),vec3_new(0,0,0),vec3_new(0,1,0));
  mat4_t proj=mat4_perspective(TO_RADIANS(60),1.0f,0.5f,10);
  e.world_to_clip=mat4_mul_mat4(proj,view); e.shader=sh; e.uniforms=NULL; e.enable_blend=0;
  double lo=2,hi=-1; int n=0;
  graphics_draw_ellipsoid(f,&e);
  for(int i=0;i<W*W;i++){float d=f->depth_buffer[i]; if(d<1.0f){n++; if(d<lo)lo=d; if(d>hi)hi=d;}}
  printf("W=%d pixels=%d depth %.5f..%.5f span %.5f\n",W,n,lo,hi,hi-lo);
  framebuffer_release(f);
 }
 return 0;
}
