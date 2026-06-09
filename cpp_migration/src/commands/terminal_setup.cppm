/// @file terminal_setup.cppm
/// @brief Terminal and shell setup helpers.
/// Detects the user's shell, generates rc-file snippets for completions,
/// the CLAUDE_CODE_BIN env var, and theme hints. Supports --apply to
/// write the snippet to the appropriate rc file with a timestamped backup.
/// (FTXUI rendering DEFERRED to Phase 4).
module;

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#if !defined(_WIN32)
#  include <pwd.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.commands.terminal_setup;

export namespace cc::commands::terminal_setup {

// ============================================================
// Public types
// ============================================================

struct CommandResponse {
    bool ok{true};
    std::string message;
};

/// Supported shell names (canonical).
enum class ShellKind : std::uint8_t {
    Bash,
    Zsh,
    Fish,
    Nushell,
    PowerShell,
    Unknown,
};

/// Detected shell information.
struct ShellInfo {
    ShellKind kind = ShellKind::Unknown;
    std::string canonical_name;     // "bash", "zsh", "fish", "nu", "powershell"
    std::string exec_path;          // e.g. "/bin/zsh"
    std::string rc_path;            // e.g. "~/.bashrc" (with ~ expanded)
};

/// Human-readable display name for a shell.
[[nodiscard]] inline constexpr std::string_view shell_display_name(ShellKind k) noexcept {
    switch (k) {
        case ShellKind::Bash:       return "Bash";
        case ShellKind::Zsh:        return "Zsh";
        case ShellKind::Fish:       return "Fish";
        case ShellKind::Nushell:    return "Nushell";
        case ShellKind::PowerShell: return "PowerShell";
        case ShellKind::Unknown:    return "Unknown";
    }
    return "Unknown";
}

[[nodiscard]] inline constexpr std::string_view shell_canonical(ShellKind k) noexcept {
    switch (k) {
        case ShellKind::Bash:       return "bash";
        case ShellKind::Zsh:        return "zsh";
        case ShellKind::Fish:       return "fish";
        case ShellKind::Nushell:    return "nu";
        case ShellKind::PowerShell: return "powershell";
        case ShellKind::Unknown:    return "unknown";
    }
    return "unknown";
}

// ============================================================
// Helpers
// ============================================================

namespace detail {

/// Expand a leading `~` in a path to the user's home directory.
[[nodiscard]] inline std::string expand_tilde(std::string_view path) {
    if (!path.empty() && path.front() == '~') {
        const char* home = std::getenv("HOME");
        if (!home) home = ".";
        std::string result(home);
        path.remove_prefix(1);
        // Skip a leading slash so we don't get "home//foo"
        if (!path.empty() && path.front() == '/') path.remove_prefix(1);
        if (!path.empty()) {
            result.push_back('/');
            result.append(path);
        }
        return result;
    }
    return std::string(path);
}

/// Try to detect the shell from an executable path string.
[[nodiscard]] inline ShellKind classify_shell_path(std::string_view exec) {
    auto basename = [](std::string_view p) -> std::string_view {
        auto slash = p.find_last_of("/\\");
        if (slash != std::string_view::npos) p.remove_prefix(slash + 1);
        return p;
    };
    std::string_view b = basename(exec);
    if (b == "bash" || b == "-bash" || b == "rbash")      return ShellKind::Bash;
    if (b == "zsh"  || b == "-zsh")                        return ShellKind::Zsh;
    if (b == "fish")                                       return ShellKind::Fish;
    if (b == "nu")                                         return ShellKind::Nushell;
    if (b == "powershell" || b == "pwsh" || b == "pwsh.exe") return ShellKind::PowerShell;
    return ShellKind::Unknown;
}

/// Compute the conventional rc-file path for a given shell (tilde NOT expanded).
[[nodiscard]] inline std::string rc_for_shell(ShellKind kind) {
    switch (kind) {
        case ShellKind::Bash:       return "~/.bashrc";
        case ShellKind::Zsh:        return "~/.zshrc";
        case ShellKind::Fish:       return "~/.config/fish/config.fish";
        case ShellKind::Nushell:    return "~/.config/nushell/config.nu";
        case ShellKind::PowerShell: return "~/.config/powershell/profile.ps1";
        case ShellKind::Unknown:    return "~/.profile";
    }
    return "~/.profile";
}

/// Read a file fully into a string, or "" on failure.
[[nodiscard]] inline std::string read_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

/// Append text to a file, creating parent directories if needed.
/// Returns true on success.
[[nodiscard]] inline bool append_file(const std::string& path, std::string_view text) {
    namespace fs = std::filesystem;
    fs::path p(path);
    auto parent = p.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) return false;
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::app);
    if (!ofs.is_open()) return false;
    ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    return ofs.good();
}

