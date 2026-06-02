module;

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <expected>
#include <chrono>
#include <future>
#include <filesystem>
#include <array>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>

export module cc.utils.shell;

export namespace cc::utils {

namespace fs = std::filesystem;
using namespace std::chrono;

// Shell execution options
struct ShellOptions {
    std::optional<fs::path> cwd;
    std::map<std::string, std::string> env;
    std::optional<seconds> timeout;
    bool capture_stderr = true;
};

// Result of a shell command execution
struct ShellResult {
    int exit_code;
    std::string stdout_output;
    std::string stderr_output;
    milliseconds duration;
};

// Execute a shell command synchronously with full output capture
inline std::expected<ShellResult, std::string>
run_shell(std::string_view command, ShellOptions opts = {}) {
    auto start = steady_clock::now();

    // Create pipes for stdout and stderr
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        return std::unexpected("Failed to create pipes");
    }

    pid_t pid = fork();
    if (pid < 0) {
        return std::unexpected("Failed to fork process");
    }

    if (pid == 0) {
        // Child process
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(opts.capture_stderr ? stderr_pipe[1] : STDOUT_FILENO, STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Change working directory if specified
        if (opts.cwd) {
            if (chdir(opts.cwd->c_str()) != 0) {
                _exit(127);
            }
        }

        // Set environment variables
        for (const auto& [key, value] : opts.env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::string cmd_str(command);
        execl("/bin/sh", "sh", "-c", cmd_str.c_str(), nullptr);
        _exit(127);
    }

    // Parent process
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Set non-blocking mode for reading
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    std::string stdout_output, stderr_output;
    std::array<char, 4096> buffer{};
    bool stdout_done = false, stderr_done = false;

    auto timeout_point = opts.timeout
        ? std::optional(steady_clock::now() + *opts.timeout)
        : std::nullopt;

    while (!stdout_done || !stderr_done) {
        // Check timeout
        if (timeout_point && steady_clock::now() > *timeout_point) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            return std::unexpected("Command timed out");
        }

        struct pollfd fds[2] = {
            {stdout_pipe[0], POLLIN, 0},
            {stderr_pipe[0], POLLIN, 0}
        };

        int ready = poll(fds, 2, 100); // 100ms poll timeout
        if (ready < 0) break;

        if (fds[0].revents & POLLIN) {
            ssize_t n = read(stdout_pipe[0], buffer.data(), buffer.size());
            if (n > 0) stdout_output.append(buffer.data(), n);
            else stdout_done = true;
        } else if (fds[0].revents & POLLHUP) {
            stdout_done = true;
        }

        if (fds[1].revents & POLLIN) {
            ssize_t n = read(stderr_pipe[0], buffer.data(), buffer.size());
            if (n > 0) stderr_output.append(buffer.data(), n);
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
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    return ShellResult{
        .exit_code = exit_code,
        .stdout_output = std::move(stdout_output),
        .stderr_output = std::move(stderr_output),
        .duration = duration_cast<milliseconds>(end - start)
    };
}

// Execute a shell command asynchronously
inline std::future<ShellResult>
run_shell_async(std::string_view command, ShellOptions opts = {}) {
    std::string cmd(command);
    return std::async(std::launch::async, [cmd, opts = std::move(opts)]() -> ShellResult {
        auto result = run_shell(cmd, opts);
        if (result) return *result;
        return ShellResult{.exit_code = -1, .stderr_output = result.error(), .duration = milliseconds(0)};
    });
}

// Long-running shell process with stdin/stdout streaming
class ShellProcess {
public:
    ShellProcess() = default;
    ~ShellProcess() { if (pid_ > 0) kill(); }

    // Non-copyable, movable
    ShellProcess(const ShellProcess&) = delete;
    ShellProcess& operator=(const ShellProcess&) = delete;
    ShellProcess(ShellProcess&& other) noexcept
        : pid_(other.pid_), stdin_fd_(other.stdin_fd_),
          stdout_fd_(other.stdout_fd_), running_(other.running_) {
        other.pid_ = -1;
        other.stdin_fd_ = -1;
        other.stdout_fd_ = -1;
        other.running_ = false;
    }

    // Start a shell process with the given command
    bool start(std::string_view command, ShellOptions opts = {}) {
        int stdin_pipe[2], stdout_pipe[2];
        if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) return false;

        pid_ = fork();
        if (pid_ < 0) return false;

        if (pid_ == 0) {
            close(stdin_pipe[1]);
            close(stdout_pipe[0]);
            dup2(stdin_pipe[0], STDIN_FILENO);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stdout_pipe[1], STDERR_FILENO);
            close(stdin_pipe[0]);
            close(stdout_pipe[1]);

            if (opts.cwd) chdir(opts.cwd->c_str());
            for (const auto& [k, v] : opts.env) setenv(k.c_str(), v.c_str(), 1);

            std::string cmd(command);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        fcntl(stdout_fd_, F_SETFL, O_NONBLOCK);
        running_ = true;
        return true;
    }

    // Write data to stdin of the process
    bool write(std::string_view data) {
        if (!running_ || stdin_fd_ < 0) return false;
        ssize_t n = ::write(stdin_fd_, data.data(), data.size());
        return n == static_cast<ssize_t>(data.size());
    }

    // Read a line from stdout (non-blocking)
    std::optional<std::string> read_line() {
        if (!running_ || stdout_fd_ < 0) return std::nullopt;

        char ch;
        std::string line;
        while (true) {
            ssize_t n = ::read(stdout_fd_, &ch, 1);
            if (n <= 0) {
                if (line.empty()) return std::nullopt;
                break;
            }
            if (ch == '\n') break;
            line += ch;
        }
        return line.empty() ? std::nullopt : std::optional(std::move(line));
    }

    // Kill the process
    void kill() {
        if (pid_ > 0 && running_) {
            ::kill(pid_, SIGKILL);
            running_ = false;
        }
        cleanup_fds();
    }

    // Wait for the process to exit and return exit code
    int wait() {
        if (pid_ <= 0) return -1;
        int status = 0;
        waitpid(pid_, &status, 0);
        running_ = false;
        cleanup_fds();
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    bool is_running() const { return running_; }

private:
    void cleanup_fds() {
        if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
        if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    }

    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    bool running_ = false;
};

} // namespace cc::utils
