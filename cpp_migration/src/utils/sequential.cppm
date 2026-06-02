module;
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>

export module cc.utils.sequential;

export namespace cc::utils {

// A queue that executes tasks sequentially (one at a time) on a background thread
class SequentialQueue {
public:
    SequentialQueue() : running_(true) {
        worker_ = std::thread([this] { run(); });
    }

    ~SequentialQueue() {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    // Non-copyable, non-movable
    SequentialQueue(const SequentialQueue&) = delete;
    SequentialQueue& operator=(const SequentialQueue&) = delete;

    // Enqueue a void task
    std::future<void> enqueue(std::function<void()> task) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();

        {
            std::lock_guard lock(mutex_);
            tasks_.push([task = std::move(task), promise]() {
                try {
                    task();
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
        }
        cv_.notify_one();
        return future;
    }

    // Enqueue a typed task
    template<typename T>
    std::future<T> enqueue(std::function<T()> task) {
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();

        {
            std::lock_guard lock(mutex_);
            tasks_.push([task = std::move(task), promise]() {
                try {
                    promise->set_value(task());
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
        }
        cv_.notify_one();
        return future;
    }

    // Return number of pending tasks
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }

    // Block until all queued tasks have completed
    void drain() {
        std::unique_lock lock(mutex_);
        drain_cv_.wait(lock, [this] { return tasks_.empty() && !executing_; });
    }

private:
    void run() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return !tasks_.empty() || !running_; });

                if (!running_ && tasks_.empty()) break;

                task = std::move(tasks_.front());
                tasks_.pop();
                executing_ = true;
            }

            // Execute outside lock
            task();

            {
                std::lock_guard lock(mutex_);
                executing_ = false;
            }
            drain_cv_.notify_all();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable drain_cv_;
    std::queue<std::function<void()>> tasks_;
    std::thread worker_;
    bool running_;
    bool executing_ = false;
};

} // namespace cc::utils
