#pragma once

#import <Metal/Metal.h>

#include <array>
#include <cstdint>
#include <vector>

#include "rendering/metal/layout/MetalSortKeys.h"

namespace splatkit {

// Stable ascending argsort of uint32 keys. No camera or splat knowledge.
// Call reserve while idle, fill keys() on the GPU, then encode on the render queue.
// The GPU-written count must not exceed capacity(). order() then holds the keys' indices in
// ascending key order, equal keys by index; the first pass starts from the indices themselves,
// so nobody writes or reads an identity permutation. keys() is scratch afterwards, since the
// last pass skips the key write nobody reads.
// Scratch is shared across frames: all encodes and their consumers use one queue.
class MetalRadixSort {
 public:
  bool create(id<MTLDevice> device, id<MTLLibrary> library);
  // Transactional allocation: failure preserves the previous buffers and capacity.
  bool reserve(uint32_t capacity);
  uint32_t capacity() const { return capacity_; }
  void encode(id<MTLCommandBuffer> cmd, id<MTLBuffer> count, KeyBits bits = KeyBits::Full32);
  // Every compute pipeline this sort encodes, so tests can tell whether a world load rebuilt them.
  std::vector<id<MTLComputePipelineState>> pipelines() const {
    return {prepare_, histogram_, scan_, scatterFirst_, scatter_, scatterLast_};
  }
  id<MTLBuffer> keys() const { return keys_[0]; }
  id<MTLBuffer> order() const { return values_[0]; }

  static constexpr uint32_t kThreads = 256;
  static constexpr uint32_t kBlock = kThreads * 16;

 private:
  static constexpr uint32_t kDigitBits = 8;
  static constexpr uint32_t kBins = 1u << kDigitBits;
  static_assert(16 / kDigitBits % 2 == 0 && 32 / kDigitBits % 2 == 0,
                "Both key widths must finish in the input buffers");

  id<MTLDevice> device_ = nil;
  id<MTLComputePipelineState> prepare_ = nil;
  id<MTLComputePipelineState> histogram_ = nil;
  id<MTLComputePipelineState> scan_ = nil;
  id<MTLComputePipelineState> scatterFirst_ = nil, scatter_ = nil, scatterLast_ = nil;
  uint32_t capacity_ = 0;
  std::array<id<MTLBuffer>, 2> keys_{};
  std::array<id<MTLBuffer>, 2> values_{};
  id<MTLBuffer> histogramBuffer_ = nil;
  id<MTLBuffer> totals_ = nil;
  id<MTLBuffer> dispatch_ = nil;
};

}  // namespace splatkit
