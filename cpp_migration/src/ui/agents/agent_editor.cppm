/// @file agent_editor.cppm
/// @brief Agent management UI — list, detail, tab-based editor, and creation
/// wizard. Migrated from agents/AgentsList.tsx, AgentEditor.tsx,
/// AgentDetail.tsx, and CreateAgentWizard.tsx.
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
#include <ftxui/component/event.hpp>

export module cc.ui.agent_editor;

import cc.types.types;

export namespace cc::ui::agent_editor {
using namespace ftxui;

// ============================================================
// Data Model (from agents/types.ts)
// ============================================================

/// Agent execution status
enum class AgentStatus : std::uint8_t {
    Idle,
    Running,
    Error,
};

/// A single tool an agent can use
struct AgentTool {
    std::string name;
    std::string description;
    bool enabled;
};

/// Agent capability flags
struct AgentCapability {
    std::string id;
    std::string label;
    bool enabled;
};

/// Token usage statistics
struct TokenUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_write_tokens = 0;
};

/// A single step in an agent run
struct AgentStep {
    std::string type;       // "thinking", "tool_use", "text", "error"
    std::string content;
    std::chrono::system_clock::time_point timestamp;
    std::optional<TokenUsage> usage;
};

/// Configuration for an agent
struct AgentConfig {
    std::string name;
    std::string model;
    std::string instructions;
    std::vector<AgentTool> tools;
    std::vector<AgentCapability> capabilities;
    int max_turns = 25;
    double temperature = 1.0;
};

/// State of a single run
struct AgentRunState {
    std::string run_id;
    AgentStatus status;
    std::vector<AgentStep> steps;
    std::chrono::system_clock::time_point started_at;
    std::optional<std::chrono::system_clock::time_point> completed_at;
    std::optional<std::string> error;
};

/// Result of a completed run
struct AgentRunResult {
    std::string run_id;
    bool success;
    std::string output;
    std::vector<AgentStep> steps;
    TokenUsage total_usage;
    std::chrono::milliseconds duration{0};
};

/// Full agent definition
struct Agent {
    std::string id;
    std::string name;
    AgentStatus status;
    AgentConfig config;
    std::vector<AgentRunResult> run_history;
    std::optional<AgentRunState> current_run;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
};

// ============================================================
// Agent List (from AgentsList.tsx)
// ============================================================

/// Props for the agent list component
struct AgentListProps {
    std::vector<Agent> agents;
    std::optional<std::string> selected_id;
    std::function<void(const std::string& agent_id)> on_select;
    std::function<void()> on_create;
};

/// Get status indicator icon and color
[[nodiscard]] inline std::pair<std::string, Color> status_indicator(AgentStatus status) {
    switch (status) {
        case AgentStatus::Idle:    return {"●", Color::GrayLight};
        case AgentStatus::Running: return {"◉", Color::Green};
        case AgentStatus::Error:   return {"✗", Color::Red};
    }
    return {"?", Color::White};
}

/// Render a single agent list item
[[nodiscard]] inline Element RenderAgentListItem(
    const Agent& agent, bool selected) {

    auto [icon, icon_color] = status_indicator(agent.status);

    auto line = hbox({
        text(" " + icon + " ") | color(icon_color),
        text(agent.name) | color(selected ? Color::White : Color::GrayLight),
        filler(),
        text(agent.config.model) | dim,
        text(" "),
    });

    if (selected) {
        line = line | bgcolor(Color::RGB(30, 40, 60)) | bold;
    }
    return line;
}

/// Render the full agent list
[[nodiscard]] inline Element RenderAgentList(const AgentListProps& props) {
    Elements items;
    items.push_back(text(" Agents") | bold | color(Color::Cyan));
    items.push_back(separator());

    if (props.agents.empty()) {
        items.push_back(text("  No agents configured") | dim);
        items.push_back(text("  Press [n] to create one") | dim);
    } else {
        for (const auto& agent : props.agents) {
            bool is_selected = props.selected_id.has_value() &&
                               *props.selected_id == agent.id;
            items.push_back(RenderAgentListItem(agent, is_selected));
        }
    }

    items.push_back(separator());
    items.push_back(hbox({
        text(" [n]") | color(Color::Cyan), text("ew "),
        text("[Enter]") | color(Color::Cyan), text(" select "),
        text("[d]") | color(Color::Cyan), text("elete"),
    }) | dim);

    return vbox(items) | border;
}

