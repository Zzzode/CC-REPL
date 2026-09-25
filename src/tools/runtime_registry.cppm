/// @file runtime_registry.cppm
/// @brief Runtime registration for all migrated tools exposed to the query engine.
///
/// Declaration-only module interface. All non-trivial bodies live in module
/// implementation units beginning `module cc.tools.runtime_registry;`
/// (runtime_registry_{json,executors,native_agents,computer_use,skills,
/// dispatch,team_dispatch,register}.cpp, wired via a separate PRIVATE
/// target_sources block in cc_tools.cmake — never the FILE_SET CXX_MODULES
/// list). The constexpr function-pointer aliases, the inline team-artifact
/// wrappers that take function-pointer addresses, and the mutable
/// computer-use testing override variables stay in this interface.
module;

export module cc.tools.runtime_registry;

import std;

import cc.tools.tool;
import cc.tools.agent;
import cc.tools.agent_runtime;
import cc.tools.bash;
import cc.tools.computer_use;
import cc.tools.lsp;
import cc.tools.notebook;
import cc.tools.task;
import cc.tools.team;
// No name textually referenced, but removing this import crashes clang 22
// (SIGSEGV in ASTReader on a missing typedef chain while reading this BMI) —
// a PCM-reachability instance of LLVM #184957.
// arch-check: keep-import
import cc.tools.synthetic_output_tool;
import cc.tools.web_browser;
import cc.tools.runtime_message_delivery;
import cc.tools.runtime_team_shared;
import cc.tools.runtime_shared_utils;
import cc.utils.json;

export namespace cc::tools {

namespace fs = std::filesystem;

namespace detail {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::InputSchema;
using cc::core::ITool;
using cc::core::Result;
using cc::core::SchemaProperty;
using cc::core::ToolDefinition;
using cc::core::ToolInput;
using cc::core::ToolOutputContent;
using cc::core::ToolPermission;
using cc::core::ToolRegistry;
using cc::core::ToolResult;

namespace json = cc::utils::json;

using RuntimeExecutor = std::function<Result<ToolResult>(const ToolInput&)>;

class RuntimeFunctionTool final : public ITool {
public:
    RuntimeFunctionTool(
        ToolDefinition definition,
        RuntimeExecutor executor,
        cc::tools::agent::AgentLivePermissionCheckFn permission_check = {})
        : definition_(std::move(definition)),
          executor_(std::move(executor)),
          permission_check_(std::move(permission_check)) {}

    [[nodiscard]] const ToolDefinition& definition() const override {
        return definition_;
    }

    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) override {
        return executor_(input);
    }

