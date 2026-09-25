// swarm_backends_tmux.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). All non-inline TmuxBackend members:
// is_available (the out-of-line key function — its body is strong here;
// under clang named modules the class vtable/typeinfo is emitted strong in
// the interface unit swarm_backends.cppm.o), pane
// create/send/title/color/kill/capture/hide/show, the with-leader and
// external-session creation flows, rebalancing, and the tmux color table.
// The verified-dead get_current_pane_id / get_current_window_target were
// removed entirely (declaration and definition).
module;

module cc.utils.swarm_backends;

import std;

namespace cc::utils::swarm_backends {

bool TmuxBackend::is_available() const {
    return EnvironmentDetection::is_tmux_available();
}

bool TmuxBackend::is_running_inside() const {
    return EnvironmentDetection::is_inside_tmux();
}

CreatePaneResult TmuxBackend::create_teammate_pane(std::string_view name, AgentColor color) {
    std::lock_guard lock(pane_creation_mutex_);
    auto result = is_running_inside()
        ? create_pane_with_leader(name, color)
        : create_pane_external(name, color);
    if (!result.pane_id.empty()) {
        set_pane_title(result.pane_id, name, color, !is_running_inside());
        set_pane_border_color(result.pane_id, color, !is_running_inside());
    }
    return result;
}

void TmuxBackend::send_command_to_pane(
    const PaneId& pane_id,
    std::string_view command,
    bool use_external_session
) {
    (void)use_external_session;
    if (pane_id.empty() || command.empty()) return;
    auto cmd = "tmux send-keys -t " + detail::shell_quote(pane_id) + " " +
        detail::shell_quote(command) + " Enter >/dev/null 2>&1";
    (void)detail::run_shell(cmd);
}

void TmuxBackend::set_pane_border_color(
    const PaneId& pane_id,
    AgentColor color,
    bool use_external_session
) {
    (void)use_external_session;
    if (pane_id.empty()) return;
    auto cmd = "tmux select-pane -t " + detail::shell_quote(pane_id) +
        " -P '" + std::string(get_tmux_color(color)) + "' >/dev/null 2>&1";
    (void)detail::run_shell(cmd);
}

void TmuxBackend::set_pane_title(
    const PaneId& pane_id,
    std::string_view name,
    AgentColor color,
    bool use_external_session
) {
    (void)color;
    (void)use_external_session;
    if (pane_id.empty()) return;
    auto cmd = "tmux select-pane -t " + detail::shell_quote(pane_id) +
        " -T " + detail::shell_quote(name) + " >/dev/null 2>&1";
    (void)detail::run_shell(cmd);
}

void TmuxBackend::enable_pane_border_status(
    std::optional<std::string_view> window_target,
    bool use_external_session
) {
    (void)use_external_session;
    std::string target = window_target ? " -t " + detail::shell_quote(*window_target) : "";
    (void)detail::run_shell("tmux set-option" + target + " pane-border-status top >/dev/null 2>&1");
}

void TmuxBackend::rebalance_panes(std::string_view window_target, bool has_leader) {
    if (has_leader) {
        rebalance_with_leader(window_target);
    } else {
        rebalance_tiled(window_target);
    }
}

bool TmuxBackend::kill_pane(const PaneId& pane_id, bool use_external_session) {
    (void)use_external_session;
    if (pane_id.empty()) return false;
    auto cmd = "tmux kill-pane -t " + detail::shell_quote(pane_id) + " >/dev/null 2>&1";
    return detail::run_shell(cmd) == 0;
}

std::optional<std::string> TmuxBackend::capture_pane_text(
    const PaneId& pane_id, int tail_lines, bool use_external_session
) {
    // TODO: external swarm sessions run on a -L loom-swarm-<pid> socket in TS
    // (constants.ts getSwarmSocketName); the C++ port currently shells out on
    // the default socket everywhere (see create_pane_external), so honor the
    // same simplification here until the socket gap is ported.
    (void)use_external_session;
    if (pane_id.empty()) return std::nullopt;
    auto result = detail::read_shell_output_with_status(
        detail::build_capture_pane_command(pane_id, tail_lines));
    if (result.exit_code != 0) return std::nullopt;  // pane is gone
    return result.text;
}

bool TmuxBackend::hide_pane(const PaneId& pane_id, bool use_external_session) {
    (void)use_external_session;
    if (pane_id.empty()) return false;
    auto cmd = "tmux break-pane -d -s " + detail::shell_quote(pane_id) +
        " -t " + detail::shell_quote(HIDDEN_SESSION_NAME) + " >/dev/null 2>&1";
    return detail::run_shell(cmd) == 0;
}

bool TmuxBackend::show_pane(
    const PaneId& pane_id,
    std::string_view target_window_or_pane,
    bool use_external_session
) {
    (void)use_external_session;
    if (pane_id.empty()) return false;
    auto cmd = "tmux join-pane -s " + detail::shell_quote(pane_id) +
        " -t " + detail::shell_quote(target_window_or_pane) + " >/dev/null 2>&1";
    return detail::run_shell(cmd) == 0;
}

CreatePaneResult TmuxBackend::create_pane_with_leader(std::string_view name, AgentColor color) {
    (void)name;
    (void)color;
    // Leader-attached balanced layout (TS TmuxBackend.createTeammatePane):
    //  - first teammate: split the leader pane horizontally, leader keeps 70%
    //  - further teammates: split from a middle teammate pane, alternating
    //    vertical/horizontal by parity so the grid stays balanced.
    const auto leader_pane =
        detail::read_shell_output("tmux display-message -p '#{pane_id}' 2>/dev/null");
    if (leader_pane.empty()) return {};

    const auto window_target =
        detail::read_shell_output("tmux display-message -p '#{session_name}:#{window_id}' 2>/dev/null");

    auto split = [&](std::string target, const std::string& flag,
                     bool with_size) {
        std::string cmd = "tmux split-window -t " + detail::shell_quote(target) +
                          " " + flag + " -P -F '#{pane_id}'";
        if (with_size) cmd += " -l 70%";
        cmd += " 2>/dev/null";
        return detail::read_shell_output(cmd);
    };

    // Panes in the current window; pane 0 is the leader, the rest are
    // teammates already spawned.
    auto list_raw = window_target.empty()
        ? detail::read_shell_output("tmux list-panes -F '#{pane_id}' 2>/dev/null")
        : detail::read_shell_output("tmux list-panes -t " +
                                    detail::shell_quote(window_target) +
                                    " -F '#{pane_id}' 2>/dev/null");
    std::vector<std::string> panes;
    {
        std::string acc;
        for (char c : list_raw) {
            if (c == '\n') {
                if (!acc.empty()) { panes.push_back(acc); acc.clear(); }
            } else acc += c;
        }
        if (!acc.empty()) panes.push_back(acc);
    }

    std::string pane;
    if (panes.size() <= 1) {
        // First teammate: 70% horizontal split off the leader.
        pane = split(leader_pane, "-h", /*with_size=*/true);
    } else {
        const std::size_t teammate_count = panes.size() - 1;  // exclude leader
        const bool split_vertical = (teammate_count % 2 == 1);
        const std::size_t target_index =
            std::min(static_cast<std::size_t>((teammate_count - 1) / 2),
                     panes.size() - 1);
        pane = split(panes[target_index], split_vertical ? "-v" : "-h",
                     /*with_size=*/false);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kPaneShellInitDelayMs));
    return CreatePaneResult{.pane_id = pane, .is_first_teammate = panes.size() <= 1};
}

