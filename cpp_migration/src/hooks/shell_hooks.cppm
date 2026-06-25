/// @file shell_hooks.cppm
/// @brief User-configurable shell execution hooks.
/// Loads hook definitions from settings files (~/.claude/settings.json, .claude/settings.json)
/// and executes matching shell commands on lifecycle events with timeout, output capture,
/// and return-code-based permission control.
module;

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

export module cc.hooks.shell_hooks;

import cc.utils.hooks_registry;
import cc.hooks.lifecycle_hooks;

export namespace cc::hooks::shell {

using namespace cc::utils::hooks_registry;

// ============================================================================
// Constants
// ============================================================================

/// Default timeout for hook commands (seconds)
inline constexpr int kDefaultHookTimeoutSeconds = 60;

/// Maximum output captured from a hook subprocess (bytes)
inline constexpr std::size_t kMaxHookOutputBytes = 64 * 1024;

/// Maximum concurrent hooks executing simultaneously
inline constexpr std::size_t kMaxConcurrentHooks = 8;

// ============================================================================
// Hook Execution Result
// ============================================================================

/// Result of executing a single shell hook command
struct ShellHookResult {
    std::string hook_name;
    std::string command;
    int exit_code = -1;
    std::string stdout_output;
    std::string stderr_output;
    std::chrono::milliseconds duration{0};
    bool timed_out = false;
    bool killed = false;
};

/// Aggregated result of executing all matching hooks for an event
struct HookExecutionSummary {
    HookEventType event;
    std::vector<ShellHookResult> results;
    bool all_passed = true;          // All hooks returned 0
    bool any_denied = false;         // Any hook returned non-zero (permission denied)
    std::optional<std::string> deny_reason; // First deny reason from hook output
};

// ============================================================================
// Hook Execution Context — passed to shell hooks as environment variables
// ============================================================================

/// Context information passed to hook subprocesses via environment or stdin
struct HookContext {
    std::string event_name;
    std::string tool_name;           // For PreToolUse/PostToolUse
    std::string tool_input_json;     // For PreToolUse
    std::string tool_output_preview; // For PostToolUse
    std::string tool_use_id;
    std::string session_id;
    std::string working_directory;
    std::unordered_map<std::string, std::string> extra_env;
};

// ============================================================================
// Shell Hook Executor — runs a single command with timeout
// ============================================================================

namespace detail {

/// Execute a shell command with timeout and output capture.
/// Uses fork+exec to avoid blocking the main thread.
[[nodiscard]] inline ShellHookResult execute_command(
    std::string_view command,
    std::string_view shell,
    const HookContext& context,
    std::chrono::seconds timeout)
{
    ShellHookResult result;
    result.command = std::string(command);

    auto start = std::chrono::steady_clock::now();

    // Create pipes for stdout and stderr
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (::pipe(stdout_pipe) != 0 || ::pipe(stderr_pipe) != 0) {
        result.exit_code = -1;
        result.stderr_output = "Failed to create pipes";
        return result;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdout_pipe[0]); ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        result.exit_code = -1;
        result.stderr_output = "Failed to fork";
        return result;
    }

    if (pid == 0) {
        // Child process
        ::close(stdout_pipe[0]);
        ::close(stderr_pipe[0]);

        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[1]);

        // Set environment variables for hook context
        ::setenv("CLAUDE_HOOK_EVENT", context.event_name.c_str(), 1);
        if (!context.tool_name.empty()) {
            ::setenv("CLAUDE_HOOK_TOOL_NAME", context.tool_name.c_str(), 1);
        }
        if (!context.tool_use_id.empty()) {
            ::setenv("CLAUDE_HOOK_TOOL_USE_ID", context.tool_use_id.c_str(), 1);
        }
        if (!context.session_id.empty()) {
            ::setenv("CLAUDE_HOOK_SESSION_ID", context.session_id.c_str(), 1);
        }
        if (!context.working_directory.empty()) {
            ::setenv("CLAUDE_HOOK_CWD", context.working_directory.c_str(), 1);
        }
        for (const auto& [key, value] : context.extra_env) {
            ::setenv(key.c_str(), value.c_str(), 1);
        }

