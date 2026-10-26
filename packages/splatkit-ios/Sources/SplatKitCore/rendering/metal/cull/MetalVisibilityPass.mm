#include "rendering/metal/cull/MetalVisibilityPass.h"

#include <algorithm>
#include <cstring>

#include "rendering/metal/device/MetalShaderLibrary.h"
#include "splatkit/Log.h"

namespace splatkit {

bool MetalVisibilityPass::create(id<MTLDevice> device, id<MTLLibrary> library, KeyBits depthBits) {
  device_ = device;
  library_ = library;
  depthBits_ = depthBits;
  if (!createPipelines()) return false;
  bool ok = true;
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
    count_[slot] = metal::buffer(device, sizeof(uint32_t));
    // The CPU reads the count and writes the ranges; only the GPU touches the draw arguments.
    drawArguments_[slot] =
        metal::buffer(device, kDrawBatches * kDrawArgumentBytes, MTLResourceStorageModePrivate);
    ranges_[slot] = metal::buffer(device, size_t{kMaxRanges} * sizeof(SplatRenderer::Range));
    rangeStarts_[slot] = metal::buffer(device, size_t{kMaxRanges + 1} * sizeof(uint32_t));
    ok = ok && count_[slot] != nil && drawArguments_[slot] != nil && ranges_[slot] != nil &&
         rangeStarts_[slot] != nil;
  }
  return ok;
}

// Only the pipelines depend on the key width; the buffers and the radix scratch are reused.
bool MetalVisibilityPass::reconfigure(KeyBits depthBits) {
  if (device_ == nil || library_ == nil) return false;
  const KeyBits previousBits = depthBits_;
  depthBits_ = depthBits;
  if (!createPipelines()) {
    depthBits_ = previousBits;
    return false;
  }
  return true;
}

bool MetalVisibilityPass::createPipelines() {
  const bool quantized = depthBits_ == KeyBits::Low16;
  bool ok = true;
  for (int degree = 0; degree < kShDegrees; ++degree) {
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    uint32_t value = static_cast<uint32_t>(degree);
    [constants setConstantValue:&value type:MTLDataTypeUInt atIndex:kFnShDegree];
    [constants setConstantValue:&quantized type:MTLDataTypeBool atIndex:kFnQuantizedDepth];
    bool indexed = false;
    [constants setConstantValue:&indexed type:MTLDataTypeBool atIndex:kFnIndexedLod];
    visibility_[static_cast<size_t>(degree)] =
        metal::pipeline(device_, library_, "visibility", constants);
    indexed = true;
    [constants setConstantValue:&indexed type:MTLDataTypeBool atIndex:kFnIndexedLod];
    indexedVisibility_[static_cast<size_t>(degree)] =
        metal::pipeline(device_, library_, "visibility", constants);
    ok = ok && indexedVisibility_[static_cast<size_t>(degree)] != nil;
    ok = ok && visibility_[static_cast<size_t>(degree)] != nil;
    const auto pipeline = visibility_[static_cast<size_t>(degree)];
    ok = ok && pipeline.threadExecutionWidth == 32 &&
         pipeline.maxTotalThreadsPerThreadgroup >= kThreads;
  }
  prepareDraw_ = metal::pipeline(device_, library_, "prepareDrawArguments");
  return ok && prepareDraw_ != nil;
}

std::vector<id<MTLComputePipelineState>> MetalVisibilityPass::pipelines() const {
  std::vector<id<MTLComputePipelineState>> all(visibility_.begin(), visibility_.end());
  all.insert(all.end(), indexedVisibility_.begin(), indexedVisibility_.end());
  all.push_back(prepareDraw_);
  return all;
}

uint32_t MetalVisibilityPass::slotsFor(uint32_t capacity, uint32_t activeCapacity) {
  if (activeCapacity > 0) capacity = std::min(capacity, activeCapacity);
  return std::max(capacity, 1u);
}

bool MetalVisibilityPass::reserve(uint32_t capacity, uint32_t activeCapacity) {
  const uint32_t sourceCapacity = capacity;
  capacity = slotsFor(capacity, activeCapacity);
  if (capacity <= capacity_) {
    sourceCapacity_ = sourceCapacity;
    indexedOnly_ = activeCapacity > 0 && activeCapacity < sourceCapacity;
    return true;
  }
  id<MTLBuffer> projected = metal::buffer(device_, size_t{capacity} * sizeof(ProjectedSplat),
                                          MTLResourceStorageModePrivate);
  if (projected == nil) {
    LOGE("visibility buffers for %u splats failed", capacity);
    return false;
  }
  projected_ = projected;
  capacity_ = capacity;
  sourceCapacity_ = sourceCapacity;
  indexedOnly_ = activeCapacity > 0 && activeCapacity < sourceCapacity;
  return true;
}

