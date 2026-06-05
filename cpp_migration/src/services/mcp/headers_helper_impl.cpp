module;
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

module cc.services.mcp.headers_helper;

import cc.utils.json;

namespace cc::services::mcp {

std::string trim_header_helper_output(std::string_view value) {
    while (!value.empty()) {
        const auto ch = static_cast<unsigned char>(value.front());
        if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
        value.remove_prefix(1);
    }
    while (!value.empty()) {
        const auto ch = static_cast<unsigned char>(value.back());
        if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') break;
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::optional<HeaderMap> parse_header_helper_json(std::string_view output) {
    const auto trimmed = trim_header_helper_output(output);
    if (trimmed.empty()) return std::nullopt;

    auto parsed = cc::utils::json::parse(trimmed);
    if (!parsed || !parsed->root().is_obj()) return std::nullopt;

    HeaderMap headers;
    bool valid = true;
    parsed->root().iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal value) {
        if (!key.is_str() || !value.is_str()) {
            valid = false;
            return;
        }
        headers[std::string(key.as_str())] = std::string(value.as_str());
    });
    if (!valid) return std::nullopt;
    return headers;
}

std::optional<std::string> run_headers_helper_command(
    std::string_view server_name,
    std::string_view server_url,
    std::string_view command,
    std::chrono::milliseconds timeout
) {
    if (command.empty()) return std::nullopt;

    int pipe_fds[2]{-1, -1};
    if (::pipe(pipe_fds) != 0) return std::nullopt;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        const std::string child_server_name(server_name);
        const std::string child_server_url(server_url);
        const std::string child_command(command);
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::close(pipe_fds[1]);
        ::setenv("CLAUDE_CODE_MCP_SERVER_NAME", child_server_name.c_str(), 1);
        ::setenv("CLAUDE_CODE_MCP_SERVER_URL", child_server_url.c_str(), 1);
        ::execl("/bin/sh", "sh", "-c", child_command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(pipe_fds[1]);
    const int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string output;
    bool child_done = false;
    bool pipe_done = false;
    int status = 0;
    std::array<char, 4096> buffer{};

    auto drain_pipe = [&]() {
        while (true) {
            const auto n = ::read(pipe_fds[0], buffer.data(), buffer.size());
            if (n > 0) {
                output.append(buffer.data(), static_cast<std::size_t>(n));
                if (output.size() > 1024 * 1024) {
                    ::kill(pid, SIGKILL);
                    pipe_done = true;
                    return;
                }
                continue;
            }
            if (n == 0) {
                pipe_done = true;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                pipe_done = true;
            }
            break;
        }
    };

    while (!pipe_done || !child_done) {
        if (!child_done) {
            const auto done = ::waitpid(pid, &status, WNOHANG);
            if (done == pid) child_done = true;
        }

        drain_pipe();
        if (pipe_done && child_done) break;

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            ::close(pipe_fds[0]);
            return std::nullopt;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto poll_timeout = static_cast<int>(std::min<std::int64_t>(remaining.count(), 100));
        pollfd pfd{.fd = pipe_fds[0], .events = POLLIN | POLLHUP, .revents = 0};
        (void)::poll(&pfd, 1, poll_timeout);
    }

    ::close(pipe_fds[0]);
    if (!child_done) {
        ::waitpid(pid, &status, 0);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    return output;
}

std::optional<HeaderMap> get_mcp_headers_from_helper(
    std::string_view server_name,
    std::string_view server_url,
    std::string_view helper_command,
    std::chrono::milliseconds timeout
) {
    auto output = run_headers_helper_command(server_name, server_url, helper_command, timeout);
    if (!output) return std::nullopt;
    return parse_header_helper_json(*output);
}

HeaderMap get_mcp_server_headers(
    std::string_view server_name,
    const HeaderMap& static_headers,
    std::string_view server_url,
    std::string_view helper_command,
    std::chrono::milliseconds timeout
) {
    auto result = static_headers;
    auto dynamic_headers = get_mcp_headers_from_helper(server_name, server_url, helper_command, timeout);
    if (!dynamic_headers) return result;
    for (auto& [key, value] : *dynamic_headers) {
        result[key] = value;
    }
    return result;
}

} // namespace cc::services::mcp
