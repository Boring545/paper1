#ifndef CFD_UTILS_FIXED_WORKER_POOL_H
#define CFD_UTILS_FIXED_WORKER_POOL_H

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cfd::utils {

class FixedWorkerPool {
 public:
  explicit FixedWorkerPool(size_t worker_count) {
    worker_count = std::max<size_t>(1, worker_count);
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
      workers_.emplace_back([this]() { worker_loop(); });
    }
  }

  FixedWorkerPool(const FixedWorkerPool&) = delete;
  FixedWorkerPool& operator=(const FixedWorkerPool&) = delete;

  ~FixedWorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    task_cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  template <typename Fn>
  void parallel_for(size_t task_count, Fn&& fn) {
    if (task_count == 0) {
      return;
    }
    if (task_count == 1 || workers_.empty()) {
      for (size_t i = 0; i < task_count; ++i) {
        fn(i);
      }
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_tasks_ != 0) {
        throw std::logic_error("FixedWorkerPool::parallel_for called while tasks are pending");
      }
      first_exception_ = nullptr;
      pending_tasks_ = task_count;
      for (size_t i = 0; i < task_count; ++i) {
        tasks_.emplace([&, i]() { fn(i); });
      }
    }

    task_cv_.notify_all();

    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this]() { return pending_tasks_ == 0; });
    if (first_exception_ != nullptr) {
      std::rethrow_exception(first_exception_);
    }
  }

 private:
  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        task_cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
      }

      try {
        task();
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (first_exception_ == nullptr) {
          first_exception_ = std::current_exception();
        }
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        --pending_tasks_;
        if (pending_tasks_ == 0) {
          done_cv_.notify_one();
        }
      }
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable task_cv_;
  std::condition_variable done_cv_;
  bool stopping_ = false;
  size_t pending_tasks_ = 0;
  std::exception_ptr first_exception_ = nullptr;
};

inline size_t recommended_worker_count(size_t task_count, size_t max_worker_count = 6) {
  const size_t hardware_workers = std::max<size_t>(1, std::thread::hardware_concurrency());
  return std::max<size_t>(1, std::min({task_count, max_worker_count, hardware_workers}));
}

}  // namespace cfd::utils

#endif  // CFD_UTILS_FIXED_WORKER_POOL_H
