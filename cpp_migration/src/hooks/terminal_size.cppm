// C++23 Module: Terminal size monitoring with SIGWINCH subscription and debounced resize events
module;

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

export module cc.hooks.terminal_size;

export namespace cc::hooks {


struct TerminalDimensions {
    std::uint16_t cols{80};
    std::uint16_t rows{24};

    [[nodiscard]] auto area() const -> std::uint32_t {
        return static_cast<std::uint32_t>(cols) * rows;
    }

    auto operator==(const TerminalDimensions&) const -> bool = default;
    auto operator!=(const TerminalDimensions&) const -> bool = default;
};


struct ResizeEvent {
    TerminalDimensions old_dims;
    TerminalDimensions new_dims;
    std::chrono::steady_clock::time_point timestamp;


    [[nodiscard]] auto changed() const -> bool { return old_dims != new_dims; }
};


using UnsubscribeFn = std::function<void()>;

using ResizeCallback = std::function<void(const ResizeEvent&)>;


class TerminalSizeHook {
    using Clock = std::chrono::steady_clock;
public:
    explicit TerminalSizeHook(std::chrono::milliseconds debounce = std::chrono::milliseconds(50))
        : debounce_delay_(debounce) {

        current_ = query_terminal_size();
    }

    ~TerminalSizeHook() { stop_monitoring(); }


    [[nodiscard]] auto get_size() -> TerminalDimensions {
        if (s_resize_flag_.load(std::memory_order_relaxed)) {
            s_resize_flag_.store(false, std::memory_order_relaxed);
            handle_resize_signal();
        }
        std::lock_guard lock{mu_};
        return current_;
    }


    [[nodiscard]] auto on_resize(ResizeCallback callback) -> UnsubscribeFn {
        std::lock_guard lock{mu_};
        auto id = next_id_++;
        subscribers_.push_back({.id = id, .callback = std::move(callback)});

        return [this, id]() {
            std::lock_guard inner_lock{mu_};
            std::erase_if(subscribers_, [id](const auto& s) { return s.id == id; });
        };
    }


    [[nodiscard]] auto is_too_small(std::uint16_t min_cols = 40,
                                     std::uint16_t min_rows = 10) -> bool {
        auto dims = get_size();
        return dims.cols < min_cols || dims.rows < min_rows;
    }


    auto start_monitoring() -> void {
        std::lock_guard lock{mu_};
        if (monitoring_) return;
        monitoring_ = true;

        // Install SIGWINCH handler that sets atomic flag
        struct sigaction sa{};
        sa.sa_handler = [](int) {
            s_resize_flag_.store(true, std::memory_order_relaxed);
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &sa, nullptr);
    }


    auto stop_monitoring() -> void {
        std::lock_guard lock{mu_};
        if (!monitoring_) return;
        monitoring_ = false;

        // Restore default signal disposition
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGWINCH, &sa, nullptr);
    }


    auto handle_resize_signal() -> void {
        auto new_dims = query_terminal_size();
        std::lock_guard lock{mu_};

        if (new_dims == current_) return;

        auto now = Clock::now();

        if (last_resize_time_ && (now - *last_resize_time_) < debounce_delay_) {
            pending_dims_ = new_dims;
            return;
        }

        emit_resize(new_dims, now);
    }


    auto flush_pending() -> void {
        std::lock_guard lock{mu_};
        if (pending_dims_) {
            emit_resize(*pending_dims_, Clock::now());
            pending_dims_ = std::nullopt;
        }
    }

    [[nodiscard]] auto is_monitoring() const -> bool {
        std::lock_guard lock{mu_};
        return monitoring_;
    }

private:
    struct Subscriber {
        std::uint64_t id;
        ResizeCallback callback;
    };

    // Async-signal-safe flag set by SIGWINCH handler
    static inline std::atomic<bool> s_resize_flag_{false};

    std::chrono::milliseconds debounce_delay_;
    mutable std::mutex mu_;
    TerminalDimensions current_;
    std::optional<TerminalDimensions> pending_dims_;
    std::optional<Clock::time_point> last_resize_time_;
    std::vector<Subscriber> subscribers_;
    std::uint64_t next_id_{1};
    bool monitoring_{false};


    [[nodiscard]] static auto query_terminal_size() -> TerminalDimensions {
        TerminalDimensions dims;
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            dims.cols = ws.ws_col;
            dims.rows = ws.ws_row;
        }
        return dims;
    }


    auto emit_resize(TerminalDimensions new_dims, Clock::time_point now) -> void {
        ResizeEvent event{
            .old_dims = current_,
            .new_dims = new_dims,
            .timestamp = now
        };
        current_ = new_dims;
        last_resize_time_ = now;

        for (const auto& sub : subscribers_) {
            sub.callback(event);
        }
    }
};

} // namespace cc::hooks
