// BashTool - Executes shell commands with process lifecycle management
module;

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

export module cc.tools.bash;

import cc.utils.error;
import cc.utils.async;
import cc.tools.tool;
import cc.tools.agent_runtime;
import cc.utils.json;
import cc.utils.shell_providers;
import cc.tools.sed_validation;
// migrated (Agent 8): result formatting + exit-code semantics
import cc.tools.command_semantics;
import cc.tools.bash_result_formatting;
// migrated (Agent 3): bash security & validation helper modules
import cc.tools.destructive_command_warning;
import cc.tools.mode_validation;
import cc.tools.path_validation;
import cc.tools.readonly_validation;
import cc.tools.should_use_sandbox;

export namespace cc::tools::bash {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::async::EventLoop;
using cc::utils::async::Task;
using cc::utils::Result;

// =========================================================================
// Bash Tool Configuration and Types
// =========================================================================

/// Dangerous command patterns that require explicit user approval
constexpr std::array<std::string_view, 10> kDangerousPatterns = {
    "rm -rf",
    "mkfs",
    "dd if=",
    "> /dev/sd",
    "chmod -R 777",
    ":(){ :|:& };:",
    "shutdown",
    "reboot",
    "init 0",
    "git push --force"
};

/// Command types for classification and UI display
enum class CommandType {
    ReadOnly,   // cat, ls, grep (read-only operations)
    Write,      // echo >, sed -i (modify files)
    Execute,    // npm, make, cargo (execute programs)
    Dangerous,  // rm -rf, format (potentially destructive)
    Unknown
};

/// Input parameters for BashTool
struct BashToolInput {
    std::string command;
    std::optional<std::string> cwd;
    std::optional<std::chrono::milliseconds> timeout;
    std::optional<std::string> description;
    std::optional<std::string> agent_id;
    bool run_in_background = false;
    bool dangerously_disable_sandbox = false;

