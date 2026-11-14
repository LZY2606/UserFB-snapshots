#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include <cstdio>
#include <cstring>
int main(){
 M4::Allocator a;M4::HLSLTree tree(&a);
 const char*s="struct Leaf {float2 uv:TEXCOORD0;}; struct Inputs{Leaf data;}; float4 main(Inputs input):POSITION{return float4(input.data.uv,0,1);}";
 M4::HLSLParser parser(&a,"input.hlsl",s,strlen(s));if(!parser.Parse(&tree))return 1;
 M4::GLSLGenerator g;if(!g.Generate(&tree,M4::GLSLGenerator::Target_VertexShader,M4::GLSLGenerator::Version_140,"main"))return 2;
 const char*out=g.GetResult();if(!out||!strstr(out,"gl_Position")||strstr(out,"input0.data.uv =")||strstr(out,"in vec2 TEXCOORD0;"))return 3;
 puts("BASELINE_GAP nested_input_unmarshalled=1");return 0;
}
