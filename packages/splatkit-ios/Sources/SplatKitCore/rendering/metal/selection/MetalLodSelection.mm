#include "rendering/metal/selection/MetalLodSelection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "rendering/metal/device/MetalShaderLibrary.h"
#include "splat/lod/LodFile.h"

namespace splatkit {
namespace {

// Match SplatLOD.metal: its kernels run in 256 thread groups of 32 wide SIMD groups, and
// the block scan sums 256 SIMD group totals per block.
constexpr uint32_t kSimdWidth = 32;
constexpr uint32_t kGroupThreads = 256;
constexpr uint32_t kBlockGroups = 256;
// LodState in SplatLOD.metal: its size and the offsets of the four indirect dispatch
// arguments (dispatchX, emitX, scanX, scatterX).
constexpr size_t kStateBytes = 88;
constexpr NSUInteger kDispatchArgumentsOffset = 32;
constexpr NSUInteger kEmitArgumentsOffset = 44;
constexpr NSUInteger kScanArgumentsOffset = 64;
constexpr NSUInteger kScatterArgumentsOffset = 76;

}  // namespace

bool MetalLodSelection::create(id<MTLDevice> device, id<MTLLibrary> library) {
  device_ = device;
  initialize_ = metal::pipeline(device, library, "initializeSplatLOD");
  evaluate_ = metal::pipeline(device, library, "evaluateSplatLOD");
  budget_ = metal::pipeline(device, library, "budgetSplatLOD");
  compact_ = metal::pipeline(device, library, "compactSplatLOD");
  commit_ = metal::pipeline(device, library, "commitSplatLOD");
  scatter_ = metal::pipeline(device, library, "scatterSplatLOD");
  emit_ = metal::pipeline(device, library, "emitSplatLOD");
  scanGroups_ = metal::pipeline(device, library, "scanSplatLODGroups");
  if (!initialize_ || !evaluate_ || !budget_ || !compact_ || !commit_ || !scatter_ || !emit_ ||
      !scanGroups_)
    return false;
  for (auto p : {evaluate_, compact_, scatter_, emit_, scanGroups_})
    if (p.threadExecutionWidth != kSimdWidth || p.maxTotalThreadsPerThreadgroup < kGroupThreads)
      return false;
  return true;
}
bool MetalLodSelection::upload(const splat::LodTree& tree, uint32_t capacity, float pixelLimit,
                               float colorWeight, bool frustumCull) {
  auto valid = splat::validateLodTree(tree);
  if (!valid) LOGE("invalid LOD hierarchy: %s", valid.error().message.c_str());
  if (!valid || capacity == 0 || !std::isfinite(pixelLimit) || pixelLimit < 0 ||
      !std::isfinite(colorWeight) || colorWeight < 0)
    return false;
  capacity = std::min({capacity, kMaxBudget, static_cast<uint32_t>(tree.leafCount)});
  splat::LodSelectionData compatibility;
  const auto* data = &tree.selection;
  if (data->clusters.empty()) {
    LOGI("LOD v1 compatibility: constructing selection metadata once at upload");
    compatibility = splat::buildLodSelectionData(tree);
    data = &compatibility;
  }
  const size_t frontierCapacity = std::min(size_t{capacity}, data->clusters.size());
  const size_t groupCount = (frontierCapacity + kSimdWidth - 1) / kSimdWidth;
  constexpr auto storage = MTLResourceStorageModePrivate;
  // Unified memory: the tree is written straight into the GPU's bytes. The buffers the
  // selection writes stay private.
  auto upload = [&](const void* source, size_t bytes) -> id<MTLBuffer> {
    auto buffer = metal::buffer(device_, std::max(bytes, size_t{4}));
    if (buffer) std::memcpy(buffer.contents, source, bytes);
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
  blocks_ =
      metal::buffer(device_, ((groupCount + kBlockGroups - 1) / kBlockGroups + 1) * 16, storage);
  state_ = metal::buffer(device_, kStateBytes, storage);
  for (auto& buffer : frontier_) buffer = metal::buffer(device_, frontierCapacity * 4, storage);
  if (!nodes_ || !leaves_ || !indices_ || !packets_ || !costs_ || !offsets_ || !costGroups_ ||
      !groups_ || !blocks_ || !state_ || !frontier_[0] || !frontier_[1])
    return false;
  depth_ = valid.value() + 1;
  capacity_ = capacity;
  config_ = {capacity, pixelLimit, colorWeight, frustumCull ? 1u : 0u};
  LOGI(
      "GPU LOD SSE: %zu interior clusters, %zu leaves, %u rounds, capacity %u, %.2f px, color %.2f",
      data->clusters.size(), data->leaves.size(), depth_, capacity, pixelLimit, colorWeight);
  return true;
}
void MetalLodSelection::encode(id<MTLCommandBuffer> command, id<MTLBuffer> uniforms) {
  // Serial: each dispatch sees the writes and indirect arguments of the ones before it.
  auto e = [command computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
  e.label = @"LOD selection";
  // A phase's debug group closes with its dispatch.
  auto phase = [&](id<MTLComputePipelineState> pipeline, NSString* label) {
    [e pushDebugGroup:label];
    [e setComputePipelineState:pipeline];
  };
  auto one = [&] {
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e popDebugGroup];
  };
  auto indirect = [&](NSUInteger offset) {
    [e dispatchThreadgroupsWithIndirectBuffer:state_
                         indirectBufferOffset:offset
                        threadsPerThreadgroup:MTLSizeMake(kGroupThreads, 1, 1)];
    [e popDebugGroup];
  };
  auto scan = [&](id<MTLBuffer> groups) {
    phase(scanGroups_, @"LOD parallel scan of SIMD totals");
    [e setBuffer:groups offset:0 atIndex:0];
    [e setBuffer:blocks_ offset:0 atIndex:1];
    [e setBuffer:state_ offset:0 atIndex:2];
    indirect(kScanArgumentsOffset);
  };
  phase(initialize_, @"Initialize interior LOD frontier");
  [e setBuffer:frontier_[0] offset:0 atIndex:0];
  [e setBuffer:state_ offset:0 atIndex:1];
  one();
  for (uint32_t level = 0; level < depth_; ++level) {
    phase(evaluate_, @"LOD node bounds and screen error");
    [e setBuffer:uniforms offset:0 atIndex:0];
    [e setBuffer:nodes_ offset:0 atIndex:1];
    [e setBuffer:frontier_[level & 1u] offset:0 atIndex:2];
    [e setBuffer:state_ offset:0 atIndex:3];
    [e setBytes:&config_ length:sizeof(config_) atIndex:4];
    [e setBuffer:costs_ offset:0 atIndex:5];
    [e setBuffer:costGroups_ offset:0 atIndex:6];
    indirect(kDispatchArgumentsOffset);
    scan(costGroups_);
    phase(budget_, @"LOD block scan and deterministic capacity guard");
    [e setBuffer:costGroups_ offset:0 atIndex:0];
    [e setBuffer:costs_ offset:0 atIndex:1];
    [e setBuffer:state_ offset:0 atIndex:2];
    [e setBytes:&config_ length:sizeof(config_) atIndex:3];
    [e setBuffer:blocks_ offset:0 atIndex:4];
    one();
    phase(compact_, @"LOD SIMD prefix compaction");
    [e setBuffer:nodes_ offset:0 atIndex:0];
    [e setBuffer:frontier_[level & 1u] offset:0 atIndex:1];
    [e setBuffer:costs_ offset:0 atIndex:2];
    [e setBuffer:costGroups_ offset:0 atIndex:3];
    [e setBuffer:state_ offset:0 atIndex:4];
    [e setBuffer:offsets_ offset:0 atIndex:5];
    [e setBuffer:groups_ offset:0 atIndex:6];
    [e setBuffer:blocks_ offset:0 atIndex:7];
    indirect(kDispatchArgumentsOffset);
    scan(groups_);
    phase(commit_, @"LOD block scan, packet offsets and next round");
    [e setBuffer:blocks_ offset:0 atIndex:0];
    [e setBuffer:state_ offset:0 atIndex:1];
    one();
    phase(scatter_, @"LOD child frontier and leaf packets");
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
    indirect(kScatterArgumentsOffset);
  }
  phase(emit_, @"Emit LOD leaf indices cooperatively");
  [e setBuffer:packets_ offset:0 atIndex:0];
  [e setBuffer:leaves_ offset:0 atIndex:1];
  [e setBuffer:state_ offset:0 atIndex:2];
  [e setBuffer:indices_ offset:0 atIndex:3];
  indirect(kEmitArgumentsOffset);
  [e endEncoding];
}
}  // namespace splatkit