    /// Parse from JSON using yyjson for proper escape handling
    static std::expected<BashToolInput, std::string> from_json(std::string_view json) {
        using namespace cc::utils::json;
        auto doc = parse(json);
        if (!doc) {
            return std::unexpected("Invalid JSON input");
        }

        auto root = doc->root();
        if (!root.is_obj()) {
            return std::unexpected("Expected JSON object");
        }

        BashToolInput input;

        // Extract command (required)
        auto cmd_node = root.get("command");
        if (!cmd_node.is_str()) {
            return std::unexpected("Missing 'command' field");
        }
        input.command = std::string(cmd_node.as_str());

        // Extract cwd (optional)
        auto cwd_node = root.get("cwd");
        if (cwd_node.is_str()) {
            input.cwd = std::string(cwd_node.as_str());
        }

        // Extract timeout (optional)
        auto timeout_node = root.get("timeout");
        if (timeout_node.is_num()) {
            input.timeout = std::chrono::milliseconds(static_cast<int64_t>(timeout_node.as_int()));
        }

        // Extract description (optional)
        auto desc_node = root.get("description");
        if (desc_node.is_str()) {
            input.description = std::string(desc_node.as_str());
        }

        auto agent_id_node = root.get("agent_id");
        if (!agent_id_node.is_str()) agent_id_node = root.get("agentId");
        if (agent_id_node.is_str()) {
            input.agent_id = std::string(agent_id_node.as_str());
        }

        // Extract run_in_background (optional)
        auto bg_node = root.get("run_in_background");
        if (bg_node.is_bool()) {
            input.run_in_background = bg_node.as_bool();
        }

        // Extract dangerously_disable_sandbox (optional)
        auto sandbox_node = root.get("dangerously_disable_sandbox");
        if (sandbox_node.is_bool()) {
            input.dangerously_disable_sandbox = sandbox_node.as_bool();
        }

        if (input.command.empty()) {
            return std::unexpected("Missing 'command' field");
        }

        return input;
    }
};

/// Output from BashTool execution
struct BashToolOutput {
    std::string stdout;
    std::string stderr;
    int exit_code = 0;
    bool interrupted = false;
    bool is_image = false;
    std::optional<std::string> background_task_id;
    std::optional<std::string> return_code_interpretation;
    bool no_output_expected = false;
    std::optional<std::string> persisted_output_path;
    std::optional<std::string> interrupted_reason;
    // migrated: wall-clock duration tracked for UI cards (Phase 4 FTXUI)
    std::chrono::milliseconds duration_ms{0};
};

[[nodiscard]] bool should_use_sandbox(std::string_view command) noexcept;

namespace detail {

constexpr size_t kMaxOutput = 30000;

struct BackgroundTaskStart {
    std::string id;
    pid_t pid = -1;
};

struct BackgroundTaskState {
    std::string id;
    pid_t pid = -1;
    std::string command;
    std::optional<std::string> agent_id;
    std::string output;
    bool running = true;
    bool stopped = false;
    std::optional<int> exit_code;
    std::optional<std::string> error;
    std::chrono::steady_clock::time_point created_at;
    std::thread reader_thread;
};

inline std::mutex background_tasks_mutex;
inline std::unordered_map<std::string, std::shared_ptr<BackgroundTaskState>> background_tasks;

struct BackgroundTasksCleanupGuard {
    ~BackgroundTasksCleanupGuard() {
        std::vector<std::shared_ptr<BackgroundTaskState>> all;
        {
            std::lock_guard lock(background_tasks_mutex);
            for (auto& [_, state] : background_tasks) {
                if (state->running) {
                    state->stopped = true;
                    if (state->pid > 0) kill(-state->pid, SIGTERM);
                }
                all.push_back(state);
            }
        }
        for (auto& state : all) {
            if (state->reader_thread.joinable()) state->reader_thread.join();
        }
        {
            std::lock_guard lock(background_tasks_mutex);
            background_tasks.clear();
        }
    }
};
inline BackgroundTasksCleanupGuard background_tasks_cleanup_guard;

[[nodiscard]] std::optional<pid_t> parse_pid_key(std::string_view key) {
    if (key.empty()) return std::nullopt;
    pid_t value = 0;
    for (char ch : key) {
        if (ch < '0' || ch > '9') return std::nullopt;
        value = static_cast<pid_t>((value * 10) + (ch - '0'));
    }
    return value > 0 ? std::optional<pid_t>{value} : std::nullopt;
}

struct ShellInvocation {
    std::string shell_path;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    std::optional<std::string> sandbox_tmp_dir;
    std::string cwd_file_path;
};

[[nodiscard]] std::string next_shell_invocation_id() {
    static std::atomic<std::uint64_t> counter{0};
    auto value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return std::format("{}-{}", std::chrono::steady_clock::now().time_since_epoch().count(), value);
}

[[nodiscard]] std::optional<std::string> create_sandbox_tmp_dir(std::string_view id) {
    auto path = std::filesystem::temp_directory_path() / std::format("cc-repl-shell-sandbox-{}", id);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return std::nullopt;
    return path.string();
}

[[nodiscard]] ShellInvocation build_shell_invocation(const BashToolInput& input) {
    auto provider = cc::utils::shell_providers::create_default_provider();
    auto id = next_shell_invocation_id();
    const auto use_sandbox = should_use_sandbox(input.command) && !input.dangerously_disable_sandbox;
    auto sandbox_tmp = use_sandbox ? create_sandbox_tmp_dir(id) : std::optional<std::string>{};
    cc::utils::shell_providers::BuildExecOptions opts{
        .id = id,
        .sandbox_tmp_dir = sandbox_tmp,
        .use_sandbox = use_sandbox && sandbox_tmp.has_value(),
    };
    auto exec = provider->build_exec_command(input.command, opts);
    return ShellInvocation{
        .shell_path = provider->shell_path(),
        .args = provider->get_spawn_args(exec.command_string),
        .env = provider->get_environment_overrides(input.command),
        .sandbox_tmp_dir = std::move(sandbox_tmp),
        .cwd_file_path = std::move(exec.cwd_file_path),
    };
}

[[nodiscard]] std::vector<char*> make_exec_argv(
    std::string& executable,
    std::vector<std::string>& args
) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(executable.data());
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    return argv;
}

void apply_env_overrides(const std::map<std::string, std::string>& env) {
    for (const auto& [key, value] : env) {
        setenv(key.c_str(), value.c_str(), 1);
    }
}

[[nodiscard]] std::string truncate_output(std::string data) {
    if (data.size() <= kMaxOutput) return data;

    constexpr size_t kHalf = kMaxOutput / 2;
    const auto omitted = data.size() - kMaxOutput;
    return data.substr(0, kHalf)
        + "\n\n... [output truncated, "
        + std::to_string(omitted)
        + " bytes omitted] ...\n\n"
        + data.substr(data.size() - kHalf);
}

[[nodiscard]] int decode_exit_status(int status) noexcept {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

void close_if_open(int& fd) noexcept {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

[[nodiscard]] bool set_nonblocking(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

[[nodiscard]] bool drain_fd(int fd, std::string& output) {
    std::array<char, 4096> buffer{};
    while (true) {
        const auto nread = read(fd, buffer.data(), buffer.size());
        if (nread > 0) {
            output.append(buffer.data(), static_cast<size_t>(nread));
            continue;
        }
        if (nread == 0) return false;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }
}

[[nodiscard]] bool agent_cancel_requested(const BashToolInput& input) {
    return input.agent_id &&
        cc::tools::agent_runtime::native_agent_store().is_cancel_requested(*input.agent_id);
}

inline void append_background_output(BackgroundTaskState& state, std::string_view data) {
    state.output.append(data);
    if (state.output.size() > kMaxOutput) {
        state.output.erase(0, state.output.size() - kMaxOutput);
    }
}

[[nodiscard]] std::expected<BackgroundTaskStart, std::string> spawn_background_shell(
    const BashToolInput& input) {
    auto invocation = build_shell_invocation(input);
    int output_pipe[2]{-1, -1};
    if (pipe(output_pipe) != 0) {
        return std::unexpected(std::format("Failed to create output pipe: {}", std::strerror(errno)));
    }

    const pid_t worker = fork();
    if (worker < 0) {
        close_if_open(output_pipe[0]);
        close_if_open(output_pipe[1]);
        return std::unexpected(std::format("Failed to fork background process: {}", std::strerror(errno)));
    }

    if (worker == 0) {
        close_if_open(output_pipe[0]);
        setpgid(0, 0);

        if (input.cwd && !input.cwd->empty() && chdir(input.cwd->c_str()) != 0) {
            const auto message = std::format("cd: {}: {}\n", *input.cwd, std::strerror(errno));
            const auto bytes = write(output_pipe[1], message.data(), message.size());
            (void)bytes;
            _exit(126);
        }

        const int dev_null = open("/dev/null", O_RDONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDIN_FILENO);
            if (dev_null > STDIN_FILENO) close(dev_null);
        }
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close_if_open(output_pipe[1]);

        apply_env_overrides(invocation.env);
        auto argv = make_exec_argv(invocation.shell_path, invocation.args);
        execvp(invocation.shell_path.c_str(), argv.data());
        _exit(127);
    }

    close_if_open(output_pipe[1]);
    const auto task_id = std::format("task_{}", worker);
    auto state = std::make_shared<BackgroundTaskState>(BackgroundTaskState{
        .id = task_id,
        .pid = worker,
        .command = input.command,
        .agent_id = input.agent_id,
        .created_at = std::chrono::steady_clock::now(),
    });
    {
        std::lock_guard lock(background_tasks_mutex);
        background_tasks[task_id] = state;
    }

    state->reader_thread = std::thread([state, read_fd = output_pipe[0]]() mutable {
        std::array<char, 4096> buffer{};
        while (true) {
            const auto nread = read(read_fd, buffer.data(), buffer.size());
            if (nread > 0) {
                std::lock_guard lock(background_tasks_mutex);
                append_background_output(*state, std::string_view(buffer.data(), static_cast<std::size_t>(nread)));
                continue;
            }
            if (nread == 0) break;
            if (errno == EINTR) continue;
            break;
        }
        close_if_open(read_fd);

        int status = 0;
        const auto waited = waitpid(state->pid, &status, 0);
        std::lock_guard lock(background_tasks_mutex);
        state->running = false;
        if (waited < 0) {
            state->error = std::format("waitpid failed: {}", std::strerror(errno));
            return;
        }
        if (WIFEXITED(status)) {
            state->exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            state->exit_code = 128 + WTERMSIG(status);
            if (!state->stopped) {
                state->error = std::format("terminated by signal {}", WTERMSIG(status));
            }
        }
    });

    return BackgroundTaskStart{.id = task_id, .pid = worker};
}

[[nodiscard]] std::expected<BashToolOutput, std::string> execute_shell(
    const BashToolInput& input) {
    auto invocation = build_shell_invocation(input);
    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    if (pipe(stdout_pipe) != 0) {
        return std::unexpected(std::format("Failed to create stdout pipe: {}", std::strerror(errno)));
    }
    if (pipe(stderr_pipe) != 0) {
        close_if_open(stdout_pipe[0]);
        close_if_open(stdout_pipe[1]);
        return std::unexpected(std::format("Failed to create stderr pipe: {}", std::strerror(errno)));
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close_if_open(stdout_pipe[0]);
        close_if_open(stdout_pipe[1]);
        close_if_open(stderr_pipe[0]);
        close_if_open(stderr_pipe[1]);
        return std::unexpected(std::format("Failed to fork command: {}", std::strerror(errno)));
    }

    if (pid == 0) {
        close_if_open(stdout_pipe[0]);
        close_if_open(stderr_pipe[0]);
        setpgid(0, 0);

        if (input.cwd && !input.cwd->empty() && chdir(input.cwd->c_str()) != 0) {
            const auto message = std::format("cd: {}: {}\n", *input.cwd, std::strerror(errno));
            const auto bytes = write(stderr_pipe[1], message.data(), message.size());
            (void)bytes;
            _exit(126);
        }

        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close_if_open(stdout_pipe[1]);
        close_if_open(stderr_pipe[1]);

        apply_env_overrides(invocation.env);
        auto argv = make_exec_argv(invocation.shell_path, invocation.args);
        execvp(invocation.shell_path.c_str(), argv.data());
        _exit(127);
    }

    close_if_open(stdout_pipe[1]);
    close_if_open(stderr_pipe[1]);
    (void)set_nonblocking(stdout_pipe[0]);
    (void)set_nonblocking(stderr_pipe[0]);

    BashToolOutput output{
        .exit_code = -1
    };

    // migrated: record duration for Phase-4 UI cards
    const auto exec_started_at = std::chrono::steady_clock::now();

    bool stdout_open = true;
    bool stderr_open = true;
    bool process_exited = false;
    int status = 0;
    const auto timeout = input.timeout.value_or(std::chrono::seconds(120));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool cancel_requested = false;
    bool cancel_kill_sent = false;
    std::optional<std::chrono::steady_clock::time_point> cancel_kill_deadline;

    while (stdout_open || stderr_open || !process_exited) {
        if (!process_exited) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                process_exited = true;
                output.exit_code = decode_exit_status(status);
            } else if (waited < 0 && errno == ECHILD) {
                process_exited = true;
                output.exit_code = output.exit_code < 0 ? 0 : output.exit_code;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (!process_exited && !cancel_requested && agent_cancel_requested(input)) {
            cancel_requested = true;
            output.interrupted = true;
            output.interrupted_reason = "Command cancelled";
            if (kill(-pid, SIGTERM) != 0) {
                kill(pid, SIGTERM);
            }
            cancel_kill_deadline = now + std::chrono::milliseconds(250);
        }
        if (!process_exited &&
            cancel_requested &&
            !cancel_kill_sent &&
            cancel_kill_deadline &&
            now >= *cancel_kill_deadline) {
            cancel_kill_sent = true;
            if (kill(-pid, SIGKILL) != 0) {
                kill(pid, SIGKILL);
            }
        }
        if (!process_exited && !output.interrupted && now >= deadline) {
            output.interrupted = true;
            output.interrupted_reason = "Command timed out";
            if (kill(-pid, SIGKILL) != 0) {
                kill(pid, SIGKILL);
            }
        }

        std::array<pollfd, 2> fds{};
        nfds_t nfds = 0;
        if (stdout_open) {
            fds[nfds++] = pollfd{.fd = stdout_pipe[0], .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
        }
        if (stderr_open) {
            fds[nfds++] = pollfd{.fd = stderr_pipe[0], .events = POLLIN | POLLHUP | POLLERR, .revents = 0};
        }

        int poll_timeout_ms = 25;
        if (!output.interrupted && !process_exited) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            poll_timeout_ms = static_cast<int>(std::clamp<long long>(remaining, 0, 25));
        }

        const int ready = nfds == 0 ? poll(nullptr, 0, poll_timeout_ms) : poll(fds.data(), nfds, poll_timeout_ms);
        if (ready > 0) {
            nfds_t index = 0;
            if (stdout_open) {
                const auto events = fds[index++].revents;
                if (events != 0) {
                    stdout_open = drain_fd(stdout_pipe[0], output.stdout);
                    if (!stdout_open) close_if_open(stdout_pipe[0]);
                }
            }
            if (stderr_open) {
                const auto events = fds[index].revents;
                if (events != 0) {
                    stderr_open = drain_fd(stderr_pipe[0], output.stderr);
                    if (!stderr_open) close_if_open(stderr_pipe[0]);
                }
            }
        }
    }

    close_if_open(stdout_pipe[0]);
    close_if_open(stderr_pipe[0]);

    output.stdout = truncate_output(std::move(output.stdout));
    output.stderr = truncate_output(std::move(output.stderr));
    if (output.interrupted) {
        output.exit_code = 128 + SIGKILL;
    }

    // migrated: record wall-clock duration for UI cards (Phase 4 FTXUI)
    output.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - exec_started_at);

    return output;
}

} // namespace detail

struct BackgroundTaskSnapshot {
    std::string id;
    pid_t pid = -1;
    std::string command;
    std::optional<std::string> agent_id;
    std::string output;
    bool running = false;
    bool stopped = false;
    std::optional<int> exit_code;
    std::optional<std::string> error;
};

[[nodiscard]] BackgroundTaskSnapshot snapshot_from_state(const detail::BackgroundTaskState& state) {
    return BackgroundTaskSnapshot{
        .id = state.id,
        .pid = state.pid,
        .command = state.command,
        .agent_id = state.agent_id,
        .output = state.output,
        .running = state.running,
        .stopped = state.stopped,
        .exit_code = state.exit_code,
        .error = state.error,
    };
}

[[nodiscard]] std::optional<BackgroundTaskSnapshot> get_background_task_snapshot(std::string_view id) {
    std::lock_guard lock(detail::background_tasks_mutex);
    const auto it = detail::background_tasks.find(std::string(id));
    if (it != detail::background_tasks.end()) {
        return snapshot_from_state(*it->second);
    }

    auto pid = detail::parse_pid_key(id);
    if (!pid) {
        return std::nullopt;
    }
    for (const auto& [_, state] : detail::background_tasks) {
        if (state->pid == *pid) {
            return snapshot_from_state(*state);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool stop_background_task(std::string_view id) {
    pid_t pid = -1;
    {
        std::lock_guard lock(detail::background_tasks_mutex);
        auto it = detail::background_tasks.find(std::string(id));
        if (it == detail::background_tasks.end()) {
            if (auto pid_key = detail::parse_pid_key(id)) {
                for (auto candidate = detail::background_tasks.begin();
                     candidate != detail::background_tasks.end();
                     ++candidate) {
                    if (candidate->second->pid == *pid_key) {
                        it = candidate;
                        break;
                    }
                }
            }
        }
        if (it == detail::background_tasks.end()) {
            return false;
        }
        auto& state = *it->second;
        state.stopped = true;
        pid = state.pid;
        if (!state.running) {
            return true;
        }
    }

    if (pid <= 0) {
        return false;
    }
    if (kill(-pid, SIGTERM) == 0) {
        return true;
    }
    return kill(pid, SIGTERM) == 0 || errno == ESRCH;
}

[[nodiscard]] std::vector<BackgroundTaskSnapshot> stop_background_tasks_for_agent(std::string_view agent_id) {
    if (agent_id.empty()) return {};

    std::vector<std::string> task_ids;
    {
        std::lock_guard lock(detail::background_tasks_mutex);
        for (const auto& [id, state] : detail::background_tasks) {
            if (state->agent_id && *state->agent_id == agent_id && state->running) {
                task_ids.push_back(id);
            }
        }
    }

    std::vector<BackgroundTaskSnapshot> stopped;
    stopped.reserve(task_ids.size());
    for (const auto& task_id : task_ids) {
        if (stop_background_task(task_id)) {
            if (auto snapshot = get_background_task_snapshot(task_id)) {
                stopped.push_back(std::move(*snapshot));
            }
        }
    }
    return stopped;
}

inline void drain_all_background_tasks() {
    std::vector<std::shared_ptr<detail::BackgroundTaskState>> tasks;
    {
        std::lock_guard lock(detail::background_tasks_mutex);
        for (auto& [_, state] : detail::background_tasks) {
            if (state->running) {
                state->stopped = true;
                if (state->pid > 0) {
                    kill(-state->pid, SIGTERM);
                }
            }
            tasks.push_back(state);
        }
    }
    for (auto& state : tasks) {
        if (state->reader_thread.joinable()) {
            state->reader_thread.join();
        }
    }
    {
        std::lock_guard lock(detail::background_tasks_mutex);
        detail::background_tasks.clear();
    }
}

// =========================================================================
// Command Classification and Validation
// =========================================================================

/// Classify a command by type.
///
/// Dangerous-pattern detection is now delegated to
/// cc.tools.destructive_command_warning which has a richer regex-based
/// table covering git, file-deletion, database, and infrastructure ops.
[[nodiscard]] CommandType classify_command(std::string_view command) noexcept {
    std::string_view base_cmd = command;
    auto space = command.find(' ');
    if (space != std::string_view::npos) {
        base_cmd = command.substr(0, space);
    }

    // (1) Read-only commands
    static const std::unordered_set<std::string_view> kReadCmds = {
        "cat", "head", "tail", "less", "more", "wc", "stat", "file", "strings",
        "ls", "tree", "du", "find", "grep", "rg", "ag", "ack", "locate",
        "which", "whereis"
    };
    if (kReadCmds.contains(base_cmd)) {
        return CommandType::ReadOnly;
    }

    // (2) Write commands
    static const std::unordered_set<std::string_view> kWriteCmds = {
        "echo", "tee", "sed", "awk", "touch", "mkdir", "rmdir", "cp", "mv", "ln"
    };
    if (kWriteCmds.contains(base_cmd)) {
        if (cc::tools::bash_validation::is_destructive_command(command)) {
            return CommandType::Dangerous;
        }
        return CommandType::Write;
    }

    // (3) Check for dangerous patterns (uses regex table from Agent 3 module)
    if (cc::tools::bash_validation::is_destructive_command(command)) {
        return CommandType::Dangerous;
    }

    // (4) Fallback: allowlist-based read-only detection
    if (readonly_validation::is_command_safe_via_flag_parsing(command)) {
        return CommandType::ReadOnly;
    }

    return CommandType::Execute;
}

/// Check if command requires sandbox (delegates to Agent 3's
/// should_use_sandbox module with sensible defaults).
[[nodiscard]] bool should_use_sandbox(std::string_view command) noexcept {
    sandbox::SandboxInput input{
        .command = std::string(command),
        .dangerously_disable_sandbox = false,
    };
    sandbox::SandboxRuntimeConfig runtime;
    return sandbox::should_use_sandbox(input, runtime);
}


/// Check if command is silent (expected no output)
[[nodiscard]] bool is_silent_command(std::string_view command) noexcept {
    static const std::unordered_set<std::string_view> kSilentCmds = {
        "mv", "cp", "rm", "mkdir", "rmdir", "chmod", "chown", "chgrp",
        "touch", "ln", "cd", "export", "unset", "wait"
    };
    
    auto space = command.find(' ');
    auto base_cmd = space != std::string::npos ? command.substr(0, space) : command;
    return kSilentCmds.contains(base_cmd);
}

// =========================================================================
// BashTool Implementation
// =========================================================================

/// BashTool - Executes shell commands with safety checks
class BashTool {
public:
    static constexpr std::string_view kName = "Bash";
    static constexpr std::string_view kDescription = 
        "Execute shell commands in a terminal session. Use for running CLI tools, "
        "installing dependencies, or executing build commands.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "command",
                        .type = "string",
                        .description = "The shell command to execute",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "cwd",
                        .type = "string",
                        .description = "Working directory for execution (optional)",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "description",
                        .type = "string",
                        .description = "Clear description of what this command does",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "timeout",
                        .type = "number",
                        .description = "Timeout in milliseconds (default: 120000)",
                        .required = false
                    },
                    SchemaProperty{
                        .name = "run_in_background",
                        .type = "boolean",
                        .description = "Run command in background (optional)",
                        .required = false
                    }
                }
            },
            .permission = ToolPermission::Execute,
            .category = "execution",
            .max_result_size_chars = 30'000
        };
    }
    
    explicit BashTool(EventLoop& loop = EventLoop::default_loop()) {
        (void)loop;
    }
    
    /// Permission mode for the tool
    enum class PermissionMode {
        Ask,        // Always ask user for confirmation
        AutoAllow,  // Auto-allow safe commands, ask for dangerous
        YoloMode    // Allow everything without asking
    };
    
    void set_permission_mode(PermissionMode mode) { permission_mode_ = mode; }
    void set_allowed_directories(std::vector<std::string> dirs) { 
        allowed_directories_ = std::move(dirs); 
    }
    
    /// Check if execution is allowed based on permission mode and command type
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        auto parsed = BashToolInput::from_json(input.json());
        if (!parsed) return false;

        // TODO(migration): integrate — run sed safety allowlist / denylist for
        // any command whose base token is "sed".  On Ask we force ask-mode
        // semantics regardless of outer permission, otherwise fall through
        // to the existing command-type classification.
        {
            std::string_view cmd = parsed->command;
            while (!cmd.empty() && std::isspace(static_cast<unsigned char>(cmd.front()))) {
                cmd.remove_prefix(1);
            }
            if (cmd.starts_with("sed") &&
                (cmd.size() == 3 || std::isspace(static_cast<unsigned char>(cmd[3])))) {
                const bool allow_file_writes =
                    (permission_mode_ == PermissionMode::AutoAllow ||
                     permission_mode_ == PermissionMode::YoloMode);
                auto decision = cc::tools::sed_validation::check_sed_constraints(
                    parsed->command, allow_file_writes);
                if (decision.decision ==
                    cc::tools::sed_validation::SedSafetyDecision::Ask) {
                    return permission_mode_ == PermissionMode::YoloMode;
                }
            }
        }

        auto cmd_type = classify_command(parsed->command);
        
        switch (permission_mode_) {
            case PermissionMode::YoloMode:
                return true;
            case PermissionMode::AutoAllow:
                // Auto-allow read-only and simple write commands
                // Block dangerous commands (require external approval)
                return cmd_type != CommandType::Dangerous;
            case PermissionMode::Ask:
            default:
                // In ask mode, only auto-allow read-only
                return cmd_type == CommandType::ReadOnly;
        }
    }
    
    /// Check if command is dangerous and needs explicit confirmation
    [[nodiscard]] bool requires_confirmation(std::string_view command) const {
        auto cmd_type = classify_command(command);
        if (permission_mode_ == PermissionMode::YoloMode) return false;
        return cmd_type == CommandType::Dangerous;
    }
    
    /// Validate working directory is within allowed paths
    [[nodiscard]] bool is_directory_allowed(std::string_view cwd) const {
        if (allowed_directories_.empty()) return true;  // No restriction
        for (const auto& dir : allowed_directories_) {
            if (cwd.starts_with(dir)) return true;
        }
        return false;
    }
    
    /// Execute a command synchronously
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed_input = BashToolInput::from_json(input.json());
        if (!parsed_input) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                parsed_input.error()
            ));
        }
        
        return execute_internal(*parsed_input);
    }
    
    /// Execute command asynchronously (coroutine)
    [[nodiscard]] Task<Result<ToolResult>> execute_async(const ToolInput& input) {
        auto parsed_input = BashToolInput::from_json(input.json());
        if (!parsed_input) {
            co_return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument,
                parsed_input.error()
            ));
        }
        
        co_return co_await execute_async_internal(*parsed_input);
    }
    
