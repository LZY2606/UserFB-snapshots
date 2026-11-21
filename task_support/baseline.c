#include "anim.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int closef(float a,float b){return fabsf(a-b)<0.0001f;}
int main(void){
 struct anm_node root,child;float m[16],inv[16],local[16];
 assert(anm_init_node(&root)==0);assert(anm_init_node(&child)==0);
 anm_link_node(&root,&child);
 anm_set_position3f(&root,1,0,0,0);anm_set_position3f(&root,3,0,0,1000);
 anm_set_rotation_axis(&root,1.5707963267948966f,0,0,1,0);
 anm_set_position3f(&child,2,0,0,0);anm_set_position3f(&child,4,0,0,1000);
 anm_get_node_matrix(&child,local,500);assert(closef(local[12],3));
 anm_get_matrix(&child,m,500);assert(closef(m[12],2)&&closef(m[13],3));
 assert(closef(m[0]+m[12],2)&&closef(m[1]+m[13],4));
 anm_get_inv_matrix(&child,inv,500);
 for(int c=0;c<4;c++)for(int r=0;r<4;r++){
  float v=0;for(int k=0;k<4;k++)v+=m[k*4+r]*inv[c*4+k];
  assert(closef(v,c==r?1:0));
 }
 anm_eval(&root,500);for(int i=0;i<16;i++)assert(closef(child.matrix[i],m[i]));
 anm_get_matrix(&child,m,250);assert(closef(m[12],1.5f)&&closef(m[13],2.5f));
 anm_get_matrix(&child,m,500);assert(closef(m[12],2)&&closef(m[13],3));
 anm_unlink_node(&root,&child);anm_destroy_node(&child);anm_destroy_node(&root);
 puts("ANIMATION_BASELINE_OK world=(2,3,0) vertex=(2,4,0) inverse=identity prepass=match replay=match");return 0;
}
