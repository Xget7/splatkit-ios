#include "rendering/metal/presentation/MetalSurface.h"

#include <algorithm>

#include "splatkit/Log.h"

namespace splatkit {

MetalSurface::MetalSurface(id<MTLDevice> device, id<MTLLibrary> library, MetalFrameLoop& frameLoop,
                           bool compositesInHalfFloat)
    : device_(device),
      frameLoop_(frameLoop),
      compositesInHalfFloat_(compositesInHalfFloat),
      target_(device),
      upscaler_(device, library) {}

bool MetalSurface::setLayer(CAMetalLayer* layer) {
  if (layer == layer_) return true;
  frameLoop_.waitIdle();
  layer_ = layer;
  if (layer_ == nil) return true;
  layer_.device = device_;
  layer_.pixelFormat = pixelFormat();
  layer_.framebufferOnly = YES;
  ++generation_;
  return upscaler_.setOutputFormat(pixelFormat());
}

void MetalSurface::setDrawableSize(uint32_t width, uint32_t height) {
  if (width == width_ && height == height_) return;
  LOGI("drawable %ux%u, was %ux%u", width, height, width_, height_);
  width_ = width;
  height_ = height;
  if (upscaler_.active()) frameLoop_.waitIdle();
  ++generation_;
  rebuild();
}

void MetalSurface::setRenderScale(float scale) {
  scale = std::clamp(scale, kMinRenderScale, kMaxRenderScale);
  if (scale == renderScale_) return;
  renderScale_ = scale;
  if (upscaler_.active()) frameLoop_.waitIdle();
  ++generation_;
  rebuild();
}

void MetalSurface::setSpatialUpscaling(bool enabled) {
  if (enabled == spatialUpscaling_) return;
  spatialUpscaling_ = enabled;
  // The scaler is not retained by the command buffers that encode it, so it must not be
  // replaced while one is in flight.
  if (upscaler_.active()) frameLoop_.waitIdle();
  ++generation_;
  rebuild();
}

bool MetalSurface::setLinearBlending(bool linear) {
  if (linear == linearBlending_) return true;
  linearBlending_ = linear;
  frameLoop_.waitIdle();
  if (layer_ != nil) layer_.pixelFormat = pixelFormat();
  ++generation_;
  const bool blitReady = upscaler_.setOutputFormat(pixelFormat());
  rebuild();
  return blitReady;
}

void MetalSurface::setVsync(bool vsync) {
  // Presentation is tied to the display link that drives the frames; a benchmark
  // without vsync would need its own loop. Host frame intervals and GPU times differ.
  (void)vsync;
}

id<CAMetalDrawable> MetalSurface::nextDrawable() {
  return [layer_ nextDrawable];
}

void MetalSurface::allowReadback(bool allowed) {
  layer_.framebufferOnly = allowed ? NO : YES;
}

bool MetalSurface::ensureDepth(NSUInteger width, NSUInteger height) {
  return target_.ensureDepth(width, height);
}

void MetalSurface::encodeOutput(id<MTLCommandBuffer> cmd, id<MTLTexture> drawable) {
  upscaler_.encode(cmd, target_.color(), drawable);
}

Extent MetalSurface::drawExtent() const {
  if (id<MTLTexture> color = target_.color()) {
    return {static_cast<uint32_t>(color.width), static_cast<uint32_t>(color.height)};
  }
  return {width_, height_};
}

MTLPixelFormat MetalSurface::pixelFormat() const {
  return linearBlending_ ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
}

MTLPixelFormat MetalSurface::targetFormat() const {
  return compositesInHalfFloat_ ? MTLPixelFormatRGBA16Float : pixelFormat();
}

bool MetalSurface::rebuild() {
  upscaler_.reset();  // before the target is reallocated, so both are never resident
  bool built = true;
  if ((renderScale_ == 1.0f && !compositesInHalfFloat_) || width_ == 0 || height_ == 0) {
    target_.release();
  } else {
    built = target_.resize(width_, height_, renderScale_, targetFormat());
  }
  const bool configured = upscaler_.configure(target_, width_, height_, spatialUpscaling_);
  return built && configured;
}

}  // namespace splatkit
