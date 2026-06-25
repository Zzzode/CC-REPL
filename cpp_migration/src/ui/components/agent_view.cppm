/// @file agent_view.cppm
/// @brief Agent state and management view - displays running sub-agents,
/// their status, task assignments, and allows lifecycle control.
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <cstdint>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.components.agent_view;

import cc.types.types;

export namespace cc::ui::components::agent_view {
using namespace ftxui;

// ============================================================
// Types
// ============================================================

/// Agent execution state
enum class AgentState : std::uint8_t {
    Idle,           // Ready but not working
    Thinking,       // Processing/reasoning
    ToolUse,        // Executing a tool
    WaitingInput,   // Waiting for user input
    Completed,      // Finished successfully
    Failed,         // Failed with error
    Cancelled,      // Cancelled by user
};

/// A tool call being made by the agent
struct AgentToolCall {
    std::string tool_name;
    std::string status;     // Brief status text
    std::chrono::milliseconds elapsed{0};
};

/// Data for a single agent entry
struct AgentEntry {
    std::string id;
    std::string name;           // Display name
    std::string model;          // Model being used
    AgentState state;
    std::string current_task;   // What it's working on
    std::optional<AgentToolCall> active_tool;
    int messages_count = 0;
    int tool_calls_count = 0;
    double cost_usd = 0.0;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::milliseconds elapsed{0};
    std::optional<std::string> error_message;
    std::optional<double> progress;  // 0.0 - 1.0 if known
};

/// Options for the agent view
struct AgentViewOptions {
    std::vector<AgentEntry> agents;
    int selected_index = 0;
    bool show_details = true;

