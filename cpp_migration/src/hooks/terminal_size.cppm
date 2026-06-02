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

// 终端尺寸
struct TerminalDimensions {
    std::uint16_t cols{80};   // 列数
    std::uint16_t rows{24};   // 行数

    [[nodiscard]] auto area() const -> std::uint32_t {
        return static_cast<std::uint32_t>(cols) * rows;
    }

    auto operator==(const TerminalDimensions&) const -> bool = default;
    auto operator!=(const TerminalDimensions&) const -> bool = default;
};

// 尺寸变化事件
struct ResizeEvent {
    TerminalDimensions old_dims;
    TerminalDimensions new_dims;
    std::chrono::steady_clock::time_point timestamp;

    // 尺寸是否实际发生了变化
    [[nodiscard]] auto changed() const -> bool { return old_dims != new_dims; }
};

// 取消订阅回调类型
using UnsubscribeFn = std::function<void()>;
// resize 回调类型
using ResizeCallback = std::function<void(const ResizeEvent&)>;

// TerminalSizeHook: 监控终端尺寸并在变化时发出通知
class TerminalSizeHook {
    using Clock = std::chrono::steady_clock;
public:
    explicit TerminalSizeHook(std::chrono::milliseconds debounce = std::chrono::milliseconds(50))
        : debounce_delay_(debounce) {
        // 初始化时读取当前终端尺寸
        current_ = query_terminal_size();
    }

    ~TerminalSizeHook() { stop_monitoring(); }

    // 获取当前终端尺寸 (checks SIGWINCH flag and re-queries if needed)
    [[nodiscard]] auto get_size() -> TerminalDimensions {
        if (s_resize_flag_.load(std::memory_order_relaxed)) {
            s_resize_flag_.store(false, std::memory_order_relaxed);
            handle_resize_signal();
        }
        std::lock_guard lock{mu_};
        return current_;
    }

    // 订阅 resize 事件，返回取消订阅函数
    [[nodiscard]] auto on_resize(ResizeCallback callback) -> UnsubscribeFn {
        std::lock_guard lock{mu_};
        auto id = next_id_++;
        subscribers_.push_back({.id = id, .callback = std::move(callback)});

        return [this, id]() {
            std::lock_guard inner_lock{mu_};
            std::erase_if(subscribers_, [id](const auto& s) { return s.id == id; });
        };
    }

    // 判断终端是否太小，无法正常显示 UI
    [[nodiscard]] auto is_too_small(std::uint16_t min_cols = 40,
                                     std::uint16_t min_rows = 10) -> bool {
        auto dims = get_size();
        return dims.cols < min_cols || dims.rows < min_rows;
    }

    // 启动 SIGWINCH 信号监控
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

    // 停止监控 — restore SIG_DFL
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

    // 手动触发尺寸检查（供外部或内部调用）
    auto handle_resize_signal() -> void {
        auto new_dims = query_terminal_size();
        std::lock_guard lock{mu_};

        if (new_dims == current_) return; // 尺寸未变

        auto now = Clock::now();
        // 防抖：忽略在窗口期内的重复事件
        if (last_resize_time_ && (now - *last_resize_time_) < debounce_delay_) {
            pending_dims_ = new_dims;
            return;
        }

        emit_resize(new_dims, now);
    }

    // 刷新待处理的防抖事件（由定时器回调调用）
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

    // 从系统查询终端实际尺寸
    [[nodiscard]] static auto query_terminal_size() -> TerminalDimensions {
        TerminalDimensions dims;
        struct winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            dims.cols = ws.ws_col;
            dims.rows = ws.ws_row;
        }
        return dims;
    }

    // 发出 resize 事件并更新状态
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
