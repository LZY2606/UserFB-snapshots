#include "spirv_reflect.h"
#include <cassert>
#include <fstream>
#include <vector>
#include <iostream>
int main() {
 std::ifstream in("checks/layout.spv",std::ios::binary|std::ios::ate); assert(in);
 auto bytes=in.tellg(); assert(bytes>0 && bytes%4==0); std::vector<uint32_t> code(size_t(bytes)/4);
 in.seekg(0); in.read(reinterpret_cast<char*>(code.data()),bytes); auto original=code;
 SpvReflectShaderModule m{}; assert(spvReflectCreateShaderModule(code.size()*4,code.data(),&m)==SPV_REFLECT_RESULT_SUCCESS);
 assert(m.descriptor_binding_count==2); SpvReflectResult result;
 auto u=spvReflectGetDescriptorBinding(&m,2,1,&result); assert(result==SPV_REFLECT_RESULT_SUCCESS && u);
 assert(u->block.member_count==2 && u->block.members[0].size==48);
 assert(u->block.members[0].numeric.matrix.stride==16 && u->block.members[1].offset==48);
 auto out=spvReflectGetDescriptorBinding(&m,3,2,&result); assert(out && out->block.members[1].offset==16);
 assert(spvReflectChangeDescriptorBindingNumbers(&m,u,6,4)==SPV_REFLECT_RESULT_SUCCESS);
 assert(code==original); auto n=spvReflectGetCodeSize(&m); auto p=spvReflectGetCode(&m);
 SpvReflectShaderModule again{}; assert(spvReflectCreateShaderModule(n,p,&again)==SPV_REFLECT_RESULT_SUCCESS);
 assert(spvReflectGetDescriptorBinding(&again,6,4,&result));
 spvReflectDestroyShaderModule(&again); spvReflectDestroyShaderModule(&m);
 std::cout<<"REFLECT_BASELINE_OK row_major=48 array_offset=48 runtime_offset=16 remap=4:6\n";
}
