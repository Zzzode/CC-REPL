module;
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.ui.permission_request;

export namespace cc::ui::permissions {

// --- Permission type classification ---
enum class PermissionType {
    FileRead,
    FileWrite,
    FileEdit,
    BashCommand,
    McpTool,
    NetworkAccess,
    EnvAccess,
    Custom
};

// --- Risk assessment levels ---
enum class RiskLevel {
    Low,
    Medium,
    High,
    Critical
};

// --- Permission request metadata ---
struct PermissionRequestInfo {
    std::string tool_name;
    PermissionType type;
    RiskLevel risk;
    std::string description;
    std::vector<std::string> affected_paths;
    std::optional<std::string> command;
    std::optional<std::string> working_dir;
    std::chrono::system_clock::time_point requested_at;
};

// --- User response to a permission prompt ---
enum class UserResponse {
    Allow,
    AllowAlways,
    Deny,
    DenyAlways,
    Abort
};

// --- Configuration for permission prompt rendering ---
struct PermissionPromptConfig {
    bool show_risk_indicator{true};
    bool show_affected_paths{true};
    bool allow_always_option{true};
    int max_paths_shown{5};
};

// --- Rendering & formatting functions ---

// Get a human-readable label for a permission type
[[nodiscard]] inline auto get_permission_type_label(PermissionType type) -> std::string_view {
    switch (type) {
        case PermissionType::FileRead:      return "File Read";
        case PermissionType::FileWrite:     return "File Write";
        case PermissionType::FileEdit:      return "File Edit";
        case PermissionType::BashCommand:   return "Bash Command";
        case PermissionType::McpTool:       return "MCP Tool";
        case PermissionType::NetworkAccess: return "Network Access";
        case PermissionType::EnvAccess:     return "Environment Access";
        case PermissionType::Custom:        return "Custom Permission";
    }
    return "Unknown";
}

// Get ANSI color code for a risk level
[[nodiscard]] inline auto get_risk_color(RiskLevel level) -> std::string {
    switch (level) {
        case RiskLevel::Low:      return "\033[32m";   // green
        case RiskLevel::Medium:   return "\033[33m";   // yellow
        case RiskLevel::High:     return "\033[31m";   // red
        case RiskLevel::Critical: return "\033[1;31m"; // bold red
    }
    return "\033[0m";
}

// Format a risk level badge with color
[[nodiscard]] inline auto format_risk_badge(RiskLevel level) -> std::string {
    std::string color = get_risk_color(level);
    std::string_view label;
    switch (level) {
        case RiskLevel::Low:      label = "LOW"; break;
        case RiskLevel::Medium:   label = "MEDIUM"; break;
        case RiskLevel::High:     label = "HIGH"; break;
        case RiskLevel::Critical: label = "CRITICAL"; break;
    }
    return color + "[" + std::string(label) + "]\033[0m";
}

// Format affected paths list, truncating if exceeding max_shown
[[nodiscard]] inline auto format_affected_paths(const std::vector<std::string>& paths,
                                                 int max_shown = 5) -> std::string {
    if (paths.empty()) {
        return "";
    }
    std::string result;
    int shown = 0;
    for (const auto& path : paths) {
        if (shown >= max_shown) {
            auto remaining = paths.size() - static_cast<std::size_t>(max_shown);
            result += "  \033[2m... and " + std::to_string(remaining) + " more\033[0m\n";
            break;
        }
        result += "  \033[36m" + path + "\033[0m\n";
        ++shown;
    }
    return result;
}

// Determine if a permission request should be auto-allowed (low-risk read ops)
[[nodiscard]] inline auto should_auto_allow(const PermissionRequestInfo& info) -> bool {
    if (info.risk != RiskLevel::Low) {
        return false;
    }
    return info.type == PermissionType::FileRead;
}

// Format a full permission request for terminal display
[[nodiscard]] inline auto format_permission_request(const PermissionRequestInfo& info,
                                                     PermissionPromptConfig config = {}) -> std::string {
    std::string result;

    // Header
    result += "\033[1;33m\u26a0 Permission Request: \033[0m";
    result += "\033[1m" + std::string(get_permission_type_label(info.type)) + "\033[0m";

    // Risk badge
    if (config.show_risk_indicator) {
        result += " " + format_risk_badge(info.risk);
    }
    result += "\n";

    // Tool name and description
    result += "\033[1m  Tool:\033[0m " + info.tool_name + "\n";
    if (!info.description.empty()) {
        result += "\033[2m  " + info.description + "\033[0m\n";
    }

    // Command if present
    if (info.command.has_value()) {
        result += "\033[1;33m  $ \033[0m" + *info.command + "\n";
    }

    // Working directory
    if (info.working_dir.has_value()) {
        result += "\033[2m  in: " + *info.working_dir + "\033[0m\n";
    }

    // Affected paths
    if (config.show_affected_paths && !info.affected_paths.empty()) {
        result += "\033[1m  Affected paths:\033[0m\n";
        result += format_affected_paths(info.affected_paths, config.max_paths_shown);
    }

    // Action hints
    result += "\n\033[2m  [y] Allow  [n] Deny";
    if (config.allow_always_option) {
        result += "  [a] Always  [d] Never";
    }
    result += "  [x] Abort\033[0m";

    return result;
}

// Render a history of recent permission requests
[[nodiscard]] inline auto render_permission_history(const std::vector<PermissionRequestInfo>& history,
                                                     std::size_t limit = 10) -> std::string {
    if (history.empty()) {
        return "\033[2mNo permission history.\033[0m\n";
    }
    std::string result;
    result += "\033[1mRecent Permission Requests:\033[0m\n";
    std::size_t count = 0;
    for (const auto& info : history) {
        if (count >= limit) break;
        result += "  " + format_risk_badge(info.risk) + " ";
        result += std::string(get_permission_type_label(info.type));
        result += " \033[2m(" + info.tool_name + ")\033[0m\n";
        ++count;
    }
    if (history.size() > limit) {
        auto remaining = history.size() - limit;
        result += "\033[2m  ... " + std::to_string(remaining) + " more entries\033[0m\n";
    }
    return result;
}

} // namespace cc::ui::permissions
