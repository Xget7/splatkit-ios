#pragma once

#import <Metal/Metal.h>
#include <array>
#include "splatkit/rendering/SceneBackdrop.h"

namespace splatkit {

// What shows where the capture has no splats. A capture holds nothing past what
// was scanned, so the frame's own colours, spread outward and blurred, stand in for the
// surroundings instead of a flat background. Render thread only.
class MetalBackdrop {
 public:
  // The pipelines for scenes of `sceneFormat`. format() reports it even when a pipeline failed,
  // so a failed build is not retried until the format changes. False when a pipeline could not
  // be built.
  bool create(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat sceneFormat);
  MTLPixelFormat format() const { return format_; }

  // Release successful allocations when disabled, preserving failed-size and pipeline latches.
  void releaseTextures() {
    if (pyramid_ != nil) sceneWidth_ = sceneHeight_ = 0;
    pyramid_ = backdrop_ = scratch_ = nil;
  }
  // Prepare before changing the splat pass, so failures retain the original fixed background.
  bool prepare(id<MTLDevice> device, id<MTLLibrary> library, id<MTLTexture> scene);
  // Composite under premultiplied scene coverage in the same command buffer as the splats.
  bool encode(id<MTLCommandBuffer> cmd, id<MTLTexture> scene, const SceneBackdrop& settings,
              const std::array<float, 3>& background);

 private:
  // Sized from the scene, reallocated only when its size changes.
  bool ensureTextures(NSUInteger width, NSUInteger height);

  id<MTLDevice> device_ = nil;
  id<MTLRenderPipelineState> reduce_ = nil;
  id<MTLRenderPipelineState> push_ = nil;
  id<MTLRenderPipelineState> blur_ = nil;
  id<MTLRenderPipelineState> under_ = nil;
  // The scene averaged down a power-of-two mip chain, the filled backdrop and the blur's
  // intermediate.
  id<MTLTexture> pyramid_ = nil;
  id<MTLTexture> backdrop_ = nil;
  id<MTLTexture> scratch_ = nil;
  NSUInteger sceneWidth_ = 0;
  NSUInteger sceneHeight_ = 0;
  MTLPixelFormat format_ = MTLPixelFormatInvalid;
};

}  // namespace splatkit
