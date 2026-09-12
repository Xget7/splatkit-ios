#import <Metal/Metal.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <random>
#include <vector>
#include "MetalTestContext.h"
#include "rendering/MetalRadixSort.h"

namespace splatkit {
namespace {
using test::Gpu;

struct Pair {
  uint32_t key;
  uint32_t value;
  bool operator==(const Pair& o) const { return key == o.key && value == o.value; }
};

// Fills the key and value buffers with `pairs`, sorts on the GPU, returns what came out.
std::vector<Pair> sortOnGpu(MetalRadixSort& v, const std::vector<Pair>& pairs,
                            MetalRadixSort::KeyBits bits = MetalRadixSort::KeyBits::Full32) {
  Gpu& gpu = Gpu::get();
  auto* keys = static_cast<uint32_t*>(v.keys().contents);
  auto* values = static_cast<uint32_t*>(v.values().contents);
  for (size_t i = 0; i < pairs.size(); ++i) {
    keys[i] = pairs[i].key;
    values[i] = pairs[i].value;
  }
  uint32_t count = static_cast<uint32_t>(pairs.size());
  id<MTLBuffer> countBuffer = [gpu.device newBufferWithBytes:&count
                                                      length:sizeof(count)
                                                     options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> cmd = [gpu.queue commandBuffer];
  v.encode(cmd, countBuffer, bits);
  [cmd commit];
  [cmd waitUntilCompleted];
  EXPECT_EQ(cmd.status, MTLCommandBufferStatusCompleted);
  std::vector<Pair> out(pairs.size());
  for (size_t i = 0; i < pairs.size(); ++i) {
    out[i] = {static_cast<const uint32_t*>(v.keys().contents)[i],
              static_cast<const uint32_t*>(v.values().contents)[i]};
  }
  return out;
}

std::vector<Pair> sortOnCpu(std::vector<Pair> pairs) {
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const Pair& a, const Pair& b) { return a.key < b.key; });
  return pairs;
}

std::vector<Pair> randomPairs(size_t n, uint32_t keyMask, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<Pair> pairs(n);
  for (size_t i = 0; i < n; ++i) pairs[i] = {rng() & keyMask, static_cast<uint32_t>(i)};
  return pairs;
}

class MetalRadixSortTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(Gpu::get().device, nil);
    ASSERT_NE(Gpu::get().library, nil);
    ASSERT_TRUE(sort.create(Gpu::get().device, Gpu::get().library));
  }
  MetalRadixSort sort;
};

TEST_F(MetalRadixSortTest, SortsRandomKeysLikeAStableCpuSort) {
  const size_t n = 1000003;  // many blocks, a partial last one
  ASSERT_TRUE(sort.reserve(n));
  const auto pairs = randomPairs(n, 0xffffffffu, 1);
  EXPECT_EQ(sortOnGpu(sort, pairs), sortOnCpu(pairs));
}

TEST_F(MetalRadixSortTest, KeepsTheOrderOfEqualKeys) {
  const size_t n = 70000;  // few distinct keys: long runs of ties across blocks
  ASSERT_TRUE(sort.reserve(n));
  const auto pairs = randomPairs(n, 0x7u, 2);
  EXPECT_EQ(sortOnGpu(sort, pairs), sortOnCpu(pairs));
}

TEST_F(MetalRadixSortTest, SortsSmallAndEmptyInputs) {
  ASSERT_TRUE(sort.reserve(64));
  EXPECT_EQ(sortOnGpu(sort, {}), std::vector<Pair>{});
  const std::vector<Pair> one{{5, 9}};
  EXPECT_EQ(sortOnGpu(sort, one), one);
  const auto pairs = randomPairs(17, 0xffffffffu, 3);
  EXPECT_EQ(sortOnGpu(sort, pairs), sortOnCpu(pairs));
}

TEST_F(MetalRadixSortTest, SortsExactlyOneBlock) {
  const size_t n = MetalRadixSort::kBlock;
  ASSERT_TRUE(sort.reserve(n));
  const auto pairs = randomPairs(n, 0xffffffffu, 4);
  EXPECT_EQ(sortOnGpu(sort, pairs), sortOnCpu(pairs));
}

TEST_F(MetalRadixSortTest, TwoPassKeysMatchFourPassAndStableCpuIncludingTiesAndTails) {
  ASSERT_TRUE(sort.reserve(1000003));
  for (uint32_t n : {0u, 1u, 31u, 32u, 33u, 4095u, 4096u, 4097u, 1000003u}) {
    SCOPED_TRACE(n);
    auto pairs = randomPairs(n, 0xffffu, 16);
    if (n > 1) {
      pairs.front().key = 65535;
      pairs.back().key = 0;
    }
    const auto expected = sortOnCpu(pairs);
    EXPECT_EQ(sortOnGpu(sort, pairs, MetalRadixSort::KeyBits::Low16), expected);
    EXPECT_EQ(sortOnGpu(sort, pairs), expected);
  }
  const auto ties = randomPairs(70000, 0x7u, 17);
  EXPECT_EQ(sortOnGpu(sort, ties, MetalRadixSort::KeyBits::Low16), sortOnCpu(ties));
}

// Not a check, a number: the GPU time of a sort at the scale a phone draws.
TEST_F(MetalRadixSortTest, ReportsTheSortTimeOfFiveMillionKeys) {
  const size_t n = 5000000;
  ASSERT_TRUE(sort.reserve(n));
  const auto pairs = randomPairs(n, 0xffffffffu, 5);
  auto* keys = static_cast<uint32_t*>(sort.keys().contents);
  auto* values = static_cast<uint32_t*>(sort.values().contents);
  for (size_t i = 0; i < n; ++i) {
    keys[i] = pairs[i].key;
    values[i] = pairs[i].value;
  }
  uint32_t count = static_cast<uint32_t>(n);
  id<MTLBuffer> countBuffer = [Gpu::get().device newBufferWithBytes:&count
                                                             length:sizeof(count)
                                                            options:MTLResourceStorageModeShared];
  double best = 1e9;
  for (int i = 0; i < 10; ++i) {
    id<MTLCommandBuffer> cmd = [Gpu::get().queue commandBuffer];
    sort.encode(cmd, countBuffer);
    [cmd commit];
    [cmd waitUntilCompleted];
    best = std::min(best, (cmd.GPUEndTime - cmd.GPUStartTime) * 1000.0);
    // The result is in place after an even number of passes: sort it again as is.
  }
  const auto* sorted = static_cast<const uint32_t*>(sort.keys().contents);
  EXPECT_TRUE(std::is_sorted(sorted, sorted + n));
  printf("[ sort     ] %zu keys: %.2f ms on %s\n", n, best, Gpu::get().device.name.UTF8String);
}

}  // namespace
}  // namespace splatkit
