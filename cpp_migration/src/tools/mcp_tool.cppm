// McpTool - Invokes tools and resources exposed by connected MCP servers
module;
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module cc.tools.mcp;

import cc.config.config;
import cc.services.mcp.config;
import cc.services.mcp.connection_manager;
import cc.services.mcp.types;
import cc.utils.json;


export namespace cc::tools {

namespace fs = std::filesystem;
namespace svc_mcp = cc::services::mcp;

enum class McpError {
    ServerNotFound,
    ToolNotFound,
    ResourceNotFound,
    ConnectionFailed,
    AuthRequired,
    AuthFailed,
    MarshalingFailed,
    Timeout,
    InvalidInput,
    ProtocolError,
};

constexpr auto format_error(McpError err) -> std::string_view {
    switch (err) {
        case McpError::ServerNotFound:    return "MCP server not found";
        case McpError::ToolNotFound:      return "Tool not found on MCP server";
        case McpError::ResourceNotFound:  return "Resource not found on MCP server";
        case McpError::ConnectionFailed:  return "Failed to connect to MCP server";
        case McpError::AuthRequired:      return "Authentication required for MCP server";
        case McpError::AuthFailed:        return "MCP server authentication failed";
        case McpError::MarshalingFailed:  return "Failed to marshal/unmarshal tool I/O";
        case McpError::Timeout:           return "MCP request timed out";
        case McpError::InvalidInput:      return "Invalid input parameters";
        case McpError::ProtocolError:     return "MCP protocol error";
        default:                          return "Unknown MCP error";
    }
}


struct McpServerInfo {
    std::string name;
    std::string endpoint;        // stdio, sse, or streamable-http URL
    bool authenticated{false};
    std::vector<std::string> available_tools;
    std::vector<std::string> available_resources;
};


struct McpToolRequest {
    std::string server_name;
    std::string tool_name;
    std::unordered_map<std::string, std::string> arguments;
    std::optional<std::string> arguments_json;
    std::chrono::seconds timeout{30};
};


struct McpToolResult {
    std::string content;
    std::string content_type;  // "text", "image", "resource"
    bool is_error{false};
};


struct McpResource {
    std::string uri;
    std::string name;
    std::string mime_type;
    std::optional<std::string> description;
};

struct McpToolInfo {
    std::string name;
    std::string description;
};

struct McpPromptInfo {
    std::string name;
    std::string description;
    std::vector<std::string> arguments;
};

struct NativeMcpConfiguredServer {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    svc_mcp::TransportType transport = svc_mcp::TransportType::Stdio;
    std::string url = {};
    std::unordered_map<std::string, std::string> headers = {};
    std::string headers_helper = {};
};

struct NativeMcpServerStatus {
    std::string name;
    std::string status;
    std::optional<std::string> error;
    std::optional<std::string> endpoint;
    std::optional<std::string> server_info;
    std::optional<std::string> capabilities;
    std::vector<McpToolInfo> tools;
    std::vector<McpResource> resources;
    std::vector<McpPromptInfo> prompts;
};

[[nodiscard]] inline McpError map_native_error(svc_mcp::McpClientError error) {
    using enum svc_mcp::McpClientError;
    switch (error) {
        case ServerNotFound: return McpError::ServerNotFound;
        case ToolNotFound: return McpError::ToolNotFound;
        case NotConnected:
        case ConnectionFailed:
        case AlreadyConnected:
        case TransportError:
        case ServerClosed:
        case InitializationFailed:
            return McpError::ConnectionFailed;
        case Unauthorized:
            return McpError::AuthRequired;
        case Timeout:
            return McpError::Timeout;
        case InvalidResponse:
        case ProtocolError:
            return McpError::ProtocolError;
    }
    return McpError::ProtocolError;
}

[[nodiscard]] inline std::string status_label(svc_mcp::ConnectionStatus status) {
    using enum svc_mcp::ConnectionStatus;
    switch (status) {
        case Disconnected: return "not started";
        case Connecting: return "starting";
        case Connected: return "ready";
        case NeedsAuth: return "needs-auth";
        case Error: return "error";
    }
    return "unknown";
}

[[nodiscard]] inline NativeMcpServerStatus to_native_status(const svc_mcp::McpServerSnapshot& snapshot) {
    NativeMcpServerStatus status;
    status.name = snapshot.name;
    status.status = status_label(snapshot.status);
    status.error = snapshot.last_error;
    status.endpoint = snapshot.endpoint;
    status.server_info = snapshot.server_info;
    status.capabilities = snapshot.capabilities;
    for (const auto& tool : snapshot.tools) {
        status.tools.push_back(McpToolInfo{.name = tool.name, .description = tool.description});
    }
    for (const auto& resource : snapshot.resources) {
        status.resources.push_back(McpResource{
            .uri = resource.uri,
            .name = resource.name.empty() ? resource.uri : resource.name,
            .mime_type = resource.mime_type.empty() ? "text/plain" : resource.mime_type,
            .description = resource.description.empty() ? std::nullopt : std::optional<std::string>{resource.description},
        });
    }
    for (const auto& prompt : snapshot.prompts) {
        std::vector<std::string> arguments;
        for (const auto& arg : prompt.arguments) arguments.push_back(arg.name);
        status.prompts.push_back(McpPromptInfo{
            .name = prompt.name,
            .description = prompt.description,
            .arguments = std::move(arguments),
        });
    }
    return status;
}

[[nodiscard]] inline svc_mcp::McpConfig make_native_config(
    const std::vector<NativeMcpConfiguredServer>& servers
) {
    svc_mcp::McpConfig config;
    for (const auto& server : servers) {
        svc_mcp::ServerConfig native;
        native.name = server.name;
        native.transport = server.transport;
        native.command = server.command;
        native.args = server.args;
        for (const auto& [key, value] : server.env) native.env[key] = value;
        native.url = server.url;
        for (const auto& [key, value] : server.headers) native.headers[key] = value;
        native.headers_helper = server.headers_helper;
        config.servers[native.name] = std::move(native);
    }
    return config;
}

[[nodiscard]] inline std::optional<std::string> json_string(
    cc::utils::json::JsonVal value,
    std::string_view key
) {
    auto child = value.get(key);
    if (!child.is_str()) return std::nullopt;
    return std::string(child.as_str());
}

inline void append_json_string_array(
    cc::utils::json::JsonVal value,
    std::vector<std::string>& out
) {
    if (!value.is_arr()) return;
    value.iter([&](cc::utils::json::JsonVal item) {
        if (item.is_str()) out.emplace_back(item.as_str());
    });
}

inline void append_json_string_map(
    cc::utils::json::JsonVal value,
    std::unordered_map<std::string, std::string>& out
) {
    if (!value.is_obj()) return;
    value.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal item) {
        if (key.is_str() && item.is_str()) {
            out[std::string(key.as_str())] = std::string(item.as_str());
        }
    });
}