/// Copy src to dst byte-for-byte. Returns true on success.
[[nodiscard]] inline bool copy_file(const std::string& src, const std::string& dst) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

/// Produce a timestamp suffix for backup filenames: YYYYMMDD-HHMMSS.
[[nodiscard]] inline std::string backup_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d-%H%M%S");
    return oss.str();
}

/// Check if the generated snippet marker is already present in the file content.
[[nodiscard]] inline bool snippet_already_installed(std::string_view file_content,
                                                    std::string_view start_marker) {
    return file_content.find(start_marker) != std::string_view::npos;
}

} // namespace detail

// ============================================================
// Shell detection
// ============================================================

/// Detect the user's shell using the ordered heuristic:
///   1. $SHELL environment variable
///   2. getpwuid(getuid())->pw_shell
///   3. Fallback: bash
[[nodiscard]] inline ShellInfo detect_shell() {
    ShellInfo info;

    // Step 1: $SHELL
    if (const char* shell_env = std::getenv("SHELL")) {
        info.exec_path = shell_env;
        info.kind = detail::classify_shell_path(info.exec_path);
    }

    // Step 2: getpwuid if $SHELL didn't give us a usable shell
    if (info.kind == ShellKind::Unknown || info.exec_path.empty()) {
#if !defined(_WIN32)
        struct passwd pwd{};
        struct passwd* result = nullptr;
        // Use sysconf(_SC_GETPW_R_SIZE_MAX) to get the buffer size; fallback to 4096
        long bufsize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
        if (bufsize <= 0) bufsize = 4096;
        std::vector<char> buf(static_cast<std::size_t>(bufsize));
        int rc = ::getpwuid_r(::getuid(), &pwd, buf.data(), buf.size(), &result);
        if (rc == 0 && result != nullptr && result->pw_shell) {
            info.exec_path = result->pw_shell;
            info.kind = detail::classify_shell_path(info.exec_path);
        }
#endif
    }

    // Step 3: fallback
    if (info.kind == ShellKind::Unknown) {
        info.kind = ShellKind::Bash;
        info.exec_path = "/bin/bash";
    }

    info.canonical_name = std::string(shell_canonical(info.kind));
    info.rc_path = detail::expand_tilde(detail::rc_for_shell(info.kind));
    return info;
}

/// Detect the current terminal emulator from env vars.
[[nodiscard]] inline std::string detect_terminal() {
    if (auto* prog = std::getenv("TERM_PROGRAM"); prog && prog[0] != '\0') return prog;
    if (auto* term = std::getenv("TERM");         term && term[0] != '\0') return term;
    return "unknown";
}

// ============================================================
// RC snippet generation
// ============================================================

/// Unique markers for bracket-installed snippets (used to detect and re-install).
[[nodiscard]] inline std::string snippet_start_marker() {
    return "# >>> cc-repl terminal-setup (do not remove this line) >>>";
}
[[nodiscard]] inline std::string snippet_end_marker() {
    return "# <<< cc-repl terminal-setup (do not remove this line) <<<";
}
[[nodiscard]] inline std::string snippet_start_marker_fish() {
    return "# >>> cc-repl terminal-setup >>>";
}
[[nodiscard]] inline std::string snippet_start_marker_nushell() {
    return "# >>> cc-repl terminal-setup >>>";
}
[[nodiscard]] inline std::string snippet_start_marker_powershell() {
    return "# >>> cc-repl terminal-setup >>>";
}

/// Generate the shell-completion invocation for a given shell kind.
/// These are placeholders that match the TS completion-cache module's output shape.
[[nodiscard]] inline std::string completion_snippet(ShellKind kind) {
    switch (kind) {
        case ShellKind::Bash: return R"(# Completions for cc-repl
if command -v cc-repl >/dev/null 2>&1; then
  eval "$(cc-repl completions bash)"
fi
)";
        case ShellKind::Zsh: return R"(# Completions for cc-repl
if command -v cc-repl >/dev/null 2>&1; then
  eval "$(cc-repl completions zsh)"
fi
# Make sure compinit picks up new entries
autoload -Uz compinit && compinit -C
)";
        case ShellKind::Fish: return R"(# Completions for cc-repl
if command -v cc-repl >/dev/null 2>&1
    cc-repl completions fish > $__fish_config_dir/completions/cc-repl.fish
end
)";
        case ShellKind::Nushell: return R"(# Completions for cc-repl (Nushell)
# Run once per session: source $(cc-repl completions nu | save --force /tmp/ccrepl-completions.nu)
# Then: source /tmp/ccrepl-completions.nu
)";
        case ShellKind::PowerShell: return R"(# Completions for cc-repl (PowerShell)
