// SleepTool - Proactive mode waiting with cancellation support
module;
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

export module cc.tools.sleep;


export namespace cc::tools {

// Sleep 操作错误类型
enum class SleepError {
    InvalidDuration,
    DurationTooLong,
    Cancelled,
    ReasonEmpty,
};

constexpr auto format_error(SleepError err) -> std::string_view {
    switch (err) {
        case SleepError::InvalidDuration:  return "Invalid sleep duration (must be > 0)";
        case SleepError::DurationTooLong:  return "Sleep duration exceeds maximum allowed";
        case SleepError::Cancelled:        return "Sleep was cancelled by abort signal";
        case SleepError::ReasonEmpty:      return "Sleep reason must be specified";
        default:                           return "Unknown sleep error";
    }
}

// Sleep 请求参数
struct SleepRequest {
    std::chrono::seconds duration;          // 等待时长
    std::string reason;                     // 等待原因 (用于日志追踪)
    std::optional<std::string> resume_hint; // 恢复后的执行提示
};

// Sleep 结果
struct SleepResult {
    std::chrono::milliseconds actual_duration{0};
    bool was_cancelled{false};
    std::string reason;
};

// 取消信号：用于从外部中断 sleep
class AbortSignal {
public:
    // 发送取消信号
    void abort() {
        std::lock_guard lock(mutex_);
        aborted_ = true;
        cv_.notify_all();
    }

    // 检查是否已取消
    [[nodiscard]] bool is_aborted() const {
        std::lock_guard lock(mutex_);
        return aborted_;
    }

    // 等待指定时间或取消信号
    auto wait_for(std::chrono::milliseconds duration) -> bool {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, duration, [this] { return aborted_; });
    }

    // 重置信号状态
    void reset() {
        std::lock_guard lock(mutex_);
        aborted_ = false;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool aborted_{false};
};

// SleepTool - 主动模式等待工具
class SleepTool {
public:
    static constexpr std::string_view name = "sleep";
    static constexpr std::string_view description = "Wait for a specified duration in proactive mode";
    static constexpr std::chrono::seconds kMaxDuration{300};  // 最大等待 5 分钟
    static constexpr std::chrono::seconds kMinDuration{1};    // 最小等待 1 秒

    explicit SleepTool(std::shared_ptr<AbortSignal> signal = nullptr)
        : abort_signal_(std::move(signal)) {}

    auto validate(const SleepRequest& request) const -> std::expected<void, SleepError> {
        if (request.duration < kMinDuration) {
            return std::unexpected(SleepError::InvalidDuration);
        }
        if (request.duration > kMaxDuration) {
            return std::unexpected(SleepError::DurationTooLong);
        }
        if (request.reason.empty()) {
            return std::unexpected(SleepError::ReasonEmpty);
        }
        return {};
    }

    auto execute(SleepRequest request) -> std::expected<SleepResult, SleepError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        auto start = std::chrono::steady_clock::now();
        auto target_duration = std::chrono::duration_cast<std::chrono::milliseconds>(request.duration);
        bool cancelled = false;

        if (abort_signal_) {
            // 使用可取消的等待
            cancelled = abort_signal_->wait_for(target_duration);
        } else {
            // 分段 sleep 以支持优雅中断
            auto remaining = target_duration;
            constexpr auto check_interval = std::chrono::milliseconds{100};

            while (remaining > std::chrono::milliseconds::zero()) {
                auto sleep_time = std::min(remaining, check_interval);
                std::this_thread::sleep_for(sleep_time);
                remaining -= sleep_time;
            }
        }

        auto actual = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        if (cancelled) {
            return SleepResult{
                .actual_duration = actual,
                .was_cancelled = true,
                .reason = request.reason,
            };
        }

        return SleepResult{
            .actual_duration = actual,
            .was_cancelled = false,
            .reason = request.reason,
        };
    }

    // 从外部取消当前 sleep
    void cancel() {
        if (abort_signal_) {
            abort_signal_->abort();
        }
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "duration": {{ "type": "integer", "description": "Duration to sleep in seconds (1-300)" }},
      "reason": {{ "type": "string", "description": "Reason for waiting (logged for tracking)" }},
      "resume_hint": {{ "type": "string", "description": "Hint for what to do after waking" }}
    }},
    "required": ["duration", "reason"]
  }}
}})json", name, description);
    }

private:
    std::shared_ptr<AbortSignal> abort_signal_;
};

} // namespace cc::tools
