#include "splat/sorting/AsyncSorter.h"

#include <chrono>
#include <utility>

namespace splat {

AsyncSorter::AsyncSorter(std::vector<float> positions) : sorter_(std::move(positions)) {
  thread_ = std::thread([this] { run(); });
}

AsyncSorter::AsyncSorter(std::shared_ptr<const LodTree> tree)
    : sorter_(tree->nodes.positions), tree_(std::move(tree)) {
  thread_ = std::thread([this] { run(); });
}

AsyncSorter::~AsyncSorter() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void AsyncSorter::request(Vec3 from) {
  const std::lock_guard<std::mutex> lock(mutex_);

  if (sortedFrom_) {
    const float dx = from.x - sortedFrom_->x;
    const float dy = from.y - sortedFrom_->y;
    const float dz = from.z - sortedFrom_->z;
    if ((dx * dx + dy * dy + dz * dz) < 0.000001f) return;
  }

  pending_ = Request{from, std::nullopt, LodSettings{}};
  wake_.notify_one();
}

void AsyncSorter::requestVisible(const Frustum& frustum, LodSettings lod) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_ = Request{frustum.origin, frustum, lod};
  }
  wake_.notify_one();
}

std::optional<AsyncSorter::Result> AsyncSorter::take() {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::optional<Result> out = std::move(finished_);
  finished_.reset();
  return out;
}

void AsyncSorter::run() {
  std::vector<uint32_t> candidateIndices;
  std::vector<uint32_t> visibleIndices;

  for (;;) {
    Request request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      request = *pending_;
      pending_.reset();
    }

    using Clock = std::chrono::steady_clock;
    auto millisBetween = [](Clock::time_point a, Clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };

    const auto start = Clock::now();
    const bool useLod = tree_ && request.lod.budget > 0;

    if (useLod) {
      selectLodNodes(*tree_, request.from, request.lod.view, request.lod.budget,
                     request.lod.pixelScaleLimit, candidateIndices);
    } else {
      const std::size_t totalCount = sorter_.count();
      if (candidateIndices.size() != totalCount) {
        candidateIndices.resize(totalCount);
        for (std::size_t i = 0; i < totalCount; ++i) {
          candidateIndices[i] = static_cast<uint32_t>(i);
        }
      }
    }
    const auto selectEnd = Clock::now();
    lastSelectMillis_ = millisBetween(start, selectEnd);
    lastSelected_ = candidateIndices.size();

    const auto cullStart = Clock::now();
    if (request.frustum) {
      sorter_.cull(candidateIndices, *request.frustum, visibleIndices);
    } else {
      visibleIndices = candidateIndices;
    }
    const auto cullEnd = Clock::now();
    const double cullMillis = millisBetween(cullStart, cullEnd);

    const auto sortStart = Clock::now();
    sorter_.sortSubset(request.from, visibleIndices);
    const auto sortEnd = Clock::now();
    lastSortMillis_ = millisBetween(sortStart, sortEnd);

    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> recycled =
        finished_ ? std::move(finished_->order) : std::vector<uint32_t>();
    finished_ = Result{std::move(visibleIndices), lastSortMillis_, cullMillis, lastSelectMillis_,
                       lastSelected_};

    visibleIndices = std::move(recycled);
  }
}

}  // namespace splat