    // Key function: defined out-of-line in runtime_registry_register.cpp.
    [[nodiscard]] bool check_permission(const ToolInput& input) const override;

private:
    ToolDefinition definition_;
    RuntimeExecutor executor_;
    cc::tools::agent::AgentLivePermissionCheckFn permission_check_;
};

[[nodiscard]] ToolDefinition define_tool(
    std::string name,
    std::string description,
    ToolPermission permission,
    std::vector<SchemaProperty> properties = {},
    std::string category = "runtime"
);

[[nodiscard]] std::unique_ptr<ITool> make_runtime_tool(
    std::string name,
    std::string description,
    ToolPermission permission,
    std::vector<SchemaProperty> properties,
    RuntimeExecutor executor,
    std::string category = "runtime",
    cc::tools::agent::AgentLivePermissionCheckFn permission_check = {}
);

// Ad-hoc JSON field accessors over a raw JSON string; definitions in
// runtime_registry_json.cpp.

[[nodiscard]] std::optional<std::string> json_string(std::string_view json, std::string_view key);

[[nodiscard]] std::optional<int> json_int(std::string_view json, std::string_view key);

[[nodiscard]] bool json_bool(std::string_view json, std::string_view key, bool fallback = false);

[[nodiscard]] std::optional<std::string> runtime_json_string(cc::utils::json::JsonVal obj, std::string_view key);

[[nodiscard]] std::optional<int> runtime_json_int(cc::utils::json::JsonVal obj, std::string_view key);

[[nodiscard]] std::optional<bool> runtime_json_bool(cc::utils::json::JsonVal obj, std::string_view key);

[[nodiscard]] std::optional<bool> runtime_json_semantic_bool(
    cc::utils::json::JsonVal obj,
    std::string_view key
);

[[nodiscard]] std::vector<std::string> runtime_json_event_array(cc::utils::json::JsonVal obj, std::string_view key);

[[nodiscard]] std::vector<std::string> json_string_array(std::string_view json, std::string_view key);

[[nodiscard]] std::optional<std::string> json_raw_value(std::string_view json, std::string_view key);

[[nodiscard]] std::optional<std::size_t> parse_notebook_cell_index(std::string_view text);

[[nodiscard]] std::optional<std::size_t> resolve_notebook_cell_index(
    const Notebook& notebook,
    std::optional<std::string> cell_id
);

[[nodiscard]] std::optional<CellOperation> parse_notebook_operation(std::string_view text);

constexpr auto runtime_shell_quote = &runtime_shared_utils::shell_quote;

[[nodiscard]] Result<ToolResult> run_command(std::string command, std::size_t max_bytes = 1024 * 512);

[[nodiscard]] std::string join_args(const std::vector<std::string>& args);

[[nodiscard]] std::vector<std::string> read_lines(const fs::path& file);

[[nodiscard]] std::string word_at_position(const std::vector<std::string>& lines, int line_no, int character);

[[nodiscard]] bool is_source_file(const fs::path& path);

[[nodiscard]] LspAction parse_lsp_action(std::string_view action);

[[nodiscard]] std::string format_lsp_result(const LspResult& result, std::string_view action);

[[nodiscard]] Result<ToolResult> execute_lsp_tool(const ToolInput& input);

[[nodiscard]] Result<ToolResult> execute_script(const ToolInput& input);

[[nodiscard]] TaskStatus parse_task_status(std::string_view status);

[[nodiscard]] std::string format_task_summary(const Task& task);

constexpr auto escape_xml_text = &runtime_shared_utils::escape_xml;

[[nodiscard]] bool is_native_agent_task(const agent_runtime::NativeAgentRecord& record);

[[nodiscard]] std::string native_agent_display_name(const agent_runtime::NativeAgentRecord& record);

[[nodiscard]] std::string native_agent_output_file(const agent_runtime::NativeAgentRecord& record);

constexpr auto runtime_delivery_message_id = &runtime_shared_utils::runtime_delivery_message_id;

constexpr auto format_agent_pending_user_message = &runtime_shared_utils::format_agent_pending_user_message;

constexpr auto format_team_task_assignment_message = &runtime_team_shared::format_team_task_assignment_message;

// Defined exactly once in runtime_registry_native_agents.cpp; their addresses
// are taken by the inline wrappers below (terminal-status predicate and
// transcript-artifact cleanup).
[[nodiscard]] bool native_agent_status_is_terminal(agent_runtime::NativeAgentStatus status);

[[nodiscard]] std::size_t cleanup_native_agent_transcript_artifacts(
    const agent_runtime::NativeAgentRecord& record
);

constexpr auto safe_runtime_dir_component = &runtime_shared_utils::safe_runtime_dir_component;

constexpr auto ts_sanitized_team_dir_name = &runtime_team_shared::ts_sanitized_team_dir_name;

constexpr auto path_has_prefix = &runtime_shared_utils::path_has_prefix;

constexpr auto normalized_absolute_path = &runtime_shared_utils::normalized_absolute_path;

using TeamDeletionCleanupSummary = runtime_team_shared::TeamDeletionCleanupSummary;
using TeamCreationArtifactsSummary = runtime_team_shared::TeamCreationArtifactsSummary;

constexpr auto team_member_inbox_name = &runtime_team_shared::team_member_inbox_name;
constexpr auto write_empty_inbox_if_missing = &runtime_team_shared::write_empty_inbox_if_missing;
constexpr auto write_team_task_snapshot = &runtime_team_shared::write_team_task_snapshot;
constexpr auto team_agent_name_from_id = &runtime_team_shared::team_agent_name_from_id;
constexpr auto team_lead_agent_id = &runtime_team_shared::team_lead_agent_id;

using TeamConfigMemberRuntimeState = runtime_team_shared::TeamConfigMemberRuntimeState;

constexpr auto write_team_config_optional_string = &runtime_team_shared::write_team_config_optional_string;
constexpr auto write_team_config_optional_bool = &runtime_team_shared::write_team_config_optional_bool;
constexpr auto write_team_config_member = &runtime_team_shared::write_team_config_member;
constexpr auto write_team_config_file = &runtime_team_shared::write_team_config_file;

/// Wrap the extracted function (which takes an explicit terminal-status
/// predicate) so call sites keep their zero-argument signature.
[[nodiscard]] inline std::unordered_map<std::string, TeamConfigMemberRuntimeState>
team_config_runtime_states_from_native_records(
    std::span<const agent_runtime::NativeAgentRecord> records
) {
    return runtime_team_shared::team_config_runtime_states_from_native_records(
        records,
        &native_agent_status_is_terminal);
}

constexpr auto ensure_team_runtime_artifacts = &runtime_team_shared::ensure_team_runtime_artifacts;
constexpr auto contains_agent_id = &runtime_team_shared::contains_agent_id;
constexpr auto collect_team_native_agents = &runtime_team_shared::collect_team_native_agents;

/// Wrap the extracted function (which takes two explicit predicate/cleanup
/// function pointers) so call sites keep their three-argument signature.
[[nodiscard]] inline TeamDeletionCleanupSummary cleanup_team_runtime_artifacts(
    std::string_view team_id,
    std::string_view team_name,
    std::span<const agent_runtime::NativeAgentRecord> records
) {
    return runtime_team_shared::cleanup_team_runtime_artifacts(
        team_id,
        team_name,
        records,
        &native_agent_status_is_terminal,
        &cleanup_native_agent_transcript_artifacts);
}

[[nodiscard]] std::string format_native_agent_task_summary(const agent_runtime::NativeAgentRecord& record);

[[nodiscard]] std::optional<std::string> native_agent_notification_status(
    agent_runtime::NativeAgentStatus status);

[[nodiscard]] std::optional<std::string> format_native_agent_task_notification(
    const agent_runtime::NativeAgentRecord& record);

[[nodiscard]] std::string format_native_agent_task_output(const agent_runtime::NativeAgentRecord& record);

[[nodiscard]] std::string background_task_status(const bash::BackgroundTaskSnapshot& task);

[[nodiscard]] std::string format_background_task_summary(
    const bash::BackgroundTaskSnapshot& task,
    bool include_output);

[[nodiscard]] Result<ToolResult> execute_task_tool(std::string_view tool_name, const ToolInput& input);

[[nodiscard]] fs::path config_path();

[[nodiscard]] Result<ToolResult> execute_config_tool(const ToolInput& input);

[[nodiscard]] Result<ToolResult> execute_skill_tool(const ToolInput& input);

[[nodiscard]] Result<ToolResult> execute_local_resource_read(const ToolInput& input);

[[nodiscard]] Result<ToolResult> execute_resource_list(const ToolInput& input);

[[nodiscard]] std::vector<std::string> runtime_tool_names_impl();

[[nodiscard]] Result<ToolResult> execute_tool_search(const ToolInput& input);

[[nodiscard]] std::optional<cc::tools::BrowserAction> parse_browser_action(std::string_view action);

[[nodiscard]] std::vector<cc::tools::FormField> json_form_fields(std::string_view json);

[[nodiscard]] Result<ToolResult> execute_web_browser(const ToolInput& input);

[[nodiscard]] std::optional<cc::core::computer_use::ActionType> parse_computer_action(
    std::string_view action);

[[nodiscard]] std::expected<std::string, std::string> run_computer_use_command_backend(
    const cc::core::computer_use::ComputerAction& action
);

[[nodiscard]] std::optional<std::string> computer_json_optional_string(
    cc::utils::json::JsonVal root,
    std::string_view key
);

struct ComputerUseCommandBackendResult {
    std::optional<std::string> screenshot_base64;
    std::optional<std::string> format;
    std::optional<std::int64_t> width;
    std::optional<std::int64_t> height;
};

[[nodiscard]] std::expected<ComputerUseCommandBackendResult, std::string> parse_computer_command_result(
    std::string_view output
);

[[nodiscard]] std::optional<cc::core::computer_use::CaptureProvider> computer_use_command_capture_provider();

[[nodiscard]] std::optional<cc::core::computer_use::InputProvider> computer_use_command_input_provider();

// Mutable inline overrides read/written across implementation units.
inline std::optional<cc::core::computer_use::CaptureProvider> computer_use_capture_provider_override;
inline std::optional<cc::core::computer_use::InputProvider> computer_use_input_provider_override;

} // namespace detail

