module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>

export module cc.commands.sandbox_toggle;

export namespace cc::commands {


namespace detail {
    inline bool sandbox_enabled = false;
}


auto is_sandbox_enabled() -> bool {
    return detail::sandbox_enabled;
}


auto toggle_sandbox() -> bool {
    detail::sandbox_enabled = !detail::sandbox_enabled;
    return detail::sandbox_enabled;
}


auto get_sandbox_status() -> std::string {
    if (detail::sandbox_enabled) {
        return "Sandbox: ENABLED - File writes and commands are restricted";
    }
    return "Sandbox: DISABLED - Full system access";
}


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
