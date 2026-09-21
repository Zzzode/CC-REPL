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


struct SleepRequest {
    std::chrono::seconds duration;
    std::string reason;
    std::optional<std::string> resume_hint;
};


struct SleepResult {
    std::chrono::milliseconds actual_duration{0};
    bool was_cancelled{false};
    std::string reason;
};


class AbortSignal {
public:

    void abort() {
        std::lock_guard lock(mutex_);
        aborted_ = true;
        cv_.notify_all();
    }


    [[nodiscard]] bool is_aborted() const {
        std::lock_guard lock(mutex_);
        return aborted_;
    }


    auto wait_for(std::chrono::milliseconds duration) -> bool {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, duration, [this] { return aborted_; });
    }


    void reset() {
        std::lock_guard lock(mutex_);
        aborted_ = false;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool aborted_{false};
};


class SleepTool {
public:
    static constexpr std::string_view name = "sleep";
    static constexpr std::string_view description = "Wait for a specified duration in proactive mode";
    static constexpr std::chrono::seconds kMaxDuration{300};
    static constexpr std::chrono::seconds kMinDuration{1};

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

            cancelled = abort_signal_->wait_for(target_duration);
        } else {

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
