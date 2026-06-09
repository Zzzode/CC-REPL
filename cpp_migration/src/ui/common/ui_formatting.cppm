/// @file ui_formatting.cppm
/// @brief Pure formatting helper functions used by UI layers.
///
/// Consolidates pure logic formatting helpers from these TS files
/// (no JSX, no I/O, no side effects — only string/color math):
///   - Source: src/components/design-system/color.ts (30 lines → merged here)
///   - Source: src/components/agents/utils.ts (18 lines → merged here)
///   - Source: src/components/messages/teamMemSaved.ts (19 lines → merged here)
///   - Source: src/components/messages/UserToolResultMessage/utils.tsx (45 lines → merged here)
///   - Source: src/components/ManagedSettingsSecurityDialog/utils.ts (144 lines → partial, types only)
module;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

export module cc.ui.common.formatting;

export namespace cc::ui::common::formatting {

// ============================================================
// From: src/components/design-system/color.ts
// ============================================================

enum class ColorType { Foreground, Background };

/// Resolves a raw or theme color into an ANSI color string.
/// Raw color values ("rgb(...)", "#rrggbb", "ansi256(...)", "ansi:...")
/// are passed through unchanged; theme keys are resolved via `theme_lookup`.
[[nodiscard]] inline std::string resolve_color(
    std::string_view color_key,
    std::string_view /*theme_name*/,
    ColorType /*type*/ = ColorType::Foreground,
    std::function<std::string(std::string_view)> theme_lookup = nullptr) {
    if (color_key.empty()) return {};
    // Raw color values bypass theme lookup
    if (color_key.starts_with("rgb(") ||
        color_key.starts_with("#") ||
        color_key.starts_with("ansi256(") ||
        color_key.starts_with("ansi:")) {
        return std::string(color_key);
    }
    // Theme key lookup
    if (theme_lookup) return theme_lookup(color_key);
    return std::string(color_key);
}

// ============================================================
// From: src/components/agents/utils.ts
// ============================================================

namespace agent_utils {

enum class AgentSettingSource {
    All,
    BuiltIn,
    Plugin,
    User,
    Project,
};

[[nodiscard]] inline std::string get_agent_source_display_name(
    AgentSettingSource source) {
    switch (source) {
        case AgentSettingSource::All:      return "Agents";
        case AgentSettingSource::BuiltIn:  return "Built-in agents";
        case AgentSettingSource::Plugin:   return "Plugin agents";
        case AgentSettingSource::User:     return "User agents";
        case AgentSettingSource::Project:  return "Project agents";
    }
    return "Agents";
}

} // namespace agent_utils

// ============================================================
// From: src/components/messages/teamMemSaved.ts
// ============================================================

struct TeamMemorySegment {
    std::string segment;
    int count = 0;
};

/// Returns a display string for team-memory notifications plus the raw count.
/// Returns std::nullopt when the team memory count is zero.
[[nodiscard]] inline std::optional<TeamMemorySegment> team_mem_saved_segment(
    std::optional<int> team_count) {
    const int count = team_count.value_or(0);
    if (count == 0) return std::nullopt;
    return TeamMemorySegment{
        std::format("{} team {}", count, count == 1 ? "memory" : "memories"),
        count
    };
}

// ============================================================
// From: src/components/messages/UserToolResultMessage/utils.tsx
// ============================================================

namespace tool_result_utils {

/// Status enum for tool result display row prefixes.
enum class ToolResultStatus {
    Success,
    Error,
    Rejected,
    Canceled,
    PlanRejected,
};

/// Returns the (icon, color) pair for a tool result status badge.
[[nodiscard]] inline std::tuple<std::string_view, std::string_view>
tool_result_badge(ToolResultStatus status) {
    switch (status) {
        case ToolResultStatus::Success:       return {"✓", "success"};
        case ToolResultStatus::Error:         return {"✗", "error"};
        case ToolResultStatus::Rejected:      return {"⊘", "rejected"};
        case ToolResultStatus::Canceled:      return {"⤫", "cancelled"};
        case ToolResultStatus::PlanRejected:  return {"⊘", "planMode"};
    }
    return {"?", "subtle"};
}

/// Shortens a tool error message by truncating to `max_len` with an ellipsis.
[[nodiscard]] inline std::string shorten_error_message(std::string_view msg,
                                                       size_t max_len = 200) {
    if (msg.size() <= max_len) return std::string(msg);
    return std::format("{}…", msg.substr(0, max_len - 1));
}

/// Formats an elapsed-seconds value into "HH:MM:SS" (or "MM:SS" under 1 hour).
[[nodiscard]] inline std::string format_elapsed_seconds(int64_t total_seconds) {
    if (total_seconds < 0) total_seconds = 0;
    const int h = static_cast<int>(total_seconds / 3600);
    const int m = static_cast<int>((total_seconds % 3600) / 60);
    const int s = static_cast<int>(total_seconds % 60);
    std::ostringstream oss;
    oss << std::setfill('0');
    if (h > 0) {
        oss << std::setw(2) << h << ':'
            << std::setw(2) << m << ':'
            << std::setw(2) << s;
    } else {
        oss << std::setw(2) << m << ':'
            << std::setw(2) << s;
    }
    return oss.str();
}

/// Capitalises the first letter of a string (ASCII-only).
[[nodiscard]] inline std::string capitalize_ascii(std::string_view s) {
    if (s.empty()) return {};
    std::string out(s);
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

} // namespace tool_result_utils

// ============================================================
// Generic helpers used across formatting layer
// ============================================================

/// Returns true if the string starts with a scheme prefix ("http://", etc.)
[[nodiscard]] inline bool has_uri_scheme(std::string_view s) {
    return s.starts_with("http://") ||
           s.starts_with("https://") ||
           s.starts_with("file://") ||
           s.starts_with("mailto:");
}

/// Trims whitespace on both ends of the string_view.
[[nodiscard]] inline std::string_view trim(std::string_view s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto begin = std::find_if(s.begin(), s.end(), not_space);
    auto rbegin = std::reverse_iterator(s.end());
    auto rend = std::reverse_iterator(begin);
    auto last = std::find_if(rbegin, rend, not_space).base();
    if (begin >= last) return {};
    auto first = static_cast<size_t>(std::distance(s.begin(), begin));
    auto count = static_cast<size_t>(std::distance(begin, last));
    return s.substr(first, count);
}

} // namespace cc::ui::common::formatting
