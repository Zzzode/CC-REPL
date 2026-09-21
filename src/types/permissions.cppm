/// @file permissions.cppm
/// @brief Permission types and mode definitions.
/// Migrated from src/types/permissions.ts
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_set>

export module cc.types.permissions;

export namespace cc::types {

/// Permission mode for tool execution
enum class PermissionMode : std::uint8_t {
    /// Ask for every tool use
    Default,
    /// Auto-approve all tool uses (dangerous)
    AutoApprove,
    /// Only suggest, never execute
    Suggest,
    /// Plan mode - read-only, no mutations
    Plan,
};

/// Convert to string
[[nodiscard]] constexpr std::string_view permission_mode_to_string(PermissionMode mode) noexcept {
    switch (mode) {
        case PermissionMode::Default: return "default";
        case PermissionMode::AutoApprove: return "auto-approve";
        case PermissionMode::Suggest: return "suggest";
        case PermissionMode::Plan: return "plan";
    }
    return "default";
}

/// Parse from string
[[nodiscard]] inline std::optional<PermissionMode> parse_permission_mode(std::string_view str) {
    if (str == "default") return PermissionMode::Default;
    if (str == "auto-approve" || str == "dangerously-skip-permissions") return PermissionMode::AutoApprove;
    if (str == "suggest") return PermissionMode::Suggest;
    if (str == "plan") return PermissionMode::Plan;
    return std::nullopt;
}

/// A permission rule for a specific tool
struct PermissionRule {
    std::string tool_name;
    bool allowed = false;
    std::optional<std::string> reason;
};

/// Permission decision
enum class PermissionDecision : std::uint8_t {
    Allow,
    Deny,
    Ask,
};

} // namespace cc::types