/// Create an interactive agent list component
[[nodiscard]] inline Component AgentList(AgentListProps props) {
    struct State {
        AgentListProps props;
        int cursor = 0;
    };
    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderAgentList(state->props);
    }) | CatchEvent([state](Event event) -> bool {
        int count = static_cast<int>(state->props.agents.size());
        if (count == 0) {
            if (event == Event::Character('n')) {
                if (state->props.on_create) state->props.on_create();
                return true;
            }
            return false;
        }

        if (event == Event::ArrowUp || event == Event::Character('k')) {
            state->cursor = std::max(0, state->cursor - 1);
            state->props.selected_id = state->props.agents[state->cursor].id;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Character('j')) {
            state->cursor = std::min(count - 1, state->cursor + 1);
            state->props.selected_id = state->props.agents[state->cursor].id;
            return true;
        }
        if (event == Event::Return) {
            if (state->props.on_select) {
                state->props.on_select(state->props.agents[state->cursor].id);
            }
            return true;
        }
        if (event == Event::Character('n')) {
            if (state->props.on_create) state->props.on_create();
            return true;
        }
        return false;
    });
}

// ============================================================
// Agent Editor (from AgentEditor.tsx)
// ============================================================

/// Active tab in the editor
enum class EditorTab : std::uint8_t {
    Config,
    Runs,
    Settings,
};

/// Props for the agent editor
struct AgentEditorProps {
    Agent agent;
    std::function<void(const AgentConfig&)> on_save;
    std::function<void()> on_run;
    std::function<void()> on_delete;
};

/// Render tab bar
[[nodiscard]] inline Element RenderEditorTabs(EditorTab active) {
    auto tab_style = [&](EditorTab tab, const std::string& label) -> Element {
        if (tab == active) {
            return text(" " + label + " ") | bold | color(Color::Cyan)
                   | bgcolor(Color::RGB(30, 40, 60));
        }
        return text(" " + label + " ") | dim;
    };

    return hbox({
        tab_style(EditorTab::Config, "Config"),
        text(" │ ") | dim,
        tab_style(EditorTab::Runs, "Runs"),
        text(" │ ") | dim,
        tab_style(EditorTab::Settings, "Settings"),
    });
}

/// Render the config tab content
[[nodiscard]] inline Element RenderConfigTab(const AgentConfig& config) {
    Elements items;

    items.push_back(hbox({
        text("  Name: ") | dim,
        text(config.name) | bold,
    }));
    items.push_back(hbox({
        text("  Model: ") | dim,
        text(config.model) | color(Color::Cyan),
    }));
    items.push_back(hbox({
        text("  Max Turns: ") | dim,
        text(std::format("{}", config.max_turns)),
    }));
    items.push_back(hbox({
        text("  Temperature: ") | dim,
        text(std::format("{:.1f}", config.temperature)),
    }));

    items.push_back(separator());
    items.push_back(text("  Instructions:") | dim);
    if (config.instructions.empty()) {
        items.push_back(text("    (none)") | dim);
    } else {
        // Truncate long instructions for display
        auto display = config.instructions.substr(
            0, std::min<size_t>(200, config.instructions.size()));
        if (config.instructions.size() > 200) display += "...";
        items.push_back(text("    " + display) | color(Color::GrayLight));
    }

    items.push_back(separator());
    items.push_back(text(std::format("  Tools ({}):", config.tools.size())) | dim);
    for (const auto& tool : config.tools) {
        auto icon = tool.enabled ? "✓" : "○";
        auto clr = tool.enabled ? Color::Green : Color::GrayDark;
        items.push_back(hbox({
            text("    " + std::string(icon) + " ") | color(clr),
            text(tool.name) | color(Color::GrayLight),
        }));
    }

    return vbox(items);
}

