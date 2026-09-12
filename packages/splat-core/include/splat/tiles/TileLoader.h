#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "splat/core/Result.h"
#include "splat/formats/SplatCloud.h"
#include "splat/formats/SplatDecoder.h"

namespace splat {

// Reads and decodes tile files on its own threads, most urgent first. The queue is
// replaced whole every time the scheduler plans, so a tile the camera left behind is
// never read; what was already started finishes and is reported like any other.
class TileLoader {
 public:
  struct Request {
    std::uint32_t tile;
    std::string path;
    float priority;  // bigger first
  };
  struct Loaded {
    std::uint32_t tile;
    Result<SplatCloud> cloud;
    double millis;
  };

  explicit TileLoader(SplatDecodeOptions options = {}, std::size_t threads = 1);
  ~TileLoader();

  TileLoader(const TileLoader&) = delete;
  TileLoader& operator=(const TileLoader&) = delete;

  // Replaces the queue. Tiles already started or finished are not started again.
  // Returns the tiles that were queued, not started, and are not in the new queue.
  std::vector<std::uint32_t> setQueue(std::vector<Request> queue);
  // Every tile decoded since the last take, in the order they finished.
  std::vector<Loaded> take();
  // Queued or being read.
  std::size_t pending() const;

 private:
  void run();

  SplatDecodeOptions options_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<std::thread> threads_;
  bool stop_ = false;
  std::vector<Request> queue_;
  std::vector<std::uint32_t> started_;  // being read right now
  std::vector<Loaded> finished_;
};

}  // namespace splat
