#pragma once

#import <Metal/Metal.h>

#include <array>
#include <memory>

#include "splatkit/rendering/SplatRenderer.h"

namespace splatkit {

// GPU residency of one world, independent of its camera and rendering pipelines.
// Static worlds upload once into private memory; slabs accept incremental tile writes.
// The renderer guarantees no frame reads a slab range or order slot while it is written.
class MetalWorld {
 public:
  static std::unique_ptr<MetalWorld> upload(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                            const splat::SplatCloud& cloud, int maxShDegree);
  static std::unique_ptr<MetalWorld> slab(id<MTLDevice> device, uint32_t capacity, int shDegree);
  bool uploadTile(uint32_t offset, const splat::SplatCloud& cloud);
  // CPU-order compatibility, allocated lazily even when the device supports GPU sort.
  bool writeOrder(const uint32_t* order, uint32_t count);

  GpuWorldInfo info() const { return {count_, shDegree_}; }
  id<MTLBuffer> splats() const { return splats_; }
  id<MTLBuffer> harmonics() const { return sh_; }
  id<MTLBuffer> order() const { return orders_[currentOrder_]; }

 private:
  explicit MetalWorld(id<MTLDevice> device) : device_(device) {}
  id<MTLDevice> device_ = nil;
  id<MTLBuffer> splats_ = nil;
  id<MTLBuffer> sh_ = nil;
  std::array<id<MTLBuffer>, 2> orders_{};
  uint32_t currentOrder_ = 0;
  uint32_t count_ = 0;
  int shDegree_ = 0;
  bool slab_ = false;
};

}  // namespace splatkit
