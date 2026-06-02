/// @file components.cppm
/// @brief Reusable UI components for the Claude Code REPL.
/// Each component renders a specific visual element using FTXUI:
/// message rows, tool displays, permission prompts, diffs, etc.
module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <functional>
#include <chrono>
#include <ranges>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.components;

import cc.types.types;
import cc.ui.markdown;

export namespace cc::ui {

// ============================================================
// MessageRow - displays a single conversation message
// ============================================================

/// Visual style for a message row
struct MessageStyle {
    ftxui::Color prefix_color;
    std::string prefix_label;
    bool show_timestamp = false;
};

/// Render a single message with role prefix and markdown content
[[nodiscard]] inline ftxui::Element render_message_row(
    const cc::core::Message& msg, const MessageStyle& style) {
    // Extract text content from the message variant
    auto text_content = std::visit([](const auto& m) -> std::string {
        std::string result;
        for (const auto& block : m.content) {
            if (auto* tb = std::get_if<cc::core::TextBlock>(&block)) {
                if (!result.empty()) result += '\n';
                result += tb->text;
            }
        }
        return result;
    }, msg);

    auto prefix = ftxui::text(style.prefix_label + " ")
                  | ftxui::color(style.prefix_color) | ftxui::bold;
    auto body = render_markdown(text_content);

    return ftxui::hbox({prefix, body}) | ftxui::xflex;
}

/// Create the appropriate message style based on role
[[nodiscard]] inline MessageStyle style_for_role(cc::core::Role role) {
    switch (role) {
        case cc::core::Role::User:
            return {ftxui::Color::Green, "You:", false};
        case cc::core::Role::Assistant:
            return {ftxui::Color::Cyan, "Claude:", false};
        case cc::core::Role::System:
            return {ftxui::Color::Yellow, "System:", false};
        case cc::core::Role::Tool:
            return {ftxui::Color::Magenta, "Tool:", false};
    }
    return {ftxui::Color::White, "???:", false};
}

// ============================================================
// ToolUseDisplay - shows tool invocation and results
// ============================================================

/// Data representing a tool invocation for display
struct ToolUseDisplayData {
    std::string tool_name;
    std::string input_summary;      // Brief description of tool input
    std::optional<std::string> output_preview;  // Truncated output
    bool is_running = false;
    bool is_error = false;
    std::chrono::milliseconds duration{0};
};

/// Render a tool use block with collapsible details
[[nodiscard]] inline ftxui::Element render_tool_use(const ToolUseDisplayData& data) {
    // Tool name header with status indicator
    auto status_icon = data.is_running ? "⟳" : (data.is_error ? "✗" : "✓");
    auto status_color = data.is_running ? ftxui::Color::Yellow
                        : (data.is_error ? ftxui::Color::Red : ftxui::Color::Green);

    auto header = ftxui::hbox({
        ftxui::text(status_icon) | ftxui::color(status_color),
        ftxui::text(" " + data.tool_name) | ftxui::bold | ftxui::color(ftxui::Color::Magenta),
        ftxui::text(std::format(" ({}ms)", data.duration.count()))
            | ftxui::color(ftxui::Color::GrayDark),
    });

    // Input summary
    auto input_line = ftxui::hbox({
        ftxui::text("  → ") | ftxui::color(ftxui::Color::GrayDark),
        ftxui::text(data.input_summary) | ftxui::dim,
    });

    std::vector<ftxui::Element> elements = {header, input_line};

    // Output preview (if available)
    if (data.output_preview) {
        auto output_line = ftxui::hbox({
            ftxui::text("  ← ") | ftxui::color(ftxui::Color::GrayDark),
            ftxui::text(*data.output_preview) | ftxui::dim,
        });
        elements.push_back(output_line);
    }

    return ftxui::vbox(elements) | ftxui::borderLight
           | ftxui::color(ftxui::Color::GrayLight);
}

// ============================================================
// PermissionPrompt - asks user for tool permission
// ============================================================

/// Permission prompt options
enum class PermissionChoice : std::uint8_t {
    Allow,          // Allow this invocation only
    AllowAlways,    // Allow this tool always
    Deny,           // Deny this invocation
    DenyAlways,     // Deny this tool always
};

/// Data for rendering a permission prompt
struct PermissionPromptData {
    std::string tool_name;
    std::string description;
    std::string input_preview;
    std::vector<std::string> affected_paths;
};

/// Render a permission prompt element
[[nodiscard]] inline ftxui::Element render_permission_prompt(
    const PermissionPromptData& data) {
    auto title = ftxui::hbox({
        ftxui::text("⚠ Permission Required: ") | ftxui::color(ftxui::Color::Yellow) | ftxui::bold,
        ftxui::text(data.tool_name) | ftxui::color(ftxui::Color::Magenta),
    });

    auto desc = ftxui::text("  " + data.description) | ftxui::dim;
    auto input = ftxui::text("  Input: " + data.input_preview)
                 | ftxui::color(ftxui::Color::GrayLight);

    std::vector<ftxui::Element> elements = {title, desc, input};

    // Show affected file paths
    if (!data.affected_paths.empty()) {
        elements.push_back(ftxui::text("  Affected:") | ftxui::bold);
        for (const auto& path : data.affected_paths | std::views::take(5)) {
            elements.push_back(ftxui::text("    " + path)
                               | ftxui::color(ftxui::Color::Cyan));
        }
        if (data.affected_paths.size() > 5) {
            elements.push_back(ftxui::text(std::format("    ... and {} more",
                               data.affected_paths.size() - 5)) | ftxui::dim);
        }
    }

    // Action hint
    elements.push_back(ftxui::text("  [y]es / [n]o / [a]lways / [d]eny always")
                       | ftxui::color(ftxui::Color::Yellow));

    return ftxui::vbox(elements) | ftxui::borderDouble
           | ftxui::bgcolor(ftxui::Color::RGB(40, 30, 0));
}

// ============================================================
// SearchBox - typeahead search component
// ============================================================

/// Search result item for display
struct SearchResult {
    std::string label;
    std::string detail;
    std::optional<std::string> icon;
};

/// Render a search box with results dropdown
[[nodiscard]] inline ftxui::Element render_search_box(
    const std::string& query,
    const std::vector<SearchResult>& results,
    std::int32_t selected_index) {

    auto input_line = ftxui::hbox({
        ftxui::text("🔍 ") | ftxui::color(ftxui::Color::Cyan),
        ftxui::text(query.empty() ? "Type to search..." : query),
    });

    std::vector<ftxui::Element> result_elements;
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(results.size()); ++i) {
        const auto& r = results[i];
        auto icon = r.icon.value_or("  ");
        auto line = ftxui::hbox({
            ftxui::text(icon + " "),
            ftxui::text(r.label) | ftxui::bold,
            ftxui::text(" — " + r.detail) | ftxui::dim,
        });
        // Highlight selected item
        if (i == selected_index) {
            line = line | ftxui::inverted;
        }
        result_elements.push_back(line);
    }