/// Render the runs tab content
[[nodiscard]] inline Element RenderRunsTab(const Agent& agent) {
    Elements items;

    if (agent.current_run.has_value()) {
        auto& run = *agent.current_run;
        auto [icon, clr] = status_indicator(run.status);
        items.push_back(hbox({
            text("  Active: ") | dim,
            text(icon + " ") | color(clr),
            text(run.run_id) | color(Color::Cyan),
            text(std::format(" ({} steps)", run.steps.size())) | dim,
        }));
        items.push_back(separator());
    }

    items.push_back(text(std::format("  History ({} runs):",
                         agent.run_history.size())) | dim);

    for (auto it = agent.run_history.rbegin();
         it != agent.run_history.rend() && it < agent.run_history.rbegin() + 10;
         ++it) {
        auto icon = it->success ? "✓" : "✗";
        auto clr = it->success ? Color::Green : Color::Red;
        items.push_back(hbox({
            text("    " + std::string(icon) + " ") | color(clr),
            text(it->run_id) | dim,
            text(std::format(" ({}ms, {} steps)",
                 it->duration.count(), it->steps.size())) | dim,
        }));
    }

    if (agent.run_history.empty() && !agent.current_run.has_value()) {
        items.push_back(text("    No runs yet") | dim);
    }

    return vbox(items);
}

/// Render the settings tab content
[[nodiscard]] inline Element RenderSettingsTab(const AgentConfig& config) {
    Elements items;

    items.push_back(text("  Capabilities:") | dim);
    for (const auto& cap : config.capabilities) {
        auto icon = cap.enabled ? "✓" : "○";
        auto clr = cap.enabled ? Color::Green : Color::GrayDark;
        items.push_back(hbox({
            text("    " + std::string(icon) + " ") | color(clr),
            text(cap.label) | color(Color::GrayLight),
        }));
    }

    if (config.capabilities.empty()) {
        items.push_back(text("    (no capabilities configured)") | dim);
    }

    return vbox(items);
}

/// Render the full agent editor
[[nodiscard]] inline Element RenderAgentEditor(
    const AgentEditorProps& props, EditorTab active_tab) {

    auto header = hbox({
        text(" ✎ ") | color(Color::Cyan),
        text(props.agent.name) | bold,
        filler(),
        text("[s]") | color(Color::Cyan), text("ave "),
        text("[r]") | color(Color::Cyan), text("un "),
        text("[D]") | color(Color::Red), text("elete "),
    });

    Element tab_content;
    switch (active_tab) {
        case EditorTab::Config:
            tab_content = RenderConfigTab(props.agent.config);
            break;
        case EditorTab::Runs:
            tab_content = RenderRunsTab(props.agent);
            break;
        case EditorTab::Settings:
            tab_content = RenderSettingsTab(props.agent.config);
            break;
    }

    return vbox({
        header,
        separator(),
        RenderEditorTabs(active_tab),
        separator(),
        tab_content | flex,
    }) | border;
}

/// Create an interactive agent editor component
[[nodiscard]] inline Component AgentEditor(AgentEditorProps props) {
    struct State {
        AgentEditorProps props;
        EditorTab active_tab = EditorTab::Config;
    };
    auto state = std::make_shared<State>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderAgentEditor(state->props, state->active_tab);
    }) | CatchEvent([state](Event event) -> bool {
        // Tab switching
        if (event == Event::Character('1')) {
            state->active_tab = EditorTab::Config;
            return true;
        }
        if (event == Event::Character('2')) {
            state->active_tab = EditorTab::Runs;
            return true;
        }
        if (event == Event::Character('3')) {
            state->active_tab = EditorTab::Settings;
            return true;
        }
        if (event == Event::Tab) {
            int t = static_cast<int>(state->active_tab);
            state->active_tab = static_cast<EditorTab>((t + 1) % 3);
            return true;
        }

        // Actions
        if (event == Event::Character('s')) {
            if (state->props.on_save)
                state->props.on_save(state->props.agent.config);
            return true;
        }
        if (event == Event::Character('r')) {
            if (state->props.on_run) state->props.on_run();
            return true;
        }
        if (event == Event::Character('D')) {
            if (state->props.on_delete) state->props.on_delete();
            return true;
        }
        return false;
    });
}

// ============================================================
// Agent Detail View (from AgentDetail.tsx)
// ============================================================

/// Props for agent detail view
struct AgentDetailProps {
    Agent agent;
    std::function<void()> on_back;
    std::function<void()> on_edit;
};