[[nodiscard]] inline svc_mcp::TransportType parse_native_mcp_transport(
    cc::utils::json::JsonVal config
) {
    const auto type = json_string(config, "type").or_else([&] {
        return json_string(config, "transport");
    });
    if (type == "sse") return svc_mcp::TransportType::Sse;
    if (type == "http" || type == "streamable-http" || type == "streamableHttp") {
        return svc_mcp::TransportType::StreamableHttp;
    }
    if (config.get("url").is_str()) return svc_mcp::TransportType::StreamableHttp;
    return svc_mcp::TransportType::Stdio;
}

[[nodiscard]] inline std::optional<NativeMcpConfiguredServer> parse_native_mcp_server(
    std::string name,
    cc::utils::json::JsonVal config
) {
    if (!config.is_obj()) return std::nullopt;

    NativeMcpConfiguredServer server;
    server.name = std::move(name);
    server.transport = parse_native_mcp_transport(config);
    if (auto command = json_string(config, "command")) server.command = std::move(*command);
    if (auto url = json_string(config, "url")) server.url = std::move(*url);
    if (auto helper = json_string(config, "headersHelper").or_else([&] {
        return json_string(config, "headers_helper");
    })) {
        server.headers_helper = std::move(*helper);
    }
    append_json_string_array(config.get("args"), server.args);
    append_json_string_map(config.get("env"), server.env);
    append_json_string_map(config.get("headers"), server.headers);

    if (server.transport == svc_mcp::TransportType::Stdio && server.command.empty()) {
        return std::nullopt;
    }
    if ((server.transport == svc_mcp::TransportType::Sse ||
         server.transport == svc_mcp::TransportType::StreamableHttp) &&
        server.url.empty()) {
        return std::nullopt;
    }
    return server;
}

