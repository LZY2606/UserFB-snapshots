/*
 * MTR (multiple render target) offscreen tests for the software renderer.
 *
 * Registered with task_support/driver.c in three groups:
 *   routing     - sparse slot binding, output-0 alias, call count, LESS fan-out,
 *                 rebind refresh and framebuffer validation
 *   composition - per-attachment blending, write masks, discard, early-z both
 *                 modes, points/lines and the retained cubemap slot-0 overload
 *   resolve     - four-sample per-attachment resolve and single-sample control
 */

#include "task_support/check.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"

using namespace SoftGL;

namespace {

constexpr unsigned SLOTS = FrameBuffer::COLOR_ATTACHMENT_COUNT;

// ---------------------------------------------------------------------------
// shaders
// ---------------------------------------------------------------------------

class GVS : public ShaderSoft {
  glm::vec4 *attributes_ = nullptr;
  glm::vec4 *varyings_ = nullptr;
  std::vector<std::string> defines_;
  std::vector<UniformDesc> uniforms_;

 public:
  void shaderMain() override {
    gl->Position = *attributes_;
    *varyings_ = *attributes_;
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
  std::shared_ptr<ShaderSoft> clone() override { return std::make_shared<GVS>(*this); }
};

class GFS : public ShaderSoft {
  glm::vec4 *varyings_ = nullptr;
  std::vector<std::string> defines_;
  std::vector<UniformDesc> uniforms_;

 public:
  glm::vec4 outColor[SLOTS] = {
      glm::vec4(1, 0, 0, 1), glm::vec4(0, 1, 0, 1),
      glm::vec4(0, 0, 1, 1), glm::vec4(1, 1, 0, 1),
  };
  bool doDiscard = false;
  float discardZ = 0.5f;
  bool writeViaAccessor = false;
  std::atomic<unsigned> *calls = nullptr;
  std::shared_ptr<bool> aliasOk;

