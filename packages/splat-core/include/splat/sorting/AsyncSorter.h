#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "splat/lod/LodTree.h"
#include "splat/math/Mat4.h"
#include "splat/sorting/DistanceSorter.h"

namespace splat {

// Runs DistanceSorter on its own thread. The renderer asks whenever the camera moved or
// turned and keeps drawing with the last order it received; requests made while one is
// running collapse into one, always with the latest camera. Every request selects, culls
// (given a frustum) and sorts from scratch. With a level of detail tree the order covers
// the nodes the budget selects from the camera instead of every splat.
struct LodSettings {
  std::size_t budget = 0;  // 0 draws every splat
  float pixelScaleLimit = 0.0f;
  LodView view;
};

class AsyncSorter {
 public:
  struct Result {
    std::vector<uint32_t> order;  // only the visible splats when a frustum was given
    double sortMillis = 0;
    double cullMillis = 0;
    double selectMillis = 0;   // the most recent level of detail selection
    std::size_t selected = 0;  // nodes the selection chose, before the cull
  };

  explicit AsyncSorter(std::vector<float> positions);
  // Sorts over the nodes of a tree; indices in results are node indices.
  explicit AsyncSorter(std::shared_ptr<const LodTree> tree);
  ~AsyncSorter();

  AsyncSorter(const AsyncSorter&) = delete;
  AsyncSorter& operator=(const AsyncSorter&) = delete;

  // Schedules a full order from this position, nothing culled.
  void request(Vec3 from);
  // Schedules the visible order for this camera: select, cull, then sort.
  void requestVisible(const Frustum& frustum, LodSettings lod = {});

  // The newest finished order not yet taken, if any. Moves it out.
  std::optional<Result> take();

 private:
  void run();

  DistanceSorter sorter_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  struct Request {
    Vec3 from;
    std::optional<Frustum> frustum;
    LodSettings lod;
  };
  std::optional<Request> pending_;
  std::optional<Result> finished_;
  // Worker thread only.
  std::shared_ptr<const LodTree> tree_;
  double lastSortMillis_ = 0;
  double lastSelectMillis_ = 0;
  std::size_t lastSelected_ = 0;
};

}  // namespace splat
