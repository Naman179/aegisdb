#include "server/thread_pool.h"

namespace aegis {

ThreadPool::ThreadPool(size_t threadCount) {
    if (threadCount == 0) threadCount = 1;
    workers_.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait(lk, [this]() {
                        return stopped_ || !tasks_.empty();
                    });
                    if (stopped_ && tasks_.empty()) {
                        return;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                if (task) {
                    task();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

bool ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (stopped_) return false;
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::stop() {
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (stopped_) return;
        stopped_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

}  // namespace aegis
