/// @file internal_event.cppm
/// @brief Claude Code internal event types for analytics/telemetry.
/// Migrated from: src/types/generated/events_mono/claude_code/v1/claude_code_internal_event.ts
module;

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module cc.types.internal_event;

import cc.types.auth;

export namespace cc::types::internal_event {

/// GitHub Actions-specific environment information
struct GitHubActionsMetadata {
    std::optional<std::string> actor_id;
    std::optional<std::string> repository_id;
    std::optional<std::string> repository_owner_id;
};

/// Environment and runtime information collected from the client
struct EnvironmentMetadata {
    std::optional<std::string> platform;
    std::optional<std::string> node_version;
    std::optional<std::string> terminal;
    std::optional<std::string> package_managers;
    std::optional<std::string> runtimes;
    std::optional<bool> is_running_with_bun;
    std::optional<bool> is_ci;
    std::optional<bool> is_claubbit;
    std::optional<bool> is_github_action;
    std::optional<bool> is_claude_code_action;
    std::optional<bool> is_claude_ai_auth;
    std::optional<std::string> version;
    // GitHub Actions specific fields
    std::optional<std::string> github_event_name;
    std::optional<std::string> github_actions_runner_environment;
    std::optional<std::string> github_actions_runner_os;
    std::optional<std::string> github_action_ref;
    // WSL specific field
    std::optional<std::string> wsl_version;
    std::optional<GitHubActionsMetadata> github_actions_metadata;
    std::optional<std::string> arch;
    std::optional<bool> is_claude_code_remote;
    std::optional<std::string> remote_environment_type;
    std::optional<std::string> claude_code_container_id;
    std::optional<std::string> claude_code_remote_session_id;
    std::optional<std::vector<std::string>> tags;
    std::optional<std::string> deployment_environment;
    std::optional<bool> is_conductor;
    std::optional<std::string> version_base;
    std::optional<std::string> coworker_type;
    std::optional<std::string> build_time;
    std::optional<bool> is_local_agent_mode;
    std::optional<std::string> linux_distro_id;
    std::optional<std::string> linux_distro_version;
    std::optional<std::string> linux_kernel;
    std::optional<std::string> vcs;
    std::optional<std::string> platform_raw;
};

/// Slack context fields present on every Claude-in-Slack event
struct SlackContext {
    std::optional<std::string> slack_team_id;
    std::optional<bool> is_enterprise_install;
    std::optional<std::string> trigger;
    std::optional<std::string> creation_method;
};

/// Main internal event structure for Claude Code analytics.
/// Maps to events logged via Statsig.
struct ClaudeCodeInternalEvent {
    // Event identification
    std::optional<std::string> event_name;
    std::optional<std::chrono::system_clock::time_point> client_timestamp;
    std::optional<std::string> model;
    std::optional<std::string> session_id;
    std::optional<std::string> user_type;
    std::optional<std::string> betas;
    // Environment metadata
    std::optional<EnvironmentMetadata> env;
    std::optional<std::string> entrypoint;
    std::optional<std::string> agent_sdk_version;
    std::optional<bool> is_interactive;
    std::optional<std::string> client_type;
    // Process metrics (JSON string)
    std::optional<std::string> process;
    // Additional event-specific metadata (JSON string)
    std::optional<std::string> additional_metadata;
    // Auth context injected by API
    std::optional<cc::types::auth::PublicApiAuth> auth;
    std::optional<std::chrono::system_clock::time_point> server_timestamp;
    // Identifiers
    std::optional<std::string> event_id;
    std::optional<std::string> device_id;
    // SWE-bench fields
    std::optional<std::string> swe_bench_run_id;
    std::optional<std::string> swe_bench_instance_id;
    std::optional<std::string> swe_bench_task_id;
    std::optional<std::string> email;
    // Swarm/team agent identification
    std::optional<std::string> agent_id;
    std::optional<std::string> parent_session_id;
    std::optional<std::string> agent_type;
    // Slack context (only for cis_* events)
    std::optional<SlackContext> slack;
    std::optional<std::string> team_name;
    std::optional<std::string> skill_name;
    std::optional<std::string> plugin_name;
    std::optional<std::string> marketplace_name;
};

} // namespace cc::types::internal_event
