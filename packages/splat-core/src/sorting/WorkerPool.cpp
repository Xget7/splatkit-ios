#include "splat/sorting/WorkerPool.h"

namespace splat {

WorkerPool::WorkerPool(std::size_t threads) {
  for (std::size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { worker(); });
}

WorkerPool::~WorkerPool() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  for (auto& t : threads_) t.join();
}

void WorkerPool::run(std::size_t count, const Task& task) {
  std::unique_lock<std::mutex> lock(mutex_);
  task_ = &task;
  count_ = count;
  next_ = 0;
  running_ = 0;
  ++job_;
  wake_.notify_all();
  // The caller works too, then waits for the workers still on a task.
  for (;;) {
    if (next_ >= count_) break;
    const std::size_t index = next_++;
    lock.unlock();
    task(index);
    lock.lock();
  }
  done_.wait(lock, [this] { return running_ == 0; });
  task_ = nullptr;
}

void WorkerPool::worker() {
  unsigned seenJob = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    wake_.wait(lock, [&] { return stop_ || job_ != seenJob; });
    if (stop_) return;
    seenJob = job_;
    while (task_ != nullptr && next_ < count_) {
      const std::size_t index = next_++;
      ++running_;
      const Task* task = task_;
      lock.unlock();
      (*task)(index);
      lock.lock();
      if (--running_ == 0) done_.notify_all();
    }
  }
}

}  // namespace splat
