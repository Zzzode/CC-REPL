module;
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <cstddef>

export module cc.bridge.capacity_wake;

export namespace cc::bridge {

// CapacityWake provides a wake-on-capacity notification mechanism.
// Consumers can wait until capacity becomes available in a bounded buffer/queue.
class CapacityWake {
public:
    explicit CapacityWake(size_t max_capacity = 100)
        : max_capacity_(max_capacity), current_used_(0) {}

    // Notify that capacity has become available (e.g., after consuming items)
    void notify_capacity_available() {
        {
            std::lock_guard lock(mutex_);
            if (current_used_ > 0) {
                --current_used_;
            }
        }
        cv_.notify_one();
    }

    // Wait until there is available capacity, with timeout.
    // Returns true if capacity available, false on timeout.
    bool wait_for_capacity(std::chrono::seconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return current_used_ < max_capacity_;
        });
    }

    // Get the current available capacity
    size_t get_available_capacity() const {
        std::lock_guard lock(mutex_);
        return max_capacity_ - current_used_;
    }

    // Set the maximum capacity (can be changed at runtime)
    void set_max_capacity(size_t capacity) {
        std::lock_guard lock(mutex_);
        max_capacity_ = capacity;
        // If we now have available capacity, notify waiters
        if (current_used_ < max_capacity_) {
            cv_.notify_all();
        }
    }

    // Reserve capacity (called when producing an item)
    bool try_reserve() {
        std::lock_guard lock(mutex_);
        if (current_used_ < max_capacity_) {
            ++current_used_;
            return true;
        }
        return false;
    }

private:
    size_t max_capacity_;
    size_t current_used_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace cc::bridge