    std::vector<ftxui::Element> all = {input_line, ftxui::separator()};
    all.insert(all.end(), result_elements.begin(), result_elements.end());
    return ftxui::vbox(all) | ftxui::border;
}

// ============================================================
// DiffView - shows file diffs with colors
// ============================================================

/// Type of diff line
enum class DiffLineKind : std::uint8_t {
    Context,    // Unchanged line
    Added,      // Added line (green)
    Removed,    // Removed line (red)
    Header,     // @@ hunk header @@
};

/// Single line in a diff view
struct DiffLine {
    DiffLineKind kind;
    std::string content;
    std::optional<std::uint32_t> old_line_num;
    std::optional<std::uint32_t> new_line_num;
};

/// Render a unified diff view with colored additions/deletions
[[nodiscard]] inline ftxui::Element render_diff_view(
    const std::string& file_path,
    const std::vector<DiffLine>& lines) {

    auto header = ftxui::text("  " + file_path) | ftxui::bold
                  | ftxui::color(ftxui::Color::White);

    std::vector<ftxui::Element> diff_lines;
    for (const auto& line : lines) {
        ftxui::Color line_color;
        std::string prefix;
        switch (line.kind) {
            case DiffLineKind::Added:
                line_color = ftxui::Color::Green;
                prefix = "+";
                break;
            case DiffLineKind::Removed:
                line_color = ftxui::Color::Red;
                prefix = "-";
                break;
            case DiffLineKind::Header:
                line_color = ftxui::Color::Cyan;
                prefix = "@";
                break;
            default:
                line_color = ftxui::Color::GrayLight;
                prefix = " ";
                break;
        }
        diff_lines.push_back(
            ftxui::text(prefix + " " + line.content) | ftxui::color(line_color));
    }

    std::vector<ftxui::Element> all = {header, ftxui::separator()};
    all.insert(all.end(), diff_lines.begin(), diff_lines.end());
    return ftxui::vbox(all) | ftxui::borderRounded;
}

// ============================================================
// ProgressBar - visual progress indicator
// ============================================================

/// Render a progress bar with percentage and optional label
[[nodiscard]] inline ftxui::Element render_progress_bar(
    double progress, const std::string& label = "") {
    // Clamp progress to [0, 1]
    auto clamped = std::clamp(progress, 0.0, 1.0);

    auto bar = ftxui::gauge(clamped) | ftxui::color(ftxui::Color::Cyan);
    auto pct = ftxui::text(std::format(" {:3.0f}%", clamped * 100))
               | ftxui::color(ftxui::Color::White);

    if (label.empty()) {
        return ftxui::hbox({bar | ftxui::flex, pct});
    }
    auto lbl = ftxui::text(label + " ") | ftxui::color(ftxui::Color::GrayLight);
    return ftxui::hbox({lbl, bar | ftxui::flex, pct});
}

// ============================================================
// Notification - transient notification display
// ============================================================

/// Notification severity level
enum class NotificationLevel : std::uint8_t {
    Info, Success, Warning, Error,
};

/// Notification data
struct NotificationData {
    NotificationLevel level;
    std::string title;
    std::optional<std::string> body;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::seconds ttl{5};
};

/// Render a notification banner
[[nodiscard]] inline ftxui::Element render_notification(const NotificationData& data) {
    ftxui::Color border_color;
    std::string icon;
    switch (data.level) {
        case NotificationLevel::Info:    icon = "ℹ"; border_color = ftxui::Color::Blue; break;
        case NotificationLevel::Success: icon = "✓"; border_color = ftxui::Color::Green; break;
        case NotificationLevel::Warning: icon = "⚠"; border_color = ftxui::Color::Yellow; break;
        case NotificationLevel::Error:   icon = "✗"; border_color = ftxui::Color::Red; break;
    }

    auto title_el = ftxui::hbox({
        ftxui::text(icon + " ") | ftxui::color(border_color),
        ftxui::text(data.title) | ftxui::bold,
    });

    if (data.body) {
        return ftxui::vbox({
            title_el,
            ftxui::text("  " + *data.body) | ftxui::dim,
        }) | ftxui::borderLight | ftxui::color(border_color);
    }
    return title_el | ftxui::borderLight | ftxui::color(border_color);
}

} // namespace cc::ui
