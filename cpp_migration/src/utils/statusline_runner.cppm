/// @file statusline_runner.cppm
/// @brief StatusLine command execution — faithful port of TS StatusLine.tsx
///        executeStatusLineCommand() + buildStatusLineCommandInput().
///
/// The statusline is a user-configurable shell command that receives a
/// JSON blob on stdin (StatusLineCommandInput) and prints text (optionally
/// with ANSI color codes) to stdout.  The output is rendered in the footer
/// area on the left side.
///
/// TS REFERENCE:
///   - src/components/StatusLine.tsx  (buildStatusLineCommandInput + rendering)
///   - src/utils/hooks.ts             (executeStatusLineCommand)
///   - src/types/statusLine.d.ts      (StatusLineCommandInput type)
///
/// KEY BEHAVIOR (matching TS):
///   - JSON input is written to stdin followed by a newline
///   - Command runs via bash -c (user's shell command)
///   - Default timeout: 5000 ms (short — statusline must be snappy)
///   - Only stdout is used (stderr is ignored for display)
///   - Exit code != 0 → empty result (no error shown to user)
///   - Empty output → no display
///   - Output is trimmed; blank lines are removed
///
/// MODULE:   cc.utils.statusline_runner
/// LICENCE:  Exported.  Imported by app.cppm (AppAdapter).
/// =========================================================================

module;

#include <string>
#include <string_view>
#include <optional>
#include <chrono>
#include <format>
#include <vector>

export module cc.utils.statusline_runner;

import cc.utils.json;
import cc.utils.hooks_execution;

export namespace cc::utils::statusline {

using namespace std::chrono;
namespace hooks_ns = cc::utils::hooks_execution;
namespace json_ns  = cc::utils::json;

// =========================================================================
// StatusLineCommandInput (mirrors TS type)
// =========================================================================
// Rich context passed to the statusline command as JSON on stdin.
// Only the fields the C++ engine can populate are included; the rest are
// omitted (missing optional fields are simply absent from the JSON object,
// matching TS semantics where undefined fields are not serialized).

struct StatusLineModelInfo {
    std::string id;
    std::string display_name;
};

struct StatusLineWorkspaceInfo {
    std::string current_dir;
    std::string project_dir;
    std::vector<std::string> added_dirs;
};

struct StatusLineCostInfo {
    double total_cost_usd = 0.0;
    std::int64_t total_duration_ms = 0;
    std::int64_t total_api_duration_ms = 0;
    std::int64_t total_lines_added = 0;
    std::int64_t total_lines_removed = 0;
};

struct StatusLineContextWindowInfo {
    std::int64_t total_input_tokens = 0;
    std::int64_t total_output_tokens = 0;
    std::int64_t context_window_size = 0;
    std::int64_t current_usage = 0;
    double used_percentage = 0.0;
    double remaining_percentage = 0.0;
};

struct StatusLineVimInfo {
    std::string mode;   // "INSERT", "NORMAL", "VISUAL", etc.
};

struct StatusLineAgentInfo {
    std::string name;
};

struct StatusLineRemoteInfo {
    std::string session_id;
};

struct StatusLineWorktreeInfo {
    std::string name;
    std::string path;
    std::string branch;
    std::string original_cwd;
    std::string original_branch;
};

struct StatusLineRateLimitBucket {
    double used_percentage = 0.0;
    std::int64_t resets_at = 0;  // unix timestamp ms
};

struct StatusLineRateLimits {
    std::optional<StatusLineRateLimitBucket> five_hour;
    std::optional<StatusLineRateLimitBucket> seven_day;
};

/// Full input payload sent to the statusline command.
/// Mirrors TS StatusLineCommandInput — all fields are optional-ish; only
/// populated fields appear in the serialized JSON.
struct StatusLineCommandInput {
    // Base hook input fields (TS: createBaseHookInput)
    std::optional<std::string> session_name;

    // Model info
    StatusLineModelInfo model;

    // Workspace
    StatusLineWorkspaceInfo workspace;

    // Version
    std::string version;

    // Output style
    std::string output_style_name;

    // Cost / usage
    StatusLineCostInfo cost;

    // Context window
    StatusLineContextWindowInfo context_window;

