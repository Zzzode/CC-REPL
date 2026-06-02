// LSP Server Instance Module
module;
#include <any>
#include <expected>
#include <memory>
#include <string>
#include <system_error>

export module cc.services.lsp.LSPServerInstance;

import cc.utils.error;
import cc.services.lsp.types;

export namespace cc::services::lsp {

using cc::utils::Result;
using cc::services::lsp::ScopedLspServerConfig;

// Forward declarations
struct LSPServerInstance;

// LSP Server Instance
struct LSPServerInstance {
    std::string name;
    ScopedLspServerConfig config;
    bool is_running = false;
    // Add more fields like process handle, message queues, etc.
    
    // Start the server
    Result<void> start();
    
    // Stop the server
    Result<void> stop();
    
    // Send a request
    template<typename T>
    Result<T> send_request(const std::string& method, const std::any& params);
    
    // Send a notification
    Result<void> send_notification(const std::string& method, const std::any& params);
};

// Create an LSP server instance
Result<std::unique_ptr<LSPServerInstance>> create_lsp_server_instance(
    const std::string& name,
    const ScopedLspServerConfig& config) {
    
    auto instance = std::make_unique<LSPServerInstance>();
    instance->name = name;
    instance->config = config;
    return instance;
}

// Start the server
Result<void> LSPServerInstance::start() {
    if (is_running) {
        return {};
    }
    
    // In real implementation, spawn the LSP server process
    is_running = true;
    return {};
}

// Stop the server
Result<void> LSPServerInstance::stop() {
    if (!is_running) {
        return {};
    }
    
    // In real implementation, shut down the LSP server process
    is_running = false;
    return {};
}

// Send a request
template<typename T>
Result<T> LSPServerInstance::send_request(const std::string& method, const std::any& params) {
    if (!is_running) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::unavailable, "LSP server not connected"));
    }
    
    // In real implementation, send request via LSP protocol
    return T{};
}

// Send a notification
Result<void> LSPServerInstance::send_notification(const std::string& method, const std::any& params) {
    if (!is_running) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::unavailable, "LSP server not connected"));
    }
    
    // In real implementation, send notification via LSP protocol
    return {};
}

} // namespace cc::services::lsp
