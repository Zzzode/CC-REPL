module;

#include <cstdlib>

export module cc.tools.agent_runtime;

import std;

import cc.utils.json;
// cc.utils.team_helpers is no longer named by any declaration in this
// interface (its only caller, has_teammate_identity, moved to
// agent_runtime_builtin_impl.cpp), but removing this import makes
// src/ui/app/app_team.cpp fail with "call to 'operator new' is ambiguous"
// under clang 22 + libc++ named modules: team_helpers' global module
// fragment textually includes <cstddef>/<fcntl.h>/<unistd.h>, and its
// position inside this BMI's closure is what keeps the aligned operator
// new declarations one entity for that consumer (it already imports
// team_helpers directly; reordering its own imports does not help).
// Empirically verified by removing and rebuilding.
// arch-check: keep-import
import cc.utils.team_helpers;
import cc.utils.yaml;

export namespace cc::tools::agent_runtime {

namespace fs = std::filesystem;

inline constexpr std::string_view teammate_system_prompt_addendum = R"(
# Agent Teammate Communication

IMPORTANT: You are running as an agent in a team. To communicate with anyone on your team:
- Use the SendMessage tool with `to: "<name>"` to send messages to specific teammates
- Use the SendMessage tool with `to: "*"` sparingly for team-wide broadcasts

Just writing a response in text is not visible to others on your team - you MUST use the SendMessage tool.

The user interacts primarily with the team lead. Your work is coordinated through the task system and teammate messaging.
)";

struct AgentRuntimeConfig {
    std::string agent_id;
    std::string working_dir;
    std::vector<std::string> capabilities;
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> worktree_path;
    std::optional<std::string> worktree_branch;
    std::optional<std::string> worktree_base_commit;
    std::optional<std::string> worktree_git_root;
    std::optional<std::string> fork_directive;
    bool allow_fork{true};
};

struct AgentExecutionResult {
    std::string agent_id;
    int exit_code;
    std::string output;
    std::optional<std::string> error;
    std::vector<std::string> transcript;
};

enum class AgentLifecycle {
    Starting,
    Running,
    Suspended,
    Completed,
    Failed,
    Cancelled
};

enum class NativeAgentStatus {
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled
};

