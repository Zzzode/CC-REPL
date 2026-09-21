/// @file agent_color_manager.cppm
/// @brief ANSI color palette assignment for agents.
/// Migrated from src/tools/AgentTool/agentColorManager.ts.
///
/// The underlying `AgentColor` enum and `TeammateLayoutManager` round-robin
/// palette already live in `cc.utils.swarm_backends` / `cc.utils.swarm_helpers`.
/// This module exposes the thin agent-specific API (get/set/cycle/reset) used
/// by the AgentTool entry points.  The general-purpose ("main thread") agent
/// intentionally has no assigned color to distinguish it from sub-agents.
module;

#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

export module cc.tools.agent_color_manager;

import cc.utils.swarm_backends;
import cc.utils.swarm_helpers;

export namespace cc::tools::agent_color_manager {

using cc::utils::swarm_backends::AgentColor;
using cc::utils::swarm_backends::agent_color_name;
using cc::utils::swarm_helpers::TeammateLayoutManager;

/// The 8 named agent colors (same order as the TS `AGENT_COLORS` array).
inline constexpr AgentColor AGENT_COLOR_PALETTE[] = {
    AgentColor::Red,    AgentColor::Blue,   AgentColor::Green,  AgentColor::Yellow,
    AgentColor::Purple, AgentColor::Orange, AgentColor::Pink,   AgentColor::Cyan,
};

/// Number of entries in AGENT_COLOR_PALETTE.
inline constexpr size_t PALETTE_SIZE = sizeof(AGENT_COLOR_PALETTE) / sizeof(AGENT_COLOR_PALETTE[0]);

/// Parse a named color (case-insensitive).  Returns nullopt on failure.
[[nodiscard]] inline std::optional<AgentColor> parse_color_name(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char ch : name) lower.push_back(static_cast<char>(std::tolower(ch)));
    if (lower == "red")    return AgentColor::Red;
    if (lower == "blue")   return AgentColor::Blue;
    if (lower == "green")  return AgentColor::Green;
    if (lower == "yellow") return AgentColor::Yellow;
    if (lower == "purple") return AgentColor::Purple;
    if (lower == "orange") return AgentColor::Orange;
    if (lower == "pink")   return AgentColor::Pink;
    if (lower == "cyan")   return AgentColor::Cyan;
    return std::nullopt;
}

/// Thread-safe map used for explicit `setAgentColor` / per-type overrides
/// (distinct from the round-robin palette handled by `TeammateLayoutManager`).
class AgentColorMap {
public:
    /// Explicitly set (or clear) a color for a given agent type.
    static void set(std::string_view agent_type, std::optional<AgentColor> color) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!color) {
            map_.erase(std::string(agent_type));
            return;
        }
        map_[std::string(agent_type)] = *color;
    }

    /// Retrieve an explicitly set color, or nullopt if none has been assigned.
    [[nodiscard]] static std::optional<AgentColor> get(std::string_view agent_type) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(std::string(agent_type));
        if (it != map_.end()) return it->second;
        return std::nullopt;
    }

    /// Drop all explicit color assignments (used on session reset).
    static void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.clear();
    }

private:
    static inline std::mutex mutex_;
    static inline std::map<std::string, AgentColor, std::less<>> map_;
};

/// Return the color assigned to a given agent type, or nullopt for
/// `general-purpose` (the main thread agent intentionally stays uncoloured).
///
/// Priority:
///   1. If the agent-type is `general-purpose`, return nullopt.
///   2. Return any explicitly set color (via `set_agent_color`).
///   3. Fall back to the round-robin palette via `TeammateLayoutManager`.
[[nodiscard]] inline std::optional<AgentColor> get_agent_color(std::string_view agent_type) {
    if (agent_type == "general-purpose") return std::nullopt;

    if (auto explicit_color = AgentColorMap::get(agent_type)) {
        return explicit_color;
    }
    return TeammateLayoutManager::get_color(agent_type);
}

/// Explicitly assign (or clear) a color for an agent type.
inline void set_agent_color(
    std::string_view agent_type,
    std::optional<AgentColor> color
) {
    AgentColorMap::set(agent_type, color);
}

/// Assign a new color from the palette if the agent does not already have one.
/// Uses the round-robin TeammateLayoutManager policy so teammates and agent
/// colors share the same global cycle.
[[nodiscard]] inline AgentColor assign_cycle_color(std::string_view agent_type) {
    if (auto c = get_agent_color(agent_type)) return *c;
    return TeammateLayoutManager::assign_color(agent_type);
}

/// Convert an agent color to a theme key name (as used by the UI layer).
/// Phase 4 — FTXUI components are responsible for turning this into ANSI codes.
[[nodiscard]] inline std::string_view to_theme_color_key(AgentColor color) {
    // Matches the TypeScript `AGENT_COLOR_TO_THEME_COLOR` mapping.
    switch (color) {
        case AgentColor::Red:    return "red_FOR_SUBAGENTS_ONLY";
        case AgentColor::Blue:   return "blue_FOR_SUBAGENTS_ONLY";
        case AgentColor::Green:  return "green_FOR_SUBAGENTS_ONLY";
        case AgentColor::Yellow: return "yellow_FOR_SUBAGENTS_ONLY";
        case AgentColor::Purple: return "purple_FOR_SUBAGENTS_ONLY";
        case AgentColor::Orange: return "orange_FOR_SUBAGENTS_ONLY";
        case AgentColor::Pink:   return "pink_FOR_SUBAGENTS_ONLY";
        case AgentColor::Cyan:   return "cyan_FOR_SUBAGENTS_ONLY";
    }
    return "default_FOR_SUBAGENTS_ONLY";
}

} // namespace cc::tools::agent_color_manager
