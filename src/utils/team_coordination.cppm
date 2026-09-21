module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.utils.team_coordination;

export namespace cc::utils::team_coordination {

struct TeammateInfo {
    std::string id;
    std::string name;
    std::string role;
    bool is_online{false};
};

struct MailboxMessage {
    std::string from;
    std::string to;
    std::string content;
    std::string timestamp;
};

struct TeamDiscoveryResult {
    std::vector<TeammateInfo> teammates;
    std::string team_id;
};

inline std::expected<TeamDiscoveryResult, std::string> discover_team() {
    return TeamDiscoveryResult{{}, ""};
}

inline std::expected<void, std::string> send_to_mailbox(
    std::string_view, std::string_view) {
    return {};
}

inline std::expected<std::vector<MailboxMessage>, std::string> read_mailbox(
    std::string_view) {
    return {};
}

inline std::expected<std::string, std::string> get_teammate_context(
    std::string_view) {
    return "";
}

inline std::expected<void, std::string> sync_team_memory(
    std::string_view) {
    return {};
}

} // namespace cc::utils::team_coordination