[[nodiscard]] inline std::string_view native_agent_status_name(NativeAgentStatus status) {
    switch (status) {
        case NativeAgentStatus::Queued: return "queued";
        case NativeAgentStatus::Running: return "running";
        case NativeAgentStatus::Completed: return "completed";
        case NativeAgentStatus::Failed: return "failed";
        case NativeAgentStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

struct NativeAgentRecord {
    std::string agent_id{};
    std::string agent_type{};
    std::optional<std::string> parent_agent_id{};
    std::optional<std::string> description{};
    std::optional<std::string> name{};
    std::optional<std::string> team_name{};
    std::optional<std::string> cwd{};
    std::optional<std::string> isolation{};
    std::optional<std::string> mode{};
    bool background = false;
    NativeAgentStatus status = NativeAgentStatus::Queued;
    std::chrono::system_clock::time_point created_at = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point updated_at = created_at;
    std::optional<std::string> output{};
    std::optional<std::string> error{};
    std::vector<std::string> capabilities{};
    std::optional<std::string> transcript_path{};
    std::optional<std::string> sidechain_jsonl_path{};
    std::optional<std::string> output_file_path{};
    std::vector<std::string> sidechain_entries{};
    std::vector<std::string> pending_messages{};
    std::optional<std::string> worktree_path{};
    std::optional<std::string> worktree_branch{};
    std::optional<std::string> worktree_base_commit{};
    std::optional<std::string> worktree_git_root{};
    std::optional<std::string> teammate_backend{};
    std::optional<std::string> teammate_task_id{};
    std::optional<std::string> teammate_pane_id{};
    std::optional<std::string> teammate_color{};
    std::optional<std::string> parent_session_id{};
    std::vector<std::string> transcript{};
    std::optional<double> progress{};
    bool cancel_requested = false;
    bool notification_delivered = false;
    bool worktree_cleanup_performed = false;
};

[[nodiscard]] inline NativeAgentRecord make_native_agent_record(
    std::string agent_id,
    std::string agent_type)
{
    NativeAgentRecord record;
    record.agent_id = std::move(agent_id);
    record.agent_type = std::move(agent_type);
    return record;
}

struct AgentInlineMcpServerConfig {
    std::string name;
    std::string transport;
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::string headers_helper;
};

struct AgentHookCommand {
    std::string command;
    std::string shell = "bash";
    std::optional<int> timeout_seconds;
    std::optional<std::string> condition;
};

struct AgentHookMatcher {
    std::optional<std::string> matcher;
    std::vector<AgentHookCommand> hooks;
};

using AgentHooksByEvent = std::map<std::string, std::vector<AgentHookMatcher>, std::less<>>;

struct AgentDefinition {
    std::string agent_type;
    std::string when_to_use;
    std::string model;
    std::string source;
    std::optional<std::string> filename;
    std::optional<std::string> path;
    std::string system_prompt;
    std::vector<std::string> tools;
    std::vector<std::string> disallowed_tools;
    std::optional<std::string> permission_mode;
    std::optional<int> max_turns;
    std::optional<std::string> initial_prompt;
    bool background = false;
    std::optional<std::string> isolation;
    std::vector<std::string> required_mcp_servers;
    std::vector<std::string> mcp_servers;
    std::vector<AgentInlineMcpServerConfig> inline_mcp_servers;
    std::vector<std::string> skills;
    AgentHooksByEvent hooks;
    bool hooks_present = false;
    std::optional<std::string> effort;
    std::optional<std::string> memory;
    std::optional<std::string> color;
    bool omit_loom_md = false;
    std::optional<std::string> critical_system_reminder;
};

struct PluginComponentPaths {
    std::string plugin_name;
    fs::path plugin_dir;
    std::vector<fs::path> agents_paths;
    std::vector<fs::path> skills_paths;
};

[[nodiscard]] std::string trim(std::string_view value);
[[nodiscard]] std::string unquote(std::string value);
[[nodiscard]] std::vector<std::string> split_list_value(std::string_view value);
[[nodiscard]] std::optional<int> parse_positive_int(std::string_view value);
[[nodiscard]] std::string canonicalize_agent_type(std::string_view value);
[[nodiscard]] bool parse_bool_field(std::string_view value, bool fallback = false);

[[nodiscard]] inline bool env_truthy(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    auto text = canonicalize_agent_type(value);
    return text == "1" || text == "true" || text == "yes" || text == "on";
}

[[nodiscard]] inline bool valid_agent_memory_scope(std::string_view value) {
    return value == "user" || value == "project" || value == "local";
}

[[nodiscard]] inline bool valid_agent_color(std::string_view value) {
    return value == "red" || value == "blue" || value == "green" || value == "yellow" ||
        value == "purple" || value == "orange" || value == "pink" || value == "cyan";
}

[[nodiscard]] bool valid_agent_effort(std::string_view value);
[[nodiscard]] bool is_ant_user_type();
[[nodiscard]] bool valid_agent_isolation(std::string_view value);
[[nodiscard]] std::string valid_agent_isolation_options();


[[nodiscard]] std::optional<std::string> yaml_scalar_to_string(const cc::utils::YamlValue& value);
[[nodiscard]] const cc::utils::YamlValue* yaml_field(
    const cc::utils::YamlMap& fields,
    std::string_view key);
[[nodiscard]] std::optional<std::string> yaml_string_field(
    const cc::utils::YamlMap& fields,
    std::string_view key);
[[nodiscard]] std::vector<std::string> yaml_string_list(const cc::utils::YamlValue& value);
[[nodiscard]] std::vector<std::string> yaml_string_list_field(
    const cc::utils::YamlMap& fields,
    std::string_view key);
void append_yaml_string_map(
    const cc::utils::YamlValue& value,
    std::unordered_map<std::string, std::string>& out);


[[nodiscard]] std::optional<std::string> json_scalar_to_string(cc::utils::json::JsonVal value);
[[nodiscard]] std::optional<std::string> json_string_field(
    cc::utils::json::JsonVal object,
    std::string_view key);
[[nodiscard]] std::vector<std::string> json_string_list(cc::utils::json::JsonVal value);
[[nodiscard]] std::vector<std::string> json_string_list_field(
    cc::utils::json::JsonVal object,
    std::string_view key);
void append_json_string_map(
    cc::utils::json::JsonVal value,
    std::unordered_map<std::string, std::string>& out);


[[nodiscard]] std::optional<AgentInlineMcpServerConfig> parse_inline_mcp_server_config(
    std::string name,
    const cc::utils::YamlValue& value);


[[nodiscard]] std::optional<AgentInlineMcpServerConfig> parse_inline_mcp_server_config(
    std::string name,
    cc::utils::json::JsonVal value);

struct ParsedAgentMcpServers {
    std::vector<std::string> references;
    std::vector<AgentInlineMcpServerConfig> inline_configs;
};


[[nodiscard]] ParsedAgentMcpServers parse_agent_mcp_servers(const cc::utils::YamlValue& value);


[[nodiscard]] ParsedAgentMcpServers parse_agent_mcp_servers(cc::utils::json::JsonVal value);


[[nodiscard]] std::string canonical_hook_event_name(std::string_view event);


[[nodiscard]] std::optional<AgentHookCommand> parse_agent_hook_command(const cc::utils::YamlValue& value);
[[nodiscard]] std::vector<AgentHookCommand> parse_agent_hook_commands(const cc::utils::YamlValue& value);
[[nodiscard]] std::optional<AgentHookMatcher> parse_agent_hook_matcher(const cc::utils::YamlValue& value);
[[nodiscard]] std::vector<AgentHookMatcher> parse_agent_hook_matchers(const cc::utils::YamlValue& value);
[[nodiscard]] AgentHooksByEvent parse_agent_hooks(const cc::utils::YamlValue& value);


[[nodiscard]] std::vector<std::string> agent_alias_candidates(std::string_view requested_type);

enum class ResolutionError {
    EmptyRequestedType,
    ExactOrCanonicalMatchNotFound,
    LegacyAliasAmbiguous,
    LegacyAliasNoMatch,
    SuffixMatchAmbiguous,
    NoCompatibleMatch,
};

[[nodiscard]] inline std::string_view resolution_error_name(ResolutionError err) {
    switch (err) {
        case ResolutionError::EmptyRequestedType:        return "empty_requested_type";
        case ResolutionError::ExactOrCanonicalMatchNotFound: return "exact_or_canonical_match_not_found";
        case ResolutionError::LegacyAliasAmbiguous:      return "legacy_alias_ambiguous";
        case ResolutionError::LegacyAliasNoMatch:        return "legacy_alias_no_match";
        case ResolutionError::SuffixMatchAmbiguous:      return "suffix_match_ambiguous";
        case ResolutionError::NoCompatibleMatch:         return "no_compatible_match";
    }
    return "unknown";
}

[[nodiscard]] std::optional<std::string> find_canonical_agent_type_match(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents);

// Resolves a user-provided agent type string into a concrete agent.
//
// Resolution order (matches TypeScript resolveRequestedAgentType):
//   1. Exact string equality on agent.agent_type
//      // Test case: exact match when present  →  Plan → "Plan" (preserves casing)
//   2. Case- and separator-insensitive canonical match
//      // Test case: case-insensitive + space/dash variants  →  "General Purpose" → "general-purpose"
//   3. Legacy alias expansion (explore/explorer/plan/planner) with canonical match
//      // Test case: legacy Explore → namespaced code-explorer  →  "Explore" → "feature-dev:code-explorer"
//   4. Legacy alias expansion with `:alias` suffix filter (single match only)
//      // Test case: ambiguous legacy suffix → undefined  →  "Explore" with two ":code-explorer" agents → nullopt
//   5. Otherwise nullopt
//      // Test case: no compatible match  →  "non-existent-agent" → undefined

[[nodiscard]] std::optional<std::string> resolve_requested_agent_type(
    std::string_view requested_type,
    const std::vector<AgentDefinition>& agents);

// Rich-result wrapper around resolve_requested_agent_type: returns the matched
// AgentDefinition directly, or a ResolutionError explaining why resolution failed.
//
// This is the C++-idiomatic public API consumed by cc.tools.agent_type_resolution.

[[nodiscard]] std::expected<AgentDefinition, ResolutionError> resolve_agent_type(
    std::string_view id,
    const std::vector<AgentDefinition>& agents);


[[nodiscard]] std::string format_agent_type_list(
    const std::vector<AgentDefinition>& agents);
[[nodiscard]] bool is_sdk_entrypoint();

// Feature flag gate for Explore + Plan (TS: areExplorePlanAgentsEnabled).
// 3P (open-source / Bedrock / Vertex) default: on. Ant-native default: off
// (opt-in via GrowthBook tengu_amber_stoat — ported as explicit env override).

[[nodiscard]] bool are_explore_plan_agents_enabled();
[[nodiscard]] bool is_verification_agent_enabled();


[[nodiscard]] std::vector<AgentDefinition> built_in_agent_definitions();


void append_existing_plugin_component_path(
    cc::utils::json::JsonVal value,
    const fs::path& plugin_dir,
    std::vector<fs::path>& out);
[[nodiscard]] std::optional<PluginComponentPaths> read_plugin_component_paths(
    const fs::path& plugin_dir);
[[nodiscard]] std::vector<PluginComponentPaths> discover_plugin_component_paths();


[[nodiscard]] std::optional<AgentDefinition> parse_agent_markdown(
    const fs::path& path,
    std::string source);


[[nodiscard]] std::optional<int> json_positive_int_field(
    cc::utils::json::JsonVal object,
    std::string_view key);
[[nodiscard]] std::optional<bool> json_bool_field(
    cc::utils::json::JsonVal object,
    std::string_view key);


[[nodiscard]] std::optional<AgentHookCommand> parse_agent_hook_command(cc::utils::json::JsonVal value);
[[nodiscard]] std::vector<AgentHookCommand> parse_agent_hook_commands(cc::utils::json::JsonVal value);
[[nodiscard]] std::optional<AgentHookMatcher> parse_agent_hook_matcher(cc::utils::json::JsonVal value);
[[nodiscard]] std::vector<AgentHookMatcher> parse_agent_hook_matchers(cc::utils::json::JsonVal value);
[[nodiscard]] AgentHooksByEvent parse_agent_hooks(cc::utils::json::JsonVal value);


[[nodiscard]] std::optional<AgentDefinition> parse_agent_json_definition(
    std::string name,
    cc::utils::json::JsonVal object,
    const fs::path& path,
    std::string source);
[[nodiscard]] std::vector<AgentDefinition> parse_agents_json_file(
    const fs::path& path,
    std::string source);
[[nodiscard]] std::vector<AgentDefinition> parse_agents_json_string(
    std::string_view json,
    std::string source,
    const fs::path& virtual_path = {});
[[nodiscard]] std::vector<AgentDefinition> load_agent_definitions_from_settings_file(
    const fs::path& path,
    std::string source);
[[nodiscard]] std::vector<AgentDefinition> load_flag_agent_definitions();
[[nodiscard]] std::vector<AgentDefinition> load_policy_agent_definitions();

struct FailedAgentFile {
    std::string path;
    std::string error;
};

// Mirrors TS `getParseError`: describes *why* a markdown file with a
// frontmatter `name:` field failed to parse as a valid agent definition.

[[nodiscard]] std::string get_parse_error(
    const cc::utils::YamlMap& fields,
    std::string_view fallback = "Unknown parsing error");

struct LoadAgentDefinitionsResult {
    std::vector<AgentDefinition> agents;
    std::vector<FailedAgentFile> failed;
};

// Scans a directory for `.md` and `.json` agent definition files and returns
// both successfully parsed agents and any files that had `name:` in their
// frontmatter but were otherwise invalid (matching TS getParseError semantics).
// Co-located reference markdown (no `name:` frontmatter) is silently skipped.

[[nodiscard]] LoadAgentDefinitionsResult load_agent_definitions_from_dir_ex(
    const fs::path& dir,
    std::string source);

// Original (backward-compatible) API: returns only the successfully parsed
// agents and drops any diagnostic information. Implemented in terms of the
// richer loader so callers can pick whichever surface they need.
[[nodiscard]] std::vector<AgentDefinition> load_agent_definitions_from_dir(
    const fs::path& dir,
    std::string source);


[[nodiscard]] std::string qualify_plugin_component_name(
    std::string_view plugin_name,
    std::string value);
void qualify_plugin_mcp_names(AgentDefinition& agent, std::string_view plugin_name);
void append_plugin_agent_definition(
    std::vector<AgentDefinition>& agents,
    AgentDefinition agent,
    std::string_view plugin_name,
    const std::vector<std::string>& namespace_parts);
void load_plugin_agents_from_path(
    std::vector<AgentDefinition>& agents,
    const fs::path& path,
    std::string_view plugin_name,
    std::vector<std::string> namespace_parts = {});
[[nodiscard]] std::vector<AgentDefinition> load_plugin_agent_definitions();


[[nodiscard]] std::vector<AgentDefinition> get_all_agent_definitions(
    std::optional<fs::path> cwd = std::nullopt);
[[nodiscard]] std::optional<AgentDefinition> find_agent_definition(
    std::string_view requested_type,
    std::optional<fs::path> cwd = std::nullopt);
[[nodiscard]] bool has_teammate_identity();

inline void append_prompt_section(std::string& prompt, std::string_view section) {
    if (section.empty()) return;
    if (!prompt.empty()) prompt += "\n\n";
    prompt += section;
}

[[nodiscard]] std::optional<std::string> build_teammate_append_system_prompt(
    std::optional<std::string> existing_append_prompt = std::nullopt,
    std::optional<fs::path> cwd = std::nullopt);


std::expected<AgentExecutionResult, std::string> run_agent(const AgentRuntimeConfig& config);

std::expected<std::string, std::string> fork_subagent(std::string_view parent_id, const AgentRuntimeConfig& config);

std::expected<AgentExecutionResult, std::string> resume_agent(std::string_view agent_id);

// Minimal light-weight API used by consumers that only need the agent type
// names discovered in a directory.

[[nodiscard]] std::expected<std::vector<std::string>, std::string> load_agents_from_dir(std::string_view dir_path);


// Full loader: scans the directory and returns every parsed AgentDefinition
// (matching TS getAgentDefinitionsWithOverrides for a single directory).
// Callers that also need the full built-in + settings + plugin union should
// use `get_all_agent_definitions()` instead.
//
// `source` is forwarded directly to the parser; TS conventions are:
//   "userSettings"    ~/.loom/agents
//   "projectSettings" <cwd>/.loom/agents
//   "policySettings"  policy config directory
//   "flagSettings"    env var parsed definitions
//   "custom"          arbitrary caller-supplied directory

[[nodiscard]] LoadAgentDefinitionsResult load_agents_dir(
    const fs::path& dir,
    std::string_view source = "custom");

// Fork / resume message helpers.
//
// Mirrors TS `buildForkedMessages` / `buildChildMessage` — these are the text
// fragments the parent injects into a fork child's initial user messages so
// the child can recognize it is a worker, knows its directive, and sees
// placeholder tool_results for every parent tool_use.
// Mirrors TS `isForkSubagentEnabled`. Fork is disabled in three scenarios:
//   1. The FORK_SUBAGENT feature flag env var is explicitly disabled.
//   2. We're in coordinator mode (coordinator already owns delegation).
//   3. The session is non-interactive (no terminal to surface notifications).
//
// In C++ all three are encoded as environment gating so the semantics stay
// close to the TS feature-flag surface without pulling in GrowthBook SDK:
//   - FORK_SUBAGENT=0            → disabled
//   - CC_COORDINATOR_MODE=1      → disabled  (TS isCoordinatorMode)
//   - CC_NON_INTERACTIVE=1       → disabled  (TS getIsNonInteractiveSession)

[[nodiscard]] bool is_fork_subagent_enabled();

// Returns true if a text block contains the magic fork boilerplate tag. Used
// by the recursive-fork guard in agent_tool.cppm to reject forks inside forks
// (Agent tool is still present in the child's tool pool for cache-identical
// API prefixes, so the guard is call-time, not schema-time).
[[nodiscard]] inline bool text_contains_fork_boilerplate_tag(std::string_view text) {
    return text.find("<fork-boilerplate>") != std::string_view::npos;
}
// Builds the child-machine instructions prepended to a fork directive. The
// string is intentionally byte-stable across all fork children so the
// rendered fork prefix stays cache-identical and prompt cache hits maximise.

[[nodiscard]] std::string build_fork_child_message(std::string_view directive);

// Injected into the fork child's context when the child is isolated in a
// separate git worktree — instructs it to translate inherited paths and
// re-read stale files.

[[nodiscard]] std::string build_worktree_fork_notice(
    std::string_view parent_cwd,
    std::string_view worktree_cwd);


AgentLifecycle get_agent_lifecycle(std::string_view agent_id);


[[nodiscard]] std::string json_escape(std::string_view value);

[[nodiscard]] inline fs::path runtime_state_dir() {
    if (const char* env = std::getenv("LOOM_AGENT_RUNTIME_DIR"); env && *env) {
        return fs::path{env};
    }
    return fs::current_path() / ".loom" / "agent-runtime";
}

[[nodiscard]] std::string safe_agent_filename(std::string_view agent_id);

[[nodiscard]] inline fs::path agent_record_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".json");
}

