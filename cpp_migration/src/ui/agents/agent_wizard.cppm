/// @file agent_wizard.cppm
/// @brief Agent creation wizard UI — step-based flow for creating new agents.
/// Migrates agent creation wizard UI.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>

export module cc.ui.agent_wizard;

export namespace cc::ui::agents {

// ============================================================
// Enumerations
// ============================================================

/// Steps in the agent creation wizard
enum class WizardStep {
    SelectType,
    ConfigureName,
    SetCapabilities,
    SetPermissions,
    Review,
    Complete,
};

/// Type of agent to create
enum class AgentType {
    Teammate,
    Background,
    OneShot,
    Coordinator,
};

// ============================================================
// Data Structures
// ============================================================

/// Template for a pre-configured agent type
struct AgentTemplate {
    AgentType type;
    std::string name;
    std::string description;
    std::vector<std::string> default_capabilities;
    std::vector<std::string> suggested_tools;
};

/// Current state of the wizard
struct WizardState {
    WizardStep current_step;
    std::optional<AgentType> selected_type;
    std::string name;
    std::vector<std::string> capabilities;
    std::vector<std::string> permissions;
    bool confirmed{false};
};

// ============================================================
// Rendering & Logic Functions
// ============================================================

/// Get the display title for a wizard step
inline constexpr std::string_view get_step_title(WizardStep step) {
    switch (step) {
        case WizardStep::SelectType:       return "Select Agent Type";
        case WizardStep::ConfigureName:    return "Configure Name";
        case WizardStep::SetCapabilities:  return "Set Capabilities";
        case WizardStep::SetPermissions:   return "Set Permissions";
        case WizardStep::Review:           return "Review & Confirm";
        case WizardStep::Complete:         return "Complete";
    }
    return "Unknown";
}

/// Render the current wizard step UI
inline std::string render_wizard_step(WizardState state) {
    return "[wizard_step: " + std::string(get_step_title(state.current_step)) + "]";
}

/// Get all available agent templates
inline std::vector<AgentTemplate> get_available_templates() {
    return {
        {AgentType::Teammate, "Teammate", "Persistent collaborative agent",
            {"code_edit", "file_read", "bash_exec"}, {"git", "editor"}},
        {AgentType::Background, "Background", "Long-running background worker",
            {"file_read", "bash_exec"}, {"monitor", "scheduler"}},
        {AgentType::OneShot, "One-Shot", "Single task execution agent",
            {"code_edit", "file_read"}, {"editor"}},
        {AgentType::Coordinator, "Coordinator", "Orchestrates other agents",
            {"web_search", "mcp_tool"}, {"dispatcher", "planner"}},
    };
}

/// Render the type selection UI
inline std::string render_type_selector(
    std::vector<AgentTemplate> templates, std::optional<AgentType> selected) {
    std::string output;
    output += "\033[1mSelect Agent Type:\033[0m\n\n";
    for (const auto& tmpl : templates) {
        bool is_selected = selected && *selected == tmpl.type;
        if (is_selected) {
            output += " \033[36m\u25B6 " + tmpl.name + "\033[0m";
        } else {
            output += "   " + tmpl.name;
        }
        output += " - \033[2m" + tmpl.description + "\033[0m\n";
    }
    return output;
}

/// Render a checklist of capabilities
inline std::string render_capability_checklist(
    std::vector<std::string> available, std::vector<std::string> selected) {
    std::string output;
    output += "\033[1mCapabilities:\033[0m\n";
    for (const auto& cap : available) {
        bool is_checked = std::find(selected.begin(), selected.end(), cap) != selected.end();
        output += is_checked ? " \033[32m[\u2714]\033[0m " : " [ ] ";
        output += cap + "\n";
    }
    return output;
}

/// Render the final review summary before creation
inline std::string render_review_summary(WizardState state) {
    std::string output;
    output += "\033[1mReview Agent Configuration:\033[0m\n\n";
    output += "  Name:         \033[36m" + state.name + "\033[0m\n";
    if (state.selected_type) {
        for (const auto& tmpl : get_available_templates()) {
            if (tmpl.type == *state.selected_type) {
                output += "  Type:         " + tmpl.name + "\n";
                output += "  Description:  " + tmpl.description + "\n";
                break;
            }
        }
    }
    output += "  Capabilities: ";
    for (size_t i = 0; i < state.capabilities.size(); ++i) {
        if (i > 0) output += ", ";
        output += state.capabilities[i];
    }
    output += "\n";
    output += "  Permissions:  ";
    for (size_t i = 0; i < state.permissions.size(); ++i) {
        if (i > 0) output += ", ";
        output += state.permissions[i];
    }
    output += "\n";
    return output;
}

/// Get total number of wizard steps
inline constexpr int get_step_count() {
    return 6;
}

/// Check whether the wizard can proceed to the next step
inline bool can_proceed(WizardState state) {
    switch (state.current_step) {
        case WizardStep::SelectType:
            return state.selected_type.has_value();
        case WizardStep::ConfigureName:
            return !state.name.empty();
        case WizardStep::SetCapabilities:
            return !state.capabilities.empty();
        case WizardStep::SetPermissions:
            return true; // permissions are optional
        case WizardStep::Review:
            return state.confirmed;
        case WizardStep::Complete:
            return false;
    }
    return false;
}

} // namespace cc::ui::agents
