module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>

export module cc.commands.sandbox_toggle;

export namespace cc::commands {

// 全局沙箱状态（模块内部状态）
namespace detail {
    inline bool sandbox_enabled = false;
}

// 检查沙箱模式是否启用
auto is_sandbox_enabled() -> bool {
    return detail::sandbox_enabled;
}

// 切换沙箱模式，返回切换后的新状态
auto toggle_sandbox() -> bool {
    detail::sandbox_enabled = !detail::sandbox_enabled;
    return detail::sandbox_enabled;
}

// 获取沙箱状态的可读描述
auto get_sandbox_status() -> std::string {
    if (detail::sandbox_enabled) {
        return "Sandbox: ENABLED - File writes and commands are restricted";
    }
    return "Sandbox: DISABLED - Full system access";
}

// 显示沙箱模式的详细说明
auto show_sandbox_info() -> std::string {
    std::string info = "Sandbox Mode Information:\n\n";
    info += "When enabled, sandbox mode restricts:\n";
    info += "  - File write operations (redirected to temp directory)\n";
    info += "  - Shell command execution (blocked or sandboxed)\n";
    info += "  - Network access (limited to API calls)\n";
    info += "  - System modifications (all blocked)\n\n";
    info += "Current status: " + get_sandbox_status() + "\n\n";
    info += "Use /sandbox to toggle sandbox mode.\n";
    return info;
}

} // namespace cc::commands
