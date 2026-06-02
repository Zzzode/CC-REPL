/// @file tool_use_message.cppm
/// @brief Tool call message rendering with FTXUI - displays tool invocations,
/// their parameters, status, and results in the conversation view.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>
#include <variant>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

export module cc.ui.messages.tool_use_message;

import cc.types.types;

export namespace cc::ui::messages::tool_use_message {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Status of a tool invocation
enum class ToolStatus : std::uint8_t {
    Pending,    // Waiting for permission
    Running,    // Currently executing
    Success,    // Completed successfully
    Error,      // Completed with error
    Cancelled,  // Cancelled by user
};

/// Parameter display entry
struct ToolParam {
    std::string name;
    std::string value;
    bool is_truncated = false;
};

/// A single tool use message for display
struct ToolUseMessageData {
    std::string tool_use_id;
    std::string tool_name;
    std::string server_name;    // MCP server or "built-in"
    ToolStatus status;
    std::vector<ToolParam> parameters;
    std::optional<std::string> result_preview;
    std::optional<std::string> error_message;
    std::chrono::milliseconds duration{0};
    std::optional<std::string> file_path;   // Primary affected file
    bool is_collapsed = true;
};

/// Options for the tool use message component
struct ToolUseMessageOptions {
    ToolUseMessageData data;
    bool show_params = false;
    bool show_result = false;
    int max_result_lines = 10;
    std::function<void()> on_toggle_collapse;
    std::function<void()> on_copy_result;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get status icon and color
[[nodiscard]] inline std::pair<std::string, Color> status_display(ToolStatus status) {
    switch (status) {
        case ToolStatus::Pending:   return {"◯", Color::Yellow};
        case ToolStatus::Running:   return {"⟳", Color::Cyan};
        case ToolStatus::Success:   return {"✓", Color::Green};
        case ToolStatus::Error:     return {"✗", Color::Red};
        case ToolStatus::Cancelled: return {"⊘", Color::GrayDark};
    }
    return {"?", Color::White};
}

/// Get tool category icon based on name
[[nodiscard]] inline std::string tool_icon(const std::string& tool_name) {
    if (tool_name.find("read") != std::string::npos ||
        tool_name.find("Read") != std::string::npos) return "📖";
    if (tool_name.find("write") != std::string::npos ||
        tool_name.find("Write") != std::string::npos ||
        tool_name.find("Edit") != std::string::npos) return "✏️";
    if (tool_name.find("Bash") != std::string::npos ||
        tool_name.find("bash") != std::string::npos) return "⚡";
    if (tool_name.find("Glob") != std::string::npos ||
        tool_name.find("glob") != std::string::npos) return "🔍";
    if (tool_name.find("Grep") != std::string::npos ||
        tool_name.find("grep") != std::string::npos) return "🔎";
    if (tool_name.find("Web") != std::string::npos) return "🌐";
    return "🔧";
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a tool use message as an Element
[[nodiscard]] inline Element RenderToolUseMessage(const ToolUseMessageOptions& opts) {
    const auto& data = opts.data;
    auto [status_icon, status_color] = status_display(data.status);

    // Header line: [icon] tool_name (server) - duration
    Elements header_parts = {
        text(status_icon + " ") | color(status_color),
        text(tool_icon(data.tool_name) + " ") | dim,
        text(data.tool_name) | bold | color(Color::Magenta),
    };

    if (!data.server_name.empty() && data.server_name != "built-in") {
        header_parts.push_back(text(" (" + data.server_name + ")") | dim);
    }

    if (data.duration.count() > 0) {
        header_parts.push_back(filler());
        std::string dur_str;
        if (data.duration.count() >= 1000) {
            dur_str = std::format("{:.1f}s", data.duration.count() / 1000.0);
        } else {
            dur_str = std::format("{}ms", data.duration.count());
        }
        header_parts.push_back(text(dur_str) | color(Color::GrayDark));
    }

    auto header = hbox(header_parts);

    // File path if present
    Elements body_elements = {header};

    if (data.file_path) {
        body_elements.push_back(hbox({
            text("  → ") | color(Color::GrayDark),
            text(*data.file_path) | color(Color::Cyan) | dim,
        }));
    }

    // Parameters (when expanded)
    if (opts.show_params && !data.parameters.empty()) {
        body_elements.push_back(text("  Parameters:") | dim);
        for (const auto& param : data.parameters) {
            std::string val = param.value;
            if (param.is_truncated) {
                val = val.substr(0, 60) + "...";
            }
            body_elements.push_back(hbox({
                text("    " + param.name + ": ") | color(Color::Yellow),
                text(val) | dim,
            }));
        }
    }

    // Result preview or error
    if (data.status == ToolStatus::Error && data.error_message) {
        body_elements.push_back(hbox({
            text("  ✗ ") | color(Color::Red),
            text(*data.error_message) | color(Color::Red) | dim,
        }));
    } else if (opts.show_result && data.result_preview) {
        body_elements.push_back(separator() | dim);
        // Split result into lines, limit display
        std::string preview = *data.result_preview;
        int lines_shown = 0;
        size_t pos = 0;
        while (pos < preview.size() && lines_shown < opts.max_result_lines) {
            auto nl = preview.find('\n', pos);
            std::string line;
            if (nl == std::string::npos) {
                line = preview.substr(pos);
                pos = preview.size();
            } else {
                line = preview.substr(pos, nl - pos);
                pos = nl + 1;
            }
            body_elements.push_back(text("  │ " + line) | color(Color::GrayLight));
            ++lines_shown;
        }
        if (pos < preview.size()) {
            body_elements.push_back(text("  │ ...") | dim);
        }
    }

    // Collapse indicator
    if (data.is_collapsed && (data.result_preview || !data.parameters.empty())) {
        body_elements.push_back(
            text("  ▸ Press Enter to expand") | dim | color(Color::GrayDark));
    }

    // Border color based on status
    return vbox(body_elements) | borderLight | color(status_color);
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an interactive tool use message component
[[nodiscard]] inline Component ToolUseMessage(ToolUseMessageOptions options) {
    struct State {
        ToolUseMessageOptions opts;
        bool expanded = false;
    };

    auto state = std::make_shared<State>();
    state->opts = std::move(options);
    state->expanded = !state->opts.data.is_collapsed;

    return Renderer([state] {
        state->opts.show_params = state->expanded;
        state->opts.show_result = state->expanded;
        state->opts.data.is_collapsed = !state->expanded;
        return RenderToolUseMessage(state->opts);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::Return) {
            state->expanded = !state->expanded;
            if (state->opts.on_toggle_collapse) {
                state->opts.on_toggle_collapse();
            }
            return true;
        }
        if (event == Event::Character('c')) {
            if (state->opts.on_copy_result) {
                state->opts.on_copy_result();
            }
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::messages::tool_use_message
