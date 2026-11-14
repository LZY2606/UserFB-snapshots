#include "glsl_optimizer.h"
#include <cassert>
#include <cstdio>
#include <cstring>
int main(){
 const char* src="#version 430\nlayout(std430,binding=4) buffer Original { uint data[]; } orig;uniform float gain;out vec4 color;void main(){float a=gain;for(int i=0;i<3;++i){a=a*.5+float(i);orig.data[0]+=1u;}if(gain<0.0)discard;color.xy=vec2(a,-a);color.zw=vec2(0.0,1.0);}";
 auto c=glslopt_initialize(kGlslTargetOpenGL);auto s=glslopt_optimize(c,kGlslOptShaderFragment,src,0);
 if(!glslopt_get_status(s))std::puts(glslopt_get_log(s));assert(glslopt_get_status(s));
 assert(glslopt_shader_get_buffer_count(s)==1);auto b=glslopt_shader_get_buffer_desc(s,0);assert(b&&b->kind==kGlslBufferStorage&&b->binding==4&&b->layout==kGlslLayoutStd430);
 assert(std::strstr(glslopt_get_output(s),"discard"));assert(std::strstr(glslopt_get_output(s),"orig"));
 int a,t,f;glslopt_shader_get_stats(s,&a,&t,&f);std::printf("POSTPROCESS_STATS alu=%d texture=%d flow=%d\n",a,t,f);
 glslopt_shader_delete(s);glslopt_cleanup(c);std::puts("POSTPROCESS_BASELINE_OK");
}
