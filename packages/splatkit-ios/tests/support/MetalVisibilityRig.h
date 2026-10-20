#pragma once

#import <Metal/Metal.h>

#include "rendering/metal/cull/MetalVisibilityPass.h"
#include "rendering/metal/sort/MetalRadixSort.h"
#include "support/MetalTestContext.h"

namespace splatkit::test {

// The visibility pass with the sort in the place MetalFrameCompute puts it: the projection, the
// radix sort of the survivors, then the indirect draw arguments, in one command buffer.
struct VisibilityRig {
  MetalVisibilityPass pass;
  MetalRadixSort sort;

  bool create(MetalRadixSort::KeyBits bits = MetalRadixSort::KeyBits::Full32) {
    return pass.create(Gpu::get().device, Gpu::get().library, bits) &&
           sort.create(Gpu::get().device, Gpu::get().library);
  }
  bool reserve(uint32_t capacity, uint32_t activeCapacity = 0) {
    return sort.reserve(MetalVisibilityPass::slotsFor(capacity, activeCapacity)) &&
           pass.reserve(capacity, activeCapacity);
  }
  bool encode(id<MTLCommandBuffer> cmd, uint32_t slot, id<MTLBuffer> uniforms, id<MTLBuffer> splats,
              id<MTLBuffer> sh, int shDegree, const SplatRenderer::Range* ranges,
              uint32_t rangeCount, id<MTLBuffer> indices = nil, id<MTLBuffer> activeCount = nil) {
    if (!pass.encodeProject(cmd, slot, uniforms, splats, sh, shDegree, ranges, rangeCount,
                            sort.keys(), sort.values(), indices, activeCount))
      return false;
    sort.encode(cmd, pass.countBuffer(slot), pass.depthBits());
    pass.encodeIndirect(cmd, slot);
    return true;
  }
  uint32_t count(uint32_t slot) const { return pass.count(slot); }
  id<MTLBuffer> projected() const { return pass.projected(); }
  id<MTLBuffer> drawArguments(uint32_t slot) const { return pass.drawArguments(slot); }
  // The sorted indices into projected().
  id<MTLBuffer> order() const { return sort.values(); }
  // The depth keys as the cull wrote them, which the sort leaves in place.
  id<MTLBuffer> depthKeys() const { return sort.keys(); }
};

}  // namespace splatkit::test