private:
    PermissionMode permission_mode_ = PermissionMode::AutoAllow;
    std::vector<std::string> allowed_directories_;
    
    /// Internal synchronous execution
    Result<ToolResult> execute_internal(const BashToolInput& input) {
        try {
            // Check directory permissions
            if (input.cwd && !is_directory_allowed(*input.cwd)) {
                return ToolResult::error(std::format(
                    "Directory '{}' is not in the allowed directories list", *input.cwd));
            }
            
            // Check if command requires confirmation
            if (requires_confirmation(input.command)) {
                return ToolResult::error(std::format(
                    "Command requires user confirmation: {}", input.command));
            }

            if (input.run_in_background) {
                auto background_task = detail::spawn_background_shell(input);
                if (!background_task) {
                    return ToolResult::error(background_task.error());
                }

                BashToolOutput output{
                    .stdout = std::format(
                        "Background task started\nTask ID: {}\nPID: {}",
                        background_task->id,
                        background_task->pid),
                    .exit_code = 0,
                    .background_task_id = background_task->id,
                    .no_output_expected = false
                };
                return format_result(output, input.command, /*semantic_is_error=*/false);
            }

            auto executed = detail::execute_shell(input);
            if (!executed) {
                return ToolResult::error(executed.error());
            }

            auto output = std::move(*executed);
            output.no_output_expected = is_silent_command(input.command);

            // migrated: interpret exit codes via command_semantics rules.
            // Many tools (grep, find, diff, test, ...) use exit 1 to signal
            // "no match / different / false" rather than a genuine failure.
            const auto interpreted = cc::tools::interpret_command_result(
                input.command, output.exit_code, output.stdout, output.stderr);
            if (interpreted.message) {
                output.return_code_interpretation = *std::move(interpreted.message);
            }

            return format_result(output, input.command, interpreted.is_error);
            
        } catch (const std::exception& e) {
            return ToolResult::error(std::format("Execution error: {}", e.what()));
        }
    }
    
    /// Internal async execution using libuv process
    Task<Result<ToolResult>> execute_async_internal(const BashToolInput& input) {
        co_return execute_internal(input);
    }
    
    /// Format the BashToolOutput into a ToolResult.
    ///
    /// Also constructs a `BashResultInfo` struct (via bash_result_formatting)
    /// containing all the data the Phase 4 FTXUI layer will need to render a
    /// proper result card.  Today that struct is materialised and then left
    /// unused (it is intended as a bridge to a future UI render path); all
    /// textual output still goes through the legacy flat-text layout below.
    ///
    /// \param command            original user command (for display)
    /// \param semantic_is_error  true if command_semantics declared the
    ///                           execution a *real* error (as opposed to e.g.
    ///                           grep returning exit 1 for no matches).
    [[nodiscard]] ToolResult format_result(
        const BashToolOutput& output,
        std::string_view command,
        bool semantic_is_error
    ) {
        // ---- Phase 4 bridge: build structured BashResultInfo --------------
        // (UI rendering is deferred to FTXUI; only the data bridge lives here.)
        const auto dur_ms = static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, std::chrono::duration_cast<
                std::chrono::milliseconds>(output.duration_ms).count()));
        [[maybe_unused]] const auto info = cc::tools::bash::make_result_info(
            std::string(command),
            output.exit_code,
            dur_ms,
            output.stdout,
            output.stderr,
            output.interrupted,
            output.is_image,
            output.no_output_expected,
            output.background_task_id.has_value(),
            output.return_code_interpretation,
            output.interrupted_reason
        );
        // TODO(Phase 4): hand `info` to FTXUI renderer instead of flattening.
        // -------------------------------------------------------------------

        std::string result_text;

        const bool hard_error =
            output.interrupted || (output.exit_code != 0 && semantic_is_error);

        if (hard_error) {
            result_text = output.interrupted
                ? output.interrupted_reason.value_or("Command timed out")
                : "Command failed";
            if (!output.stderr.empty()) {
                result_text += ":\n" + output.stderr;
            }
            if (!output.stdout.empty()) {
                result_text += "\nOutput:\n" + output.stdout;
            }
            result_text += std::format("\nExit code: {}", output.exit_code);
            return ToolResult::error(result_text);
        }

        if (!output.stdout.empty()) {
            result_text = output.stdout;
        } else if (!output.stderr.empty()) {
            result_text = output.stderr;
        } else if (output.no_output_expected) {
            result_text = "Command completed successfully";
        } else {
            result_text = "(no output)";
        }

        if (output.return_code_interpretation) {
            result_text += "\n\n" + *output.return_code_interpretation;
        }

        return ToolResult::success(result_text);
    }
    
    /// Check if output represents an error
    [[nodiscard]] bool is_error(const BashToolOutput& output) const noexcept {
        return output.exit_code != 0 || output.interrupted;
    }
};

} // namespace cc::tools::bash

// Export main tool class
export namespace cc::tools {
    using cc::tools::bash::BashTool;

    /// Factory: create BashTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_bash_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            BashTool tool_;
            cc::core::ToolDefinition def_ = BashTool::definition();

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>();
    }
}
