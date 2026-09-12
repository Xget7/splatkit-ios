#include "rendering/MetalWorld.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "rendering/MetalCompute.h"
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

id<MTLBuffer> privateBuffer(id<MTLDevice> device, size_t bytes) {
  if (bytes > device.maxBufferLength) return nil;
  return [device newBufferWithLength:std::max<size_t>(bytes, 4)
                             options:MTLResourceStorageModePrivate];
}

}  // namespace

std::unique_ptr<MetalWorld> MetalWorld::upload(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                               const splat::SplatCloud& cloud, int maxShDegree) {
  if (!validCloud(cloud)) return nullptr;
  auto world = std::unique_ptr<MetalWorld>(new MetalWorld(device));
  world->count_ = static_cast<uint32_t>(cloud.count());
  const int degree = std::clamp(std::min(cloud.shDegree, maxShDegree), 0, 3);
  world->shDegree_ = carriesSh(cloud, degree) ? degree : 0;
  const size_t splatBytes = cloud.count() * sizeof(GpuSplat);
  const size_t stride = world->shDegree_ > 0 ? shStride(world->shDegree_) : 0;
  const size_t shBytes = std::max(size_t{4}, cloud.count() * stride * sizeof(uint32_t));
  world->splats_ = privateBuffer(device, splatBytes);
  world->sh_ = privateBuffer(device, shBytes);
  constexpr size_t chunk = 65536;
  id<MTLBuffer> stagingSplats =
      metal::buffer(device, std::min(size_t{world->count_}, chunk) * sizeof(GpuSplat));
  id<MTLBuffer> stagingSh =
      metal::buffer(device, std::min(size_t{world->count_}, chunk) * stride * 4);
  if (world->splats_ == nil || world->sh_ == nil || stagingSplats == nil || stagingSh == nil) {
    return nullptr;
  }
  // Pack directly into a bounded staging window, not full-size packed + staging copies.
  for (size_t offset = 0; offset < std::max(size_t{1}, cloud.count()); offset += chunk) {
    const size_t count = std::min(chunk, cloud.count() - offset);
    packSplatRange(cloud, offset, count, static_cast<GpuSplat*>(stagingSplats.contents));
    if (stride)
      packShRange(cloud, world->shDegree_, offset, count,
                  static_cast<uint32_t*>(stagingSh.contents));
    id<MTLCommandBuffer> upload = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [upload blitCommandEncoder];
    if (upload == nil || blit == nil) return nullptr;
    upload.label = @"Splat world upload";
    if (count > 0) {
      [blit copyFromBuffer:stagingSplats
               sourceOffset:0
                   toBuffer:world->splats_
          destinationOffset:offset * sizeof(GpuSplat)
                       size:count * sizeof(GpuSplat)];
    }
    if (stride > 0 && count > 0) {
      [blit copyFromBuffer:stagingSh
               sourceOffset:0
                   toBuffer:world->sh_
          destinationOffset:offset * stride * 4
                       size:count * stride * 4];
    } else if (offset == 0) {
      [blit fillBuffer:world->sh_ range:NSMakeRange(0, 4) value:0];
    }
    [blit endEncoding];
    [upload commit];
    [upload waitUntilCompleted];
    if (upload.status != MTLCommandBufferStatusCompleted) {
      LOGE("world upload failed: %s", upload.error.localizedDescription.UTF8String);
      return nullptr;
    }
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
  const auto packed = packSplats(cloud);
  std::memcpy(static_cast<GpuSplat*>(splats_.contents) + offset, packed.data(),
              packed.size() * sizeof(GpuSplat));
  if (shDegree_ == 0) return true;
  const size_t stride = shStride(shDegree_);
  const auto sh =
      carriesSh(cloud, shDegree_) ? packSh(cloud, shDegree_) : std::vector<uint32_t>(n * stride, 0);
  std::memcpy(static_cast<uint32_t*>(sh_.contents) + size_t{offset} * stride, sh.data(),
              sh.size() * sizeof(uint32_t));
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