  void shaderMain() override {
    if (calls) {
      calls->fetch_add(1);
    }
    if (writeViaAccessor) {
      // write slot 0 only through the accessor, FragColor must be the same object
      gl->colorOutput(0) = glm::vec4(0.2f, 0.4f, 0.6f, 1.f);
      if (aliasOk) {
        *aliasOk = (&gl->colorOutput(0) == &gl->FragColor) && gl->FragColor == glm::vec4(0.2f, 0.4f, 0.6f, 1.f);
      }
      return;
    }
    for (unsigned s = 0; s < SLOTS; s++) {
      gl->colorOutput(s) = outColor[s];
    }
    if (doDiscard && gl->FragCoord.z >= discardZ) {
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
  std::shared_ptr<ShaderSoft> clone() override { return std::make_shared<GFS>(*this); }
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

struct Mesh {
  std::vector<glm::vec4> verts;
  std::vector<int32_t> idx;

  std::shared_ptr<VertexArrayObject> build(RendererSoft &r) const {
    VertexArray va;
    va.indexBuffer = const_cast<int32_t *>(idx.data());
    va.indexBufferLength = idx.size() * sizeof(int32_t);
    va.vertexSize = sizeof(glm::vec4);
    va.vertexesDesc.push_back({4, sizeof(glm::vec4), 0});
    va.vertexesBuffer = const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(verts.data()));
    va.vertexesBufferLength = verts.size() * sizeof(glm::vec4);
    return r.createVertexArrayObject(va);
  }
};

Mesh triMesh(float z) {
  Mesh m;
  m.verts = {glm::vec4(-1, -1, z, 1), glm::vec4(1, -1, z, 1), glm::vec4(0, 1, z, 1)};
  m.idx = {0, 1, 2};
  return m;
}

Mesh offscreenMesh() {
  Mesh m;
  m.verts = {glm::vec4(2, -1, 0, 1), glm::vec4(3, -1, 0, 1), glm::vec4(2, 1, 0, 1)};
  m.idx = {0, 1, 2};
  return m;
}

std::shared_ptr<Texture> makeColor(RendererSoft &r, int w, int h, bool ms, bool cube = false, bool mip = false) {
  TextureDesc d;
  d.width = w;
  d.height = h;
  d.multiSample = ms;
  d.type = cube ? TextureType_CUBE : TextureType_2D;
  d.format = TextureFormat_RGBA8;
  d.usage = TextureUsage_AttachmentColor;
  d.useMipmaps = mip;
  auto t = r.createTexture(d);
  t->initImageData();
  return t;
}

std::shared_ptr<Texture> makeDepth(RendererSoft &r, int w, int h, bool ms) {
  TextureDesc d;
  d.width = w;
  d.height = h;
  d.multiSample = ms;
  d.format = TextureFormat_FLOAT32;
  d.usage = TextureUsage_AttachmentDepth;
  auto t = r.createTexture(d);
  t->initImageData();
  return t;
}

std::shared_ptr<ShaderProgram> makeProgram(RendererSoft &r, const std::shared_ptr<GFS> &fs) {
  auto p = r.createShaderProgram();
  auto *soft = dynamic_cast<ShaderProgramSoft *>(p.get());
  soft->SetShaders(std::make_shared<GVS>(), fs);
  return p;
}

ClearStates defaultClear(glm::vec4 color = glm::vec4(0, 0, 0, 1)) {
  ClearStates c;
  c.colorFlag = true;
  c.depthFlag = true;
  c.clearColor = color;
  c.clearDepth = 1.f;
  return c;
}

void runDraw(RendererSoft &r, std::shared_ptr<FrameBuffer> &fbo, const ClearStates &clr,
             std::shared_ptr<VertexArrayObject> &vao, std::shared_ptr<ShaderProgram> &prog,
             std::shared_ptr<PipelineStates> &ps, int w, int h) {
  r.beginRenderPass(fbo, clr);
  r.setViewPort(0, 0, w, h);
  r.setVertexArrayObject(vao);
  r.setShaderProgram(prog);
  r.setPipelineStates(ps);
  r.draw();
  r.endRenderPass();
}

RGBA c8(const glm::vec4 &c) {
  return RGBA(c * 255.f);
}

bool near(const RGBA &a, const RGBA &b, int tol = 1) {
  return std::abs(int(a.r) - int(b.r)) <= tol && std::abs(int(a.g) - int(b.g)) <= tol
      && std::abs(int(a.b) - int(b.b)) <= tol && std::abs(int(a.a) - int(b.a)) <= tol;
}

const RGBA *colorPx(const std::shared_ptr<Texture> &tex, int x, int y, int level = 0, uint32_t layer = 0) {
  auto *t = dynamic_cast<TextureSoft<RGBA> *>(tex.get());
  return t->getImage(layer).getBuffer(level)->buffer->get(x, y);
}

float depthPx(const std::shared_ptr<Texture> &tex, int x, int y) {
  auto *t = dynamic_cast<TextureSoft<float> *>(tex.get());
  return *t->getImage().getBuffer(0)->buffer->get(x, y);
}

int countColor(const std::shared_ptr<Texture> &tex, int w, int h, const RGBA &want) {
  int n = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (near(*colorPx(tex, x, y), want)) {
        n++;
      }
    }
  }
  return n;
}

bool findPixel(const std::shared_ptr<Texture> &tex, int w, int h, const RGBA &want, int &ox, int &oy) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (near(*colorPx(tex, x, y), want)) {
        ox = x;
        oy = y;
        return true;
      }
    }
  }
  return false;
}

RenderStates depthLessState() {
  RenderStates s;
  s.depthTest = true;
  s.depthFunc = DepthFunc_LESS;
  return s;
}

// ---------------------------------------------------------------------------
// routing group
// ---------------------------------------------------------------------------

int routing_sparse_slot2_only() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto c2 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  std::shared_ptr<Texture> none;
  fbo->setColorAttachment(2, c2, 0);
  fbo->setDepthAttachment(depth);

  TASK_CHECK(fbo->isColorReady(2));
  TASK_CHECK(!fbo->isColorReady(0));
  TASK_CHECK(!fbo->isColorReady(1));
  TASK_CHECK(!fbo->isColorReady(3));

  auto *softFbo = dynamic_cast<FrameBufferSoft *>(fbo.get());
  TASK_CHECK(softFbo->getColorBuffer(0) == nullptr);
  TASK_CHECK(softFbo->getColorBuffer(2) != nullptr);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto state = depthLessState();
  auto ps = r.createPipelineStates(state);
  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);

  // only slot 2 exists and receives the slot-2 (blue) output
  int x, y;
  TASK_CHECK(findPixel(c2, W, H, c8(fs->outColor[2]), x, y));
  TASK_CHECK(countColor(c2, W, H, RGBA(0, 0, 0, 255)) >= 1);  // uncovered stays clear
  return 0;
}

