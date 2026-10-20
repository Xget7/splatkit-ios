#pragma once

#import <Metal/Metal.h>

#include <cstdint>

namespace splatkit {

// The offscreen colour texture a render scale or half-float compositing draws into, and the
// depth buffer that goes with whatever colour is drawn. Render thread only.
class MetalRenderTarget {
 public:
  static constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth16Unorm;

  explicit MetalRenderTarget(id<MTLDevice> device) : device_(device) {}

  MetalRenderTarget(const MetalRenderTarget&) = delete;
  MetalRenderTarget& operator=(const MetalRenderTarget&) = delete;

  // Replaces color() with a texture of `renderScale` times the size, at least one pixel each
  // way. A zero size leaves no texture. False when the texture could not be allocated.
  bool resize(uint32_t width, uint32_t height, float renderScale, MTLPixelFormat format);
  // Drops color(); the frame then draws straight to the drawable.
  void release();
  // The depth buffer for the colour drawn this frame (the target, or the drawable when there
  // is none), rebuilt only when the size changes. False when it could not be allocated.
  bool ensureDepth(NSUInteger width, NSUInteger height);

  // Nil when the frame draws straight to the drawable.
  id<MTLTexture> color() const { return color_; }
  // The colour a frame draws into: the target, or `drawable` when there is none.
  id<MTLTexture> colorOr(id<MTLTexture> drawable) const {
    return color_ != nil ? color_ : drawable;
  }
  id<MTLTexture> depth() const { return depth_; }
  // Pixel format of color(); invalid while there is none.
  MTLPixelFormat format() const { return format_; }
  // The scale of the last resize: color() is this fraction of the drawable.
  float renderScale() const { return renderScale_; }

 private:
  id<MTLDevice> device_ = nil;
  id<MTLTexture> color_ = nil;  // scaled rendering or half-float GPU compositing
  id<MTLTexture> depth_ = nil;  // GPU-private, the size of the colour target
  MTLPixelFormat format_ = MTLPixelFormatInvalid;
  float renderScale_ = 1.0f;
};

}  // namespace splatkit
