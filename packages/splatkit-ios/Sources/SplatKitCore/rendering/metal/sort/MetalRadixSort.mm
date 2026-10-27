#include "rendering/metal/sort/MetalRadixSort.h"

#include "rendering/metal/device/MetalShaderLibrary.h"
#include "rendering/metal/layout/MetalShaderTypes.h"

namespace splatkit {

bool MetalRadixSort::create(id<MTLDevice> device, id<MTLLibrary> library) {
  device_ = device;
  prepare_ = metal::pipeline(device, library, "prepareRadixSort");
  histogram_ = metal::pipeline(device, library, "radixHistogram");
  scan_ = metal::pipeline(device, library, "radixScan");
  // Both key widths take at least two passes, so no pass is both the first and the last.
  auto scatter = [&](bool first, bool last) {
    MTLFunctionConstantValues* constants = [MTLFunctionConstantValues new];
    [constants setConstantValue:&first type:MTLDataTypeBool atIndex:kFnFirstSortPass];
    [constants setConstantValue:&last type:MTLDataTypeBool atIndex:kFnLastSortPass];
    return metal::pipeline(device, library, "radixScatter", constants);
  };
  scatterFirst_ = scatter(true, false);
  scatter_ = scatter(false, false);
  scatterLast_ = scatter(false, true);
  // The shader's blocked layout and ballot masks use 32 lanes per SIMD group.
  if (histogram_.threadExecutionWidth != 32 || scan_.threadExecutionWidth != 32 ||
      scatterFirst_.threadExecutionWidth != 32 || scatter_.threadExecutionWidth != 32 ||
      scatterLast_.threadExecutionWidth != 32) {
    LOGE("radix sort requires 32-lane SIMD groups");
    return false;
  }
  // Every buffer here is written by a GPU pass before any pass reads it, so none needs the CPU.
  totals_ = metal::buffer(device, kBins * sizeof(uint32_t), MTLResourceStorageModePrivate);
  dispatch_ = metal::buffer(device, 4 * sizeof(uint32_t), MTLResourceStorageModePrivate);
  return prepare_ != nil && histogram_ != nil && scan_ != nil && scatterFirst_ != nil &&
         scatter_ != nil && scatterLast_ != nil && totals_ != nil && dispatch_ != nil;
}

bool MetalRadixSort::reserve(uint32_t capacity) {
  capacity = std::max(capacity, 1u);
  if (capacity <= capacity_) return true;
  const size_t blocks = (size_t{capacity} + kBlock - 1) / kBlock;
  std::array<id<MTLBuffer>, 2> keys{};
  std::array<id<MTLBuffer>, 2> values{};
  constexpr auto storage = MTLResourceStorageModePrivate;
  const size_t bytes = size_t{capacity} * sizeof(uint32_t);
  for (auto& key : keys) key = metal::buffer(device_, bytes, storage);
  for (auto& value : values) value = metal::buffer(device_, bytes, storage);
  id<MTLBuffer> histogram = metal::buffer(device_, blocks * kBins * sizeof(uint32_t), storage);
  if (keys[0] == nil || keys[1] == nil || values[0] == nil || values[1] == nil ||
      histogram == nil) {
    LOGE("sort buffers for %u pairs failed", capacity);
    return false;
  }
  keys_ = keys;
  values_ = values;
  histogramBuffer_ = histogram;
  capacity_ = capacity;
  return true;
}

void MetalRadixSort::encode(id<MTLCommandBuffer> cmd, id<MTLBuffer> count, KeyBits bits) {
  id<MTLBuffer> dispatch = dispatch_;
  id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
  enc.label = bits == KeyBits::Low16 ? @"Splat radix sort (16 bits / 2 passes)"
                                     : @"Splat radix sort (32 bits / 4 passes)";
  [enc setComputePipelineState:prepare_];
  [enc setBuffer:count offset:0 atIndex:0];
  [enc setBuffer:dispatch offset:0 atIndex:1];
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];

  const MTLSize threads = MTLSizeMake(kThreads, 1, 1);
  const uint32_t passes = bits == KeyBits::Low16 ? 2u : 4u;
  for (uint32_t pass = 0; pass < passes; ++pass) {
    const uint32_t shift = pass * kDigitBits;
    const uint32_t in = pass & 1u;
    const uint32_t out = in ^ 1u;

    [enc setComputePipelineState:histogram_];
    [enc setBuffer:keys_[in] offset:0 atIndex:0];
    [enc setBuffer:count offset:0 atIndex:1];
    [enc setBuffer:dispatch offset:0 atIndex:2];
    [enc setBytes:&shift length:sizeof(shift) atIndex:3];
    [enc setBuffer:histogramBuffer_ offset:0 atIndex:4];
    [enc dispatchThreadgroupsWithIndirectBuffer:dispatch
                           indirectBufferOffset:0
                          threadsPerThreadgroup:threads];

    [enc setComputePipelineState:scan_];
    [enc setBuffer:dispatch offset:0 atIndex:0];
    [enc setBuffer:histogramBuffer_ offset:0 atIndex:1];
    [enc setBuffer:totals_ offset:0 atIndex:2];
    [enc dispatchThreadgroups:MTLSizeMake(kBins, 1, 1) threadsPerThreadgroup:threads];

    [enc setComputePipelineState:pass == 0            ? scatterFirst_
                                 : pass + 1 == passes ? scatterLast_
                                                      : scatter_];
    [enc setBuffer:keys_[in] offset:0 atIndex:0];
    [enc setBuffer:values_[in] offset:0 atIndex:1];
    [enc setBuffer:keys_[out] offset:0 atIndex:2];
    [enc setBuffer:values_[out] offset:0 atIndex:3];
    [enc setBuffer:count offset:0 atIndex:4];
    [enc setBuffer:dispatch offset:0 atIndex:5];
    [enc setBytes:&shift length:sizeof(shift) atIndex:6];
    [enc setBuffer:histogramBuffer_ offset:0 atIndex:7];
    [enc setBuffer:totals_ offset:0 atIndex:8];
    [enc dispatchThreadgroupsWithIndirectBuffer:dispatch
                           indirectBufferOffset:0
                          threadsPerThreadgroup:threads];
  }
  [enc endEncoding];
}

}  // namespace splatkit