int routing_four_slots() {
  RendererSoft r;
  const int W = 4, H = 4;
  std::shared_ptr<Texture> colors[SLOTS];
  for (unsigned s = 0; s < SLOTS; s++) {
    colors[s] = makeColor(r, W, H, false);
  }
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  for (unsigned s = 0; s < SLOTS; s++) {
    fbo->setColorAttachment(s, colors[s], 0);
  }
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);

  int x = -1, y = -1;
  TASK_CHECK(findPixel(colors[0], W, H, c8(fs->outColor[0]), x, y));
  for (unsigned s = 0; s < SLOTS; s++) {
    // every attachment got its own distinct output at the same covered pixel
    TASK_CHECK(near(*colorPx(colors[s], x, y), c8(fs->outColor[s])));
    // and shares one depth
  }
  TASK_CHECK(depthPx(depth, x, y) < 1.f);
  return 0;
}

int routing_output0_alias() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto c0 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setDepthAttachment(depth);

  auto aliasOk = std::make_shared<bool>(false);
  auto fs = std::make_shared<GFS>();
  fs->writeViaAccessor = true;
  fs->aliasOk = aliasOk;
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());

  // point primitive runs the program directly, so a covered pixel is deterministic
  Mesh m;
  m.verts = {glm::vec4(0, 0, 0, 1)};
  m.idx = {0};
  RenderStates rs = depthLessState();
  rs.primitiveType = Primitive_POINT;
  ps = r.createPipelineStates(rs);
  auto vao = m.build(r);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);

  TASK_CHECK(*aliasOk);
  int x, y;
  TASK_CHECK(findPixel(c0, W, H, c8(glm::vec4(0.2f, 0.4f, 0.6f, 1.f)), x, y));
  return 0;
}

int routing_fragment_call_count() {
  RendererSoft r;
  const int W = 8, H = 8;
  auto depthA = makeDepth(r, W, H, false);
  auto depthB = makeDepth(r, W, H, false);
  auto one = makeColor(r, W, H, false);
  std::shared_ptr<Texture> four[SLOTS];
  for (unsigned s = 0; s < SLOTS; s++) {
    four[s] = makeColor(r, W, H, false);
  }

  auto fboOne = r.createFrameBuffer(true);
  fboOne->setColorAttachment(0, one, 0);
  fboOne->setDepthAttachment(depthA);
  auto fboFour = r.createFrameBuffer(true);
  for (unsigned s = 0; s < SLOTS; s++) {
    fboFour->setColorAttachment(s, four[s], 0);
  }
  fboFour->setDepthAttachment(depthB);

  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto fs = std::make_shared<GFS>();
  fs->calls = calls.get();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = triMesh(0.f);
  auto vaoA = mesh.build(r);
  auto vaoB = mesh.build(r);

  runDraw(r, fboOne, defaultClear(), vaoA, prog, ps, W, H);
  unsigned oneCalls = calls->load();
  calls->store(0);
  runDraw(r, fboFour, defaultClear(), vaoB, prog, ps, W, H);
  unsigned fourCalls = calls->load();

  TASK_CHECK(oneCalls > 0);
  TASK_CHECK(oneCalls == fourCalls);  // shader runs once per fragment, not per attachment
  return 0;
}

int routing_less_all_attachments() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto c0 = makeColor(r, W, H, false);
  auto c1 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setColorAttachment(1, c1, 0);
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto meshA = triMesh(0.f);
  auto vaoA = meshA.build(r);
  runDraw(r, fbo, defaultClear(), vaoA, prog, ps, W, H);

  int x = -1, y = -1;
  TASK_CHECK(findPixel(c0, W, H, c8(fs->outColor[0]), x, y));
  // LESS: both attachments are written together from one fragment
  TASK_CHECK(near(*colorPx(c1, x, y), c8(fs->outColor[1])));
  float firstDepth = depthPx(depth, x, y);
  RGBA keep0 = *colorPx(c0, x, y);
  RGBA keep1 = *colorPx(c1, x, y);

  // a second, farther fragment must fail LESS and change neither color nor depth
  for (unsigned s = 0; s < SLOTS; s++) {
    fs->outColor[s] = glm::vec4(0.9f, 0.9f, 0.9f, 1.f);
  }
  auto meshB = triMesh(0.5f);
  auto vaoB = meshB.build(r);
  r.beginRenderPass(fbo, ClearStates());  // no clear, same attachments
  r.setViewPort(0, 0, W, H);
  r.setVertexArrayObject(vaoB);
  r.setShaderProgram(prog);
  r.setPipelineStates(ps);
  r.draw();

  TASK_CHECK(near(*colorPx(c0, x, y), keep0));
  TASK_CHECK(near(*colorPx(c1, x, y), keep1));
  TASK_CHECK(std::abs(depthPx(depth, x, y) - firstDepth) < 1e-6f);
  return 0;
}

