module;

#include <cstdlib>

export module cc.tools.agent.utils;

import std;

import cc.utils.json;
import cc.tools.tool;
import cc.tools.agent_runtime;
import cc.tools.mcp;
// Team / MemberRole (cc.tools.team), AgentColor (cc.utils.swarm_backends)
// and SkillDefinition (cc.skills.skill) are named in declarations kept in
// this interface, so their owner modules must be imported here even though
// every function body that uses them moved to an implementation unit.
import cc.tools.team;
import cc.utils.swarm_backends;
import cc.skills.skill;
import cc.services.api.client;

export namespace cc::tools::agent::utils {

namespace fs = std::filesystem;

using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::SchemaProperty;
using cc::services::api::CreateMessageRequest;
using cc::services::api::Message;
using cc::services::api::ContentBlock;
using cc::services::api::ContentBlockType;

// Agent Configuration
// =========================================================================

struct AgentConfig {
    int max_turns = 200;           // Max agentic loop iterations
    int max_depth = 3;             // Max recursive agent nesting depth
    std::string default_model = "claude-sonnet-4-20250514";
    std::vector<std::string> allowed_tools;  // Empty = inherit all from parent
    std::vector<std::string> denied_tools;   // Explicitly blocked tools for sub-agents
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> parent_permission_mode;
    bool prefer_in_process_teammate = false;
};

struct AgentLivePermissionCheck {
    bool allowed = true;
    std::optional<std::string> updated_input_json;
    std::optional<std::string> message;
};

using AgentLivePermissionCheckFn = std::function<AgentLivePermissionCheck(
    std::string_view tool_name,
    std::string_view input_json,
    std::string_view tool_use_id
)>;

[[nodiscard]] std::string json_escape_string(std::string_view value);

[[nodiscard]] inline bool is_todo_write_tool_name(std::string_view tool_name) {
    return tool_name == "todo_write" || tool_name == "TodoWrite";
}

[[nodiscard]] inline bool is_agent_scoped_shell_tool_name(std::string_view tool_name) {
    return tool_name == "bash" || tool_name == "Bash";
}

[[nodiscard]] inline bool text_contains_fork_boilerplate(std::string_view text) {
    return text.find("<fork-boilerplate>") != std::string_view::npos;
}

[[nodiscard]] inline bool query_source_is_fork_child(std::string_view query_source) {
    return query_source == "agent:builtin:fork";
}

[[nodiscard]] std::string inject_agent_id_into_tool_input(
    std::string_view raw_json,
    std::string_view agent_id
);

[[nodiscard]] std::string inject_agent_id_into_todo_input(
    std::string_view raw_json,
    std::string_view agent_id
);

// The destructors of these RAII guards are intentionally out-of-line: they
// call cleanup helpers (todo / shell-task / native-MCP) from other modules,
// and every destroying importer must bind to one external symbol instead of
// emitting that cleanup inline. Exactly one strong definition lives in
// agent_sub_utils_hooks.cpp.
struct AgentTodoCleanupGuard {
    std::string agent_id;

    ~AgentTodoCleanupGuard();
};

struct AgentShellTaskCleanupGuard {
    std::string agent_id;

    ~AgentShellTaskCleanupGuard();
};

struct AgentInlineMcpServerRuntimeState {
    std::string name;
    std::optional<cc::tools::NativeMcpConfiguredServer> previous_config;
};

struct AgentMcpCleanupGuard {
    std::string agent_id;
    std::vector<AgentInlineMcpServerRuntimeState> inline_servers;