        // Write tool_input_json to stdin if present
        // For simplicity, set as env var (limited to ~128KB on most systems)
        if (!context.tool_input_json.empty()) {
            ::setenv("CLAUDE_HOOK_TOOL_INPUT", context.tool_input_json.c_str(), 1);
        }
        if (!context.tool_output_preview.empty()) {
            ::setenv("CLAUDE_HOOK_TOOL_OUTPUT", context.tool_output_preview.c_str(), 1);
        }

        // Execute via shell
        std::string shell_str(shell.empty() ? "bash" : shell);
        ::execlp(shell_str.c_str(), shell_str.c_str(), "-c", std::string(command).c_str(), nullptr);
        ::_exit(127); // exec failed
    }

    // Parent process
    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);

    // Set non-blocking on read ends
    ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    // Poll for completion with timeout
    auto deadline = start + timeout;
    bool child_done = false;
    int status = 0;

    while (!child_done) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            // Timeout — kill the child
            ::kill(pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            result.timed_out = true;
            result.killed = true;
            break;
        }

        pid_t wpid = ::waitpid(pid, &status, WNOHANG);
        if (wpid == pid) {
            child_done = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // Read remaining output
    auto read_all = [](int fd, std::string& output) {
        std::array<char, 4096> buf{};
        while (true) {
            auto n = ::read(fd, buf.data(), buf.size());
            if (n <= 0) break;
            if (output.size() + static_cast<std::size_t>(n) > kMaxHookOutputBytes) {
                output.append(buf.data(), kMaxHookOutputBytes - output.size());
                break;
            }
            output.append(buf.data(), static_cast<std::size_t>(n));
        }
    };

    read_all(stdout_pipe[0], result.stdout_output);
    read_all(stderr_pipe[0], result.stderr_output);

    ::close(stdout_pipe[0]);
    ::close(stderr_pipe[0]);

    if (!result.timed_out) {
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
            result.killed = true;
        }
    }

    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    return result;
}

/// Check if a matcher pattern matches a given value.
/// Supports:
/// - Empty/null matcher → matches everything
/// - Exact string match
/// - Glob-style '*' wildcard prefix/suffix
[[nodiscard]] inline bool matches_pattern(
    std::optional<std::string_view> pattern,
    std::string_view value) noexcept
{
    if (!pattern.has_value() || pattern->empty()) {
        return true; // No pattern = match all
    }

    auto pat = *pattern;

    // Exact match
    if (pat == value) return true;

    // Wildcard suffix: "Bash*" matches "BashTool"
    if (pat.back() == '*') {
        auto prefix = pat.substr(0, pat.size() - 1);
        return value.starts_with(prefix);
    }

    // Wildcard prefix: "*Tool" matches "BashTool"
    if (pat.front() == '*') {
        auto suffix = pat.substr(1);
        return value.ends_with(suffix);
    }

    return false;
}

