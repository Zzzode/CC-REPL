// swarm_backends_executor.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). Cross-archive RAII guard and vtable anchors:
// PaneBackend's destructor (defaulted) and non-pure capture_pane_text default,
// TeammateExecutor's destructor (defaulted), and PaneBackendExecutor's
// destructor plus kill_all_spawned and every other out-of-line member.
// Each base destructor is its class's key function, making those bodies
// strong once in this object. Under clang named modules the exported
// classes' vtables/typeinfo are emitted strong in the INTERFACE unit
// (swarm_backends.cppm.o); every other TU binds them and these bodies as U.
module;

module cc.utils.swarm_backends;

import std;

namespace cc::utils::swarm_backends {

// ── Base anchors ────────────────────────────────────────────────────────────

PaneBackend::~PaneBackend() = default;

std::optional<std::string> PaneBackend::capture_pane_text(
    const PaneId& pane_id, int tail_lines, bool use_external_session
) {
    (void)pane_id;
    (void)tail_lines;
    (void)use_external_session;
    return std::nullopt;
}

TeammateExecutor::~TeammateExecutor() = default;

// ── PaneBackendExecutor ─────────────────────────────────────────────────────

PaneBackendExecutor::~PaneBackendExecutor() {
    kill_all_spawned();
}

void PaneBackendExecutor::kill_all_spawned() {
    std::map<std::string, SpawnedPane> spawned;
    {
        std::lock_guard lock(spawned_mutex_);
        spawned = spawned_teammates_;
        spawned_teammates_.clear();
    }
    for (const auto& [id, info] : spawned) {
        if (backend_) (void)backend_->kill_pane(info.pane_id, info.inside_tmux);
    }
}

TeammateSpawnResult PaneBackendExecutor::spawn(const TeammateSpawnConfig& config) {
    if (!backend_ || !backend_->is_available()) {
        return TeammateSpawnResult{
            .success = false,
            .agent_id = {},
            .error = "pane backend is not available",
            .task_id = std::nullopt,
            .pane_id = std::nullopt,
        };
    }
    auto color = config.color.value_or(AgentColor::Cyan);
    auto pane = backend_->create_teammate_pane(config.name, color);
    if (pane.pane_id.empty()) {
        return TeammateSpawnResult{
            .success = false,
            .agent_id = {},
            .error = "failed to create teammate pane",
            .task_id = std::nullopt,
            .pane_id = std::nullopt,
        };
    }
    auto agent_id = format_agent_id(config.name, config.team_name);
    auto inside = backend_->is_running_inside();
    backend_->send_command_to_pane(pane.pane_id, detail::build_teammate_cli_command(config), !inside);
    {
        std::lock_guard lock(spawned_mutex_);
        spawned_teammates_[agent_id] = SpawnedPane{.pane_id = pane.pane_id, .inside_tmux = inside};
    }
    // Deliver the initial task to the new pane's mailbox so the spawned
    // teammate has work the moment its inbox poller starts (TS writes the
    // initial prompt via writeToMailbox right after spawning).
    if (!config.prompt.empty()) {
        TeammateMessage initial{
            .text = config.prompt,
            .from = std::string("team-lead"),
            .color = std::nullopt,
            .timestamp = std::string(detail::timestamp_now()),
            .summary = std::nullopt,
        };
        (void)detail::write_backend_message_to_mailbox(agent_id, initial);
    }
    return TeammateSpawnResult{
        .success = true,
        .agent_id = agent_id,
        .error = std::nullopt,
        .task_id = std::nullopt,
        .pane_id = pane.pane_id,
    };
}

void PaneBackendExecutor::send_message(std::string_view agent_id, const TeammateMessage& message) {
    (void)detail::write_backend_message_to_mailbox(agent_id, message);
}

bool PaneBackendExecutor::terminate(std::string_view agent_id, std::optional<std::string_view> reason) {
    return detail::write_backend_message_to_mailbox(
        agent_id,
        detail::shutdown_request_message(agent_id, reason));
}

bool PaneBackendExecutor::kill(std::string_view agent_id) {
    std::lock_guard lock(spawned_mutex_);
    auto it = spawned_teammates_.find(std::string(agent_id));
    if (it == spawned_teammates_.end()) return false;
    auto pane_id = it->second.pane_id;
    auto inside = it->second.inside_tmux;
    spawned_teammates_.erase(it);
    return backend_->kill_pane(pane_id, !inside);
}

bool PaneBackendExecutor::is_active(std::string_view agent_id) const {
    std::lock_guard lock(spawned_mutex_);
    return spawned_teammates_.contains(std::string(agent_id));
}

std::optional<std::string> PaneBackendExecutor::capture_agent_pane(
    std::string_view agent_id, int tail_lines
) const {
    std::lock_guard lock(spawned_mutex_);
    auto it = spawned_teammates_.find(std::string(agent_id));
    if (it == spawned_teammates_.end()) return std::nullopt;
    return backend_->capture_pane_text(
        it->second.pane_id, tail_lines, !it->second.inside_tmux);
}

std::map<std::string, PaneBackendExecutor::SpawnedPane>
PaneBackendExecutor::spawned_panes() const {
    std::lock_guard lock(spawned_mutex_);
    return spawned_teammates_;
}

} // namespace cc::utils::swarm_backends
