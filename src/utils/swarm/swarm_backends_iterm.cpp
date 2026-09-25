// swarm_backends_iterm.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). All non-inline ITermBackend members:
// is_available (the out-of-line key function — its body is strong here;
// under clang named modules the class vtable/typeinfo is emitted strong in
// the interface unit swarm_backends.cppm.o), pane
// create/send/close via the it2 CLI, the no-op style/layout methods, and the
// unsupported capture/hide/show stubs. The verified-dead
// get_leader_session_id was removed entirely (declaration and definition).
module;

module cc.utils.swarm_backends;

import std;

namespace cc::utils::swarm_backends {

bool ITermBackend::is_available() const {
    return EnvironmentDetection::is_in_iterm2() && EnvironmentDetection::is_it2_cli_available();
}

bool ITermBackend::is_running_inside() const {
    return EnvironmentDetection::is_in_iterm2();
}

CreatePaneResult ITermBackend::create_teammate_pane(std::string_view name, AgentColor color) {
    (void)name;
    (void)color;
    std::lock_guard lock(pane_creation_mutex_);
    auto output = detail::read_shell_output("it2 session split right 2>/dev/null");
    auto pane = parse_split_output(output);
    if (!pane.empty()) teammate_session_ids_.push_back(pane);
    const bool is_first = !first_pane_used_;
    if (!pane.empty()) first_pane_used_ = true;
    return CreatePaneResult{.pane_id = pane, .is_first_teammate = is_first};
}

void ITermBackend::send_command_to_pane(
    const PaneId& pane_id,
    std::string_view command,
    bool use_external_session
) {
    (void)use_external_session;
    if (pane_id.empty() || command.empty()) return;
    auto cmd = "it2 session send-text -s " + detail::shell_quote(pane_id) + " " +
        detail::shell_quote(std::string(command) + "\n") + " >/dev/null 2>&1";
    (void)detail::run_shell(cmd);
}

void ITermBackend::set_pane_border_color(const PaneId&, AgentColor, bool) {}
void ITermBackend::set_pane_title(const PaneId&, std::string_view, AgentColor, bool) {}
void ITermBackend::enable_pane_border_status(std::optional<std::string_view>, bool) {}
void ITermBackend::rebalance_panes(std::string_view, bool) {}

bool ITermBackend::kill_pane(const PaneId& pane_id, bool use_external_session) {
    (void)use_external_session;
    if (pane_id.empty()) return false;
    auto cmd = "it2 session close -s " + detail::shell_quote(pane_id) + " >/dev/null 2>&1";
    return detail::run_shell(cmd) == 0;
}

std::optional<std::string> ITermBackend::capture_pane_text(
    const PaneId&, int, bool
) {
    // TODO: iTerm2 has no capture-pane; explore 'it2 session get-screen' later.
    return std::nullopt;
}

bool ITermBackend::hide_pane(const PaneId&, bool) { return false; }
bool ITermBackend::show_pane(const PaneId&, std::string_view, bool) { return false; }

std::string ITermBackend::parse_split_output(std::string_view output) {
    std::string text(output);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

} // namespace cc::utils::swarm_backends
