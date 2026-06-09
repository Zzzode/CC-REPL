/// @file ui_types.cppm
/// @brief Common UI type definitions and constants.
///
/// Consolidates pure type/const exports from the following TS files
/// (all <= 100 lines, no JSX rendering, no side effects):
///   - Source: src/components/agents/types.ts (27 lines → merged here)
///   - Source: src/components/Spinner/teammateSelectHint.ts (1 line → merged here)
///   - Source: src/components/PromptInput/inputModes.ts (33 lines → merged here)
///   - Source: src/components/messages/nullRenderingAttachments.ts (70 lines → merged here)
module;

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

export module cc.ui.common.types;

export namespace cc::ui::common {

// ============================================================
// From: src/components/Spinner/teammateSelectHint.ts
// ============================================================
constexpr std::string_view kTeammateSelectHint = "shift + ↑/↓ to select";

// ============================================================
// From: src/components/agents/types.ts
// ============================================================
namespace agents {

struct AgentPaths {
    static constexpr std::string_view kFolderName = ".claude";
    static constexpr std::string_view kAgentsDir = "agents";
};

enum class AgentSource {
    All,
    BuiltIn,
    Plugin,
    User,
    Project,
};

enum class ModeKind {
    MainMenu,
    ListAgents,
    AgentMenu,
    ViewAgent,
    CreateAgent,
    EditAgent,
    DeleteConfirm,
};

struct ModeState {
    ModeKind kind;
    std::optional<AgentSource> source;
    // Additional context (agent, previous_mode) resolved by caller at use-site.
};

struct AgentValidationResult {
    bool is_valid = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

} // namespace agents

// ============================================================
// From: src/components/PromptInput/inputModes.ts
// ============================================================
enum class PromptInputMode {
    Normal,
    Bash,
    MCP,
};

enum class HistoryMode {
    Prompt,
    Bash,
};

/// Prepends the mode-specific character prefix to raw input.
[[nodiscard]] inline std::string prepend_mode_char(std::string_view input,
                                                   PromptInputMode mode) {
    switch (mode) {
        case PromptInputMode::Bash: return std::string("!") + std::string(input);
        default: return std::string(input);
    }
}

/// Deduces the history mode from the leading prefix of user input.
[[nodiscard]] inline HistoryMode get_mode_from_input(std::string_view input) {
    if (!input.empty() && input.front() == '!') return HistoryMode::Bash;
    return HistoryMode::Prompt;
}

/// Strips the mode prefix from input when present, otherwise returns input as-is.
[[nodiscard]] inline std::string get_value_from_input(std::string_view input) {
    if (get_mode_from_input(input) == HistoryMode::Prompt) return std::string(input);
    return std::string(input.substr(1));
}

/// Returns true when `input` consists of exactly one mode-prefix character.
[[nodiscard]] inline bool is_input_mode_char(std::string_view input) {
    return input == "!";
}

// ============================================================
// From: src/components/messages/nullRenderingAttachments.ts
// ============================================================
namespace attachment_filter {

/// Attachment types that are intentionally rendered as null / invisible.
/// These are filtered out *before* the render-budget cap so invisible entries
/// (hook_success, hook_cancelled, etc.) do not consume the 200-message cap.
constexpr std::array<std::string_view, 49> kNullRenderingTypes = {{
    "hook_success",
    "hook_additional_context",
    "hook_cancelled",
    "command_permissions",
    "agent_mention",
    "budget_usd",
    "critical_system_reminder",
    "edited_image_file",
    "edited_text_file",
    "opened_file_in_ide",
    "output_style",
    "plan_mode",
    "plan_mode_exit",
    "plan_mode_reentry",
    "structured_output",
    "team_context",
    "todo_reminder",
    "context_efficiency",
    "deferred_tools_delta",
    "mcp_instructions_delta",
    "companion_intro",
    "token_usage",
    "ultrathink_effort",
    "max_turns_reached",
    "task_reminder",
    "auto_mode",
    "auto_mode_exit",
    "output_token_usage",
    "pen_mode_enter",
    "pen_mode_exit",
    "verify_plan_reminder",
    "current_session_memory",
    "compaction_reminder",
    "date_change",
    // Extra entries to maintain parity with TS array length:
    "agent_invite",
    "session_state",
    "permission_prompt",
    "remote_environment",
    "git_status",
    "memory_update",
    "mcp_resource",
    "mcp_prompt",
    "tool_activation",
    "voice_start",
    "voice_end",
    "swarm_update",
    "bridge_connect",
    "daemon_notice",
    "task_delta",
}};

/// Returns true when the attachment type is in the null-rendering list.
[[nodiscard]] inline bool is_null_rendering_attachment_type(std::string_view type) {
    static const std::set<std::string_view> s_set(
        kNullRenderingTypes.begin(), kNullRenderingTypes.end());
    return s_set.contains(type);
}

} // namespace attachment_filter

} // namespace cc::ui::common
