module;
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.services.mcp_transport;

export namespace cc::services::mcp_transport {

enum class MCPTransportType { Stdio, SSE, WebSocket, InProcess };

struct MCPHeaders {
    std::vector<std::pair<std::string, std::string>> headers;
};

struct OAuthPortConfig {
    std::uint16_t port;
    std::string redirect_path;
    std::optional<std::string> state;
};

// In-process transport: paired bidirectional message queues for MCP communication
// within the same process (e.g., plugin ↔ host).
class InProcessTransport {
public:
    using MessageHandler = std::function<void(std::string)>;

    InProcessTransport() = default;

    // Non-copyable, non-movable (owns mutexes)
    InProcessTransport(const InProcessTransport&) = delete;
    InProcessTransport& operator=(const InProcessTransport&) = delete;
    InProcessTransport(InProcessTransport&&) = delete;
    InProcessTransport& operator=(InProcessTransport&&) = delete;

    // Send a message to the peer
    void send(std::string message) {
        std::lock_guard lock(peer_mutex_);
        if (peer_) {
            peer_->enqueue(std::move(message));
        }
    }

    // Set handler for incoming messages
    void set_on_message(MessageHandler handler) {
        std::lock_guard lock(handler_mutex_);
        on_message_ = std::move(handler);
    }

    // Process any queued messages (call from event loop)
    void drain() {
        std::deque<std::string> batch;
        {
            std::lock_guard lock(queue_mutex_);
            batch.swap(queue_);
        }
        std::lock_guard lock(handler_mutex_);
        if (on_message_) {
            for (auto& msg : batch) {
                on_message_(std::move(msg));
            }
        }
    }

    // Check if there are pending messages
    [[nodiscard]] bool has_pending() const {
        std::lock_guard lock(queue_mutex_);
        return !queue_.empty();
    }

    void set_peer(InProcessTransport* peer) {
        std::lock_guard lock(peer_mutex_);
        peer_ = peer;
    }

private:
    void enqueue(std::string message) {
        std::lock_guard lock(queue_mutex_);
        queue_.push_back(std::move(message));
    }