int routing_rebind_refresh() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto a = makeColor(r, W, H, false);
  auto b = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, a, 0);
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);
  TASK_CHECK(countColor(a, W, H, c8(fs->outColor[0])) > 0);

  // rebind slot 0 to a fresh image, then the next draw must target it
  fbo->setColorAttachment(0, b, 0);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);
  TASK_CHECK(countColor(b, W, H, c8(fs->outColor[0])) > 0);

  // unbind again: slot 0 becomes empty and the draw still runs (depth only)
  std::shared_ptr<Texture> empty;
  fbo->setColorAttachment(0, empty, 0);
  TASK_CHECK(!fbo->isColorReady(0));
  r.beginRenderPass(fbo, defaultClear());
  r.setViewPort(0, 0, W, H);
  r.setVertexArrayObject(vao);
  r.setShaderProgram(prog);
  r.setPipelineStates(ps);
  r.draw();
  TASK_CHECK(depthPx(depth, W / 2, H / 2) < 1.f);
  return 0;
}

int routing_invalid_throws() {
  // size mismatch
  {
    RendererSoft r;
    auto color = makeColor(r, 4, 4, false);
    auto depth = makeDepth(r, 8, 8, false);
    auto fbo = r.createFrameBuffer(true);
    fbo->setColorAttachment(0, color, 0);
    fbo->setDepthAttachment(depth);
    bool threw = false;
    try {
      r.beginRenderPass(fbo, defaultClear());
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // same image bound twice
  {
    RendererSoft r;
    auto color = makeColor(r, 4, 4, false);
    auto fbo = r.createFrameBuffer(true);
    fbo->setColorAttachment(0, color, 0);
    fbo->setColorAttachment(1, color, 0);
    bool threw = false;
    try {
      r.beginRenderPass(fbo, defaultClear());
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // unallocated attachment
  {
    RendererSoft r;
    TextureDesc d;
    d.width = 4;
    d.height = 4;
    d.format = TextureFormat_RGBA8;
    d.usage = TextureUsage_AttachmentColor;
    auto color = r.createTexture(d);  // no initImageData()
    auto fbo = r.createFrameBuffer(true);
    fbo->setColorAttachment(0, color, 0);
    bool threw = false;
    try {
      r.beginRenderPass(fbo, defaultClear());
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // slot out of range
  {
    RendererSoft r;
    auto fbo = r.createFrameBuffer(true);
    auto color = makeColor(r, 4, 4, false);
    bool threw = false;
    try {
      fbo->setColorAttachment(4, color, 0);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    TASK_CHECK(threw);
  }
  return 0;
}

int routing_mip_level_size() {
  RendererSoft r;
  const int W = 8, H = 8;
  auto mipColor = makeColor(r, W, H, false, false, true);  // 8x8 with a full mip chain
  auto depth4 = makeDepth(r, 4, 4, false);
  auto fbo = r.createFrameBuffer(true);

  // level 1 of an 8x8 texture is 4x4 and matches the 4x4 depth attachment
  fbo->setColorAttachment(0, mipColor, 1);
  fbo->setDepthAttachment(depth4);
  TASK_CHECK(fbo->isColorReady(0));

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, 4, 4);

  // the bound mip level received the draw, using its own 4x4 effective size
  int covered = 0;
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      if (near(*colorPx(mipColor, x, y, 1), c8(fs->outColor[0]))) {
        covered++;
      }
    }
  }
  TASK_CHECK(covered > 0);

  // binding level 0 (8x8) against the same 4x4 depth must be rejected
  fbo->setColorAttachment(0, mipColor, 0);
  bool threw = false;
  try {
    r.beginRenderPass(fbo, defaultClear());
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  TASK_CHECK(threw);
  return 0;
}

// ---------------------------------------------------------------------------
// composition group
// ---------------------------------------------------------------------------

int composition_per_attachment_blend() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto c0 = makeColor(r, W, H, false);
  auto c1 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setColorAttachment(1, c1, 0);
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  fs->outColor[0] = glm::vec4(1, 0, 0, 1);
  fs->outColor[1] = glm::vec4(1, 0, 0, 0.25f);
  auto prog = makeProgram(r, fs);

  RenderStates rs = depthLessState();
  rs.blend = false;  // global blending off
  BlendParameters bp;
  bp.SetBlendFactor(BlendFactor_SRC_ALPHA, BlendFactor_ONE_MINUS_SRC_ALPHA);
  rs.setAttachmentBlend(1, true, bp);  // slot 1 blends against its own target
  auto ps = r.createPipelineStates(rs);

  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);
  runDraw(r, fbo, defaultClear(glm::vec4(0.5f, 0.5f, 0.5f, 1.f)), vao, prog, ps, W, H);

  int x = -1, y = -1;
  TASK_CHECK(findPixel(c0, W, H, RGBA(255, 0, 0, 255), x, y));
  // slot 1: src*0.25 + dst*0.75, dst = (0.5,0.5,0.5,1)
  RGBA expect1 = c8(glm::vec4(0.625f, 0.375f, 0.375f, 0.8125f));
  TASK_CHECK(near(*colorPx(c1, x, y), expect1));
  return 0;
}

int composition_write_mask_and_depth() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto c0 = makeColor(r, W, H, false);
  auto c1 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setColorAttachment(1, c1, 0);
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  fs->outColor[0] = glm::vec4(0.8f, 0.7f, 0.6f, 0.5f);
  auto prog = makeProgram(r, fs);

  RenderStates rs = depthLessState();
  rs.setAttachmentWriteMask(0, ColorWrite_R);  // only red on slot 0
  rs.setAttachmentWriteMask(1, 0);             // slot 1 color fully disabled
  auto ps = r.createPipelineStates(rs);

  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);

  int x = -1, y = -1;
  TASK_CHECK(findPixel(c0, W, H, RGBA(204, 0, 0, 255), x, y));  // g/b/a keep clear value
  // slot 1 never changes color ...
  TASK_CHECK(countColor(c1, W, H, RGBA(0, 0, 0, 255)) == W * H);
  // ... but the shared depth still updates for this sample
  TASK_CHECK(depthPx(depth, x, y) < 1.f);
  // an uncovered top-right pixel keeps its cleared depth
  TASK_CHECK(std::abs(depthPx(depth, W - 1, H - 1) - 1.f) < 1e-6f);
  return 0;
}

int composition_all_masks_depth_only() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto c0 = makeColor(r, W, H, false);
  auto c1 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setColorAttachment(1, c1, 0);
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);

  // disable every color channel on every attachment
  RenderStates rs = depthLessState();
  rs.setAttachmentWriteMask(0, 0);
  rs.setAttachmentWriteMask(1, 0);
  auto ps = r.createPipelineStates(rs);

  auto meshNear = triMesh(0.2f);
  auto vaoNear = meshNear.build(r);
  runDraw(r, fbo, defaultClear(), vaoNear, prog, ps, W, H);

  // colors stay at the clear value ...
  TASK_CHECK(countColor(c0, W, H, RGBA(0, 0, 0, 255)) == W * H);
  TASK_CHECK(countColor(c1, W, H, RGBA(0, 0, 0, 255)) == W * H);
  // ... while the shared depth still records the near fragment
  int cx = W / 2, cy = H / 2;
  TASK_CHECK(std::abs(depthPx(depth, cx, cy) - 0.2f) < 1e-6f);

  // re-enable color and draw a farther fragment: LESS must now occlude it
  RenderStates rsColor = depthLessState();
  auto psColor = r.createPipelineStates(rsColor);
  auto meshFar = triMesh(0.8f);
  auto vaoFar = meshFar.build(r);
  r.beginRenderPass(fbo, ClearStates());  // keep depth, no clear
  r.setViewPort(0, 0, W, H);
  r.setVertexArrayObject(vaoFar);
  r.setShaderProgram(prog);
  r.setPipelineStates(psColor);
  r.draw();
  TASK_CHECK(countColor(c0, W, H, RGBA(0, 0, 0, 255)) == W * H);
  TASK_CHECK(countColor(c1, W, H, RGBA(0, 0, 0, 255)) == W * H);
  TASK_CHECK(std::abs(depthPx(depth, cx, cy) - 0.2f) < 1e-6f);
  return 0;
}