/// Render agent detail view
[[nodiscard]] inline Element RenderAgentDetail(const AgentDetailProps& props) {
    const auto& agent = props.agent;
    auto [icon, icon_color] = status_indicator(agent.status);

    Elements items;

    // Header
    items.push_back(hbox({
        text(" " + icon + " ") | color(icon_color),
        text(agent.name) | bold,
        text(" — ") | dim,
        text(agent.config.model) | color(Color::Cyan),
    }));
    items.push_back(separator());

    // Config summary
    items.push_back(text("  Configuration") | bold | dim);
    items.push_back(hbox({
        text("    Max Turns: ") | dim,
        text(std::format("{}", agent.config.max_turns)),
    }));
    items.push_back(hbox({
        text("    Tools: ") | dim,
        text(std::format("{}", agent.config.tools.size())),
    }));
    items.push_back(hbox({
        text("    Temperature: ") | dim,
        text(std::format("{:.1f}", agent.config.temperature)),
    }));

    // Run stats
    items.push_back(separator());
    items.push_back(text("  Run History") | bold | dim);
    items.push_back(hbox({
        text("    Total Runs: ") | dim,
        text(std::format("{}", agent.run_history.size())),
    }));

    int successes = 0;
    for (const auto& r : agent.run_history) {
        if (r.success) ++successes;
    }
    items.push_back(hbox({
        text("    Successful: ") | dim,
        text(std::format("{}", successes)) | color(Color::Green),
        text(std::format(" / {}", agent.run_history.size())) | dim,
    }));

    // Current run
    if (agent.current_run.has_value()) {
        items.push_back(separator());
        items.push_back(hbox({
            text("  ◉ Active Run: ") | color(Color::Green),
            text(agent.current_run->run_id) | color(Color::Cyan),
        }));
    }

    // Actions
    items.push_back(filler());
    items.push_back(separator());
    items.push_back(hbox({
        text(" [e]") | color(Color::Cyan), text("dit "),
        text("[Esc]") | color(Color::Cyan), text(" back"),
    }) | dim);

    return vbox(items) | border;
}

/// Create an interactive agent detail component
[[nodiscard]] inline Component AgentDetail(AgentDetailProps props) {
    auto state = std::make_shared<AgentDetailProps>(std::move(props));

    return Renderer([state] {
        return RenderAgentDetail(*state);
    }) | CatchEvent([state](Event event) -> bool {
        if (event == Event::Escape) {
            if (state->on_back) state->on_back();
            return true;
        }
        if (event == Event::Character('e')) {
            if (state->on_edit) state->on_edit();
            return true;
        }
        return false;
    });
}

// ============================================================
// Create Agent Wizard (from CreateAgentWizard.tsx)
// ============================================================

/// Wizard step identifier
enum class WizardStep : std::uint8_t {
    Name,
    Model,
    Instructions,
    Tools,
    Review,
};

/// Props for the create agent wizard
struct CreateWizardProps {
    std::function<void(const AgentConfig&)> on_complete;
    std::function<void()> on_cancel;
};

/// Wizard state holding in-progress configuration
struct WizardState {
    WizardStep current_step = WizardStep::Name;
    AgentConfig config;
    std::vector<std::string> available_models;
    std::vector<AgentTool> available_tools;
};

/// Render the step indicator
[[nodiscard]] inline Element RenderStepIndicator(WizardStep current) {
    constexpr std::pair<WizardStep, const char*> steps[] = {
        {WizardStep::Name, "Name"},
        {WizardStep::Model, "Model"},
        {WizardStep::Instructions, "Instructions"},
        {WizardStep::Tools, "Tools"},
        {WizardStep::Review, "Review"},
    };

    Elements parts;
    for (auto [step, label] : steps) {
        if (step == current) {
            parts.push_back(text(std::string(" ● ") + label + " ")
                            | bold | color(Color::Cyan));
        } else if (static_cast<int>(step) < static_cast<int>(current)) {
            parts.push_back(text(std::string(" ✓ ") + label + " ")
                            | color(Color::Green));
        } else {
            parts.push_back(text(std::string(" ○ ") + label + " ") | dim);
        }
        if (step != WizardStep::Review) {
            parts.push_back(text("→") | dim);
        }
    }
    return hbox(parts);
}