// Definitions in runtime_registry_computer_use.cpp.
void set_runtime_computer_use_capture_provider_for_testing(
    cc::core::computer_use::CaptureProvider provider);

void clear_runtime_computer_use_capture_provider_for_testing();

void set_runtime_computer_use_input_provider_for_testing(
    cc::core::computer_use::InputProvider provider);

void clear_runtime_computer_use_input_provider_for_testing();

namespace detail {

[[nodiscard]] std::string normalize_name_for_mcp(std::string name);

[[nodiscard]] std::optional<std::string>
connected_computer_use_mcp_server();

[[nodiscard]] Result<ToolResult> execute_computer_use(const ToolInput& input);

[[nodiscard]] bool safe_ref(std::string_view text);

[[nodiscard]] Result<ToolResult> execute_worktree(std::string_view mode, const ToolInput& input);

[[nodiscard]] Result<ToolResult> execute_brief(const ToolInput& input);

[[nodiscard]] Result<ToolResult> execute_notebook_edit(const ToolInput& input);

constexpr auto runtime_timestamp_string = &runtime_shared_utils::runtime_timestamp_string;

using StructuredSendMessagePayload = runtime_message_delivery::StructuredSendMessagePayload;
using RuntimePeerAddressScheme = runtime_message_delivery::RuntimePeerAddressScheme;
using RuntimePeerAddress = runtime_message_delivery::RuntimePeerAddress;

constexpr auto parse_runtime_peer_address = &runtime_message_delivery::parse_runtime_peer_address;
constexpr auto build_runtime_json_object = &runtime_message_delivery::build_runtime_json_object;
constexpr auto build_cross_session_prompt = &runtime_message_delivery::build_cross_session_prompt;
constexpr auto build_uds_cross_session_payload = &runtime_message_delivery::build_uds_cross_session_payload;
constexpr auto runtime_env_value = &runtime_message_delivery::runtime_env_value;
constexpr auto first_runtime_env = &runtime_message_delivery::first_runtime_env;
constexpr auto strip_runtime_trailing_slashes = &runtime_message_delivery::strip_runtime_trailing_slashes;
constexpr auto is_safe_runtime_session_id = &runtime_message_delivery::is_safe_runtime_session_id;
constexpr auto build_bridge_cross_session_event = &runtime_message_delivery::build_bridge_cross_session_event;
constexpr auto send_bridge_cross_session_message = &runtime_message_delivery::send_bridge_cross_session_message;
constexpr auto send_uds_cross_session_message = &runtime_message_delivery::send_uds_cross_session_message;
constexpr auto build_structured_send_message_payload = &runtime_message_delivery::build_structured_send_message_payload;
constexpr auto runtime_has_agent_api_credentials = &runtime_message_delivery::runtime_has_agent_api_credentials;
constexpr auto native_agent_can_resume_locally = &runtime_message_delivery::native_agent_can_resume_locally;
constexpr auto native_agent_resume_cwd = &runtime_message_delivery::native_agent_resume_cwd;
constexpr auto build_native_agent_resume_input_json = &runtime_message_delivery::build_native_agent_resume_input_json;

// Definition in runtime_registry_native_agents.cpp.
[[nodiscard]] std::string build_team_member_agent_start_input_json(
    const TeamMember& member,
    std::string_view team_name,
    std::string_view prompt,
    const runtime_team_shared::TeamMemberStartOptions* options,
    const std::optional<std::string>& default_cwd,
    const std::optional<std::string>& default_mode,
    const std::optional<std::string>& default_isolation
);

constexpr auto runtime_tool_result_text = &runtime_message_delivery::runtime_tool_result_text;
constexpr auto try_start_native_agent_resume = &runtime_message_delivery::try_start_native_agent_resume;

[[nodiscard]] Result<ToolResult> execute_simple_runtime_tool(
    std::string_view name,
    const ToolInput& input,
    ToolRegistry* registry = nullptr
);

// team_create / team_delete branches extracted to
// runtime_registry_team_dispatch.cpp.
[[nodiscard]] Result<ToolResult> execute_team_create_runtime_tool(
    std::string_view json,
    ToolRegistry* registry
);

[[nodiscard]] Result<ToolResult> execute_team_delete_runtime_tool(std::string_view json);

} // namespace detail

using cc::core::SchemaProperty;
using cc::core::ToolPermission;
using cc::tools::agent::AgentLivePermissionCheck;
using cc::tools::agent::AgentLivePermissionCheckFn;

struct RuntimeToolOptions {
    std::optional<std::string> parent_permission_mode;
    AgentLivePermissionCheckFn permission_check;
    bool permission_hook_valid_for_background = false;
};

[[nodiscard]] std::vector<std::string> runtime_tool_names();

[[nodiscard]] std::vector<agent_runtime::AgentDefinition>
get_built_in_agent_definitions();

[[nodiscard]] bool are_explore_plan_agents_enabled();

void register_runtime_tools(cc::core::ToolRegistry& registry, RuntimeToolOptions options);

void register_runtime_tools(cc::core::ToolRegistry& registry);

[[nodiscard]] std::vector<cc::core::ToolDefinition> collect_mcp_tool_definitions();

[[nodiscard]] std::unordered_map<std::string, std::string>
collect_mcp_input_schemas();

} // namespace cc::tools
