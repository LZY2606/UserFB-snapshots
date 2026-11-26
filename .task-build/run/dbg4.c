#include "renderer/core/api.h"
#include <stdio.h>
#include <math.h>
int main(void){
 mat4_t view=mat4_lookat(vec3_new(0,0,5),vec3_new(0,0,0),vec3_new(0,1,0));
 mat4_t proj=mat4_perspective(TO_RADIANS(60),1.0f,0.5f,10);
 mat4_t vc=mat4_mul_mat4(proj,view);
 /* center of the 32x32 frame is pixel 16 */
 double xf=2*((double)16+0.5)/32-1, yf=2*((double)16+0.5)/32-1;
 printf("center pixel ndc %f %f\n",xf,yf);
 /* near/far unproject for this screen point - use mat4 inverse */
 mat4_t inv=mat4_inverse(vc);
 vec4_t hn=mat4_mul_vec4(inv,vec4_new((float)xf,(float)yf,-1,1));
 vec4_t hf=mat4_mul_vec4(inv,vec4_new((float)xf,(float)yf,1,1));
 printf("near w %f far w %f\n",hn.w,hf.w);
 printf("near %f %f %f\n",hn.x/hn.w,hn.y/hn.w,hn.z/hn.w);
 printf("far  %f %f %f\n",hf.x/hf.w,hf.y/hf.w,hf.z/hf.w);
 return 0;
}
