#include <stdio.h>
#include <math.h>
#include "mathc.h"
int main(void) {
    mfloat_t m[16], p[4]={1,1,0,1}, out[4];
    mat4_identity(m); m[0]=1.5f; m[5]=2;
    vec4_multiply_mat4(out,p,m);
    if(fabs(out[0]-1.5)>1e-6 || fabs(out[1]-2)>1e-6 || out[3]!=1) return 1;
    printf("blended affine position=(%.3f,%.3f,%.3f,%.3f)\n",out[0],out[1],out[2],out[3]);
    mfloat_t n[4]={1,1,0,0};
    vec4_multiply_mat4(out,n,m);
    printf("forward-direction ratio=%.3f; normal contract requires ratio=1.333333\n",out[0]/out[1]);
    puts("BASELINE: matrix/vector API works; no skinning frame helper exists");
    return 0;
}
