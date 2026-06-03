// C++23 Bash Execution Module
// Provides utilities for executing bash commands, managing processes, history, and shell sessions
// Migrates: bashExecution.ts, bashHistory.ts, bashSignals.ts, bashCompletion.ts,
//           bashState.ts, processManager.ts, ptyManager.ts, shellSession.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <functional>
#include <chrono>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <algorithm>
#include <filesystem>
#include <deque>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#endif

export module cc.utils.bash_execution;

export namespace cc::utils::bash {

/// Exit code type alias
using ExitCode = std::int32_t;

/// Signal types that can be sent to processes
enum class SignalType {
    Int,
    Term,
    Kill,
    Hup,
    Pipe,
    Tstp,
    Cont
};

/// Current state of a process
enum class ProcessState {
    Running,
    Stopped,
    Exited,
    Signaled
};

/// Information about a running or completed process
struct ProcessInfo {
    std::uint64_t pid;
    std::string command;
    ProcessState state;
    std::optional<ExitCode> exit_code;
    std::chrono::steady_clock::time_point started_at;
    std::optional<std::chrono::steady_clock::time_point> ended_at;
};

/// A single entry in bash command history
struct BashHistoryEntry {
    std::string command;
    std::chrono::system_clock::time_point timestamp;
    std::optional<ExitCode> exit_code;
    std::chrono::milliseconds duration;
};

/// A completion suggestion item
struct CompletionItem {
    std::string text;
    std::string description;
    enum class Type {
        File,
        Directory,
        Command,
        Variable,
        Alias
    } type;
};

/// Configuration for a shell session
struct ShellSessionConfig {
    std::string shell_path{"/bin/bash"};
    std::vector<std::string> env_vars;
    std::optional<std::string> working_dir;
    bool interactive{true};
    std::chrono::seconds timeout{300};
};

/// Result of executing a command
struct ExecutionResult {
    std::string stdout_output;
    std::string stderr_output;
    ExitCode exit_code;
    std::chrono::milliseconds duration;
    bool timed_out{false};
    bool interrupted{false};
};

namespace detail {

// In-memory history store (thread-safe)
inline std::mutex& history_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline std::deque<BashHistoryEntry>& history_store() {
    static std::deque<BashHistoryEntry> store;
    return store;
}

constexpr std::size_t MAX_HISTORY_SIZE = 1000;

#ifndef _WIN32
inline int signal_type_to_posix(SignalType sig) {
    switch (sig) {
        case SignalType::Int:  return SIGINT;
        case SignalType::Term: return SIGTERM;
        case SignalType::Kill: return SIGKILL;
        case SignalType::Hup:  return SIGHUP;
        case SignalType::Pipe: return SIGPIPE;
        case SignalType::Tstp: return SIGTSTP;
        case SignalType::Cont: return SIGCONT;
    }
    return SIGTERM;
}
#endif

// Read all output from a FILE* stream
inline std::string read_stream(FILE* stream) {
    std::string output;
    std::array<char, 4096> buffer;
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), stream)) {
        output.append(buffer.data(), bytes);
    }
    return output;
}

} // namespace detail

[[nodiscard]] inline auto escape_shell_arg(std::string_view arg) -> std::string;

/// Execute a command synchronously and return the result
[[nodiscard]] inline auto execute_command(
    std::string_view command,
    ShellSessionConfig config = {}
) -> std::expected<ExecutionResult, std::string> {
    if (command.empty()) {
        return std::unexpected(std::string{"command must not be empty"});
    }

    auto start_time = std::chrono::steady_clock::now();

    // Build the full command with shell and optional working directory
    std::string full_cmd;
    if (config.working_dir && !config.working_dir->empty()) {
        full_cmd = "cd " + *config.working_dir + " && ";
    }
    full_cmd += command;

    // Set up environment variables
    for (const auto& env : config.env_vars) {
        // Each env var is "KEY=VALUE"
        #ifndef _WIN32
        putenv(const_cast<char*>(env.c_str()));
        #endif
    }

    // Execute via shell using popen for stdout capture
    // For combined stderr, redirect stderr to stdout
    std::string shell_cmd = config.shell_path + " -c " + escape_shell_arg(full_cmd) + " 2>&1";

    FILE* pipe = popen(shell_cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected(std::string{"failed to execute command: popen failed"});
    }

    std::string output = detail::read_stream(pipe);
    int status = pclose(pipe);

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    ExecutionResult result;
    result.stdout_output = std::move(output);
    result.duration = duration;

    #ifndef _WIN32
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.interrupted = true;
    } else {
        result.exit_code = -1;
    }
    #else
    result.exit_code = status;
    #endif

    // Check for timeout
    if (duration >= std::chrono::duration_cast<std::chrono::milliseconds>(config.timeout)) {
        result.timed_out = true;
    }

    return result;
}

