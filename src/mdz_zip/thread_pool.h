#pragma once
#include "common.h"

namespace fz {

class ThreadPool {
public:
    explicit ThreadPool(size_t thread_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F>
    auto submit(F&& fn) -> std::future<typename std::invoke_result_t<F>> {
        using Result = typename std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(fn));
        std::future<Result> future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                throw std::runtime_error("ThreadPool is stopping");
            }
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
        return future;
    }

    size_t size() const noexcept { return workers_.size(); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
};

} // namespace fz