/// Render the wizard body for current step
[[nodiscard]] inline Element RenderWizardBody(const WizardState& ws) {
    switch (ws.current_step) {
        case WizardStep::Name:
            return vbox({
                text("  Enter agent name:") | bold,
                text("  " + (ws.config.name.empty()
                     ? "(type a name...)" : ws.config.name))
                    | color(ws.config.name.empty() ? Color::GrayDark : Color::White),
            });
        case WizardStep::Model:
            return vbox({
                text("  Select model:") | bold,
                text("  " + (ws.config.model.empty()
                     ? "(choose...)" : ws.config.model))
                    | color(ws.config.model.empty() ? Color::GrayDark : Color::Cyan),
            });
        case WizardStep::Instructions:
            return vbox({
                text("  System instructions:") | bold,
                text("  " + (ws.config.instructions.empty()
                     ? "(enter instructions...)"
                     : ws.config.instructions.substr(
                         0, std::min<size_t>(120, ws.config.instructions.size()))))
                    | color(ws.config.instructions.empty()
                            ? Color::GrayDark : Color::GrayLight),
            });
        case WizardStep::Tools: {
            Elements tool_items;
            tool_items.push_back(text("  Select tools:") | bold);
            for (const auto& tool : ws.config.tools) {
                auto icon = tool.enabled ? "✓" : "○";
                auto clr = tool.enabled ? Color::Green : Color::GrayDark;
                tool_items.push_back(hbox({
                    text("    " + std::string(icon) + " ") | color(clr),
                    text(tool.name),
                }));
            }
            return vbox(tool_items);
        }
        case WizardStep::Review:
            return vbox({
                text("  Review Configuration") | bold,
                separator(),
                hbox({text("    Name: ") | dim, text(ws.config.name)}),
                hbox({text("    Model: ") | dim, text(ws.config.model) | color(Color::Cyan)}),
                hbox({text("    Tools: ") | dim,
                      text(std::format("{}", ws.config.tools.size()))}),
                hbox({text("    Max Turns: ") | dim,
                      text(std::format("{}", ws.config.max_turns))}),
                separator(),
                text("  Press Enter to create agent") | color(Color::Green),
            });
    }
    return text("Unknown step") | color(Color::Red);
}

/// Render the full wizard
[[nodiscard]] inline Element RenderCreateWizard(const WizardState& ws) {
    return vbox({
        text(" New Agent") | bold | color(Color::Cyan),
        separator(),
        RenderStepIndicator(ws.current_step),
        separator(),
        RenderWizardBody(ws) | flex,
        separator(),
        hbox({
            text(" [←]") | color(Color::Cyan), text(" back "),
            text("[→/Enter]") | color(Color::Cyan), text(" next "),
            text("[Esc]") | color(Color::Cyan), text(" cancel"),
        }) | dim,
    }) | border;
}

/// Create an interactive wizard component
[[nodiscard]] inline Component CreateAgentWizard(CreateWizardProps props) {
    struct FullState {
        CreateWizardProps props;
        WizardState wizard;
    };
    auto state = std::make_shared<FullState>();
    state->props = std::move(props);

    return Renderer([state] {
        return RenderCreateWizard(state->wizard);
    }) | CatchEvent([state](Event event) -> bool {
        auto& ws = state->wizard;

        if (event == Event::Escape) {
            if (state->props.on_cancel) state->props.on_cancel();
            return true;
        }

        // Navigate forward
        if (event == Event::ArrowRight || event == Event::Return) {
            if (ws.current_step == WizardStep::Review) {
                if (state->props.on_complete)
                    state->props.on_complete(ws.config);
            } else {
                int s = static_cast<int>(ws.current_step);
                ws.current_step = static_cast<WizardStep>(s + 1);
            }
            return true;
        }

        // Navigate backward
        if (event == Event::ArrowLeft) {
            int s = static_cast<int>(ws.current_step);
            if (s > 0) {
                ws.current_step = static_cast<WizardStep>(s - 1);
            }
            return true;
        }

        return false;
    });
}

} // namespace cc::ui::agent_editor
