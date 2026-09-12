#include "splat/tiles/TileLoader.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "splat/io/MappedFile.h"

namespace splat {

TileLoader::TileLoader(SplatDecodeOptions options, std::size_t threads) : options_(options) {
  for (std::size_t i = 0; i < std::max<std::size_t>(threads, 1); ++i) {
    threads_.emplace_back([this] { run(); });
  }
}

TileLoader::~TileLoader() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (auto& t : threads_) t.join();
}

std::vector<std::uint32_t> TileLoader::setQueue(std::vector<Request> queue) {
  const std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::uint32_t> dropped;
  for (const Request& old : queue_) {
    const bool kept = std::any_of(queue.begin(), queue.end(),
                                  [&](const Request& r) { return r.tile == old.tile; });
    if (!kept) dropped.push_back(old.tile);
  }
  queue_.clear();
  for (Request& r : queue) {
    const bool started = std::find(started_.begin(), started_.end(), r.tile) != started_.end();
    const bool finished = std::any_of(finished_.begin(), finished_.end(),
                                      [&](const Loaded& l) { return l.tile == r.tile; });
    if (!started && !finished) queue_.push_back(std::move(r));
  }
  wake_.notify_all();
  return dropped;
}

std::vector<TileLoader::Loaded> TileLoader::take() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(finished_, {});
}

std::size_t TileLoader::pending() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size() + started_.size();
}

void TileLoader::run() {
  using Clock = std::chrono::steady_clock;
  for (;;) {
    Request request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      auto best = std::max_element(
          queue_.begin(), queue_.end(),
          [](const Request& a, const Request& b) { return a.priority < b.priority; });
      request = std::move(*best);
      queue_.erase(best);
      started_.push_back(request.tile);
    }
    const auto start = Clock::now();
    Result<SplatCloud> cloud = Error{ErrorCode::unreadable, request.path};
    if (auto file = MappedFile::open(request.path)) {
      cloud = decodeSplatFile(file.value().data(), file.value().size(), options_);
    } else {
      cloud = file.error();
    }
    const double millis = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const std::lock_guard<std::mutex> lock(mutex_);
    started_.erase(std::find(started_.begin(), started_.end(), request.tile));
    finished_.push_back({request.tile, std::move(cloud), millis});
  }
}

}  // namespace splat
