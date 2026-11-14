#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"
#include <atomic>
#include <iostream>
using namespace SoftGL;
static std::atomic<unsigned> calls{0};
class SolidShader:public ShaderSoft {
 bool vertex_;
 glm::vec4 *attributes_=nullptr,*varyings_=nullptr;
 std::vector<std::string> defines_;
 std::vector<UniformDesc> uniforms_;
 public:
 explicit SolidShader(bool vertex):vertex_(vertex){}
 void shaderMain() override {
  if(vertex_){gl->Position=*attributes_;*varyings_=glm::vec4(1,.25f,0,1);}
  else {gl->FragColor=*varyings_;++calls;}
 }
 void bindDefines(void*) override {}
 void bindBuiltin(void *p) override {gl=static_cast<ShaderBuiltin*>(p);}
 void bindShaderAttributes(void*p) override {attributes_=static_cast<glm::vec4*>(p);}
 void bindShaderUniforms(void*) override {}
 void bindShaderVaryings(void*p) override {varyings_=static_cast<glm::vec4*>(p);}
 size_t getShaderUniformsSize() override {return 16;}
 size_t getShaderVaryingsSize() override {return sizeof(glm::vec4);}
 std::vector<std::string>& getDefines() override {return defines_;}
 std::vector<UniformDesc>& getUniformsDesc() override {return uniforms_;}
 std::shared_ptr<ShaderSoft> clone() override {return std::make_shared<SolidShader>(*this);}
};
int main(){
 RendererSoft renderer;
 TextureDesc desc;desc.width=8;desc.height=8;desc.multiSample=true;
 desc.format=TextureFormat_RGBA8;desc.usage=TextureUsage_AttachmentColor;
 auto color=renderer.createTexture(desc);color->initImageData();
 desc.format=TextureFormat_FLOAT32;desc.usage=TextureUsage_AttachmentDepth;
 auto depth=renderer.createTexture(desc);depth->initImageData();
 auto fbo=renderer.createFrameBuffer(true);
 fbo->setColorAttachment(color,0);fbo->setDepthAttachment(depth);
 glm::vec4 vertices[]={glm::vec4(-1,-1,0,1),glm::vec4(1,-1,0,1),glm::vec4(0,1,0,1)};
 int32_t indices[]={0,1,2};
 VertexArray va;va.indexBuffer=indices;va.indexBufferLength=sizeof(indices);va.vertexSize=sizeof(glm::vec4);va.vertexesDesc.push_back({4,sizeof(glm::vec4),0});
 va.vertexesBuffer=reinterpret_cast<uint8_t*>(vertices);va.vertexesBufferLength=sizeof(vertices);
 auto vao=renderer.createVertexArrayObject(va);
 auto program=renderer.createShaderProgram();
 auto *soft=dynamic_cast<ShaderProgramSoft*>(program.get());
 if(!soft->SetShaders(std::make_shared<SolidShader>(true),std::make_shared<SolidShader>(false)))return 1;
 RenderStates state;state.depthTest=true;state.depthFunc=DepthFunc_LESS;
 auto pipeline=renderer.createPipelineStates(state);
 ClearStates clear;clear.colorFlag=true;clear.depthFlag=true;clear.clearColor=glm::vec4(0,0,0,1);
 renderer.beginRenderPass(fbo,clear);renderer.setViewPort(0,0,8,8);
 renderer.setVertexArrayObject(vao);renderer.setShaderProgram(program);renderer.setPipelineStates(pipeline);renderer.draw();
 auto *image=dynamic_cast<TextureSoft<RGBA>*>(color.get());
 auto buffer=image->getImage().getBuffer(0);
 unsigned colored=0;for(int y=0;y<8;++y)for(int x=0;x<8;++x)colored+=buffer->buffer->get(x,y)->r!=0;
 if(!colored || !calls.load())return 2;
 std::cout<<"SINGLE_ATTACHMENT_DRAW_OK colored="<<colored<<" shader_calls="<<calls<<" samples="<<buffer->sampleCnt<<"\n";
 auto prior=*buffer->buffer->get(3,3);
 for(auto &v:vertices)v.z=.5f;
 vao->updateVertexData(vertices,sizeof(vertices));renderer.draw();
 if(*buffer->buffer->get(3,3)!=prior)return 3;
 renderer.endRenderPass();
 std::cout<<"DEPTH_OCCLUSION_OK\n";
 return 0;
}
