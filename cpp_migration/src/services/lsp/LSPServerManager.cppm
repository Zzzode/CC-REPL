// LSP Server Manager Module
module;
#include <algorithm>
#include <any>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

export module cc.services.lsp.LSPServerManager;

import cc.utils.error;
import cc.utils.json;
import cc.services.lsp.types;
import cc.services.lsp.LSPServerInstance;

export namespace cc::services::lsp {

using cc::utils::Result;
namespace fs = std::filesystem;

struct PluginLspServerDefinition {
    std::string name;
    ScopedLspServerConfig config;
};

namespace detail {

[[nodiscard]] std::optional<std::string> json_string(
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

[[nodiscard]] std::string normalize_lsp_extension(std::string_view extension) {
    while (!extension.empty() && extension.front() == '.') extension.remove_prefix(1);
    return std::string(extension);
}

[[nodiscard]] std::unordered_map<std::string, std::string> parse_extension_to_language(
    cc::utils::json::JsonVal value
) {
    std::unordered_map<std::string, std::string> out;
    if (!value.is_obj()) return out;
    value.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal item) {
        if (!key.is_str() || !item.is_str()) return;
        auto extension = normalize_lsp_extension(key.as_str());
        if (!extension.empty()) out[std::move(extension)] = std::string(item.as_str());
    });
    return out;
}

inline void replace_all(std::string& value, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

[[nodiscard]] std::string sanitize_plugin_data_id(std::string_view plugin_id) {
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

[[nodiscard]] fs::path plugin_data_dir(std::string_view plugin_id) {
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

[[nodiscard]] std::optional<std::string> json_user_config_value_to_string(
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

inline void merge_plugin_lsp_user_config_from_settings(
    const fs::path& settings_path,
    std::string_view plugin_name,
    std::unordered_map<std::string, std::string>& out
) {
    auto parsed = cc::utils::json::parse_file(settings_path);
    if (!parsed) return;
    auto plugin_config = parsed->root().get("pluginConfigs").get(plugin_name);
    if (!plugin_config.is_obj()) return;
    merge_user_config_values(plugin_config.get("options"), out);
}

[[nodiscard]] std::unordered_map<std::string, std::string> load_plugin_lsp_user_config(
    std::string_view plugin_name
) {
    std::unordered_map<std::string, std::string> values;
    if (const char* home = std::getenv("HOME")) {
        merge_plugin_lsp_user_config_from_settings(
            fs::path{home} / ".claude" / "settings.json",
            plugin_name,
            values
        );
    }
    merge_plugin_lsp_user_config_from_settings(
        fs::current_path() / ".claude" / "settings.json",
        plugin_name,
        values
    );
    merge_plugin_lsp_user_config_from_settings(
        fs::current_path() / ".claude" / "settings.local.json",
        plugin_name,
        values
    );
    return values;
}

[[nodiscard]] std::optional<std::string> resolve_plugin_lsp_value(
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

        const auto expression = value.substr(start + 2, end - start - 2);
        if (expression.starts_with("user_config.")) {
            const auto key = expression.substr(std::string_view("user_config.").size());
            auto it = user_config.find(key);
            if (it == user_config.end()) return std::nullopt;
            resolved += it->second;
        } else {
            auto name = expression;
            std::optional<std::string> default_value;
            if (const auto default_pos = expression.find(":-"); default_pos != std::string::npos) {
                name = expression.substr(0, default_pos);
                default_value = expression.substr(default_pos + 2);
            }
            if (const char* env = std::getenv(name.c_str())) {
                resolved += env;
            } else if (default_value) {
                resolved += *default_value;
            } else {
                resolved.append(value.substr(start, end - start + 1));
            }
        }
        pos = end + 1;
    }
    return resolved;
}

[[nodiscard]] std::optional<ScopedLspServerConfig> parse_plugin_lsp_server(
    cc::utils::json::JsonVal config
) {
    if (!config.is_obj()) return std::nullopt;
    if (auto transport = json_string(config, "transport"); transport && *transport != "stdio") {
        return std::nullopt;
    }

    ScopedLspServerConfig server;
    if (auto command = json_string(config, "command")) server.command = std::move(*command);
    append_json_string_array(config.get("args"), server.args);
    append_json_string_map(config.get("env"), server.env);
    server.extension_to_language = parse_extension_to_language(config.get("extensionToLanguage"));
    if (auto workspace_folder = json_string(config, "workspaceFolder")) {
        server.workspace_folder = std::move(*workspace_folder);
    }
    if (auto initialization_options = config.get("initializationOptions"); initialization_options.valid()) {
        server.initialization_options_json = initialization_options.to_string();
    }
    if (server.command.empty() || server.extension_to_language.empty()) return std::nullopt;
    return server;
}

[[nodiscard]] std::optional<ScopedLspServerConfig> resolve_plugin_lsp_server_environment(
    ScopedLspServerConfig server,
    const fs::path& plugin_dir,
    std::string_view plugin_name
) {
    const auto user_config = load_plugin_lsp_user_config(plugin_name);
    auto resolve = [&](std::string value) -> std::optional<std::string> {
        return resolve_plugin_lsp_value(std::move(value), plugin_dir, plugin_name, user_config);
    };

    auto command = resolve(std::move(server.command));
    if (!command) return std::nullopt;
    server.command = std::move(*command);
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
    if (server.workspace_folder) {
        auto resolved = resolve(std::move(*server.workspace_folder));
        if (!resolved) return std::nullopt;
        server.workspace_folder = std::move(*resolved);
    }
    server.env.try_emplace("CLAUDE_PLUGIN_ROOT", plugin_dir.string());
    server.env.try_emplace("CLAUDE_PLUGIN_DATA", plugin_data_dir(plugin_name).string());
    return server;
}

inline void merge_plugin_lsp_servers(
    std::vector<PluginLspServerDefinition>& base,
    std::vector<PluginLspServerDefinition> overlay
) {
    for (auto& server : overlay) {
        std::erase_if(base, [&](const PluginLspServerDefinition& existing) {
            return existing.name == server.name;
        });
        base.push_back(std::move(server));
    }
}

[[nodiscard]] std::vector<PluginLspServerDefinition> parse_plugin_lsp_server_map(
    cc::utils::json::JsonVal servers,
    std::string_view plugin_name,
    const fs::path& plugin_dir
) {
    std::vector<PluginLspServerDefinition> parsed;
    if (!servers.is_obj()) return parsed;
    servers.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str() || !value.is_obj()) return;
        if (auto server = parse_plugin_lsp_server(value)) {
            if (auto resolved = resolve_plugin_lsp_server_environment(
                    std::move(*server),
                    plugin_dir,
                    plugin_name)) {
                parsed.push_back(PluginLspServerDefinition{
                    .name = std::format("plugin:{}:{}", plugin_name, key.as_str()),
                    .config = std::move(*resolved),
                });
            }
        }
    });
    return parsed;
}

[[nodiscard]] bool path_stays_within_plugin(const fs::path& plugin_dir, const fs::path& path) {
    if (path.is_absolute()) return false;
    const auto base = plugin_dir.lexically_normal();
    const auto resolved = (plugin_dir / path).lexically_normal();
    const auto relative = resolved.lexically_relative(base);
    if (relative.empty()) return resolved == base;
    for (const auto& part : relative) {
        if (part == "..") return false;
    }
    return true;
}

[[nodiscard]] std::vector<PluginLspServerDefinition> load_plugin_lsp_servers_from_file(
    const fs::path& plugin_dir,
    std::string_view relative_path,
    std::string_view plugin_name
) {
    if (relative_path.empty()) return {};
    fs::path path{std::string(relative_path)};
    if (!path_stays_within_plugin(plugin_dir, path)) return {};
    path = plugin_dir / path;

    std::ifstream input(path);
    if (!input) return {};
    std::stringstream buffer;
    buffer << input.rdbuf();
    auto doc = cc::utils::json::parse(buffer.str());
    if (!doc) return {};

    auto root = doc->root();
    if (auto wrapped = root.get("lspServers"); wrapped.is_obj()) root = wrapped;
    return parse_plugin_lsp_server_map(root, plugin_name, plugin_dir);
}

[[nodiscard]] std::vector<PluginLspServerDefinition> load_plugin_lsp_servers_from_manifest_spec(
    const fs::path& plugin_dir,
    std::string_view plugin_name,
    cc::utils::json::JsonVal spec
) {
    std::vector<PluginLspServerDefinition> servers;
    if (spec.is_str()) {
        merge_plugin_lsp_servers(
            servers,
            load_plugin_lsp_servers_from_file(plugin_dir, spec.as_str(), plugin_name)
        );
    } else if (spec.is_arr()) {
        spec.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) {
                merge_plugin_lsp_servers(
                    servers,
                    load_plugin_lsp_servers_from_file(plugin_dir, item.as_str(), plugin_name)
                );
            } else if (item.is_obj()) {
                merge_plugin_lsp_servers(
                    servers,
                    parse_plugin_lsp_server_map(item, plugin_name, plugin_dir)
                );
            }
        });
    } else if (spec.is_obj()) {
        merge_plugin_lsp_servers(
            servers,
            parse_plugin_lsp_server_map(spec, plugin_name, plugin_dir)
        );
    }
    return servers;
}

