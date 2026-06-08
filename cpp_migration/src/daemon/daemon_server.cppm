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
#include <cerrno>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

export module cc.daemon.daemon_server;

import cc.bridge.api;
import cc.bridge.work_secret;
import cc.utils.json;

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
    std::string work_api_url;                       // CCR API base URL
    std::string bridge_environment_id;              // Backend-issued bridge environment ID
    std::string bridge_environment_secret;          // Backend-issued bridge environment secret
    std::string bridge_access_token;                // OAuth token for stop/deregister operations
    std::string bridge_runner_version{"cc-repl-daemon"};
    std::optional<std::string> trusted_device_token;
    std::string session_binary{"cc-repl"};          // Headless child executable
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
    std::optional<std::string> remote_session_id;
    std::optional<std::string> session_ingress_token;
    std::optional<std::string> session_api_base_url;
    std::optional<bool> use_code_sessions;
    std::optional<std::string> code_session_mode;
    std::optional<int64_t> worker_epoch;
    std::vector<std::string> work_secret_sources_json;
    std::optional<std::string> work_secret_auth_json;
    std::optional<std::string> work_secret_mcp_config_json;
    std::optional<std::string> work_secret_environment_json;
    std::unordered_map<std::string, std::string> work_secret_environment_variables;
    std::optional<std::string> work_secret_raw_json;
    std::optional<std::chrono::system_clock::time_point> last_heartbeat_at;
    std::optional<std::string> last_heartbeat_state;
    std::optional<int> heartbeat_ttl_seconds;
    std::optional<std::string> last_heartbeat_error;
    int heartbeat_failures = 0;
    int stdin_fd = -1;
    std::vector<std::string> stdout_lines;
    bool stdout_closed = false;
    bool completion_reported = false;
    std::size_t delivered_remote_events = 0;
    std::optional<std::string> last_delivered_remote_event_type;
    std::optional<std::string> last_remote_event_error;
};

struct DaemonBackoffState {
    int consecutive_poll_failures = 0;
    std::chrono::seconds current_poll_backoff{0};
    std::optional<std::string> last_poll_error;
    bool auth_failed = false;
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
        cleanup_session_io();
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
        if (!running_.exchange(false)) {
            cleanup_session_io();
            return;
        }

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
        cleanup_session_io();
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

    [[nodiscard]] DaemonBackoffState backoff_state() const {
        std::lock_guard lock(state_mutex_);
        return backoff_state_;
    }

    /// Register a request handler for JSON-RPC methods
    void on_request(std::function<RpcResponse(const RpcRequest&)> handler) {
        rpc_handler_ = std::move(handler);
    }

    /// Override process spawning, mainly for tests and alternate runners.
    void set_session_spawner(std::function<std::expected<std::string, std::string>(std::string_view)> spawner) {
        session_spawner_ = std::move(spawner);
    }

    /// Poll once for bridge work and spawn a session if work is available.
    auto poll_for_work_once() -> std::expected<std::optional<std::string>, std::string> {
        auto result = poll_for_work();
        if (result) {
            record_poll_success();
        } else {
            record_poll_failure(result.error());
        }
        return result;
    }

    /// Send heartbeat for all running bridge work sessions.
    auto heartbeat_sessions_once() -> std::expected<std::size_t, std::string> {
        return heartbeat_running_sessions();
    }

