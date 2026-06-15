// C++23 Shell Providers Module
// Merges: bashProvider.ts, powershellProvider.ts, powershellDetection.ts,
//         shellProvider.ts, resolveDefaultShell.ts
module;

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.shell_providers;

import cc.utils.shell;
import cc.utils.bash_execution;

export namespace cc::utils::shell_providers {

namespace fs = std::filesystem;

// ============================================================================
// Types & Enums
// ============================================================================

/// Supported shell types
enum class ShellType {
    Bash,
    PowerShell,
};

/// Convert ShellType to string
[[nodiscard]] inline std::string_view shell_type_name(ShellType type) {
    switch (type) {
        case ShellType::Bash: return "bash";
        case ShellType::PowerShell: return "powershell";
    }
    return "unknown";
}

/// Default hook shell type
inline constexpr ShellType DEFAULT_HOOK_SHELL = ShellType::Bash;

/// Options for building an exec command
struct BuildExecOptions {
    std::string id;
    std::optional<std::string> sandbox_tmp_dir;
    bool use_sandbox = false;
};

/// Result of building an exec command
struct ExecCommandResult {
    std::string command_string;
    std::string cwd_file_path;
};

// ============================================================================
// ShellProvider — Abstract interface for shell execution
// ============================================================================

/// Abstract interface for shell providers.
/// Each provider knows how to build commands, spawn args, and env overrides
/// for its specific shell type.
class ShellProvider {
public:
    virtual ~ShellProvider() = default;

    /// The shell type this provider handles
    [[nodiscard]] virtual ShellType type() const = 0;

    /// The path to the shell executable
    [[nodiscard]] virtual const std::string& shell_path() const = 0;

    /// Whether to spawn the shell in detached mode
    [[nodiscard]] virtual bool detached() const = 0;

    /// Build the full command string including all shell-specific setup.
    /// For bash: source snapshot, session env, disable extglob, eval-wrap, pwd tracking.
    [[nodiscard]] virtual ExecCommandResult build_exec_command(
        std::string_view command, const BuildExecOptions& opts) = 0;

    /// Shell args for spawn (e.g., ["-c", "-l", cmd] for bash)
    [[nodiscard]] virtual std::vector<std::string> get_spawn_args(
        std::string_view command_string) const = 0;

    /// Extra env vars for this shell type.
    /// May perform initialization (e.g., tmux socket setup for bash).
    [[nodiscard]] virtual std::map<std::string, std::string>
    get_environment_overrides(std::string_view command) = 0;
};

// ============================================================================
// BashProvider — Bash shell provider implementation
// ============================================================================

/// Bash shell provider that handles snapshot sourcing, extglob disabling,
/// eval wrapping, and cwd tracking.
class BashProvider : public ShellProvider {
public:
    struct Options {
        bool skip_snapshot = false;
    };

    explicit BashProvider(std::string shell_path)
        : BashProvider(std::move(shell_path), Options{}) {}

    explicit BashProvider(std::string shell_path, Options opts)
        : shell_path_(std::move(shell_path))
        , skip_snapshot_(opts.skip_snapshot) {
        if (!skip_snapshot_) {
            snapshot_path_ = create_shell_snapshot(shell_path_);
        }
    }

    [[nodiscard]] ShellType type() const override { return ShellType::Bash; }
    [[nodiscard]] const std::string& shell_path() const override { return shell_path_; }
    [[nodiscard]] bool detached() const override { return true; }

    [[nodiscard]] ExecCommandResult build_exec_command(
        std::string_view command, const BuildExecOptions& opts) override {

        std::string tmp_dir = get_tmp_dir();
        std::string cwd_file_path;
        if (opts.use_sandbox && opts.sandbox_tmp_dir) {
            cwd_file_path = *opts.sandbox_tmp_dir + "/cwd-" + opts.id;
        } else {
            cwd_file_path = tmp_dir + "/claude-" + opts.id + "-cwd";
        }

        std::vector<std::string> parts;

        if (opts.use_sandbox && opts.sandbox_tmp_dir) {
            parts.push_back("export TMPDIR=" + shell_quote(*opts.sandbox_tmp_dir));
        }

        // Source snapshot (if available)
        if (snapshot_path_ && fs::exists(*snapshot_path_)) {
            parts.push_back("source " + shell_quote(*snapshot_path_) +
                           " 2>/dev/null || true");
            last_snapshot_valid_ = true;
        } else {
            last_snapshot_valid_ = false;
        }

        // Disable extended glob for security
        auto extglob_cmd = get_disable_extglob_command();
        if (extglob_cmd) {
            parts.push_back(*extglob_cmd);
        }

        // Eval-wrap the command
        std::string quoted = quote_shell_command(std::string(command));
        parts.push_back("eval " + quoted);

        // Track cwd after execution
        parts.push_back("pwd -P >| " + shell_quote(cwd_file_path));

        // Join with &&
        std::string command_string;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i > 0) command_string += " && ";
            command_string += parts[i];
        }

        // Apply shell prefix if set
        if (auto prefix = get_shell_prefix()) {
            command_string = format_shell_prefix(*prefix, command_string);
        }