CreatePaneResult TmuxBackend::create_pane_external(std::string_view name, AgentColor color) {
    (void)name;
    (void)color;
    const std::string session{SWARM_SESSION_NAME};
    const std::string window{SWARM_VIEW_WINDOW_NAME};
    const std::string window_target = session + ":" + window;

    // Split newline-delimited tmux output into non-empty lines (matches the
    // TS '.trim().split("\n").filter(Boolean)' idiom).
    auto split_lines = [](std::string_view raw) {
        std::vector<std::string> lines;
        std::string acc;
        for (char c : raw) {
            if (c == '\n') {
                if (!acc.empty() && acc != "\r") lines.push_back(acc);
                acc.clear();
            } else if (c != '\r') {
                acc.push_back(c);
            }
        }
        if (!acc.empty()) lines.push_back(acc);
        return lines;
    };

    // 1. Probe the external swarm session (TS hasSessionInSwarm).
    const bool session_exists =
        detail::run_shell(detail::tmux_has_session_argv(session).join_shell()) == 0;

    // 2. Probe the swarm-view window and count its live panes.
    bool window_exists = false;
    std::size_t pane_count = 0;
    std::vector<std::string> existing_panes;
    if (session_exists) {
        const auto windows = split_lines(detail::read_shell_output(
            detail::tmux_list_windows_argv(session).join_shell()));
        for (const auto& line : windows) {
            if (line == window) window_exists = true;
        }
        if (window_exists) {
            existing_panes = split_lines(detail::read_shell_output(
                detail::tmux_list_panes_argv(window_target).join_shell()));
            pane_count = existing_panes.size();
        }
    }

    // 3. Pure re-attach decision.
    const auto plan = detail::plan_external_swarm_view(
        session_exists, window_exists, pane_count, first_pane_used_external_);

    // 4. Create the missing session/window; both print the fresh pane id via
    // -P -F '#{pane_id}'. Reusing an existing window starts from pane 0.
    std::string pane;
    if (plan.action == detail::ExternalSessionAction::CreateSession) {
        pane = detail::read_shell_output(
            detail::tmux_new_session_argv(session, window).join_shell());
        pane_count = 1;
    } else if (plan.action == detail::ExternalSessionAction::CreateWindow) {
        pane = detail::read_shell_output(
            detail::tmux_new_window_argv(session, window).join_shell());
        pane_count = 1;
    } else if (!existing_panes.empty()) {
        pane = existing_panes.front();
    }
    if (pane.empty()) return {};

    // 5. First teammate takes the lone pane; further teammates split from a
    // middle pane. Unlike the previous one-shot flag, pane_count is derived
    // from live tmux state, so a leader restart splits instead of hijacking
    // pane 0 when teammate panes already exist (TS guards on paneCount===1).
    const bool first = plan.reuse_first_pane;
    if (first) {
        first_pane_used_external_ = true;
    } else {
        // External windows have no leader pane: every live pane is a teammate,
        // so index against the full pane list (TS createTeammatePaneExternal).
        auto panes = split_lines(detail::read_shell_output(
            detail::tmux_list_panes_argv(window_target).join_shell()));
        if (panes.empty()) return {};
        pane_count = panes.size();
        const bool vertical = (pane_count % 2 == 1);
        const std::size_t target_index =
            std::min((pane_count - 1) / 2, pane_count - 1);
        pane = detail::read_shell_output(
            detail::tmux_split_window_argv(panes[target_index], vertical).join_shell());
        if (pane.empty()) return {};
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kPaneShellInitDelayMs));
    return CreatePaneResult{.pane_id = pane, .is_first_teammate = first};
}

void TmuxBackend::rebalance_with_leader(std::string_view window_target) {
    auto cmd = "tmux select-layout -t " + detail::shell_quote(window_target) + " even-horizontal >/dev/null 2>&1";
    (void)detail::run_shell(cmd);
}

void TmuxBackend::rebalance_tiled(std::string_view window_target) {
    auto cmd = "tmux select-layout -t " + detail::shell_quote(window_target) + " tiled >/dev/null 2>&1";
    (void)detail::run_shell(cmd);
}

std::string_view TmuxBackend::get_tmux_color(AgentColor color) {
    switch (color) {
        case AgentColor::Red: return "red";
        case AgentColor::Blue: return "blue";
        case AgentColor::Green: return "green";
        case AgentColor::Yellow: return "yellow";
        case AgentColor::Purple: return "magenta";
        case AgentColor::Orange: return "colour208";
        case AgentColor::Pink: return "colour205";
        case AgentColor::Cyan: return "cyan";
    }
    return "white";
}

} // namespace cc::utils::swarm_backends