[[nodiscard]] std::vector<PluginLspServerDefinition> load_plugin_lsp_servers_from_dir(
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
    std::vector<PluginLspServerDefinition> servers =
        load_plugin_lsp_servers_from_file(plugin_dir, ".lsp.json", plugin_name);
    if (auto spec = root.get("lspServers"); spec.valid()) {
        merge_plugin_lsp_servers(
            servers,
            load_plugin_lsp_servers_from_manifest_spec(plugin_dir, plugin_name, spec)
        );
    }
    return servers;
}

} // namespace detail

[[nodiscard]] std::vector<PluginLspServerDefinition> discover_plugin_lsp_servers() {
    std::vector<PluginLspServerDefinition> servers;
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
            detail::merge_plugin_lsp_servers(servers, detail::load_plugin_lsp_servers_from_dir(entry.path()));
        }
    }
    return servers;
}

// LSP Server Manager
class LSPServerManager {
public:
    ~LSPServerManager() { (void)shutdown(); }

    // Initialize the manager
    Result<void> initialize();
    
    // Shutdown all running servers and clear state
    Result<void> shutdown();
    
    // Get the LSP server instance for a given file path
    LSPServerInstance* get_server_for_file(const std::string& file_path);
    
    // Ensure the appropriate LSP server is started for the given file
    Result<LSPServerInstance*> ensure_server_started(const std::string& file_path);
    
