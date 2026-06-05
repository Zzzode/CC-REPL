module;
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module cc.services.mcp.config;

import cc.utils.json;

namespace cc::services::mcp {

using cc::utils::json::JsonVal;
using cc::utils::json::parse;

namespace {

[[nodiscard]] std::optional<std::string> json_string(JsonVal value, std::string_view key) {
    auto child = value.get(key);
    if (!child.is_str()) return std::nullopt;
    return std::string(child.as_str());
}

void append_string_array(JsonVal value, std::vector<std::string>& out) {
    if (!value.is_arr()) return;
    value.iter([&](JsonVal item) {
        if (item.is_str()) out.emplace_back(item.as_str());
    });
}

void append_string_map(JsonVal value, std::map<std::string, std::string>& out) {
    if (!value.is_obj()) return;
    value.iter_obj([&](JsonVal key, JsonVal item) {
        if (key.is_str() && item.is_str()) {
            out[std::string(key.as_str())] = std::string(item.as_str());
        }
    });
}

[[nodiscard]] std::optional<TransportType> parse_transport(JsonVal value) {
    auto type = json_string(value, "type");
    if (!type) type = json_string(value, "transport");
    if (type) {
        if (*type == "stdio") return TransportType::Stdio;
        if (*type == "sse") return TransportType::Sse;
        if (*type == "http" || *type == "streamable-http" || *type == "streamableHttp") {
            return TransportType::StreamableHttp;
        }
        return std::nullopt;
    }
    if (value.get("url").is_str()) return TransportType::StreamableHttp;
    return TransportType::Stdio;
}

[[nodiscard]] std::optional<ServerConfig>
parse_single_server(std::string name, JsonVal value, ConfigScope scope) {
    if (!value.is_obj()) return std::nullopt;

    ServerConfig config;
    config.name = std::move(name);
    config.scope = scope;
    auto transport = parse_transport(value);
    if (!transport) return std::nullopt;
    config.transport = *transport;
    config.command = json_string(value, "command").value_or(std::string{});
    config.url = json_string(value, "url").value_or(std::string{});
	config.headers_helper = json_string(value, "headersHelper")
	    .or_else([&] { return json_string(value, "headers_helper"); })
	    .value_or(std::string{});
	append_string_array(value.get("args"), config.args);
	append_string_map(value.get("env"), config.env);
	append_string_map(value.get("headers"), config.headers);
	if (auto oauth = value.get("oauth"); oauth.is_obj()) {
	    McpOAuthConfig oauth_config;
	    oauth_config.auth_server_metadata_url = json_string(oauth, "authServerMetadataUrl")
	        .or_else([&] { return json_string(oauth, "auth_server_metadata_url"); });
	    if (auto callback_port = oauth.get("callbackPort"); callback_port.is_num()) {
	        oauth_config.callback_port = static_cast<int>(callback_port.as_int());
	    } else if (auto callback_port = oauth.get("callback_port"); callback_port.is_num()) {
	        oauth_config.callback_port = static_cast<int>(callback_port.as_int());
	    }
	    oauth_config.client_id = json_string(oauth, "clientId")
	        .or_else([&] { return json_string(oauth, "client_id"); });
	    if (auto xaa = oauth.get("xaa"); xaa.is_bool()) {
	        oauth_config.xaa = xaa.as_bool();
	    }
	    config.oauth = std::move(oauth_config);
	}
	if (auto timeout = value.get("timeout"); timeout.is_num()) {
	    config.timeout = std::chrono::milliseconds{timeout.as_int()};
	}
    if (auto auto_start = value.get("autoStart"); auto_start.is_bool()) {
        config.auto_start = auto_start.as_bool();
    }
    if (auto auto_start = value.get("auto_start"); auto_start.is_bool()) {
        config.auto_start = auto_start.as_bool();
    }
    if (auto enabled = value.get("enabled"); enabled.is_bool()) {
        config.enabled = enabled.as_bool();
    }
    if (auto disabled = value.get("disabled"); disabled.is_bool()) {
        config.enabled = !disabled.as_bool();
    }

    return config;
}

} // namespace

std::expected<void, std::string> ServerConfig::validate() const {
    if (name.empty()) {
        return std::unexpected("Server name is required");
    }
    if (transport == TransportType::Stdio && command.empty()) {
        return std::unexpected(std::format("Server '{}': command is required for stdio transport", name));
    }
    if ((transport == TransportType::Sse || transport == TransportType::Http ||
         transport == TransportType::StreamableHttp) && url.empty()) {
        return std::unexpected(std::format("Server '{}': url is required for SSE/HTTP transport", name));
    }
    return {};
}

std::vector<const ServerConfig*> McpConfig::auto_start_servers() const {
    std::vector<const ServerConfig*> result;
    for (const auto& [_, server] : servers) {
        if (server.auto_start && server.enabled) {
            result.push_back(&server);
        }
    }
    return result;
}

std::optional<std::reference_wrapper<const ServerConfig>>
McpConfig::get_server(std::string_view name) const {
    auto it = servers.find(std::string(name));
    if (it != servers.end()) return std::cref(it->second);
    return std::nullopt;
}

std::filesystem::path ConfigPaths::global_config() {
    auto home = home_directory();
    return home / ".config" / "claude" / "mcp_servers.json";
}

std::filesystem::path ConfigPaths::user_config() {
    auto home = home_directory();
    return home / ".claude" / "mcp_servers.json";
}

std::filesystem::path ConfigPaths::project_config(const std::filesystem::path& project_root) {
    return project_root / ".claude" / "mcp_servers.json";
}

std::filesystem::path ConfigPaths::local_config(const std::filesystem::path& project_root) {
    return project_root / ".claude" / "mcp_servers.local.json";
}

std::vector<std::pair<ConfigScope, std::filesystem::path>>
ConfigPaths::all_config_paths(const std::filesystem::path& project_root) {
    return {
        {ConfigScope::Global, global_config()},
        {ConfigScope::User, user_config()},
        {ConfigScope::Project, project_config(project_root)},
        {ConfigScope::Local, local_config(project_root)},
    };
}

std::filesystem::path ConfigPaths::home_directory() {
    if (auto* home = std::getenv("HOME")) {
        return std::filesystem::path(home);
    }
    return std::filesystem::path("/tmp");
}

std::expected<std::map<std::string, ServerConfig>, ConfigError>
ConfigParser::parse_file(const std::filesystem::path& path, ConfigScope scope) {
    if (!std::filesystem::exists(path)) {
        return std::unexpected(ConfigError::FileNotFound);
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected(ConfigError::PermissionDenied);
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    return parse_json(content, scope);
}

std::expected<std::map<std::string, ServerConfig>, ConfigError>
ConfigParser::parse_json(const std::string& json, ConfigScope scope) {
    std::map<std::string, ServerConfig> servers;

    auto parsed = parse(json);
    if (!parsed) return std::unexpected(ConfigError::ParseError);
    auto root = parsed->root();
    if (!root.is_obj()) return std::unexpected(ConfigError::InvalidSchema);

    auto servers_node = root.get("mcpServers");
    if (!servers_node.valid()) servers_node = root.get("servers");
    if (!servers_node.valid()) return servers;
    if (!servers_node.is_obj()) return std::unexpected(ConfigError::InvalidSchema);

    servers_node.iter_obj([&](JsonVal key, JsonVal value) {
        if (!key.is_str()) return;
        auto config = parse_single_server(std::string(key.as_str()), value, scope);
        if (!config) return;
        if (config->validate()) {
            servers[config->name] = std::move(*config);
        }
    });
    return servers;
}

ConfigLoader::ConfigLoader(std::filesystem::path project_root)
    : project_root_(std::move(project_root)) {}

std::expected<McpConfig, ConfigError> ConfigLoader::load() const {
    McpConfig merged;

    for (const auto& [scope, path] : ConfigPaths::all_config_paths(project_root_)) {
        auto result = ConfigParser::parse_file(path, scope);
        if (result) {
            for (auto& [name, config] : *result) {
                merged.servers[name] = std::move(config);
            }
        }
    }
    return merged;
}

std::expected<McpConfig, ConfigError> ConfigLoader::load_scope(ConfigScope scope) const {
    McpConfig config;
    std::filesystem::path path;

    switch (scope) {
        case ConfigScope::Global: path = ConfigPaths::global_config(); break;
        case ConfigScope::User: path = ConfigPaths::user_config(); break;
        case ConfigScope::Project: path = ConfigPaths::project_config(project_root_); break;
        case ConfigScope::Local: path = ConfigPaths::local_config(project_root_); break;
    }

    auto result = ConfigParser::parse_file(path, scope);
    if (!result) return std::unexpected(result.error());

    for (auto& [name, server] : *result) {
        config.servers[name] = std::move(server);
    }
    return config;
}

std::expected<void, ConfigError> ConfigLoader::save_server(
    const ServerConfig& server, ConfigScope scope) const {
    std::filesystem::path path;
    switch (scope) {
        case ConfigScope::Global: path = ConfigPaths::global_config(); break;
        case ConfigScope::User: path = ConfigPaths::user_config(); break;
        case ConfigScope::Project: path = ConfigPaths::project_config(project_root_); break;
        case ConfigScope::Local: path = ConfigPaths::local_config(project_root_); break;
    }

    std::filesystem::create_directories(path.parent_path());

    auto json = serialize_server(server);
    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) return std::unexpected(ConfigError::PermissionDenied);
    file << json;
    return {};
}

std::string ConfigLoader::serialize_server(const ServerConfig& server) {
    std::string json = std::format(R"("{}":{{)", server.name);
    if (server.transport == TransportType::Stdio) {
        json += std::format(R"("command":"{}")", server.command);
        if (!server.args.empty()) {
            json += R"(,"args":[)";
            for (std::size_t i = 0; i < server.args.size(); ++i) {
                if (i > 0) json += ",";
                json += std::format(R"("{}")", server.args[i]);
            }
            json += "]";
        }
	} else {
	    json += std::format(R"("url":"{}")", server.url);
	}
	if (!server.headers.empty()) {
	    json += R"(,"headers":{)";
	    std::size_t header_index = 0;
	    for (const auto& [key, value] : server.headers) {
	        if (header_index++ > 0) json += ",";
	        json += std::format(R"("{}":"{}")", key, value);
	    }
	    json += "}";
	}
	if (!server.headers_helper.empty()) {
	    json += std::format(R"(,"headersHelper":"{}")", server.headers_helper);
	}
	if (server.oauth) {
	    json += R"(,"oauth":{)";
	    bool wrote_oauth = false;
	    auto add_oauth = [&](std::string field) {
	        if (wrote_oauth) json += ",";
	        json += field;
	        wrote_oauth = true;
	    };
	    if (server.oauth->auth_server_metadata_url) {
	        add_oauth(std::format(R"("authServerMetadataUrl":"{}")", *server.oauth->auth_server_metadata_url));
	    }
	    if (server.oauth->callback_port) {
	        add_oauth(std::format(R"("callbackPort":{})", *server.oauth->callback_port));
	    }
	    if (server.oauth->client_id) {
	        add_oauth(std::format(R"("clientId":"{}")", *server.oauth->client_id));
	    }
	    if (server.oauth->xaa) {
	        add_oauth(R"("xaa":true)");
	    }
	    json += "}";
	}
	json += "}";
	return json;
}

} // namespace cc::services::mcp
