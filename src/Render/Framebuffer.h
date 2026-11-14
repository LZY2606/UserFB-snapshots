/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include <memory>
#include <stdexcept>
#include "Texture.h"

namespace SoftGL {

struct FrameBufferAttachment {
  std::shared_ptr<Texture> tex = nullptr;
  uint32_t layer = 0; // for cube map
  uint32_t level = 0;
};

class FrameBuffer {
 public:
  // maximum number of offscreen color outputs (MTR), slot 0 .. 3
  static constexpr unsigned COLOR_ATTACHMENT_COUNT = 4;

  explicit FrameBuffer(bool offscreen) : offscreen_(offscreen) {}

  virtual int getId() const = 0;
  virtual bool isValid() = 0;

  /**
   * Bind a 2D color texture to the given color slot.
   *
   * Slots 0..3 may be bound sparsely, e.g. binding only slot 2 is legal.
   * Passing a null texture (or calling with an empty shared_ptr) unbinds
   * the slot again. Slots 1..3 currently only accept 2D RGBA8 textures;
   * slot 0 additionally keeps the cube map overload below.
   */
  virtual void setColorAttachment(unsigned slot, std::shared_ptr<Texture> &color, int level) {
    if (slot >= COLOR_ATTACHMENT_COUNT) {
      throw std::invalid_argument("color attachment slot out of range");
    }
    if (color) {
      colorAttachments_[slot].tex = color;
      colorAttachments_[slot].layer = 0;
      colorAttachments_[slot].level = level;
      colorReady_[slot] = true;
    } else {
      colorAttachments_[slot] = FrameBufferAttachment{};
      colorReady_[slot] = false;
    }
  }

  // old single color attachment interface, delegates to slot 0
  virtual void setColorAttachment(std::shared_ptr<Texture> &color, int level) {
    setColorAttachment(0, color, level);
  };

  // cube map overload, retained for slot 0
  virtual void setColorAttachment(std::shared_ptr<Texture> &color, CubeMapFace face, int level) {
    colorAttachments_[0].tex = color;
    colorAttachments_[0].layer = face;
    colorAttachments_[0].level = level;
    colorReady_[0] = true;
  };

  virtual void setDepthAttachment(std::shared_ptr<Texture> &depth) {
    depthAttachment_.tex = depth;
    depthAttachment_.layer = 0;
    depthAttachment_.level = 0;
    depthReady_ = true;
  };

  inline const FrameBufferAttachment &getColorAttachment(unsigned slot) const {
    static const FrameBufferAttachment emptyAttachment{};
    if (slot >= COLOR_ATTACHMENT_COUNT) {
      return emptyAttachment;
    }
    return colorAttachments_[slot];
  }

  // old single color attachment interface, returns slot 0
  inline const FrameBufferAttachment &getColorAttachment() const {
    return getColorAttachment(0);
  }

  inline const FrameBufferAttachment &getDepthAttachment() const {
    return depthAttachment_;
  }

  inline bool isColorReady(unsigned slot) const {
    return slot < COLOR_ATTACHMENT_COUNT && colorReady_[slot];
  }

  // old single color attachment interface, reports slot 0
  inline bool isColorReady() const {
    return isColorReady(0);
  }

  inline bool isDepthReady() const {
    return depthReady_;
  }

  inline bool isMultiSample() const {
    for (unsigned i = 0; i < COLOR_ATTACHMENT_COUNT; i++) {
      if (colorReady_[i]) {
        return colorAttachments_[i].tex->multiSample;
      }
    }
    if (depthReady_) {
      return depthAttachment_.tex->multiSample;
    }

    return false;
  }

  inline bool isOffscreen() const {
    return offscreen_;
  }

  inline void setOffscreen(bool offscreen) {
    offscreen_ = offscreen;
  }

 protected:
  bool offscreen_ = false;
  bool colorReady_[COLOR_ATTACHMENT_COUNT] = {false, false, false, false};
  bool depthReady_ = false;

  // MTR: up to four sparse color outputs, slot 0 is the legacy FragColor target
  FrameBufferAttachment colorAttachments_[COLOR_ATTACHMENT_COUNT]{};
  FrameBufferAttachment depthAttachment_{};
};

}