    /// Write one chunk to a running headless child stdin.
    auto send_session_stdin(std::string_view session_id, std::string_view data)
        -> std::expected<void, std::string> {
        if (session_id.empty()) return std::unexpected("Session ID is required");
        if (data.empty()) return {};

        int fd = -1;
        {
            std::lock_guard lock(sessions_mutex_);
            auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
                return session.id == session_id;
            });
            if (it == sessions_.end()) return std::unexpected("Session not found");
            if (it->status != "running") return std::unexpected("Session is not running");
            if (it->stdin_fd < 0) return std::unexpected("Session stdin is closed");
            fd = it->stdin_fd;
        }

        auto remaining = data;
        while (!remaining.empty()) {
            auto written = ::write(fd, remaining.data(), remaining.size());
            if (written < 0) {
                if (errno == EINTR) continue;
                return std::unexpected("Failed to write session stdin");
            }
            if (written == 0) return std::unexpected("Session stdin write made no progress");
            remaining.remove_prefix(static_cast<std::size_t>(written));
        }
        return {};
    }

    /// Close a running headless child stdin pipe so stream-json input can finish.
    auto close_session_stdin(std::string_view session_id) -> std::expected<void, std::string> {
        if (session_id.empty()) return std::unexpected("Session ID is required");
        int fd = -1;
        {
            std::lock_guard lock(sessions_mutex_);
            auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
                return session.id == session_id;
            });
            if (it == sessions_.end()) return std::unexpected("Session not found");
            fd = it->stdin_fd;
            it->stdin_fd = -1;
        }
        if (fd >= 0) ::close(fd);
        return {};
    }

    /// Deliver one raw remote SDK/control event to a daemon-managed child.
    auto deliver_session_event_to_stdin(std::string_view session_id, std::string_view event_json)
        -> std::expected<void, std::string> {
        if (session_id.empty()) return std::unexpected("Session ID is required");
        if (event_json.empty()) return std::unexpected("Remote event JSON is required");

        auto parsed = cc::utils::json::parse(event_json);
        if (!parsed || !parsed->root().is_obj()) {
            record_remote_event_failure(session_id, "Remote event must be a JSON object");
            return std::unexpected("Remote event must be a JSON object");
        }

        auto type = parsed->root().get("type");
        if (!type.is_str() || type.as_str().empty()) {
            record_remote_event_failure(session_id, "Remote event must include a string type");
            return std::unexpected("Remote event must include a string type");
        }

        std::string line(event_json);
        if (line.empty() || line.back() != '\n') line.push_back('\n');
        auto written = send_session_stdin(session_id, line);
        if (!written) {
            record_remote_event_failure(session_id, written.error());
            return std::unexpected(written.error());
        }

        record_remote_event_success(session_id, type.as_str());
        return {};
    }

    /// Return child stdout NDJSON/text lines captured by the daemon.
    [[nodiscard]] std::vector<std::string> session_stdout_lines(std::string_view session_id) const {
        std::lock_guard lock(sessions_mutex_);
        auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
            return session.id == session_id;
        });
        if (it == sessions_.end()) return {};
        return it->stdout_lines;
    }

    /// Mark a daemon session finished and report bridge work completion once.
    auto complete_session(std::string_view session_id, std::string_view status)
        -> std::expected<void, std::string> {
        std::optional<DaemonSession> finished;
        {
            std::lock_guard lock(sessions_mutex_);
            auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
                return session.id == session_id;
            });
            if (it == sessions_.end()) return std::unexpected("Session not found");
            it->status = status.empty() ? std::string("completed") : std::string(status);
            if (!it->completion_reported) {
                it->completion_reported = true;
                finished = *it;
            }
        }
        if (finished) report_bridge_session_finished(*finished);
        return {};
    }

    /// Spawn a new session as a child process
    auto spawn_session(std::string_view task_id) -> std::expected<std::string, std::string> {
        return spawn_session_with_bridge_context(task_id, nullptr, nullptr);
    }

    auto spawn_session_with_bridge_context(
        std::string_view task_id,
        const cc::bridge::WorkResponse* work,
        const cc::bridge::DecodedWorkSecret* secret
    ) -> std::expected<std::string, std::string> {
        std::lock_guard lock(sessions_mutex_);

        if (static_cast<int>(sessions_.size()) >= config_.max_sessions) {
            return std::unexpected("Max sessions reached");
        }

        auto session_id = generate_session_id();
        const auto remote_session_id = work ? work->data_id : std::nullopt;
        const auto child_api_base_url = secret ? bridge_child_api_base_url(*secret) : std::string{};
        auto sdk_url = secret
            ? bridge_child_sdk_url(*secret, remote_session_id, child_api_base_url)
            : std::nullopt;
        auto worker_epoch = secret
            ? register_bridge_worker_if_needed(*secret, sdk_url)
            : std::expected<std::optional<int64_t>, std::string>{std::optional<int64_t>{}};
        if (!worker_epoch) return std::unexpected(worker_epoch.error());

        int stdin_pipe[2]{-1, -1};
        int stdout_pipe[2]{-1, -1};
        if (::pipe(stdin_pipe) != 0) {
            return std::unexpected("Failed to create stdin pipe");
        }
        if (::pipe(stdout_pipe) != 0) {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
            return std::unexpected("Failed to create stdout pipe");
        }

        pid_t pid = fork();
        if (pid < 0) {
            ::close(stdin_pipe[0]);
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::close(stdout_pipe[1]);
            return std::unexpected("Fork failed");
        }

        if (pid == 0) {
            ::close(stdin_pipe[1]);
            ::close(stdout_pipe[0]);
            ::dup2(stdin_pipe[0], STDIN_FILENO);
            ::dup2(stdout_pipe[1], STDOUT_FILENO);
            ::close(stdin_pipe[0]);
            ::close(stdout_pipe[1]);
            if (secret) {
                apply_bridge_work_environment(*secret, remote_session_id, task_id, child_api_base_url, *worker_epoch);
            }
            // Child process — exec cc-repl with session args
            std::string sid_arg = std::format("--session-id={}", session_id);
            std::string task_arg = std::format("--task-id={}", task_id);

            std::vector<std::string> args{
                config_.session_binary,
                "--headless",
                "--input-format",
                "stream-json",
                "--output-format",
                "stream-json",
                "--replay-user-messages",
            };
            if (sdk_url && !sdk_url->empty()) {
                args.push_back("--sdk-url");
                args.push_back(*sdk_url);
            }
            args.push_back(sid_arg);
            args.push_back(task_arg);

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (auto& arg : args) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);
            execvp(config_.session_binary.c_str(), argv.data());
            _exit(1); // exec failed
        }

        ::close(stdin_pipe[0]);
        ::close(stdout_pipe[1]);

        // Parent: record the session
        DaemonSession session;
        session.id = session_id;
        session.pid = pid;
        session.started_at = std::chrono::system_clock::now();
        session.status = "running";
        session.task_id = std::string(task_id);
        session.stdin_fd = stdin_pipe[1];
        if (work && secret) {
            apply_bridge_work_metadata(session, *work, *secret, *worker_epoch);
        }
        sessions_.push_back(std::move(session));
        start_stdout_reader(session_id, stdout_pipe[0]);

        return session_id;
    }

    /// Check on running sessions, clean up finished ones
    void reap_sessions() {
        std::vector<DaemonSession> finished_to_report;
        {
            std::lock_guard lock(sessions_mutex_);

            for (auto& session : sessions_) {
                if (session.status != "running") continue;
                if (session.pid <= 0) continue;

                int status = 0;
                pid_t result = waitpid(session.pid, &status, WNOHANG);
                if (result > 0) {
                    session.status = WIFEXITED(status) && WEXITSTATUS(status) == 0
                        ? "completed" : "failed";
                } else if (result < 0) {
                    session.status = "failed";
                }
                if (session.status != "running" && !session.completion_reported) {
                    session.completion_reported = true;
                    finished_to_report.push_back(session);
                }
            }

            // Remove old completed sessions (keep recent ones)
            std::erase_if(sessions_, [](const DaemonSession& s) {
                if (s.status == "running") return false;
                auto age = std::chrono::system_clock::now() - s.started_at;
                return age > std::chrono::hours(1);
            });
        }

        for (const auto& session : finished_to_report) {
            report_bridge_session_finished(session);
        }
    }