    InProcessTransport* peer_ = nullptr;
    mutable std::mutex queue_mutex_;
    std::mutex peer_mutex_;
    std::mutex handler_mutex_;
    std::deque<std::string> queue_;
    MessageHandler on_message_;
};

// Create a linked pair of in-process transports.
// Messages sent on one are received by the other.
// Returns shared_ptr because InProcessTransport is non-movable (contains mutexes).
struct LinkedTransportPair {
    std::shared_ptr<InProcessTransport> first;
    std::shared_ptr<InProcessTransport> second;
};

inline LinkedTransportPair create_linked_pair() {
    auto a = std::make_shared<InProcessTransport>();
    auto b = std::make_shared<InProcessTransport>();
    a->set_peer(b.get());
    b->set_peer(a.get());
    return {std::move(a), std::move(b)};
}

// Create an in-process transport pair identified by server_id
inline std::expected<void, std::string> create_in_process_transport(std::string_view server_id) {
    if (server_id.empty()) {
        return std::unexpected("Server ID cannot be empty");
    }
    // The actual transport pair is created via create_linked_pair() by the caller.
    // This function validates the server_id and could register it in a global registry.
    return {};
}

// Build authorization headers for MCP connections
inline MCPHeaders build_mcp_headers(std::string_view auth_token) {
    MCPHeaders headers;
    if (!auth_token.empty()) {
        headers.headers.emplace_back("Authorization", std::format("Bearer {}", auth_token));
    }
    headers.headers.emplace_back("Content-Type", "application/json");
    headers.headers.emplace_back("Accept", "application/json");
    return headers;
}

// Setup OAuth redirect port for MCP server auth
inline std::expected<OAuthPortConfig, std::string> setup_oauth_port() {
    return OAuthPortConfig{8080, "/callback", std::nullopt};
}

// IDE lockfile entry
struct IdeLockfile {
    std::string ide_name;          // "vscode", "cursor", "jetbrains", etc.
    std::string transport;         // "ws" or "sse"
    std::string auth_token;
    std::vector<std::string> workspace_folders;
    int pid = 0;
};

// Discover running IDE by scanning lockfiles in ~/.claude/ide/
inline std::optional<IdeLockfile> discover_ide(std::string_view workspace_path) {
    auto home = std::getenv("HOME");
    if (!home) return std::nullopt;

    auto ide_dir = std::filesystem::path(home) / ".claude" / "ide";
    if (!std::filesystem::exists(ide_dir)) return std::nullopt;

    for (const auto& entry : std::filesystem::directory_iterator(ide_dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::ifstream file(entry.path());
        if (!file.is_open()) continue;

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        // Simple JSON field extraction for lockfile
        auto extract = [&](std::string_view key) -> std::string {
            auto pattern = "\"" + std::string(key) + "\"";
            auto pos = content.find(pattern);
            if (pos == std::string::npos) return {};
            pos = content.find('"', pos + pattern.size() + 1); // skip ":"
            if (pos == std::string::npos) return {};
            auto end = content.find('"', pos + 1);
            if (end == std::string::npos) return {};
            return content.substr(pos + 1, end - pos - 1);
        };

        IdeLockfile lockfile;
        lockfile.ide_name = extract("ideName");
        lockfile.transport = extract("transport");
        lockfile.auth_token = extract("authToken");

        // Check if workspace matches
        if (!workspace_path.empty() && content.find(workspace_path) != std::string::npos) {
            // Validate PID is still alive
            auto pid_str = extract("pid");
            if (!pid_str.empty()) {
                lockfile.pid = std::atoi(pid_str.c_str());
                // Check if process is still running (kill with signal 0)
                if (lockfile.pid > 0 && kill(lockfile.pid, 0) == 0) {
                    return lockfile;
                }
            }
        }
    }
    return std::nullopt;
}

// Connect to VS Code SDK MCP server via discovered IDE lockfile
inline std::expected<void, std::string> connect_vscode_sdk(std::string_view extension_id) {
    // Get current working directory as workspace hint
    auto cwd = std::filesystem::current_path().string();
    auto ide = discover_ide(cwd);
    if (!ide) {
        return std::unexpected("No running IDE found for current workspace");
    }
    if (ide->ide_name != "vscode" && ide->ide_name != "cursor" && ide->ide_name != "windsurf") {
        return std::unexpected(std::format("IDE '{}' does not support MCP SDK connection", ide->ide_name));
    }
    if (ide->auth_token.empty()) {
        return std::unexpected("IDE lockfile missing auth token");
    }
    // Connection would be established via the transport type indicated in lockfile.
    // The actual MCP client connection is handled by McpClient using the auth_token.
    // extension_id selects which VS Code extension provides the MCP server.
    if (extension_id.empty()) {
        return std::unexpected("Extension ID is required for VS Code SDK connection");
    }
    return {};
}

// XAA (Cross-App Access) IdP login — enterprise authentication
inline std::expected<void, std::string> xaa_idp_login(std::string_view provider) {
    if (provider.empty()) {
        return std::unexpected("IdP provider name required");
    }
    // XAA token exchange requires enterprise configuration.
    // This is a placeholder for enterprise-specific auth flows.
    return std::unexpected(std::format("XAA IdP login for '{}' not configured", provider));
}

inline std::string normalize_mcp_string(std::string_view input) {
    return std::string(input);
}

inline std::expected<void, std::string> manage_mcp_connections(
    const std::vector<std::string>& server_ids, bool connect) {
    (void)connect;
    if (server_ids.empty()) {
        return std::unexpected("No server IDs provided");
    }
    return {};
}

} // namespace cc::services::mcp_transport
