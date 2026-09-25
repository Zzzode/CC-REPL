// swarm_backends_inprocess.cpp — implementation unit for cc.utils.swarm_backends
// (RFC 0001 Phase C batch 10). InProcessBackend's five out-of-line members
// (spawn is the out-of-line key function — its body is strong here; under
// clang named modules the class vtable/typeinfo is emitted strong in the
// interface unit swarm_backends.cppm.o) plus
// detail::write_backend_message_to_mailbox. This is the only swarm_backends
// unit that imports cc.utils.team_helpers, since mailbox I/O is the sole use.
module;

module cc.utils.swarm_backends;

import std;

import cc.utils.team_helpers;

namespace cc::utils::swarm_backends {

TeammateSpawnResult InProcessBackend::spawn(const TeammateSpawnConfig& config) {
    if (config.name.empty() || config.team_name.empty()) {
        return TeammateSpawnResult{
            .success = false,
            .agent_id = {},
            .error = "teammate name and team_name are required",
            .task_id = std::nullopt,
            .pane_id = std::nullopt,
        };
    }
    auto agent_id = format_agent_id(config.name, config.team_name);
    {
        std::lock_guard lock(mutex_);
        active_teammates_[agent_id] = true;
    }
    return TeammateSpawnResult{
        .success = true,
        .agent_id = agent_id,
        .error = std::nullopt,
        .task_id = "in-process:" + agent_id,
        .pane_id = std::nullopt,
    };
}

void InProcessBackend::send_message(std::string_view agent_id, const TeammateMessage& message) {
    const bool delivered = detail::write_backend_message_to_mailbox(agent_id, message);
    if (!delivered) return;
    std::lock_guard lock(mutex_);
    if (!active_teammates_.contains(std::string(agent_id))) {
        active_teammates_[std::string(agent_id)] = true;
    }
}

bool InProcessBackend::terminate(std::string_view agent_id, std::optional<std::string_view> reason) {
    {
        std::lock_guard lock(mutex_);
        auto it = active_teammates_.find(std::string(agent_id));
        if (it == active_teammates_.end() || !it->second) return false;
    }
    return detail::write_backend_message_to_mailbox(
        agent_id,
        detail::shutdown_request_message(agent_id, reason));
}

bool InProcessBackend::kill(std::string_view agent_id) {
    std::lock_guard lock(mutex_);
    return active_teammates_.erase(std::string(agent_id)) > 0;
}

bool InProcessBackend::is_active(std::string_view agent_id) const {
    std::lock_guard lock(mutex_);
    auto it = active_teammates_.find(std::string(agent_id));
    return it != active_teammates_.end() && it->second;
}

namespace detail {

bool write_backend_message_to_mailbox(
    std::string_view agent_id,
    const TeammateMessage& message
) {
    auto parsed = parse_agent_id(agent_id);
    if (!parsed) return false;

    cc::utils::TeammateMessage mailbox_message{
        .from = message.from.empty() ? std::string("team-lead") : message.from,
        .text = message.text,
        .timestamp = message.timestamp.value_or(std::string{}),
        .read = false,
        .color = message.color,
        .summary = message.summary,
    };
    auto delivered = cc::utils::write_to_mailbox(
        parsed->agent_name,
        std::move(mailbox_message),
        std::optional<std::string_view>{parsed->team_name});
    return delivered.has_value();
}

} // namespace detail

} // namespace cc::utils::swarm_backends
