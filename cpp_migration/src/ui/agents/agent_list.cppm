/// @file agent_list.cppm
/// @brief Agent list UI — cards, status badges, filtering, and sorting.
/// Migrates components/agents/ (26 TS files - agent list, wizard, cards, status).
module;

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.ui.agent_list;

export namespace cc::ui::agents {

// ============================================================
// Enumerations
// ============================================================

/// Agent execution status
enum class AgentStatus {
    Idle,
    Running,
    Waiting,
    Error,
    Complete,
};

/// Agent capability type
enum class AgentCapability {
    CodeEdit,
    FileRead,
    BashExec,
    WebSearch,
    McpTool,
};

// ============================================================
// Data Structures
// ============================================================

/// Information about a single agent instance
struct AgentInfo {
    std::string id;
    std::string name;
    std::string description;
    AgentStatus status;
    std::vector<AgentCapability> capabilities;
    std::optional<std::string> current_task;
    std::chrono::steady_clock::time_point started_at;
    std::size_t messages_sent{0};
};

/// Configuration for agent list rendering
struct AgentListConfig {
    bool show_capabilities{true};
    bool show_status{true};
    bool show_duration{true};
    bool compact_mode{false};
};

// ============================================================
// Rendering Functions
// ============================================================

/// Render the full agent list view
inline std::string render_agent_list(
    std::vector<AgentInfo> agents, AgentListConfig config = {}) {
    std::string output;
    output += "\033[1m Agents (" + std::to_string(agents.size()) + ")\033[0m\n";
    output += std::string(40, '\u2500') + "\n";

    for (const auto& agent : agents) {
        output += render_agent_card(agent, false);
        output += "\n";
        if (config.show_capabilities && !agent.capabilities.empty()) {
            output += "  Capabilities: ";
            for (size_t i = 0; i < agent.capabilities.size(); ++i) {
                if (i > 0) output += " ";
                output += std::string(get_capability_icon(agent.capabilities[i]));
            }
            output += "\n";
        }
        if (config.show_duration && agent.status == AgentStatus::Running) {
            output += "  Duration: " + format_agent_duration(agent.started_at) + "\n";
        }
    }
    return output;
}

/// Render an individual agent card
inline std::string render_agent_card(AgentInfo agent, bool selected = false) {
    std::string output;
    if (selected) output += "\033[7m"; // inverse video for selection
    output += " " + std::string(get_status_icon(agent.status)) + " ";
    output += "\033[1m" + agent.name + "\033[0m";
    if (!agent.description.empty()) {
        output += " - " + agent.description;
    }
    if (agent.current_task) {
        output += " \033[2m[" + *agent.current_task + "]\033[0m";
    }
    if (selected) output += "\033[0m";
    return output;
}

/// Get the icon character for a given agent status
inline constexpr std::string_view get_status_icon(AgentStatus status) {
    switch (status) {
        case AgentStatus::Idle:     return "\u25CB"; // ○
        case AgentStatus::Running:  return "\u25B6"; // ▶
        case AgentStatus::Waiting:  return "\u23F8"; // ⏸
        case AgentStatus::Error:    return "\u2718"; // ✘
        case AgentStatus::Complete: return "\u2714"; // ✔
    }
    return "?";
}

/// Get the icon character for a given capability
inline constexpr std::string_view get_capability_icon(AgentCapability cap) {
    switch (cap) {
        case AgentCapability::CodeEdit:  return "\u270E"; // ✎
        case AgentCapability::FileRead:  return "\U0001F4C4"; // 📄
        case AgentCapability::BashExec:  return "\u2318"; // ⌘
        case AgentCapability::WebSearch: return "\U0001F310"; // 🌐
        case AgentCapability::McpTool:   return "\U0001F527"; // 🔧
    }
    return "?";
}

/// Render the status badge for an agent
inline std::string render_agent_status_badge(AgentStatus status) {
    std::string icon(get_status_icon(status));
    switch (status) {
        case AgentStatus::Idle:     return "\033[90m" + icon + " Idle\033[0m";
        case AgentStatus::Running:  return "\033[32m" + icon + " Running\033[0m";
        case AgentStatus::Waiting:  return "\033[33m" + icon + " Waiting\033[0m";
        case AgentStatus::Error:    return "\033[31m" + icon + " Error\033[0m";
        case AgentStatus::Complete: return "\033[32m" + icon + " Done\033[0m";
    }
    return "[" + icon + "]";
}

/// Format duration since agent started
inline std::string format_agent_duration(std::chrono::steady_clock::time_point start) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    if (secs < 60) return std::to_string(secs) + "s";
    if (secs < 3600) return std::to_string(secs / 60) + "m " + std::to_string(secs % 60) + "s";
    return std::to_string(secs / 3600) + "h " + std::to_string((secs % 3600) / 60) + "m";
}

/// Filter agents by their current status
inline std::vector<AgentInfo> filter_agents_by_status(
    std::vector<AgentInfo> agents, AgentStatus status) {
    std::vector<AgentInfo> result;
    for (auto& a : agents) {
        if (a.status == status) result.push_back(std::move(a));
    }
    return result;
}

/// Sort agents by most recent activity (in-place)
inline void sort_agents_by_activity(std::vector<AgentInfo>& agents) {
    std::ranges::sort(agents, [](const AgentInfo& a, const AgentInfo& b) {
        return a.started_at > b.started_at;
    });
}

} // namespace cc::ui::agents