    ~AgentMcpCleanupGuard();
};

void append_json_string_field(
    std::string& out,
    std::string_view name,
    std::string_view value,
    bool& first
);

void append_json_optional_string_field(
    std::string& out,
    std::string_view name,
    const std::optional<std::string>& value,
    bool& first
);

[[nodiscard]] SchemaProperty agent_schema_property(
    std::string name,
    std::string type,
    std::string description,
    bool required = false
);

struct AgentToolRequest {
    std::string prompt;
    std::optional<std::string> description;
    std::string subagent_type = "general-purpose";
    std::optional<std::string> model;
    bool run_in_background = false;
    std::optional<std::string> agent_id_override;
    bool resume_existing = false;
    std::optional<std::string> query_source;
    bool fork_child_context = false;
    std::optional<std::string> parent_system_prompt;
    std::vector<std::string> exact_tools;
    bool use_exact_tools = false;
    std::vector<std::string> fork_context_entries;
    std::vector<std::string> parent_assistant_message_entries;
    std::optional<std::string> name;
    std::optional<std::string> team_name;
    std::optional<std::string> mode;
    std::optional<std::string> isolation;
    std::optional<std::string> cwd;
};

struct AgentMcpToolBinding {
    std::string server_name;
    std::string tool_name;
    std::string description;
};

struct AgentExecutionPlan {
    std::string agent_id;
    std::string prompt;
    std::optional<std::string> description;
    std::string agent_type;
    bool is_built_in = false;
    std::string model;
    std::string system_prompt;
    std::vector<std::string> preloaded_skill_messages;
    std::vector<std::string> hook_additional_contexts;
    std::vector<std::string> agent_mcp_servers;
    std::vector<AgentMcpToolBinding> agent_mcp_tools;
    std::optional<std::string> agent_mcp_context_message;
    std::vector<AgentInlineMcpServerRuntimeState> inline_mcp_server_states;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> disallowed_tools;
    int max_turns = 200;
    bool background = false;
    bool resume_existing = false;
    std::optional<std::string> query_source;
    bool fork_child_context = false;
    bool system_prompt_overridden = false;
    std::vector<std::string> exact_tools;
    bool use_exact_tools = false;
    std::vector<Message> fork_context_messages;
    bool fork_context_includes_prompt = false;
    std::optional<std::string> name;
    std::optional<std::string> team_name;
    std::optional<std::string> mode;
    std::optional<std::string> isolation;
    std::optional<std::string> working_dir;
    std::optional<std::string> worktree_path;
    std::optional<std::string> worktree_branch;
    std::optional<std::string> worktree_base_commit;
    std::optional<std::string> worktree_git_root;
    cc::tools::agent_runtime::AgentHooksByEvent frontmatter_hooks;
    std::optional<std::string> effort;
    std::optional<std::string> memory;
    std::optional<std::string> color;
    std::optional<std::string> teammate_backend;
    std::optional<std::string> teammate_task_id;
    std::optional<std::string> teammate_pane_id;
    std::optional<std::string> teammate_color;
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> parent_session_id;
    bool omit_loom_md = false;
    std::optional<std::string> critical_system_reminder;
};

[[nodiscard]] bool is_auto_memory_enabled();
[[nodiscard]] fs::path config_home_dir();
[[nodiscard]] fs::path agent_memory_base_dir();
[[nodiscard]] std::string sanitize_agent_memory_component(std::string_view value);
[[nodiscard]] std::string agent_memory_dir(
    std::string_view agent_type,
    std::string_view scope,
    const std::optional<std::string>& working_dir
);
[[nodiscard]] std::string agent_memory_scope_note(std::string_view scope);
[[nodiscard]] std::string join_agent_memory_prompt_lines(const std::vector<std::string>& lines);
[[nodiscard]] std::string load_agent_memory_prompt(
    std::string_view agent_type,
    std::string_view scope,
    const std::optional<std::string>& working_dir
);
void add_agent_memory_tools(std::vector<std::string>& tools);

[[nodiscard]] std::string trim_ascii_copy(std::string_view value);

[[nodiscard]] std::optional<std::string> json_string(
    cc::utils::json::JsonVal root,
    std::string_view key
);

[[nodiscard]] bool json_bool(
    cc::utils::json::JsonVal root,
    std::string_view key,
    bool fallback = false
);

[[nodiscard]] std::optional<int> json_int(
    cc::utils::json::JsonVal root,
    std::string_view key
);

[[nodiscard]] bool has_non_empty_string(
    cc::utils::json::JsonVal root,
    std::string_view key
);

[[nodiscard]] std::optional<std::string> resolve_agent_relative_path(
    const std::optional<std::string>& working_dir,
    std::string_view value
);

[[nodiscard]] std::string json_object_with_string_overrides(
    std::string_view raw_json,
    const std::unordered_map<std::string, std::string>& overrides
);

[[nodiscard]] std::vector<std::string> json_string_array(cc::utils::json::JsonVal value);

[[nodiscard]] std::vector<std::string> json_string_array_field(
    cc::utils::json::JsonVal root,
    std::string_view key
);

[[nodiscard]] bool json_array_looks_like_content_blocks(cc::utils::json::JsonVal value);

[[nodiscard]] std::string assistant_content_array_json_to_message_json(std::string_view content_json);

[[nodiscard]] std::vector<std::string> json_message_entries(cc::utils::json::JsonVal value);

[[nodiscard]] std::vector<std::string> json_message_entries_field(
    cc::utils::json::JsonVal root,
    std::string_view key
);

[[nodiscard]] bool agent_tool_input_omits_agent_type(std::string_view raw_json);

[[nodiscard]] std::string next_agent_id(const std::optional<std::string>& preferred_name);

[[nodiscard]] std::string sanitize_teammate_agent_name(std::string name);

[[nodiscard]] std::string lowercase_ascii(std::string_view value);

[[nodiscard]] std::string teammate_name_from_agent_id(std::string_view agent_id);

[[nodiscard]] std::string unique_teammate_agent_name(
    std::string base_name,
    const cc::tools::Team& team
);

[[nodiscard]] std::string format_teammate_agent_id(std::string_view agent_name, std::string_view team_name);

[[nodiscard]] bool current_session_is_teammate();

[[nodiscard]] std::optional<std::string> resolve_agent_model(std::optional<std::string> model);

[[nodiscard]] std::expected<AgentToolRequest, std::string> parse_agent_tool_request(
    const ToolInput& input
);

[[nodiscard]] std::string join_fields(const std::vector<std::string>& fields);

[[nodiscard]] std::string built_in_system_prompt(std::string_view agent_type);

[[nodiscard]] std::string_view trim_tool_rule(std::string_view value);

[[nodiscard]] std::string_view permission_rule_tool_name(std::string_view rule);

[[nodiscard]] std::string canonical_tool_name(std::string_view value);

[[nodiscard]] bool env_flag_enabled(const char* name);

[[nodiscard]] bool is_ant_user();

[[nodiscard]] bool agent_model_supports_effort(std::string_view model);

[[nodiscard]] bool agent_model_supports_max_effort(std::string_view model);

[[nodiscard]] bool is_agent_effort_level(std::string_view value);

[[nodiscard]] std::optional<std::string> normalized_permission_mode(
    const std::optional<std::string>& mode
);

[[nodiscard]] bool parent_permission_mode_blocks_agent_override(
    const std::optional<std::string>& parent_mode
);

[[nodiscard]] std::optional<std::string> effective_agent_permission_mode(
    const std::optional<std::string>& request_mode,
    const std::optional<std::string>& definition_mode,
    const std::optional<std::string>& parent_mode
);

[[nodiscard]] std::optional<int> parse_agent_numeric_effort(std::string_view value);

void append_agent_effort_beta(CreateMessageRequest& request);

void apply_agent_effort_to_request(
    CreateMessageRequest& request,
    const std::optional<std::string>& effort
);

[[nodiscard]] bool tool_rule_matches_tool_name(
    std::string_view rule,
    std::string_view tool_name
);

[[nodiscard]] bool normalized_tool_name_is(std::string_view tool_name, std::string_view expected);

[[nodiscard]] bool normalized_tool_name_in(
    std::string_view tool_name,
    std::initializer_list<std::string_view> names
);

[[nodiscard]] bool is_mcp_tool_name(std::string_view tool_name);

[[nodiscard]] std::string apply_agent_tool_execution_context_to_input(
    std::string_view tool_name,
    std::string_view raw_json,
    const AgentExecutionPlan& plan
);

[[nodiscard]] bool all_agent_disallows_tool(std::string_view tool_name);

[[nodiscard]] bool custom_agent_disallows_tool(std::string_view tool_name);

[[nodiscard]] bool async_agent_allows_tool(std::string_view tool_name);

[[nodiscard]] bool in_process_teammate_allows_tool(std::string_view tool_name);

[[nodiscard]] bool agent_base_filter_allows_tool(
    std::string_view tool_name,
    bool is_built_in,
    bool is_async,
    std::optional<std::string_view> permission_mode = std::nullopt,
    bool is_in_process_teammate = false
);

[[nodiscard]] std::string canonical_tool_rule_value(std::string_view value);

[[nodiscard]] std::vector<std::string> permission_rule_arguments(std::string_view rule);

[[nodiscard]] bool agent_type_matches_permission_rule(
    std::string_view rule,
    std::string_view agent_type
);

[[nodiscard]] bool agent_type_allowed_by_permission_rules(
    std::string_view agent_type,
    const std::vector<std::string>& allowed_tools,
    const std::vector<std::string>& denied_tools
);

[[nodiscard]] bool tool_name_allowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& allowed_tools
);

