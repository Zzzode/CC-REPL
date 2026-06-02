module;
#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <chrono>
#include <vector>

export module cc.hooks.tool_permission.permission_logging;

export namespace cc::hooks::tool_permission {

// Permission decision outcome
enum class PermissionDecisionType {
    accept,
    reject
};

// Source of the permission approval
enum class PermissionApprovalSource {
    user_allow,         // User explicitly allowed
    user_allow_always,  // User chose "always allow"
    config_rule,        // Matched a config auto-approve rule
    sandbox,            // Running in sandbox mode
    trusted_tool        // Tool is in trusted list
};

// Source of the permission rejection
enum class PermissionRejectionSource {
    user_deny,          // User explicitly denied
    user_deny_always,   // User chose "always deny"
    config_rule,        // Matched a config deny rule
    timeout,            // Permission prompt timed out
    security_policy     // Blocked by security policy
};

// Context passed to logging functions
struct PermissionLogContext {
    std::string tool_name;
    std::string tool_input_json;
    std::string message_id;
    std::string tool_use_id;
    std::optional<std::string> file_path;
    std::optional<std::string> language;
};

// A logged permission event for audit trail
struct PermissionLogEntry {
    PermissionDecisionType decision;
    std::string source;          // stringified source
    PermissionLogContext context;
    std::chrono::system_clock::time_point timestamp;
};

// Analytics event callback
using AnalyticsCallback = std::function<void(std::string_view event_name,
                                             const PermissionLogEntry& entry)>;

// Code edit tools that get special metrics tracking
inline constexpr std::string_view code_editing_tools[] = {
    "Edit", "Write", "NotebookEdit"
};

// Check if a tool is a code editing tool
[[nodiscard]] inline auto is_code_editing_tool(std::string_view tool_name) -> bool {
    for (auto name : code_editing_tools) {
        if (name == tool_name) return true;
    }
    return false;
}

// Sanitize tool name for analytics (remove special chars, truncate)
[[nodiscard]] inline auto sanitize_tool_name(std::string_view name) -> std::string {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            result += c;
        }
    }
    if (result.size() > 64) result.resize(64);
    return result;
}

// Log a permission decision (central logging point)
inline auto log_permission_decision(
    [[maybe_unused]] PermissionDecisionType decision,
    [[maybe_unused]] std::string_view source,
    [[maybe_unused]] const PermissionLogContext& context,
    [[maybe_unused]] AnalyticsCallback analytics_cb = nullptr
) -> void {
    PermissionLogEntry entry{
        .decision = decision,
        .source = std::string(source),
        .context = context,
        .timestamp = std::chrono::system_clock::now()
    };

    if (analytics_cb) {
        analytics_cb("tool_permission_decision", entry);
    }

    // Code editing tools get additional counter metrics
    if (is_code_editing_tool(context.tool_name)) {
        if (analytics_cb) {
            analytics_cb("code_edit_tool_decision", entry);
        }
    }
}

// Log an approval event
inline auto log_permission_approved(
    PermissionApprovalSource source,
    const PermissionLogContext& context,
    AnalyticsCallback analytics_cb = nullptr
) -> void {
    auto source_str = [source]() -> std::string_view {
        switch (source) {
            case PermissionApprovalSource::user_allow: return "user_allow";
            case PermissionApprovalSource::user_allow_always: return "user_allow_always";
            case PermissionApprovalSource::config_rule: return "config_rule";
            case PermissionApprovalSource::sandbox: return "sandbox";
            case PermissionApprovalSource::trusted_tool: return "trusted_tool";
        }
        return "unknown";
    }();
    log_permission_decision(PermissionDecisionType::accept, source_str, context, analytics_cb);
}

// Log a rejection event
inline auto log_permission_rejected(
    PermissionRejectionSource source,
    const PermissionLogContext& context,
    AnalyticsCallback analytics_cb = nullptr
) -> void {
    auto source_str = [source]() -> std::string_view {
        switch (source) {
            case PermissionRejectionSource::user_deny: return "user_deny";
            case PermissionRejectionSource::user_deny_always: return "user_deny_always";
            case PermissionRejectionSource::config_rule: return "config_rule";
            case PermissionRejectionSource::timeout: return "timeout";
            case PermissionRejectionSource::security_policy: return "security_policy";
        }
        return "unknown";
    }();
    log_permission_decision(PermissionDecisionType::reject, source_str, context, analytics_cb);
}

} // namespace cc::hooks::tool_permission
