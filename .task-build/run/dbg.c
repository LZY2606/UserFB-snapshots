#include "renderer/core/api.h"
#include <stdio.h>
#include <math.h>
static vec4_t sh(const ellipsoid_hit_t*h,void*u,int*d){(void)h;(void)u;(void)d;return vec4_new(1,0,0,1);}
int main(void){
 framebuffer_t*f=framebuffer_create(32,32);
 ellipsoid_draw_t e; e.object_to_world=mat4_identity();
 mat4_t view=mat4_lookat(vec3_new(0,0,5),vec3_new(0,0,0),vec3_new(0,1,0));
 mat4_t proj=mat4_perspective(TO_RADIANS(60),1.0,0.5,10);
 e.world_to_clip=mat4_mul_mat4(proj,view); e.shader=sh; e.uniforms=NULL; e.enable_blend=0;
 graphics_draw_ellipsoid(f,&e);
 int blocked=0,open=0,hit=0;
 for(int y=0;y<32;y++)for(int x=0;x<32;x++){
   float d2=f->depth_buffer[y*32+x];
   if(d2<1.0f){hit++; if(d2<=0.5)blocked++; else open++;}
 }
 printf("hit=%d blocked=%d open=%d\n",hit,blocked,open);
 framebuffer_release(f);return 0;
}