inline void replace_all(std::string& value, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

[[nodiscard]] inline std::string sanitize_plugin_data_id(std::string_view plugin_id) {
    std::string sanitized;
    sanitized.reserve(plugin_id.size());
    for (char ch : plugin_id) {
        const auto ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '-' || ch == '_';
        sanitized.push_back(ok ? ch : '-');
    }
    return sanitized;
}

[[nodiscard]] inline fs::path plugin_data_dir(std::string_view plugin_id) {
    fs::path plugins_dir;
    if (const char* override_dir = std::getenv("CLAUDE_CODE_PLUGIN_CACHE_DIR")) {
        plugins_dir = override_dir;
    } else if (const char* home = std::getenv("HOME")) {
        plugins_dir = fs::path{home} / ".claude" / "plugins";
    } else {
        plugins_dir = fs::current_path() / ".claude" / "plugins";
    }
    auto dir = plugins_dir / "data" / sanitize_plugin_data_id(plugin_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

[[nodiscard]] inline std::optional<std::string> json_user_config_value_to_string(
    cc::utils::json::JsonVal value
) {
    if (value.is_str()) return std::string(value.as_str());
    if (value.is_bool()) return value.as_bool() ? "true" : "false";
    if (value.is_num()) {
        const auto as_int = value.as_int();
        const auto as_double = value.as_double();
        if (as_double == static_cast<double>(as_int)) return std::to_string(as_int);
        return std::format("{}", as_double);
    }
    if (value.is_arr()) {
        std::string joined;
        value.iter([&](cc::utils::json::JsonVal item) {
            auto scalar = json_user_config_value_to_string(item);
            if (!scalar) return;
            if (!joined.empty()) joined += ",";
            joined += *scalar;
        });
        return joined;
    }
    return std::nullopt;
}

inline void merge_user_config_values(
    cc::utils::json::JsonVal values,
    std::unordered_map<std::string, std::string>& out
) {
    if (!values.is_obj()) return;
    values.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str()) return;
        auto parsed = json_user_config_value_to_string(value);
        if (parsed) out[std::string(key.as_str())] = std::move(*parsed);
    });
}

inline void merge_plugin_mcp_user_config_from_settings(
    const fs::path& settings_path,
    std::string_view plugin_name,
    std::string_view server_name,
    std::unordered_map<std::string, std::string>& out
) {
    auto parsed = cc::utils::json::parse_file(settings_path);
    if (!parsed) return;
    auto plugin_config = parsed->root().get("pluginConfigs").get(plugin_name);
    if (!plugin_config.is_obj()) return;
    merge_user_config_values(plugin_config.get("options"), out);
    merge_user_config_values(plugin_config.get("mcpServers").get(server_name), out);
}

[[nodiscard]] inline std::unordered_map<std::string, std::string> load_plugin_mcp_user_config(
    std::string_view plugin_name,
    std::string_view server_name
) {
    std::unordered_map<std::string, std::string> values;
    if (const char* home = std::getenv("HOME")) {
        merge_plugin_mcp_user_config_from_settings(
            fs::path{home} / ".claude" / "settings.json",
            plugin_name,
            server_name,
            values
        );
    }
    merge_plugin_mcp_user_config_from_settings(
        fs::current_path() / ".claude" / "settings.json",
        plugin_name,
        server_name,
        values
    );
    merge_plugin_mcp_user_config_from_settings(
        fs::current_path() / ".claude" / "settings.local.json",
        plugin_name,
        server_name,
        values
    );
    return values;
}

[[nodiscard]] inline std::optional<std::string> resolve_plugin_mcp_value(
    std::string value,
    const fs::path& plugin_dir,
    std::string_view plugin_name,
    const std::unordered_map<std::string, std::string>& user_config
) {
    replace_all(value, "${CLAUDE_PLUGIN_ROOT}", plugin_dir.string());
    replace_all(value, "${CLAUDE_PLUGIN_DATA}", plugin_data_dir(plugin_name).string());

    std::string resolved;
    resolved.reserve(value.size());
    std::size_t pos = 0;
    while (pos < value.size()) {
        const auto start = value.find("${", pos);
        if (start == std::string::npos) {
            resolved.append(value.substr(pos));
            break;
        }
        resolved.append(value.substr(pos, start - pos));
        const auto end = value.find('}', start + 2);
        if (end == std::string::npos) {
            resolved.append(value.substr(start));
            break;
        }

        const auto name = value.substr(start + 2, end - start - 2);
        if (name.starts_with("user_config.")) {
            const auto key = name.substr(std::string_view("user_config.").size());
            auto it = user_config.find(key);
            if (it == user_config.end()) return std::nullopt;
            resolved += it->second;
        } else if (const char* env = std::getenv(name.c_str())) {
            resolved += env;
        } else {
            resolved.append(value.substr(start, end - start + 1));
        }
        pos = end + 1;
    }
    return resolved;
}

