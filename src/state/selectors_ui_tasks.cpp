// selectors_ui_tasks.cpp - impl unit for cc.state.selectors
// (RFC 0001 Phase C batch 10). UI-state selectors (selected IP agent index,
// coordinator task index, view mode, footer/spinner) and the tasks & agent
// name-registry selectors, plus the task-view / multi-agent composites.
module;

#include <cstdint>
#include <cstddef>

module cc.state.selectors;

import std;

import cc.state.app_state;

namespace cc::state::selectors {

// ============================================================
// UI State Selectors
// ============================================================

/// Get the selected IP agent index
[[nodiscard]] int32_t get_selected_ip_agent_index(const AppState& state) noexcept {
    return state.selected_ip_agent_index;
}

/// Get the coordinator task index
[[nodiscard]] int32_t get_coordinator_task_index(const AppState& state) noexcept {
    return state.coordinator_task_index;
}

/// Get the view selection mode
[[nodiscard]] std::string_view get_view_selection_mode(const AppState& state) noexcept {
    return state.view_selection_mode;
}

/// Check if we're in brief-only mode
[[nodiscard]] bool is_brief_only(const AppState& state) noexcept {
    return state.is_brief_only;
}

/// Check if we should show teammate message previews
[[nodiscard]] bool should_show_teammate_message_preview(const AppState& state) noexcept {
    return state.show_teammate_message_preview;
}

/// Get the footer selection
[[nodiscard]] std::optional<FooterItem> get_footer_selection(const AppState& state) noexcept {
    return state.footer_selection;
}

/// Check if a specific footer item is selected
[[nodiscard]] bool is_footer_item_selected(const AppState& state, FooterItem item) noexcept {
    return state.footer_selection == item;
}

/// Get the spinner tip
[[nodiscard]] std::optional<std::string_view> get_spinner_tip(const AppState& state) noexcept {
    if (state.spinner_tip) {
        return *state.spinner_tip;
    }
    return std::nullopt;
}

// ============================================================
// Tasks & Agents Selectors
// ============================================================

/// Get the number of tasks
[[nodiscard]] size_t get_task_count(const AppState& state) noexcept {
    return state.tasks.size();
}

/// Check if there are any tasks
[[nodiscard]] bool has_tasks(const AppState& state) noexcept {
    return !state.tasks.empty();
}

/// Get the foregrounded task ID
[[nodiscard]] std::optional<std::string_view> get_foregrounded_task_id(const AppState& state) noexcept {
    if (state.foregrounded_task_id) {
        return *state.foregrounded_task_id;
    }
    return std::nullopt;
}

/// Check if a specific task is foregrounded
[[nodiscard]] bool is_task_foregrounded(const AppState& state, std::string_view task_id) noexcept {
    return state.foregrounded_task_id == task_id;
}

/// Get the viewing agent task ID
[[nodiscard]] std::optional<std::string_view> get_viewing_agent_task_id(const AppState& state) noexcept {
    if (state.viewing_agent_task_id) {
        return *state.viewing_agent_task_id;
    }
    return std::nullopt;
}

/// Check if we're viewing a specific agent's task
[[nodiscard]] bool is_viewing_agent_task(const AppState& state, std::string_view task_id) noexcept {
    return state.viewing_agent_task_id == task_id;
}

/// Get the number of agents in the name registry
[[nodiscard]] size_t get_agent_name_registry_count(const AppState& state) noexcept {
    return state.agent_name_registry.size();
}

/// Check if an agent name is registered
[[nodiscard]] bool is_agent_name_registered(const AppState& state, std::string_view name) noexcept {
    return state.agent_name_registry.contains(std::string(name));
}

/// Get an agent ID by name
[[nodiscard]] std::optional<std::string_view> get_agent_id_by_name(const AppState& state, std::string_view name) noexcept {
    auto it = state.agent_name_registry.find(std::string(name));
    if (it != state.agent_name_registry.end()) {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================
// Derived Selectors (Composite)
// ============================================================

/// Check if any task-related view is active
[[nodiscard]] bool is_any_task_view_active(const AppState& state) noexcept {
    return is_tasks_view_expanded(state) || 
           state.foregrounded_task_id.has_value() ||
           state.viewing_agent_task_id.has_value();
}

/// Check if we're in a multi-agent mode
[[nodiscard]] bool is_multi_agent_mode(const AppState& state) noexcept {
    return state.view_selection_mode == "selecting-agent" || 
           state.view_selection_mode == "viewing-agent";
}

} // namespace cc::state::selectors