if (Get-Command cc-repl -ErrorAction SilentlyContinue) {
  cc-repl completions powershell | Out-String | Invoke-Expression
}
)";
        case ShellKind::Unknown: return "";
    }
    return "";
}

/// Generate the environment-variable snippet (CLAUDE_CODE_BIN).
[[nodiscard]] inline std::string env_snippet(ShellKind kind) {
    switch (kind) {
        case ShellKind::Bash:
        case ShellKind::Zsh:
            return R"(# Locate the cc-repl binary for easy reuse
if command -v cc-repl >/dev/null 2>&1; then
  export CLAUDE_CODE_BIN="$(command -v cc-repl)"
fi
)";
        case ShellKind::Fish:
            return R"(# Locate the cc-repl binary for easy reuse
if command -v cc-repl >/dev/null 2>&1
    set -gx CLAUDE_CODE_BIN (command -v cc-repl)
end
)";
        case ShellKind::Nushell:
            return R"(# Locate the cc-repl binary for easy reuse
# $env.CLAUDE_CODE_BIN = (which cc-repl | get path)
)";
        case ShellKind::PowerShell:
            return R"(# Locate the cc-repl binary for easy reuse
if (Get-Command cc-repl -ErrorAction SilentlyContinue) {
  $env:CLAUDE_CODE_BIN = (Get-Command cc-repl).Source
}
)";
        case ShellKind::Unknown: return "";
    }
    return "";
}

/// Generate the theme-hint snippet (terminal title / prompt tweaks).
[[nodiscard]] inline std::string theme_hint_snippet(ShellKind kind) {
    switch (kind) {
        case ShellKind::Bash:
        case ShellKind::Zsh:
            return R"(# Theme / terminal integration hints
# - Set CC_REPL_THEME=dark|light|auto to override theme detection
# - Ensure 256-color / truecolor terminfo for best UI rendering
# export CC_REPL_THEME=auto
)";
        case ShellKind::Fish:
            return R"(# Theme / terminal integration hints
# set -gx CC_REPL_THEME auto   # dark|light|auto
)";
        case ShellKind::Nushell:
            return R"(# Theme / terminal integration hints
# $env.CC_REPL_THEME = "auto"   # dark|light|auto
)";
        case ShellKind::PowerShell:
            return R"(# Theme / terminal integration hints
# $env:CC_REPL_THEME = "auto"   # dark|light|auto
)";
        case ShellKind::Unknown: return "";
    }
    return "";
}

/// Build the complete rc-file snippet for a given shell.
/// Includes bracketing markers so it can be detected / regenerated idempotently.
[[nodiscard]] inline std::string generate_rc_snippet(const ShellInfo& shell) {
    std::string out;
    auto [start, end] = [&]() -> std::pair<std::string, std::string> {
        switch (shell.kind) {
            case ShellKind::Fish:       return {snippet_start_marker_fish(), "# <<< cc-repl terminal-setup <<<"};
            case ShellKind::Nushell:    return {snippet_start_marker_nushell(), "# <<< cc-repl terminal-setup <<<"};
            case ShellKind::PowerShell: return {snippet_start_marker_powershell(), "# <<< cc-repl terminal-setup <<<"};
            default:                    return {snippet_start_marker(), snippet_end_marker()};
        }
    }();

    out += start + "\n";
    out += completion_snippet(shell.kind);
    out += env_snippet(shell.kind);
    out += theme_hint_snippet(shell.kind);
    out += end + "\n";
    return out;
}

// ============================================================
// Apply / backup logic
// ============================================================

struct ApplyResult {
    bool succeeded = false;
    std::string report;
    std::string backup_path;   // empty if no backup was created
    bool already_present = false;
};