/// Parse JSON hooks config from file content.
/// Expected format:
/// {
///   "hooks": {
///     "PreToolUse": [
///       { "matcher": "BashTool", "hooks": [{"type": "command", "command": "..."}] }
///     ]
///   }
/// }
/// Returns a map from event type name → vector of HookMatcher.
[[nodiscard]] inline std::unordered_map<std::string, std::vector<HookMatcher>>
parse_hooks_from_json(std::string_view json_content) {
    std::unordered_map<std::string, std::vector<HookMatcher>> result;

    // Minimal JSON parsing: find "hooks" key, then iterate event types.
    // Uses simple string-based parsing sufficient for the known format.
    auto find_key = [](std::string_view json, std::string_view key, std::size_t start = 0) -> std::size_t {
        std::string needle = std::format("\"{}\"", key);
        auto pos = json.find(needle, start);
        if (pos == std::string_view::npos) return std::string_view::npos;
        // Skip past the key and colon
        pos = json.find(':', pos + needle.size());
        if (pos == std::string_view::npos) return std::string_view::npos;
        return pos + 1;
    };

    auto skip_whitespace = [](std::string_view json, std::size_t pos) -> std::size_t {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == '\t')) {
            ++pos;
        }
        return pos;
    };

    auto extract_string = [](std::string_view json, std::size_t pos) -> std::pair<std::string, std::size_t> {
        if (pos >= json.size() || json[pos] != '"') return {"", pos};
        ++pos;
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
                switch (json[pos]) {
                    case 'n': value += '\n'; break;
                    case 't': value += '\t'; break;
                    case '\\': value += '\\'; break;
                    case '"': value += '"'; break;
                    default: value += json[pos]; break;
                }
            } else {
                value += json[pos];
            }
            ++pos;
        }
        if (pos < json.size()) ++pos; // skip closing quote
        return {value, pos};
    };

    // Find "hooks" object
    auto hooks_pos = find_key(json_content, "hooks");
    if (hooks_pos == std::string_view::npos) return result;

    hooks_pos = skip_whitespace(json_content, hooks_pos);
    if (hooks_pos >= json_content.size() || json_content[hooks_pos] != '{') return result;

    // Parse each event type within "hooks" object
    // This is a simplified parser that handles the known structure
    static constexpr std::array<std::string_view, 8> known_events = {
        "PreToolUse", "PostToolUse", "Notification", "Stop",
        "SubagentStart", "SubagentStop", "UserPromptSubmit", "SessionStart"
    };

    for (auto event_name : known_events) {
        auto event_pos = find_key(json_content, event_name, hooks_pos);
        if (event_pos == std::string_view::npos) continue;

        event_pos = skip_whitespace(json_content, event_pos);
        if (event_pos >= json_content.size() || json_content[event_pos] != '[') continue;

        // Parse array of matcher objects
        std::vector<HookMatcher> matchers;
        auto pos = event_pos + 1;

        while (pos < json_content.size()) {
            pos = skip_whitespace(json_content, pos);
            if (pos >= json_content.size() || json_content[pos] == ']') break;
            if (json_content[pos] == ',') { ++pos; continue; }
            if (json_content[pos] != '{') break;

            // Parse a single matcher object
            HookMatcher matcher;
            ++pos; // skip '{'

            // Find "matcher" field
            auto matcher_field = find_key(json_content, "matcher", pos);
            if (matcher_field != std::string_view::npos && matcher_field < json_content.find('}', pos)) {
                auto mf_pos = skip_whitespace(json_content, matcher_field);
                if (mf_pos < json_content.size() && json_content[mf_pos] == '"') {
                    auto [val, next] = extract_string(json_content, mf_pos);
                    matcher.matcher = val;
                }
            }

            // Find "hooks" array within this matcher
            auto hooks_arr = find_key(json_content, "hooks", pos);
            // Find closing brace of this matcher object
            int brace_depth = 1;
            auto obj_end = pos;
            while (obj_end < json_content.size() && brace_depth > 0) {
                if (json_content[obj_end] == '{') ++brace_depth;
                else if (json_content[obj_end] == '}') --brace_depth;
                ++obj_end;
            }

            if (hooks_arr != std::string_view::npos && hooks_arr < obj_end) {
                auto ha_pos = skip_whitespace(json_content, hooks_arr);
                if (ha_pos < json_content.size() && json_content[ha_pos] == '[') {
                    ++ha_pos;
                    // Parse individual hook commands
                    while (ha_pos < obj_end) {
                        ha_pos = skip_whitespace(json_content, ha_pos);
                        if (ha_pos >= json_content.size() || json_content[ha_pos] == ']') break;
                        if (json_content[ha_pos] == ',') { ++ha_pos; continue; }
                        if (json_content[ha_pos] != '{') break;

                        // Parse a hook command object
                        int cmd_depth = 1;
                        auto cmd_start = ha_pos + 1;
                        auto cmd_end = cmd_start;
                        while (cmd_end < json_content.size() && cmd_depth > 0) {
                            if (json_content[cmd_end] == '{') ++cmd_depth;
                            else if (json_content[cmd_end] == '}') --cmd_depth;
                            ++cmd_end;
                        }

                        // Extract "command" field
                        auto cmd_field = find_key(json_content, "command", cmd_start);
                        if (cmd_field != std::string_view::npos && cmd_field < cmd_end) {
                            auto cf_pos = skip_whitespace(json_content, cmd_field);
                            if (cf_pos < json_content.size() && json_content[cf_pos] == '"') {
                                auto [cmd_val, _] = extract_string(json_content, cf_pos);

                                CommandHookConfig hook_cfg;
                                hook_cfg.command = cmd_val;

                                // Extract optional "timeout" field
                                auto timeout_field = find_key(json_content, "timeout", cmd_start);
                                if (timeout_field != std::string_view::npos && timeout_field < cmd_end) {
                                    auto tf_pos = skip_whitespace(json_content, timeout_field);
                                    std::string num_str;
                                    while (tf_pos < json_content.size() && json_content[tf_pos] >= '0' && json_content[tf_pos] <= '9') {
                                        num_str += json_content[tf_pos];
                                        ++tf_pos;
                                    }
                                    if (!num_str.empty()) {
                                        hook_cfg.timeout_seconds = std::stoi(num_str);
                                    }
                                }

                                matcher.hooks.push_back(std::move(hook_cfg));
                            }
                        }

                        ha_pos = cmd_end;
                    }
                }
            }

            if (!matcher.hooks.empty()) {
                matchers.push_back(std::move(matcher));
            }
            pos = obj_end;
        }

        if (!matchers.empty()) {
            result[std::string(event_name)] = std::move(matchers);
        }
    }

    return result;
}

} // namespace detail