[[nodiscard]] bool tool_name_disallowed_by_definition(
    std::string_view tool_name,
    const std::vector<std::string>& disallowed_tools
);

[[nodiscard]] std::string lowercase_copy(std::string_view value);

[[nodiscard]] bool case_insensitive_contains(std::string_view haystack, std::string_view needle);

[[nodiscard]] std::vector<std::string> available_mcp_servers_with_tools();

[[nodiscard]] std::vector<std::string> missing_required_mcp_servers(
    const std::vector<std::string>& required_patterns,
    const std::vector<std::string>& available_servers
);

[[nodiscard]] std::string prepend_initial_prompt(
    const std::optional<std::string>& initial_prompt,
    std::string_view prompt
);

[[nodiscard]] std::string format_critical_system_reminder(std::string_view reminder);

[[nodiscard]] std::optional<std::string> resolve_agent_skill_name(
    std::string_view skill_name,
    const std::vector<cc::skills::SkillDefinition>& skills,
    const cc::tools::agent_runtime::AgentDefinition& agent_definition
);

[[nodiscard]] std::string format_preloaded_skill_message(
    std::string_view skill_name,
    std::string_view content
);

[[nodiscard]] std::vector<std::string> load_preloaded_skill_messages(
    const cc::tools::agent_runtime::AgentDefinition& definition
);

