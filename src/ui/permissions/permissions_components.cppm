/// @file permissions_components.cppm
/// @brief Shared reusable UI widgets for the Permissions subsystem.
///
/// Consolidates 47 small TS widgets (titles, icons, rows, list items,
/// confirmation bars, badges, etc.) into one reusable component library.
///
/// Provides: ToolIcon, ActionIcon, PathLabel (middle-ellipsis), RiskPill,
/// HeaderRow, ThinDivider, StatusDot, PathContextBadge, ToolNameBadge,
/// KeyboardHint, CheckboxToggle, and utility formatters.
///
/// Engine logic is 100% delegated to cc.utils.permissions_engine — this
/// file only imports display types, never duplicates matching logic.
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.permissions.components;

import cc.utils.permissions_engine;

export namespace cc::ui::permissions::components {
using namespace ftxui;
using Rule = cc::utils::permissions::PermissionRule;
using MatchStrategy = cc::utils::permissions::MatchStrategy;

// ============================================================
// ToolIcon — icon for a tool by canonical name
// ============================================================

/// Get a short unicode icon glyph for a tool name.
/// Falls back to a generic gear if the tool is not known.
[[nodiscard]] inline std::string_view ToolIconGlyph(std::string_view tool_name) {
    // File tools
    if (tool_name == "FileReadTool" || tool_name == "file_read" ||
        tool_name == "Read" || tool_name == "FileRead")
        return "📖";
    if (tool_name == "FileWriteTool" || tool_name == "file_write" ||
        tool_name == "Write" || tool_name == "FileWrite")
        return "✏️";
    if (tool_name == "FileEditTool" || tool_name == "file_edit" ||
        tool_name == "Edit" || tool_name == "FileEdit")
        return "🔧";
    if (tool_name == "GlobTool" || tool_name == "glob")     return "🔍";
    if (tool_name == "GrepTool" || tool_name == "grep")     return "🔎";
    // Shell tools
    if (tool_name == "BashTool" || tool_name == "bash")     return "⚡";
    if (tool_name == "PowerShellTool" || tool_name == "powershell") return "⚡";
    if (tool_name == "SedEditTool")                         return "🔧";
    // Network tools
    if (tool_name == "WebFetchTool" || tool_name == "web_fetch") return "🌐";
    if (tool_name == "WebSearchTool" || tool_name == "web_search") return "🔎";
    // Agent / planning
    if (tool_name == "AgentTool" || tool_name == "agent")   return "🤖";
    if (tool_name == "SkillTool" || tool_name == "skill")   return "🧩";
    if (tool_name == "EnterPlanModeTool" || tool_name == "EnterPlanMode") return "📋";
    if (tool_name == "ExitPlanModeV2Tool" || tool_name == "ExitPlanMode") return "✅";
    if (tool_name == "TaskCreateTool" || tool_name == "task_create") return "📝";
    if (tool_name == "TaskUpdateTool" || tool_name == "task_update") return "📝";
    if (tool_name == "TeamCreateTool" || tool_name == "team_create") return "👥";
    // MCP / Bridge
    if (tool_name == "MCPTool" || tool_name == "mcp")       return "🔌";
    if (tool_name == "NotebookEditTool")                    return "📓";
    // User interaction
    if (tool_name == "AskUserQuestionTool" || tool_name == "AskUserQuestion") return "❓";
    if (tool_name == "ComputerUseApproval" || tool_name == "ComputerUse")     return "🖥️";
    // Generic
    return "🛠️";
}

/// Render a tool icon as an FTXUI Element (fixed 2-cell width)
[[nodiscard]] inline Element ToolIcon(std::string_view tool_name) {
    return text(std::string{ToolIconGlyph(tool_name)}) | center;
}

// ============================================================
// ActionIcon — icon for an action category (Read/Write/Execute/Network)
// ============================================================

enum class ActionKind : std::uint8_t {
    Read,       // File read, search, fetch
    Write,      // File write, edit, delete
    Execute,    // Bash, PowerShell, Skill, Agent
    Network,    // Web fetch, web search
    MCP,        // MCP tool call
    Environment,// Environment variable access
    Plan,       // Plan mode entry/exit
    Other,
};

[[nodiscard]] inline std::string_view ActionIconGlyph(ActionKind kind) {
    switch (kind) {
        case ActionKind::Read:        return "📖";
        case ActionKind::Write:       return "✏️";
        case ActionKind::Execute:     return "▶";
        case ActionKind::Network:     return "🌐";
        case ActionKind::MCP:         return "🔌";
        case ActionKind::Environment: return "🔐";
        case ActionKind::Plan:        return "📋";
        case ActionKind::Other:       return "🛠️";
    }
    return "🛠️";
}

[[nodiscard]] inline Color ActionColor(ActionKind kind) {
    switch (kind) {
        case ActionKind::Read:        return Color::Cyan;
        case ActionKind::Write:       return Color::Yellow;
        case ActionKind::Execute:     return Color::Magenta;
        case ActionKind::Network:     return Color::BlueLight;
        case ActionKind::MCP:         return Color::Purple;
        case ActionKind::Environment: return Color::Orange1;
        case ActionKind::Plan:        return Color::GreenLight;
        case ActionKind::Other:       return Color::GrayLight;
    }
    return Color::White;
}

/// Render an action icon with its color as an FTXUI Element
[[nodiscard]] inline Element ActionIcon(ActionKind kind) {
    return text(std::string{ActionIconGlyph(kind)}) | color(ActionColor(kind));
}

// ============================================================
// PathLabel — display a path with middle-ellipsis and optional workspace indicator
// ============================================================

/// Produce a middle-ellipsis path string to fit within max_len.
/// e.g. /a/very/long/path/to/some/file.txt → /a/…/some/file.txt
[[nodiscard]] inline std::string MiddleEllipsisPath(std::string_view path,
                                                    std::size_t max_len = 50) {
    if (path.size() <= max_len) return std::string{path};
    // Preserve at least 10 chars for the tail (filename + 2 dirs preferred)
    constexpr std::size_t kMinTail = 10;
    constexpr std::size_t kMinHead = 5;
    const std::size_t ellipsis = 3; // "…"
    // If max_len is too small for head+ellipsis+tail, just head-ellipsis
    if (max_len < kMinHead + ellipsis + kMinTail) {
        if (max_len <= ellipsis) return "…";
        return std::string{path.substr(0, max_len - ellipsis)} + "…";
    }
    const std::size_t tail_len = std::min(
        std::max(kMinTail, max_len / 3),
        path.size() - kMinHead - ellipsis);
    const std::size_t head_len = max_len - tail_len - ellipsis;
    return std::string{path.substr(0, head_len)} + "…"
         + std::string{path.substr(path.size() - tail_len, tail_len)};
}

/// Returns true if the path is inside (or equal to) the workspace root.
[[nodiscard]] inline bool IsInsideWorkspace(std::string_view path,
                                            std::string_view workspace_root) {
    if (workspace_root.empty()) return false;
    if (path == workspace_root) return true;
    // Ensure trailing separator consistency
    std::string root{workspace_root};
    if (!root.empty() && root.back() != '/' && root.back() != '\\') {
        root += '/';
    }
    return path.substr(0, root.size()) == std::string_view{root};
}

/// Render a path label with middle-ellipsis and optional workspace badge.
[[nodiscard]] inline Element PathLabel(std::string_view path,
                                       std::size_t max_len = 50,
                                       std::optional<std::string_view> workspace_root = {}) {
    auto el = text(MiddleEllipsisPath(path, max_len));
    if (workspace_root && IsInsideWorkspace(path, *workspace_root)) {
        return hbox({
            el,
            text(" w") | dim | color(Color::Green),
        });
    }
    return el;
}

/// Render a path with a danger highlight if it's on the high-risk list.
/// caller supplies a predicate — this file never hard-codes dangerous paths.
[[nodiscard]] inline Element PathLabelHighlighted(
    std::string_view path,
    bool is_dangerous,
    std::size_t max_len = 50)
{
    auto el = text(MiddleEllipsisPath(path, max_len));
    if (is_dangerous) {
        return el | bold | color(Color::Red);
    }
    return el;
}

// ============================================================
// RiskPill — colored risk-level indicator pill
// ============================================================

enum class RiskLevel : std::uint8_t {
    Low,        // Green  — read-only, trivial tools
    Medium,     // Yellow — writes inside workspace
    High,       // Red    — deletes, writes outside workspace
    Critical,   // Bold   red — rm -rf /, network uploads of secrets, etc.
};

[[nodiscard]] inline Color RiskPillBgColor(RiskLevel level) {
    switch (level) {
        case RiskLevel::Low:      return Color::Green;
        case RiskLevel::Medium:   return Color::Yellow;
        case RiskLevel::High:     return Color::Red;
        case RiskLevel::Critical: return Color::RedLight;
    }
    return Color::GrayDark;
}

[[nodiscard]] inline Color RiskPillFgColor(RiskLevel level) {
    switch (level) {
        case RiskLevel::Low:      return Color::Black;
        case RiskLevel::Medium:   return Color::Black;
        case RiskLevel::High:     return Color::White;
        case RiskLevel::Critical: return Color::White;
    }
    return Color::White;
}

[[nodiscard]] inline std::string_view RiskLabel(RiskLevel level) {
    switch (level) {
        case RiskLevel::Low:      return "LOW";
        case RiskLevel::Medium:   return "MEDIUM";
        case RiskLevel::High:     return "HIGH";
        case RiskLevel::Critical: return "CRITICAL";
    }
    return "???";
}

/// Render a risk pill as a colored badge element: ` [LOW] `
[[nodiscard]] inline Element RiskPill(RiskLevel level) {
    auto fg = RiskPillFgColor(level);
    auto bg = RiskPillBgColor(level);
    return text(std::format(" {} ", RiskLabel(level))) | color(fg) | bgcolor(bg) | bold;
}

// ============================================================
// HeaderRow — title + optional subtitle used at the top of any permission UI
// ============================================================

/// Render a standard two-line header row with a bold title and dimmed subtitle.
[[nodiscard]] inline Element HeaderRow(std::string_view title,
                                       std::optional<std::string_view> subtitle = {}) {
    auto title_el = text(std::string{title}) | bold;
    if (!subtitle) return title_el;
    return vbox({
        title_el,
        text(std::string{*subtitle}) | dim,
    });
}

/// Header with a leading icon (tool or action) on the left and a right widget
/// (e.g. a RiskPill) on the right.
[[nodiscard]] inline Element HeaderRowWithIcon(
    std::string_view icon,
    std::string_view title,
    std::optional<std::string_view> subtitle = {},
    Element right_widget = text(""))
{
    auto left = hbox({
        text(std::string{icon}),
        text(" "),
        HeaderRow(title, subtitle),
    });
    return hbox({
        left | xflex,
        right_widget,
    });
}

// ============================================================
// ThinDivider — light separator line
// ============================================================

/// A thin horizontal separator (dimmer than ftxui::separator()).
/// Optional `col` override — when provided, paints the separator in that
/// color (faithful to TS Pane.tsx:52 Divider(color)).  Defaults to GrayDark
/// for the pre-existing uncoloured in-dialog section separators.
[[nodiscard]] inline Element ThinDivider(
    std::optional<Color> col = std::nullopt) {
    return separator() | color(col.value_or(Color::GrayDark));
}

// ============================================================
// StatusDot — colored circular indicator for list items
// ============================================================

enum class ItemStatus : std::uint8_t {
    Pending,   // Yellow — awaiting decision
    Approved,  // Green  — allowed
    Denied,    // Red    — denied
    Always,    // Cyan   — always rule applied
};

[[nodiscard]] inline Color StatusDotColor(ItemStatus s) {
    switch (s) {
        case ItemStatus::Pending:  return Color::Yellow;
        case ItemStatus::Approved: return Color::Green;
        case ItemStatus::Denied:   return Color::Red;
        case ItemStatus::Always:   return Color::Cyan;
    }
    return Color::GrayDark;
}

/// Render a 2-cell status dot (unicode bullet + 1 space).
[[nodiscard]] inline Element StatusDot(ItemStatus s) {
    return text("● ") | color(StatusDotColor(s));
}

// ============================================================
// PathContextBadge — small badge showing scope (Read/Write/Exec)
// ============================================================

enum class PathScope : std::uint8_t {
    Read,
    Write,
    Execute,
    Network,
    All,
};

[[nodiscard]] inline std::string_view PathScopeLabel(PathScope s) {
    switch (s) {
        case PathScope::Read:    return "R";
        case PathScope::Write:   return "W";
        case PathScope::Execute: return "X";
        case PathScope::Network: return "N";
        case PathScope::All:     return "RWX";
    }
    return "?";
}

[[nodiscard]] inline Color PathScopeColor(PathScope s) {
    switch (s) {
        case PathScope::Read:    return Color::Cyan;
        case PathScope::Write:   return Color::Yellow;
        case PathScope::Execute: return Color::Magenta;
        case PathScope::Network: return Color::BlueLight;
        case PathScope::All:     return Color::Green;
    }
    return Color::White;
}

/// Render a small scope badge, e.g. `[R]` or `[W]`.
[[nodiscard]] inline Element PathScopeBadge(PathScope scope) {
    return text(std::format("[{}]", PathScopeLabel(scope)))
           | color(PathScopeColor(scope)) | dim;
}

// ============================================================
// ToolNameBadge — tool name with color based on risk category
// ============================================================

/// Render a tool name badge with a dim border.
[[nodiscard]] inline Element ToolNameBadge(std::string_view tool_name,
                                           RiskLevel level = RiskLevel::Low) {
    return text(std::format(" {} ", tool_name))
           | color(ActionColor([&] {
               // heuristic coloring based on risk
               if (level >= RiskLevel::High) return ActionKind::Execute;
               if (level == RiskLevel::Medium) return ActionKind::Write;
               return ActionKind::Read;
           }()))
           | borderLight | size(WIDTH, EQUAL, static_cast<int>(tool_name.size()) + 4);
}

// ============================================================
// KeyboardHint — bottom-of-dialog keybinding hint row
// ============================================================

struct KeyHint {
    std::string key;        // e.g. "y", "Esc", "Enter"
    std::string action;     // e.g. "Allow once"
};

/// Render a row of keyboard hints, dimmed: `[y] Allow  [n] Deny  [Esc] Cancel`
[[nodiscard]] inline Element KeyboardHintRow(
    const std::vector<KeyHint>& hints)
{
    Elements parts;
    for (std::size_t i = 0; i < hints.size(); ++i) {
        const auto& h = hints[i];
        parts.push_back(text("[") | dim);
        parts.push_back(text(h.key) | color(Color::Cyan) | bold | dim);
        parts.push_back(text("] ") | dim);
        parts.push_back(text(h.action) | dim);
        if (i + 1 < hints.size()) parts.push_back(text("  ") | dim);
    }
    return hbox(parts);
}

// ============================================================
// CheckboxToggle — interactive [ ] / [x] toggle used for "always" options
// ============================================================

/// Render a checkbox state (static label — for embedding in a row).
[[nodiscard]] inline Element CheckboxBox(bool checked) {
    return checked
        ? hbox({text("[") | dim, text("x") | color(Color::Green) | bold, text("]") | dim})
        : hbox({text("[") | dim, text(" "),                        text("]") | dim});
}

/// State for a checkbox component.
struct CheckboxState {
    bool checked = false;
    std::string label;
};

/// Create an interactive checkbox with keyboard support (Space toggles).
[[nodiscard]] inline Component CheckboxToggle(
    std::shared_ptr<CheckboxState> state,
    std::function<void(bool)> on_change = {})
{
    return Renderer([state] {
        return hbox({
            CheckboxBox(state->checked),
            text(" "),
            text(state->label),
        });
    }) | CatchEvent([state, on_change](Event event) -> bool {
        if (event == Event::Character(' ') || event == Event::Return) {
            state->checked = !state->checked;
            if (on_change) on_change(state->checked);
            return true;
        }
        return false;
    });
}

// ============================================================
// MatchStrategy labels — delegate to engine enums without duplication
// ============================================================

[[nodiscard]] inline std::string_view MatchStrategyLabel(MatchStrategy s) {
    using cc::utils::permissions::MatchStrategy;
    switch (s) {
        case MatchStrategy::Exact:  return "exact";
        case MatchStrategy::Prefix: return "prefix";
        case MatchStrategy::Glob:   return "glob";
        case MatchStrategy::Regex:  return "regex";
    }
    return "???";
}

// ============================================================
// Helpers for path depth and list summarisation
// ============================================================

/// Count path separators (rough depth count).
[[nodiscard]] inline int PathDepth(std::string_view path) {
    int depth = 0;
    for (char c : path) if (c == '/' || c == '\\') ++depth;
    return depth;
}

/// Summarise N paths: show up to `max_shown` with middle-ellipsis,
/// add "+N more" footer. Returns an Elements vector.
[[nodiscard]] inline Elements SummarisePathList(
    const std::vector<std::string>& paths,
    int max_shown = 5,
    std::size_t max_path_len = 50)
{
    Elements els;
    int shown = 0;
    for (const auto& p : paths) {
        if (shown >= max_shown) break;
        els.push_back(hbox({
            text("• ") | dim,
            PathLabel(p, max_path_len),
        }));
        ++shown;
    }
    if (paths.size() > static_cast<std::size_t>(max_shown)) {
        auto remaining = paths.size() - static_cast<std::size_t>(max_shown);
        els.push_back(text(std::format("… and {} more", remaining)) | dim | color(Color::GrayLight));
    }
    return els;
}

// ============================================================
// Ellipsised command preview
// ============================================================

/// Trim a shell command preview to fit `max_len` using head-ellipsis
/// (preserves the end of the command, which is usually the interesting part).
[[nodiscard]] inline std::string HeadEllipsisCommand(std::string_view cmd,
                                                     std::size_t max_len = 80) {
    if (cmd.size() <= max_len) return std::string{cmd};
    if (max_len < 4) return "…";
    const std::size_t keep = max_len - 1;
    return "…" + std::string{cmd.substr(cmd.size() - keep, keep)};
}

/// Render a shell command with a "$ " prefix and optional truncation.
[[nodiscard]] inline Element CommandPreview(std::string_view cmd,
                                            std::size_t max_len = 80) {
    return hbox({
        text("$ ") | color(Color::Yellow) | bold,
        text(HeadEllipsisCommand(cmd, max_len)) | color(Color::Yellow),
    });
}

// ============================================================
// Destructive warning banner (reused from trust_utils visual style)
// ============================================================

/// Render a yellow warning banner for destructive commands.
/// Caller determines destructiveness — we only draw the banner.
[[nodiscard]] inline Element DestructiveWarningBanner(
    std::string_view reason = "This command may permanently delete or overwrite files.")
{
    return vbox({
        hbox({
            text("⚠ ") | color(Color::Yellow) | bold,
            text(std::string{reason}) | color(Color::Yellow) | bold,
        }),
    }) | borderStyled(Color::Yellow) | bgcolor(Color::RGB(40, 30, 0));
}

} // namespace cc::ui::permissions::components
