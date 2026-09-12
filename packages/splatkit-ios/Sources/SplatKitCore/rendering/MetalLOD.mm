#include "rendering/MetalLOD.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "rendering/MetalCompute.h"
#include "splat/lod/LodFile.h"

namespace splatkit {
bool MetalLOD::create(id<MTLDevice> device, id<MTLLibrary> library) {
  device_ = device;
  initialize_ = metal::pipeline(device, library, "initializeSplatLOD");
  evaluate_ = metal::pipeline(device, library, "evaluateSplatLOD");
  budget_ = metal::pipeline(device, library, "budgetSplatLOD");
  compact_ = metal::pipeline(device, library, "compactSplatLOD");
  allocate_ = metal::pipeline(device, library, "allocateSplatLOD");
  scatter_ = metal::pipeline(device, library, "scatterSplatLOD");
  advance_ = metal::pipeline(device, library, "advanceSplatLOD");
  emit_ = metal::pipeline(device, library, "emitSplatLOD");
  scanGroups_ = metal::pipeline(device, library, "scanSplatLODGroups");
  scanBlocks_ = metal::pipeline(device, library, "scanSplatLODBlocks");
  if (!initialize_ || !evaluate_ || !budget_ || !compact_ || !allocate_ || !scatter_ || !advance_ ||
      !emit_ || !scanGroups_ || !scanBlocks_)
    return false;
  for (auto p : {evaluate_, compact_, scatter_, emit_, scanGroups_})
    if (p.threadExecutionWidth != 32 || p.maxTotalThreadsPerThreadgroup < 256) return false;
  return true;
}
bool MetalLOD::upload(id<MTLCommandQueue> queue, const splat::LodTree& tree, uint32_t capacity,
                      float pixelLimit, float colorWeight, bool frustumCull) {
  auto valid = splat::validateLodTree(tree);
  if (!valid) LOGE("invalid LOD hierarchy: %s", valid.error().message.c_str());
  if (!valid || capacity == 0 || !std::isfinite(pixelLimit) || pixelLimit < 0 ||
      !std::isfinite(colorWeight) || colorWeight < 0)
    return false;
  capacity = std::min({capacity, 2200000u, static_cast<uint32_t>(tree.leafCount)});
  splat::LodSelectionData compatibility;
  const auto* data = &tree.selection;
  if (data->clusters.empty()) {
    LOGI("LOD v1 compatibility: constructing selection metadata once at upload");
    compatibility = splat::buildLodSelectionData(tree);
    data = &compatibility;
  }
  const size_t frontierCapacity = std::min(size_t{capacity}, data->clusters.size());
  const size_t groupCount = (frontierCapacity + 31) / 32;
  constexpr auto storage = MTLResourceStorageModePrivate;
  auto upload = [&](const void* source, size_t bytes) -> id<MTLBuffer> {
    auto buffer = metal::buffer(device_, std::max(bytes, size_t{4}), storage);
    constexpr size_t chunkBytes = 4u << 20;
    auto staging = metal::buffer(device_, std::max(size_t{4}, std::min(chunkBytes, bytes)));
    if (!buffer || !staging) return nil;
    for (size_t offset = 0; offset < bytes; offset += chunkBytes) {
      const size_t count = std::min(chunkBytes, bytes - offset);
      std::memcpy(staging.contents, static_cast<const uint8_t*>(source) + offset, count);
      auto command = [queue commandBuffer];
      auto blit = [command blitCommandEncoder];
      [blit copyFromBuffer:staging
               sourceOffset:0
                   toBuffer:buffer
          destinationOffset:offset
                       size:count];
      [blit endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status == MTLCommandBufferStatusError) return nil;
    }
    return buffer;
  };
  nodes_ = upload(data->clusters.data(), data->clusters.size() * sizeof(splat::LodCluster));
  leaves_ = upload(data->leaves.data(), data->leaves.size() * 4);
  indices_ = metal::buffer(device_, size_t{capacity} * 4, storage);
  packets_ =
      metal::buffer(device_, std::min(size_t{capacity}, data->clusters.size()) * 16, storage);
  costs_ = metal::buffer(device_, frontierCapacity * 8, storage);
  offsets_ = metal::buffer(device_, frontierCapacity * 16, storage);
  costGroups_ = metal::buffer(device_, groupCount * 16, storage);
  groups_ = metal::buffer(device_, groupCount * 16, storage);
  blocks_ = metal::buffer(device_, ((groupCount + 255) / 256 + 1) * 16, storage);
  state_ = metal::buffer(device_, 80, storage);
  for (auto& buffer : frontier_) buffer = metal::buffer(device_, frontierCapacity * 4, storage);
  if (!nodes_ || !leaves_ || !indices_ || !packets_ || !costs_ || !offsets_ || !costGroups_ ||
      !groups_ || !blocks_ || !state_ || !frontier_[0] || !frontier_[1])
    return false;
  depth_ = valid.value() + 1;
  config_ = {capacity, pixelLimit, colorWeight, frustumCull ? 1u : 0u};
  LOGI(
      "GPU LOD SSE: %zu interior clusters, %zu leaves, %u rounds, capacity %u, %.2f px, color %.2f",
      data->clusters.size(), data->leaves.size(), depth_, capacity, pixelLimit, colorWeight);
  return true;
}
void MetalLOD::encode(id<MTLCommandBuffer> command, id<MTLBuffer> uniforms) {
  auto start = [&](id<MTLComputePipelineState> pipeline, NSString* label) {
    auto e = [command computeCommandEncoder];
    e.label = label;
    [e setComputePipelineState:pipeline];
    return e;
  };
  auto one = [](id<MTLComputeCommandEncoder> e) {
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e endEncoding];
  };
  auto active = [&](id<MTLComputeCommandEncoder> e) {
    [e dispatchThreadgroupsWithIndirectBuffer:state_
                         indirectBufferOffset:32
                        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [e endEncoding];
  };
  auto scan = [&](id<MTLBuffer> groups) {
    auto e = start(scanGroups_, @"LOD parallel scan of SIMD totals");
    [e setBuffer:groups offset:0 atIndex:0];
    [e setBuffer:blocks_ offset:0 atIndex:1];
    [e setBuffer:state_ offset:0 atIndex:2];
    [e dispatchThreadgroupsWithIndirectBuffer:state_
                         indirectBufferOffset:64
                        threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [e endEncoding];
    e = start(scanBlocks_, @"LOD scan of block totals");
    [e setBuffer:blocks_ offset:0 atIndex:0];
    [e setBuffer:state_ offset:0 atIndex:1];
    one(e);
  };
  auto e = start(initialize_, @"Initialize interior LOD frontier");
  [e setBuffer:frontier_[0] offset:0 atIndex:0];
  [e setBuffer:state_ offset:0 atIndex:1];
  one(e);
  for (uint32_t level = 0; level < depth_; ++level) {
    e = start(evaluate_, @"LOD node bounds and screen error");
    [e setBuffer:uniforms offset:0 atIndex:0];
    [e setBuffer:nodes_ offset:0 atIndex:1];
    [e setBuffer:frontier_[level & 1u] offset:0 atIndex:2];
    [e setBuffer:state_ offset:0 atIndex:3];
    [e setBytes:&config_ length:sizeof(config_) atIndex:4];
    [e setBuffer:costs_ offset:0 atIndex:5];
    [e setBuffer:costGroups_ offset:0 atIndex:6];
    active(e);
    scan(costGroups_);
    e = start(budget_, @"LOD deterministic capacity guard");
    [e setBuffer:costGroups_ offset:0 atIndex:0];
    [e setBuffer:costs_ offset:0 atIndex:1];
    [e setBuffer:state_ offset:0 atIndex:2];
    [e setBytes:&config_ length:sizeof(config_) atIndex:3];
    [e setBuffer:blocks_ offset:0 atIndex:4];
    one(e);
    e = start(compact_, @"LOD SIMD prefix compaction");
    [e setBuffer:nodes_ offset:0 atIndex:0];
    [e setBuffer:frontier_[level & 1u] offset:0 atIndex:1];
    [e setBuffer:costs_ offset:0 atIndex:2];
    [e setBuffer:costGroups_ offset:0 atIndex:3];
    [e setBuffer:state_ offset:0 atIndex:4];
    [e setBuffer:offsets_ offset:0 atIndex:5];
    [e setBuffer:groups_ offset:0 atIndex:6];
    [e setBuffer:blocks_ offset:0 atIndex:7];
    active(e);
    scan(groups_);
    e = start(allocate_, @"LOD deterministic packet offsets");
    [e setBuffer:blocks_ offset:0 atIndex:0];
    [e setBuffer:state_ offset:0 atIndex:1];
    one(e);
    e = start(scatter_, @"LOD child frontier and leaf packets");
    [e setBuffer:nodes_ offset:0 atIndex:0];
    [e setBuffer:frontier_[level & 1u] offset:0 atIndex:1];
    [e setBuffer:offsets_ offset:0 atIndex:2];
    [e setBuffer:groups_ offset:0 atIndex:3];
    [e setBuffer:state_ offset:0 atIndex:4];
    [e setBuffer:frontier_[(level + 1) & 1u] offset:0 atIndex:5];
    [e setBuffer:packets_ offset:0 atIndex:6];
    [e setBuffer:indices_ offset:0 atIndex:7];
    [e setBuffer:leaves_ offset:0 atIndex:8];
    [e setBuffer:blocks_ offset:0 atIndex:9];
    active(e);
    e = start(advance_, @"Advance LOD indirect work");
    [e setBuffer:state_ offset:0 atIndex:0];
    one(e);
  }
  e = start(emit_, @"Emit LOD leaf indices cooperatively");
  [e setBuffer:packets_ offset:0 atIndex:0];
  [e setBuffer:leaves_ offset:0 atIndex:1];
  [e setBuffer:state_ offset:0 atIndex:2];
  [e setBuffer:indices_ offset:0 atIndex:3];
  [e dispatchThreadgroupsWithIndirectBuffer:state_
                       indirectBufferOffset:44
                      threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
  [e endEncoding];
}
}  // namespace splatkit