    // 200k token threshold flag
    bool exceeds_200k_tokens = false;

    // Rate limits (optional)
    std::optional<StatusLineRateLimits> rate_limits;

    // Vim (optional — only when vim enabled)
    std::optional<StatusLineVimInfo> vim;

    // Agent (optional)
    std::optional<StatusLineAgentInfo> agent;

    // Remote (optional)
    std::optional<StatusLineRemoteInfo> remote;

    // Worktree (optional)
    std::optional<StatusLineWorktreeInfo> worktree;
};

// =========================================================================
// JSON serialization
// =========================================================================

/// Serialize a StatusLineCommandInput to a JSON string.
/// Faithful to TS: only populated optional fields are included.
[[nodiscard]] inline std::string to_json(const StatusLineCommandInput& input) {
    json_ns::JsonMutDoc doc;
    auto root = doc.object();
    doc.set_root(root);

    // session_name (optional)
    if (input.session_name && !input.session_name->empty()) {
        root.add("session_name", doc.string(*input.session_name));
    }

    // model
    {
        auto m = doc.object();
        m.add("id", doc.string(input.model.id));
        m.add("display_name", doc.string(input.model.display_name));
        root.add("model", std::move(m));
    }

    // workspace
    {
        auto w = doc.object();
        w.add("current_dir", doc.string(input.workspace.current_dir));
        w.add("project_dir", doc.string(input.workspace.project_dir));
        auto added = doc.array();
        for (const auto& d : input.workspace.added_dirs) {
            added.append(doc.string(d));
        }
        w.add("added_dirs", std::move(added));
        root.add("workspace", std::move(w));
    }

    // version
    root.add("version", doc.string(input.version));

    // output_style
    {
        auto os = doc.object();
        os.add("name", doc.string(input.output_style_name));
        root.add("output_style", std::move(os));
    }

    // cost
    {
        auto c = doc.object();
        c.add("total_cost_usd", doc.number(input.cost.total_cost_usd));
        c.add("total_duration_ms", doc.number(input.cost.total_duration_ms));
        c.add("total_api_duration_ms", doc.number(input.cost.total_api_duration_ms));
        c.add("total_lines_added", doc.number(input.cost.total_lines_added));
        c.add("total_lines_removed", doc.number(input.cost.total_lines_removed));
        root.add("cost", std::move(c));
    }

    // context_window
    {
        auto cw = doc.object();
        cw.add("total_input_tokens", doc.number(input.context_window.total_input_tokens));
        cw.add("total_output_tokens", doc.number(input.context_window.total_output_tokens));
        cw.add("context_window_size", doc.number(input.context_window.context_window_size));
        cw.add("current_usage", doc.number(input.context_window.current_usage));
        cw.add("used_percentage", doc.number(input.context_window.used_percentage));
        cw.add("remaining_percentage", doc.number(input.context_window.remaining_percentage));
        root.add("context_window", std::move(cw));
    }

    // exceeds_200k_tokens
    root.add("exceeds_200k_tokens", doc.boolean(input.exceeds_200k_tokens));

    // rate_limits (optional)
    if (input.rate_limits &&
        (input.rate_limits->five_hour || input.rate_limits->seven_day))
    {
        auto rl = doc.object();
        if (input.rate_limits->five_hour) {
            auto fh = doc.object();
            fh.add("used_percentage", doc.number(input.rate_limits->five_hour->used_percentage));
            fh.add("resets_at", doc.number(input.rate_limits->five_hour->resets_at));
            rl.add("five_hour", std::move(fh));
        }
        if (input.rate_limits->seven_day) {
            auto sd = doc.object();
            sd.add("used_percentage", doc.number(input.rate_limits->seven_day->used_percentage));
            sd.add("resets_at", doc.number(input.rate_limits->seven_day->resets_at));
            rl.add("seven_day", std::move(sd));
        }
        root.add("rate_limits", std::move(rl));
    }

    // vim (optional)
    if (input.vim && !input.vim->mode.empty()) {
        auto v = doc.object();
        v.add("mode", doc.string(input.vim->mode));
        root.add("vim", std::move(v));
    }

    // agent (optional)
    if (input.agent && !input.agent->name.empty()) {
        auto a = doc.object();
        a.add("name", doc.string(input.agent->name));
        root.add("agent", std::move(a));
    }

    // remote (optional)
    if (input.remote && !input.remote->session_id.empty()) {
        auto r = doc.object();
        r.add("session_id", doc.string(input.remote->session_id));
        root.add("remote", std::move(r));
    }

    // worktree (optional)
    if (input.worktree && !input.worktree->name.empty()) {
        auto w = doc.object();
        w.add("name", doc.string(input.worktree->name));
        w.add("path", doc.string(input.worktree->path));
        w.add("branch", doc.string(input.worktree->branch));
        w.add("original_cwd", doc.string(input.worktree->original_cwd));
        w.add("original_branch", doc.string(input.worktree->original_branch));
        root.add("worktree", std::move(w));
    }

    return doc.to_string();
}

// =========================================================================
// Command execution
// =========================================================================

/// Result of executing a statusline command.
struct StatusLineResult {
    bool success = false;
    std::string output;      ///< Trimmed stdout (may contain ANSI codes)
    std::string error;       ///< Error message (on failure)
    milliseconds elapsed{0};
};

/// Execute a statusline shell command.
///
/// @param command  Shell command string (passed to bash -c)
/// @param input    StatusLineCommandInput data serialized as JSON on stdin
/// @param timeout_ms Timeout in milliseconds (default 5000, matching TS)
/// @returns StatusLineResult with output text on success.
///
/// Faithful to TS executeStatusLineCommand:
///   - Runs command via bash -c
///   - Writes JSON input to stdin
///   - Trims output and removes blank lines
///   - Non-zero exit → empty output (success=false)
///   - Timeout → empty output
[[nodiscard]] inline StatusLineResult execute_statusline_command(
    std::string_view command,
    const StatusLineCommandInput& input,
    int timeout_ms = 5000)
{
    StatusLineResult result;
    if (command.empty()) {
        result.error = "empty command";
        return result;
    }

    auto t0 = steady_clock::now();
    std::string json_input = to_json(input);

    // Run via bash -c, matching TS execCommandHook shell behavior.
    auto cmd_result = hooks_ns::CommandHookRunner::run_raw(
        "bash", {"-c", std::string(command)},
        timeout_ms, {},
        json_input);

    result.elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

    if (!cmd_result.executed) {
        result.error = cmd_result.error.empty() ? "command failed" : cmd_result.error;
        return result;
    }

    if (cmd_result.exit_code != 0) {
        result.error = "exit code " + std::to_string(cmd_result.exit_code);
        return result;
    }

    // Process output: trim, split by lines, skip blank lines, rejoin.
    // Mirrors TS:
    //   result.stdout.trim().split('\n').flatMap(line => line.trim() || []).join('\n')
    std::string out = cmd_result.stdout;

    // Trim leading whitespace
    std::size_t start = out.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        // All whitespace — empty output
        return result;  // success=false, output empty
    }

    // Trim trailing whitespace
    std::size_t end = out.find_last_not_of(" \t\r\n");
    std::string trimmed = out.substr(start, end - start + 1);

    // Split by newline, skip blank lines
    std::string processed;
    std::size_t pos = 0;
    bool first_line = true;
    while (pos <= trimmed.size()) {
        std::size_t nl = trimmed.find('\n', pos);
        std::string_view line;
        if (nl == std::string::npos) {
            line = std::string_view(trimmed).substr(pos);
        } else {
            line = std::string_view(trimmed).substr(pos, nl - pos);
        }

        // Trim the line
        std::size_t ls = line.find_first_not_of(" \t\r");
        if (ls != std::string_view::npos) {
            std::size_t le = line.find_last_not_of(" \t\r");
            std::string_view trimmed_line = line.substr(ls, le - ls + 1);
            if (!trimmed_line.empty()) {
                if (!first_line) processed.push_back('\n');
                processed += trimmed_line;
                first_line = false;
            }
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    if (processed.empty()) {
        return result;  // success=false, output empty
    }

    result.output = std::move(processed);
    result.success = true;
    return result;
}

} // namespace cc::utils::statusline
