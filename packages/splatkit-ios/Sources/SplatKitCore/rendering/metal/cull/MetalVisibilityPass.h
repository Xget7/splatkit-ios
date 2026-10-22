#pragma once

#import <Metal/Metal.h>

#include <array>
#include <cstdint>
#include <vector>

#include "rendering/metal/layout/MetalDrawArguments.h"
#include "rendering/metal/layout/MetalFrameSlots.h"
#include "rendering/metal/layout/MetalShaderTypes.h"
#include "rendering/metal/layout/MetalSortKeys.h"
#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// Projects and compacts requested ranges and prepares indirect draws. The sort between the two
// belongs to the caller, which hands over the key and value buffers the projection fills.
// Owns per-frame CPU inputs and GPU scratch; never commits or waits on a command buffer.
// Call reserve only while idle. All encodes and raster consumers use one command queue.
class MetalVisibilityPass {
 public:
  // Both widths sort by linear view depth: Full32 by its float bits, Low16 quantized between
  // finite forward-Z Mat4::perspective planes. Shader key width and radix passes agree.
  bool create(id<MTLDevice> device, id<MTLLibrary> library, KeyBits depthBits = KeyBits::Full32);
  // Rebuilds the visibility pipelines for a new key width, keeping every reserved buffer
  // and the radix scratch. The key width and the shader's quantized flag must agree, so
  // both move together. Render thread, while idle. On failure the previous key width is
  // restored.
  bool reconfigure(KeyBits depthBits);
  // The key width the pipelines were built for.
  KeyBits depthBits() const { return depthBits_; }
  // Space for every source splat, or an explicitly bounded GPU index list when
  // activeCapacity is nonzero. The latter must only be used with indexed encode.
  // Failure preserves the previous allocation. The caller reserves its sort for
  // slotsFor(capacity, activeCapacity) pairs first, so the sort is never smaller than this.
  bool reserve(uint32_t capacity, uint32_t activeCapacity = 0);
  // The projections, and so the sort pairs, reserve(capacity, activeCapacity) makes room for.
  static uint32_t slotsFor(uint32_t capacity, uint32_t activeCapacity = 0);
  uint32_t capacity() const { return capacity_; }

  // Resets the slot's count, then culls and projects the ranges into `projected()`, writing one
  // uint32 depth key into `keys` and the projection's index into `values` per survivor (Low16
  // only uses the lower 16 bits of a key). Both buffers must hold at least capacity() entries,
  // or this returns false. Invalid ranges fail before encoding any work. Empty ranges produce an
  // empty draw. Caller owns uniforms/source buffers and must keep slot inputs unchanged until this
  // frame completes. Ranges must be disjoint and refer to the reserved source.
  bool encodeProject(id<MTLCommandBuffer> cmd, uint32_t slot, id<MTLBuffer> uniforms,
                     id<MTLBuffer> splats, id<MTLBuffer> sh, int shDegree,
                     const SplatRenderer::Range* ranges, uint32_t rangeCount, id<MTLBuffer> keys,
                     id<MTLBuffer> values, id<MTLBuffer> indices = nil,
                     id<MTLBuffer> activeCount = nil);
  // Turns the slot's survivor count into the indirect draw arguments. Encode it after the
  // sort of the same command buffer.
  void encodeIndirect(id<MTLCommandBuffer> cmd, uint32_t slot);

  // Every compute pipeline this pass encodes, so tests can tell whether a world load rebuilt them.
  std::vector<id<MTLComputePipelineState>> pipelines() const;

  id<MTLBuffer> projected() const { return projected_; }
  id<MTLBuffer> drawArguments(uint32_t slot) const { return drawArguments_[slot]; }
  id<MTLBuffer> countBuffer(uint32_t slot) const { return count_[slot]; }
  // Only read after the GPU has completed this slot.
  uint32_t count(uint32_t slot) const {
    return *static_cast<const uint32_t*>(count_[slot].contents);
  }

 private:
  static constexpr uint32_t kThreads = 256;
  static constexpr uint32_t kMaxRanges = 65536;
  static constexpr int kShDegrees = 4;

  bool createPipelines();

  id<MTLDevice> device_ = nil;
  id<MTLLibrary> library_ = nil;
  std::array<id<MTLComputePipelineState>, kShDegrees> visibility_{};
  std::array<id<MTLComputePipelineState>, kShDegrees> indexedVisibility_{};
  id<MTLComputePipelineState> prepareDraw_ = nil;
  KeyBits depthBits_ = KeyBits::Full32;
  uint32_t capacity_ = 0;
  uint32_t sourceCapacity_ = 0;
  bool indexedOnly_ = false;
  id<MTLBuffer> projected_ = nil;
  std::array<id<MTLBuffer>, kFramesInFlight> count_{};
  std::array<id<MTLBuffer>, kFramesInFlight> drawArguments_{};
  std::array<id<MTLBuffer>, kFramesInFlight> ranges_{};
  std::array<id<MTLBuffer>, kFramesInFlight> rangeStarts_{};
};

}  // namespace splatkit