/// Execute a command with streaming output via callback
[[nodiscard]] inline auto execute_command_streaming(
    std::string_view command,
    std::function<void(std::string_view)> on_output,
    ShellSessionConfig config = {}
) -> std::expected<ExitCode, std::string> {
    if (command.empty()) {
        return std::unexpected(std::string{"command must not be empty"});
    }

    std::string full_cmd;
    if (config.working_dir && !config.working_dir->empty()) {
        full_cmd = "cd " + *config.working_dir + " && ";
    }
    full_cmd += command;

    std::string shell_cmd = config.shell_path + " -c " + escape_shell_arg(full_cmd) + " 2>&1";

    FILE* pipe = popen(shell_cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected(std::string{"failed to execute command: popen failed"});
    }

    std::array<char, 4096> buffer;
    while (auto bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) {
        if (on_output) {
            on_output(std::string_view{buffer.data(), bytes});
        }
    }

    int status = pclose(pipe);

    #ifndef _WIN32
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return static_cast<ExitCode>(128 + WTERMSIG(status));
    }
    #endif

    return static_cast<ExitCode>(status);
}

/// Send a signal to a process by PID
[[nodiscard]] inline auto send_signal(
    std::uint64_t pid,
    SignalType signal
) -> std::expected<void, std::string> {
    #ifndef _WIN32
    int posix_sig = detail::signal_type_to_posix(signal);
    int result = kill(static_cast<pid_t>(pid), posix_sig);
    if (result != 0) {
        return std::unexpected(std::string{"failed to send signal: "} + std::strerror(errno));
    }
    return {};
    #else
    // Windows: only support termination
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        return std::unexpected(std::string{"failed to open process"});
    }
    BOOL ok = TerminateProcess(process, 1);
    CloseHandle(process);
    if (!ok) {
        return std::unexpected(std::string{"failed to terminate process"});
    }
    return {};
    #endif
}

/// Get information about a process by PID
[[nodiscard]] inline auto get_process_info(
    std::uint64_t pid
) -> std::optional<ProcessInfo> {
    #ifndef _WIN32
    // Check if process exists by sending signal 0
    int result = kill(static_cast<pid_t>(pid), 0);
    if (result != 0) {
        return std::nullopt; // Process doesn't exist or we can't access it
    }

    ProcessInfo info;
    info.pid = pid;
    info.state = ProcessState::Running;
    info.started_at = std::chrono::steady_clock::now(); // approximate

    // Try to get command name from /proc on Linux
    #ifdef __linux__
    std::string cmdline_path = "/proc/" + std::to_string(pid) + "/cmdline";
    FILE* f = fopen(cmdline_path.c_str(), "r");
    if (f) {
        std::array<char, 256> buf;
        auto bytes = fread(buf.data(), 1, buf.size() - 1, f);
        buf[bytes] = '\0';
        info.command = std::string(buf.data());
        fclose(f);
    }
    #endif

    // Check if stopped via waitpid with WNOHANG
    int status = 0;
    pid_t wait_result = waitpid(static_cast<pid_t>(pid), &status, WNOHANG | WUNTRACED);
    if (wait_result > 0) {
        if (WIFEXITED(status)) {
            info.state = ProcessState::Exited;
            info.exit_code = WEXITSTATUS(status);
            info.ended_at = std::chrono::steady_clock::now();
        } else if (WIFSIGNALED(status)) {
            info.state = ProcessState::Signaled;
            info.exit_code = 128 + WTERMSIG(status);
            info.ended_at = std::chrono::steady_clock::now();
        } else if (WIFSTOPPED(status)) {
            info.state = ProcessState::Stopped;
        }
    }

    return info;
    #else
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return std::nullopt;

    DWORD exit_code;
    ProcessInfo info;
    info.pid = pid;
    info.started_at = std::chrono::steady_clock::now();

    if (GetExitCodeProcess(process, &exit_code)) {
        if (exit_code == STILL_ACTIVE) {
            info.state = ProcessState::Running;
        } else {
            info.state = ProcessState::Exited;
            info.exit_code = static_cast<ExitCode>(exit_code);
            info.ended_at = std::chrono::steady_clock::now();
        }
    }
    CloseHandle(process);
    return info;
    #endif
}

/// Retrieve recent bash history entries
[[nodiscard]] inline auto get_bash_history(
    std::size_t limit = 100
) -> std::vector<BashHistoryEntry> {
    std::lock_guard lock(detail::history_mutex());
    auto& store = detail::history_store();

    std::size_t count = std::min(limit, store.size());
    std::vector<BashHistoryEntry> result;
    result.reserve(count);

    // Return most recent entries
    auto it = store.end();
    for (std::size_t i = 0; i < count && it != store.begin(); ++i) {
        --it;
        result.push_back(*it);
    }

    return result;
}

/// Add an entry to the bash history
inline void add_history_entry(BashHistoryEntry entry) {
    std::lock_guard lock(detail::history_mutex());
    auto& store = detail::history_store();

    store.push_back(std::move(entry));

    // Trim if exceeds max size
    while (store.size() > detail::MAX_HISTORY_SIZE) {
        store.pop_front();
    }
}

