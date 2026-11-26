#include "renderer/core/api.h"
#include <stdio.h>
#include <math.h>
static vec4_t sh(const ellipsoid_hit_t*h,void*u,int*d){(void)h;(void)u;(void)d;return vec4_new(0,1,0,1);}
int main(void){
 framebuffer_t*f=framebuffer_create(16,16);
 ellipsoid_draw_t e; e.object_to_world=mat4_identity();
 mat4_t view=mat4_lookat(vec3_new(0,0,5),vec3_new(0,0,0),vec3_new(0,1,0));
 mat4_t proj=mat4_perspective(TO_RADIANS(60),1.0f,0.5f,10);
 e.world_to_clip=mat4_mul_mat4(proj,view); e.shader=sh; e.uniforms=NULL; e.enable_blend=0;
 double deepest=-1;
 /* first draw to learn range */
 graphics_draw_ellipsoid(f,&e);
 for(int i=0;i<256;i++){float d=f->depth_buffer[i]; if(d<1.0f && d>deepest) deepest=d;}
 printf("deepest from render = %.6f\n", deepest);
 framebuffer_clear_color(f,vec4_new(0,0,0,1));
 for(int i=0;i<256;i++) f->depth_buffer[i]=(float)(deepest+0.05);
 graphics_draw_ellipsoid(f,&e);
 int drawn=0;
 for(int i=0;i<256;i++) if(f->color_buffer[i*4+1]!=0) drawn++;
 printf("drawn=%d\n",drawn);
 /* print any hit depth */
 for(int i=0;i<256;i++){
   if(f->depth_buffer[i]<(float)(deepest+0.05)-1e-6) printf("wrote at %d depth %.6f\n",i,f->depth_buffer[i]);
 }
 framebuffer_release(f);
 return 0;
}
