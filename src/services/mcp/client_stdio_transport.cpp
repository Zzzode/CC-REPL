// Implementation unit for cc.services.mcp.client — StdioTransport bodies.
// Out-of-line so the POSIX GMF below never enters the module interface BMI.
module;

#include <cerrno>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

module cc.services.mcp.client;

import std;

import cc.services.mcp.types;

namespace cc::services::mcp {

StdioTransport::StdioTransport(std::string command, std::vector<std::string> args,
                               std::map<std::string, std::string> env)
    : command_(std::move(command))
    , args_(std::move(args))
    , env_(std::move(env)) {}

StdioTransport::~StdioTransport() {
    close();
}

McpResult<void> StdioTransport::start() {
    if (is_connected()) {
        return std::unexpected(McpClientError::AlreadyConnected);
    }

    int child_stdin[2]{-1, -1};
    int child_stdout[2]{-1, -1};
    if (::pipe(child_stdin) != 0 || ::pipe(child_stdout) != 0) {
        if (child_stdin[0] >= 0) ::close(child_stdin[0]);
        if (child_stdin[1] >= 0) ::close(child_stdin[1]);
        if (child_stdout[0] >= 0) ::close(child_stdout[0]);
        if (child_stdout[1] >= 0) ::close(child_stdout[1]);
        return std::unexpected(McpClientError::ConnectionFailed);
    }

    child_pid_ = ::fork();
    if (child_pid_ < 0) {
        ::close(child_stdin[0]);
        ::close(child_stdin[1]);
        ::close(child_stdout[0]);
        ::close(child_stdout[1]);
        child_pid_ = -1;
        return std::unexpected(McpClientError::ConnectionFailed);
    }

    if (child_pid_ == 0) {
        ::dup2(child_stdin[0], STDIN_FILENO);
        ::dup2(child_stdout[1], STDOUT_FILENO);
        ::close(child_stdin[0]);
        ::close(child_stdin[1]);
        ::close(child_stdout[0]);
        ::close(child_stdout[1]);

        for (const auto& [key, value] : env_) {
            ::setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(args_.size() + 2);
        argv.push_back(const_cast<char*>(command_.c_str()));
        for (auto& arg : args_) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(command_.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(child_stdin[0]);
    ::close(child_stdout[1]);
    write_fd_ = child_stdin[1];
    read_fd_ = child_stdout[0];
    connected_ = true;
    return {};
}

McpResult<void> StdioTransport::send(std::string_view message) {
    if (!is_connected()) {
        return std::unexpected(McpClientError::NotConnected);
    }

    auto msg = std::string(message) + "\n";
    std::size_t written = 0;
    while (written < msg.size()) {
        const auto bytes = ::write(write_fd_, msg.data() + written, msg.size() - written);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(McpClientError::TransportError);
        }
        if (bytes == 0) {
            return std::unexpected(McpClientError::TransportError);
        }
        written += static_cast<std::size_t>(bytes);
    }
    return {};
}

McpResult<std::string> StdioTransport::receive() {
    if (!is_connected()) {
        return std::unexpected(McpClientError::NotConnected);
    }

    std::string result;
    char ch = '\0';
    while (true) {
        const auto bytes = ::read(read_fd_, &ch, 1);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(McpClientError::TransportError);
        }
        if (bytes == 0) {
            connected_ = false;
            return std::unexpected(McpClientError::ServerClosed);
        }
        if (ch == '\n') break;
        result.push_back(ch);
    }

    if (!result.empty() && result.back() == '\r') {
        result.pop_back();
    }

    return result;
}

void StdioTransport::close() {
    if (write_fd_ >= 0) {
        ::close(write_fd_);
        write_fd_ = -1;
    }
    if (read_fd_ >= 0) {
        ::close(read_fd_);
        read_fd_ = -1;
    }
    if (child_pid_ > 0) {
        int status = 0;
        if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
            ::kill(child_pid_, SIGTERM);
            if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
                ::kill(child_pid_, SIGKILL);
                ::waitpid(child_pid_, &status, 0);
            }
        }
        child_pid_ = -1;
    }
    connected_ = false;
}

} // namespace cc::services::mcp
