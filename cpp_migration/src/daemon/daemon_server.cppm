/// @file daemon_server.cppm
/// @brief Daemon server with PID file management, JSON-RPC protocol,
/// session spawning, work queue polling, and graceful shutdown.
module;

#include <string>
#include <string_view>
#include <expected>
#include <functional>
#include <optional>
#include <vector>
#include <queue>
#include <cstdint>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <format>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

export module cc.daemon.daemon_server;

export namespace cc::daemon {

// ============================================================
// Daemon configuration
// ============================================================

struct DaemonConfig {
    uint16_t port = 0;                              // 0 = auto-assign
    std::filesystem::path pid_file;                 // PID file path
    std::filesystem::path port_file;                // Port file path
    std::chrono::seconds poll_interval{30};         // Work queue poll interval
    std::chrono::seconds heartbeat_interval{60};    // Health check interval
    int max_sessions = 4;                           // Max concurrent sessions
    std::string work_api_url;                       // CCR API work queue endpoint
};

// ============================================================
// Session descriptor
// ============================================================

struct DaemonSession {
    std::string id;
    pid_t pid = -1;
    std::chrono::system_clock::time_point started_at;
    std::string status;   // "running", "completed", "failed"
    std::string task_id;  // Associated work item
};

// ============================================================
// JSON-RPC request/response
// ============================================================

struct RpcRequest {
    std::string id;
    std::string method;
    std::string params;  // JSON params
};

struct RpcResponse {
    std::string id;
    std::string result;  // JSON result
    std::optional<std::string> error;
};

// ============================================================
// DaemonServer — Full daemon with lifecycle management
// ============================================================

class DaemonServer {
public:
    DaemonServer() = default;

    explicit DaemonServer(DaemonConfig config) : config_(std::move(config)) {
        if (config_.pid_file.empty()) {
            config_.pid_file = default_pid_path();
        }
        if (config_.port_file.empty()) {
            config_.port_file = default_port_path();
        }
    }

    ~DaemonServer() {
        stop();
    }

    // Prevent copy
    DaemonServer(const DaemonServer&) = delete;
    DaemonServer& operator=(const DaemonServer&) = delete;

    /// Start the daemon: bind socket, write PID file, begin accept loop
    auto start() -> std::expected<uint16_t, std::string> {
        // Check for existing daemon
        if (is_another_daemon_running()) {
            return std::unexpected("Another daemon is already running");
        }

        // Create server socket
        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            return std::unexpected("Failed to create socket");
        }

        int opt = 1;
        ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Bind to localhost
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(config_.port);

        if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(server_fd_);
            server_fd_ = -1;
            return std::unexpected(std::format("Failed to bind to port {}", config_.port));
        }

        if (::listen(server_fd_, 8) < 0) {
            ::close(server_fd_);
            server_fd_ = -1;
            return std::unexpected("Failed to listen");
        }

        // Get actual port
        struct sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        ::getsockname(server_fd_, reinterpret_cast<struct sockaddr*>(&bound), &blen);
        port_ = ntohs(bound.sin_port);

        // Write PID and port files
        auto pid_result = write_pid_file();
        if (!pid_result) {
            ::close(server_fd_);
            server_fd_ = -1;
            return std::unexpected(pid_result.error());
        }
        write_port_file();

        // Set non-blocking for accept loop
        int flags = fcntl(server_fd_, F_GETFL, 0);
        fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

        running_.store(true);

        // Start work poller thread
        if (!config_.work_api_url.empty()) {
            work_poller_thread_ = std::thread([this] { work_poll_loop(); });
        }

        // Start accept loop thread
        accept_thread_ = std::thread([this] { accept_loop(); });

