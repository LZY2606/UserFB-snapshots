/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include "Base/GLMInc.h"

namespace SoftGL {

enum DepthFunction {
  DepthFunc_NEVER,
  DepthFunc_LESS,
  DepthFunc_EQUAL,
  DepthFunc_LEQUAL,
  DepthFunc_GREATER,
  DepthFunc_NOTEQUAL,
  DepthFunc_GEQUAL,
  DepthFunc_ALWAYS,
};

enum BlendFactor {
  BlendFactor_ZERO,
  BlendFactor_ONE,
  BlendFactor_SRC_COLOR,
  BlendFactor_SRC_ALPHA,
  BlendFactor_DST_COLOR,
  BlendFactor_DST_ALPHA,
  BlendFactor_ONE_MINUS_SRC_COLOR,
  BlendFactor_ONE_MINUS_SRC_ALPHA,
  BlendFactor_ONE_MINUS_DST_COLOR,
  BlendFactor_ONE_MINUS_DST_ALPHA,
};

enum BlendFunction {
  BlendFunc_ADD,
  BlendFunc_SUBTRACT,
  BlendFunc_REVERSE_SUBTRACT,
  BlendFunc_MIN,
  BlendFunc_MAX,
};

enum PolygonMode {
  PolygonMode_POINT,
  PolygonMode_LINE,
  PolygonMode_FILL,
};

struct BlendParameters {
  BlendFunction blendFuncRgb = BlendFunc_ADD;
  BlendFactor blendSrcRgb = BlendFactor_ONE;
  BlendFactor blendDstRgb = BlendFactor_ZERO;

  BlendFunction blendFuncAlpha = BlendFunc_ADD;
  BlendFactor blendSrcAlpha = BlendFactor_ONE;
  BlendFactor blendDstAlpha = BlendFactor_ZERO;

  void SetBlendFactor(BlendFactor src, BlendFactor dst) {
    blendSrcRgb = src;
    blendSrcAlpha = src;
    blendDstRgb = dst;
    blendDstAlpha = dst;
  }

  void SetBlendFunc(BlendFunction func) {
    blendFuncRgb = func;
    blendFuncAlpha = func;
  }
};

enum PrimitiveType {
  Primitive_POINT,
  Primitive_LINE,
  Primitive_TRIANGLE,
};

// four bit RGBA color write mask, OR-ed values
enum ColorWriteMask {
  ColorWrite_R = 1 << 0,
  ColorWrite_G = 1 << 1,
  ColorWrite_B = 1 << 2,
  ColorWrite_A = 1 << 3,
  ColorWrite_RGBA = ColorWrite_R | ColorWrite_G | ColorWrite_B | ColorWrite_A,
};

/**
 * Per color-attachment overrides.
 *
 * By default blendOverride is false: the attachment inherits the global
 * RenderStates::blend / blendParams and every RGBA channel is writable.
 * Set blendOverride = true to make this attachment use its own blend switch
 * and BlendParameters, and/or restrict colorWriteMask to a subset of channels.
 * The write mask only affects color: a fully masked attachment still takes
 * part in the shared depth test and depth write.
 */
struct ColorAttachmentState {
  bool blendOverride = false;
  bool blend = false;
  BlendParameters blendParams;

  // bitmask of ColorWriteMask, ColorWrite_RGBA allows every channel
  uint8_t colorWriteMask = ColorWrite_RGBA;
};

struct RenderStates {
  bool blend = false;
  BlendParameters blendParams;

  bool depthTest = false;
  bool depthMask = true;
  DepthFunction depthFunc = DepthFunc_LESS;

  bool cullFace = false;
  PrimitiveType primitiveType = Primitive_TRIANGLE;
  PolygonMode polygonMode = PolygonMode_FILL;

  float lineWidth = 1.f;

  // per-attachment blend override and RGBA write mask, index = color slot
  ColorAttachmentState colorAttachments[4];

  /**
   * Make color slot use its own blending configuration instead of the global
   * one. Unconfigured slots keep inheriting RenderStates::blend/blendParams.
   */
  inline void setAttachmentBlend(unsigned slot, bool enable, const BlendParameters &params) {
    if (slot >= 4) {
      return;
    }
    colorAttachments[slot].blendOverride = true;
    colorAttachments[slot].blend = enable;
    colorAttachments[slot].blendParams = params;
  }

  /**
   * Restrict which RGBA channels a color slot may write, e.g.
   * setAttachmentWriteMask(1, ColorWrite_R | ColorWrite_A). A mask of 0
   * disables all color writes for that slot while depth keeps updating.
   */
  inline void setAttachmentWriteMask(unsigned slot, uint8_t mask) {
    if (slot >= 4) {
      return;
    }
    colorAttachments[slot].colorWriteMask = mask;
  }

  inline const ColorAttachmentState &getAttachmentState(unsigned slot) const {
    return colorAttachments[slot < 4 ? slot : 0];
  }
};

struct ClearStates {
  bool depthFlag = false;
  bool colorFlag = false;
  glm::vec4 clearColor = glm::vec4(0.f);
  float clearDepth = 1.f;
};

}
