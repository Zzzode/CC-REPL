/// @file pill_label.cppm
/// @brief Footer pill label generation for background tasks.
/// Migrated from src/tasks/pillLabel.ts
module;

#include <string>
#include <vector>
#include <set>
#include <format>
#include <algorithm>

export module cc.tasks.pill_label;

import cc.tasks.task;
import cc.tasks.types;

export namespace cc::tasks {

// ============================================================
// Unicode Figures
// ============================================================

/// Filled diamond character for ultraplan ready state
inline constexpr std::string_view DIAMOND_FILLED = "\u25C6";

/// Open diamond character for running cloud sessions
inline constexpr std::string_view DIAMOND_OPEN = "\u25C7";

// ============================================================
// Pill Label Generation
// ============================================================

/// Count elements matching a predicate
template<typename Container, typename Pred>
[[nodiscard]] std::size_t count_if(const Container& c, Pred pred) {
    return static_cast<std::size_t>(std::count_if(c.begin(), c.end(), pred));
}

/// Produces the compact footer-pill label for a set of background tasks.
/// Used by both the footer pill and the turn-duration transcript line.
[[nodiscard]] inline std::string get_pill_label(
    const std::vector<cc::core::TaskStateBase*>& tasks
) {
    if (tasks.empty()) return "";
    
    auto n = tasks.size();
    auto first_type = tasks[0]->type;
    bool all_same_type = std::all_of(tasks.begin(), tasks.end(),
        [first_type](const auto* t) { return t->type == first_type; });
    
    if (all_same_type) {
        switch (first_type) {
            case cc::core::TaskType::LocalBash: {
                std::size_t monitors = 0;
                for (const auto* t : tasks) {
                    const auto* shell = static_cast<const LocalShellTaskState*>(t);
                    if (shell->kind == BashTaskKind::Monitor) {
                        ++monitors;
                    }
                }
                auto shells = n - monitors;
                
                std::string result;
                if (shells > 0) {
                    result = (shells == 1) ? "1 shell" : std::format("{} shells", shells);
                }
                if (monitors > 0) {
                    if (!result.empty()) result += ", ";
                    result += (monitors == 1) ? "1 monitor" : std::format("{} monitors", monitors);
                }
                return result;
            }
            
            case cc::core::TaskType::InProcessTeammate: {
                std::set<std::string> teams;
                for (const auto* t : tasks) {
                    const auto* teammate = static_cast<const InProcessTeammateTaskState*>(t);
                    teams.insert(teammate->identity.team_name);
                }
                auto team_count = teams.size();
                return (team_count == 1) ? "1 team" : std::format("{} teams", team_count);
            }
            
            case cc::core::TaskType::LocalAgent:
                return (n == 1) ? "1 local agent" : std::format("{} local agents", n);
            
            case cc::core::TaskType::RemoteAgent: {
                if (n == 1) {
                    const auto* remote = static_cast<const RemoteAgentTaskState*>(tasks[0]);
                    if (remote->is_ultraplan) {
                        if (remote->ultraplan_phase == UltraplanPhase::PlanReady) {
                            return std::format("{} ultraplan ready", DIAMOND_FILLED);
                        }
                        if (remote->ultraplan_phase == UltraplanPhase::NeedsInput) {
                            return std::format("{} ultraplan needs your input", DIAMOND_OPEN);
                        }
                        return std::format("{} ultraplan", DIAMOND_OPEN);
                    }
                    return std::format("{} 1 cloud session", DIAMOND_OPEN);
                }
                return std::format("{} {} cloud sessions", DIAMOND_OPEN, n);
            }
            
            case cc::core::TaskType::LocalWorkflow:
                return (n == 1) ? "1 background workflow" : std::format("{} background workflows", n);
            
            case cc::core::TaskType::MonitorMcp:
                return (n == 1) ? "1 monitor" : std::format("{} monitors", n);
            
            case cc::core::TaskType::Dream:
                return "dreaming";
        }
    }
    
    // Mixed types
    return (n == 1) 
        ? "1 background task" 
        : std::format("{} background tasks", n);
}

/// True when the pill should show the dimmed " · ↓ to view" call-to-action.
/// Only ultraplan attention states (needs_input, plan_ready) surface the CTA.
[[nodiscard]] inline bool pill_needs_cta(
    const std::vector<cc::core::TaskStateBase*>& tasks
) {
    if (tasks.size() != 1) return false;
    if (tasks[0]->type != cc::core::TaskType::RemoteAgent) return false;
    const auto* remote = static_cast<const RemoteAgentTaskState*>(tasks[0]);
    return remote->is_ultraplan && remote->ultraplan_phase.has_value();
}

} // namespace cc::tasks
