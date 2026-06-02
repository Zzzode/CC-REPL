module;
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

export module cc.utils.idle_timeout;

export namespace cc::utils {

class IdleTimeout {
public:
    IdleTimeout(std::chrono::seconds timeout, std::function<void()> on_idle)
        : timeout_(timeout), on_idle_(std::move(on_idle)), running_(false), idle_(false) {}

    ~IdleTimeout() { stop(); }

    // Non-copyable
    IdleTimeout(const IdleTimeout&) = delete;
    IdleTimeout& operator=(const IdleTimeout&) = delete;

    // Reset the idle timer (called on user activity)
    void reset() {
        std::lock_guard lock(mutex_);
        last_activity_ = std::chrono::steady_clock::now();
        idle_ = false;
        cv_.notify_one();
    }

    // Start the idle timeout watcher
    void start() {
        if (running_.exchange(true)) return;

        last_activity_ = std::chrono::steady_clock::now();
        worker_ = std::thread([this] {
            while (running_) {
                std::unique_lock lock(mutex_);

                // Wait until timeout or stop
                auto deadline = last_activity_ + timeout_;
                cv_.wait_until(lock, deadline, [this] {
                    return !running_.load() ||
                           std::chrono::steady_clock::now() < last_activity_ + timeout_;
                });

                if (!running_) break;

                // Check if we've actually been idle long enough
                auto now = std::chrono::steady_clock::now();
                if (now - last_activity_ >= timeout_) {
                    idle_ = true;
                    lock.unlock();
                    if (on_idle_) on_idle_();
                    lock.lock();
                }
            }
        });
    }

    // Stop the idle timeout watcher
    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    // Check if currently idle
    bool is_idle() const { return idle_.load(); }

private:
    std::chrono::seconds timeout_;
    std::function<void()> on_idle_;
    std::atomic<bool> running_;
    std::atomic<bool> idle_;
    std::chrono::steady_clock::time_point last_activity_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
};

} // namespace cc::utils
