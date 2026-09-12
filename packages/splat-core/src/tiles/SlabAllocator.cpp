#include "splat/tiles/SlabAllocator.h"

namespace splat {

SlabAllocator::SlabAllocator(std::uint32_t capacity) : capacity_(capacity) {
  if (capacity > 0) free_[0] = capacity;
}

// Best fit: the smallest hole that takes the range, so big holes stay whole for big tiles.
std::optional<std::uint32_t> SlabAllocator::allocate(std::uint32_t count) {
  if (count == 0) return std::nullopt;
  auto best = free_.end();
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second < count) continue;
    if (best == free_.end() || it->second < best->second) best = it;
  }
  if (best == free_.end()) return std::nullopt;
  const std::uint32_t offset = best->first;
  const std::uint32_t left = best->second - count;
  free_.erase(best);
  if (left > 0) free_[offset + count] = left;
  used_ += count;
  return offset;
}

void SlabAllocator::release(std::uint32_t offset, std::uint32_t count) {
  if (count == 0) return;
  used_ -= count;
  auto next = free_.lower_bound(offset);
  // Merge with the free range that ends where this one starts.
  if (next != free_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      offset = prev->first;
      count += prev->second;
      free_.erase(prev);
    }
  }
  // And with the one that starts where this one ends.
  if (next != free_.end() && next->first == offset + count) {
    count += next->second;
    free_.erase(next);
  }
  free_[offset] = count;
}

}  // namespace splat
