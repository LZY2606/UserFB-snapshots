#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "renderer/shaders/pbr_shader.h"
int main(void) {
    pbr_uniforms_t u;
    pbr_varyings_t v;
    vec4_t texel={.8f,.5f,.9f,1}, front,back;
    texture_t map={1,1,&texel};
    int discard=0;
    memset(&u,0,sizeof u); memset(&v,0,sizeof v);
    u.workflow=METALNESS_WORKFLOW;
    u.basecolor_factor=vec4_new(1,1,1,1); u.roughness_factor=.5f;
    u.layer_view=5; u.normal_map=&map;
    v.clip_position=vec4_new(0,0,0,1);
    v.world_tangent=vec3_new(1,0,0); v.world_bitangent=vec3_new(0,1,0);
    v.world_normal=vec3_new(0,0,1);
    front=pbr_fragment_shader(&v,&u,&discard,0);
    back=pbr_fragment_shader(&v,&u,&discard,1);
    assert(!discard && fabs(front.x-.8)<1e-6 && fabs(front.z-.9)<1e-6);
    assert(fabs(front.x+back.x-1)<1e-6);
    printf("single normal front=(%.3f %.3f %.3f), back.x=%.3f\n",front.x,front.y,front.z,back.x);
    return 0;
}