[[nodiscard]] inline fs::path agent_transcript_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".transcript");
}

[[nodiscard]] inline fs::path agent_output_file_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".output");
}

[[nodiscard]] inline fs::path agent_sidechain_jsonl_path(std::string_view agent_id) {
    return runtime_state_dir() / (safe_agent_filename(agent_id) + ".sidechain.jsonl");
}

[[nodiscard]] inline std::optional<NativeAgentStatus> native_agent_status_from_string(std::string_view status) {
    if (status == "queued") return NativeAgentStatus::Queued;
    if (status == "running") return NativeAgentStatus::Running;
    if (status == "completed") return NativeAgentStatus::Completed;
    if (status == "failed") return NativeAgentStatus::Failed;
    if (status == "cancelled") return NativeAgentStatus::Cancelled;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> native_agent_terminal_notification_status(NativeAgentStatus status) {
    switch (status) {
        case NativeAgentStatus::Completed: return "completed";
        case NativeAgentStatus::Failed: return "failed";
        case NativeAgentStatus::Cancelled: return "stopped";
        case NativeAgentStatus::Queued:
        case NativeAgentStatus::Running:
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::string json_string_field(
    cc::utils::json::JsonVal value,
    std::initializer_list<std::string_view> keys);


[[nodiscard]] std::string json_text_from_content(cc::utils::json::JsonVal content);


[[nodiscard]] std::string xml_escape(std::string_view text);

[[nodiscard]] inline std::string native_agent_display_name(const NativeAgentRecord& record) {
    if (record.description && !record.description->empty()) return *record.description;
    if (record.name && !record.name->empty()) return *record.name;
    return record.agent_id;
}

[[nodiscard]] inline std::string native_agent_output_file(const NativeAgentRecord& record) {
    if (record.output_file_path && !record.output_file_path->empty()) return *record.output_file_path;
    if (record.transcript_path && !record.transcript_path->empty()) return *record.transcript_path;
    return agent_output_file_path(record.agent_id).string();
}

[[nodiscard]] std::optional<std::string> format_native_agent_task_notification(
    const NativeAgentRecord& record);

void write_json_string_array(std::ostream& out, std::string_view name, const std::vector<std::string>& values);

void write_json_optional_string(
    std::ostream& out,
    std::string_view name,
    const std::optional<std::string>& value);

void refresh_native_agent_output_symlink(
    const fs::path& output_path,
    const fs::path& transcript_path);


[[nodiscard]] std::pair<std::string_view, std::string_view> split_transcript_role(
    std::string_view entry);
[[nodiscard]] std::string sidechain_message_role(std::string_view role);


[[nodiscard]] std::string fallback_sidechain_content_json(std::string_view text);
[[nodiscard]] std::string normalize_sidechain_content_json(
    std::string_view content_json,
    std::string_view fallback_text);
[[nodiscard]] std::string make_sidechain_jsonl_entry(
    std::string_view agent_id,
    std::size_t index,
    std::string_view role,
    std::string_view content_json,
    std::string_view fallback_text);
[[nodiscard]] std::optional<std::string> rebase_sidechain_jsonl_entry_for_agent(
    std::string_view entry,
    std::string_view agent_id,
    std::size_t index);
[[nodiscard]] std::optional<std::string> rebase_content_replacement_entry_for_agent(
    std::string_view entry,
    std::string_view agent_id);
[[nodiscard]] std::vector<std::string> fork_sidechain_entries_for_child(
    const NativeAgentRecord& parent,
    std::string_view child_agent_id);
void collect_sidechain_tool_use_state(
    cc::utils::json::JsonVal content,
    std::vector<std::string>& tool_use_ids,
    std::unordered_set<std::string>& seen_tool_use_ids,
    std::unordered_set<std::string>& tool_result_ids);
[[nodiscard]] std::vector<std::string> unresolved_tool_use_ids_from_sidechain_entries(
    const std::vector<std::string>& entries);
[[nodiscard]] std::string fork_missing_tool_results_content_json(
    const std::vector<std::string>& tool_use_ids,
    std::string_view directive_message);
[[nodiscard]] bool native_agent_record_is_fork_child(const NativeAgentRecord& record);
[[nodiscard]] std::vector<std::string> fork_child_capabilities(
    const NativeAgentRecord& parent,
    const AgentRuntimeConfig& config);
bool write_sidechain_jsonl(
    const NativeAgentRecord& record,
    const fs::path& sidechain_path);
bool persist_native_agent_record(const NativeAgentRecord& record);
[[nodiscard]] std::vector<std::string> read_transcript_lines(const fs::path& path);
[[nodiscard]] std::string transcript_text_from_content(cc::utils::json::JsonVal content);
[[nodiscard]] std::optional<std::string> transcript_entry_from_ts_jsonl(cc::utils::json::JsonVal root);
[[nodiscard]] std::optional<std::string> transcript_entry_from_sidechain_jsonl_line(std::string_view line);
[[nodiscard]] std::vector<std::string> transcript_lines_from_sidechain_entries(
    const std::vector<std::string>& entries);
[[nodiscard]] std::vector<std::string> read_sidechain_transcript_lines(const fs::path& path);
[[nodiscard]] std::vector<std::string> read_sidechain_jsonl_entries(const fs::path& path);


[[nodiscard]] std::vector<std::string> json_string_array(cc::utils::json::JsonVal value);


[[nodiscard]] std::optional<NativeAgentRecord> load_native_agent_record_from_path(const fs::path& path);
[[nodiscard]] std::optional<NativeAgentRecord> load_native_agent_record(std::string_view agent_id);
[[nodiscard]] std::vector<NativeAgentRecord> load_all_native_agent_records();


class NativeAgentStore {
public:
    void upsert(NativeAgentRecord record);

    [[nodiscard]] std::optional<NativeAgentRecord> get(std::string_view agent_id) const;

    [[nodiscard]] std::vector<NativeAgentRecord> list() const;

    void mark_running(std::string_view agent_id);

    void mark_completed(std::string_view agent_id, std::string output);

    void mark_failed(std::string_view agent_id, std::string error);

    void mark_cancelled(std::string_view agent_id, std::string reason);

    void request_cancel(std::string_view agent_id, std::string reason = "cancel requested");

    [[nodiscard]] bool is_cancel_requested(std::string_view agent_id) const;

    [[nodiscard]] std::vector<std::string> take_pending_task_notifications();

    void clear_for_testing();

    void append_transcript(std::string_view agent_id, std::string entry);

    void enqueue_pending_message(std::string_view agent_id, std::string message);

    void enqueue_resume_message(std::string_view agent_id, std::string message);

    [[nodiscard]] std::vector<std::string> take_pending_messages(std::string_view agent_id);

    void append_sidechain_message(
        std::string_view agent_id,
        std::string_view role,
        std::string_view content_json,
        std::string_view fallback_text = {});

    void append_sidechain_entry(std::string_view agent_id, std::string entry);

    void update_progress(std::string_view agent_id, double progress);

    void set_worktree_metadata(
        std::string_view agent_id,
        std::string path,
        std::string branch,
        std::string base_commit,
        std::string git_root);

    void mark_worktree_cleaned(std::string_view agent_id);

    void set_teammate_metadata(
        std::string_view agent_id,
        std::string backend,
        std::optional<std::string> task_id = std::nullopt,
        std::optional<std::string> pane_id = std::nullopt,
        std::optional<std::string> color = std::nullopt,
        std::optional<std::string> parent_session_id = std::nullopt);

private:
    template <typename Fn>
    void update(std::string_view agent_id, Fn&& fn) {
        std::scoped_lock lock(mutex_);
        auto it = records_.find(std::string(agent_id));
        if (it == records_.end()) {
            if (auto loaded = load_native_agent_record(agent_id)) {
                auto loaded_agent_id = loaded->agent_id;
                auto [inserted, _] = records_.emplace(std::move(loaded_agent_id), std::move(*loaded));
                it = inserted;
            } else {
                return;
            }
        }
        fn(it->second);
        if (!it->second.transcript_path) {
            it->second.transcript_path = agent_transcript_path(it->second.agent_id).string();
        }
        if (!it->second.sidechain_jsonl_path) {
            it->second.sidechain_jsonl_path = agent_sidechain_jsonl_path(it->second.agent_id).string();
        }
        if (!it->second.output_file_path) {
            it->second.output_file_path = agent_output_file_path(it->second.agent_id).string();
        }
        it->second.updated_at = std::chrono::system_clock::now();
        (void)persist_native_agent_record(it->second);
    }

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, NativeAgentRecord> records_;
};

NativeAgentStore& native_agent_store();


[[nodiscard]] std::string runtime_agent_id(const AgentRuntimeConfig& config);

} // namespace cc::tools::agent_runtime