// ============================================================================
// ShellHookLoader — discovers and loads hook configurations
// ============================================================================

class ShellHookLoader {
public:
    ShellHookLoader() = default;

    /// Load hooks from all settings files in priority order.
    /// Searches: ~/.claude/settings.json, .claude/settings.json, .claude/settings.local.json
    void load_from_settings() {
        std::lock_guard lock(mu_);
        hooks_by_event_.clear();

        // Load in priority order (later overrides earlier for same matcher)
        load_file(get_user_settings_path(), HookSource::UserSettings);
        load_file(get_project_settings_path(), HookSource::ProjectSettings);
        load_file(get_local_settings_path(), HookSource::LocalSettings);
    }

    /// Load hooks from a specific file path
    void load_from_file(const std::filesystem::path& path, HookSource source) {
        std::lock_guard lock(mu_);
        load_file(path, source);
    }

    /// Get all matchers for a given event type
    [[nodiscard]] std::vector<HookMatcher> get_hooks_for_event(HookEventType event) const {
        std::lock_guard lock(mu_);
        auto name = std::string(hook_event_name(event));
        if (auto it = hooks_by_event_.find(name); it != hooks_by_event_.end()) {
            return it->second;
        }
        return {};
    }

    /// Check if any hooks are configured for a given event type
    [[nodiscard]] bool has_hooks_for_event(HookEventType event) const {
        std::lock_guard lock(mu_);
        auto name = std::string(hook_event_name(event));
        auto it = hooks_by_event_.find(name);
        return it != hooks_by_event_.end() && !it->second.empty();
    }

    /// Get total number of configured hook matchers across all events
    [[nodiscard]] std::size_t total_matchers() const {
        std::lock_guard lock(mu_);
        std::size_t count = 0;
        for (const auto& [_, matchers] : hooks_by_event_) {
            count += matchers.size();
        }
        return count;
    }