[[nodiscard]] inline std::optional<NativeMcpConfiguredServer> resolve_plugin_mcp_server_environment(
    NativeMcpConfiguredServer server,
    const fs::path& plugin_dir,
    std::string_view plugin_name,
    std::string_view server_name
) {
    const auto user_config = load_plugin_mcp_user_config(plugin_name, server_name);
    auto resolve = [&](std::string value) -> std::optional<std::string> {
        return resolve_plugin_mcp_value(std::move(value), plugin_dir, plugin_name, user_config);
    };

    if (!server.command.empty()) {
        auto resolved = resolve(std::move(server.command));
        if (!resolved) return std::nullopt;
        server.command = std::move(*resolved);
    }
    if (!server.url.empty()) {
        auto resolved = resolve(std::move(server.url));
        if (!resolved) return std::nullopt;
        server.url = std::move(*resolved);
    }
    for (auto& arg : server.args) {
        auto resolved = resolve(std::move(arg));
        if (!resolved) return std::nullopt;
        arg = std::move(*resolved);
    }
    for (auto& [_, value] : server.env) {
        auto resolved = resolve(std::move(value));
        if (!resolved) return std::nullopt;
        value = std::move(*resolved);
    }
    for (auto& [_, value] : server.headers) {
        auto resolved = resolve(std::move(value));
        if (!resolved) return std::nullopt;
        value = std::move(*resolved);
    }
    if (!server.headers_helper.empty()) {
        auto resolved = resolve(std::move(server.headers_helper));
        if (!resolved) return std::nullopt;
        server.headers_helper = std::move(*resolved);
    }

    if (server.transport == svc_mcp::TransportType::Stdio) {
        server.env.try_emplace("CLAUDE_PLUGIN_ROOT", plugin_dir.string());
        server.env.try_emplace("CLAUDE_PLUGIN_DATA", plugin_data_dir(plugin_name).string());
    }
    return server;
}

inline void merge_native_mcp_servers(
    std::vector<NativeMcpConfiguredServer>& base,
    std::vector<NativeMcpConfiguredServer> overlay
) {
    for (auto& server : overlay) {
        std::erase_if(base, [&](const NativeMcpConfiguredServer& existing) {
            return existing.name == server.name;
        });
        base.push_back(std::move(server));
    }
}

[[nodiscard]] inline std::vector<NativeMcpConfiguredServer> parse_native_mcp_server_map(
    cc::utils::json::JsonVal servers,
    std::string_view name_prefix,
    const fs::path& plugin_dir,
    std::string_view plugin_name
) {
    std::vector<NativeMcpConfiguredServer> parsed;
    if (!servers.is_obj()) return parsed;

    servers.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str() || !value.is_obj()) return;
        const std::string server_name{key.as_str()};
        auto name = std::format("{}{}", name_prefix, server_name);
        if (auto server = parse_native_mcp_server(std::move(name), value)) {
            if (auto resolved = resolve_plugin_mcp_server_environment(
                    std::move(*server),
                    plugin_dir,
                    plugin_name,
                    server_name)) {
                parsed.push_back(std::move(*resolved));
            }
        }
    });
    return parsed;
}

[[nodiscard]] inline std::vector<NativeMcpConfiguredServer> load_plugin_mcp_servers_from_file(
    const fs::path& plugin_dir,
    std::string_view relative_path,
    std::string_view plugin_name
) {
    if (relative_path.empty()) return {};
    std::string rel{relative_path};
    if (rel.ends_with(".mcpb")) return {};

    fs::path path{rel};
    if (path.is_relative()) path = plugin_dir / path;
    std::ifstream input(path);
    if (!input) return {};

    std::stringstream buffer;
    buffer << input.rdbuf();
    auto doc = cc::utils::json::parse(buffer.str());
    if (!doc) return {};

    auto root = doc->root();
    auto servers = root.get("mcpServers");
    if (!servers.is_obj()) servers = root;
    return parse_native_mcp_server_map(
        servers,
        std::format("plugin:{}:", plugin_name),
        plugin_dir,
        plugin_name
    );
}

[[nodiscard]] inline std::vector<NativeMcpConfiguredServer> load_plugin_mcp_servers_from_manifest_spec(
    const fs::path& plugin_dir,
    std::string_view plugin_name,
    cc::utils::json::JsonVal spec
) {
    std::vector<NativeMcpConfiguredServer> servers;
    if (spec.is_str()) {
        merge_native_mcp_servers(
            servers,
            load_plugin_mcp_servers_from_file(plugin_dir, spec.as_str(), plugin_name)
        );
    } else if (spec.is_arr()) {
        spec.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) {
                merge_native_mcp_servers(
                    servers,
                    load_plugin_mcp_servers_from_file(plugin_dir, item.as_str(), plugin_name)
                );
            } else if (item.is_obj()) {
                merge_native_mcp_servers(
                    servers,
                    parse_native_mcp_server_map(item, std::format("plugin:{}:", plugin_name), plugin_dir, plugin_name)
                );
            }
        });
    } else if (spec.is_obj()) {
        merge_native_mcp_servers(
            servers,
            parse_native_mcp_server_map(spec, std::format("plugin:{}:", plugin_name), plugin_dir, plugin_name)
        );
    }
    return servers;
}

