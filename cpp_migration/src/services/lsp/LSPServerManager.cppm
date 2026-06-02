// LSP Server Manager Module
module;
#include <any>
#include <expected>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

export module cc.services.lsp.LSPServerManager;

import cc.utils.error;
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
    
    std::unordered_map<std::string, std::unique_ptr<LSPServerInstance>> servers_;
    std::unordered_map<std::string, std::vector<std::string>> extension_map_;
    std::unordered_map<std::string, std::string> opened_files_; // URI -> server name
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
    
    // In real implementation, load server configs and build extension map
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
    
    // Send didOpen notification
    auto result = server->send_notification("textDocument/didOpen", std::any{}); // In real code, proper params
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
    
    return server->send_notification("textDocument/didChange", std::any{}); // In real code, proper params
}

// Save file
Result<void> LSPServerManager::save_file(const std::string& file_path) {
    auto server = get_server_for_file(file_path);
    if (!server || !server->is_running) {
        return {};
    }
    
    return server->send_notification("textDocument/didSave", std::any{}); // In real code, proper params
}

// Close file
Result<void> LSPServerManager::close_file(const std::string& file_path) {
    auto it = opened_files_.find(file_path);
    if (it == opened_files_.end()) {
        return {};
    }
    
    auto server = get_server_for_file(file_path);
    if (server && server->is_running) {
        auto result = server->send_notification("textDocument/didClose", std::any{}); // In real code, proper params
        // Ignore error
    }
    
    opened_files_.erase(it);
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

} // namespace cc::services::lsp
