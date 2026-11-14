#include "glsl_optimizer.h"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
const char* text="#version 430\nuniform sampler2D albedo;layout(rgba32f,binding=5) uniform image2D dst;in vec2 uv;out vec4 c;void main(){c=textureLod(albedo,uv,0.0);imageStore(dst,ivec2(0),c);}";
auto ctx=glslopt_initialize(kGlslTargetMetal);auto s=glslopt_optimize(ctx,kGlslOptShaderFragment,text,0);
if(!glslopt_get_status(s))std::puts(glslopt_get_log(s));assert(glslopt_get_status(s));
assert(glslopt_shader_get_texture_count(s)==2);bool foundSampler=false,foundImage=false;
for(int i=0;i<2;++i){const char* n;glslopt_basic_type t;glslopt_precision p;int v,m,a,l;glslopt_shader_get_texture_desc(s,i,&n,&t,&p,&v,&m,&a,&l);
if(!std::strcmp(n,"albedo")){assert(l==0);foundSampler=true;}if(!std::strcmp(n,"dst")){assert(l==1);foundImage=true;}}
assert(foundSampler&&foundImage);assert(std::strstr(glslopt_get_output(s),"[[texture(0)]]"));assert(std::strstr(glslopt_get_output(s),"[[texture(1)]]"));assert(std::strstr(glslopt_get_output(s),".sample("));assert(std::strstr(glslopt_get_output(s),".write("));
glslopt_shader_delete(s);glslopt_cleanup(ctx);std::puts("METAL_BASELINE_OK");}
