#include "rendering/MetalVisibility.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rendering/MetalCompute.h"

namespace splatkit {

bool MetalVisibility::create(id<MTLDevice> device, id<MTLLibrary> library, bool experiment,
                             float minPixelRadius, MetalRadixSort::KeyBits depthBits) {
  if (!std::isfinite(minPixelRadius) || minPixelRadius < 0.0f) return false;
  device_ = device;
  depthBits_ = depthBits;
  const bool quantized = depthBits_ == MetalRadixSort::KeyBits::Low16;
  storage_ = experiment ? MTLResourceStorageModePrivate : MTLResourceStorageModeShared;
  bool ok = true;
  for (int degree = 0; degree < kShDegrees; ++degree) {
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    uint32_t value = static_cast<uint32_t>(degree);
    [constants setConstantValue:&value type:MTLDataTypeUInt atIndex:0];
    [constants setConstantValue:&experiment type:MTLDataTypeBool atIndex:1];
    [constants setConstantValue:&minPixelRadius type:MTLDataTypeFloat atIndex:2];
    [constants setConstantValue:&quantized type:MTLDataTypeBool atIndex:4];
    bool indexed = false;
    [constants setConstantValue:&indexed type:MTLDataTypeBool atIndex:3];
    visibility_[static_cast<size_t>(degree)] =
        metal::pipeline(device, library, "visibility", constants);
    indexed = true;
    [constants setConstantValue:&indexed type:MTLDataTypeBool atIndex:3];
    indexedVisibility_[static_cast<size_t>(degree)] =
        metal::pipeline(device, library, "visibility", constants);
    ok = ok && indexedVisibility_[static_cast<size_t>(degree)] != nil;
    ok = ok && visibility_[static_cast<size_t>(degree)] != nil;
    const auto pipeline = visibility_[static_cast<size_t>(degree)];
    ok = ok && pipeline.threadExecutionWidth == 32 &&
         pipeline.maxTotalThreadsPerThreadgroup >= kThreads;
  }
  prepareDraw_ = metal::pipeline(device, library, "prepareDrawArguments");
  for (uint32_t slot = 0; slot < kSlots; ++slot) {
    count_[slot] = metal::buffer(device, sizeof(uint32_t), storage_);
    countReadback_[slot] = experiment ? metal::buffer(device, sizeof(uint32_t)) : count_[slot];
    drawArguments_[slot] = metal::buffer(device, (kDrawBatches + 1) * kDrawArgumentBytes, storage_);
    ranges_[slot] = metal::buffer(device, size_t{kMaxRanges} * sizeof(SplatRenderer::Range));
    rangeStarts_[slot] = metal::buffer(device, size_t{kMaxRanges + 1} * sizeof(uint32_t));
    ok = ok && count_[slot] != nil && countReadback_[slot] != nil && drawArguments_[slot] != nil &&
         ranges_[slot] != nil && rangeStarts_[slot] != nil;
  }
  return ok && prepareDraw_ != nil && sort_.create(device, library, storage_);
}

bool MetalVisibility::reserve(uint32_t capacity, uint32_t activeCapacity) {
  const uint32_t sourceCapacity = capacity;
  if (activeCapacity > 0) capacity = std::min(capacity, activeCapacity);
  capacity = std::max(capacity, 1u);
  if (capacity <= capacity_) {
    sourceCapacity_ = sourceCapacity;
    indexedOnly_ = activeCapacity > 0 && activeCapacity < sourceCapacity;
    return true;
  }
  id<MTLBuffer> projected =
      metal::buffer(device_, size_t{capacity} * sizeof(ProjectedSplat), storage_);
  if (projected == nil || !sort_.reserve(capacity)) {
    LOGE("visibility buffers for %u splats failed", capacity);
    return false;
  }
  projected_ = projected;
  capacity_ = capacity;
  sourceCapacity_ = sourceCapacity;
  indexedOnly_ = activeCapacity > 0 && activeCapacity < sourceCapacity;
  return true;
}

bool MetalVisibility::encode(id<MTLCommandBuffer> cmd, uint32_t slot, id<MTLBuffer> uniforms,
                             id<MTLBuffer> splats, id<MTLBuffer> sh, int shDegree,
                             const SplatRenderer::Range* ranges, uint32_t rangeCount,
                             id<MTLBuffer> indices, id<MTLBuffer> activeCount) {
  const bool indexed = indices != nil;
  if (indexedOnly_ && !indexed) return false;
  if (indexed != (activeCount != nil)) return false;
  if (slot >= kSlots || rangeCount > kMaxRanges || capacity_ == 0 ||
      (rangeCount > 0 && ranges == nullptr))
    return false;
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
  // Queue-ordered reset also works for GPU-private counters, without a CPU fence.
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
  [cull setBuffer:sort_.keys() offset:0 atIndex:5];
  [cull setBuffer:sort_.values() offset:0 atIndex:6];
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

  sort_.encode(cmd, count_[slot], depthBits_);
  id<MTLComputeCommandEncoder> draw = [cmd computeCommandEncoder];
  draw.label = @"Splat indirect draw arguments";
  [draw setComputePipelineState:prepareDraw_];
  [draw setBuffer:count_[slot] offset:0 atIndex:0];
  [draw setBuffer:drawArguments_[slot] offset:0 atIndex:1];
  [draw dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [draw endEncoding];
  if (countReadback_[slot] != count_[slot]) {
    id<MTLBlitCommandEncoder> readback = [cmd blitCommandEncoder];
    [readback copyFromBuffer:count_[slot]
                sourceOffset:0
                    toBuffer:countReadback_[slot]
           destinationOffset:0
                        size:sizeof(uint32_t)];
    [readback endEncoding];
  }
  return true;
}

}  // namespace splatkit
