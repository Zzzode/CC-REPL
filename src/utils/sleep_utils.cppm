module;

#include <chrono>
#include <thread>
#include <functional>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>

export module cc.utils.sleep_utils;

export namespace cc::utils {

using namespace std::chrono;

// Interruptible sleep for a given number of milliseconds
inline void sleep_ms(int64_t ms) {
    std::this_thread::sleep_for(milliseconds(ms));
}

// Sleep until a specific time point
inline void sleep_until(system_clock::time_point tp) {
    std::this_thread::sleep_until(tp);
}

// High-resolution timer for measuring elapsed time
class Timer {
public:
    Timer() = default;

    void start() {
        start_ = high_resolution_clock::now();
        running_ = true;
    }

    void stop() {
        if (running_) {
            end_ = high_resolution_clock::now();
            running_ = false;
        }
    }

    // Get elapsed time as nanoseconds
    nanoseconds elapsed() const {
        auto end = running_ ? high_resolution_clock::now() : end_;
        return duration_cast<nanoseconds>(end - start_);
    }

    // Get elapsed time in milliseconds as double
    double elapsed_ms() const {
        return duration<double, std::milli>(elapsed()).count();
    }

    bool is_running() const { return running_; }

private:
    high_resolution_clock::time_point start_{};
    high_resolution_clock::time_point end_{};
    bool running_ = false;
};

// Periodic callback timer that runs a function at fixed intervals
class IntervalTimer {
public:
    IntervalTimer(milliseconds interval, std::function<void()> callback)
        : interval_(interval), callback_(std::move(callback)) {}

    ~IntervalTimer() { stop(); }

    // Non-copyable
    IntervalTimer(const IntervalTimer&) = delete;
    IntervalTimer& operator=(const IntervalTimer&) = delete;

    // Start the interval timer in a background thread
    void start() {
        if (running_.load()) return;
        running_.store(true);

        thread_ = std::thread([this]() {
            while (running_.load()) {
                std::unique_lock lock(mutex_);
                // Wait for the interval or until stopped
                if (cv_.wait_for(lock, interval_, [this] {
                    return !running_.load();
                })) {
                    break; // Stopped
                }
                // Execute callback
                if (running_.load() && callback_) {
                    callback_();
                }
            }
        });
    }

    // Stop the interval timer
    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool is_running() const { return running_.load(); }

private:
    milliseconds interval_;
    std::function<void()> callback_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace cc::utils
