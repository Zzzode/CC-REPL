/// @file repl_screen.cppm
/// @brief Main REPL screen layout: message list, input area, status bar orchestration.
/// Migrated from src/screens/REPL.tsx
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <variant>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>

export module cc.ui.repl_screen;

import cc.ui.task_list_ui;
import cc.ui.team_status;

export namespace cc::ui::repl_screen {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// REPL screen mode
enum class ReplMode : std::uint8_t {
    Normal,           // Standard input/output
    PermissionPrompt, // Waiting for permission approval
    Elicitation,      // MCP elicitation dialog
    HookPrompt,       // Hook prompt dialog
    MessageSelector,  // Editing/rewind message selector
    CostThreshold,    // Cost threshold dialog
    IdleReturn,       // Idle return dialog
    TasksView,        // Background tasks panel
    TeamsView,        // Teams panel
};

/// Input area mode
enum class InputMode : std::uint8_t {
    Insert,       // Normal text input
    Command,      // Slash command mode
    Vim,          // Vim-style editing
    Search,       // Search/filter mode
};

/// Spinner display mode
enum class SpinnerMode : std::uint8_t {
    Hidden,
    Thinking,     // Model is generating
    ToolUse,      // Executing a tool
    Briefing,     // Brief idle status
};

/// Status bar data
struct StatusBarData {
    std::string model_name;
    std::optional<std::string> session_name;
    std::optional<double> cost_usd;
    int input_tokens = 0;
    int output_tokens = 0;
    bool is_fast_mode = false;
    bool is_auto_mode = false;
    std::optional<std::string> agent_name;
    std::optional<std::string> effort_level;
};

/// Message display entry (simplified from full message type)
struct MessageDisplayEntry {
    std::string id;
    std::string role;           // "user", "assistant", "system"
    std::string content_preview;
    bool is_thinking = false;
    bool is_tool_use = false;
    std::optional<std::string> tool_name;
    std::chrono::system_clock::time_point timestamp;
};

/// Permission request info for display
struct PermissionRequestInfo {
    std::string tool_name;
    std::string description;
    std::optional<std::string> file_path;
    std::vector<std::string> risk_labels;
};

/// REPL screen state
struct ReplScreenState {
    ReplMode mode = ReplMode::Normal;
    InputMode input_mode = InputMode::Insert;
    SpinnerMode spinner_mode = SpinnerMode::Hidden;

    // Messages
    std::vector<MessageDisplayEntry> messages;
    int scroll_offset = 0;

    // Input
    std::string input_text;
    std::string input_placeholder;
    std::vector<std::string> autocomplete_suggestions;
    int autocomplete_index = -1;

    // Status
    StatusBarData status_bar;

    // Contextual panels
    std::optional<PermissionRequestInfo> permission_request;
    std::optional<std::string> spinner_verb;

