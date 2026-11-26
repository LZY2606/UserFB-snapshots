#include "renderer/core/api.h"
#include <stdio.h>
#include <math.h>
int main(void){
 mat4_t view=mat4_lookat(vec3_new(0,0,5),vec3_new(0,0,0),vec3_new(0,1,0));
 mat4_t proj=mat4_perspective(TO_RADIANS(60),1.0f,0.5f,10);
 mat4_t vc=mat4_mul_mat4(proj,view);
 for(int i=0;i<4;i++)for(int j=0;j<4;j++)printf("%f ",vc.m[i][j]);
 printf("\n");
 vec4_t c=mat4_mul_vec4(vc,vec4_new(0,0,0,1));
 printf("center clip %f %f %f %f ndc %f\n",c.x,c.y,c.z,c.w,c.z/c.w);
 vec4_t c2=mat4_mul_vec4(vc,vec4_new(1,0,0,1));
 printf("edge clip %f %f %f %f ndc %f\n",c2.x,c2.y,c2.z,c2.w,c2.z/c2.w);
 return 0;
}
