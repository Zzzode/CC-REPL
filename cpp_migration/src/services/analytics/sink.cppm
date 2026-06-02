module;
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.analytics.sink;

export namespace cc::services::analytics {

// Analytics event sink for batched event delivery
class AnalyticsSink {
public:
    AnalyticsSink() = default;

    // Send an analytics event
    auto send(std::string_view event_type, std::map<std::string, std::string> data) -> void {
        std::lock_guard lock(mutex_);
        buffer_.push_back({std::string(event_type), std::move(data)});
        // Auto-flush if batch size reached
        if (buffer_.size() >= batch_size_) {
            flush_impl();
        }
    }

    // Flush all buffered events
    auto flush() -> void {
        std::lock_guard lock(mutex_);
        flush_impl();
    }

    // Configure batch size before auto-flush
    auto set_batch_size(size_t size) -> void {
        batch_size_ = size;
    }

    // Configure flush interval
    auto set_flush_interval(std::chrono::seconds interval) -> void {
        flush_interval_ = interval;
    }

private:
    struct Event {
        std::string type;
        std::map<std::string, std::string> data;
    };

    // Internal flush without lock
    auto flush_impl() -> void {
        // Migration sink accepts events locally and drops the flushed batch.
        buffer_.clear();
    }

    std::mutex mutex_;
    std::vector<Event> buffer_;
    size_t batch_size_{50};
    std::chrono::seconds flush_interval_{30};
};

} // namespace cc::services::analytics
