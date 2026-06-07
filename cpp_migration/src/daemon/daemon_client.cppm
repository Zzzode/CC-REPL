/// @file daemon_client.cppm
/// @brief Client that connects to a running daemon server via JSON-RPC.
/// Provides daemon discovery (PID/port files), session management commands,
/// and status queries over newline-delimited JSON-RPC protocol.
module;

#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <vector>
#include <cstdint>
#include <format>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fstream>
#include <filesystem>
#include <signal.h>
#include <cstdlib>
#include <atomic>

export module cc.daemon.daemon_client;

export namespace cc::daemon {

// ============================================================
// Daemon discovery
// ============================================================

/// Default PID file path
inline std::filesystem::path daemon_pid_path() {
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / ".claude" / "daemon.pid";
    return std::filesystem::temp_directory_path() / "cc-repl-daemon.pid";
}

/// Default port file path
inline std::filesystem::path daemon_port_path() {
    const char* home = std::getenv("HOME");
    if (home) return std::filesystem::path(home) / ".claude" / "daemon.port";
    return std::filesystem::temp_directory_path() / "cc-repl-daemon.port";
}

/// Check if a daemon is currently running by looking for the PID file
inline bool is_daemon_running() {
    auto pid_file = daemon_pid_path();
    if (!std::filesystem::exists(pid_file)) return false;

    std::ifstream f(pid_file);
    int pid = 0;
    f >> pid;
    if (pid <= 0) return false;

    return (kill(pid, 0) == 0);
}

/// Get the port of the running daemon from the port file
inline std::optional<uint16_t> get_daemon_port() {
    auto port_file = daemon_port_path();
    if (!std::filesystem::exists(port_file)) return std::nullopt;

    std::ifstream f(port_file);
    int port = 0;
    f >> port;
    if (port <= 0 || port > 65535) return std::nullopt;

    return static_cast<uint16_t>(port);
}

/// Get the PID of the running daemon
inline std::optional<pid_t> get_daemon_pid() {
    auto pid_file = daemon_pid_path();
    if (!std::filesystem::exists(pid_file)) return std::nullopt;

    std::ifstream f(pid_file);
    int pid = 0;
    f >> pid;
    if (pid <= 0) return std::nullopt;

    // Validate process is alive
    if (kill(pid, 0) != 0) return std::nullopt;

    return static_cast<pid_t>(pid);
}

// ============================================================
// DaemonClient — JSON-RPC client for daemon communication
// ============================================================

class DaemonClient {
public:
    DaemonClient() = default;
    ~DaemonClient() { disconnect(); }

    // Prevent copy
    DaemonClient(const DaemonClient&) = delete;
    DaemonClient& operator=(const DaemonClient&) = delete;

    /// Auto-discover and connect to running daemon
    auto connect_auto() -> std::expected<void, std::string> {
        if (!is_daemon_running()) {
            return std::unexpected("No daemon running");
        }

        auto port = get_daemon_port();
        if (!port) {
            return std::unexpected("Daemon port file not found");
        }

        return connect(*port);
    }

    /// Connect to the daemon on the given port
    auto connect(uint16_t port) -> std::expected<void, std::string> {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return std::unexpected("Failed to create socket");
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return std::unexpected(std::format("Failed to connect to daemon on port {}", port));
        }

        port_ = port;
        connected_ = true;
        return {};
    }

    /// Send a JSON-RPC request and receive the response
    auto call(std::string_view method, std::string_view params = "{}")
        -> std::expected<std::string, std::string> {
        if (!connected_ || fd_ < 0) {
            return std::unexpected("Not connected to daemon");
        }

        auto id = std::format("{}", ++request_counter_);
        auto request = std::format(
            R"({{"jsonrpc":"2.0","id":"{}","method":"{}","params":{}}})",
            id, method, params);
        request += '\n';

        ssize_t sent = ::send(fd_, request.data(), request.size(), 0);
        if (sent < 0) {
            connected_ = false;
            return std::unexpected("Failed to send request");
        }

        // Read response until newline
        std::string response;
        char buf[4096];
        while (true) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf) - 1, 0);
            if (n <= 0) {
                connected_ = false;
                return std::unexpected("Connection closed by daemon");
            }
            response.append(buf, static_cast<size_t>(n));
            if (response.back() == '\n') {
                response.pop_back();
                break;
            }
        }

        // Check for error in response
        if (response.find("\"error\"") != std::string::npos) {
            // Extract error message
            auto msg_pos = response.find("\"message\":\"");
            if (msg_pos != std::string::npos) {
                msg_pos += 11;
                auto end = response.find('"', msg_pos);
                if (end != std::string::npos) {
                    return std::unexpected(response.substr(msg_pos, end - msg_pos));
                }
            }
            return std::unexpected("RPC error: " + response);
        }

        // Extract result field
        auto result_pos = response.find("\"result\":");
        if (result_pos != std::string::npos) {
            return response.substr(result_pos + 9);
        }

        return response;
    }

    // ─── Convenience methods ────────────────────────────────────

    /// Query daemon status
    auto status() -> std::expected<std::string, std::string> {
        return call("status");
    }

    /// List active sessions
    auto list_sessions() -> std::expected<std::string, std::string> {
        return call("sessions");
    }

    /// Spawn a new session with the given task
    auto spawn(std::string_view task_id) -> std::expected<std::string, std::string> {
        auto params = std::format(R"({{"task_id":"{}"}})", task_id);
        return call("spawn", params);
    }

    /// Write one chunk to a daemon-managed headless session stdin.
    auto send_stdin(std::string_view session_id, std::string_view data) -> std::expected<std::string, std::string> {
        auto params = std::format(
            R"({{"session_id":"{}","data":"{}"}})",
            json_escape(session_id),
            json_escape(data));
        return call("stdin", params);
    }

    /// Deliver one raw remote SDK/control event to a daemon-managed headless session.
    auto send_event(std::string_view session_id, std::string_view event_json) -> std::expected<std::string, std::string> {
        auto params = std::format(
            R"({{"session_id":"{}","event":{}}})",
            json_escape(session_id),
            event_json);
        return call("event", params);
    }

    /// Close a daemon-managed headless session stdin.
    auto close_stdin(std::string_view session_id) -> std::expected<std::string, std::string> {
        auto params = std::format(R"({{"session_id":"{}"}})", json_escape(session_id));
        return call("close_stdin", params);
    }

    /// Read captured child stdout lines for a daemon-managed session.
    auto stdout_lines(std::string_view session_id) -> std::expected<std::string, std::string> {
        auto params = std::format(R"({{"session_id":"{}"}})", json_escape(session_id));
        return call("stdout", params);
    }

    /// Request graceful daemon shutdown
    auto shutdown() -> std::expected<std::string, std::string> {
        return call("shutdown");
    }

    /// Check if connected
    [[nodiscard]] bool is_connected() const { return connected_; }

    /// Disconnect from the daemon
    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

private:
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

    int fd_ = -1;
    uint16_t port_ = 0;
    bool connected_ = false;
    uint64_t request_counter_ = 0;
};

} // namespace cc::daemon
