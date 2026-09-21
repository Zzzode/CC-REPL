// C++23 Module: Command queue for serialized execution with priority and deduplication
module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.command_queue;


export namespace cc::hooks {


enum class QueuePriority {
    low,
    normal,
    high,
    system,
};


struct QueuedCommand {
    std::string id;
    std::string command_text;
    QueuePriority priority{QueuePriority::normal};
    std::chrono::steady_clock::time_point enqueued_at;
    std::uint32_t timeout_ms{30000};


    [[nodiscard]] auto is_expired() const -> bool {
        auto elapsed = std::chrono::steady_clock::now() - enqueued_at;
        return elapsed > std::chrono::milliseconds(timeout_ms);
    }
};


struct QueueState {
    std::size_t pending{0};
    std::optional<QueuedCommand> current;
    std::size_t completed_count{0};
    std::size_t cancelled_count{0};
};


struct CommandCompleteEvent {
    std::string id;
    bool success{true};
    std::string error_message;
    std::chrono::milliseconds duration;
};


using CommandCompleteCallback = std::function<void(const CommandCompleteEvent&)>;

using CommandExecutor = std::function<std::expected<void, std::string>(const QueuedCommand&)>;


class CommandQueue {
    using Clock = std::chrono::steady_clock;
public:
    CommandQueue() = default;

    /**
     * Enqueue a command and return its command ID.
     * System-priority commands are inserted at the front.
     */
    [[nodiscard]] auto enqueue(std::string_view command_text,
                                QueuePriority priority = QueuePriority::normal,
                                std::uint32_t timeout_ms = 30000) -> std::string {
        std::lock_guard lock{mu_};

        auto id = generate_id();
        QueuedCommand cmd{
            .id = id,
            .command_text = std::string(command_text),
            .priority = priority,
            .enqueued_at = Clock::now(),
            .timeout_ms = timeout_ms
        };


        if (is_duplicate(cmd)) {
            return id;
        }


        if (priority == QueuePriority::system) {
            queue_.push_front(std::move(cmd));
        } else {
            auto insert_pos = find_insert_position(priority);
            queue_.insert(insert_pos, std::move(cmd));
        }

        return id;
    }


    [[nodiscard]] auto cancel(std::string_view id) -> bool {
        std::lock_guard lock{mu_};


        if (current_ && current_->id == id) {
            cancel_requested_ = true;
            return true;
        }


        auto it = std::ranges::find_if(queue_,
            [id](const auto& cmd) { return cmd.id == id; });
        if (it != queue_.end()) {
            queue_.erase(it);
            cancelled_count_++;
            return true;
        }
        return false;
    }


    auto cancel_all() -> void {
        std::lock_guard lock{mu_};
        cancelled_count_ += queue_.size();
        queue_.clear();
        if (current_) {
            cancel_requested_ = true;
        }
    }


    [[nodiscard]] auto get_current() const -> std::optional<QueuedCommand> {
        std::lock_guard lock{mu_};
        return current_;
    }


    [[nodiscard]] auto get_pending_count() const -> std::size_t {
        std::lock_guard lock{mu_};
        return queue_.size();
    }


    [[nodiscard]] auto process_next(CommandExecutor executor)
        -> std::expected<void, std::string> {
        QueuedCommand cmd;
        {
            std::lock_guard lock{mu_};

            purge_expired();

            if (queue_.empty()) return {};
            if (current_) {
                return std::unexpected("A command is already executing");
            }

            cmd = std::move(queue_.front());
            queue_.pop_front();
            current_ = cmd;
            cancel_requested_ = false;
        }

        auto start_time = Clock::now();
        auto result = executor(cmd);
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start_time);

        {
            std::lock_guard lock{mu_};
            current_ = std::nullopt;
            completed_count_++;
        }


        CommandCompleteEvent event{
            .id = cmd.id,
            .success = result.has_value(),
            .error_message = result.has_value() ? "" : result.error(),
            .duration = duration
        };
        notify_complete(event);

        return result;
    }


    [[nodiscard]] auto is_idle() const -> bool {
        std::lock_guard lock{mu_};
        return !current_ && queue_.empty();
    }


    auto on_command_complete(CommandCompleteCallback callback) -> void {
        std::lock_guard lock{mu_};
        complete_callbacks_.push_back(std::move(callback));
    }


    [[nodiscard]] auto state() const -> QueueState {
        std::lock_guard lock{mu_};
        return QueueState{
            .pending = queue_.size(),
            .current = current_,
            .completed_count = completed_count_,
            .cancelled_count = cancelled_count_
        };
    }


    [[nodiscard]] auto is_cancel_requested() const -> bool {
        std::lock_guard lock{mu_};
        return cancel_requested_;
    }


    auto set_dedup_window(std::chrono::milliseconds window) -> void {
        std::lock_guard lock{mu_};
        dedup_window_ = window;
    }

private:
    mutable std::mutex mu_;
    std::deque<QueuedCommand> queue_;
    std::optional<QueuedCommand> current_;
    std::vector<CommandCompleteCallback> complete_callbacks_;
    std::size_t completed_count_{0};
    std::size_t cancelled_count_{0};
    std::uint64_t id_counter_{0};
    bool cancel_requested_{false};
    std::chrono::milliseconds dedup_window_{500};


    [[nodiscard]] auto generate_id() -> std::string {
        return std::format("cmd_{}", ++id_counter_);
    }


    [[nodiscard]] auto is_duplicate(const QueuedCommand& cmd) const -> bool {
        auto now = Clock::now();
        for (const auto& existing : queue_) {
            if (existing.command_text == cmd.command_text) {
                auto age = now - existing.enqueued_at;
                if (age < dedup_window_) return true;
            }
        }
        return false;
    }


    [[nodiscard]] auto find_insert_position(QueuePriority priority)
        -> std::deque<QueuedCommand>::iterator {

        auto it = queue_.end();
        for (auto cur = queue_.begin(); cur != queue_.end(); ++cur) {
            if (static_cast<int>(cur->priority) < static_cast<int>(priority)) {
                it = cur;
                break;
            }
        }
        return it;
    }


    auto purge_expired() -> void {
        std::erase_if(queue_, [this](const QueuedCommand& cmd) {
            if (cmd.is_expired()) { cancelled_count_++; return true; }
            return false;
        });
    }


    auto notify_complete(const CommandCompleteEvent& event) -> void {
        for (const auto& cb : complete_callbacks_) {
            cb(event);
        }
    }
};

} // namespace cc::hooks
