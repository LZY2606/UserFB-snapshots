#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include <cstdio>
#include <cstring>
int main(){
 M4::Allocator a; M4::HLSLTree tree(&a);
 const char *s="float4 main(float2 uv:TEXCOORD0):COLOR0 {float x=0.0; if(uv.x>0.0) x=ddx(uv.y);return float4(x,0.0,0.0,1.0);}";
 M4::HLSLParser p(&a,"baseline.hlsl",s,strlen(s));
 if(!p.Parse(&tree))return 1;
 M4::GLSLGenerator g;
 if(!g.Generate(&tree,M4::GLSLGenerator::Target_FragmentShader,M4::GLSLGenerator::Version_140,"main"))return 2;
 const char *out=g.GetResult();
 if(!out || !strstr(out,"dFdx") || !strstr(out,"if"))return 3;
 printf("DERIVATIVE_TRANSLATED bytes=%zu\n",strlen(out));
 puts("BASELINE_OK");return 0;
}
