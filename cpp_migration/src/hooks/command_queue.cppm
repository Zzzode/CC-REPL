// C++23 Module: Command queue for serialized execution with priority and deduplication
module;

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

// 命令优先级
enum class QueuePriority {
    low,       // 后台任务
    normal,    // 用户常规命令
    high,      // 高优先级命令（如自动保存）
    system,    // 系统级命令（如退出清理），始终插队
};

// 队列中的命令项
struct QueuedCommand {
    std::string id;                           // 唯一标识
    std::string command_text;                 // 命令内容
    QueuePriority priority{QueuePriority::normal};
    std::chrono::steady_clock::time_point enqueued_at;
    std::uint32_t timeout_ms{30000};          // 执行超时（默认 30 秒）

    // 是否已超时
    [[nodiscard]] auto is_expired() const -> bool {
        auto elapsed = std::chrono::steady_clock::now() - enqueued_at;
        return elapsed > std::chrono::milliseconds(timeout_ms);
    }
};

// 队列状态摘要
struct QueueState {
    std::size_t pending{0};                   // 等待执行的命令数
    std::optional<QueuedCommand> current;     // 当前正在执行的命令
    std::size_t completed_count{0};           // 已完成的命令总数
    std::size_t cancelled_count{0};           // 已取消的命令总数
};

// 命令完成事件
struct CommandCompleteEvent {
    std::string id;
    bool success{true};
    std::string error_message;
    std::chrono::milliseconds duration;
};

// 命令完成回调
using CommandCompleteCallback = std::function<void(const CommandCompleteEvent&)>;
// 命令执行器类型（协程友好，返回 expected）
using CommandExecutor = std::function<std::expected<void, std::string>(const QueuedCommand&)>;

// CommandQueue: FIFO 命令队列，支持优先级、去重和取消
class CommandQueue {
    using Clock = std::chrono::steady_clock;
public:
    CommandQueue() = default;

    /**
     * 将命令加入队列，返回命令 ID。
     * system 优先级的命令会插入队列前端。
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

        // 去重检查：同一命令在窗口期内不重复入队
        if (is_duplicate(cmd)) {
            return id; // 返回 ID 但不实际入队
        }

        // 按优先级插入
        if (priority == QueuePriority::system) {
            queue_.push_front(std::move(cmd));
        } else {
            auto insert_pos = find_insert_position(priority);
            queue_.insert(insert_pos, std::move(cmd));
        }

        return id;
    }

    // 取消指定命令
    [[nodiscard]] auto cancel(std::string_view id) -> bool {
        std::lock_guard lock{mu_};

        // 如果是当前正在执行的命令
        if (current_ && current_->id == id) {
            cancel_requested_ = true;
            return true;
        }

        // 从队列中移除
        auto it = std::ranges::find_if(queue_,
            [id](const auto& cmd) { return cmd.id == id; });
        if (it != queue_.end()) {
            queue_.erase(it);
            cancelled_count_++;
            return true;
        }
        return false;
    }

    // 取消所有等待中的命令并中断当前命令
    auto cancel_all() -> void {
        std::lock_guard lock{mu_};
        cancelled_count_ += queue_.size();
        queue_.clear();
        if (current_) {
            cancel_requested_ = true;
        }
    }

    // 获取当前正在执行的命令
    [[nodiscard]] auto get_current() const -> std::optional<QueuedCommand> {
        std::lock_guard lock{mu_};
        return current_;
    }

    // 获取等待执行的命令数
    [[nodiscard]] auto get_pending_count() const -> std::size_t {
        std::lock_guard lock{mu_};
        return queue_.size();
    }

    // 处理队列中的下一条命令
    [[nodiscard]] auto process_next(CommandExecutor executor)
        -> std::expected<void, std::string> {
        QueuedCommand cmd;
        {
            std::lock_guard lock{mu_};
            // 清除过期命令
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

        // 通知完成
        CommandCompleteEvent event{
            .id = cmd.id,
            .success = result.has_value(),
            .error_message = result.has_value() ? "" : result.error(),
            .duration = duration
        };
        notify_complete(event);

        return result;
    }

    // 队列是否空闲（无执行中和等待中的命令）
    [[nodiscard]] auto is_idle() const -> bool {
        std::lock_guard lock{mu_};
        return !current_ && queue_.empty();
    }

    // 注册命令完成回调
    auto on_command_complete(CommandCompleteCallback callback) -> void {
        std::lock_guard lock{mu_};
        complete_callbacks_.push_back(std::move(callback));
    }

    // 获取队列状态
    [[nodiscard]] auto state() const -> QueueState {
        std::lock_guard lock{mu_};
        return QueueState{
            .pending = queue_.size(),
            .current = current_,
            .completed_count = completed_count_,
            .cancelled_count = cancelled_count_
        };
    }

    // 检查取消是否已被请求（供执行器内部轮询）
    [[nodiscard]] auto is_cancel_requested() const -> bool {
        std::lock_guard lock{mu_};
        return cancel_requested_;
    }

    // 设置去重窗口
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
    std::chrono::milliseconds dedup_window_{500}; // 去重窗口

    // 生成唯一命令 ID
    [[nodiscard]] auto generate_id() -> std::string {
        return std::format("cmd_{}", ++id_counter_);
    }

    // 检查命令是否重复（同一文本在窗口期内）
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

    // 找到优先级对应的插入位置
    [[nodiscard]] auto find_insert_position(QueuePriority priority)
        -> std::deque<QueuedCommand>::iterator {
        // 在同优先级的最后一个位置之后插入
        auto it = queue_.end();
        for (auto cur = queue_.begin(); cur != queue_.end(); ++cur) {
            if (static_cast<int>(cur->priority) < static_cast<int>(priority)) {
                it = cur;
                break;
            }
        }
        return it;
    }

    // 清除队列中过期的命令
    auto purge_expired() -> void {
        std::erase_if(queue_, [this](const QueuedCommand& cmd) {
            if (cmd.is_expired()) { cancelled_count_++; return true; }
            return false;
        });
    }

    // 通知所有完成回调
    auto notify_complete(const CommandCompleteEvent& event) -> void {
        for (const auto& cb : complete_callbacks_) {
            cb(event);
        }
    }
};

} // namespace cc::hooks