    std::function<void(const std::string& agent_id)> on_cancel;
    std::function<void(const std::string& agent_id)> on_retry;
    std::function<void(const std::string& agent_id)> on_view_messages;
    std::function<void()> on_close;
};

// ============================================================
// Rendering Helpers
// ============================================================

/// Get state icon and color
[[nodiscard]] inline std::pair<std::string, Color> state_display(AgentState state) {
    switch (state) {
        case AgentState::Idle:         return {"○", Color::GrayLight};
        case AgentState::Thinking:     return {"◐", Color::Cyan};
        case AgentState::ToolUse:      return {"⚡", Color::Yellow};
        case AgentState::WaitingInput: return {"⏳", Color::Magenta};
        case AgentState::Completed:    return {"✓", Color::Green};
        case AgentState::Failed:       return {"✗", Color::Red};
        case AgentState::Cancelled:    return {"⊘", Color::GrayDark};
    }
    return {"?", Color::White};
}

/// Get state label
[[nodiscard]] inline std::string state_label(AgentState state) {
    switch (state) {
        case AgentState::Idle:         return "Idle";
        case AgentState::Thinking:     return "Thinking";
        case AgentState::ToolUse:      return "Tool Use";
        case AgentState::WaitingInput: return "Waiting";
        case AgentState::Completed:    return "Done";
        case AgentState::Failed:       return "Failed";
        case AgentState::Cancelled:    return "Cancelled";
    }
    return "Unknown";
}

/// Format elapsed time
[[nodiscard]] inline std::string format_elapsed(std::chrono::milliseconds ms) {
    auto secs = ms.count() / 1000;
    if (secs < 60) return std::format("{}s", secs);
    if (secs < 3600) return std::format("{}m {}s", secs / 60, secs % 60);
    return std::format("{}h {}m", secs / 3600, (secs % 3600) / 60);
}

// ============================================================
// Element Rendering
// ============================================================

/// Render a single agent entry
[[nodiscard]] inline Element RenderAgentEntry(
    const AgentEntry& agent, bool selected) {

    auto [icon, icon_color] = state_display(agent.state);

    // Top line: icon, name, model, elapsed
    auto header = hbox({
        text(" " + icon + " ") | color(icon_color),
        text(agent.name) | bold | color(selected ? Color::White : Color::GrayLight),
        text(" (" + agent.model + ")") | dim,
        filler(),
        text(format_elapsed(agent.elapsed)) | dim | color(Color::GrayDark),
        text(" "),
    });

    // Task/status line
    Elements body_parts = {header};

    if (!agent.current_task.empty()) {
        body_parts.push_back(hbox({
            text("   ") | dim,
            text("Task: ") | dim,
            text(agent.current_task) | color(Color::Cyan)
                | size(WIDTH, LESS_THAN, 60),
        }));
    }

    // Active tool
    if (agent.active_tool) {
        body_parts.push_back(hbox({
            text("   ⚡ ") | color(Color::Yellow),
            text(agent.active_tool->tool_name) | color(Color::Magenta),
            text(" - " + agent.active_tool->status) | dim,
        }));
    }

    // Progress bar if available
    if (agent.progress) {
        body_parts.push_back(hbox({
            text("   ") | dim,
            gauge(*agent.progress) | color(Color::Cyan) | flex,
            text(std::format(" {:.0f}%", *agent.progress * 100)) | dim,
            text(" "),
        }));
    }

    // Error message
    if (agent.error_message) {
        body_parts.push_back(hbox({
            text("   ✗ ") | color(Color::Red),
            text(*agent.error_message) | color(Color::Red) | dim,
        }));
    }

    // Stats line
    body_parts.push_back(hbox({
        text("   ") | dim,
        text(std::format("{} msgs", agent.messages_count)) | dim,
        text(" · ") | dim,
        text(std::format("{} tools", agent.tool_calls_count)) | dim,
        text(" · ") | dim,
        text(std::format("${:.4f}", agent.cost_usd)) | dim | color(Color::GrayDark),
    }));

    auto result = vbox(body_parts);
    if (selected) {
        result = result | bgcolor(Color::RGB(25, 35, 50));
    }
    return result | borderLight;
}

/// Render the full agent view
[[nodiscard]] inline Element RenderAgentView(const AgentViewOptions& opts) {
    // Header with summary stats
    int active = 0, completed = 0, failed = 0;
    double total_cost = 0.0;
    for (const auto& a : opts.agents) {
        if (a.state == AgentState::Thinking || a.state == AgentState::ToolUse) ++active;
        if (a.state == AgentState::Completed) ++completed;
        if (a.state == AgentState::Failed) ++failed;
        total_cost += a.cost_usd;
    }

    auto summary = hbox({
        text(" 🤖 Agents ") | bold | color(Color::Cyan),
        text(std::format("({} total", opts.agents.size())) | dim,
        text(active > 0 ? std::format(", {} active", active) : "") | color(Color::Green),
        text(completed > 0 ? std::format(", {} done", completed) : "") | color(Color::Blue),
        text(failed > 0 ? std::format(", {} failed", failed) : "") | color(Color::Red),
        text(")") | dim,
        filler(),
        text(std::format("${:.4f}", total_cost)) | dim | color(Color::GrayDark),
        text(" "),
    });

    // Agent list
    Elements agent_elements;
    for (int i = 0; i < static_cast<int>(opts.agents.size()); ++i) {
        agent_elements.push_back(
            RenderAgentEntry(opts.agents[i], i == opts.selected_index));
    }

    auto list = vbox(agent_elements) | vscroll_indicator | yframe | flex;

    // Action bar
    auto actions = hbox({
        text(" [c]") | color(Color::Cyan), text("ancel "),
        text("[r]") | color(Color::Cyan), text("etry "),
        text("[v]") | color(Color::Cyan), text("iew messages "),
        text("[Esc]") | color(Color::Cyan), text(" close"),
    }) | dim;

    return vbox({
        summary,
        separator(),
        list,
        separator(),
        actions,
    }) | borderRounded;
}

// ============================================================
// Interactive Component
// ============================================================

/// Create an agent view component
[[nodiscard]] inline Component AgentView(AgentViewOptions options) {
    auto state = std::make_shared<AgentViewOptions>(std::move(options));

    return Renderer([state] {
        return RenderAgentView(*state);
    }) | CatchEvent([state](Event event) -> bool {
        int count = static_cast<int>(state->agents.size());

        if (event == Event::Escape) {
            if (state->on_close) state->on_close();
            return true;
        }
        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->selected_index = std::max(0, state->selected_index - 1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->selected_index = std::min(count - 1, state->selected_index + 1);
            return true;
        }

        if (count == 0) return false;
        const auto& agent = state->agents[state->selected_index];

        if (event == Event::Character('c')) {
            if (state->on_cancel) state->on_cancel(agent.id);
            return true;
        }
        if (event == Event::Character('r')) {
            if (state->on_retry) state->on_retry(agent.id);
            return true;
        }
        if (event == Event::Character('v') || event == Event::Return) {
            if (state->on_view_messages) state->on_view_messages(agent.id);
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::components::agent_view