    /// Clear all loaded hooks
    void clear() {
        std::lock_guard lock(mu_);
        hooks_by_event_.clear();
    }

private:
    void load_file(const std::filesystem::path& path, [[maybe_unused]] HookSource source) {
        if (!std::filesystem::exists(path)) return;

        // Read file content
        std::FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return;

        std::string content;
        std::array<char, 4096> buf{};
        while (auto n = std::fread(buf.data(), 1, buf.size(), f)) {
            content.append(buf.data(), n);
        }
        std::fclose(f);

        if (content.empty()) return;

        auto parsed = detail::parse_hooks_from_json(content);
        for (auto& [event_name, matchers] : parsed) {
            auto& existing = hooks_by_event_[event_name];
            existing.insert(existing.end(),
                           std::make_move_iterator(matchers.begin()),
                           std::make_move_iterator(matchers.end()));
        }
    }

    [[nodiscard]] static std::filesystem::path get_user_settings_path() {
        const char* home = std::getenv("HOME");
        if (!home) home = "/tmp";
        return std::filesystem::path(home) / ".claude" / "settings.json";
    }

    [[nodiscard]] static std::filesystem::path get_project_settings_path() {
        return std::filesystem::current_path() / ".claude" / "settings.json";
    }

    [[nodiscard]] static std::filesystem::path get_local_settings_path() {
        return std::filesystem::current_path() / ".claude" / "settings.local.json";
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<HookMatcher>> hooks_by_event_;
};

// ============================================================================
// ShellHookExecutor — dispatches hooks for events
// ============================================================================

class ShellHookExecutor {
public:
    explicit ShellHookExecutor(ShellHookLoader& loader)
        : loader_(loader) {}

    /// Execute all matching hooks for a PreToolUse event.
    /// Returns summary; if any_denied is true, the tool should be blocked.
    [[nodiscard]] HookExecutionSummary execute_pre_tool_use(
        std::string_view tool_name,
        std::string_view tool_input_json,
        std::string_view tool_use_id,
        std::string_view session_id)
    {
        HookContext ctx{
            .event_name = "PreToolUse",
            .tool_name = std::string(tool_name),
            .tool_input_json = std::string(tool_input_json),
            .tool_output_preview = {},
            .tool_use_id = std::string(tool_use_id),
            .session_id = std::string(session_id),
            .working_directory = std::filesystem::current_path().string(),
            .extra_env = {}
        };
        return execute_event(HookEventType::PreToolUse, tool_name, ctx);
    }

    /// Execute all matching hooks for a PostToolUse event.
    [[nodiscard]] HookExecutionSummary execute_post_tool_use(
        std::string_view tool_name,
        std::string_view tool_use_id,
        std::string_view output_preview,
        std::string_view session_id)
    {
        HookContext ctx{
            .event_name = "PostToolUse",
            .tool_name = std::string(tool_name),
            .tool_input_json = {},
            .tool_output_preview = std::string(output_preview),
            .tool_use_id = std::string(tool_use_id),
            .session_id = std::string(session_id),
            .working_directory = std::filesystem::current_path().string(),
            .extra_env = {}
        };
        return execute_event(HookEventType::PostToolUse, tool_name, ctx);
    }

    /// Execute hooks for a Notification event.
    [[nodiscard]] HookExecutionSummary execute_notification(
        std::string_view session_id)
    {
        HookContext ctx{
            .event_name = "Notification",
            .tool_name = {},
            .tool_input_json = {},
            .tool_output_preview = {},
            .tool_use_id = {},
            .session_id = std::string(session_id),
            .working_directory = std::filesystem::current_path().string(),
            .extra_env = {}
        };
        return execute_event(HookEventType::Notification, "", ctx);
    }

    /// Execute hooks for a Stop event.
    [[nodiscard]] HookExecutionSummary execute_stop(
        std::string_view session_id)
    {
        HookContext ctx{
            .event_name = "Stop",
            .tool_name = {},
            .tool_input_json = {},
            .tool_output_preview = {},
            .tool_use_id = {},
            .session_id = std::string(session_id),
            .working_directory = std::filesystem::current_path().string(),
            .extra_env = {}
        };
        return execute_event(HookEventType::Stop, "", ctx);
    }

