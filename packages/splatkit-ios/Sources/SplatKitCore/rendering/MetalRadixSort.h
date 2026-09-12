#pragma once

#import <Metal/Metal.h>

#include <array>
#include <cstdint>

namespace splatkit {

// Stable ascending sort of uint32 key/value pairs. No camera or splat knowledge.
// Call reserve while idle, fill keys()/values(), then encode on the render queue.
// The GPU-written count must not exceed capacity(). Results are in the same buffers.
// Scratch is shared across frames: all encodes and their consumers use one queue.
class MetalRadixSort {
 public:
  // Low16 requires zero upper bits. Keys remain uint32 storage in both modes.
  enum class KeyBits : uint32_t { Low16 = 16, Full32 = 32 };
  bool create(id<MTLDevice> device, id<MTLLibrary> library,
              MTLResourceOptions storage = MTLResourceStorageModeShared);
  // Transactional allocation: failure preserves the previous buffers and capacity.
  bool reserve(uint32_t capacity);
  uint32_t capacity() const { return capacity_; }
  void encode(id<MTLCommandBuffer> cmd, id<MTLBuffer> count, KeyBits bits = KeyBits::Full32);
  id<MTLBuffer> keys() const { return keys_[0]; }
  id<MTLBuffer> values() const { return values_[0]; }

  static constexpr uint32_t kThreads = 256;
  static constexpr uint32_t kBlock = kThreads * 16;

 private:
  static constexpr uint32_t kDigitBits = 8;
  static constexpr uint32_t kBins = 1u << kDigitBits;
  static_assert(16 / kDigitBits % 2 == 0 && 32 / kDigitBits % 2 == 0,
                "Both key widths must finish in the input buffers");

  id<MTLDevice> device_ = nil;
  MTLResourceOptions storage_ = MTLResourceStorageModeShared;
  id<MTLComputePipelineState> prepare_ = nil;
  id<MTLComputePipelineState> histogram_ = nil;
  id<MTLComputePipelineState> scan_ = nil;
  id<MTLComputePipelineState> scatter_ = nil;
  uint32_t capacity_ = 0;
  std::array<id<MTLBuffer>, 2> keys_{};
  std::array<id<MTLBuffer>, 2> values_{};
  id<MTLBuffer> histogramBuffer_ = nil;
  id<MTLBuffer> totals_ = nil;
  id<MTLBuffer> dispatch_ = nil;
};

}  // namespace splatkit
