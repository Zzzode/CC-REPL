// selectors_conversation.cpp - impl unit for cc.state.selectors
// (RFC 0001 Phase C batch 10). Message/conversation selectors: counts,
// loading/streaming/error state, total token usage + cost, working and
// allowed directories, and the composite is_ui_busy. This is the ONLY impl
// unit that imports cc.types.types (cc::core::TokenUsage in get_total_usage).
module;

#include <cstddef>

module cc.state.selectors;

import std;

import cc.types.types;
import cc.state.app_state;

namespace cc::state::selectors {

// ============================================================
// Message & Conversation Selectors
// ============================================================

/// Get the number of messages
[[nodiscard]] size_t get_message_count(const AppState& state) noexcept {
    return state.messages.size();
}

/// Check if there are any messages
[[nodiscard]] bool has_messages(const AppState& state) noexcept {
    return !state.messages.empty();
}

/// Check if loading
[[nodiscard]] bool is_loading(const AppState& state) noexcept {
    return state.is_loading;
}

/// Check if streaming
[[nodiscard]] bool is_streaming(const AppState& state) noexcept {
    return state.is_streaming;
}

/// Get the error message
[[nodiscard]] std::optional<std::string_view> get_error_message(const AppState& state) noexcept {
    if (state.error_message) {
        return *state.error_message;
    }
    return std::nullopt;
}

/// Check if there's an error
[[nodiscard]] bool has_error(const AppState& state) noexcept {
    return state.error_message.has_value();
}

/// Get the total token usage
[[nodiscard]] const cc::core::TokenUsage& get_total_usage(const AppState& state) noexcept {
    return state.total_usage;
}

/// Get the total cost in USD
[[nodiscard]] double get_total_cost_usd(const AppState& state) noexcept {
    return state.total_cost_usd;
}

/// Get the working directory
[[nodiscard]] std::string_view get_working_directory(const AppState& state) noexcept {
    return state.working_directory;
}

/// Get allowed directories (paths added via /add-dir)
[[nodiscard]] const std::vector<std::string>& get_allowed_directories(const AppState& state) noexcept {
    return state.allowed_directories;
}

// ============================================================
// Derived Selectors (Composite)
// ============================================================

/// Check if the UI is busy (loading, streaming, or has error)
[[nodiscard]] bool is_ui_busy(const AppState& state) noexcept {
    return state.is_loading || state.is_streaming || state.error_message.has_value();
}

} // namespace cc::state::selectors