int composition_discard_occlusion() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto c0 = makeColor(r, W, H, false);
  auto c1 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setColorAttachment(1, c1, 0);
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  fs->doDiscard = true;
  fs->discardZ = 0.5f;
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());

  // first fragment maps beyond discardZ -> discarded: no color, no depth
  auto meshFar = triMesh(0.6f);
  auto vaoFar = meshFar.build(r);
  runDraw(r, fbo, defaultClear(), vaoFar, prog, ps, W, H);
  TASK_CHECK(countColor(c0, W, H, RGBA(0, 0, 0, 255)) == W * H);
  TASK_CHECK(countColor(c1, W, H, RGBA(0, 0, 0, 255)) == W * H);
  TASK_CHECK(std::abs(depthPx(depth, W / 2, H / 2) - 1.f) < 1e-6f);

  // a later, nearer fragment now passes because the discarded one wrote no depth
  fs->doDiscard = false;
  auto meshNear = triMesh(0.2f);
  auto vaoNear = meshNear.build(r);
  r.beginRenderPass(fbo, ClearStates());  // keep depth/color, no clear
  r.setViewPort(0, 0, W, H);
  r.setVertexArrayObject(vaoNear);
  r.setShaderProgram(prog);
  r.setPipelineStates(ps);
  r.draw();
  int x, y;
  TASK_CHECK(findPixel(c0, W, H, c8(fs->outColor[0]), x, y));
  TASK_CHECK(near(*colorPx(c1, x, y), c8(fs->outColor[1])));
  TASK_CHECK(depthPx(depth, x, y) < 1.f);
  return 0;
}

