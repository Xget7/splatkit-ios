#pragma once

#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include "splat/lod/LodTree.h"

namespace splatkit {

// Interior-only traversal -> bounded GPU cut. V1 metadata is built once at upload;
// v2 metadata is precomputed offline. No CPU traversal/count readback per frame.
// Create/upload while idle; encode and its consumers use the same command queue.
class MetalLOD {
 public:
  // Safety bound on the selected cut; buffers scale with the budget actually requested.
  static constexpr uint32_t kMaxBudget = 4'000'000;

  bool create(id<MTLDevice> device, id<MTLLibrary> library);
  bool upload(id<MTLCommandQueue> queue, const splat::LodTree& tree, uint32_t budget,
              float pixelLimit = 1.0f, float colorWeight = 4.0f, bool frustumCull = true);
  void encode(id<MTLCommandBuffer> command, id<MTLBuffer> uniforms);
  id<MTLBuffer> indices() const { return indices_; }
  // State words 4/5 contain denied refinements/evaluated interior nodes.
  id<MTLBuffer> count() const { return state_; }
  // Selection buffers are sized for this; the per-frame limit never exceeds it.
  uint32_t budget() const { return capacity_; }
  uint32_t limit() const { return config_.budget; }
  // Most splats one selection may emit, 0 for the full capacity. Read at each encode.
  void setSplatLimit(uint32_t splats) {
    config_.budget = splats == 0 ? capacity_ : std::min(splats, capacity_);
  }
  // Screen-space error a node may cover before it refines. Read at each encode, so it
  // applies from the next frame; callers change it between frames only.
  void setPixelLimit(float pixels) { config_.pixelLimit = pixels; }

 private:
  struct Config {
    uint32_t budget;
    float pixelLimit;
    float colorWeight;
    uint32_t cull;
  } config_{};
  uint32_t capacity_ = 0;
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
