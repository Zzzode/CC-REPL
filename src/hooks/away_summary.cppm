// cc.hooks.away_summary — generates summary of what happened while user was away
// Migrated from: useAwaySummary.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <chrono>
#include <optional>
#include <mutex>
#include <format>

export module cc.hooks.away_summary;

export namespace cc::hooks::away_summary {

struct AwaySummaryItem {
    std::string category;   // "message", "task", "error", "system"
    std::string content;
    std::chrono::system_clock::time_point timestamp;
};

struct AwaySummary {
    std::vector<std::string> new_messages;
    std::vector<std::string> completed_tasks;
    std::vector<std::string> failed_tasks;
    std::vector<std::string> errors;
    std::vector<std::string> system_events;
    std::chrono::system_clock::time_point away_since;
    std::chrono::system_clock::time_point returned_at;
};

namespace detail {

struct AwaySummaryState {
    std::mutex mutex;
    std::vector<AwaySummaryItem> events;
    std::optional<std::chrono::system_clock::time_point> away_since;
    bool is_away{false};
};

inline auto get_state() -> AwaySummaryState& {
    static AwaySummaryState state;
    return state;
}

} // namespace detail

/// Mark the user as "away" starting from now.
inline auto mark_away() -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.is_away = true;
    state.away_since = std::chrono::system_clock::now();
    state.events.clear(); // Fresh start for new away period
}

/// Mark the user as "returned" and stop collecting events.
inline auto mark_returned() -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    state.is_away = false;
}

/// Check if the user is currently marked as away.
inline auto is_away() -> bool {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    return state.is_away;
}

/// Record an event that occurred while the user was away.
inline auto record_event(std::string category, std::string content) -> void {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    if (!state.is_away) return; // Only record events during away period
    state.events.push_back(AwaySummaryItem{
        .category = std::move(category),
        .content = std::move(content),
        .timestamp = std::chrono::system_clock::now()
    });
}

/// Generate the away summary from collected events.
inline auto generate_away_summary([[maybe_unused]] std::string_view session_id)
    -> std::expected<AwaySummary, std::string>
{
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);

    if (!state.away_since.has_value()) {
        return std::unexpected(std::string{"No away period recorded"});
    }

    AwaySummary summary;
    summary.away_since = *state.away_since;
    summary.returned_at = std::chrono::system_clock::now();

    for (const auto& event : state.events) {
        if (event.category == "message") {
            summary.new_messages.push_back(event.content);
        } else if (event.category == "task_completed") {
            summary.completed_tasks.push_back(event.content);
        } else if (event.category == "task_failed") {
            summary.failed_tasks.push_back(event.content);
        } else if (event.category == "error") {
            summary.errors.push_back(event.content);
        } else {
            summary.system_events.push_back(event.content);
        }
    }

    return summary;
}

/// Format an away summary into a human-readable string.
inline auto format_away_summary(const AwaySummary& summary) -> std::string {
    auto away_duration = std::chrono::duration_cast<std::chrono::minutes>(
        summary.returned_at - summary.away_since);

    std::string result = std::format("While you were away ({} min):\n", away_duration.count());

    if (!summary.new_messages.empty()) {
        result += std::format("\n  Messages ({})\n", summary.new_messages.size());
        for (const auto& msg : summary.new_messages) {
            result += "    - " + msg + "\n";
        }
    }

    if (!summary.completed_tasks.empty()) {
        result += std::format("\n  Completed tasks ({})\n", summary.completed_tasks.size());
        for (const auto& task : summary.completed_tasks) {
            result += "    + " + task + "\n";
        }
    }

    if (!summary.failed_tasks.empty()) {
        result += std::format("\n  Failed tasks ({})\n", summary.failed_tasks.size());
        for (const auto& task : summary.failed_tasks) {
            result += "    ! " + task + "\n";
        }
    }

    if (!summary.errors.empty()) {
        result += std::format("\n  Errors ({})\n", summary.errors.size());
        for (const auto& err : summary.errors) {
            result += "    X " + err + "\n";
        }
    }

    if (!summary.system_events.empty()) {
        result += std::format("\n  System events ({})\n", summary.system_events.size());
        for (const auto& evt : summary.system_events) {
            result += "    * " + evt + "\n";
        }
    }

    if (summary.new_messages.empty() && summary.completed_tasks.empty() &&
        summary.failed_tasks.empty() && summary.errors.empty() &&
        summary.system_events.empty()) {
        result += "  Nothing happened while you were away.\n";
    }

    return result;
}

/// Determine if a summary should be shown based on away duration.
inline auto should_show_summary(std::chrono::seconds away_duration) -> bool {
    // Show summary if user was away for more than 5 minutes
    constexpr auto threshold = std::chrono::seconds{300};
    return away_duration >= threshold;
}

/// Get the duration since the user was marked away (returns 0 if not away).
inline auto get_away_duration() -> std::chrono::seconds {
    auto& state = detail::get_state();
    std::lock_guard lock(state.mutex);
    if (!state.is_away || !state.away_since.has_value()) {
        return std::chrono::seconds{0};
    }
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - *state.away_since);
}

} // namespace cc::hooks::away_summary
