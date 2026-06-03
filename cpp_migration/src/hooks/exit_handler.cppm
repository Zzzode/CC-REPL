// C++23 Module: Graceful exit handling with Ctrl+C/Ctrl+D confirmation flow
module;

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.hooks.exit_handler;


export namespace cc::hooks {


enum class ExitReason {
    ctrl_c,
    ctrl_d,
    command,
    error,
    signal,
};


struct ExitHandlerConfig {
    bool require_double_press{true};
    std::uint32_t cleanup_timeout_ms{5000};
    bool save_on_exit{true};
    std::chrono::milliseconds double_press_window{1500};
};


using CleanupCallback = std::function<bool()>;

using ExitCallback = std::function<void(ExitReason)>;


class ExitHandler {
    using Clock = std::chrono::steady_clock;
public:
    explicit ExitHandler(ExitHandlerConfig config = {})
        : config_(std::move(config)) {}

    /**
     * Handle signal and key events. Returns true when the app should exit.
     * Ctrl+C shows a prompt first and exits on the second press.
     * Ctrl+D exits immediately when the input buffer is empty.
     */
    [[nodiscard]] auto handle_signal(ExitReason reason) -> bool {
        std::lock_guard lock{mu_};

        switch (reason) {
            case ExitReason::ctrl_d:

                initiate_exit(reason);
                return true;

            case ExitReason::ctrl_c:
                return handle_ctrl_c();

            case ExitReason::command:
            case ExitReason::error:
            case ExitReason::signal:

                initiate_exit(reason);
                return true;
        }
        return false;
    }


    auto register_cleanup(CleanupCallback callback) -> void {
        std::lock_guard lock{mu_};
        cleanup_callbacks_.push_back(std::move(callback));
    }


    auto set_exit_message(std::string msg) -> void {
        std::lock_guard lock{mu_};
        exit_message_ = std::move(msg);
    }


    [[nodiscard]] auto is_exit_pending() const -> bool {
        std::lock_guard lock{mu_};
        return exit_pending_;
    }


    auto reset() -> void {
        std::lock_guard lock{mu_};
        exit_pending_ = false;
        first_press_time_ = std::nullopt;
    }


    auto force_exit(ExitReason reason) -> void {
        std::lock_guard lock{mu_};
        initiate_exit(reason);
    }


    auto on_exit(ExitCallback callback) -> void {
        std::lock_guard lock{mu_};
        exit_callbacks_.push_back(std::move(callback));
    }


    [[nodiscard]] auto exit_message() const -> std::string_view {
        std::lock_guard lock{mu_};
        return exit_message_;
    }


    [[nodiscard]] auto config() const -> const ExitHandlerConfig& { return config_; }


    auto set_config(ExitHandlerConfig config) -> void {
        std::lock_guard lock{mu_};
        config_ = std::move(config);
    }


    [[nodiscard]] auto run_cleanup() -> std::size_t {
        std::lock_guard lock{mu_};
        std::size_t failures = 0;

        for (auto it = cleanup_callbacks_.rbegin(); it != cleanup_callbacks_.rend(); ++it) {
            if (!(*it)()) {
                ++failures;
            }
        }
        return failures;
    }

private:
    ExitHandlerConfig config_;
    mutable std::mutex mu_;
    bool exit_pending_{false};
    std::optional<Clock::time_point> first_press_time_;
    std::string exit_message_{"Press Ctrl+C again to exit"};
    std::vector<CleanupCallback> cleanup_callbacks_;
    std::vector<ExitCallback> exit_callbacks_;


    [[nodiscard]] auto handle_ctrl_c() -> bool {
        if (!config_.require_double_press) {
            initiate_exit(ExitReason::ctrl_c);
            return true;
        }

        auto now = Clock::now();
        if (exit_pending_ && first_press_time_) {

            auto elapsed = now - *first_press_time_;
            if (elapsed <= config_.double_press_window) {
                initiate_exit(ExitReason::ctrl_c);
                return true;
            }
        }


        exit_pending_ = true;
        first_press_time_ = now;
        return false;
    }


    auto initiate_exit(ExitReason reason) -> void {
        for (const auto& cb : exit_callbacks_) {
            cb(reason);
        }
        exit_pending_ = false;
    }
};


[[nodiscard]] inline auto create_exit_handler() -> ExitHandler {
    return ExitHandler(ExitHandlerConfig{
        .require_double_press = true,
        .cleanup_timeout_ms = 5000,
        .save_on_exit = true,
        .double_press_window = std::chrono::milliseconds(1500)
    });
}

} // namespace cc::hooks
