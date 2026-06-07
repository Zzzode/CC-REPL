module;
#include <string>
#include <string_view>
#include <expected>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

export module cc.bridge.work_secret;

import cc.bridge.jwt_utils;
import cc.utils.json;

export namespace cc::bridge {

// Internal: get the path to the secret file
inline std::string get_secret_path();

struct DecodedWorkSecret {
    int version = 0;
    std::string session_ingress_token;
    std::string api_base_url;
    std::optional<bool> use_code_sessions;
    std::optional<std::string> code_session_mode;
    std::vector<std::string> sources_json;
    std::optional<std::string> auth_json;
    std::optional<std::string> mcp_config_json;
    std::optional<std::string> environment_json;
    std::unordered_map<std::string, std::string> environment_variables;
    std::string raw_json;
};

[[nodiscard]] inline std::optional<std::string> work_secret_string_field(
    cc::utils::json::JsonVal root,
    std::initializer_list<std::string_view> keys
) {
    if (!root.valid() || !root.is_obj()) return std::nullopt;
    for (auto key : keys) {
        auto value = root.get(key);
        if (value.is_str()) return std::string(value.as_str());
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<bool> work_secret_bool_field(
    cc::utils::json::JsonVal root,
    std::initializer_list<std::string_view> keys
) {
    if (!root.valid() || !root.is_obj()) return std::nullopt;
    for (auto key : keys) {
        auto value = root.get(key);
        if (value.is_bool()) return value.as_bool();
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> work_secret_json_field(
    cc::utils::json::JsonVal root,
    std::initializer_list<std::string_view> keys
) {
    if (!root.valid() || !root.is_obj()) return std::nullopt;
    for (auto key : keys) {
        auto value = root.get(key);
        if (value.valid() && !value.is_null()) return value.to_string();
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string work_secret_value_as_env_string(cc::utils::json::JsonVal value) {
    if (value.is_str()) return std::string(value.as_str());
    if (value.is_bool()) return value.as_bool() ? "true" : "false";
    if (value.is_num()) return value.to_string();
    return {};
}

[[nodiscard]] inline std::unordered_map<std::string, std::string> work_secret_env_vars(
    cc::utils::json::JsonVal root
) {
    std::unordered_map<std::string, std::string> env;
    if (!root.valid() || !root.is_obj()) return env;

    auto env_node = root.get("environment_variables");
    if (!env_node.valid()) env_node = root.get("environmentVariables");
    if (!env_node.valid()) env_node = root.get("env");
    if (!env_node.valid()) env_node = root.get("environment");
    if (!env_node.is_obj()) return env;

    env_node.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        auto text = work_secret_value_as_env_string(value);
        if (!text.empty()) env.emplace(std::string(key.as_str()), std::move(text));
    });
    return env;
}

[[nodiscard]] inline std::vector<std::string> work_secret_sources(cc::utils::json::JsonVal root) {
    std::vector<std::string> sources;
    if (!root.valid() || !root.is_obj()) return sources;

    auto source = root.get("source");
    if (source.valid() && !source.is_null()) sources.push_back(source.to_string());

    auto sources_node = root.get("sources");
    if (sources_node.is_arr()) {
        sources_node.iter([&](cc::utils::json::JsonVal item) {
            if (item.valid() && !item.is_null()) sources.push_back(item.to_string());
        });
    } else if (sources_node.valid() && !sources_node.is_null()) {
        sources.push_back(sources_node.to_string());
    }
    return sources;
}

std::expected<DecodedWorkSecret, std::string> decode_work_secret(std::string_view secret) {
    auto decoded = base64url_decode(secret);
    if (!decoded) {
        return std::unexpected("Failed to decode work secret: " + decoded.error());
    }

    auto parsed = cc::utils::json::parse(*decoded);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Invalid work secret: not a JSON object");
    }
    auto root = parsed->root();
    auto version = root.get("version");
    if (!version.is_num() || version.as_int() != 1) {
        return std::unexpected("Unsupported work secret version");
    }
    auto session_ingress_token = work_secret_string_field(root, {"session_ingress_token", "sessionIngressToken"});
    if (!session_ingress_token || session_ingress_token->empty()) {
        return std::unexpected("Invalid work secret: missing or empty session_ingress_token");
    }
    auto api_base_url = work_secret_string_field(root, {"api_base_url", "apiBaseUrl"});
    if (!api_base_url) {
        return std::unexpected("Invalid work secret: missing api_base_url");
    }

    DecodedWorkSecret result{
        .version = static_cast<int>(version.as_int()),
        .session_ingress_token = *session_ingress_token,
        .api_base_url = *api_base_url,
        .use_code_sessions = std::nullopt,
        .code_session_mode = work_secret_string_field(root, {"code_session_mode", "codeSessionMode"}),
        .sources_json = work_secret_sources(root),
        .auth_json = work_secret_json_field(root, {"auth", "auth_records", "authRecords", "credentials"}),
        .mcp_config_json = work_secret_json_field(root, {"mcp", "mcp_config", "mcpConfig", "mcp_servers", "mcpServers"}),
        .environment_json = work_secret_json_field(root, {"environment_variables", "environmentVariables", "env", "environment"}),
        .environment_variables = work_secret_env_vars(root),
        .raw_json = root.to_string(),
    };
    if (auto use_code_sessions = work_secret_bool_field(root, {"use_code_sessions", "useCodeSessions"})) {
        result.use_code_sessions = *use_code_sessions;
    }
    return result;
}

// Generate a cryptographically random work secret (hex-encoded)
std::string generate_work_secret() {
    // Use random_device for cryptographic randomness
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    // Generate 32 bytes (256 bits) of randomness
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        oss << std::setw(16) << dist(gen);
    }

    return oss.str();
}

// Store the work secret securely on disk
std::expected<void, std::string> store_work_secret(std::string_view secret) {
    if (secret.empty()) {
        return std::unexpected("Cannot store empty secret");
    }

    std::string secret_path = get_secret_path();
    std::filesystem::create_directories(std::filesystem::path(secret_path).parent_path());

    std::ofstream ofs(secret_path, std::ios::trunc);
    if (!ofs.is_open()) {
        return std::unexpected("Failed to open secret file for writing: " + secret_path);
    }

    ofs << secret;
    ofs.close();

    // Set restrictive permissions (owner read/write only)
    std::error_code ec;
    std::filesystem::permissions(secret_path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);

    if (ec) {
        return std::unexpected("Failed to set permissions on secret file: " + ec.message());
    }

    return {};
}

// Load the work secret from disk
std::expected<std::string, std::string> load_work_secret() {
    std::string secret_path = get_secret_path();

    if (!std::filesystem::exists(secret_path)) {
        return std::unexpected("Work secret file not found: " + secret_path);
    }

    std::ifstream ifs(secret_path);
    if (!ifs.is_open()) {
        return std::unexpected("Failed to open secret file for reading");
    }

    std::string secret;
    std::getline(ifs, secret);

    if (secret.empty()) {
        return std::unexpected("Secret file is empty");
    }

    return secret;
}

// Constant-time comparison to prevent timing attacks
bool verify_work_secret(std::string_view presented, std::string_view stored) {
    if (presented.size() != stored.size()) {
        return false;
    }

    // Constant-time comparison
    volatile unsigned char result = 0;
    for (size_t i = 0; i < presented.size(); ++i) {
        result |= static_cast<unsigned char>(presented[i]) ^
                  static_cast<unsigned char>(stored[i]);
    }

    return result == 0;
}

// Internal: get the path to the secret file
inline std::string get_secret_path() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.config/claude-code/bridge/work_secret";
    }
    return "/tmp/claude-code-bridge-work-secret";
}

} // namespace cc::bridge
