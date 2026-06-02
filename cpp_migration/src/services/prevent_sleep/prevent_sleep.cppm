/// @file prevent_sleep.cppm
/// @brief Prevent system sleep service.
/// Prevents system sleep during long operations using platform-specific
/// mechanisms (macOS caffeinate, Linux systemd-inhibit). Supports auto-enable
/// during tool execution and auto-disable on idle.
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <chrono>
#include <format>

export module cc.services.prevent_sleep;

import cc.types.types;

export namespace cc::services::prevent_sleep {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

// ============================================================
// 平台与状态
// ============================================================

// 目标平台
enum class Platform : std::uint8_t {
    MacOS,    // 使用 caffeinate
    Linux,    // 使用 systemd-inhibit
    Unknown,  // 不支持
};

// 抑制睡眠状态
enum class InhibitState : std::uint8_t {
    Inactive,   // 未抑制
    Active,     // 正在抑制
    Failed,     // 抑制失败
};

// 抑制配置
struct InhibitConfig {
    bool auto_enable{true};         // 工具执行时自动启用
    bool auto_disable_on_idle{true};
    std::chrono::seconds idle_timeout{300};  // 5 分钟无活动自动释放
    std::string reason{"Long-running operation in progress"};
};

// 抑制句柄信息
struct InhibitHandle {
    int pid{-1};                 // caffeinate/systemd-inhibit 进程 PID
    TimePoint started_at;
    std::string reason;
};

// ============================================================
// PreventSleepService - 系统休眠抑制
// ============================================================

class PreventSleepService {
public:
    explicit PreventSleepService(InhibitConfig config = {})
        : config_(config), state_(InhibitState::Inactive),
          platform_(detect_platform()) {}

    // 启动睡眠抑制
    VoidResult activate() {
        if (state_ == InhibitState::Active) return {};
        if (platform_ == Platform::Unknown) {
            return std::unexpected(Error{ErrorCode::NotFound, {}, "unsupported platform"});
        }
        // 实际实现: fork caffeinate / systemd-inhibit
        handle_ = InhibitHandle{
            .pid = 0,  // 占位: 实际为子进程 PID
            .started_at = Clock::now(),
            .reason = config_.reason,
        };
        state_ = InhibitState::Active;
        last_activity_ = Clock::now();
        return {};
    }

    // 释放睡眠抑制
    VoidResult deactivate() {
        if (state_ != InhibitState::Active) return {};
        // 实际实现: kill caffeinate 进程
        handle_.reset();
        state_ = InhibitState::Inactive;
        return {};
    }

    // 通知有活动 (重置空闲计时器)
    void notify_activity() noexcept { last_activity_ = Clock::now(); }

    // 检查是否应自动释放 (空闲超时)
    [[nodiscard]] bool should_auto_deactivate() const noexcept {
        if (!config_.auto_disable_on_idle || state_ != InhibitState::Active) return false;
        return (Clock::now() - last_activity_) >= config_.idle_timeout;
    }

    // 工具执行开始 (自动启用)
    VoidResult on_tool_start() {
        if (config_.auto_enable) return activate();
        return {};
    }

    // 工具执行结束
    void on_tool_end() noexcept { last_activity_ = Clock::now(); }

    // 查询状态
    [[nodiscard]] InhibitState state() const noexcept { return state_; }
    [[nodiscard]] Platform platform() const noexcept { return platform_; }
    [[nodiscard]] const InhibitConfig& config() const noexcept { return config_; }
    void set_config(InhibitConfig config) noexcept { config_ = std::move(config); }

private:
    InhibitConfig config_;
    InhibitState state_;
    Platform platform_;
    std::optional<InhibitHandle> handle_;
    TimePoint last_activity_{Clock::now()};

    // 检测当前平台
    static Platform detect_platform() noexcept {
        #if defined(__APPLE__)
            return Platform::MacOS;
        #elif defined(__linux__)
            return Platform::Linux;
        #else
            return Platform::Unknown;
        #endif
    }
};

} // namespace cc::services::prevent_sleep
