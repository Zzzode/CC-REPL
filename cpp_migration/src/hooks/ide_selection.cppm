// cc.hooks.ide_selection — tracks text selection in IDE
// Migrated from: useIdeSelection.ts
module;

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

export module cc.hooks.ide_selection;

export namespace cc::hooks::ide_selection {

struct SelectionRange {
    int start_line;
    int start_col;
    int end_line;
    int end_col;
};

struct IdeSelection {
    std::string file_path;
    SelectionRange range;
    std::string selected_text;
    std::string language_id;
};

namespace detail {

struct SelectionState {
    std::mutex mutex;
    std::optional<IdeSelection> current;
    std::unordered_map<std::uint64_t, std::function<void(IdeSelection)>> subscribers;
    std::atomic<std::uint64_t> next_id{1};
};

inline auto get_state() -> SelectionState& {
    static SelectionState state;
    return state;
}

} // namespace detail

inline auto get_current_selection() -> std::optional<IdeSelection> {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.current;
}

inline auto on_selection_change(std::function<void(IdeSelection)> callback)
    -> std::uint64_t
{
    auto& state = detail::get_state();
    auto id = state.next_id.fetch_add(1);
    std::lock_guard lock(state.mutex);
    state.subscribers[id] = std::move(callback);
    return id;
}

inline auto unsubscribe_selection(std::uint64_t subscription_id) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.subscribers.erase(subscription_id);
}

inline auto has_active_selection() -> bool {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.current.has_value() && !state.current->selected_text.empty();
}

/// Set the current selection (called by IDE bridge)
inline auto set_selection(IdeSelection selection) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.current = selection;
    // Notify subscribers
    for (const auto& [id, cb] : state.subscribers) {
        cb(selection);
    }
}

/// Clear the current selection
inline auto clear_selection() -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.current = std::nullopt;
}

} // namespace cc::hooks::ide_selection