    /// Execute hooks for a UserPromptSubmit event.
    [[nodiscard]] HookExecutionSummary execute_user_prompt_submit(
        std::string_view prompt_text,
        std::string_view session_id)
    {
        HookContext ctx{
            .event_name = "UserPromptSubmit",
            .tool_name = {},
            .tool_input_json = {},
            .tool_output_preview = {},
            .tool_use_id = {},
            .session_id = std::string(session_id),
            .working_directory = std::filesystem::current_path().string(),
            .extra_env = {}
        };
        ctx.extra_env["CLAUDE_HOOK_PROMPT"] = std::string(prompt_text);
        return execute_event(HookEventType::UserPromptSubmit, "", ctx);
    }

    /// Generic hook execution for any event type
    [[nodiscard]] HookExecutionSummary execute_event(
        HookEventType event,
        std::string_view match_value,
        const HookContext& context)
    {
        HookExecutionSummary summary;
        summary.event = event;

        auto matchers = loader_.get_hooks_for_event(event);
        if (matchers.empty()) {
            summary.all_passed = true;
            return summary;
        }

        // Find matching matchers
        for (const auto& matcher : matchers) {
            std::optional<std::string_view> pattern;
            if (matcher.matcher.has_value()) {
                pattern = std::string_view(*matcher.matcher);
            }

            if (!detail::matches_pattern(pattern, match_value)) {
                continue;
            }

            // Execute each hook command in the matcher
            for (const auto& hook_cmd : matcher.hooks) {
                // Only handle CommandHookConfig (shell commands)
                if (auto* cmd_cfg = std::get_if<CommandHookConfig>(&hook_cmd)) {
                    auto timeout_secs = cmd_cfg->timeout_seconds.value_or(kDefaultHookTimeoutSeconds);

                    auto result = detail::execute_command(
                        cmd_cfg->command,
                        cmd_cfg->shell,
                        context,
                        std::chrono::seconds(timeout_secs));

                    result.hook_name = matcher.matcher.value_or("*");

                    if (result.exit_code != 0) {
                        summary.all_passed = false;
                        summary.any_denied = true;
                        if (!summary.deny_reason.has_value()) {
                            // Use stderr or stdout as deny reason
                            if (!result.stderr_output.empty()) {
                                summary.deny_reason = result.stderr_output;
                            } else if (!result.stdout_output.empty()) {
                                summary.deny_reason = result.stdout_output;
                            } else if (result.timed_out) {
                                summary.deny_reason = std::format(
                                    "Hook '{}' timed out after {}s",
                                    cmd_cfg->command, timeout_secs);
                            } else {
                                summary.deny_reason = std::format(
                                    "Hook '{}' exited with code {}",
                                    cmd_cfg->command, result.exit_code);
                            }
                        }
                    }

                    summary.results.push_back(std::move(result));
                }
            }
        }

        return summary;
    }

    /// Set an event handler that receives hook execution lifecycle events
    void set_event_handler(HookEventHandler handler) {
        std::lock_guard lock(mu_);
        event_handler_ = std::move(handler);
    }

private:
    ShellHookLoader& loader_;
    mutable std::mutex mu_;
    HookEventHandler event_handler_;
};

// ============================================================================
// ShellHookManager — top-level integration point
// ============================================================================

/// Singleton manager that integrates hook loading, execution, and lifecycle events.
/// Wire this into the LifecycleHookRegistry to execute user-defined shell hooks
/// on each lifecycle event.
class ShellHookManager {
public:
    ShellHookManager()
        : executor_(loader_) {}

    /// Initialize: load hooks from settings and register with lifecycle registry.
    void initialize() {
        loader_.load_from_settings();
    }

    /// Reload hooks from disk (e.g., after config change).
    void reload() {
        loader_.clear();
        loader_.load_from_settings();
    }

    /// Access the loader for direct manipulation
    [[nodiscard]] ShellHookLoader& loader() noexcept { return loader_; }
    [[nodiscard]] const ShellHookLoader& loader() const noexcept { return loader_; }