    // Team/task state
    int background_task_count = 0;
    int teammate_count = 0;
    bool teams_footer_selected = false;
};

// ============================================================
// Rendering: Status Bar
// ============================================================

/// Render the top status bar
[[nodiscard]] inline Element RenderStatusBar(const StatusBarData& data) {
    Elements left_parts = {
        text(" ") | dim,
    };

    // Model name
    left_parts.push_back(
        text(data.model_name) | bold | color(Color::Cyan));

    // Fast mode indicator
    if (data.is_fast_mode) {
        left_parts.push_back(text(" ⚡") | color(Color::Yellow));
    }

    // Auto mode indicator
    if (data.is_auto_mode) {
        left_parts.push_back(text(" [auto]") | color(Color::Magenta));
    }

    // Agent name
    if (data.agent_name) {
        left_parts.push_back(text(" @") | dim);
        left_parts.push_back(text(*data.agent_name) | color(Color::Blue));
    }

    // Effort level
    if (data.effort_level) {
        left_parts.push_back(text(" [" + *data.effort_level + "]") | dim);
    }

    Elements right_parts;

    // Token counts
    if (data.input_tokens > 0 || data.output_tokens > 0) {
        right_parts.push_back(
            text(std::format("{}↓ {}↑",
                data.input_tokens, data.output_tokens)) | dim);
    }

    // Cost
    if (data.cost_usd) {
        right_parts.push_back(text(" ") | dim);
        right_parts.push_back(
            text(std::format("${:.4f}", *data.cost_usd)) | dim | color(Color::Green));
    }

    // Session name
    if (data.session_name) {
        right_parts.push_back(text(" │ ") | dim);
        right_parts.push_back(text(*data.session_name) | dim);
    }

    right_parts.push_back(text(" ") | dim);

    return hbox({
        hbox(left_parts),
        filler(),
        hbox(right_parts),
    });
}

// ============================================================
// Rendering: Spinner
// ============================================================

/// Render the spinner/activity indicator
[[nodiscard]] inline Element RenderSpinner(
    SpinnerMode mode, const std::optional<std::string>& verb) {

    if (mode == SpinnerMode::Hidden) return text("");

    std::string prefix;
    Color spinner_color;
    switch (mode) {
        case SpinnerMode::Thinking:
            prefix = "◐ Thinking";
            spinner_color = Color::Cyan;
            break;
        case SpinnerMode::ToolUse:
            prefix = "⟳ Running";
            spinner_color = Color::Yellow;
            break;
        case SpinnerMode::Briefing:
            prefix = "… Idle";
            spinner_color = Color::GrayLight;
            break;
        default:
            return text("");
    }

    if (verb) {
        prefix += ": " + *verb;
    }

    return hbox({
        text(" " + prefix) | color(spinner_color),
    });
}

// ============================================================
// Rendering: Message List
// ============================================================

/// Render a single message row (simplified)
[[nodiscard]] inline Element RenderMessageRow(
    const MessageDisplayEntry& msg, bool is_last) {

    Color role_color;
    std::string role_prefix;
    if (msg.role == "user") {
        role_color = Color::Green;
        role_prefix = "❯ ";
    } else if (msg.role == "assistant") {
        role_color = Color::Cyan;
        role_prefix = "● ";
    } else {
        role_color = Color::GrayDark;
        role_prefix = "  ";
    }

    Elements parts;
    parts.push_back(text(role_prefix) | color(role_color));

    if (msg.is_thinking) {
        parts.push_back(text("(thinking) ") | dim | color(Color::GrayLight));
    }

    if (msg.is_tool_use && msg.tool_name) {
        parts.push_back(text("[" + *msg.tool_name + "] ") | color(Color::Yellow));
    }

    parts.push_back(text(msg.content_preview) | color(Color::White));

    return hbox(parts);
}

/// Render the message list area
[[nodiscard]] inline Element RenderMessageList(
    const std::vector<MessageDisplayEntry>& messages, int scroll_offset) {

    if (messages.empty()) {
        return vbox({
            text("") | dim,
            text(" Welcome! Type a message to begin.") | dim | center,
            text("") | dim,
        }) | flex;
    }

    Elements rows;
    int start = std::max(0, static_cast<int>(messages.size()) - 50 + scroll_offset);
    int end = static_cast<int>(messages.size());

    for (int i = start; i < end; ++i) {
        rows.push_back(RenderMessageRow(messages[i], i == end - 1));
    }

    return vbox(rows) | vscroll_indicator | yframe | flex;
}

// ============================================================
// Rendering: Input Area
// ============================================================

/// Render the input area
[[nodiscard]] inline Element RenderInputArea(
    const std::string& input_text,
    const std::string& placeholder,
    InputMode mode) {

    std::string mode_indicator;
    Color mode_color;
    switch (mode) {
        case InputMode::Insert:
            mode_indicator = "❯";
            mode_color = Color::Green;
            break;
        case InputMode::Command:
            mode_indicator = "/";
            mode_color = Color::Cyan;
            break;
        case InputMode::Vim:
            mode_indicator = ":";
            mode_color = Color::Yellow;
            break;
        case InputMode::Search:
            mode_indicator = "?";
            mode_color = Color::Magenta;
            break;
    }

    if (input_text.empty()) {
        return hbox({
            text(" " + mode_indicator + " ") | color(mode_color),
            text(placeholder) | dim,
        });
    }

    return hbox({
        text(" " + mode_indicator + " ") | color(mode_color),
        text(input_text) | color(Color::White),
    });
}

// ============================================================
// Rendering: Full Screen Layout
// ============================================================

/// Render the full REPL screen
[[nodiscard]] inline Element RenderReplScreen(const ReplScreenState& state) {
    Elements layout;

    // Status bar (top)
    layout.push_back(RenderStatusBar(state.status_bar));
    layout.push_back(separator());

    // Message list (main area)
    layout.push_back(
        RenderMessageList(state.messages, state.scroll_offset));

    // Spinner (between messages and input)
    if (state.spinner_mode != SpinnerMode::Hidden) {
        layout.push_back(RenderSpinner(state.spinner_mode, state.spinner_verb));
    }

    layout.push_back(separator());

    // Input area (bottom)
    layout.push_back(RenderInputArea(
        state.input_text, state.input_placeholder, state.input_mode));

    // Footer with task/team counts
    if (state.background_task_count > 0 || state.teammate_count > 0) {
        Elements footer_parts;
        if (state.background_task_count > 0) {
            footer_parts.push_back(
                text(std::format(" {}⏳", state.background_task_count))
                    | dim | color(Color::Cyan));
        }
        if (state.teammate_count > 0) {
            footer_parts.push_back(
                text(std::format(" {}👥", state.teammate_count))
                    | dim | color(Color::Magenta));
        }
        layout.push_back(hbox(footer_parts));
    }

    return vbox(layout) | border;
}

// ============================================================
// Screen Options & Component
// ============================================================

/// Options for the REPL screen component
struct ReplScreenOptions {
    ReplScreenState initial_state;

    // Callbacks
    std::function<void(const std::string& input)> on_submit;
    std::function<void()> on_interrupt;      // Ctrl+C
    std::function<void()> on_exit;           // Ctrl+D
    std::function<void(const std::string& cmd)> on_command;
    std::function<void(bool accepted)> on_permission_response;
};

/// Create the REPL screen component
[[nodiscard]] inline Component ReplScreen(ReplScreenOptions options) {
    auto state = std::make_shared<ReplScreenState>(
        std::move(options.initial_state));
    auto callbacks = std::make_shared<ReplScreenOptions>(std::move(options));

    return Renderer([state] {
        return RenderReplScreen(*state);
    }) | CatchEvent([state, callbacks](Event event) -> bool {
        // Ctrl+C
        if (event == Event::Character('\x03')) {
            if (callbacks->on_interrupt) callbacks->on_interrupt();
            return true;
        }
        // Ctrl+D
        if (event == Event::Character('\x04')) {
            if (callbacks->on_exit) callbacks->on_exit();
            return true;
        }
        // Enter to submit
        if (event == Event::Return && !state->input_text.empty()) {
            if (state->input_mode == InputMode::Command) {
                if (callbacks->on_command)
                    callbacks->on_command(state->input_text);
            } else {
                if (callbacks->on_submit)
                    callbacks->on_submit(state->input_text);
            }
            state->input_text.clear();
            return true;
        }
        return false;
    });
}

} // namespace cc::ui::repl_screen