    // Send a request to the appropriate LSP server
    template<typename T>
    Result<T> send_request(const std::string& file_path, const std::string& method, const std::any& params);
    
    // Get all running server instances
    const std::unordered_map<std::string, std::unique_ptr<LSPServerInstance>>& get_all_servers() const {
        return servers_;
    }
    
    // File synchronization methods
    Result<void> open_file(const std::string& file_path, const std::string& content);
    Result<void> change_file(const std::string& file_path, const std::string& content);
    Result<void> save_file(const std::string& file_path);
    Result<void> close_file(const std::string& file_path);

    // Drain pending notifications and return the latest diagnostics for a file.
    Result<void> poll_file(const std::string& file_path, std::chrono::milliseconds timeout);
    Result<std::string> diagnostics_json_for_file(const std::string& file_path);
    
    // Check if a file is already open on a compatible LSP server
    bool is_file_open(const std::string& file_path) const;
    static std::string file_uri_for_path(const std::string& file_path);

private:
    // Helper to get file extension
    static std::string get_file_extension(const std::string& file_path);
    static std::string env_or(std::string_view name, std::string fallback);
    static std::string path_to_file_uri(const std::string& file_path);
    static std::string uri_encode(std::string_view value);
    void register_server(
        std::string name,
        std::string command,
        std::vector<std::string> args,
        std::unordered_map<std::string, std::string> extension_to_language);
    void register_server_config(std::string name, ScopedLspServerConfig config, bool prefer);
    std::string language_for_file(const std::string& file_path) const;
    std::string build_did_open_params(
        const std::string& file_path,
        const std::string& content,
        int64_t version) const;
    std::string build_did_change_params(
        const std::string& file_path,
        const std::string& content,
        int64_t version) const;
    std::string build_text_document_params(const std::string& file_path) const;
    
    std::unordered_map<std::string, std::unique_ptr<LSPServerInstance>> servers_;
    std::unordered_map<std::string, std::vector<std::string>> extension_map_;
    std::unordered_map<std::string, std::string> opened_files_; // URI -> server name
    std::unordered_map<std::string, int64_t> file_versions_;
    bool initialized_ = false;
};

// Create LSP server manager
std::unique_ptr<LSPServerManager> create_lsp_server_manager() {
    return std::make_unique<LSPServerManager>();
}

std::string LSPServerManager::file_uri_for_path(const std::string& file_path) {
    return path_to_file_uri(file_path);
}