        return port_;
    }

    /// Stop the daemon gracefully
    void stop() {
        if (!running_.exchange(false)) return;

        // Close server socket to unblock accept
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }

        // Join threads
        if (accept_thread_.joinable()) accept_thread_.join();
        if (work_poller_thread_.joinable()) work_poller_thread_.join();

        // Close all client connections
        {
            std::lock_guard lock(clients_mutex_);
            for (int fd : client_fds_) {
                ::close(fd);
            }
            client_fds_.clear();
        }

        // Cleanup PID and port files
        cleanup_pid_file();
        cleanup_port_file();
    }

    /// Check if the server is running
    [[nodiscard]] bool is_running() const { return running_.load(); }

    /// Get the port the server is listening on
    [[nodiscard]] uint16_t port() const { return port_; }

    /// Get active sessions
    [[nodiscard]] std::vector<DaemonSession> sessions() const {
        std::lock_guard lock(sessions_mutex_);
        return sessions_;
    }

    /// Register a request handler for JSON-RPC methods
    void on_request(std::function<RpcResponse(const RpcRequest&)> handler) {
        rpc_handler_ = std::move(handler);
    }

    /// Spawn a new session as a child process
    auto spawn_session(std::string_view task_id) -> std::expected<std::string, std::string> {
        std::lock_guard lock(sessions_mutex_);

        if (static_cast<int>(sessions_.size()) >= config_.max_sessions) {
            return std::unexpected("Max sessions reached");
        }

        auto session_id = generate_session_id();

        pid_t pid = fork();
        if (pid < 0) {
            return std::unexpected("Fork failed");
        }

        if (pid == 0) {
            // Child process — exec cc-repl with session args
            std::string sid_arg = std::format("--session-id={}", session_id);
            std::string task_arg = std::format("--task-id={}", task_id);
            
            execlp("cc-repl", "cc-repl", 
                   "--headless", sid_arg.c_str(), task_arg.c_str(), nullptr);
            _exit(1); // exec failed
        }

        // Parent: record the session
        DaemonSession session;
        session.id = session_id;
        session.pid = pid;
        session.started_at = std::chrono::system_clock::now();
        session.status = "running";
        session.task_id = std::string(task_id);
        sessions_.push_back(std::move(session));

        return session_id;
    }

    /// Check on running sessions, clean up finished ones
    void reap_sessions() {
        std::lock_guard lock(sessions_mutex_);
        
        for (auto& session : sessions_) {
            if (session.status != "running") continue;
            
            int status = 0;
            pid_t result = waitpid(session.pid, &status, WNOHANG);
            if (result > 0) {
                session.status = WIFEXITED(status) && WEXITSTATUS(status) == 0 
                    ? "completed" : "failed";
            } else if (result < 0) {
                session.status = "failed";
            }
        }

        // Remove old completed sessions (keep recent ones)
        std::erase_if(sessions_, [](const DaemonSession& s) {
            if (s.status == "running") return false;
            auto age = std::chrono::system_clock::now() - s.started_at;
            return age > std::chrono::hours(1);
        });
    }

