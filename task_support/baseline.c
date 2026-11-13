#include "renderer/core/graphics.h"
#include "renderer/shaders/pbr_shader.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
static int count;
static vec4_t vs(void* a,void* v,void* u){pbr_varyings_t* p=v;(void)u;memset(p,0,sizeof(*p));p->world_normal.z=1;return *(vec4_t*)a;}
static vec4_t fs(void* v,void* u,int* d,int b){++count;return pbr_fragment_shader(v,u,d,b);}
int main(void){framebuffer_t* f=framebuffer_create(16,16);program_t* p=program_create(vs,fs,sizeof(vec4_t),sizeof(pbr_varyings_t),sizeof(pbr_uniforms_t),1,0);pbr_uniforms_t* u=program_get_uniforms(p);vec4_t a[3]={{-.8f,-.8f,0,1},{.8f,-.8f,0,1},{0,.8f,0,1}};int i;memset(u,0,sizeof(*u));u->workflow=METALNESS_WORKFLOW;u->basecolor_factor=vec4_new(.7f,.4f,.2f,1);u->roughness_factor=.4f;u->camera_pos.z=3;u->layer_view=3;for(i=0;i<3;++i)memcpy(program_get_attribs(p,i),a+i,sizeof(a[i]));graphics_draw_triangle(f,p);i=8+8*16;if(count<=0||abs(f->color_buffer[4*i]-102)>1||fabsf(f->depth_buffer[i]-.5f)>1e-5)return 1;printf("PBR roughness byte=%u depth=%.1f real fragments=%d\n",f->color_buffer[4*i],f->depth_buffer[i],count);program_release(p);framebuffer_release(f);return 0;}