[[nodiscard]] std::string format_agent_mcp_context_message(
    const std::vector<AgentMcpToolBinding>& tools
);

[[nodiscard]] std::vector<AgentMcpToolBinding> connect_agent_mcp_servers(
    const std::vector<std::string>& server_names
);

[[nodiscard]] cc::tools::NativeMcpConfiguredServer to_native_agent_mcp_server(
    const cc::tools::agent_runtime::AgentInlineMcpServerConfig& config
);

void append_unique_agent_mcp_server(
    std::vector<std::string>& names,
    std::string name
);

[[nodiscard]] std::expected<std::vector<AgentInlineMcpServerRuntimeState>, std::string>
prepare_agent_inline_mcp_servers(
    const std::vector<cc::tools::agent_runtime::AgentInlineMcpServerConfig>& configs
);

[[nodiscard]] std::string format_agent_runtime_context(
    const AgentExecutionPlan& plan
);

[[nodiscard]] std::string shell_quote(std::string_view value);

[[nodiscard]] std::string sanitized_agent_file_part(std::string_view agent_id);

[[nodiscard]] std::string default_agent_transcript_path(std::string_view agent_id);

struct AgentWorktreeInfo {
    fs::path path;
    std::string branch;
    std::string head_commit;
    fs::path git_root;
};

[[nodiscard]] std::expected<AgentWorktreeInfo, std::string> create_agent_worktree(
    const AgentExecutionPlan& plan
);

[[nodiscard]] bool hook_condition_allows(const cc::tools::agent_runtime::AgentHookCommand& hook);

[[nodiscard]] bool hook_pattern_matches_one(std::string_view pattern, std::string_view value);

[[nodiscard]] bool hook_pattern_matches(
    const std::optional<std::string>& pattern,
    std::string_view value
);

struct AgentHookRunResult {
    int exit_code = 0;
    std::string output;
};

struct AgentToolHookContext {
    std::string_view tool_name;
    std::string_view tool_input_json;
    std::string_view tool_use_id;
    std::string_view tool_output_preview;
    std::string_view tool_error;
};

[[nodiscard]] AgentHookRunResult run_agent_command_hook(
    const cc::tools::agent_runtime::AgentHookCommand& hook,
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message,
    std::optional<AgentToolHookContext> tool_context = std::nullopt
);

[[nodiscard]] std::string agent_hook_output_preview(std::string_view output);