/// Write the generated snippet to the rc file with backup semantics.
[[nodiscard]] inline ApplyResult apply_snippet(const ShellInfo& shell, std::string_view snippet) {
    namespace fs = std::filesystem;
    ApplyResult r;

    // Ensure parent directory exists
    std::error_code ec;
    auto parent = fs::path(shell.rc_path).parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        fs::create_directories(parent, ec);
        if (ec) {
            r.succeeded = false;
            r.report = std::format("Failed to create directory '{}': {}",
                                   parent.string(), ec.message());
            return r;
        }
    }

    // Read existing content if any
    bool existed = fs::exists(shell.rc_path, ec);
    std::string existing;
    if (existed && !ec) {
        existing = detail::read_file(shell.rc_path);
        if (existing.empty() && fs::file_size(shell.rc_path, ec) > 0) {
            // File exists but we couldn't read it — bail out to avoid corrupting it
            r.succeeded = false;
            r.report = std::format("Failed to read existing rc file: {}", shell.rc_path);
            return r;
        }

        // Check if already installed
        auto marker = snippet_start_marker();
        if (detail::snippet_already_installed(existing, marker)) {
            r.succeeded = true;
            r.already_present = true;
            r.report = std::format("Snippet already present in {}.\n"
                                   "Remove the bracketing # >>> / # <<< lines to re-apply.",
                                   shell.rc_path);
            return r;
        }

        // Backup
        r.backup_path = shell.rc_path + ".bak-" + detail::backup_timestamp();
        if (!detail::copy_file(shell.rc_path, r.backup_path)) {
            r.succeeded = false;
            r.report = std::format("Failed to create backup: {} -> {}",
                                   shell.rc_path, r.backup_path);
            return r;
        }
    }

    // Ensure existing content ends with newline before appending
    std::string prefix;
    if (!existing.empty() && existing.back() != '\n') prefix = "\n\n";
    else if (!existing.empty() && existing.rfind("\n\n") != existing.size() - 2) prefix = "\n";

    std::string to_append = prefix;
    to_append += snippet;

    if (!detail::append_file(shell.rc_path, to_append)) {
        r.succeeded = false;
        r.report = std::format("Failed to write to rc file: {}", shell.rc_path);
        // Restore backup if we created one and it exists
        if (!r.backup_path.empty() && fs::exists(r.backup_path)) {
            std::error_code ec2;
            fs::copy_file(r.backup_path, shell.rc_path,
                          fs::copy_options::overwrite_existing, ec2);
        }
        return r;
    }

    r.succeeded = true;
    std::ostringstream oss;
    oss << "Successfully applied terminal-setup snippet to:\n"
        << "  " << shell.rc_path << "\n";
    if (!r.backup_path.empty()) {
        oss << "\nBackup created at:\n"
            << "  " << r.backup_path << "\n";
    }
    oss << "\nTo take effect in this shell session run:\n"
        << "  source " << shell.rc_path << "\n"
        << "Or start a new terminal.";
    r.report = oss.str();
    return r;
}

// ============================================================
// Main entry point (invoked via CC_RUNTIME_HELPER_COMMAND macro)
// ============================================================

[[nodiscard]] inline constexpr auto name() -> std::string_view { return "terminal-setup"; }

/// Parse the joined args string to extract flags. Supports:
///   --apply          Write snippet to rc file (creates backup first)
///   --shell=NAME     Override detected shell (bash|zsh|fish|nu|powershell)
[[nodiscard]] inline auto run(std::string_view args) -> CommandResponse {
    // Parse flags manually (args is a space-joined string)
    bool apply = false;
    std::string shell_override;
    std::istringstream iss(std::string{args});
    std::string tok;
    while (iss >> tok) {
        if (tok == "--apply" || tok == "-a") {
            apply = true;
        } else if (tok.starts_with("--shell=")) {
            shell_override = tok.substr(8);
        } else if (tok == "--shell") {
            if (iss >> tok) shell_override = tok;
        }
    }

    ShellInfo shell;
    if (!shell_override.empty()) {
        shell.kind = detail::classify_shell_path(shell_override);
        if (shell.kind == ShellKind::Unknown) {
            return {false,
                    std::format("Unknown shell override '{}'. "
                                "Valid: bash | zsh | fish | nu | powershell",
                                shell_override)};
        }
        shell.canonical_name = std::string(shell_canonical(shell.kind));
        shell.exec_path = shell_override;
        shell.rc_path = detail::expand_tilde(detail::rc_for_shell(shell.kind));
    } else {
        shell = detect_shell();
    }

    std::string terminal = detect_terminal();
    auto snippet = generate_rc_snippet(shell);

    std::ostringstream oss;
    oss << "Terminal Setup\n";
    oss << std::string(60, '=') << "\n\n";
    oss << std::format("Terminal : {}\n", terminal);
    oss << std::format("Shell    : {} ({})\n", shell_display_name(shell.kind), shell.exec_path);
    oss << std::format("RC file  : {}\n", shell.rc_path);
    oss << "\n";

    if (!apply) {
        // Preview mode
        oss << "Generated shell snippet (preview mode; re-run with --apply to write):\n";
        oss << std::string(60, '-') << "\n\n";
        oss << snippet;
        oss << "\n" << std::string(60, '-') << "\n";
        oss << "\nTo apply:\n";
        oss << std::format("  /terminal-setup --apply\n");
        return {true, oss.str()};
    }

    // Apply mode
    auto result = apply_snippet(shell, snippet);
    if (!result.succeeded) {
        oss << result.report;
        return {false, oss.str()};
    }
    if (result.already_present) {
        oss << result.report;
    } else {
        oss << result.report;
    }
    return {true, oss.str()};
}

} // namespace cc::commands::terminal_setup