[[nodiscard]] inline std::vector<NativeMcpConfiguredServer> load_plugin_mcp_servers_from_dir(
    const fs::path& plugin_dir
) {
    const auto manifest_path = plugin_dir / "plugin.json";
    std::ifstream input(manifest_path);
    if (!input) return {};

    std::stringstream buffer;
    buffer << input.rdbuf();
    auto doc = cc::utils::json::parse(buffer.str());
    if (!doc) return {};

    auto root = doc->root();
    auto name = root.get("name");
    if (!root.is_obj() || !name.is_str() || name.as_str().empty()) return {};

    const std::string plugin_name{name.as_str()};
    std::vector<NativeMcpConfiguredServer> servers =
        load_plugin_mcp_servers_from_file(plugin_dir, ".mcp.json", plugin_name);

    if (auto spec = root.get("mcpServers"); spec.valid()) {
        merge_native_mcp_servers(
            servers,
            load_plugin_mcp_servers_from_manifest_spec(plugin_dir, plugin_name, spec)
        );
    }
    return servers;
}

[[nodiscard]] inline std::vector<NativeMcpConfiguredServer> discover_plugin_native_mcp_servers() {
    std::vector<NativeMcpConfiguredServer> servers;
    std::vector<fs::path> roots;
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(fs::path{home} / ".claude" / "plugins");
    }
    roots.push_back(fs::current_path() / ".claude" / "plugins");

    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory(ec)) continue;
            merge_native_mcp_servers(
                servers,
                load_plugin_mcp_servers_from_dir(entry.path())
            );
        }
    }
    return servers;
}

class NativeMcpRuntime {
public:
    static NativeMcpRuntime& instance() {
        static NativeMcpRuntime runtime;
        return runtime;
    }

