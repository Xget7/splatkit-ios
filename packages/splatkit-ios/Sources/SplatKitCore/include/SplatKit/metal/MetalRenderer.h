#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "splatkit/rendering/SceneBackdrop.h"
#include "splatkit/rendering/SplatRenderer.h"

#ifdef __OBJC__
@class CAMetalLayer;
#else
class CAMetalLayer;
#endif

namespace splatkit {

// Render-thread API. Transfer ownership to SplatEngine and keep a borrowed pointer
// for surface operations only while that engine lives.
class MetalRenderer : public SplatRenderer, public SceneBackdropRenderer {
 public:
  // Null when no usable Metal device can be initialized.
  static std::unique_ptr<MetalRenderer> create();
  // Retains the layer and sets its device and pixel format; nullptr detaches it.
  virtual void setLayer(CAMetalLayer* layer) = 0;
  virtual void setDrawableSize(uint32_t width, uint32_t height) = 0;
  // BGRA rows top down; empty pixels and zero dimensions on GPU failure.
  // Completion runs on the GPU completion thread. A new request replaces a pending one.
  using CaptureHandler = std::function<void(std::vector<uint8_t>, uint32_t, uint32_t)>;
  virtual void captureNextFrame(CaptureHandler handler) = 0;
};

}  // namespace splatkit
