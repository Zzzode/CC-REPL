// LSP Server Manager Module
module;
#include <any>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
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

// LSP Server Manager
class LSPServerManager {
public:
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
    
    // Check if a file is already open on a compatible LSP server
    bool is_file_open(const std::string& file_path) const;

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

    for (const auto& [extension, _language] : config.extension_to_language) {
        extension_map_[extension].push_back(name);
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
