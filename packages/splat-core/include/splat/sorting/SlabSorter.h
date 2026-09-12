#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "splat/math/Frustum.h"
#include "splat/sorting/DistanceSorter.h"

namespace splat {

// The sorter of a tiled world: a DistanceSorter over the slab every resident tile lives
// in, run on its own thread. Tiles place their positions as they land; each request names
// the ranges to draw and the frustum, and the thread sorts those ranges back to front
// when the camera moved or the ranges changed, and otherwise only culls the order it has
// (ADR 0009). Indices in a result are slab indices, what the draw order buffer holds.
class SlabSorter {
 public:
  struct Range {
    std::uint32_t offset;
    std::uint32_t count;
    bool operator==(const Range& o) const { return offset == o.offset && count == o.count; }
  };
  struct Result {
    std::vector<std::uint32_t> order;  // the visible splats of the ranges, back to front
    double sortMillis = 0;             // the most recent sort, which this order may reuse
    double cullMillis = 0;
    std::size_t sorted = 0;     // splats in the ranges sorted
    std::uint64_t request = 0;  // the requestVisible this order answers
  };

  explicit SlabSorter(std::uint32_t capacity);
  ~SlabSorter();

  SlabSorter(const SlabSorter&) = delete;
  SlabSorter& operator=(const SlabSorter&) = delete;

  // Positions (xyz per splat) of a tile that landed at `offset`. Applied before the next sort.
  void place(std::uint32_t offset, std::vector<float> positions);
  // Schedules the visible order of these ranges from this camera. Returns the id of the
  // request, which the result carries; a newer request replaces one not started yet.
  std::uint64_t requestVisible(const Frustum& frustum, std::vector<Range> ranges);
  // The newest finished order not yet taken, if any.
  std::optional<Result> take();

 private:
  struct Placement {
    std::uint32_t offset;
    std::vector<float> positions;
  };
  struct Request {
    Frustum frustum;
    std::vector<Range> ranges;
    std::uint64_t id = 0;
  };
  void run();

  DistanceSorter sorter_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::vector<Placement> placements_;
  std::optional<Request> pending_;
  std::uint64_t requests_ = 0;
  std::optional<Result> finished_;
  // Worker thread only.
  std::vector<std::uint32_t> sorted_;
  std::optional<Vec3> sortedFrom_;
  std::vector<Range> sortedRanges_;
  double lastSortMillis_ = 0;
};

}  // namespace splat
