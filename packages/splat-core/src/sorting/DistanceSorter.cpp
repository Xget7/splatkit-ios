#include "splat/sorting/DistanceSorter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace splat {

// Four threads cover the cull in 10 ms for 2M splats on a phone; more gain nothing on
// memory bound passes. Small clouds stay on the caller.
constexpr std::size_t kMaxWorkers = 4;
constexpr std::size_t kMinPerWorker = 100000;

DistanceSorter::DistanceSorter(std::vector<float> positions)
    : pool_(kMaxWorkers - 1), positions_(std::move(positions)) {
  const std::size_t n = count();
  keys_.resize(n);
  keysScratch_.resize(n);
  orderScratch_.resize(n);
}

void DistanceSorter::place(std::size_t first, const float* xyz, std::size_t n) {
  if (first >= count()) return;
  n = std::min(n, count() - first);
  std::memcpy(&positions_[first * 3], xyz, n * 3 * sizeof(float));
}

namespace {

// Key: bit pattern of the squared distance. Non negative floats compare like their bits,
// so an integer sort on them is a float sort. Inverting the bits makes the largest
// distance the smallest key, which turns an ascending sort into back to front.
inline uint32_t distanceKey(const float* p, Vec3 from) {
  const float dx = p[0] - from.x;
  const float dy = p[1] - from.y;
  const float dz = p[2] - from.z;
  const float d2 = dx * dx + dy * dy + dz * dz;
  uint32_t bits = 0;
  std::memcpy(&bits, &d2, sizeof(bits));
  return ~bits;
}

}  // namespace

void DistanceSorter::sortSubset(Vec3 from, std::vector<uint32_t>& subset) {
  const std::size_t n = std::min(subset.size(), count());
  subset.resize(n);
  parallelFor(n, [&](std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i)
      keys_[i] = distanceKey(&positions_[subset[i] * 3], from);
  });
  radixSort(n, subset);
}

void DistanceSorter::parallelFor(std::size_t n,
                                 const std::function<void(std::size_t, std::size_t)>& body) {
  const std::size_t workers =
      std::min<std::size_t>(pool_.width(), std::max<std::size_t>(1, n / kMinPerWorker));
  const std::size_t slice = (n + workers - 1) / workers;
  pool_.run(workers, [&](std::size_t w) {
    const std::size_t begin = w * slice;
    body(begin, std::min(n, begin + slice));
  });
}

void DistanceSorter::sort(Vec3 from, std::vector<uint32_t>& order) {
  const std::size_t n = count();
  order.resize(n);
  parallelFor(n, [&](std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
      keys_[i] = distanceKey(&positions_[i * 3], from);
      order[i] = static_cast<uint32_t>(i);
    }
  });
  radixSort(n, order);
}

std::size_t DistanceSorter::cull(const std::vector<uint32_t>& sorted, const Frustum& frustum,
                                 std::vector<uint32_t>& visible) {
  const std::size_t n = std::min(sorted.size(), count());
  visible.resize(n);

  // Two passes, both streaming. Testing positions through the sorted order would be a
  // random gather of 12 bytes per splat, which is what memory latency punishes; instead
  // the test runs over the positions in index order and leaves one bit per splat, and
  // the second pass reads the bits through the order. The bits fit in cache.
  const std::size_t words = (count() + 63) / 64;
  visibleBits_.resize(words);
  parallelFor(words, [&](std::size_t begin, std::size_t end) {
    for (std::size_t word = begin; word < end; ++word) {
      uint64_t bits = 0;
      const std::size_t first = word * 64;
      const std::size_t last = std::min(count(), first + 64);
      for (std::size_t i = first; i < last; ++i) {
        const float* p = &positions_[i * 3];
        if (frustum.contains({p[0], p[1], p[2]})) bits |= uint64_t{1} << (i - first);
      }
      visibleBits_[word] = bits;
    }
  });

  // Each slice of the sorted order compacts into scratch at its own offset; joining the
  // slices in order keeps the sort order intact.
  std::vector<std::size_t> starts;
  std::vector<std::size_t> counts;
  std::mutex slicesMutex;
  parallelFor(n, [&](std::size_t begin, std::size_t end) {
    std::size_t out = begin;
    for (std::size_t i = begin; i < end; ++i) {
      const uint32_t index = sorted[i];
      const bool in = (visibleBits_[index >> 6] >> (index & 63)) & 1u;
      orderScratch_[out] = index;
      out += in ? 1 : 0;  // branch free: the write lands anyway and is overwritten if not kept
    }
    const std::lock_guard<std::mutex> lock(slicesMutex);
    starts.push_back(begin);
    counts.push_back(out - begin);
  });

  // Slices finish in any order; join them by their start.
  std::vector<std::size_t> byStart(starts.size());
  for (std::size_t i = 0; i < byStart.size(); ++i) byStart[i] = i;
  std::sort(byStart.begin(), byStart.end(),
            [&](std::size_t a, std::size_t b) { return starts[a] < starts[b]; });
  std::size_t total = 0;
  for (const std::size_t k : byStart) {
    std::memcpy(&visible[total], &orderScratch_[starts[k]], counts[k] * sizeof(uint32_t));
    total += counts[k];
  }
  visible.resize(total);
  return total;
}

void DistanceSorter::radixSort(std::size_t n, std::vector<uint32_t>& order) {
  // LSD radix sort, 8 bits per pass, least significant digit first. Each pass is a
  // stable counting sort, so after four passes the full 32-bit key is ordered.
  uint32_t* keysIn = keys_.data();
  uint32_t* keysOut = keysScratch_.data();
  uint32_t* orderIn = order.data();
  uint32_t* orderOut = orderScratch_.data();
  for (int shift = 0; shift < 32; shift += 8) {
    std::array<uint32_t, 256> histogram{};
    for (std::size_t i = 0; i < n; ++i) ++histogram[(keysIn[i] >> shift) & 0xFF];
    uint32_t running = 0;
    for (uint32_t& h : histogram) {
      const uint32_t c = h;
      h = running;
      running += c;
    }
    for (std::size_t i = 0; i < n; ++i) {
      const uint32_t digit = (keysIn[i] >> shift) & 0xFF;
      const uint32_t dst = histogram[digit]++;
      keysOut[dst] = keysIn[i];
      orderOut[dst] = orderIn[i];
    }
    std::swap(keysIn, keysOut);
    std::swap(orderIn, orderOut);
  }
  // An even number of passes leaves the result in the caller's buffer. The copy only runs
  // if someone changes the pass count to an odd number.
  if (orderIn != order.data()) std::memcpy(order.data(), orderIn, n * sizeof(uint32_t));
}

}  // namespace splat
