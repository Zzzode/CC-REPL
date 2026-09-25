// swarm_backends_detail.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). The exported detail helpers: JSON escaping and
// timestamps, the shutdown-request envelope, shell quoting, TmuxArgv
// rendering, the six pure tmux argv builders, the external-swarm re-attach
// policy, the capture-pane command builder, the teammate-command env
// resolution, the full teammate CLI command line, and the forced-mode env
// parser. Every string shape here is frozen (mailbox envelope, tmux/it2
// tokens, CLI flags, env var names) — bodies are byte-identical moves.
module;

module cc.utils.swarm_backends;

import std;

namespace cc::utils::swarm_backends::detail {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += R"(\\)"; break;
            case '"': out += R"(\")"; break;
            case '\n': out += R"(\n)"; break;
            case '\r': out += R"(\r)"; break;
            case '\t': out += R"(\t)"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(millis);
}

TeammateMessage shutdown_request_message(
    std::string_view agent_id,
    std::optional<std::string_view> reason
) {
    const auto timestamp = timestamp_now();
    std::string text = R"({"type":"shutdown_request","requestId":"shutdown-)";
    text += json_escape(agent_id);
    text += '-';
    text += timestamp;
    text += R"(","from":"team-lead")";
    if (reason && !reason->empty()) {
        text += R"(,"reason":")";
        text += json_escape(*reason);
        text += '"';
    }
    text += R"(,"timestamp":")";
    text += timestamp;
    text += R"("})";
    return TeammateMessage{
        .text = std::move(text),
        .from = "team-lead",
        .color = std::nullopt,
        .timestamp = timestamp,
        .summary = std::nullopt,
    };
}

std::string shell_quote(std::string_view value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

std::string TmuxArgv::join_shell() const {
    std::string command = program;
    for (const auto& arg : args) {
        command.push_back(' ');
        command += shell_quote(arg);
    }
    return command;
}

TmuxArgv tmux_has_session_argv(std::string_view session) {
    TmuxArgv argv;
    argv.args = {"has-session", "-t", std::string(session)};
    return argv;
}

TmuxArgv tmux_list_windows_argv(std::string_view session) {
    TmuxArgv argv;
    argv.args = {"list-windows", "-t", std::string(session),
                 "-F", "#{window_name}"};
    return argv;
}

TmuxArgv tmux_new_session_argv(
    std::string_view session, std::string_view window
) {
    TmuxArgv argv;
    argv.args = {"new-session", "-d", "-s", std::string(session),
                 "-n", std::string(window), "-P", "-F", "#{pane_id}"};
    return argv;
}

TmuxArgv tmux_new_window_argv(
    std::string_view session, std::string_view window
) {
    TmuxArgv argv;
    argv.args = {"new-window", "-t", std::string(session),
                 "-n", std::string(window), "-P", "-F", "#{pane_id}"};
    return argv;
}

TmuxArgv tmux_list_panes_argv(std::string_view target) {
    TmuxArgv argv;
    argv.args = {"list-panes", "-t", std::string(target),
                 "-F", "#{pane_id}"};
    return argv;
}

TmuxArgv tmux_split_window_argv(
    std::string_view target, bool vertical
) {
    TmuxArgv argv;
    argv.args = {"split-window", "-t", std::string(target),
                 vertical ? "-v" : "-h", "-P", "-F", "#{pane_id}"};
    return argv;
}

ExternalSessionPlan plan_external_swarm_view(
    bool session_exists,
    bool window_exists,
    std::size_t pane_count,
    bool first_pane_used
) {
    ExternalSessionPlan plan;
    plan.window_target =
        std::string(SWARM_SESSION_NAME) + ":" + std::string(SWARM_VIEW_WINDOW_NAME);
    if (!session_exists) {
        plan.action = ExternalSessionAction::CreateSession;
        plan.reuse_first_pane = !first_pane_used;
        return plan;
    }
    if (!window_exists) {
        plan.action = ExternalSessionAction::CreateWindow;
        plan.reuse_first_pane = !first_pane_used;
        return plan;
    }
    plan.action = ExternalSessionAction::ReuseExistingWindow;
    plan.reuse_first_pane = !first_pane_used && pane_count == 1;
    return plan;
}

std::string build_capture_pane_command(
    std::string_view pane_id, int tail_lines
) {
    if (tail_lines < 1) tail_lines = 1;
    return "tmux capture-pane -p -t " + shell_quote(pane_id) +
           " -S -" + std::to_string(tail_lines) + " 2>/dev/null";
}

std::string teammate_command() {
    if (const char* env = std::getenv("LOOM_TEAMMATE_COMMAND"); env && *env) return std::string(env);
    if (const char* env = std::getenv("CLAUDE_CODE_TEAMMATE_COMMAND"); env && *env) return std::string(env);
    if (const char* env = std::getenv("LOOM_BINARY"); env && *env) return std::string(env);
    return "loom";
}

std::string build_teammate_cli_command(const TeammateSpawnConfig& config) {
    std::ostringstream command;
    if (!config.cwd.empty()) command << "cd " << shell_quote(config.cwd) << " && ";
    command << "env LOOM=1 LOOM_EXPERIMENTAL_AGENT_TEAMS=1 ";
    command << shell_quote(teammate_command());
    command << " --agent-id " << shell_quote(format_agent_id(config.name, config.team_name));
    command << " --agent-name " << shell_quote(config.name);
    command << " --team-name " << shell_quote(config.team_name);
    if (config.color) command << " --agent-color " << shell_quote(std::string(agent_color_name(*config.color)));
    if (!config.parent_session_id.empty()) command << " --parent-session-id " << shell_quote(config.parent_session_id);
    if (config.plan_mode_required) command << " --plan-mode-required";
    if (config.agent_type && !config.agent_type->empty()) command << " --agent-type " << shell_quote(*config.agent_type);
    if (!config.plan_mode_required && config.permission_mode) {
        if (*config.permission_mode == "bypassPermissions") {
            command << " --dangerously-skip-permissions";
        } else if (*config.permission_mode == "acceptEdits" || *config.permission_mode == "auto") {
            command << " --permission-mode " << shell_quote(*config.permission_mode);
        }
    }
    if (config.model && !config.model->empty()) command << " --model " << shell_quote(*config.model);
    return command.str();
}

std::optional<TeammateMode> forced_mode_from_env() {
    const char* raw = std::getenv("LOOM_TEAMMATE_BACKEND");
    if (!raw || !*raw) raw = std::getenv("CLAUDE_CODE_TEAMMATE_BACKEND");
    if (!raw || !*raw) return std::nullopt;
    std::string value(raw);
    if (value == "in-process" || value == "inprocess" || value == "native") return TeammateMode::InProcess;
    if (value == "tmux" || value == "pane") return TeammateMode::Tmux;
    return std::nullopt;
}

} // namespace cc::utils::swarm_backends::detail
