#pragma once

#import <Metal/Metal.h>

#include <array>
#include <cstdint>

#include "rendering/metal/layout/MetalShaderTypes.h"
#include "rendering/metal/presentation/MetalRenderTarget.h"
#include "rendering/metal/world/MetalWorld.h"

namespace splatkit {

// The render pipelines of the splat pass for one colour format and the pass that draws with
// them: one splat pipeline per spherical harmonics degree for CPU-ordered draws, and for the
// GPU order path the projected, saturation mask and background pipelines with their two depth
// states. Render thread only.
class MetalSplatPipeline {
 public:
  // The buffers the visibility pass and the sort produced for a GPU-ordered draw.
  struct GpuOrder {
    id<MTLBuffer> projected = nil;
    id<MTLBuffer> order = nil;
    // Indirect draw arguments of the frame's slot, kDrawBatches of them.
    id<MTLBuffer> arguments = nil;
  };

  // One splat pipeline per spherical harmonics degree, specialised through a function constant
  // so a degree 0 world pays nothing for SH, and the pipelines of the GPU order path, for
  // colour attachments of `format`. format() reports it even when a pipeline failed, so a
  // failed build is not retried until the format changes. False when a pipeline could not be
  // built.
  bool create(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat format);
  // False until the splat pipelines exist.
  bool ready() const { return splatPipelines_[0] != nil; }
  // The colour format of the last create(); invalid before it.
  MTLPixelFormat format() const { return format_; }

  // Encodes the pass into `cmd`: clears the colour (the target's, or `drawable` when there is
  // none) and the depth the target holds, then draws `world` with `uniforms`, or only clears
  // when there is no world or nothing to draw. `gpuOrder` draws the GPU-sorted projections in
  // batches front to back and the background last, otherwise the first `drawCount` entries of
  // the world's CPU order go back to front. `shDegree` is what the frame asks for.
  void encode(id<MTLCommandBuffer> cmd, const MetalRenderTarget& target, id<MTLTexture> drawable,
              id<MTLBuffer> uniforms, const MetalWorld* world, int shDegree, const GpuOrder& gpu,
              uint32_t drawCount, bool gpuOrder) const;

 private:
  std::array<id<MTLRenderPipelineState>, kMaxShDegree + 1> splatPipelines_{};
  // The GPU order path: front to back in batches, a saturation mask between them, the
  // background last.
  id<MTLRenderPipelineState> projectedPipeline_ = nil;
  id<MTLRenderPipelineState> maskPipeline_ = nil;
  id<MTLRenderPipelineState> backgroundPipeline_ = nil;
  id<MTLDepthStencilState> splatDepth_ = nil;  // pass unless masked, never write
  id<MTLDepthStencilState> maskDepth_ = nil;   // always write
  MTLPixelFormat format_ = MTLPixelFormatInvalid;
};

}  // namespace splatkit
