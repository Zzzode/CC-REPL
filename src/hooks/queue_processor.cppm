module;
#include <string>
#include <functional>
#include <optional>
#include <vector>
#include <memory>
#include <chrono>

export module cc.hooks.queue_processor;

import cc.state.app_state;

export namespace cc::hooks::queue_processor {

enum class QueueItemStatus { pending, processing, completed, failed };

struct QueueItem {
    std::string id;
    std::string payload;
    QueueItemStatus status{QueueItemStatus::pending};
    int retry_count{0};
    std::optional<std::string> error;
};

struct QueueProcessorState {
    std::vector<QueueItem> items;
    bool processing{false};
    std::size_t processed_count{0};
    std::size_t failed_count{0};
};

struct QueueProcessorOptions {
    std::size_t max_retries{3};
    std::size_t concurrency{1};
    bool auto_start{true};
};

class QueueProcessorHook {
public:
    explicit QueueProcessorHook(const QueueProcessorOptions& opts = {})
        : options_(opts) {}

    void activate() { active_ = true; }
    void deactivate() { active_ = false; }

    [[nodiscard]] const QueueProcessorState& state() const { return state_; }

    void enqueue(QueueItem item) {
        state_.items.push_back(std::move(item));
        notify();
    }

    /// Process next pending item. Returns the item being processed.
    [[nodiscard]] std::optional<QueueItem> process_next() {
        for (auto& item : state_.items) {
            if (item.status == QueueItemStatus::pending) {
                item.status = QueueItemStatus::processing;
                state_.processing = true;
                notify();
                return item;
            }
        }
        return std::nullopt;
    }

    void mark_completed(std::string_view id) {
        for (auto& item : state_.items) {
            if (item.id == id) {
                item.status = QueueItemStatus::completed;
                ++state_.processed_count;
                break;
            }
        }
        update_processing_state();
        notify();
    }

    void mark_failed(std::string_view id, std::string error) {
        for (auto& item : state_.items) {
            if (item.id == id) {
                ++item.retry_count;
                if (static_cast<std::size_t>(item.retry_count) >= options_.max_retries) {
                    item.status = QueueItemStatus::failed;
                    item.error = std::move(error);
                    ++state_.failed_count;
                } else {
                    item.status = QueueItemStatus::pending; // Retry
                }
                break;
            }
        }
        update_processing_state();
        notify();
    }

    void clear_completed() {
        std::erase_if(state_.items, [](const auto& i) {
            return i.status == QueueItemStatus::completed;
        });
        notify();
    }

    [[nodiscard]] std::size_t pending_count() const {
        std::size_t count = 0;
        for (const auto& item : state_.items) {
            if (item.status == QueueItemStatus::pending) ++count;
        }
        return count;
    }

    void on_change(std::function<void(const QueueProcessorState&)> callback) {
        listeners_.push_back(std::move(callback));
    }

private:
    void update_processing_state() {
        state_.processing = false;
        for (const auto& item : state_.items) {
            if (item.status == QueueItemStatus::processing) {
                state_.processing = true;
                break;
            }
        }
    }

    void notify() {
        for (const auto& cb : listeners_) cb(state_);
    }

    QueueProcessorState state_;
    QueueProcessorOptions options_;
    std::vector<std::function<void(const QueueProcessorState&)>> listeners_;
    bool active_{false};
};

} // namespace cc::hooks::queue_processor
