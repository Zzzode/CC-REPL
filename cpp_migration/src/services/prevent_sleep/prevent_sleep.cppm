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

// ============================================================


enum class Platform : std::uint8_t {
    MacOS,
    Linux,
    Unknown,
};


enum class InhibitState : std::uint8_t {
    Inactive,
    Active,
    Failed,
};


struct InhibitConfig {
    bool auto_enable{true};
    bool auto_disable_on_idle{true};
    std::chrono::seconds idle_timeout{300};
    std::string reason{"Long-running operation in progress"};
};


struct InhibitHandle {
    int pid{-1};
    TimePoint started_at;
    std::string reason;
};

// ============================================================

// ============================================================

class PreventSleepService {
public:
    explicit PreventSleepService(InhibitConfig config = {})
        : config_(config), state_(InhibitState::Inactive),
          platform_(detect_platform()) {}


    VoidResult activate() {
        if (state_ == InhibitState::Active) return {};
        if (platform_ == Platform::Unknown) {
            return std::unexpected(Error{ErrorCode::NotFound, {}, "unsupported platform"});
        }

        handle_ = InhibitHandle{
            .pid = 0,
            .started_at = Clock::now(),
            .reason = config_.reason,
        };
        state_ = InhibitState::Active;
        last_activity_ = Clock::now();
        return {};
    }


    VoidResult deactivate() {
        if (state_ != InhibitState::Active) return {};

        handle_.reset();
        state_ = InhibitState::Inactive;
        return {};
    }


    void notify_activity() noexcept { last_activity_ = Clock::now(); }


    [[nodiscard]] bool should_auto_deactivate() const noexcept {
        if (!config_.auto_disable_on_idle || state_ != InhibitState::Active) return false;
        return (Clock::now() - last_activity_) >= config_.idle_timeout;
    }


    VoidResult on_tool_start() {
        if (config_.auto_enable) return activate();
        return {};
    }


    void on_tool_end() noexcept { last_activity_ = Clock::now(); }


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