int composition_early_z_both_modes() {
  const int W = 4, H = 4;
  RGBA colorA[16];
  RGBA colorB[16];
  float depthA = 1.f;
  float depthB = 1.f;

  for (int mode = 0; mode < 2; mode++) {
    RendererSoft r;
    r.setEnableEarlyZ(mode == 0);
    auto c0 = makeColor(r, W, H, false);
    auto c1 = makeColor(r, W, H, false);
    auto depth = makeDepth(r, W, H, false);
    auto fbo = r.createFrameBuffer(true);
    fbo->setColorAttachment(0, c0, 0);
    fbo->setColorAttachment(1, c1, 0);
    fbo->setDepthAttachment(depth);

    auto fs = std::make_shared<GFS>();
    auto prog = makeProgram(r, fs);
    auto ps = r.createPipelineStates(depthLessState());

    auto meshNear = triMesh(0.f);
    auto vaoNear = meshNear.build(r);
    runDraw(r, fbo, defaultClear(), vaoNear, prog, ps, W, H);
    auto meshFar = triMesh(0.5f);
    auto vaoFar = meshFar.build(r);
    r.beginRenderPass(fbo, ClearStates());
    r.setViewPort(0, 0, W, H);
    r.setVertexArrayObject(vaoFar);
    r.setShaderProgram(prog);
    r.setPipelineStates(ps);
    r.draw();

    RGBA *dst = mode == 0 ? colorA : colorB;
    for (int y = 0; y < H; y++) {
      for (int x = 0; x < W; x++) {
        dst[y * W + x] = *colorPx(c0, x, y);
      }
    }
    float &dOut = mode == 0 ? depthA : depthB;
    dOut = depthPx(depth, W / 2, H / 2);
  }

  TASK_CHECK(depthA < 1.f && depthB < 1.f);
  TASK_CHECK(std::abs(depthA - depthB) < 1e-6f);
  for (int i = 0; i < W * H; i++) {
    TASK_CHECK(near(colorA[i], colorB[i]));
  }
  return 0;
}

