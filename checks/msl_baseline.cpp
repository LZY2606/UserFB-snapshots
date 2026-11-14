#include "HLSLParser.h"
#include "MSLGenerator.h"
#include <cstring>
#include <cstdio>
int main(){M4::Allocator a;M4::HLSLTree t(&a);
 const char*s="float4 main(float2 uv:TEXCOORD0):COLOR0{return float4(uv,0,1);}";
 M4::HLSLParser p(&a,"legacy.hlsl",s,strlen(s));if(!p.Parse(&t))return 1;
 M4::MSLGenerator g;if(!g.Generate(&t,M4::MSLGenerator::Target_FragmentShader,"main"))return 2;
 const char*r=g.GetResult();if(!r||!strstr(r,"fragment ")||!strstr(r,"return main.main("))return 3;
 puts("MSL_BASELINE_OK actual_fragment_wrapper=1");return 0;}