struct AgentHookExecutionResult {
    int hook_count = 0;
    std::string output;
    std::vector<std::string> additional_contexts;
    std::optional<std::string> error;
    std::optional<std::string> updated_input_json;
    std::optional<std::string> updated_mcp_tool_output_text;
    bool prevent_continuation = false;
    std::optional<std::string> stop_reason;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

struct AgentHookContinuationStop {
    std::optional<std::string> stop_reason;
};

[[nodiscard]] std::optional<AgentHookContinuationStop> hook_continuation_stop_from_output(
    std::string_view output
);

[[nodiscard]] std::optional<std::string> hook_additional_context_from_output(std::string_view output);

[[nodiscard]] std::optional<std::string> pre_tool_hook_denial_reason(std::string_view output);

[[nodiscard]] std::optional<std::string> pre_tool_hook_updated_input_json(std::string_view output);

[[nodiscard]] std::optional<std::string> post_tool_hook_updated_mcp_output_text(std::string_view output);

[[nodiscard]] std::optional<std::string> hook_additional_context_for_event(
    std::string_view output,
    std::string_view event_name
);

[[nodiscard]] AgentHookExecutionResult execute_agent_frontmatter_hooks(
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view last_assistant_message = {}
);

[[nodiscard]] AgentHookExecutionResult execute_agent_tool_frontmatter_hooks(
    const AgentExecutionPlan& plan,
    std::string_view event,
    std::string_view tool_name,
    std::string_view tool_input_json,
    std::string_view tool_use_id,
    std::string_view tool_output_preview = {},
    std::string_view tool_error = {}
);

[[nodiscard]] std::string format_tool_hook_additional_context(
    std::string_view event,
    std::string_view tool_name,
    std::string_view tool_use_id,
    const std::vector<std::string>& contexts
);

[[nodiscard]] std::string format_hook_additional_context_message(
    const std::vector<std::string>& contexts
);

void append_hook_additional_context_messages(
    std::vector<Message>& messages,
    const std::vector<std::string>& contexts
);

[[nodiscard]] std::expected<std::optional<std::string>, std::string> normalize_agent_cwd(
    const std::optional<std::string>& cwd
);

void upsert_agent_record_for_plan(const AgentExecutionPlan& plan);

struct AgentWorktreeCleanupResult {
    bool attempted = false;
    bool removed = false;
    bool changed = false;
    std::string message;
};

[[nodiscard]] AgentWorktreeCleanupResult cleanup_agent_worktree(std::string_view agent_id);

[[nodiscard]] std::string agent_output_file_path(std::string_view agent_id);

[[nodiscard]] cc::tools::MemberRole teammate_role_for_agent_type(std::string_view agent_type);

[[nodiscard]] std::optional<cc::utils::swarm_backends::AgentColor> teammate_agent_color(
    const std::optional<std::string>& color
);

[[nodiscard]] std::string teammate_parent_session_id();

[[nodiscard]] std::string tool_result_content_text(const ToolResult& result);

void update_teammate_completion_status(
    const AgentExecutionPlan& plan,
    bool success,
    const std::string& result_text
);

[[nodiscard]] std::string message_content_text(const Message& message);

[[nodiscard]] std::string normalized_tool_input_json(std::string_view raw_json);

[[nodiscard]] std::string message_content_sidechain_json(const Message& message);

[[nodiscard]] std::string json_string_member(
    cc::utils::json::JsonVal object,
    std::string_view key
);

[[nodiscard]] std::string text_from_json_content(cc::utils::json::JsonVal content);

[[nodiscard]] ContentBlock content_block_from_json(cc::utils::json::JsonVal block);

[[nodiscard]] std::optional<Message> message_from_json_value(cc::utils::json::JsonVal root);

[[nodiscard]] std::vector<Message> fork_context_messages_from_entries(
    const std::vector<std::string>& entries
);

[[nodiscard]] std::vector<std::string> tool_use_ids_in_message(const Message& message);

[[nodiscard]] std::vector<std::string> tool_result_ids_in_message(const Message& message);

[[nodiscard]] std::vector<Message> filter_resume_unresolved_tool_use_messages(
    std::vector<Message> messages
);

[[nodiscard]] bool message_is_thinking_only(const Message& message);

[[nodiscard]] std::vector<Message> filter_resume_orphaned_thinking_messages(
    std::vector<Message> messages
);

[[nodiscard]] bool message_is_whitespace_only_assistant(const Message& message);

void append_merged_user_message(std::vector<Message>& messages, Message message);

[[nodiscard]] std::vector<Message> filter_resume_whitespace_assistant_messages(
    std::vector<Message> messages
);

// Filters out assistant messages that contain `tool_use` blocks for which NO
// matching `tool_result` block exists in the transcript.
//
// Mirrors TS filterIncompleteToolCalls in runAgent.ts. This is stricter than
// filter_resume_unresolved_tool_use_messages (which only drops an assistant
// message when ALL of its tool_uses are unresolved): here a single orphaned
// tool_use is enough to exclude the entire assistant message, because the
// Anthropic API rejects requests where any tool_use is missing its result.
//
// Use this when splicing a parent's conversation history into a sub-agent's
// context (fork / resume paths) to avoid sending malformed message sequences.
[[nodiscard]] std::vector<Message> filter_incomplete_tool_calls(
    std::vector<Message> messages
);

[[nodiscard]] std::unordered_map<std::string, std::string> resume_content_replacements_from_entries(
    const std::vector<std::string>& entries
);

void apply_resume_content_replacements(
    std::vector<Message>& messages,
    const std::unordered_map<std::string, std::string>& replacements
);

struct AgentContentReplacementState {
    std::unordered_set<std::string> seen_ids;
    std::unordered_map<std::string, std::string> replacements;
};

struct AgentToolResultCandidate {
    std::size_t message_index = 0;
    std::size_t block_index = 0;
    std::string tool_use_id;
    std::size_t size = 0;
};

struct AgentToolResultBudgetOutcome {
    std::size_t newly_replaced = 0;
    std::size_t reapplied = 0;
};

inline constexpr std::size_t AGENT_MAX_TOOL_RESULTS_PER_MESSAGE_CHARS = 200'000;
inline constexpr std::size_t AGENT_DEFAULT_TOOL_RESULT_THRESHOLD_CHARS = 50'000;
inline constexpr std::string_view AGENT_PERSIST_THRESHOLD_OVERRIDE_FLAG = "tengu_satin_quoll";
inline constexpr std::string_view AGENT_PER_MESSAGE_BUDGET_OVERRIDE_FLAG = "tengu_hawthorn_window";

[[nodiscard]] bool agent_growthbook_env_overrides_enabled();

[[nodiscard]] std::optional<std::size_t> json_positive_size_t(
    cc::utils::json::JsonVal value
);

template <typename Fn>
inline void with_agent_growthbook_env_overrides(Fn&& fn) {
    if (!agent_growthbook_env_overrides_enabled()) return;
    const char* raw = std::getenv("LOOM_INTERNAL_FC_OVERRIDES");
    if (!raw || !*raw) return;
    auto parsed = cc::utils::json::parse(raw);
    if (!parsed || !parsed->root().is_obj()) return;
    fn(parsed->root());
}

[[nodiscard]] std::optional<std::size_t> agent_tool_threshold_override(
    std::string_view tool_name
);

[[nodiscard]] std::size_t agent_per_message_budget_limit();

[[nodiscard]] AgentContentReplacementState agent_content_replacement_state_from_entries(
    const std::vector<std::string>& entries
);

void mark_seen_tool_result_ids(
    AgentContentReplacementState& state,
    const std::vector<Message>& messages
);

[[nodiscard]] bool tool_result_already_replaced(std::string_view text);

[[nodiscard]] std::unordered_set<std::string> unbounded_tool_result_budget_names(
    const std::vector<ToolDefinition>& definitions
);

[[nodiscard]] std::unordered_map<std::string, std::size_t> tool_result_budget_thresholds(
    const std::vector<ToolDefinition>& definitions
);

[[nodiscard]] std::unordered_map<std::string, std::string> tool_name_by_tool_use_id(
    const std::vector<Message>& messages
);

[[nodiscard]] bool should_skip_agent_budget_candidate(
    const std::unordered_map<std::string, std::string>& tool_names,
    const std::unordered_set<std::string>& skip_tool_names,
    std::string_view tool_use_id
);

[[nodiscard]] std::size_t agent_budget_threshold_for_candidate(
    const std::unordered_map<std::string, std::string>& tool_names,
    const std::unordered_map<std::string, std::size_t>& thresholds,
    std::string_view tool_use_id
);

[[nodiscard]] std::vector<std::vector<AgentToolResultCandidate>> collect_agent_budget_candidates_by_message(
    const std::vector<Message>& messages
);

[[nodiscard]] fs::path agent_tool_result_path(
    std::string_view agent_id,
    std::string_view tool_use_id
);

[[nodiscard]] std::optional<std::string> build_agent_tool_result_replacement(
    std::string_view agent_id,
    const AgentToolResultCandidate& candidate,
    std::string_view content
);

[[nodiscard]] std::string agent_content_replacement_entry_json(
    std::string_view agent_id,
    const std::vector<std::pair<std::string, std::string>>& replacements
);

AgentToolResultBudgetOutcome apply_agent_tool_result_budget(
    std::string_view agent_id,
    std::vector<Message>& messages,
    AgentContentReplacementState& state,
    const std::unordered_set<std::string>& skip_tool_names = {},
    const std::unordered_map<std::string, std::size_t>& tool_thresholds = {}
);

[[nodiscard]] std::vector<Message> resume_messages_from_sidechain_entries(
    const std::vector<std::string>& entries
);

[[nodiscard]] std::string message_json_object(const Message& message);

void append_agent_sidechain_message(std::string_view agent_id, const Message& message);

} // namespace cc::tools::agent::utils