private:
    /// Accept loop — runs in background thread
    void accept_loop() {
        while (running_.load()) {
            struct pollfd pfd{};
            pfd.fd = server_fd_;
            pfd.events = POLLIN;

            int ret = ::poll(&pfd, 1, 1000); // 1s timeout
            if (ret <= 0 || !running_.load()) continue;

            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = ::accept(server_fd_,
                reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
            
            if (client_fd < 0) continue;

            {
                std::lock_guard lock(clients_mutex_);
                client_fds_.push_back(client_fd);
            }

            // Handle client in a detached thread
            std::thread([this, client_fd] {
                handle_client(client_fd);
                std::lock_guard lock(clients_mutex_);
                std::erase(client_fds_, client_fd);
            }).detach();
        }
    }

    /// Handle a connected client — read JSON-RPC requests, send responses
    void handle_client(int fd) {
        std::string buffer;
        char buf[4096];

        while (running_.load()) {
            ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            buffer.append(buf, static_cast<size_t>(n));

            // Process complete messages (newline-delimited JSON-RPC)
            while (true) {
                auto nl = buffer.find('\n');
                if (nl == std::string::npos) break;

                auto line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);

                if (line.empty()) continue;

                auto request = parse_rpc_request(line);
                RpcResponse response;

                if (request.has_value()) {
                    response = dispatch_request(*request);
                } else {
                    response.id = "0";
                    response.error = "Parse error";
                }

                auto response_json = serialize_rpc_response(response);
                response_json += '\n';
                ::send(fd, response_json.data(), response_json.size(), 0);
            }
        }

        ::close(fd);
    }

    /// Dispatch a JSON-RPC request to the appropriate handler
    RpcResponse dispatch_request(const RpcRequest& req) {
        // Built-in methods
        if (req.method == "status") {
            return RpcResponse{req.id, R"({"status":"running"})", std::nullopt};
        }
        if (req.method == "sessions") {
            return handle_sessions_request(req);
        }
        if (req.method == "spawn") {
            return handle_spawn_request(req);
        }
        if (req.method == "shutdown") {
            std::thread([this] {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                stop();
            }).detach();
            return RpcResponse{req.id, R"({"shutting_down":true})", std::nullopt};
        }

        // User-registered handler
        if (rpc_handler_) {
            return rpc_handler_(req);
        }

        return RpcResponse{req.id, "", "Unknown method: " + req.method};
    }

    /// Handle "sessions" RPC
    RpcResponse handle_sessions_request(const RpcRequest& req) {
        reap_sessions();
        std::lock_guard lock(sessions_mutex_);
        
        std::string json = "[";
        for (size_t i = 0; i < sessions_.size(); ++i) {
            if (i > 0) json += ',';
            json += std::format(
                R"({{"id":"{}","pid":{},"status":"{}","task_id":"{}"}})",
                sessions_[i].id, sessions_[i].pid,
                sessions_[i].status, sessions_[i].task_id);
        }
        json += ']';
        return RpcResponse{req.id, json, std::nullopt};
    }

    /// Handle "spawn" RPC
    RpcResponse handle_spawn_request(const RpcRequest& req) {
        std::string task_id;
        auto pos = req.params.find("\"task_id\":\"");
        if (pos != std::string::npos) {
            pos += 11;
            auto end = req.params.find('"', pos);
            if (end != std::string::npos) {
                task_id = req.params.substr(pos, end - pos);
            }
        }

        if (task_id.empty()) {
            return RpcResponse{req.id, "", "Missing task_id parameter"};
        }

        auto result = spawn_session(task_id);
        if (result) {
            return RpcResponse{req.id, 
                std::format(R"({{"session_id":"{}"}})", *result), std::nullopt};
        }
        return RpcResponse{req.id, "", result.error()};
    }

    /// Work queue polling loop
    void work_poll_loop() {
        while (running_.load()) {
            std::this_thread::sleep_for(config_.poll_interval);
            if (!running_.load()) break;

            reap_sessions();

            int running_count = 0;
            {
                std::lock_guard lock(sessions_mutex_);
                for (const auto& s : sessions_) {
                    if (s.status == "running") ++running_count;
                }
            }

            if (running_count < config_.max_sessions) {
                poll_for_work();
            }
        }
    }

    /// Poll the CCR API for available work items
    void poll_for_work() {
        // Check the work queue endpoint. When no HTTP client is available,
        // this is a no-op — the daemon relies on incoming stdio RPC requests.
        // Once wired, this will GET /api/v1/work and enqueue returned items.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    /// Parse a JSON-RPC request from a line
    static std::optional<RpcRequest> parse_rpc_request(std::string_view line) {
        RpcRequest req;

        auto extract = [&](std::string_view key) -> std::string {
            auto pattern = std::format(R"("{}":")", key);
            auto pos = line.find(pattern);
            if (pos == std::string_view::npos) return "";
            pos += pattern.size();
            auto end = line.find('"', pos);
            if (end == std::string_view::npos) return "";
            return std::string(line.substr(pos, end - pos));
        };

        req.id = extract("id");
        req.method = extract("method");
        if (req.method.empty()) return std::nullopt;

        auto params_pos = line.find("\"params\":");
        if (params_pos != std::string_view::npos) {
            req.params = std::string(line.substr(params_pos + 9));
        }

        return req;
    }

    /// Serialize a JSON-RPC response
    static std::string serialize_rpc_response(const RpcResponse& resp) {
        if (resp.error.has_value()) {
            return std::format(
                R"({{"jsonrpc":"2.0","id":"{}","error":{{"message":"{}"}}}})",
                resp.id, *resp.error);
        }
        return std::format(
            R"({{"jsonrpc":"2.0","id":"{}","result":{}}})",
            resp.id, resp.result.empty() ? "null" : resp.result);
    }

    /// Check if another daemon is running
    bool is_another_daemon_running() const {
        if (!std::filesystem::exists(config_.pid_file)) return false;
        std::ifstream f(config_.pid_file);
        int pid = 0;
        f >> pid;
        if (pid <= 0) return false;
        return (kill(pid, 0) == 0);
    }

    /// Write PID file
    auto write_pid_file() -> std::expected<void, std::string> {
        std::error_code ec;
        std::filesystem::create_directories(config_.pid_file.parent_path(), ec);
        std::ofstream f(config_.pid_file);
        if (!f) return std::unexpected("Failed to create PID file");
        f << getpid();
        return {};
    }

    /// Write port file
    void write_port_file() {
        std::error_code ec;
        std::filesystem::create_directories(config_.port_file.parent_path(), ec);
        std::ofstream f(config_.port_file);
        if (f) f << port_;
    }

    void cleanup_pid_file() {
        std::error_code ec;
        std::filesystem::remove(config_.pid_file, ec);
    }

    void cleanup_port_file() {
        std::error_code ec;
        std::filesystem::remove(config_.port_file, ec);
    }

    static std::string generate_session_id() {
        static std::atomic<uint64_t> counter{0};
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return std::format("sess_{}_{}", ms, counter.fetch_add(1));
    }

    static std::filesystem::path default_pid_path() {
        const char* home = std::getenv("HOME");
        if (home) return std::filesystem::path(home) / ".claude" / "daemon.pid";
        return std::filesystem::temp_directory_path() / "cc-repl-daemon.pid";
    }

    static std::filesystem::path default_port_path() {
        const char* home = std::getenv("HOME");
        if (home) return std::filesystem::path(home) / ".claude" / "daemon.port";
        return std::filesystem::temp_directory_path() / "cc-repl-daemon.port";
    }

    // Configuration
    DaemonConfig config_;
    
    // Server socket
    int server_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};

    // Threads
    std::thread accept_thread_;
    std::thread work_poller_thread_;

    // Client tracking
    std::vector<int> client_fds_;
    std::mutex clients_mutex_;

    // Sessions
    mutable std::mutex sessions_mutex_;
    std::vector<DaemonSession> sessions_;

    // RPC handler
    std::function<RpcResponse(const RpcRequest&)> rpc_handler_;
};

} // namespace cc::daemon
