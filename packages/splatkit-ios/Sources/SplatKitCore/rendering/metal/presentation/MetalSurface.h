#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdint>

#include "rendering/metal/presentation/MetalFrameLoop.h"
#include "rendering/metal/presentation/MetalRenderTarget.h"
#include "rendering/metal/presentation/MetalUpscaler.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// The CAMetalLayer a renderer presents to and everything that follows its pixel size: the
// render scale and the colour space the frame is blended in, the offscreen target they need
// and the upscaler that brings it to the drawable. The view attaches the layer and sets its
// size; each setter that changes what the next frame is drawn into bumps generation() and
// rebuilds the target and the upscaler. The frame loop must outlive the surface. Render
// thread only.
class MetalSurface {
 public:
  static constexpr float kMinRenderScale = 0.1f;
  static constexpr float kMaxRenderScale = 2.0f;

  // `compositesInHalfFloat`: the splats are accumulated in a half-float target at every scale,
  // which the GPU order path needs because 8 bits would round away the coverage.
  MetalSurface(id<MTLDevice> device, id<MTLLibrary> library, MetalFrameLoop& frameLoop,
               bool compositesInHalfFloat);

  MetalSurface(const MetalSurface&) = delete;
  MetalSurface& operator=(const MetalSurface&) = delete;

  // The layer to present to, or nil when the view is going away. Sets its device and pixel
  // format. False when the blit pipeline for that format could not be built.
  bool setLayer(CAMetalLayer* layer);
  // The layer's size in pixels.
  void setDrawableSize(uint32_t width, uint32_t height);
  // Clamped to [kMinRenderScale, kMaxRenderScale].
  void setRenderScale(float scale);
  void setSpatialUpscaling(bool enabled);
  // False when the blit pipeline for the new format could not be built.
  bool setLinearBlending(bool linear);
  void setVsync(bool vsync);

  // Nil when none is available. Capture needs the drawable readable before it is acquired.
  id<CAMetalDrawable> nextDrawable();
  void allowReadback(bool allowed);
  // The depth buffer for `width` by `height`, the size of the colour the frame draws. False
  // when it could not be allocated.
  bool ensureDepth(NSUInteger width, NSUInteger height);
  // Writes the frame to `drawable` through the upscaler or the blit; nothing when the frame
  // was drawn to the drawable itself.
  void encodeOutput(id<MTLCommandBuffer> cmd, id<MTLTexture> drawable);

  CAMetalLayer* layer() const { return layer_; }
  bool ready() const { return layer_ != nil && width_ > 0 && height_ > 0; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  // The size frames are drawn at: the scaled target, or the drawable.
  Extent drawExtent() const;
  float renderScale() const { return renderScale_; }
  bool spatialUpscaling() const { return spatialUpscaling_; }
  bool linearBlending() const { return linearBlending_; }
  // Changes whenever the layer, its size or what frames are drawn into does.
  uint32_t generation() const { return generation_; }

  // True while frames go through MetalFX.
  bool upscalerActive() const { return upscaler_.active(); }
  // Where the splats are drawn: the drawable's format, or half floats when the GPU order path
  // accumulates coverage front to back.
  MTLPixelFormat targetFormat() const;
  const MetalRenderTarget& target() const { return target_; }

 private:
  // The drawable's format.
  MTLPixelFormat pixelFormat() const;
  // Rebuilds the target and the upscaler for the current size, scale and settings.
  bool rebuild();

  id<MTLDevice> device_ = nil;
  MetalFrameLoop& frameLoop_;
  const bool compositesInHalfFloat_;
  MetalRenderTarget target_;
  MetalUpscaler upscaler_;
  CAMetalLayer* layer_ = nil;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  float renderScale_ = 1.0f;
  bool spatialUpscaling_ = true;
  bool linearBlending_ = false;
  uint32_t generation_ = 0;
};

}  // namespace splatkit