        return {
            .command_string = std::move(command_string),
            .cwd_file_path = std::move(cwd_file_path),
        };
    }

    [[nodiscard]] std::vector<std::string> get_spawn_args(
        std::string_view command_string) const override {
        std::vector<std::string> args;
        if (!last_snapshot_valid_) {
            args.emplace_back("-l"); // Use login shell if no snapshot
        }
        args.emplace_back("-c");
        args.emplace_back(command_string);
        return args;
    }

    [[nodiscard]] std::map<std::string, std::string>
    get_environment_overrides(std::string_view command) override {
        std::map<std::string, std::string> env;
        env["CLAUDE_CODE_SHELL_PROVIDER"] = "native";
        env["CLAUDE_CODE_SHELL_TYPE"] = "bash";
        if (!command.empty()) env["CLAUDE_CODE_LAST_COMMAND"] = std::string(command);
        if (snapshot_path_) env["CLAUDE_CODE_SHELL_SNAPSHOT"] = *snapshot_path_;
        return env;
    }

private:
    /// Create a shell snapshot file for fast initialization
    [[nodiscard]] static std::optional<std::string> create_shell_snapshot(
        const std::string& shell) {
        if (shell.empty() || !fs::exists(shell)) return std::nullopt;

        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = fs::temp_directory_path() / ("cc-repl-shell-snapshot-" + std::to_string(stamp) + ".sh");
        std::string command = shell_quote(shell) + " -l -c " +
            shell_quote("export -p") + " > " + shell_quote(path.string()) + " 2>/dev/null";
        int status = std::system(command.c_str());
        std::error_code ec;
        if (status != 0 || !fs::exists(path, ec) || fs::file_size(path, ec) == 0) {
            fs::remove(path, ec);
            return std::nullopt;
        }
        return path.string();
    }

    /// Get command to disable extended glob patterns
    [[nodiscard]] std::optional<std::string> get_disable_extglob_command() const {
        if (get_shell_prefix()) {
            // When prefix is set, include both bash and zsh commands
            return "{ shopt -u extglob || setopt NO_EXTENDED_GLOB; } >/dev/null 2>&1 || true";
        }
        if (shell_path_.find("bash") != std::string::npos) {
            return "shopt -u extglob 2>/dev/null || true";
        }
        if (shell_path_.find("zsh") != std::string::npos) {
            return "setopt NO_EXTENDED_GLOB 2>/dev/null || true";
        }
        return std::nullopt;
    }

    /// Shell-quote a string (single quotes with escaping)
    [[nodiscard]] static std::string shell_quote(std::string_view sv) {
        std::string result = "'";
        for (char c : sv) {
            if (c == '\'') result += "'\\''";
            else result += c;
        }
        result += '\'';
        return result;
    }

    /// Quote a shell command for eval
    [[nodiscard]] static std::string quote_shell_command(const std::string& cmd) {
        return shell_quote(cmd);
    }

    /// Format with shell prefix
    [[nodiscard]] static std::string format_shell_prefix(
        const std::string& prefix, const std::string& command) {
        return prefix + " " + shell_quote(command);
    }

    /// Get CLAUDE_CODE_SHELL_PREFIX env var
    [[nodiscard]] static std::optional<std::string> get_shell_prefix() {
        if (const char* val = std::getenv("CLAUDE_CODE_SHELL_PREFIX")) {
            return std::string(val);
        }
        return std::nullopt;
    }

    /// Get temp directory
    [[nodiscard]] static std::string get_tmp_dir() {
        if (const char* tmp = std::getenv("TMPDIR")) return tmp;
        if (const char* tmp = std::getenv("TMP")) return tmp;
        return "/tmp";
    }

    std::string shell_path_;
    bool skip_snapshot_;
    std::optional<std::string> snapshot_path_;
    bool last_snapshot_valid_ = false;
};

// ============================================================================
// PowershellProvider — PowerShell provider implementation
// ============================================================================

/// PowerShell shell provider for Windows environments.
class PowershellProvider : public ShellProvider {
public:
    explicit PowershellProvider(std::string shell_path)
        : shell_path_(std::move(shell_path)) {}

    [[nodiscard]] ShellType type() const override { return ShellType::PowerShell; }
    [[nodiscard]] const std::string& shell_path() const override { return shell_path_; }
    [[nodiscard]] bool detached() const override { return false; }

    [[nodiscard]] ExecCommandResult build_exec_command(
        std::string_view command, const BuildExecOptions& opts) override {

        std::string tmp_dir = get_tmp_dir();
        std::string cwd_file_path;
        if (opts.use_sandbox && opts.sandbox_tmp_dir) {
            cwd_file_path = *opts.sandbox_tmp_dir + "/cwd-" + opts.id;
        } else {
            cwd_file_path = tmp_dir + "/claude-" + opts.id + "-cwd";
        }

        // PowerShell command wrapping
        std::string ps_command = std::string(command);

        // Add cwd tracking
        std::string command_string =
            ps_command + "; (Get-Location).Path | Out-File -FilePath '" +
            escape_powershell_single_quoted(cwd_file_path) + "' -Encoding UTF8 -NoNewline";

        return {
            .command_string = std::move(command_string),
            .cwd_file_path = std::move(cwd_file_path),
        };
    }

