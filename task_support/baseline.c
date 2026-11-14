#include "renderer/core/graphics.h"
#include <math.h>
#include <stdio.h>
typedef struct {vec4_t pos;float value;} Attr;
static vec4_t vs(void*a,void*v,void*u){(void)u;Attr*p=a;*(float*)v=p->value;return p->pos;}
static vec4_t fs(void*v,void*u,int*discard,int back){(void)v;(void)back;int mode=*(int*)u;if(mode==1)*discard=1;return mode==2?vec4_new(0,1,0,1):vec4_new(1,0,0,1);}
static void coords(program_t*p,float z){vec4_t ps[3]={{-.7f,-.6f,z,1},{.65f,-.45f,z,1},{.05f,.75f,z,1}};for(int i=0;i<3;i++){Attr*a=program_get_attribs(p,i);a->pos=ps[i];a->value=(float)i;}}
int main(void){framebuffer_t*f=framebuffer_create(32,24);program_t*p=program_create(vs,fs,sizeof(Attr),sizeof(float),sizeof(int),1,0);int*u=program_get_uniforms(p);*u=0;coords(p,0);graphics_draw_triangle(f,p);int n=0;for(int i=0;i<768;i++)if(f->color_buffer[4*i])n++;float d=f->depth_buffer[12*32+16];printf("SEED_TRIANGLE red_pixels=%d center_depth=%.3f\n",n,d);int ok=n>0&&fabsf(d-.5f)<1e-6f;
 *u=2;coords(p,.5f);graphics_draw_triangle(f,p);ok&=f->color_buffer[(12*32+16)*4]==255;printf("SEED_OCCLUSION center_red=%d\n",f->color_buffer[(12*32+16)*4]);
 framebuffer_clear_color(f,vec4_new(0,0,0,1));framebuffer_clear_depth(f,1);*u=1;coords(p,-.5f);graphics_draw_triangle(f,p);*u=2;coords(p,.5f);graphics_draw_triangle(f,p);int green=f->color_buffer[(12*32+16)*4+1];d=f->depth_buffer[12*32+16];printf("SEED_DISCARD behind_green=%d depth=%.3f\n",green,d);ok&=green==255&&fabsf(d-.75f)<1e-6f;program_release(p);framebuffer_release(f);if(!ok)return 2;puts("RENDERER_BASELINE_OK");return 0;}