    /// Access the executor
    [[nodiscard]] ShellHookExecutor& executor() noexcept { return executor_; }
    [[nodiscard]] const ShellHookExecutor& executor() const noexcept { return executor_; }

    /// Convenience: run pre-tool-use hooks and return whether the tool is allowed.
    /// Returns true if tool use is permitted (all hooks pass or no hooks configured).
    [[nodiscard]] bool check_pre_tool_use(
        std::string_view tool_name,
        std::string_view tool_input_json,
        std::string_view tool_use_id,
        std::string_view session_id)
    {
        if (!loader_.has_hooks_for_event(HookEventType::PreToolUse)) {
            return true;
        }
        auto summary = executor_.execute_pre_tool_use(
            tool_name, tool_input_json, tool_use_id, session_id);
        if (summary.any_denied) {
            last_deny_reason_ = summary.deny_reason;
            return false;
        }
        last_deny_reason_.reset();
        return true;
    }

    /// Convenience: run post-tool-use hooks (informational, no deny).
    void notify_post_tool_use(
        std::string_view tool_name,
        std::string_view tool_use_id,
        std::string_view output_preview,
        std::string_view session_id)
    {
        if (!loader_.has_hooks_for_event(HookEventType::PostToolUse)) {
            return;
        }
        (void)executor_.execute_post_tool_use(tool_name, tool_use_id, output_preview, session_id);
    }

    /// Get the deny reason from the last pre_tool_use check (if denied)
    [[nodiscard]] std::optional<std::string> last_deny_reason() const {
        return last_deny_reason_;
    }

    /// Execute hooks for an arbitrary event, returning the full summary
    [[nodiscard]] HookExecutionSummary execute(
        HookEventType event,
        std::string_view match_value,
        const HookContext& context)
    {
        return executor_.execute_event(event, match_value, context);
    }

    /// Set the session ID used for hook execution context
    void set_session_id(std::string session_id) {
        session_id_ = std::move(session_id);
    }

    /// Get the current session ID
    [[nodiscard]] const std::string& session_id() const noexcept { return session_id_; }

    /// Wire this manager into a LifecycleHookRegistry so shell hooks run automatically.
    /// Registers pre-tool-use as a blocking checker, post-tool-use as a notification.
    void wire_to_lifecycle(cc::hooks::LifecycleHookRegistry& registry) {
        // Pre-tool-use checker (can block execution)
        registry.add_pre_tool_use_checker([this](const cc::hooks::PreToolUseEvent& event)
            -> std::optional<std::string>
        {
            if (!check_pre_tool_use(
                    event.tool_name,
                    event.tool_input_json,
                    event.tool_use_id,
                    session_id_)) {
                return last_deny_reason_;
            }
            return std::nullopt;
        });

        // Post-tool-use notification
        registry.on_post_tool_use([this](const cc::hooks::PostToolUseEvent& event) {
            notify_post_tool_use(
                event.tool_name,
                event.tool_use_id,
                event.output_preview,
                session_id_);
        });

        // Stop hook checker
        registry.add_stop_hook([this]() -> std::optional<std::string> {
            if (!loader_.has_hooks_for_event(HookEventType::Stop)) {
                return std::nullopt;
            }
            HookContext ctx{
                .event_name = "Stop",
                .tool_name = {},
                .tool_input_json = {},
                .tool_output_preview = {},
                .tool_use_id = {},
                .session_id = session_id_,
                .working_directory = std::filesystem::current_path().string(),
                .extra_env = {}
            };
            auto summary = executor_.execute_event(HookEventType::Stop, "", ctx);
            if (summary.any_denied) {
                return summary.deny_reason;
            }
            return std::nullopt;
        });
    }

private:
    ShellHookLoader loader_;
    ShellHookExecutor executor_;
    std::optional<std::string> last_deny_reason_;
    std::string session_id_;
};

} // namespace cc::hooks::shell