    [[nodiscard]] std::expected<void, std::string> sync(std::vector<NativeMcpConfiguredServer> servers) {
        std::lock_guard lock(mutex_);
        const auto next_signature = signature(servers);
        if (loaded_ && next_signature == config_signature_) return {};

        ensure_manager_locked();
        manager_->set_configuration(make_native_config(servers));
        config_signature_ = next_signature;
        configured_servers_ = std::move(servers);
        loaded_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> upsert(std::vector<NativeMcpConfiguredServer> servers) {
        if (auto loaded = ensure_loaded_from_config(); !loaded) return std::unexpected(loaded.error());

        std::lock_guard lock(mutex_);
        merge_native_mcp_servers(configured_servers_, std::move(servers));
        const auto next_signature = signature(configured_servers_);
        if (next_signature == config_signature_) return {};

        ensure_manager_locked();
        manager_->set_configuration(make_native_config(configured_servers_));
        config_signature_ = next_signature;
        loaded_ = true;
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> ensure_loaded_from_config() {
        std::lock_guard lock(mutex_);
        if (loaded_) return {};

        cc::core::ConfigManager config;
        auto loaded = config.load();
        if (!loaded) return std::unexpected(loaded.error().message);

        std::vector<NativeMcpConfiguredServer> servers;
        for (const auto& server : config.settings().mcp_servers) {
            servers.push_back(NativeMcpConfiguredServer{
                .name = server.name,
                .command = server.command,
                .args = server.args,
                .env = server.env,
            });
        }
        merge_native_mcp_servers(servers, discover_plugin_native_mcp_servers());

        ensure_manager_locked();
        config_signature_ = signature(servers);
        configured_servers_ = std::move(servers);
        manager_->set_configuration(make_native_config(configured_servers_));
        loaded_ = true;
        return {};
    }

    [[nodiscard]] std::expected<NativeMcpServerStatus, std::string> restart(std::string_view server_name) {
        if (auto loaded = ensure_loaded_from_config(); !loaded) return std::unexpected(loaded.error());

        std::lock_guard lock(mutex_);
        manager_->disconnect_server(std::string(server_name));
        auto connected = manager_->connect_server(std::string(server_name));
        if (!connected) {
            return std::unexpected(std::format("Failed to start MCP server '{}': {}",
                server_name, svc_mcp::error_to_string(connected.error())));
        }
        auto snapshot = manager_->snapshot_server(std::string(server_name));
        if (!snapshot) return std::unexpected(std::format("MCP server '{}' not found", server_name));
        return to_native_status(*snapshot);
    }

    [[nodiscard]] std::optional<NativeMcpServerStatus> status(std::string_view server_name) {
        if (auto loaded = ensure_loaded_from_config(); !loaded) return std::nullopt;

        std::lock_guard lock(mutex_);
        auto snapshot = manager_->snapshot_server(std::string(server_name));
        if (!snapshot) return std::nullopt;
        return to_native_status(*snapshot);
    }

    [[nodiscard]] std::vector<NativeMcpServerStatus> all_statuses() {
        if (auto loaded = ensure_loaded_from_config(); !loaded) return {};

        std::lock_guard lock(mutex_);
        std::vector<NativeMcpServerStatus> statuses;
        for (const auto& snapshot : manager_->snapshot_all_servers()) {
            statuses.push_back(to_native_status(snapshot));
        }
        return statuses;
    }

    [[nodiscard]] std::expected<McpToolResult, McpError> call_tool(
        std::string_view server_name,
        std::string_view tool_name,
        std::string arguments_json
    ) {
        if (auto loaded = ensure_loaded_from_config(); !loaded) {
            return std::unexpected(McpError::ConnectionFailed);
        }

        std::lock_guard lock(mutex_);
        auto snapshot = manager_->snapshot_server(std::string(server_name));
        if (!snapshot) return std::unexpected(McpError::ServerNotFound);
        if (snapshot->status != svc_mcp::ConnectionStatus::Connected) {
            auto connected = manager_->connect_server(std::string(server_name));
            if (!connected) return std::unexpected(map_native_error(connected.error()));
        }

        svc_mcp::ToolCallRequest request{
            .name = std::string(tool_name),
            .arguments_json = arguments_json.empty() ? "{}" : std::move(arguments_json),
        };
        auto result = manager_->call_tool(std::string(server_name), request);
        if (!result) return std::unexpected(map_native_error(result.error()));

        std::string content;
        for (const auto& item : result->content) {
            if (!content.empty()) content += "\n";
            content += item.text;
        }
        if (content.empty()) content = "MCP tool returned no content.";
        return McpToolResult{
            .content = std::move(content),
            .content_type = "text",
            .is_error = result->is_error,
        };
    }

    [[nodiscard]] std::expected<std::vector<McpResource>, McpError> list_resources(
        std::optional<std::string> server_name
    ) {
        if (auto loaded = ensure_loaded_from_config(); !loaded) {
            return std::unexpected(McpError::ConnectionFailed);
        }

        std::lock_guard lock(mutex_);
        std::vector<McpResource> resources;
        if (server_name && !server_name->empty()) {
            auto snapshot = manager_->snapshot_server(*server_name);
            if (!snapshot) return std::unexpected(McpError::ServerNotFound);
            if (snapshot->status != svc_mcp::ConnectionStatus::Connected) {
                auto connected = manager_->connect_server(*server_name);
                if (!connected) return std::unexpected(map_native_error(connected.error()));
            }
            auto listed = manager_->list_resources(*server_name);
            if (!listed) return std::unexpected(map_native_error(listed.error()));
            for (const auto& resource : listed->resources) {
                resources.push_back(McpResource{
                    .uri = resource.uri,
                    .name = resource.name.empty() ? resource.uri : resource.name,
                    .mime_type = resource.mime_type.empty() ? "text/plain" : resource.mime_type,
                    .description = resource.description.empty() ? std::nullopt : std::optional<std::string>{resource.description},
                });
            }
            return resources;
        }

        for (const auto& snapshot : manager_->snapshot_all_servers()) {
            for (const auto& resource : snapshot.resources) {
                resources.push_back(McpResource{
                    .uri = resource.uri,
                    .name = resource.name.empty() ? resource.uri : resource.name,
                    .mime_type = resource.mime_type.empty() ? "text/plain" : resource.mime_type,
                    .description = resource.description.empty() ? std::nullopt : std::optional<std::string>{resource.description},
                });
            }
        }
        return resources;
    }

    [[nodiscard]] std::expected<McpToolResult, McpError> read_resource(
        std::string_view server_name,
        std::string_view uri
    ) {
        if (server_name == "filesystem" || server_name == "local" || uri.starts_with("file://")) {
            auto local = read_local_resource(uri);
            if (!local) return std::unexpected(McpError::ResourceNotFound);
            return McpToolResult{.content = *local, .content_type = "text"};
        }

        if (auto loaded = ensure_loaded_from_config(); !loaded) {
            return std::unexpected(McpError::ConnectionFailed);
        }

        std::lock_guard lock(mutex_);
        auto snapshot = manager_->snapshot_server(std::string(server_name));
        if (!snapshot) return std::unexpected(McpError::ServerNotFound);
        if (snapshot->status != svc_mcp::ConnectionStatus::Connected) {
            auto connected = manager_->connect_server(std::string(server_name));
            if (!connected) return std::unexpected(map_native_error(connected.error()));
        }
        auto result = manager_->read_resource(std::string(server_name), uri);
        if (!result) return std::unexpected(map_native_error(result.error()));

        std::string content;
        for (const auto& item : result->contents) {
            if (!content.empty()) content += "\n";
            content += item.text.empty() ? item.blob : item.text;
        }
        return McpToolResult{
            .content = content.empty() ? "MCP resource returned no content." : std::move(content),
            .content_type = "text",
        };
    }

private:
    NativeMcpRuntime() = default;

    void ensure_manager_locked() {
        if (manager_) return;
        manager_ = std::make_unique<svc_mcp::McpConnectionManager>(
            svc_mcp::ConnectionManagerConfig{
                .config_directory = fs::current_path(),
                .connection_timeout = std::chrono::milliseconds{30000},
                .auto_connect_on_start = false,
            });
    }

    [[nodiscard]] static std::string signature(const std::vector<NativeMcpConfiguredServer>& servers) {
        std::string out;
        for (const auto& server : servers) {
            out += server.name;
            out += '\n';
            out += std::to_string(static_cast<int>(server.transport));
            out += '\n';
            out += server.command;
            out += '\n';
            out += server.url;
            out += '\n';
            for (const auto& arg : server.args) {
                out += arg;
                out += '\0';
            }
            out += '\n';
            std::vector<std::pair<std::string, std::string>> env(server.env.begin(), server.env.end());
            std::ranges::sort(env);
            for (const auto& [key, value] : env) {
                out += key;
                out += '=';
                out += value;
                out += '\0';
            }
            out += '\n';
            std::vector<std::pair<std::string, std::string>> headers(server.headers.begin(), server.headers.end());
            std::ranges::sort(headers);
            for (const auto& [key, value] : headers) {
                out += key;
                out += '=';
                out += value;
                out += '\0';
            }
            out += '\n';
        }
        return out;
    }

    [[nodiscard]] std::expected<std::string, std::string> read_local_resource(std::string_view uri) {
        std::string path_text(uri);
        if (path_text.starts_with("file://")) path_text = path_text.substr(7);
        fs::path path = path_text;
        if (!fs::exists(path) || fs::is_directory(path)) {
            return std::unexpected("Resource is not a readable file");
        }
        std::ifstream input(path);
        if (!input) return std::unexpected("Failed to open resource");
        std::stringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    std::mutex mutex_;
    std::unique_ptr<svc_mcp::McpConnectionManager> manager_;
    std::vector<NativeMcpConfiguredServer> configured_servers_;
    std::string config_signature_;
    bool loaded_ = false;
};

inline std::expected<void, std::string> sync_native_mcp_servers(
    std::vector<NativeMcpConfiguredServer> servers
) {
    return NativeMcpRuntime::instance().sync(std::move(servers));
}

inline std::expected<void, std::string> upsert_native_mcp_servers(
    std::vector<NativeMcpConfiguredServer> servers
) {
    return NativeMcpRuntime::instance().upsert(std::move(servers));
}

inline std::expected<NativeMcpServerStatus, std::string> restart_native_mcp_server(std::string_view server_name) {
    return NativeMcpRuntime::instance().restart(server_name);
}

inline std::optional<NativeMcpServerStatus> native_mcp_status(std::string_view server_name) {
    return NativeMcpRuntime::instance().status(server_name);
}

inline std::vector<NativeMcpServerStatus> native_mcp_statuses() {
    return NativeMcpRuntime::instance().all_statuses();
}

inline std::expected<std::vector<McpResource>, McpError> list_native_mcp_resources(
    std::optional<std::string> server_name
) {
    return NativeMcpRuntime::instance().list_resources(std::move(server_name));
}

inline std::expected<McpToolResult, McpError> read_native_mcp_resource(
    std::string_view server_name,
    std::string_view uri
) {
    return NativeMcpRuntime::instance().read_resource(server_name, uri);
}


class McpClientRouter {
public:

    auto register_server(McpServerInfo info) -> std::expected<void, McpError> {
        if (info.name.empty()) return std::unexpected(McpError::InvalidInput);
        servers_.emplace(info.name, std::move(info));
        return {};
    }


    auto find_server(std::string_view name) -> std::expected<McpServerInfo*, McpError> {
        auto it = servers_.find(std::string(name));
        if (it == servers_.end()) return std::unexpected(McpError::ServerNotFound);
        return &it->second;
    }


    auto list_servers() const -> std::vector<const McpServerInfo*> {
        std::vector<const McpServerInfo*> result;
        for (const auto& [_, info] : servers_) {
            result.push_back(&info);
        }
        return result;
    }

private:
    std::unordered_map<std::string, McpServerInfo> servers_;
};


inline McpClientRouter& global_mcp_router() {
    static McpClientRouter router;
    return router;
}


class McpTool {
public:
    static constexpr std::string_view name = "mcp_tool";
    static constexpr std::string_view description = "Invoke a tool exposed by a connected MCP server";

    auto validate(const McpToolRequest& request) const -> std::expected<void, McpError> {
        if (request.server_name.empty() || request.tool_name.empty()) {
            return std::unexpected(McpError::InvalidInput);
        }
        auto server = global_mcp_router().find_server(request.server_name);
        if (!server) return std::unexpected(server.error());
        if (!(*server)->authenticated) {
            return std::unexpected(McpError::AuthRequired);
        }
        return {};
    }

    auto execute(McpToolRequest request) -> std::expected<McpToolResult, McpError> {
        std::string args_json;
        if (request.arguments_json) {
            args_json = *request.arguments_json;
        } else {
            args_json = "{";
            bool first = true;
            for (const auto& [key, value] : request.arguments) {
                if (!first) args_json += ",";
                args_json += std::format("\"{}\":\"{}\"", key, value);
                first = false;
            }
            args_json += "}";
        }

        if (auto native = NativeMcpRuntime::instance().call_tool(
            request.server_name, request.tool_name, args_json); native) {
            return native;
        }

        if (auto v = validate(request); !v) return std::unexpected(v.error());

        return McpToolResult{
            .content = std::format("[MCP call: {}/{} with {}]",
                request.server_name, request.tool_name, args_json),
            .content_type = "text",
        };
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "Name of the MCP server to invoke" }},
      "tool_name": {{ "type": "string", "description": "Name of the tool on the MCP server" }},
      "arguments": {{ "type": "object", "description": "Arguments to pass to the tool" }}
    }},
    "required": ["server_name", "tool_name"]
  }}
}})json", name, description);
    }
};


