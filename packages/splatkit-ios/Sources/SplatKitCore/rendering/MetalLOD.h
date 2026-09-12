#pragma once

#import <Metal/Metal.h>
#include <array>
#include "splat/lod/LodTree.h"

namespace splatkit {

// Interior-only traversal -> bounded GPU cut. V1 metadata is built once at upload;
// v2 metadata is precomputed offline. No CPU traversal/count readback per frame.
// Create/upload while idle; encode and its consumers use the same command queue.
class MetalLOD {
 public:
  bool create(id<MTLDevice> device, id<MTLLibrary> library);
  bool upload(id<MTLCommandQueue> queue, const splat::LodTree& tree, uint32_t budget,
              float pixelLimit = 1.0f, float colorWeight = 4.0f, bool frustumCull = true);
  void encode(id<MTLCommandBuffer> command, id<MTLBuffer> uniforms);
  id<MTLBuffer> indices() const { return indices_; }
  // State words 4/5 contain denied refinements/evaluated interior nodes.
  id<MTLBuffer> count() const { return state_; }
  uint32_t budget() const { return config_.budget; }

 private:
  struct Config {
    uint32_t budget;
    float pixelLimit;
    float colorWeight;
    uint32_t cull;
  } config_{};
  id<MTLDevice> device_ = nil;
  id<MTLComputePipelineState> initialize_ = nil, evaluate_ = nil, budget_ = nil;
  id<MTLComputePipelineState> compact_ = nil, allocate_ = nil, scatter_ = nil;
  id<MTLComputePipelineState> advance_ = nil, emit_ = nil;
  id<MTLComputePipelineState> scanGroups_ = nil, scanBlocks_ = nil;
  id<MTLBuffer> nodes_ = nil, leaves_ = nil, indices_ = nil, packets_ = nil;
  id<MTLBuffer> costs_ = nil, costGroups_ = nil, offsets_ = nil, groups_ = nil, state_ = nil;
  id<MTLBuffer> blocks_ = nil;
  std::array<id<MTLBuffer>, 2> frontier_{};
  uint32_t depth_ = 0;
};
}  // namespace splatkit
