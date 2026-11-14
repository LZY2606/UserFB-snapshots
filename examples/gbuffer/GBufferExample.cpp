/*
 * Windowless deferred-lighting G-buffer example for the software renderer.
 *
 * One fragment fills three MTR color outputs at once:
 *   slot 0: albedo (base color)
 *   slot 1: view-space normal encoded into RGBA8
 *   slot 2: material parameters (roughness / metallic / ao)
 * plus one depth attachment shared by all three. No scene is redrawn and no
 * window or graphics SDK is required; each target is dumped to a PNG via stb.
 *
 * Build (from the repository root):
 *   c++ -std=c++11 -O0 -pthread -DGLM_FORCE_PURE -D_MSC_EXTENSIONS \
 *       -include cfloat -include limits -include atomic \
 *       -I. -Isrc -Ithird_party/glm -Ithird_party/stb/include -Ithird_party/md5 \
 *       examples/gbuffer/GBufferExample.cpp .task-build/Geometry.o \
 *       .task-build/Logger.o .task-build/ImageUtils.o .task-build/RendererSoft.o \
 *       .task-build/md5.o -o gbuffer_example
 */

#include <cstdio>
#include <memory>
#include <vector>

#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"

using namespace SoftGL;

namespace {

constexpr unsigned ALBEDO_SLOT = 0;
constexpr unsigned NORMAL_SLOT = 1;
constexpr unsigned MATERIAL_SLOT = 2;

// vertex shader: pass clip position and a per-vertex "normal" through varyings
class GBufferVS : public ShaderSoft {
  glm::vec4 *attributes_ = nullptr;
  glm::vec4 *varyings_ = nullptr;
  std::vector<std::string> defines_;
  std::vector<UniformDesc> uniforms_;

 public:
  void shaderMain() override {
    gl->Position = *attributes_;
    // encode a simple view-space normal in the varying (z kept positive)
    glm::vec3 n = glm::normalize(glm::vec3(attributes_->x, attributes_->y, 1.f));
    *varyings_ = glm::vec4(n, 1.f);
  }
  void bindDefines(void *) override {}
  void bindBuiltin(void *p) override { gl = static_cast<ShaderBuiltin *>(p); }
  void bindShaderAttributes(void *p) override { attributes_ = static_cast<glm::vec4 *>(p); }
  void bindShaderUniforms(void *) override {}
  void bindShaderVaryings(void *p) override { varyings_ = static_cast<glm::vec4 *>(p); }
  size_t getShaderUniformsSize() override { return 16; }
  size_t getShaderVaryingsSize() override { return sizeof(glm::vec4); }
  std::vector<std::string> &getDefines() override { return defines_; }
  std::vector<UniformDesc> &getUniformsDesc() override { return uniforms_; }
  std::shared_ptr<ShaderSoft> clone() override { return std::make_shared<GBufferVS>(*this); }
};

// fragment shader: write every G-buffer layer from a single fragment execution
class GBufferFS : public ShaderSoft {
  glm::vec4 *varyings_ = nullptr;
  std::vector<std::string> defines_;
  std::vector<UniformDesc> uniforms_;

 public:
  void shaderMain() override {
    glm::vec3 normal = glm::vec3(*varyings_);

    // slot 0: base color
    gl->colorOutput(ALBEDO_SLOT) = glm::vec4(0.8f, 0.2f, 0.2f, 1.f);

    // slot 1: normal mapped from [-1,1] into [0,1] for RGBA8 storage
    gl->colorOutput(NORMAL_SLOT) = glm::vec4(normal * 0.5f + 0.5f, 1.f);

    // slot 2: roughness, metallic, ambient occlusion
    gl->colorOutput(MATERIAL_SLOT) = glm::vec4(0.35f, 0.0f, 1.0f, 1.f);

    // FragColor is the very same object as colorOutput(0)
    if (&gl->colorOutput(0) != &gl->FragColor) {
      gl->discard = true;
    }
  }
  void bindDefines(void *) override {}
  void bindBuiltin(void *p) override { gl = static_cast<ShaderBuiltin *>(p); }
  void bindShaderAttributes(void *) override {}
  void bindShaderUniforms(void *) override {}
  void bindShaderVaryings(void *p) override { varyings_ = static_cast<glm::vec4 *>(p); }
  size_t getShaderUniformsSize() override { return 16; }
  size_t getShaderVaryingsSize() override { return sizeof(glm::vec4); }
  std::vector<std::string> &getDefines() override { return defines_; }
  std::vector<UniformDesc> &getUniformsDesc() override { return uniforms_; }
  std::shared_ptr<ShaderSoft> clone() override { return std::make_shared<GBufferFS>(*this); }
};

std::shared_ptr<Texture> createColor(RendererSoft &renderer, int w, int h) {
  TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = TextureFormat_RGBA8;
  desc.usage = TextureUsage_AttachmentColor;
  auto tex = renderer.createTexture(desc);
  tex->initImageData();
  return tex;
}

std::shared_ptr<Texture> createDepth(RendererSoft &renderer, int w, int h) {
  TextureDesc desc;
  desc.width = w;
  desc.height = h;
  desc.format = TextureFormat_FLOAT32;
  desc.usage = TextureUsage_AttachmentDepth;
  auto tex = renderer.createTexture(desc);
  tex->initImageData();
  return tex;
}

}  // namespace

