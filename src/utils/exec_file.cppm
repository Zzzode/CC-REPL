module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <expected>
#include <chrono>
#include <span>
#include <functional>
#include <filesystem>
#include <array>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>

export module cc.utils.exec_file;

export namespace cc::utils {

namespace fs = std::filesystem;
using namespace std::chrono;

// Forward declare ShellResult to avoid circular dependency
struct ExecResult {
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
    milliseconds duration;
};

// Options for executing a file
struct ExecOptions {
    std::optional<fs::path> cwd;
    std::map<std::string, std::string> env;
    std::optional<seconds> timeout;
    size_t max_output = 10 * 1024 * 1024; // 10MB default max output
};

// Execute a file with arguments, capturing output
inline std::expected<ExecResult, std::string>
exec_file(fs::path executable, std::span<std::string> args, ExecOptions opts = {}) {
    auto start = steady_clock::now();

    // Verify executable exists
    if (!fs::exists(executable)) {
        return std::unexpected("Executable not found: " + executable.string());
    }

    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        return std::unexpected("Failed to create pipes");
    }

    pid_t pid = fork();
    if (pid < 0) {
        return std::unexpected("Failed to fork: " + std::string(strerror(errno)));
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (opts.cwd) {
            if (chdir(opts.cwd->c_str()) != 0) _exit(127);
        }

        for (const auto& [key, value] : opts.env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        // Build argv array
        std::vector<const char*> argv;
        argv.push_back(executable.c_str());
        for (const auto& arg : args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execv(executable.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    std::string stdout_output, stderr_output;
    std::array<char, 8192> buffer{};
    bool stdout_done = false, stderr_done = false;
    size_t total_output = 0;

    auto timeout_point = opts.timeout
        ? std::optional(steady_clock::now() + *opts.timeout)
        : std::nullopt;

    while (!stdout_done || !stderr_done) {
        if (timeout_point && steady_clock::now() > *timeout_point) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            return std::unexpected("Execution timed out after " +
                std::to_string(opts.timeout->count()) + "s");
        }

        if (total_output > opts.max_output) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            return std::unexpected("Output exceeded max size of " +
                std::to_string(opts.max_output) + " bytes");
        }

        struct pollfd fds[2] = {
            {stdout_pipe[0], POLLIN, 0},
            {stderr_pipe[0], POLLIN, 0}
        };

        poll(fds, 2, 50);

        if (fds[0].revents & POLLIN) {
            ssize_t n = read(stdout_pipe[0], buffer.data(), buffer.size());
            if (n > 0) { stdout_output.append(buffer.data(), n); total_output += n; }
            else stdout_done = true;
        } else if (fds[0].revents & POLLHUP) {
            stdout_done = true;
        }

        if (fds[1].revents & POLLIN) {
            ssize_t n = read(stderr_pipe[0], buffer.data(), buffer.size());
            if (n > 0) { stderr_output.append(buffer.data(), n); total_output += n; }
            else stderr_done = true;
        } else if (fds[1].revents & POLLHUP) {
            stderr_done = true;
        }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    auto end = steady_clock::now();

    return ExecResult{
        .exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1,
        .stdout_output = std::move(stdout_output),
        .stderr_output = std::move(stderr_output),
        .duration = duration_cast<milliseconds>(end - start)
    };
}

// Execute a file with streaming output callback
inline std::expected<int, std::string>
exec_file_streaming(fs::path executable, std::span<std::string> args,
                    std::function<void(std::string_view)> on_output) {
    if (!fs::exists(executable)) {
        return std::unexpected("Executable not found: " + executable.string());
    }

    int stdout_pipe[2];
    if (pipe(stdout_pipe) != 0) {
        return std::unexpected("Failed to create pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        return std::unexpected("Failed to fork");
    }

    if (pid == 0) {
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);

        std::vector<const char*> argv;
        argv.push_back(executable.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);

        execv(executable.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    close(stdout_pipe[1]);

    std::array<char, 4096> buffer{};
    while (true) {
        ssize_t n = read(stdout_pipe[0], buffer.data(), buffer.size());
        if (n <= 0) break;
        on_output(std::string_view(buffer.data(), n));
    }

    close(stdout_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace cc::utils