class ListMcpResourcesTool {
public:
    static constexpr std::string_view name = "list_mcp_resources";
    static constexpr std::string_view description = "List resources available from MCP servers";

    auto execute(std::optional<std::string> server_filter)
        -> std::expected<std::vector<McpResource>, McpError>
    {
        if (auto native = list_native_mcp_resources(server_filter); native) {
            return native;
        }

        std::vector<McpResource> resources;
        auto servers = global_mcp_router().list_servers();

        for (const auto* server : servers) {
            if (server_filter && server->name != *server_filter) continue;
            for (const auto& uri : server->available_resources) {
                resources.push_back(McpResource{.uri = uri, .name = uri, .mime_type = "text/plain", .description = std::nullopt});
            }
        }
        return resources;
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "Filter by server name (optional)" }}
    }}
  }}
}})json", name, description);
    }
};


class ReadMcpResourceTool {
public:
    static constexpr std::string_view name = "read_mcp_resource";
    static constexpr std::string_view description = "Read content of a specific MCP resource";

    auto execute(std::string server_name, std::string resource_uri)
        -> std::expected<McpToolResult, McpError>
    {
        if (server_name.empty() || resource_uri.empty()) {
            return std::unexpected(McpError::InvalidInput);
        }
        if (auto native = read_native_mcp_resource(server_name, resource_uri); native) {
            return native;
        }
        auto server = global_mcp_router().find_server(server_name);
        if (!server) return std::unexpected(server.error());


        return McpToolResult{
            .content = std::format("[Resource content: {}://{}]", server_name, resource_uri),
            .content_type = "text",
        };
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "MCP server name" }},
      "resource_uri": {{ "type": "string", "description": "URI of the resource to read" }}
    }},
    "required": ["server_name", "resource_uri"]
  }}
}})json", name, description);
    }
};


class McpAuthTool {
public:
    static constexpr std::string_view name = "mcp_auth";
    static constexpr std::string_view description = "Handle OAuth authentication for MCP servers";

    auto execute(std::string server_name, std::optional<std::string> auth_code)
        -> std::expected<std::string, McpError>
    {
        if (server_name.empty()) return std::unexpected(McpError::InvalidInput);

        auto server = global_mcp_router().find_server(server_name);
        if (!server) return std::unexpected(server.error());

        if (auth_code) {

            (*server)->authenticated = true;
            return std::format("Successfully authenticated with '{}'", server_name);
        }

        return std::format("Please authorize at: {}/oauth/authorize", (*server)->endpoint);
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "server_name": {{ "type": "string", "description": "MCP server name to authenticate" }},
      "auth_code": {{ "type": "string", "description": "OAuth authorization code (if completing flow)" }}
    }},
    "required": ["server_name"]
  }}
}})json", name, description);
    }
};

} // namespace cc::tools