int main() {
  const int W = 16;
  const int H = 16;

  RendererSoft renderer;

  // three sparse color targets + one shared depth target
  auto albedo = createColor(renderer, W, H);
  auto normal = createColor(renderer, W, H);
  auto material = createColor(renderer, W, H);
  auto depth = createDepth(renderer, W, H);

  auto fbo = renderer.createFrameBuffer(true);
  fbo->setColorAttachment(ALBEDO_SLOT, albedo, 0);
  fbo->setColorAttachment(NORMAL_SLOT, normal, 0);
  fbo->setColorAttachment(MATERIAL_SLOT, material, 0);
  // slot 3 stays unbound on purpose; sparse binding is legal
  fbo->setDepthAttachment(depth);

  // one full-screen triangle
  std::vector<glm::vec4> vertices = {
      glm::vec4(-1, -1, 0, 1),
      glm::vec4(1, -1, 0, 1),
      glm::vec4(0, 1, 0, 1),
  };
  int32_t indices[] = {0, 1, 2};
  VertexArray va;
  va.indexBuffer = indices;
  va.indexBufferLength = sizeof(indices);
  va.vertexSize = sizeof(glm::vec4);
  va.vertexesDesc.push_back({4, sizeof(glm::vec4), 0});
  va.vertexesBuffer = reinterpret_cast<uint8_t *>(vertices.data());
  va.vertexesBufferLength = vertices.size() * sizeof(glm::vec4);
  auto vao = renderer.createVertexArrayObject(va);

  auto program = renderer.createShaderProgram();
  auto *softProgram = dynamic_cast<ShaderProgramSoft *>(program.get());
  softProgram->SetShaders(std::make_shared<GBufferVS>(), std::make_shared<GBufferFS>());

  // example of per-attachment state: albedo and material inherit the global
  // state, while the normal target only writes RGB (its alpha keeps the clear
  // value). A mask of 0 would disable color entirely while depth still updates
  // because the depth attachment is shared by every color slot.
  RenderStates states;
  states.depthTest = true;
  states.depthFunc = DepthFunc_LESS;
  states.setAttachmentWriteMask(NORMAL_SLOT, ColorWrite_R | ColorWrite_G | ColorWrite_B);
  auto pipeline = renderer.createPipelineStates(states);

  ClearStates clear;
  clear.colorFlag = true;
  clear.depthFlag = true;
  clear.clearColor = glm::vec4(0, 0, 0, 1);
  clear.clearDepth = 1.f;

  renderer.beginRenderPass(fbo, clear);
  renderer.setViewPort(0, 0, W, H);
  renderer.setVertexArrayObject(vao);
  renderer.setShaderProgram(program);
  renderer.setPipelineStates(pipeline);
  renderer.draw();
  renderer.endRenderPass();

  // dump every bound layer; stb writes PNGs without any window system
  albedo->dumpImage("gbuffer_albedo.png", 0, 0);
  normal->dumpImage("gbuffer_normal.png", 0, 0);
  material->dumpImage("gbuffer_material.png", 0, 0);

  std::printf("G-buffer written: gbuffer_albedo.png, gbuffer_normal.png, gbuffer_material.png\n");
  std::printf("bound slots: 0=%d 1=%d 2=%d 3=%d\n",
              fbo->isColorReady(0), fbo->isColorReady(1), fbo->isColorReady(2), fbo->isColorReady(3));
  return 0;
}
