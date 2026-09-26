// Implementation unit for cc.tools.agent.utils — teammate role/color
// mapping, parent session id, team completion status, and ToolResult text.
module;

module cc.tools.agent.utils;

import std;

import cc.utils.swarm_backends;
import cc.tools.team;
import cc.types.tool_types;

namespace cc::tools::agent::utils {

[[nodiscard]] cc::tools::MemberRole teammate_role_for_agent_type(std::string_view agent_type) {
    auto lower = std::string(agent_type);
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lower.find("leader") != std::string::npos || lower.find("lead") != std::string::npos) {
        return cc::tools::MemberRole::Leader;
    }
    if (lower.find("review") != std::string::npos ||
        lower.find("verify") != std::string::npos ||
        lower.find("verification") != std::string::npos ||
        lower.find("validator") != std::string::npos) {
        return cc::tools::MemberRole::Reviewer;
    }
    return cc::tools::MemberRole::Worker;
}

[[nodiscard]] std::optional<cc::utils::swarm_backends::AgentColor> teammate_agent_color(
    const std::optional<std::string>& color
) {
    if (!color || color->empty()) return std::nullopt;
    auto lower = *color;
    std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    using cc::utils::swarm_backends::AgentColor;
    if (lower == "red") return AgentColor::Red;
    if (lower == "blue") return AgentColor::Blue;
    if (lower == "green") return AgentColor::Green;
    if (lower == "yellow") return AgentColor::Yellow;
    if (lower == "purple") return AgentColor::Purple;
    if (lower == "orange") return AgentColor::Orange;
    if (lower == "pink") return AgentColor::Pink;
    if (lower == "cyan") return AgentColor::Cyan;
    return std::nullopt;
}

[[nodiscard]] std::string teammate_parent_session_id() {
    if (const char* value = std::getenv("LOOM_SESSION_ID"); value && *value) {
        return value;
    }
    if (const char* value = std::getenv("CLAUDE_SESSION_ID"); value && *value) {
        return value;
    }
    return "native-session";
}

[[nodiscard]] std::string tool_result_content_text(const ToolResult& result) {
    std::string output;
    for (const auto& content : result.content) {
        if (!output.empty()) output += "\n";
        output += content.text;
    }
    return output;
}

void update_teammate_completion_status(
    const AgentExecutionPlan& plan,
    bool success,
    const std::string& result_text
) {
    if (!plan.team_name || plan.team_name->empty()) return;
    auto stored_result = result_text.empty()
        ? std::optional<std::string>{}
        : std::optional<std::string>{result_text};
    (void)cc::tools::global_team_store().update_member_status(
        *plan.team_name,
        plan.agent_id,
        success ? cc::tools::MemberStatus::Done : cc::tools::MemberStatus::Error,
        std::move(stored_result));
}

} // namespace cc::tools::agent::utils
