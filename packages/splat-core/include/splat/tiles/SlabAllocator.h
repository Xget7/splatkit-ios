#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace splat {

// Hands out ranges of one fixed size buffer, the slab every resident tile lives in on the
// GPU. Best fit over a free list that merges neighbours, so a tile dropped next to a
// free range grows it. Not thread safe.
class SlabAllocator {
 public:
  explicit SlabAllocator(std::uint32_t capacity);

  std::uint32_t capacity() const { return capacity_; }
  std::uint32_t used() const { return used_; }

  // The offset of a free range of `count`, or nothing when none fits.
  std::optional<std::uint32_t> allocate(std::uint32_t count);
  // Returns a range obtained from `allocate`.
  void release(std::uint32_t offset, std::uint32_t count);

 private:
  std::uint32_t capacity_;
  std::uint32_t used_ = 0;
  std::map<std::uint32_t, std::uint32_t> free_;  // offset to count, disjoint, never touching
};

}  // namespace splat
