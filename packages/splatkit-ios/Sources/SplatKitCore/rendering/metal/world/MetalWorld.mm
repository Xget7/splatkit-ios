#include "rendering/metal/world/MetalWorld.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "rendering/metal/device/MetalShaderLibrary.h"
#include "splatkit/rendering/GpuLayout.h"

namespace splatkit {
namespace {

// Host-supplied clouds must be complete before packing reads their attributes.
bool validCloud(const splat::SplatCloud& cloud) {
  const size_t n = cloud.count();
  return n <= std::numeric_limits<uint32_t>::max() && cloud.positions.size() == n * 3 &&
         cloud.covariances.size() == n * 6 && cloud.colors.size() == n * 3 &&
         cloud.alphas.size() == n;
}

}  // namespace

std::unique_ptr<MetalWorld> MetalWorld::upload(id<MTLDevice> device, const splat::SplatCloud& cloud,
                                               int maxShDegree) {
  if (!validCloud(cloud)) return nullptr;
  auto world = std::unique_ptr<MetalWorld>(new MetalWorld(device));
  world->count_ = static_cast<uint32_t>(cloud.count());
  const int degree = std::clamp(std::min(cloud.shDegree, maxShDegree), 0, 3);
  world->shDegree_ = carriesSh(cloud, degree) ? degree : 0;
  const size_t stride = world->shDegree_ > 0 ? shStride(world->shDegree_) : 0;
  world->splats_ = metal::buffer(device, cloud.count() * sizeof(GpuSplat));
  world->sh_ =
      metal::buffer(device, std::max(size_t{4}, cloud.count() * stride * sizeof(uint32_t)));
  if (world->splats_ == nil || world->sh_ == nil) return nullptr;
  // Unified memory: the pack writes the GPU's bytes in place, so a load is one pass over the
  // cloud with no staging copy. Shared storage is coherent by the time a command buffer commits.
  packSplatRange(cloud, 0, cloud.count(), static_cast<GpuSplat*>(world->splats_.contents));
  if (stride > 0) {
    packShRange(cloud, world->shDegree_, 0, cloud.count(),
                static_cast<uint32_t*>(world->sh_.contents));
  } else {
    std::memset(world->sh_.contents, 0, sizeof(uint32_t));
  }
  return world;
}

std::unique_ptr<MetalWorld> MetalWorld::slab(id<MTLDevice> device, uint32_t capacity,
                                             int shDegree) {
  if (capacity == 0) return nullptr;
  auto world = std::unique_ptr<MetalWorld>(new MetalWorld(device));
  world->count_ = capacity;
  world->shDegree_ = std::clamp(shDegree, 0, 3);
  world->slab_ = true;
  const size_t shBytes = world->shDegree_ > 0
                             ? size_t{capacity} * shStride(world->shDegree_) * sizeof(uint32_t)
                             : sizeof(uint32_t);
  world->splats_ = metal::buffer(device, size_t{capacity} * sizeof(GpuSplat));
  world->sh_ = metal::buffer(device, shBytes);
  if (world->splats_ == nil || world->sh_ == nil) return nullptr;
  return world;
}

bool MetalWorld::uploadTile(uint32_t offset, const splat::SplatCloud& cloud) {
  if (!slab_ || !validCloud(cloud)) return false;
  const size_t n = cloud.count();
  if (offset > count_ || n > count_ - offset) return false;
  if (n == 0) return true;
  packSplatRange(cloud, 0, n, static_cast<GpuSplat*>(splats_.contents) + offset);
  if (shDegree_ == 0) return true;
  const size_t stride = shStride(shDegree_);
  uint32_t* sh = static_cast<uint32_t*>(sh_.contents) + size_t{offset} * stride;
  if (carriesSh(cloud, shDegree_)) {
    packShRange(cloud, shDegree_, 0, n, sh);
  } else {
    std::memset(sh, 0, n * stride * sizeof(uint32_t));
  }
  return true;
}

bool MetalWorld::writeOrder(const uint32_t* order, uint32_t count) {
  if (count > count_ || (count > 0 && order == nullptr)) return false;
  if (orders_[0] == nil) {
    std::array<id<MTLBuffer>, 2> orders{};
    for (auto& buffer : orders) buffer = metal::buffer(device_, size_t{count_} * sizeof(uint32_t));
    if (orders[0] == nil || orders[1] == nil) return false;
    orders_ = orders;
  }
  const uint32_t next = currentOrder_ ^ 1u;
  if (count > 0) {
    std::memcpy(orders_[next].contents, order, size_t{count} * sizeof(uint32_t));
  }
  currentOrder_ = next;
  return true;
}

}  // namespace splatkit