int composition_points_and_lines() {
  RendererSoft r;
  const int W = 8, H = 8;
  auto c0 = makeColor(r, W, H, false);
  auto c1 = makeColor(r, W, H, false);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setColorAttachment(1, c1, 0);
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);

  RenderStates rs = depthLessState();
  rs.primitiveType = Primitive_POINT;
  auto psPoint = r.createPipelineStates(rs);
  Mesh point;
  point.verts = {glm::vec4(0, 0, 0, 1)};
  point.idx = {0};
  auto vaoPoint = point.build(r);
  runDraw(r, fbo, defaultClear(), vaoPoint, prog, psPoint, W, H);
  TASK_CHECK(countColor(c0, W, H, c8(fs->outColor[0])) >= 1);
  TASK_CHECK(countColor(c1, W, H, c8(fs->outColor[1])) >= 1);

  RenderStates rsLine = depthLessState();
  rsLine.primitiveType = Primitive_LINE;
  auto psLine = r.createPipelineStates(rsLine);
  Mesh line;
  line.verts = {glm::vec4(-0.75f, -0.75f, 0, 1), glm::vec4(0.75f, 0.75f, 0, 1)};
  line.idx = {0, 1};
  auto vaoLine = line.build(r);
  runDraw(r, fbo, defaultClear(), vaoLine, prog, psLine, W, H);
  TASK_CHECK(countColor(c0, W, H, c8(fs->outColor[0])) >= 2);
  TASK_CHECK(countColor(c1, W, H, c8(fs->outColor[1])) >= 2);
  return 0;
}

int composition_cubemap_slot0() {
  RendererSoft r;
  const int W = 4, H = 4;
  auto cube = makeColor(r, W, H, false, true);
  auto depth = makeDepth(r, W, H, false);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(cube, TEXTURE_CUBE_MAP_POSITIVE_X, 0);
  fbo->setDepthAttachment(depth);
  TASK_CHECK(fbo->isColorReady(0));

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);

  // seed an unbound face with a marker so we can prove the draw never touches it
  const RGBA marker(37, 73, 109, 219);
  auto *cubeTex = dynamic_cast<TextureSoft<RGBA> *>(cube.get());
  cubeTex->getImage(1).getBuffer(0)->buffer->setAll(marker);

  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);

  // bound face received color, the other face keeps its seeded marker
  TASK_CHECK(countColor(cube, W, H, c8(fs->outColor[0])) > 0);
  int untouched = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (near(*colorPx(cube, x, y, 0, 1), marker)) {
        untouched++;
      }
    }
  }
  TASK_CHECK(untouched == W * H);
  return 0;
}

// ---------------------------------------------------------------------------
// resolve group
// ---------------------------------------------------------------------------

int resolve_four_attachments_msaa() {
  RendererSoft r;
  const int W = 8, H = 8;
  std::shared_ptr<Texture> colors[SLOTS];
  for (unsigned s = 0; s < SLOTS; s++) {
    colors[s] = makeColor(r, W, H, true);
  }
  auto depth = makeDepth(r, W, H, true);
  auto fbo = r.createFrameBuffer(true);
  for (unsigned s = 0; s < SLOTS; s++) {
    fbo->setColorAttachment(s, colors[s], 0);
  }
  fbo->setDepthAttachment(depth);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = triMesh(0.f);
  auto vao = mesh.build(r);
  runDraw(r, fbo, defaultClear(), vao, prog, ps, W, H);

  // every attachment resolved into its own single-sample buffer
  int x = -1, y = -1;
  TASK_CHECK(findPixel(colors[0], W, H, c8(fs->outColor[0]), x, y));
  for (unsigned s = 0; s < SLOTS; s++) {
    auto *t = dynamic_cast<TextureSoft<RGBA> *>(colors[s].get());
    TASK_CHECK(t->getImage().getBuffer(0)->buffer != nullptr);
    TASK_CHECK(near(*colorPx(colors[s], x, y), c8(fs->outColor[s])));
  }
  return 0;
}