// Initialize the manager
Result<void> LSPServerManager::initialize() {
    if (initialized_) {
        return {};
    }
    
    register_server(
        "typescript",
        env_or("CC_REPL_TYPESCRIPT_LANGUAGE_SERVER", "typescript-language-server"),
        {"--stdio"},
        {{"ts", "typescript"}, {"tsx", "typescriptreact"}, {"js", "javascript"}, {"jsx", "javascriptreact"}});
    register_server(
        "python",
        env_or("CC_REPL_PYTHON_LANGUAGE_SERVER", "pyright-langserver"),
        {"--stdio"},
        {{"py", "python"}});
    register_server(
        "rust",
        env_or("CC_REPL_RUST_LANGUAGE_SERVER", "rust-analyzer"),
        {},
        {{"rs", "rust"}});
    register_server(
        "go",
        env_or("CC_REPL_GO_LANGUAGE_SERVER", "gopls"),
        {},
        {{"go", "go"}});
    register_server(
        "cpp",
        env_or("CC_REPL_CPP_LANGUAGE_SERVER", "clangd"),
        {},
        {{"c", "c"}, {"cc", "cpp"}, {"cpp", "cpp"}, {"cxx", "cpp"}, {"h", "c"}, {"hpp", "cpp"}});
    register_server(
        "java",
        env_or("CC_REPL_JAVA_LANGUAGE_SERVER", "jdtls"),
        {},
        {{"java", "java"}});
    for (auto& server : discover_plugin_lsp_servers()) {
        register_server_config(std::move(server.name), std::move(server.config), true);
    }
    initialized_ = true;
    return {};
}

// Shutdown all servers
Result<void> LSPServerManager::shutdown() {
    for (auto& [name, server] : servers_) {
        if (server->is_running) {
            auto result = server->stop();
            // Ignore errors during shutdown
        }
    }
    servers_.clear();
    extension_map_.clear();
    opened_files_.clear();
    file_versions_.clear();
    initialized_ = false;
    return {};
}

// Get server for file
LSPServerInstance* LSPServerManager::get_server_for_file(const std::string& file_path) {
    auto ext = get_file_extension(file_path);
    auto it = extension_map_.find(ext);
    if (it == extension_map_.end()) {
        return nullptr;
    }
    
    // Try each server for this extension
    for (const auto& server_name : it->second) {
        auto server_it = servers_.find(server_name);
        if (server_it != servers_.end()) {
            return server_it->second.get();
        }
    }
    return nullptr;
}

// Ensure server is started
Result<LSPServerInstance*> LSPServerManager::ensure_server_started(const std::string& file_path) {
    if (!initialized_) {
        auto init = initialize();
        if (!init) return std::unexpected(init.error());
    }
    auto server = get_server_for_file(file_path);
    if (!server) {
        return nullptr;
    }
    
    if (!server->is_running) {
        auto start_result = server->start();
        if (!start_result) {
            return std::unexpected(start_result.error());
        }
    }
    
    return server;
}

// Send request
template<typename T>
Result<T> LSPServerManager::send_request(const std::string& file_path, const std::string& method, const std::any& params) {
    auto server_result = ensure_server_started(file_path);
    if (!server_result) {
        return std::unexpected(server_result.error());
    }
    
    auto server = *server_result;
    if (!server) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::not_found, "No LSP server for file type"));
    }
    
    return server->send_request<T>(method, params);
}

// Open file
Result<void> LSPServerManager::open_file(const std::string& file_path, const std::string& content) {
    auto server_result = ensure_server_started(file_path);
    if (!server_result) {
        return std::unexpected(server_result.error());
    }
    
    auto server = *server_result;
    if (!server) {
        return {}; // No server for this file type
    }
    
    auto version = ++file_versions_[file_path];
    auto result = server->send_notification(
        "textDocument/didOpen",
        build_did_open_params(file_path, content, version));
    if (!result) {
        return result;
    }
    
    // Track opened file
    opened_files_[file_path] = server->name;
    return {};
}

// Change file
Result<void> LSPServerManager::change_file(const std::string& file_path, const std::string& content) {
    auto server = get_server_for_file(file_path);
    if (!server || !server->is_running) {
        return {};
    }
    
    auto version = ++file_versions_[file_path];
    return server->send_notification(
        "textDocument/didChange",
        build_did_change_params(file_path, content, version));
}

// Save file
Result<void> LSPServerManager::save_file(const std::string& file_path) {
    auto server = get_server_for_file(file_path);
    if (!server || !server->is_running) {
        return {};
    }
    
    return server->send_notification(
        "textDocument/didSave",
        build_text_document_params(file_path));
}

// Close file
Result<void> LSPServerManager::close_file(const std::string& file_path) {
    auto it = opened_files_.find(file_path);
    if (it == opened_files_.end()) {
        return {};
    }
    
    auto server = get_server_for_file(file_path);
    if (server && server->is_running) {
        auto result = server->send_notification(
            "textDocument/didClose",
            build_text_document_params(file_path));
        // Ignore error
    }
    
    opened_files_.erase(it);
    file_versions_.erase(file_path);
    return {};
}