private:
    void cleanup_session_io() {
        close_all_session_stdin();
        stop_stdout_readers();
    }

    void close_all_session_stdin() {
        std::vector<int> fds;
        {
            std::lock_guard lock(sessions_mutex_);
            for (auto& session : sessions_) {
                if (session.stdin_fd >= 0) {
                    fds.push_back(session.stdin_fd);
                    session.stdin_fd = -1;
                }
            }
        }
        for (int fd : fds) {
            ::close(fd);
        }
    }

    void stop_stdout_readers() {
        std::vector<std::thread> readers;
        {
            std::lock_guard lock(stdout_readers_mutex_);
            for (auto& [key, reader] : stdout_reader_threads_) {
                auto it = stdout_reader_stop_flags_.find(key);
                if (it != stdout_reader_stop_flags_.end()) {
                    it->second.store(true);
                }
            }
            readers.reserve(stdout_reader_threads_.size());
            for (auto& [_, reader] : stdout_reader_threads_) {
                readers.push_back(std::move(reader));
            }
            stdout_reader_threads_.clear();
        }
        for (auto& reader : readers) {
            if (reader.joinable()) reader.join();
        }
    }

    void start_stdout_reader(std::string_view session_id, int fd) {
        if (fd < 0) return;
        auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        std::lock_guard lock(stdout_readers_mutex_);
        auto key = std::string(session_id);
        stdout_reader_stop_flags_[key].store(false);
        stdout_reader_threads_.insert_or_assign(
            key,
            std::thread([this, key, fd]() {
                read_child_stdout_loop(key, fd);
            }));
    }

    void read_child_stdout_loop(std::string session_id, int fd) {
        std::string buffer;
        char chunk[4096];
        while (!stdout_reader_stop_flags_[session_id].load(std::memory_order_acquire)) {
            auto n = ::read(fd, chunk, sizeof(chunk));
            if (n > 0) {
                buffer.append(chunk, static_cast<std::size_t>(n));
                drain_stdout_lines(session_id, buffer);
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            break;
        }
        if (!buffer.empty()) {
            record_session_stdout_line(session_id, std::move(buffer));
        }
        ::close(fd);
        mark_session_stdout_closed(session_id);
    }

    void drain_stdout_lines(std::string_view session_id, std::string& buffer) {
        while (true) {
            auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            auto line = buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buffer.erase(0, newline + 1);
            record_session_stdout_line(session_id, std::move(line));
        }
    }

    void record_session_stdout_line(std::string_view session_id, std::string line) {
        std::lock_guard lock(sessions_mutex_);
        auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
            return session.id == session_id;
        });
        if (it == sessions_.end()) return;
        it->stdout_lines.push_back(std::move(line));
        if (it->stdout_lines.size() > 1024) {
            it->stdout_lines.erase(it->stdout_lines.begin());
        }
    }

    void mark_session_stdout_closed(std::string_view session_id) {
        std::lock_guard lock(sessions_mutex_);
        auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
            return session.id == session_id;
        });
        if (it == sessions_.end()) return;
        it->stdout_closed = true;
    }

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
        if (req.method == "stdin") {
            return handle_stdin_request(req);
        }
        if (req.method == "event") {
            return handle_event_request(req);
        }
        if (req.method == "close_stdin") {
            return handle_close_stdin_request(req);
        }
        if (req.method == "stdout") {
            return handle_stdout_request(req);
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

    static std::string json_escape(std::string_view value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (char ch : value) {
            switch (ch) {
                case '\\': out += R"(\\)"; break;
                case '"': out += R"(\")"; break;
                case '\b': out += R"(\b)"; break;
                case '\f': out += R"(\f)"; break;
                case '\n': out += R"(\n)"; break;
                case '\r': out += R"(\r)"; break;
                case '\t': out += R"(\t)"; break;
                default: out.push_back(ch); break;
            }
        }
        return out;
    }

    RpcResponse handle_stdin_request(const RpcRequest& req) {
        auto params = cc::utils::json::parse(req.params);
        if (!params || !params->root().is_obj()) return RpcResponse{req.id, "", "Invalid params"};
        auto root = params->root();
        auto session_id = root.get("session_id");
        auto data = root.get("data");
        if (!session_id.is_str() || !data.is_str()) {
            return RpcResponse{req.id, "", "stdin requires session_id and data"};
        }
        auto written = send_session_stdin(session_id.as_str(), data.as_str());
        if (!written) return RpcResponse{req.id, "", written.error()};
        return RpcResponse{req.id, R"({"ok":true})", std::nullopt};
    }

    RpcResponse handle_event_request(const RpcRequest& req) {
        auto params = cc::utils::json::parse(req.params);
        if (!params || !params->root().is_obj()) return RpcResponse{req.id, "", "Invalid params"};
        auto root = params->root();
        auto session_id = root.get("session_id");
        if (!session_id.is_str()) {
            return RpcResponse{req.id, "", "event requires session_id"};
        }

        auto event = root.get("event");
        std::string event_json;
        if (event.is_obj()) {
            event_json = event.to_string();
        } else if (event.is_str()) {
            event_json = std::string(event.as_str());
        } else {
            return RpcResponse{req.id, "", "event requires event object or event JSON string"};
        }

        auto delivered = deliver_session_event_to_stdin(session_id.as_str(), event_json);
        if (!delivered) return RpcResponse{req.id, "", delivered.error()};
        return RpcResponse{req.id, R"({"ok":true})", std::nullopt};
    }

    RpcResponse handle_close_stdin_request(const RpcRequest& req) {
        auto params = cc::utils::json::parse(req.params);
        if (!params || !params->root().is_obj()) return RpcResponse{req.id, "", "Invalid params"};
        auto session_id = params->root().get("session_id");
        if (!session_id.is_str()) {
            return RpcResponse{req.id, "", "close_stdin requires session_id"};
        }
        auto closed = close_session_stdin(session_id.as_str());
        if (!closed) return RpcResponse{req.id, "", closed.error()};
        return RpcResponse{req.id, R"({"ok":true})", std::nullopt};
    }

    RpcResponse handle_stdout_request(const RpcRequest& req) {
        auto params = cc::utils::json::parse(req.params);
        if (!params || !params->root().is_obj()) return RpcResponse{req.id, "", "Invalid params"};
        auto session_id = params->root().get("session_id");
        if (!session_id.is_str()) {
            return RpcResponse{req.id, "", "stdout requires session_id"};
        }
        auto lines = session_stdout_lines(session_id.as_str());
        std::string result = R"({"lines":[)";
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) result += ',';
            result += '"';
            result += json_escape(lines[i]);
            result += '"';
        }
        result += "]}";
        return RpcResponse{req.id, result, std::nullopt};
    }

    /// Work queue polling loop
    void work_poll_loop() {
        auto last_heartbeat = std::chrono::steady_clock::now();
        while (running_.load()) {
            auto sleep_interval = poll_sleep_interval();
            std::this_thread::sleep_for(sleep_interval);
            if (!running_.load()) break;

            reap_sessions();

            const auto now = std::chrono::steady_clock::now();
            const auto heartbeat_interval = normalized_interval(config_.heartbeat_interval);
            if (now - last_heartbeat >= heartbeat_interval) {
                (void)heartbeat_sessions_once();
                last_heartbeat = now;
            }

            int running_count = 0;
            {
                std::lock_guard lock(sessions_mutex_);
                for (const auto& s : sessions_) {
                    if (s.status == "running") ++running_count;
                }
            }

            if (running_count < config_.max_sessions) {
                (void)poll_for_work_once();
            }
        }
    }

    [[nodiscard]] std::chrono::seconds poll_sleep_interval() const {
        auto interval = normalized_interval(config_.poll_interval);
        auto state = backoff_state();
        if (state.current_poll_backoff > interval) {
            interval = state.current_poll_backoff;
        }
        return interval;
    }

    [[nodiscard]] static std::chrono::seconds normalized_interval(std::chrono::seconds interval) {
        if (interval.count() <= 0) return std::chrono::seconds{1};
        return interval;
    }

    /// Poll the CCR API for available work items
    auto poll_for_work() -> std::expected<std::optional<std::string>, std::string> {
        if (config_.work_api_url.empty() ||
            config_.bridge_environment_id.empty() ||
            config_.bridge_environment_secret.empty()) {
            return std::optional<std::string>{};
        }

        cc::bridge::BridgeApiClient client(cc::bridge::BridgeApiConfig{
            .base_url = config_.work_api_url,
            .access_token = config_.bridge_access_token,
            .runner_version = config_.bridge_runner_version,
            .trusted_device_token = config_.trusted_device_token,
        });

        auto work = client.poll_for_work(config_.bridge_environment_id, config_.bridge_environment_secret);
        if (!work) return std::unexpected(work.error().format());
        if (!*work) return std::optional<std::string>{};

        const auto& item = **work;
        {
            std::lock_guard lock(sessions_mutex_);
            if (claimed_work_ids_.contains(item.id)) return std::optional<std::string>{};
        }

        auto decoded_secret = cc::bridge::decode_work_secret(item.secret);
        if (!decoded_secret) {
            auto stopped = client.stop_work(config_.bridge_environment_id, item.id, false);
            (void)stopped;
            return std::unexpected(decoded_secret.error());
        }

        auto ack = client.acknowledge_work(
            config_.bridge_environment_id,
            item.id,
            decoded_secret->session_ingress_token);
        if (!ack) {
            return std::unexpected(ack.error().format());
        }

        if (item.data_type && *item.data_type == "healthcheck") {
            std::lock_guard lock(sessions_mutex_);
            claimed_work_ids_.insert(item.id);
            return std::optional<std::string>{};
        }

        auto spawned = spawn_session_for_work(item, *decoded_secret);
        if (!spawned) return std::unexpected(spawned.error());

        {
            std::lock_guard lock(sessions_mutex_);
            claimed_work_ids_.insert(item.id);
        }
        return std::optional<std::string>{*spawned};
    }

    auto heartbeat_running_sessions() -> std::expected<std::size_t, std::string> {
        if (config_.work_api_url.empty() || config_.bridge_environment_id.empty()) {
            return std::size_t{0};
        }

        struct HeartbeatTarget {
            std::string session_id;
            std::string work_id;
            std::string session_token;
        };

        std::vector<HeartbeatTarget> targets;
        {
            std::lock_guard lock(sessions_mutex_);
            for (const auto& session : sessions_) {
                if (session.status != "running") continue;
                if (session.task_id.empty()) continue;
                if (!session.session_ingress_token || session.session_ingress_token->empty()) continue;
                targets.push_back(HeartbeatTarget{
                    .session_id = session.id,
                    .work_id = session.task_id,
                    .session_token = *session.session_ingress_token,
                });
            }
        }

        if (targets.empty()) return std::size_t{0};

        cc::bridge::BridgeApiClient client(cc::bridge::BridgeApiConfig{
            .base_url = config_.work_api_url,
            .access_token = config_.bridge_access_token,
            .runner_version = config_.bridge_runner_version,
            .trusted_device_token = config_.trusted_device_token,
        });

        std::size_t succeeded = 0;
        std::optional<std::string> first_error;
        for (const auto& target : targets) {
            auto heartbeat = client.heartbeat_work(
                config_.bridge_environment_id,
                target.work_id,
                target.session_token);
            if (heartbeat) {
                ++succeeded;
                update_session_heartbeat_success(target.session_id, *heartbeat);
            } else {
                auto error = heartbeat.error().format();
                if (!first_error) first_error = error;
                update_session_heartbeat_failure(target.session_id, error);
            }
        }

        if (first_error) return std::unexpected(*first_error);
        return succeeded;
    }

    void update_session_heartbeat_success(
        std::string_view session_id,
        const cc::bridge::HeartbeatResponse& heartbeat
    ) {
        std::lock_guard lock(sessions_mutex_);
        auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
            return session.id == session_id;
        });
        if (it == sessions_.end()) return;
        it->last_heartbeat_at = std::chrono::system_clock::now();
        it->last_heartbeat_state = heartbeat.state;
        it->heartbeat_ttl_seconds = heartbeat.ttl_seconds;
        it->last_heartbeat_error = std::nullopt;
        it->heartbeat_failures = 0;
    }

    void update_session_heartbeat_failure(std::string_view session_id, std::string error) {
        std::lock_guard lock(sessions_mutex_);
        auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
            return session.id == session_id;
        });
        if (it == sessions_.end()) return;
        it->last_heartbeat_error = std::move(error);
        ++it->heartbeat_failures;
    }

    void record_poll_success() {
        std::lock_guard lock(state_mutex_);
        backoff_state_ = DaemonBackoffState{};
    }

    void record_poll_failure(std::string_view error) {
        std::lock_guard lock(state_mutex_);
        ++backoff_state_.consecutive_poll_failures;
        backoff_state_.last_poll_error = std::string(error);
        backoff_state_.auth_failed = is_auth_failure(error);
        backoff_state_.current_poll_backoff = calculate_poll_backoff(backoff_state_.consecutive_poll_failures);
    }

    [[nodiscard]] std::chrono::seconds calculate_poll_backoff(int failures) const {
        auto base = normalized_interval(config_.poll_interval);
        auto multiplier = 1;
        for (int i = 1; i < failures && multiplier < 64; ++i) {
            multiplier *= 2;
        }
        auto delay = base * multiplier;
        constexpr auto max_backoff = std::chrono::seconds{300};
        return delay > max_backoff ? max_backoff : delay;
    }

    [[nodiscard]] static bool is_auth_failure(std::string_view error) {
        return error.find("[E200]") != std::string_view::npos ||
            error.find("HTTP 401") != std::string_view::npos ||
            error.find("HTTP 403") != std::string_view::npos ||
            error.find("authentication") != std::string_view::npos ||
            error.find("Authentication") != std::string_view::npos;
    }

    auto spawn_session_for_work(
        const cc::bridge::WorkResponse& work,
        const cc::bridge::DecodedWorkSecret& secret
    ) -> std::expected<std::string, std::string> {
        if (!session_spawner_) return spawn_session_with_bridge_context(work.id, &work, &secret);

        const auto child_api_base_url = bridge_child_api_base_url(secret);
        auto sdk_url = bridge_child_sdk_url(secret, work.data_id, child_api_base_url);
        auto worker_epoch = register_bridge_worker_if_needed(secret, sdk_url);
        if (!worker_epoch) return std::unexpected(worker_epoch.error());

        std::lock_guard lock(sessions_mutex_);
        if (static_cast<int>(sessions_.size()) >= config_.max_sessions) {
            return std::unexpected("Max sessions reached");
        }

        auto spawned = session_spawner_(work.id);
        if (!spawned) return std::unexpected(spawned.error());

        DaemonSession session;
        session.id = *spawned;
        session.pid = -1;
        session.started_at = std::chrono::system_clock::now();
        session.status = "running";
        session.task_id = work.id;
        apply_bridge_work_metadata(session, work, secret, *worker_epoch);
        sessions_.push_back(std::move(session));
        return *spawned;
    }

    void record_remote_event_success(std::string_view session_id, std::string_view type) {
        std::lock_guard lock(sessions_mutex_);
        auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
            return session.id == session_id;
        });
        if (it == sessions_.end()) return;
        ++it->delivered_remote_events;
        it->last_delivered_remote_event_type = std::string(type);
        it->last_remote_event_error = std::nullopt;
    }

    void record_remote_event_failure(std::string_view session_id, std::string error) {
        std::lock_guard lock(sessions_mutex_);
        auto it = std::ranges::find_if(sessions_, [&](const DaemonSession& session) {
            return session.id == session_id;
        });
        if (it == sessions_.end()) return;
        it->last_remote_event_error = std::move(error);
    }

    static void apply_bridge_work_metadata(
        DaemonSession& session,
        const cc::bridge::WorkResponse& work,
        const cc::bridge::DecodedWorkSecret& secret,
        const std::optional<int64_t>& worker_epoch = std::nullopt
    ) {
        session.remote_session_id = work.data_id;
        session.session_ingress_token = secret.session_ingress_token;
        session.session_api_base_url = secret.api_base_url;
        session.use_code_sessions = secret.use_code_sessions;
        session.code_session_mode = secret.code_session_mode;
        session.worker_epoch = worker_epoch;
        session.work_secret_sources_json = secret.sources_json;
        session.work_secret_auth_json = secret.auth_json;
        session.work_secret_mcp_config_json = secret.mcp_config_json;
        session.work_secret_environment_json = secret.environment_json;
        session.work_secret_environment_variables = secret.environment_variables;
        session.work_secret_raw_json = secret.raw_json;
    }

    static std::string strip_trailing_slashes(std::string value) {
        while (!value.empty() && value.back() == '/') value.pop_back();
        return value;
    }

    static bool api_base_url_is_local(std::string_view api_base_url) {
        std::string lower(api_base_url);
        std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return lower.find("localhost") != std::string::npos ||
            lower.find("127.0.0.1") != std::string::npos;
    }

    static std::string api_base_url_host(std::string base_url) {
        base_url = strip_trailing_slashes(std::move(base_url));
        if (base_url.starts_with("https://")) return base_url.substr(8);
        if (base_url.starts_with("http://")) return base_url.substr(7);
        return base_url;
    }

    static std::optional<std::string> bridge_child_sdk_url(
        const cc::bridge::DecodedWorkSecret& secret,
        const std::optional<std::string>& remote_session_id,
        std::string_view api_base_url
    ) {
        if (!remote_session_id || remote_session_id->empty()) return std::nullopt;
        if (api_base_url.empty()) return std::nullopt;
        if (secret.use_code_sessions.value_or(false)) {
            return strip_trailing_slashes(std::string(api_base_url)) + "/v1/code/sessions/" + *remote_session_id;
        }

        const bool is_local = api_base_url_is_local(api_base_url);
        const auto protocol = is_local ? std::string_view("ws") : std::string_view("wss");
        const auto version = is_local ? std::string_view("v2") : std::string_view("v1");
        return std::format(
            "{}://{}/{}/session_ingress/ws/{}",
            protocol,
            api_base_url_host(std::string(api_base_url)),
            version,
            *remote_session_id);
    }

    std::string bridge_child_api_base_url(const cc::bridge::DecodedWorkSecret& secret) const {
        if (secret.use_code_sessions.value_or(false) && !config_.work_api_url.empty()) {
            return strip_trailing_slashes(config_.work_api_url);
        }
        return strip_trailing_slashes(secret.api_base_url);
    }

    auto register_bridge_worker_if_needed(
        const cc::bridge::DecodedWorkSecret& secret,
        const std::optional<std::string>& sdk_url
    ) -> std::expected<std::optional<int64_t>, std::string> {
        if (!secret.use_code_sessions.value_or(false)) return std::optional<int64_t>{};
        if (!sdk_url || sdk_url->empty()) return std::optional<int64_t>{};
        if (config_.work_api_url.empty()) return std::optional<int64_t>{};

        cc::bridge::BridgeApiClient client(cc::bridge::BridgeApiConfig{
            .base_url = config_.work_api_url,
            .access_token = config_.bridge_access_token,
            .runner_version = config_.bridge_runner_version,
            .trusted_device_token = config_.trusted_device_token,
        });
        auto registration = client.register_worker(*sdk_url, secret.session_ingress_token);
        if (!registration) return std::unexpected(registration.error().format());
        return std::optional<int64_t>{registration->worker_epoch};
    }

    static bool is_valid_env_name(std::string_view name) {
        if (name.empty()) return false;
        auto first = static_cast<unsigned char>(name.front());
        if (!(std::isalpha(first) || name.front() == '_')) return false;
        for (char ch : name.substr(1)) {
            auto c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || ch == '_')) return false;
        }
        return true;
    }

    static void set_child_env(std::string_view key, std::string_view value) {
        if (key.empty() || value.empty() || !is_valid_env_name(key)) return;
        ::setenv(std::string(key).c_str(), std::string(value).c_str(), 1);
    }

    static void apply_bridge_work_environment(
        const cc::bridge::DecodedWorkSecret& secret,
        const std::optional<std::string>& remote_session_id,
        std::string_view work_id,
        std::string_view api_base_url,
        const std::optional<int64_t>& worker_epoch
    ) {
        set_child_env("CLAUDE_CODE_SESSION_ACCESS_TOKEN", secret.session_ingress_token);
        set_child_env("CLAUDE_CODE_REMOTE_API_BASE_URL", api_base_url);
        set_child_env("CC_REPL_REMOTE_API_BASE_URL", api_base_url);
        set_child_env("CLAUDE_CODE_REMOTE", "1");
        set_child_env("CLAUDE_CODE_BRIDGE_WORK_ID", work_id);
        set_child_env("CC_REPL_BRIDGE_WORK_ID", work_id);
        if (!secret.use_code_sessions.value_or(false)) {
            set_child_env("CLAUDE_CODE_POST_FOR_SESSION_INGRESS_V2", "1");
        }
        if (remote_session_id && !remote_session_id->empty()) {
            set_child_env("CC_REMOTE_SESSION_ID", *remote_session_id);
            set_child_env("CLAUDE_CODE_REMOTE_SESSION_ID", *remote_session_id);
        }
        if (secret.use_code_sessions) {
            set_child_env("CLAUDE_CODE_USE_CODE_SESSIONS", *secret.use_code_sessions ? "true" : "false");
            if (*secret.use_code_sessions) set_child_env("CLAUDE_CODE_USE_CCR_V2", "1");
        }
        if (worker_epoch) {
            set_child_env("CLAUDE_CODE_WORKER_EPOCH", std::to_string(*worker_epoch));
        }
        if (secret.code_session_mode && !secret.code_session_mode->empty()) {
            set_child_env("CLAUDE_CODE_SESSION_MODE", *secret.code_session_mode);
        }
        if (!secret.raw_json.empty()) {
            set_child_env("CLAUDE_CODE_BRIDGE_WORK_SECRET_JSON", secret.raw_json);
        }
        if (secret.auth_json && !secret.auth_json->empty()) {
            set_child_env("CLAUDE_CODE_REMOTE_AUTH_JSON", *secret.auth_json);
        }
        if (secret.mcp_config_json && !secret.mcp_config_json->empty()) {
            set_child_env("CLAUDE_CODE_REMOTE_MCP_CONFIG_JSON", *secret.mcp_config_json);
        }
        if (secret.environment_json && !secret.environment_json->empty()) {
            set_child_env("CLAUDE_CODE_REMOTE_ENVIRONMENT_JSON", *secret.environment_json);
        }
        for (const auto& [key, value] : secret.environment_variables) {
            set_child_env(key, value);
        }
    }

    void report_bridge_session_finished(const DaemonSession& session) {
        if (config_.work_api_url.empty() || config_.bridge_environment_id.empty() ||
            config_.bridge_access_token.empty() || session.task_id.empty()) {
            return;
        }
        if (session.status == "interrupted") return;

        cc::bridge::BridgeApiClient client(cc::bridge::BridgeApiConfig{
            .base_url = config_.work_api_url,
            .access_token = config_.bridge_access_token,
            .runner_version = config_.bridge_runner_version,
            .trusted_device_token = config_.trusted_device_token,
        });
        (void)client.stop_work(config_.bridge_environment_id, session.task_id, false);
        if (session.remote_session_id && !session.remote_session_id->empty()) {
            (void)client.archive_session(*session.remote_session_id);
        }
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
            auto value_start = params_pos + 9;
            while (value_start < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[value_start]))) {
                ++value_start;
            }
            auto value_end = extract_json_value_end(line, value_start);
            if (value_end > value_start) {
                req.params = std::string(line.substr(value_start, value_end - value_start));
            } else {
                req.params = std::string(line.substr(value_start));
            }
        }

        return req;
    }

    static std::size_t extract_json_value_end(std::string_view text, std::size_t start) {
        if (start >= text.size()) return start;
        const char first = text[start];
        if (first == '{' || first == '[') {
            int depth = 0;
            bool in_string = false;
            bool escaped = false;
            for (std::size_t i = start; i < text.size(); ++i) {
                const char ch = text[i];
                if (in_string) {
                    if (escaped) {
                        escaped = false;
                    } else if (ch == '\\') {
                        escaped = true;
                    } else if (ch == '"') {
                        in_string = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    in_string = true;
                    continue;
                }
                if (ch == '{' || ch == '[') {
                    ++depth;
                } else if (ch == '}' || ch == ']') {
                    --depth;
                    if (depth == 0) return i + 1;
                }
            }
            return text.size();
        }
        if (first == '"') {
            bool escaped = false;
            for (std::size_t i = start + 1; i < text.size(); ++i) {
                const char ch = text[i];
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    return i + 1;
                }
            }
            return text.size();
        }
        auto end = start;
        while (end < text.size() && text[end] != ',' && text[end] != '}') ++end;
        return end;
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
    std::unordered_set<std::string> claimed_work_ids_;

    // Poll/reconnect state
    mutable std::mutex state_mutex_;
    DaemonBackoffState backoff_state_;

    // Child stdio readers
    std::mutex stdout_readers_mutex_;
    std::unordered_map<std::string, std::thread> stdout_reader_threads_;
    std::unordered_map<std::string, std::atomic<bool>> stdout_reader_stop_flags_;

    // RPC handler
    std::function<RpcResponse(const RpcRequest&)> rpc_handler_;
    std::function<std::expected<std::string, std::string>(std::string_view)> session_spawner_;
};

} // namespace cc::daemon
