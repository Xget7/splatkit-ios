#pragma once

#include <cstdint>
#include <vector>

#include "splat/math/Frustum.h"
#include "splat/math/Mat4.h"
#include "splat/sorting/WorkerPool.h"

namespace splat {

// Orders splats back to front by Euclidean distance from a point.
//
// Distance rather than view depth on purpose: the order only depends on where the
// camera is, not where it looks, so turning (gyroscope, drag) never triggers a sort.
// Only translation does. The sort is an LSD radix sort on the float bits of the squared
// distance: 4 passes of 8 bits, linear time, no comparisons.
class DistanceSorter {
 public:
  // Keeps a copy of the positions (xyz per splat) so the caller's cloud may go away.
  explicit DistanceSorter(std::vector<float> positions);

  std::size_t count() const { return positions_.size() / 3; }

  // Overwrites the positions of splats [first, first + n) with `xyz`, for a slab that
  // tiles land in. Not concurrent with a sort or a cull.
  void place(std::size_t first, const float* xyz, std::size_t n);

  // Fills `order` with every splat index, farthest first.
  void sort(Vec3 from, std::vector<uint32_t>& order);

  // Reorders the given indices in place, farthest first. For a level of detail
  // selection: only the chosen nodes are sorted.
  void sortSubset(Vec3 from, std::vector<uint32_t>& subset);

  // Filters a sorted order down to the splats inside the frustum, keeping their relative
  // order, so `visible` is back to front too. Linear and parallel: far cheaper than a
  // sort, which is why turning costs a cull and only moving costs a sort. `visible` is
  // resized to the count returned.
  std::size_t cull(const std::vector<uint32_t>& sorted, const Frustum& frustum,
                   std::vector<uint32_t>& visible);

 private:
  // Sorts the first n entries of keys_ and order together, ascending by key.
  void radixSort(std::size_t n, std::vector<uint32_t>& order);

  // Splits [0, n) into slices for the pool, one per worker, and runs `body(begin, end)`.
  void parallelFor(std::size_t n, const std::function<void(std::size_t, std::size_t)>& body);

  WorkerPool pool_;
  std::vector<float> positions_;
  std::vector<uint32_t> keys_;
  std::vector<uint32_t> keysScratch_;
  std::vector<uint32_t> orderScratch_;
  std::vector<uint64_t> visibleBits_;  // one bit per splat, set by the last cull
};

}  // namespace splat
