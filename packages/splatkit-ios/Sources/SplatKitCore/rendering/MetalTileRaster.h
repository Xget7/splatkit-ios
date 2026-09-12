#pragma once

#import <Metal/Metal.h>
#include <array>
#include <cstdint>

namespace splatkit {

// Bounded hybrid compositor: exact lists of at most 512 candidates per screen tile.
// Dense/unsafe tiles are transparent sentinels, completed by hardware rasterization.
// The caller MUST load the result and mask completed tiles before the hardware draw.
class MetalTileRaster {
 public:
  bool create(id<MTLDevice> device, id<MTLLibrary> library);
  // Returns false before encoding if scratch cannot fit (128 MiB ceiling).
  // The caller can then draw this frame with the existing hardware rasterizer.
  bool encode(id<MTLCommandBuffer> cmd, id<MTLBuffer> uniforms, id<MTLBuffer> projected,
              id<MTLBuffer> order, id<MTLBuffer> count, uint32_t capacity, id<MTLTexture> target,
              uint32_t slot = 0);
  // [compute tiles, hardware tiles, invalid input, nonempty compute tiles].
  // Read after slot completion. Compute tiles include background-only tiles.
  id<MTLBuffer> diagnostics(uint32_t slot) const { return diagnostics_[slot]; }

 private:
  id<MTLDevice> device_ = nil;
  id<MTLComputePipelineState> bin_ = nil;
  id<MTLComputePipelineState> scanRows_ = nil;
  id<MTLComputePipelineState> prepareFallback_ = nil;
  id<MTLComputePipelineState> raster_ = nil;
  id<MTLComputePipelineState> summarize_ = nil;
  std::array<id<MTLBuffer>, 2> diagnostics_{};
  id<MTLBuffer> bins_ = nil;
  id<MTLBuffer> counts_ = nil;
  id<MTLBuffer> fallback_ = nil;
  id<MTLBuffer> rectangles_ = nil;
};

}  // namespace splatkit
