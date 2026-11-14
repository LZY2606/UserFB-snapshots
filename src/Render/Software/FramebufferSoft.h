/*
 * SoftGLRender
 * @author 	: keith@robot9.me
 *
 */

#pragma once

#include <stdexcept>
#include "Base/UUID.h"
#include "Render/Framebuffer.h"
#include "Render/Software/TextureSoft.h"

namespace SoftGL {

class FrameBufferSoft : public FrameBuffer {
 public:
  explicit FrameBufferSoft(bool offscreen) : FrameBuffer(offscreen) {}

  int getId() const override {
    return uuid_.get();
  }

  bool isValid() override {
    for (unsigned i = 0; i < COLOR_ATTACHMENT_COUNT; i++) {
      if (colorReady_[i]) {
        return true;
      }
    }
    return depthReady_;
  }

  std::shared_ptr<ImageBufferSoft<RGBA>> getColorBuffer(unsigned slot) const {
    if (!isColorReady(slot)) {
      return nullptr;
    }
    auto &attachment = colorAttachments_[slot];
    auto *colorTex = dynamic_cast<TextureSoft<RGBA> *>(attachment.tex.get());
    if (!colorTex) {
      return nullptr;
    }
    return colorTex->getImage(attachment.layer).getBuffer(attachment.level);
  };

  // old single color attachment interface, returns slot 0
  std::shared_ptr<ImageBufferSoft<RGBA>> getColorBuffer() const {
    return getColorBuffer(0);
  };

  std::shared_ptr<ImageBufferSoft<float>> getDepthBuffer() const {
    if (!depthReady_) {
      return nullptr;
    }
    auto *depthTex = dynamic_cast<TextureSoft<float> *>(depthAttachment_.tex.get());
    if (!depthTex) {
      return nullptr;
    }
    return depthTex->getImage(depthAttachment_.layer).getBuffer(depthAttachment_.level);
  };

  /**
   * Validate the framebuffer before a clear or a draw.
   *
   * Every bound color/depth attachment must be allocated; all of them must
   * share the same dimensions (computed from the bound mip level) and the
   * same sample count; the same image may not be attached twice.
   * Throws std::invalid_argument on the first violation.
   */
  void validate() {
    int refWidth = -1;
    int refHeight = -1;
    int refSamples = -1;

    auto checkImage = [&](Texture *tex, uint32_t level, const char *what) {
      if (!tex) {
        throw std::invalid_argument(std::string("framebuffer attachment is null: ") + what);
      }
      int w = (int) tex->getLevelWidth(level);
      int h = (int) tex->getLevelHeight(level);
      int samples = tex->multiSample ? SOFT_MS_CNT : 1;
      if (refWidth < 0) {
        refWidth = w;
        refHeight = h;
        refSamples = samples;
      } else if (w != refWidth || h != refHeight || samples != refSamples) {
        throw std::invalid_argument(
            std::string("framebuffer attachment size/sample mismatch: ") + what);
      }
    };

    // every color attachment needs an allocated image and a usable buffer
    for (unsigned slot = 0; slot < COLOR_ATTACHMENT_COUNT; slot++) {
      if (!colorReady_[slot]) {
        continue;
      }
      auto &attachment = colorAttachments_[slot];
      auto *colorTex = dynamic_cast<TextureSoft<RGBA> *>(attachment.tex.get());
      if (!colorTex) {
        throw std::invalid_argument("color attachment is not an RGBA8 texture");
      }
      auto &image = colorTex->getImage(attachment.layer);
      if (image.empty() || attachment.level >= image.levels.size() || !image.getBuffer(attachment.level)) {
        throw std::invalid_argument("color attachment image is not allocated");
      }
      checkImage(attachment.tex.get(), attachment.level, "color");
    }

    if (depthReady_) {
      auto *depthTex = dynamic_cast<TextureSoft<float> *>(depthAttachment_.tex.get());
      if (!depthTex) {
        throw std::invalid_argument("depth attachment is not a FLOAT32 texture");
      }
      auto &image = depthTex->getImage(depthAttachment_.layer);
      if (image.empty() || depthAttachment_.level >= image.levels.size()
          || !image.getBuffer(depthAttachment_.level)) {
        throw std::invalid_argument("depth attachment image is not allocated");
      }
      checkImage(depthAttachment_.tex.get(), depthAttachment_.level, "depth");
    }

    if (refWidth < 0) {
      throw std::invalid_argument("framebuffer has no attachments");
    }

    // the same image (texture + layer + level) must not be attached twice
    for (unsigned i = 0; i < COLOR_ATTACHMENT_COUNT; i++) {
      if (!colorReady_[i]) {
        continue;
      }
      auto &a = colorAttachments_[i];
      for (unsigned j = i + 1; j < COLOR_ATTACHMENT_COUNT; j++) {
        if (!colorReady_[j]) {
          continue;
        }
        auto &b = colorAttachments_[j];
        if (a.tex.get() == b.tex.get() && a.layer == b.layer && a.level == b.level) {
          throw std::invalid_argument("framebuffer color image attached more than once");
        }
      }
      if (depthReady_ && a.tex.get() == depthAttachment_.tex.get()) {
        throw std::invalid_argument("framebuffer image used as both color and depth");
      }
    }
  }

 private:
  UUID<FrameBufferSoft> uuid_;
};

}