bool MetalVisibilityPass::encodeProject(id<MTLCommandBuffer> cmd, uint32_t slot,
                                        id<MTLBuffer> uniforms, id<MTLBuffer> splats,
                                        id<MTLBuffer> sh, int shDegree,
                                        const SplatRenderer::Range* ranges, uint32_t rangeCount,
                                        id<MTLBuffer> keys, id<MTLBuffer> values,
                                        id<MTLBuffer> indices, id<MTLBuffer> activeCount) {
  const bool indexed = indices != nil;
  if (indexedOnly_ && !indexed) return false;
  if (indexed != (activeCount != nil)) return false;
  if (slot >= kFramesInFlight || rangeCount > kMaxRanges || capacity_ == 0 ||
      (rangeCount > 0 && ranges == nullptr))
    return false;
  const size_t sortBytes = size_t{capacity_} * sizeof(uint32_t);
  if (keys.length < sortBytes || values.length < sortBytes) return false;
  // Validate before writing slot inputs, so a rejected request cannot corrupt them.
  uint32_t total = 0;
  for (uint32_t i = 0; i < rangeCount; ++i) {
    const auto& range = ranges[i];
    if (range.offset > sourceCapacity_ || range.count > sourceCapacity_ - range.offset ||
        range.count > capacity_ - total)
      return false;
    total += range.count;
  }
  auto* starts = static_cast<uint32_t*>(rangeStarts_[slot].contents);
  starts[0] = 0;
  for (uint32_t i = 0; i < rangeCount; ++i) starts[i + 1] = starts[i] + ranges[i].count;
  if (rangeCount > 0) {
    std::memcpy(ranges_[slot].contents, ranges, size_t{rangeCount} * sizeof(*ranges));
  }
  // Queue-ordered reset, without a CPU fence.
  id<MTLBlitCommandEncoder> reset = [cmd blitCommandEncoder];
  [reset fillBuffer:count_[slot] range:NSMakeRange(0, sizeof(uint32_t)) value:0];
  [reset endEncoding];

  id<MTLComputeCommandEncoder> cull = [cmd computeCommandEncoder];
  cull.label = @"Splat visibility and projection";
  const int degree = std::clamp(shDegree, 0, kShDegrees - 1);
  [cull setComputePipelineState:(indexed ? indexedVisibility_
                                         : visibility_)[static_cast<size_t>(degree)]];
  [cull setBuffer:uniforms offset:0 atIndex:0];
  [cull setBuffer:splats offset:0 atIndex:1];
  [cull setBuffer:ranges_[slot] offset:0 atIndex:2];
  [cull setBuffer:rangeStarts_[slot] offset:0 atIndex:3];
  [cull setBytes:&rangeCount length:sizeof(rangeCount) atIndex:4];
  [cull setBuffer:keys offset:0 atIndex:5];
  [cull setBuffer:values offset:0 atIndex:6];
  [cull setBuffer:count_[slot] offset:0 atIndex:7];
  [cull setBuffer:sh offset:0 atIndex:8];
  [cull setBuffer:projected_ offset:0 atIndex:9];
  if (indexed) {
    [cull setBuffer:indices offset:0 atIndex:10];
    [cull setBuffer:activeCount offset:0 atIndex:11];
    total = capacity_;
  }
  const NSUInteger groups = (size_t{std::max(total, 1u)} + kThreads - 1) / kThreads;
  [cull dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
       threadsPerThreadgroup:MTLSizeMake(kThreads, 1, 1)];
  [cull endEncoding];
  return true;
}

void MetalVisibilityPass::encodeIndirect(id<MTLCommandBuffer> cmd, uint32_t slot) {
  id<MTLComputeCommandEncoder> draw = [cmd computeCommandEncoder];
  draw.label = @"Splat indirect draw arguments";
  [draw setComputePipelineState:prepareDraw_];
  [draw setBuffer:count_[slot] offset:0 atIndex:0];
  [draw setBuffer:drawArguments_[slot] offset:0 atIndex:1];
  [draw dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [draw endEncoding];
}

}  // namespace splatkit