    [[nodiscard]] std::vector<std::string> get_spawn_args(
        std::string_view command_string) const override {
        return {
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            std::string(command_string),
        };
    }

    [[nodiscard]] std::map<std::string, std::string>
    get_environment_overrides(std::string_view command) override {
        std::map<std::string, std::string> env;
        env["CLAUDE_CODE_SHELL_PROVIDER"] = "native";
        env["CLAUDE_CODE_SHELL_TYPE"] = "powershell";
        if (!command.empty()) env["CLAUDE_CODE_LAST_COMMAND"] = std::string(command);
        return env;
    }

private:
    [[nodiscard]] static std::string escape_powershell_single_quoted(std::string_view value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value) {
            escaped.push_back(ch);
            if (ch == '\'') escaped.push_back('\'');
        }
        return escaped;
    }

    [[nodiscard]] static std::string get_tmp_dir() {
        if (const char* tmp = std::getenv("TEMP")) return tmp;
        if (const char* tmp = std::getenv("TMP")) return tmp;
        return "/tmp";
    }

    std::string shell_path_;
};

// ============================================================================
// PowerShell Detection
// ============================================================================

/// Detects available PowerShell installations on the system.
struct PowershellDetection {
    /// Check if PowerShell Core (pwsh) is available
    [[nodiscard]] static bool is_pwsh_available() {
        return fs::exists("/usr/local/bin/pwsh") ||
               fs::exists("/usr/bin/pwsh") ||
               which("pwsh").has_value();
    }

    /// Check if Windows PowerShell is available
    [[nodiscard]] static bool is_windows_powershell_available() {
        #ifdef _WIN32
        return which("powershell.exe").has_value();
        #else
        return false;
        #endif
    }

    /// Get the path to the preferred PowerShell executable
    [[nodiscard]] static std::optional<std::string> get_powershell_path() {
        // Prefer pwsh (cross-platform) over powershell.exe (Windows-only)
        if (auto path = which("pwsh")) return path;
        #ifdef _WIN32
        if (auto path = which("powershell.exe")) return path;
        #endif
        return std::nullopt;
    }

private:
    [[nodiscard]] static std::optional<std::string> which(std::string_view name) {
        if (name.empty()) return std::nullopt;
        #ifdef _WIN32
        std::string cmd = "where " + std::string(name) + " 2>NUL";
        #else
        std::string cmd = "which " + std::string(name) + " 2>/dev/null";
        #endif
        FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
        if (!pipe) return std::nullopt;
        char buffer[512]{};
        std::string result;
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        cc::utils::bash::pclose_spawn(pipe);
        // Trim trailing whitespace/newlines
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
            result.pop_back();
        }
        if (result.empty()) return std::nullopt;
        return result;
    }
};

// ============================================================================
// resolveDefaultShell — Determine the user's default shell
// ============================================================================

/// Resolves the default shell for the current user/platform.
/// Priority: CLAUDE_CODE_SHELL env > SHELL env > platform default.
[[nodiscard]] inline std::string resolve_default_shell() {
    // 1. Explicit override
    if (const char* shell = std::getenv("CLAUDE_CODE_SHELL")) {
        if (fs::exists(shell)) return shell;
    }

    // 2. Standard SHELL env var (Unix)
    if (const char* shell = std::getenv("SHELL")) {
        if (fs::exists(shell)) return shell;
    }

    // 3. Platform defaults
    #ifdef _WIN32
    // On Windows, prefer PowerShell Core > Windows PowerShell > cmd.exe
    if (auto ps = PowershellDetection::get_powershell_path()) return *ps;
    return "cmd.exe";
    #else
    // On Unix, fallback to /bin/bash
    if (fs::exists("/bin/bash")) return "/bin/bash";
    if (fs::exists("/bin/sh")) return "/bin/sh";
    return "/bin/sh";
    #endif
}

/// Determine the shell type from a shell path
[[nodiscard]] inline ShellType detect_shell_type(std::string_view shell_path) {
    if (shell_path.find("pwsh") != std::string_view::npos ||
        shell_path.find("powershell") != std::string_view::npos) {
        return ShellType::PowerShell;
    }
    return ShellType::Bash; // Default: treat as bash-compatible
}

/// Create the appropriate shell provider for the given shell path
[[nodiscard]] inline std::unique_ptr<ShellProvider> create_provider(
    const std::string& shell_path,
    bool skip_snapshot = false) {
    auto type = detect_shell_type(shell_path);
    switch (type) {
        case ShellType::PowerShell:
            return std::make_unique<PowershellProvider>(shell_path);
        case ShellType::Bash:
            return std::make_unique<BashProvider>(
                shell_path,
                BashProvider::Options{.skip_snapshot = skip_snapshot});
    }
    return std::make_unique<BashProvider>(shell_path);
}

/// Create a provider using the resolved default shell
[[nodiscard]] inline std::unique_ptr<ShellProvider> create_default_provider() {
    return create_provider(resolve_default_shell());
}

} // namespace cc::utils::shell_providers
