#include "glslang/Public/ShaderLang.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/MachineIndependent/localintermediate.h"
#include "SPIRV/GlslangToSpv.h"
#include <cstdio>
#include <map>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"BASELINE_FAIL line=%d %s\n",__LINE__,#x); return 1; } } while(0)
struct Observe : glslang::TIntermTraverser {
    unsigned functions=0, calls=0, constParameters=0;
    bool arity=true;
    Observe():TIntermTraverser(true,false,false){}
    bool visitAggregate(glslang::TVisit,glslang::TIntermAggregate* n) override {
        if(n->getOp()==glslang::EOpFunction) {
            ++functions;
            auto* params=n->getSequence()[0]->getAsAggregate();
            for(auto* p:params->getSequence())
                if(p->getAsTyped()->getQualifier().storage==glslang::EvqConstReadOnly) ++constParameters;
        }
        if(n->getOp()==glslang::EOpFunctionCall && n->isUserDefined()) {
            ++calls;
            arity=arity && n->getSequence().size()==2 && n->getQualifierList().size()==2;
        }
        return true;
    }
};
static int compileExisting() {
    const char* source=R"glsl(#version 450
layout(location=0) flat in int mode;
layout(location=1) in float value;
layout(location=0) out vec4 color;
float material(const in int kind, float x) {
    float y=x*2.0;
    if(kind==1) return y+3.0;
    return y-5.0;
}
void main() {
    color=vec4(material(1,value),material(2,value+1.0),material(mode,value-1.0),1.0);
}
)glsl";
    glslang::TShader shader(EShLangFragment);
    shader.setStrings(&source,1);
    shader.setEnvInput(glslang::EShSourceGlsl,EShLangFragment,glslang::EShClientVulkan,450);
    shader.setEnvClient(glslang::EShClientVulkan,glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv,glslang::EShTargetSpv_1_0);
    auto messages=static_cast<EShMessages>(EShMsgSpvRules|EShMsgVulkanRules);
    if(!shader.parse(GetDefaultResources(),450,false,messages)) {
        std::fprintf(stderr,"%s\n",shader.getInfoLog());return 1;
    }
    glslang::TProgram program;program.addShader(&shader);
    if(!program.link(messages)) {std::fprintf(stderr,"%s\n",program.getInfoLog());return 1;}
    auto* ir=program.getIntermediate(EShLangFragment); REQUIRE(ir && ir->getTreeRoot());
    Observe o;ir->getTreeRoot()->traverse(&o);
    REQUIRE(o.functions==2 && o.calls==3 && o.constParameters==1 && o.arity);
    std::vector<unsigned> words;glslang::SpvOptions options;options.disableOptimizer=true;
    glslang::GlslangToSpv(*ir,words,&options);
    REQUIRE(words.size()>5 && words[0]==0x07230203);
    // SPIR-V OpFunction=54, OpFunctionParameter=55, OpFunctionCall=57.
    std::map<unsigned,unsigned> parameters;std::vector<std::pair<unsigned,unsigned>> calls;
    unsigned current=0,functions=0;
    for(size_t i=5;i<words.size();) {
        unsigned count=words[i]>>16,op=words[i]&65535;
        REQUIRE(count && i+count<=words.size());
        if(op==54) {REQUIRE(count==5);current=words[i+2];parameters[current]=0;++functions;}
        if(op==55) {REQUIRE(current && count==3);++parameters[current];}
        if(op==57) {REQUIRE(count>=4);calls.push_back({words[i+3],count-4});}
        if(op==56) current=0;
        i+=count;
    }
    REQUIRE(functions==2 && calls.size()==3);
    for(auto c:calls) REQUIRE(parameters.count(c.first) && parameters[c.first]==2 && c.second==2);
    std::puts("BASELINE_OK ir_functions=2 ir_calls=3 const_parameters=1 spv_functions=2 spv_calls=3");
    return 0;
}
int main() {
    if(!glslang::InitializeProcess()) return 1;
    int rc=compileExisting();glslang::FinalizeProcess();return rc;
}
