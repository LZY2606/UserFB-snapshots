#include "HLSLParser.h"
#include "GLSLGenerator.h"
#include <cstring>
#include <iostream>
using namespace M4;
static bool translate(const char* source, std::string& out) {
 Allocator alloc; HLSLTree tree(&alloc);
 HLSLParser parser(&alloc,"baseline.hlsl",source,std::strlen(source));
 if(!parser.Parse(&tree)) return false;
 GLSLGenerator gen;
 if(!gen.Generate(&tree,GLSLGenerator::Target_FragmentShader,GLSLGenerator::Version_140,"main"))return false;
 out=gen.GetResult();return true;
}
int main(){
 std::string out;
 if(!translate("float4 main(float2 uv:TEXCOORD0):COLOR0 { float3x3 m=float3x3(1,2,3,4,5,6,7,8,9); return float4(mul(float3(1,2,3),m),1); }",out))return 1;
 if(out.find("mat3")==std::string::npos || out.find("void main")==std::string::npos)return 2;
 std::cout<<"SQUARE_TRANSLATE_OK bytes="<<out.size()<<"\n";
 bool ok=translate("float4 main(float2 uv:TEXCOORD0):COLOR0 { float4x3 m; return float4(mul(float4(1,2,3,4),m),1); }",out);
 std::cout<<"RECT4x3 parse_generate="<<ok<<" unmapped_type="<<(ok && out.find("? ")!=std::string::npos)<<"\n";
 ok=translate("float4 main(float2 uv:TEXCOORD0):COLOR0 { float2x3 m; return float4(m[0],1); }",out);
 std::cout<<"RECT2x3 parse_generate="<<ok<<"\n";
 return 0;
}
