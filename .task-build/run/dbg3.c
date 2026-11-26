#include "renderer/core/api.h"
#include <stdio.h>
#include <math.h>
int main(void){
 mat4_t view=mat4_lookat(vec3_new(0,0,5),vec3_new(0,0,0),vec3_new(0,1,0));
 mat4_t proj=mat4_perspective(TO_RADIANS(60),1.0f,0.5f,10);
 mat4_t obj=mat4_identity();
 mat4_t vc=mat4_mul_mat4(proj,view);
 /* sample world points on the unit sphere */
 double lo=2,hi=-1;
 for(int i=0;i<2000;i++){
   double a=i*2*M_PI/100, b=(i%100)*M_PI/99;
   vec3_t p=vec3_new((float)(sin(b)*cos(a)),(float)cos(b),(float)(sin(b)*sin(a)));
   vec4_t c=mat4_mul_vec4(vc,vec4_new(p.x,p.y,p.z,1));
   double z=c.z/c.w; double dep=(z+1)*0.5;
   if(fabs(z)>1) continue;
   if(dep<lo)lo=dep; if(dep>hi)hi=dep;
 }
 printf("sphere depth range %.6f .. %.6f width %.6f\n",lo,hi,hi-lo);
 framebuffer_release: return 0;
}