/// Get completion suggestions for a partial command
[[nodiscard]] inline auto get_completions(
    std::string_view partial,
    std::string_view cwd
) -> std::vector<CompletionItem> {
    std::vector<CompletionItem> completions;

    if (partial.empty()) return completions;

    // Determine if we're completing a path or a command
    bool is_path = partial.find('/') != std::string_view::npos ||
                   partial.starts_with("./") || partial.starts_with("~/");

    if (is_path) {
        // File/directory completion
        std::filesystem::path base_path;
        std::string prefix;

        if (partial.starts_with("~/")) {
            const char* home = std::getenv("HOME");
            if (home) {
                base_path = std::filesystem::path(home) / std::string(partial.substr(2));
            }
        } else if (partial.starts_with("/")) {
            base_path = std::string(partial);
        } else {
            base_path = std::filesystem::path(std::string(cwd)) / std::string(partial);
        }

        // Get the parent directory and the partial filename
        auto parent = base_path.parent_path();
        auto stem = base_path.filename().string();

        std::error_code ec;
        if (std::filesystem::exists(parent, ec)) {
            for (auto& entry : std::filesystem::directory_iterator(parent, ec)) {
                auto name = entry.path().filename().string();
                if (name.starts_with(stem) && !name.starts_with(".")) {
                    CompletionItem item;
                    item.text = (parent / name).string();
                    if (entry.is_directory()) {
                        item.type = CompletionItem::Type::Directory;
                        item.description = "directory";
                        item.text += "/";
                    } else {
                        item.type = CompletionItem::Type::File;
                        item.description = "file";
                    }
                    completions.push_back(std::move(item));
                    if (completions.size() >= 50) break;
                }
            }
        }
    } else {
        // Command completion — search PATH
        const char* path_env = std::getenv("PATH");
        if (!path_env) return completions;

        std::string path_str(path_env);
        std::string::size_type start = 0;
        std::string partial_str(partial);

        while (start < path_str.size()) {
            auto end = path_str.find(':', start);
            if (end == std::string::npos) end = path_str.size();

            std::string dir = path_str.substr(start, end - start);
            std::error_code ec;
            if (std::filesystem::exists(dir, ec)) {
                for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                    if (!entry.is_regular_file()) continue;
                    auto name = entry.path().filename().string();
                    if (name.starts_with(partial_str)) {
                        CompletionItem item;
                        item.text = name;
                        item.type = CompletionItem::Type::Command;
                        item.description = "command";
                        completions.push_back(std::move(item));
                        if (completions.size() >= 50) break;
                    }
                }
            }
            if (completions.size() >= 50) break;
            start = end + 1;
        }

        // Deduplicate
        std::sort(completions.begin(), completions.end(),
            [](const auto& a, const auto& b) { return a.text < b.text; });
        auto last = std::unique(completions.begin(), completions.end(),
            [](const auto& a, const auto& b) { return a.text == b.text; });
        completions.erase(last, completions.end());
    }

    return completions;
}

/// Check if a command is considered dangerous
[[nodiscard]] inline auto is_dangerous_command(
    std::string_view command
) -> bool {
    // Known dangerous patterns
    static constexpr std::array dangerous_patterns = {
        "rm -rf /",
        "rm -rf /*",
        "mkfs.",
        "dd if=/dev/zero",
        "dd if=/dev/random",
        ":(){ :|:& };:",
        "chmod -R 777 /",
        "chown -R",
        "> /dev/sda",
        "mv /* /dev/null",
        "wget.*|.*sh",
        "curl.*|.*sh",
        "format c:",
    };

    std::string cmd_lower(command);
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    for (auto pattern : dangerous_patterns) {
        if (cmd_lower.find(pattern) != std::string::npos) {
            return true;
        }
    }

    return false;
}

/// Escape a string for safe use as a shell argument
[[nodiscard]] inline auto escape_shell_arg(
    std::string_view arg
) -> std::string {
    std::string result;
    result += '\'';
    for (char c : arg) {
        if (c == '\'') {
            result += "'\\''";
        } else {
            result += c;
        }
    }
    result += '\'';
    return result;
}

/// Split a shell command string into individual arguments
[[nodiscard]] inline auto split_shell_command(
    std::string_view command
) -> std::vector<std::string> {
    std::vector<std::string> args;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    for (std::size_t i = 0; i < command.size(); ++i) {
        char c = command[i];

        if (escaped) {
            current += c;
            escaped = false;
            continue;
        }

        if (c == '\\' && !in_single_quote) {
            escaped = true;
            continue;
        }

        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }

        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }

        if ((c == ' ' || c == '\t') && !in_single_quote && !in_double_quote) {
            if (!current.empty()) {
                args.push_back(std::move(current));
                current.clear();
            }
            continue;
        }

        current += c;
    }

    if (!current.empty()) {
        args.push_back(std::move(current));
    }

    return args;
}

} // namespace cc::utils::bash