int resolve_per_attachment_average() {
  RendererSoft r;
  const int W = 2, H = 2;
  auto c0 = makeColor(r, W, H, true);
  auto c1 = makeColor(r, W, H, true);
  auto depth = makeDepth(r, W, H, true);
  auto fbo = r.createFrameBuffer(true);
  fbo->setColorAttachment(0, c0, 0);
  fbo->setColorAttachment(1, c1, 0);
  fbo->setDepthAttachment(depth);

  ClearStates noColorClear;
  noColorClear.depthFlag = true;
  r.beginRenderPass(fbo, noColorClear);

  // hand-fill the multisample storage with known per-sample patterns
  auto *t0 = dynamic_cast<TextureSoft<RGBA> *>(c0.get());
  auto *t1 = dynamic_cast<TextureSoft<RGBA> *>(c1.get());
  auto buf0 = t0->getImage().getBuffer(0);
  auto buf1 = t1->getImage().getBuffer(0);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      buf0->bufferMs4x->set(x, y, glm::tvec4<RGBA>(RGBA(0, 0, 0, 255), RGBA(255, 255, 255, 255),
                                                   RGBA(0, 0, 0, 255), RGBA(255, 255, 255, 255)));
      buf1->bufferMs4x->set(x, y, glm::tvec4<RGBA>(RGBA(0, 0, 0, 255), RGBA(0, 0, 0, 255),
                                                   RGBA(0, 0, 0, 255), RGBA(255, 255, 255, 255)));
    }
  }

  // geometry fully outside the frustum: no color is rasterized, resolve still runs
  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = offscreenMesh();
  auto vao = mesh.build(r);
  r.setViewPort(0, 0, W, H);
  r.setVertexArrayObject(vao);
  r.setShaderProgram(prog);
  r.setPipelineStates(ps);
  r.draw();

  // slot 0: mean of {0,255,0,255} = 127.5 -> 127 ; slot 1: mean of {0,0,0,255} = 63.75 -> 63
  TASK_CHECK(near(*colorPx(c0, 0, 0), RGBA(127, 127, 127, 255)));
  TASK_CHECK(near(*colorPx(c1, 0, 0), RGBA(63, 63, 63, 255)));
  TASK_CHECK(near(*colorPx(c1, W - 1, H - 1), RGBA(63, 63, 63, 255)));
  return 0;
}

int resolve_single_sample_control() {
  RendererSoft r;
  const int W = 8, H = 8;

  // single target reference
  auto single = makeColor(r, W, H, false);
  auto depthSingle = makeDepth(r, W, H, false);
  auto fboSingle = r.createFrameBuffer(true);
  fboSingle->setColorAttachment(0, single, 0);
  fboSingle->setDepthAttachment(depthSingle);

  // four target run, all single sample; the four-sample path is covered above
  std::shared_ptr<Texture> mtr[SLOTS];
  for (unsigned s = 0; s < SLOTS; s++) {
    mtr[s] = makeColor(r, W, H, false);
  }
  auto depthMtr = makeDepth(r, W, H, false);
  auto fboMtr = r.createFrameBuffer(true);
  for (unsigned s = 0; s < SLOTS; s++) {
    fboMtr->setColorAttachment(s, mtr[s], 0);
  }
  fboMtr->setDepthAttachment(depthMtr);

  auto fs = std::make_shared<GFS>();
  auto prog = makeProgram(r, fs);
  auto ps = r.createPipelineStates(depthLessState());
  auto mesh = triMesh(0.f);

  auto vaoA = mesh.build(r);
  runDraw(r, fboSingle, defaultClear(), vaoA, prog, ps, W, H);
  auto vaoB = mesh.build(r);
  runDraw(r, fboMtr, defaultClear(), vaoB, prog, ps, W, H);

  // slot 0 of the MTR framebuffer matches the legacy single target
  int diffs = 0;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (!near(*colorPx(single, x, y), *colorPx(mtr[0], x, y))) {
        diffs++;
      }
    }
  }
  TASK_CHECK(diffs == 0);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"routing", "sparse_slot2_only", routing_sparse_slot2_only},
    {"routing", "four_slots", routing_four_slots},
    {"routing", "output0_alias", routing_output0_alias},
    {"routing", "fragment_call_count", routing_fragment_call_count},
    {"routing", "less_all_attachments", routing_less_all_attachments},
    {"routing", "rebind_refresh", routing_rebind_refresh},
    {"routing", "mip_level_size", routing_mip_level_size},
    {"routing", "invalid_throws", routing_invalid_throws},

    {"composition", "per_attachment_blend", composition_per_attachment_blend},
    {"composition", "write_mask_and_depth", composition_write_mask_and_depth},
    {"composition", "all_masks_depth_only", composition_all_masks_depth_only},
    {"composition", "discard_occlusion", composition_discard_occlusion},
    {"composition", "early_z_both_modes", composition_early_z_both_modes},
    {"composition", "points_and_lines", composition_points_and_lines},
    {"composition", "cubemap_slot0", composition_cubemap_slot0},

    {"resolve", "four_attachments_msaa", resolve_four_attachments_msaa},
    {"resolve", "per_attachment_average", resolve_per_attachment_average},
    {"resolve", "single_sample_control", resolve_single_sample_control},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
