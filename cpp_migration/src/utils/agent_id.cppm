module;

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.agent_id;

export namespace cc::utils::agent_id {

struct ParsedAgentId {
    std::string agent_name;
    std::string team_name;
};

struct ParsedRequestId {
    std::string request_type;
    long long timestamp = 0;
    std::string agent_id;
};

[[nodiscard]] inline std::string format_agent_id(std::string_view agent_name, std::string_view team_name) {
    return std::string(agent_name) + "@" + std::string(team_name);
}

[[nodiscard]] inline std::optional<ParsedAgentId> parse_agent_id(std::string_view agent_id) {
    const auto at = agent_id.find('@');
    if (at == std::string_view::npos) return std::nullopt;
    return ParsedAgentId{.agent_name = std::string(agent_id.substr(0, at)), .team_name = std::string(agent_id.substr(at + 1))};
}

[[nodiscard]] inline std::string generate_request_id(std::string_view request_type, std::string_view agent_id, long long timestamp) {
    return std::string(request_type) + "-" + std::to_string(timestamp) + "@" + std::string(agent_id);
}

[[nodiscard]] inline std::optional<ParsedRequestId> parse_request_id(std::string_view request_id) {
    const auto at = request_id.find('@');
    if (at == std::string_view::npos) return std::nullopt;
    const auto prefix = request_id.substr(0, at);
    const auto agent = request_id.substr(at + 1);
    const auto dash = prefix.rfind('-');
    if (dash == std::string_view::npos) return std::nullopt;
    const auto type = prefix.substr(0, dash);
    const auto timestamp_text = prefix.substr(dash + 1);
    long long timestamp = 0;
    const auto* begin = timestamp_text.data();
    const auto* end = timestamp_text.data() + timestamp_text.size();
    auto [ptr, ec] = std::from_chars(begin, end, timestamp);
    if (ec != std::errc{} || ptr != end) return std::nullopt;
    return ParsedRequestId{.request_type = std::string(type), .timestamp = timestamp, .agent_id = std::string(agent)};
}

} // namespace cc::utils::agent_id
