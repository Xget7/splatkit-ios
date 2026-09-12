#pragma once

#import <Metal/Metal.h>

#include <array>
#include <cstdint>

#include "rendering/MetalRadixSort.h"
#include "rendering/MetalShaderTypes.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// Projects and compacts requested ranges, sorts survivors, prepares indirect draws.
// Owns per-frame CPU inputs and GPU scratch; never commits or waits on a command buffer.
// Call reserve only while idle. All encodes and raster consumers use one command queue.
class MetalVisibility {
 public:
  // Internal experiment only; the default preserves the existing renderer.
  // Low16 uses linear view depth and finite forward-Z Mat4::perspective planes,
  // independently of the culling option. Shader key width and radix passes agree.
  bool create(id<MTLDevice> device, id<MTLLibrary> library, bool experiment = false,
              float minPixelRadius = 0.5f,
              MetalRadixSort::KeyBits depthBits = MetalRadixSort::KeyBits::Full32);
  // Space for every source splat, or an explicitly bounded GPU index list when
  // activeCapacity is nonzero. The latter must only be used with indexed encode.
  // Failure preserves the previous allocation.
  bool reserve(uint32_t capacity, uint32_t activeCapacity = 0);
  uint32_t capacity() const { return capacity_; }

  // Invalid ranges fail before encoding any work. Empty ranges produce an empty draw.
  // Caller owns uniforms/source buffers and must keep slot inputs unchanged until
  // this frame completes. Ranges must be disjoint and refer to the reserved source.
  bool encode(id<MTLCommandBuffer> cmd, uint32_t slot, id<MTLBuffer> uniforms, id<MTLBuffer> splats,
              id<MTLBuffer> sh, int shDegree, const SplatRenderer::Range* ranges,
              uint32_t rangeCount, id<MTLBuffer> indices = nil, id<MTLBuffer> activeCount = nil);

  id<MTLBuffer> order() const { return sort_.values(); }
  // GPU-owned uint32 slots; Low16 only uses the lower 16 bits.
  id<MTLBuffer> depthKeys() const { return sort_.keys(); }
  id<MTLBuffer> projected() const { return projected_; }
  id<MTLBuffer> drawArguments(uint32_t slot) const { return drawArguments_[slot]; }
  // A four-byte statistics readback, not the GPU counter in experimental mode.
  id<MTLBuffer> countBuffer(uint32_t slot) const { return countReadback_[slot]; }
  // Only read after the GPU has completed this slot.
  uint32_t count(uint32_t slot) const {
    return *static_cast<const uint32_t*>(countReadback_[slot].contents);
  }

  static constexpr uint32_t kSlots = 2;
  static constexpr uint32_t kDrawBatches = 7;
  static constexpr uint32_t kDrawArgumentBytes = sizeof(MTLDrawPrimitivesIndirectArguments);
  // The final record describes the whole visible set (baseInstance = 0).
  // Raster currently consumes the seven partitions to retain saturation masking.
  static constexpr uint32_t kFullDrawOffset = kDrawBatches * kDrawArgumentBytes;

 private:
  static constexpr uint32_t kThreads = 256;
  static constexpr uint32_t kMaxRanges = 65536;
  static constexpr int kShDegrees = 4;

  id<MTLDevice> device_ = nil;
  MTLResourceOptions storage_ = MTLResourceStorageModeShared;
  std::array<id<MTLComputePipelineState>, kShDegrees> visibility_{};
  std::array<id<MTLComputePipelineState>, kShDegrees> indexedVisibility_{};
  id<MTLComputePipelineState> prepareDraw_ = nil;
  MetalRadixSort sort_;
  MetalRadixSort::KeyBits depthBits_ = MetalRadixSort::KeyBits::Full32;
  uint32_t capacity_ = 0;
  uint32_t sourceCapacity_ = 0;
  bool indexedOnly_ = false;
  id<MTLBuffer> projected_ = nil;
  std::array<id<MTLBuffer>, kSlots> count_{};
  std::array<id<MTLBuffer>, kSlots> countReadback_{};
  std::array<id<MTLBuffer>, kSlots> drawArguments_{};
  std::array<id<MTLBuffer>, kSlots> ranges_{};
  std::array<id<MTLBuffer>, kSlots> rangeStarts_{};
};

}  // namespace splatkit
