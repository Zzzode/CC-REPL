module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

export module cc.bridge.config;


export namespace cc::bridge {

namespace detail {

[[nodiscard]] auto read_text_file(std::string_view path) -> std::expected<std::string, std::string> {
    std::ifstream input{std::filesystem::path(std::string(path))};
    if (!input.is_open()) {
        return std::unexpected("Unable to open bridge config file: " + std::string(path));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

[[nodiscard]] auto json_string(std::string_view json, std::string_view key) -> std::optional<std::string> {
    auto marker = '"' + std::string(key) + '"';
    auto pos = json.find(marker);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find('"', pos + 1);
    if (pos == std::string_view::npos) return std::nullopt;

    std::string out;
    bool escaping = false;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (escaping) {
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(c); break;
            }
            escaping = false;
        } else if (c == '\\') {
            escaping = true;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

[[nodiscard]] auto json_number(std::string_view json, std::string_view key) -> std::optional<int64_t> {
    auto marker = '"' + std::string(key) + '"';
    auto pos = json.find(marker);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    auto end = pos;
    if (end < json.size() && (json[end] == '-' || json[end] == '+')) ++end;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (end == pos) return std::nullopt;
    try { return std::stoll(std::string(json.substr(pos, end - pos))); }
    catch (...) { return std::nullopt; }
}

[[nodiscard]] auto json_bool(std::string_view json, std::string_view key) -> std::optional<bool> {
    auto marker = '"' + std::string(key) + '"';
    auto pos = json.find(marker);
    if (pos == std::string_view::npos) return std::nullopt;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string_view::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    if (json.substr(pos, 4) == "true") return true;
    if (json.substr(pos, 5) == "false") return false;
    return std::nullopt;
}

} // namespace detail


enum class TransportType { websocket, stdio, http_polling };
enum class SpawnMode { single_session, same_dir, worktree };

namespace detail {

[[nodiscard]] auto parse_transport(std::string_view value) -> std::optional<TransportType> {
    if (value == "websocket" || value == "ws") return TransportType::websocket;
    if (value == "stdio") return TransportType::stdio;
    if (value == "http_polling" || value == "http-polling" || value == "polling") return TransportType::http_polling;
    return std::nullopt;
}

} // namespace detail


struct BridgeConfig {
    TransportType transport{TransportType::websocket};
    std::string host{"localhost"};
    uint16_t port{7860};
    std::string path{"/bridge"};
    std::optional<std::string> auth_token;
    std::chrono::milliseconds heartbeat_interval{30'000};
    std::chrono::milliseconds reconnect_delay{1'000};
    uint32_t max_reconnect_attempts{10};
    bool debug_mode{false};
    bool auto_connect{true};
    std::string dir;
    std::string machine_name;
    std::string branch;
    std::optional<std::string> git_repo_url;
    int max_sessions{1};
    SpawnMode spawn_mode{SpawnMode::single_session};
    std::string bridge_id;
    std::string worker_type;
    std::string environment_id;
    std::optional<std::string> reuse_environment_id;
    std::string api_base_url;
    std::string session_ingress_url;
    std::optional<std::string> debug_file;
    std::optional<std::chrono::milliseconds> session_timeout_ms;
};


struct BridgeEndpoint {
    std::string url;
    TransportType transport;
    bool requires_auth{false};
};


struct PollConfig {
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds long_poll_timeout{30'000};
    size_t max_batch_size{50};
    bool adaptive_interval{true};
    std::chrono::milliseconds min_interval{200};
    std::chrono::milliseconds max_interval{5000};
};


class BridgeConfigLoader {
    BridgeConfig config_;
    PollConfig poll_config_;
    
public:

    [[nodiscard]] auto load() -> std::expected<BridgeConfig, std::string> {

        if (auto* port = std::getenv("CC_BRIDGE_PORT"))
            config_.port = static_cast<uint16_t>(std::stoi(port));
        if (auto* host = std::getenv("CC_BRIDGE_HOST"))
            config_.host = host;
        if (auto* token = std::getenv("CC_BRIDGE_TOKEN"))
            config_.auth_token = token;
        return config_;
    }
    

    [[nodiscard]] auto load_from_file(std::string_view path) -> std::expected<BridgeConfig, std::string> {
        auto content = detail::read_text_file(path);
        if (!content) return std::unexpected(content.error());

        auto& json = *content;
        if (auto value = detail::json_string(json, "transport")) {
            auto parsed = detail::parse_transport(*value);
            if (!parsed) return std::unexpected("Invalid bridge transport: " + *value);
            config_.transport = *parsed;
        }
        if (auto value = detail::json_string(json, "host")) config_.host = *value;
        if (auto value = detail::json_number(json, "port")) {
            if (*value <= 0 || *value > 65535) return std::unexpected("Bridge port is out of range");
            config_.port = static_cast<uint16_t>(*value);
        }
        if (auto value = detail::json_string(json, "path")) config_.path = *value;
        if (auto value = detail::json_string(json, "auth_token")) config_.auth_token = *value;
        if (auto value = detail::json_number(json, "heartbeat_interval_ms")) {
            config_.heartbeat_interval = std::chrono::milliseconds(*value);
        }
        if (auto value = detail::json_number(json, "reconnect_delay_ms")) {
            config_.reconnect_delay = std::chrono::milliseconds(*value);
        }
        if (auto value = detail::json_number(json, "max_reconnect_attempts")) {
            config_.max_reconnect_attempts = static_cast<uint32_t>(*value);
        }
        if (auto value = detail::json_bool(json, "debug_mode")) config_.debug_mode = *value;
        if (auto value = detail::json_bool(json, "auto_connect")) config_.auto_connect = *value;
        return config_;
    }
    
    [[nodiscard]] auto get_config() const -> const BridgeConfig& { return config_; }
    [[nodiscard]] auto get_poll_config() const -> const PollConfig& { return poll_config_; }
    
    void set_config(BridgeConfig config) { config_ = std::move(config); }
    void set_poll_config(PollConfig config) { poll_config_ = config; }
};


// ---- Bridge Auth Configuration ----

[[nodiscard]] auto getBridgeTokenOverride() -> std::optional<std::string> {
    auto* user_type = std::getenv("USER_TYPE");
    if (user_type == nullptr || std::string_view(user_type) != "ant") return std::nullopt;
    auto* token = std::getenv("CLAUDE_BRIDGE_OAUTH_TOKEN");
    if (token == nullptr) return std::nullopt;
    return std::string(token);
}

[[nodiscard]] auto getBridgeBaseUrlOverride() -> std::optional<std::string> {
    auto* user_type = std::getenv("USER_TYPE");
    if (user_type == nullptr || std::string_view(user_type) != "ant") return std::nullopt;
    auto* url = std::getenv("CLAUDE_BRIDGE_BASE_URL");
    if (url == nullptr) return std::nullopt;
    return std::string(url);
}

[[nodiscard]] auto getBridgeAccessToken() -> std::optional<std::string> {
    if (auto override = getBridgeTokenOverride()) return override;
    // TODO: Wire up OAuth keychain via get_oauth_access_token() from the OAuth service.
    return std::nullopt;
}

[[nodiscard]] auto getBridgeBaseUrl() -> std::string {
    if (auto override = getBridgeBaseUrlOverride()) return *override;
    // TODO: Pull default from OAuth config (getOauthConfig().BASE_API_URL) once integrated.
    return "https://api.claude.ai";
}

} // namespace cc::bridge