Result<void> LSPServerManager::poll_file(const std::string& file_path, std::chrono::milliseconds timeout) {
    auto server_result = ensure_server_started(file_path);
    if (!server_result) {
        return std::unexpected(server_result.error());
    }
    auto* server = *server_result;
    if (!server) return {};
    return server->poll(timeout);
}

Result<std::string> LSPServerManager::diagnostics_json_for_file(const std::string& file_path) {
    auto server_result = ensure_server_started(file_path);
    if (!server_result) {
        return std::unexpected(server_result.error());
    }
    auto* server = *server_result;
    if (!server) return std::string{"[]"};
    return server->diagnostics_json_for_uri(path_to_file_uri(file_path));
}

// Check if file is open
bool LSPServerManager::is_file_open(const std::string& file_path) const {
    return opened_files_.contains(file_path);
}

// Helper to get file extension
std::string LSPServerManager::get_file_extension(const std::string& file_path) {
    auto dot_pos = file_path.rfind('.');
    if (dot_pos == std::string::npos) {
        return "";
    }
    return file_path.substr(dot_pos + 1);
}

std::string LSPServerManager::env_or(std::string_view name, std::string fallback) {
    auto key = std::string(name);
    if (const char* value = std::getenv(key.c_str()); value && *value) {
        return value;
    }
    return fallback;
}

std::string LSPServerManager::uri_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        bool safe =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
        if (safe) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0x0F]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string LSPServerManager::path_to_file_uri(const std::string& file_path) {
    auto absolute = std::filesystem::absolute(std::filesystem::path(file_path)).string();
    return "file://" + uri_encode(absolute);
}

void LSPServerManager::register_server(
    std::string name,
    std::string command,
    std::vector<std::string> args,
    std::unordered_map<std::string, std::string> extension_to_language) {
    ScopedLspServerConfig config;
    config.command = std::move(command);
    config.args = std::move(args);
    config.extension_to_language = std::move(extension_to_language);
    register_server_config(std::move(name), std::move(config), false);
}

void LSPServerManager::register_server_config(
    std::string name,
    ScopedLspServerConfig config,
    bool prefer) {
    for (const auto& [extension, _language] : config.extension_to_language) {
        auto& servers = extension_map_[extension];
        std::erase(servers, name);
        if (prefer) {
            servers.insert(servers.begin(), name);
        } else {
            servers.push_back(name);
        }
    }

    auto instance = create_lsp_server_instance(name, config);
    if (instance) {
        servers_[name] = std::move(*instance);
    }
}

std::string LSPServerManager::language_for_file(const std::string& file_path) const {
    auto ext = get_file_extension(file_path);
    auto map_it = extension_map_.find(ext);
    if (map_it == extension_map_.end() || map_it->second.empty()) return ext;
    auto server_it = servers_.find(map_it->second.front());
    if (server_it == servers_.end()) return ext;
    auto language_it = server_it->second->config.extension_to_language.find(ext);
    return language_it == server_it->second->config.extension_to_language.end()
        ? ext
        : language_it->second;
}

std::string LSPServerManager::build_did_open_params(
    const std::string& file_path,
    const std::string& content,
    int64_t version) const {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    auto text_document = doc.object();
    text_document.add("uri", doc.string(path_to_file_uri(file_path)));
    text_document.add("languageId", doc.string(language_for_file(file_path)));
    text_document.add("version", doc.number(version));
    text_document.add("text", doc.string(content));
    root.add("textDocument", text_document);
    doc.set_root(root);
    return doc.to_string();
}

std::string LSPServerManager::build_did_change_params(
    const std::string& file_path,
    const std::string& content,
    int64_t version) const {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    auto text_document = doc.object();
    text_document.add("uri", doc.string(path_to_file_uri(file_path)));
    text_document.add("version", doc.number(version));
    root.add("textDocument", text_document);
    auto changes = doc.array();
    auto change = doc.object();
    change.add("text", doc.string(content));
    changes.append(change);
    root.add("contentChanges", changes);
    doc.set_root(root);
    return doc.to_string();
}

std::string LSPServerManager::build_text_document_params(const std::string& file_path) const {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    auto text_document = doc.object();
    text_document.add("uri", doc.string(path_to_file_uri(file_path)));
    root.add("textDocument", text_document);
    doc.set_root(root);
    return doc.to_string();
}

} // namespace cc::services::lsp
