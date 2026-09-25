// selectors_features.cpp - impl unit for cc.state.selectors
// (RFC 0001 Phase C batch 10). Feature selectors: prompt suggestion,
// speculation, skill-improvement suggestion, inbox messages, and the worker
// sandbox permission/request selectors.
module;

#include <cstddef>

module cc.state.selectors;

import std;

import cc.state.app_state;

namespace cc::state::selectors {

// ============================================================
// Prompt Suggestion Selectors
// ============================================================

/// Get the prompt suggestion text
[[nodiscard]] std::optional<std::string_view> get_prompt_suggestion_text(const AppState& state) noexcept {
    if (state.prompt_suggestion.text) {
        return *state.prompt_suggestion.text;
    }
    return std::nullopt;
}

/// Get the prompt suggestion prompt ID
[[nodiscard]] std::optional<std::string_view> get_prompt_suggestion_prompt_id(const AppState& state) noexcept {
    if (state.prompt_suggestion.prompt_id) {
        return *state.prompt_suggestion.prompt_id;
    }
    return std::nullopt;
}

/// Check if there's an active prompt suggestion
[[nodiscard]] bool has_prompt_suggestion(const AppState& state) noexcept {
    return state.prompt_suggestion.text.has_value();
}

// ============================================================
// Speculation Selectors
// ============================================================

/// Get the speculation status
[[nodiscard]] SpeculationStatus get_speculation_status(const AppState& state) noexcept {
    return state.speculation.status;
}

/// Check if speculation is active
[[nodiscard]] bool is_speculation_active(const AppState& state) noexcept {
    return state.speculation.status == SpeculationStatus::Active;
}

/// Check if speculation is idle
[[nodiscard]] bool is_speculation_idle(const AppState& state) noexcept {
    return state.speculation.status == SpeculationStatus::Idle;
}

/// Get the speculation ID
[[nodiscard]] std::string_view get_speculation_id(const AppState& state) noexcept {
    return state.speculation.id;
}

/// Get the speculation message count
[[nodiscard]] size_t get_speculation_message_count(const AppState& state) noexcept {
    return state.speculation.messages.size();
}

/// Get the speculation suggestion length
[[nodiscard]] size_t get_speculation_suggestion_length(const AppState& state) noexcept {
    return state.speculation.suggestion_length;
}

/// Get the speculation tool use count
[[nodiscard]] size_t get_speculation_tool_use_count(const AppState& state) noexcept {
    return state.speculation.tool_use_count;
}

/// Check if speculation is pipelined
[[nodiscard]] bool is_speculation_pipelined(const AppState& state) noexcept {
    return state.speculation.is_pipelined;
}

// ============================================================
// Skill Improvement Selectors
// ============================================================

/// Check if there's a skill improvement suggestion
[[nodiscard]] bool has_skill_improvement_suggestion(const AppState& state) noexcept {
    return state.skill_improvement.suggestion.has_value();
}

/// Get the skill improvement suggestion skill name
[[nodiscard]] std::optional<std::string_view> get_skill_improvement_skill_name(const AppState& state) noexcept {
    if (state.skill_improvement.suggestion) {
        return state.skill_improvement.suggestion->skill_name;
    }
    return std::nullopt;
}

// ============================================================
// Inbox Selectors
// ============================================================

/// Get the inbox message count
[[nodiscard]] size_t get_inbox_message_count(const AppState& state) noexcept {
    return state.inbox.messages.size();
}

/// Check if there are any inbox messages
[[nodiscard]] bool has_inbox_messages(const AppState& state) noexcept {
    return !state.inbox.messages.empty();
}

// ============================================================
// Worker Sandbox Selectors
// ============================================================

/// Get the number of pending sandbox permission requests
[[nodiscard]] size_t get_pending_sandbox_permission_count(const AppState& state) noexcept {
    return state.worker_sandbox_permissions.queue.size();
}

/// Check if there are any pending sandbox permission requests
[[nodiscard]] bool has_pending_sandbox_permissions(const AppState& state) noexcept {
    return !state.worker_sandbox_permissions.queue.empty();
}

/// Get the selected sandbox permission index
[[nodiscard]] size_t get_selected_sandbox_permission_index(const AppState& state) noexcept {
    return state.worker_sandbox_permissions.selected_index;
}

/// Check if there's a pending worker request
[[nodiscard]] bool has_pending_worker_request(const AppState& state) noexcept {
    return state.pending_worker_request.has_value();
}

/// Check if there's a pending sandbox request
[[nodiscard]] bool has_pending_sandbox_request(const AppState& state) noexcept {
    return state.pending_sandbox_request.has_value();
}

} // namespace cc::state::selectors
