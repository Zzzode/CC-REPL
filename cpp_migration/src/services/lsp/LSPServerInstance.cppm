// LSP Server Instance Module
module;
#include <any>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <expected>
#include <format>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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
    pid_t pid = -1;
    int stdin_fd = -1;
    int stdout_fd = -1;
    int64_t next_request_id = 1;
    std::vector<std::string> outbound_messages;
    
    // Start the server
    Result<void> start();
    
    // Stop the server
    Result<void> stop();
    
    // Send a request
    template<typename T>
    Result<T> send_request(const std::string& method, const std::any& params);
    
    // Send a notification
    Result<void> send_notification(const std::string& method, const std::any& params);

private:
    Result<void> send_message(std::string_view json);
    Result<std::string> read_message(std::chrono::milliseconds timeout);
    static std::string params_to_json(const std::any& params);
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
    if (config.command.empty()) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::invalid_argument,
            "LSP server command is empty"));
    }
    
    int stdin_pipe[2]{};
    int stdout_pipe[2]{};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::network_error,
            "Failed to create LSP process pipes"));
    }

    pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::network_error,
            std::string("Failed to fork LSP server: ") + std::strerror(errno)));
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        for (const auto& [key, value] : config.env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(config.command.c_str()));
        for (auto& arg : config.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp(config.command.c_str(), argv.data());
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    stdin_fd = stdin_pipe[1];
    stdout_fd = stdout_pipe[0];
    fcntl(stdout_fd, F_SETFL, O_NONBLOCK);
    is_running = true;
    return {};
}

// Stop the server
Result<void> LSPServerInstance::stop() {
    if (!is_running) {
        return {};
    }
    
    (void)send_notification("exit", std::string{"null"});
    if (stdin_fd >= 0) {
        close(stdin_fd);
        stdin_fd = -1;
    }
    if (stdout_fd >= 0) {
        close(stdout_fd);
        stdout_fd = -1;
    }
    if (pid > 0) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
        }
        pid = -1;
    }
    is_running = false;
    return {};
}

// Send a request
template<typename T>
Result<T> LSPServerInstance::send_request(const std::string& method, const std::any& params) {
    if (!is_running) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::unavailable, "LSP server not connected"));
    }
    
    const auto id = next_request_id++;
    auto params_json = params_to_json(params);
    auto message = std::format(
        R"({{"jsonrpc":"2.0","id":{},"method":"{}","params":{}}})",
        id,
        method,
        params_json.empty() ? "{}" : params_json);
    auto sent = send_message(message);
    if (!sent) return std::unexpected(sent.error());

    if constexpr (std::is_same_v<T, std::string>) {
        auto response = read_message(std::chrono::milliseconds{30000});
        if (!response) return std::unexpected(response.error());
        return *response;
    }
    return T{};
}

// Send a notification
Result<void> LSPServerInstance::send_notification(const std::string& method, const std::any& params) {
    if (!is_running) {
        return std::unexpected(cc::utils::Error(cc::utils::ErrorCode::unavailable, "LSP server not connected"));
    }
    
    auto params_json = params_to_json(params);
    auto message = params_json.empty()
        ? std::format(R"({{"jsonrpc":"2.0","method":"{}"}})", method)
        : std::format(R"({{"jsonrpc":"2.0","method":"{}","params":{}}})", method, params_json);
    return send_message(message);
}

Result<void> LSPServerInstance::send_message(std::string_view json) {
    if (stdin_fd < 0) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::unavailable,
            "LSP server stdin is closed"));
    }
    std::string frame = std::format("Content-Length: {}\r\n\r\n", json.size());
    frame.append(json);
    outbound_messages.push_back(std::string(json));
    std::string_view remaining(frame);
    while (!remaining.empty()) {
        auto written = write(stdin_fd, remaining.data(), remaining.size());
        if (written <= 0) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::network_error,
                std::string("Failed to write LSP message: ") + std::strerror(errno)));
        }
        remaining.remove_prefix(static_cast<size_t>(written));
    }
    return {};
}

Result<std::string> LSPServerInstance::read_message(std::chrono::milliseconds timeout) {
    if (stdout_fd < 0) {
        return std::unexpected(cc::utils::Error(
            cc::utils::ErrorCode::unavailable,
            "LSP server stdout is closed"));
    }

    auto read_available = [&](std::string& buffer) -> Result<void> {
        std::array<char, 4096> chunk{};
        while (true) {
            auto n = read(stdout_fd, chunk.data(), chunk.size());
            if (n > 0) {
                buffer.append(chunk.data(), static_cast<size_t>(n));
                continue;
            }
            if (n == 0) {
                return std::unexpected(cc::utils::Error(
                    cc::utils::ErrorCode::unavailable,
                    "LSP server closed stdout"));
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return {};
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::network_error,
                std::string("Failed to read LSP response: ") + std::strerror(errno)));
        }
    };

    std::string buffer;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd{.fd = stdout_fd, .events = POLLIN, .revents = 0};
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto poll_timeout = static_cast<int>(std::max<int64_t>(1, remaining.count()));
        auto ready = poll(&fd, 1, poll_timeout);
        if (ready < 0) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::network_error,
                std::string("Failed to poll LSP response: ") + std::strerror(errno)));
        }
        if (ready == 0) break;
        auto read_result = read_available(buffer);
        if (!read_result) return std::unexpected(read_result.error());

        auto header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;
        auto header = buffer.substr(0, header_end);
        auto length_pos = header.find("Content-Length:");
        if (length_pos == std::string::npos) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::parse_error,
                "LSP response missing Content-Length header"));
        }
        length_pos += std::string_view("Content-Length:").size();
        while (length_pos < header.size() && header[length_pos] == ' ') ++length_pos;
        auto length = static_cast<size_t>(std::stoull(header.substr(length_pos)));
        auto content_start = header_end + 4;
        if (buffer.size() >= content_start + length) {
            return buffer.substr(content_start, length);
        }
    }

    return std::unexpected(cc::utils::Error(
        cc::utils::ErrorCode::timeout,
        "Timed out waiting for LSP response"));
}

std::string LSPServerInstance::params_to_json(const std::any& params) {
    if (!params.has_value()) return "{}";
    if (params.type() == typeid(std::string)) {
        return std::any_cast<std::string>(params);
    }
    if (params.type() == typeid(const char*)) {
        return std::string(std::any_cast<const char*>(params));
    }
    if (params.type() == typeid(std::string_view)) {
        return std::string(std::any_cast<std::string_view>(params));
    }
    return "{}";
}

} // namespace cc::services::lsp
