#include "splat/sorting/SlabSorter.h"

#include <chrono>

namespace splat {

SlabSorter::SlabSorter(std::uint32_t capacity)
    : sorter_(std::vector<float>(static_cast<std::size_t>(capacity) * 3, 0.0f)) {
  thread_ = std::thread([this] { run(); });
}

SlabSorter::~SlabSorter() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void SlabSorter::place(std::uint32_t offset, std::vector<float> positions) {
  const std::lock_guard<std::mutex> lock(mutex_);
  placements_.push_back({offset, std::move(positions)});
}

std::uint64_t SlabSorter::requestVisible(const Frustum& frustum, std::vector<Range> ranges) {
  std::uint64_t id = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    id = ++requests_;
    pending_ = Request{frustum, std::move(ranges), id};
  }
  wake_.notify_one();
  return id;
}

std::optional<SlabSorter::Result> SlabSorter::take() {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::optional<Result> out = std::move(finished_);
  finished_.reset();
  return out;
}

void SlabSorter::run() {
  using Clock = std::chrono::steady_clock;
  const auto millisBetween = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  std::vector<std::uint32_t> order;
  for (;;) {
    Request request;
    std::vector<Placement> placements;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      request = std::move(*pending_);
      pending_.reset();
      placements.swap(placements_);
    }
    for (const Placement& p : placements) {
      sorter_.place(p.offset, p.positions.data(), p.positions.size() / 3);
    }
    const Vec3 from = request.frustum.origin;
    const bool moved = !sortedFrom_ || sortedFrom_->x != from.x || sortedFrom_->y != from.y ||
                       sortedFrom_->z != from.z;
    const bool changed = request.ranges != sortedRanges_ || !placements.empty();
    const auto start = Clock::now();
    if (moved || changed) {
      sorted_.clear();
      for (const Range& r : request.ranges) {
        for (std::uint32_t i = 0; i < r.count; ++i) sorted_.push_back(r.offset + i);
      }
      sorter_.sortSubset(from, sorted_);
      sortedFrom_ = from;
      sortedRanges_ = request.ranges;
      lastSortMillis_ = millisBetween(start, Clock::now());
    }
    const auto sorted = Clock::now();
    sorter_.cull(sorted_, request.frustum, order);
    const auto culled = Clock::now();
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint32_t> recycled =
        finished_ ? std::move(finished_->order) : std::vector<std::uint32_t>();
    finished_ = Result{std::move(order), lastSortMillis_, millisBetween(sorted, culled),
                       sorted_.size(), request.id};
    order = std::move(recycled);
  }
}

}  // namespace splat
