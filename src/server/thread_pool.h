#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// thread_pool.h – Fixed-size worker thread pool for request execution
// ─────────────────────────────────────────────────────────────────────────────
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace aegis {

class ThreadPool {
public:
    explicit ThreadPool(size_t threadCount = 16);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a task for worker execution.
    // Returns false if the pool is already stopped.
    bool enqueue(std::function<void()> task);

    // Stop accepting new tasks and join all workers.
    void stop();

    size_t size() const { return workers_.size(); }
    bool isStopped() const { return stopped_; }

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mu_;
    std::condition_variable           cv_;
    bool                              stopped_{false};
};

}  // namespace aegis
