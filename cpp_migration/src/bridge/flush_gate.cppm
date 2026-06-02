module;
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

export module cc.bridge.flush_gate;

export namespace cc::bridge {

// FlushGate provides a backpressure mechanism for message flow control.
// When closed, producers must wait until the gate opens before sending more data.
class FlushGate {
public:
    FlushGate() = default;

    // Open the gate, allowing waiting producers to proceed
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_.store(true);
        }
        cv_.notify_all();
    }

    // Close the gate, blocking subsequent wait_until_open() calls
    void close() {
        std::lock_guard lock(mutex_);
        open_.store(false);
    }

    // Wait until the gate is opened, with a configurable timeout.
    // Returns true if the gate opened, false on timeout.
    bool wait_until_open(std::chrono::seconds timeout = std::chrono::seconds{30}) {
        if (open_.load()) return true;

        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return open_.load();
        });
    }

    // Check if the gate is currently open (non-blocking)
    bool is_open() const {
        return open_.load();
    }

private:
    std::atomic<bool> open_{true};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace cc::bridge
