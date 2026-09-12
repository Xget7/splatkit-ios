#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace splat {

// A few threads that stay alive between jobs, so a 10 ms cull does not pay to spawn
// them every time.
class WorkerPool {
 public:
  using Task = std::function<void(std::size_t index)>;

  explicit WorkerPool(std::size_t threads);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Workers plus the calling thread.
  std::size_t width() const { return threads_.size() + 1; }

  // Runs task(0 .. count - 1) across the workers and the caller; returns when all finished.
  void run(std::size_t count, const Task& task);

 private:
  void worker();

  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  std::vector<std::thread> threads_;
  bool stop_ = false;
  // The current job: task indices are handed out from `next_` until `count_`.
  const Task* task_ = nullptr;
  std::size_t count_ = 0;
  std::size_t next_ = 0;
  std::size_t running_ = 0;
  unsigned job_ = 0;
};

}  // namespace splat
