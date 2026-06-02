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

// 退出原因分类
enum class ExitReason {
    ctrl_c,    // 用户按下 Ctrl+C
    ctrl_d,    // 用户按下 Ctrl+D（空输入时）
    command,   // 用户执行退出命令（/exit, /quit）
    error,     // 致命错误导致退出
    signal,    // 系统信号（SIGTERM 等）
};

// 退出处理器配置
struct ExitHandlerConfig {
    bool require_double_press{true};          // 是否需要连续两次 Ctrl+C 才退出
    std::uint32_t cleanup_timeout_ms{5000};   // 清理回调的超时时间
    bool save_on_exit{true};                  // 退出前是否保存会话状态
    std::chrono::milliseconds double_press_window{1500}; // 两次按键的有效窗口
};

// 清理回调类型（返回 true 表示清理成功）
using CleanupCallback = std::function<bool()>;
// 退出回调类型
using ExitCallback = std::function<void(ExitReason)>;

// ExitHandler: 管理优雅退出流程
class ExitHandler {
    using Clock = std::chrono::steady_clock;
public:
    explicit ExitHandler(ExitHandlerConfig config = {})
        : config_(std::move(config)) {}

    /**
     * 处理信号/按键事件，返回 true 表示应该立即退出。
     * 对于 Ctrl+C：首次显示提示，第二次退出；
     * 对于 Ctrl+D：如果输入为空则直接退出。
     */
    [[nodiscard]] auto handle_signal(ExitReason reason) -> bool {
        std::lock_guard lock{mu_};

        switch (reason) {
            case ExitReason::ctrl_d:
                // Ctrl+D 在空输入时立即退出
                initiate_exit(reason);
                return true;

            case ExitReason::ctrl_c:
                return handle_ctrl_c();

            case ExitReason::command:
            case ExitReason::error:
            case ExitReason::signal:
                // 命令/错误/系统信号直接退出
                initiate_exit(reason);
                return true;
        }
        return false;
    }

    // 注册退出前执行的清理回调（LIFO 顺序执行）
    auto register_cleanup(CleanupCallback callback) -> void {
        std::lock_guard lock{mu_};
        cleanup_callbacks_.push_back(std::move(callback));
    }

    // 设置退出提示信息（"再按一次退出"）
    auto set_exit_message(std::string msg) -> void {
        std::lock_guard lock{mu_};
        exit_message_ = std::move(msg);
    }

    // 检查是否处于"等待第二次确认"状态
    [[nodiscard]] auto is_exit_pending() const -> bool {
        std::lock_guard lock{mu_};
        return exit_pending_;
    }

    // 重置退出状态（用于取消待定的退出确认）
    auto reset() -> void {
        std::lock_guard lock{mu_};
        exit_pending_ = false;
        first_press_time_ = std::nullopt;
    }

    // 强制立即退出，跳过确认
    auto force_exit(ExitReason reason) -> void {
        std::lock_guard lock{mu_};
        initiate_exit(reason);
    }

    // 注册退出事件回调（退出流程开始后调用）
    auto on_exit(ExitCallback callback) -> void {
        std::lock_guard lock{mu_};
        exit_callbacks_.push_back(std::move(callback));
    }

    // 获取当前退出提示消息
    [[nodiscard]] auto exit_message() const -> std::string_view {
        std::lock_guard lock{mu_};
        return exit_message_;
    }

    // 获取配置
    [[nodiscard]] auto config() const -> const ExitHandlerConfig& { return config_; }

    // 修改配置
    auto set_config(ExitHandlerConfig config) -> void {
        std::lock_guard lock{mu_};
        config_ = std::move(config);
    }

    // 执行所有清理回调，返回失败数量
    [[nodiscard]] auto run_cleanup() -> std::size_t {
        std::lock_guard lock{mu_};
        std::size_t failures = 0;
        // 按注册的逆序执行（LIFO）
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

    // 处理 Ctrl+C 的双击逻辑
    [[nodiscard]] auto handle_ctrl_c() -> bool {
        if (!config_.require_double_press) {
            initiate_exit(ExitReason::ctrl_c);
            return true;
        }

        auto now = Clock::now();
        if (exit_pending_ && first_press_time_) {
            // 检查是否在有效窗口内
            auto elapsed = now - *first_press_time_;
            if (elapsed <= config_.double_press_window) {
                initiate_exit(ExitReason::ctrl_c);
                return true;
            }
        }

        // 首次按下：标记为待定，等待确认
        exit_pending_ = true;
        first_press_time_ = now;
        return false;
    }

    // 执行退出流程：通知所有监听者
    auto initiate_exit(ExitReason reason) -> void {
        for (const auto& cb : exit_callbacks_) {
            cb(reason);
        }
        exit_pending_ = false;
    }
};

// 工厂函数：创建默认配置的退出处理器
[[nodiscard]] inline auto create_exit_handler() -> ExitHandler {
    return ExitHandler(ExitHandlerConfig{
        .require_double_press = true,
        .cleanup_timeout_ms = 5000,
        .save_on_exit = true,
        .double_press_window = std::chrono::milliseconds(1500)
    });
}

} // namespace cc::hooks
