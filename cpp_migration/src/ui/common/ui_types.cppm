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

// Canonical VimMode lives in cc_vim (low-level target) to avoid circular
// deps: cc_hooks needs VimMode but cc_ui depends on cc_hooks.
import cc.vim.vim_types;

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
// Also: src/types/textInputTypes.ts (PromptInputMode type)
//
// UNIFIED CANONICAL ENUM — replaces incompatible definitions scattered
// across the codebase (text_input.cppm, prompt_input_full.cppm,
// prompt_input_footer.cppm, repl_screen.cppm,
// and the previous 3-value stub here).
//
// TS REF: src/types/textInputTypes.ts:265 — TS PromptInputMode is only
//   'bash' | 'prompt' | 'orphaned-permission' | 'task-notification' (4 values).
// The CPP port historically mixed orthogonal concepts (vim mode, plan mode,
// history search, prefix-triggered modes) into the same enum.  This unified
// definition preserves the full union so existing switch statements compile,
// but callers should treat vim/plan/search as LAYERED state (TS parity:
// VimMode is a separate type, plan mode is a separate flag).
// ============================================================
enum class PromptInputMode {
    Normal,             ///< Default prompt (TS: 'prompt').  Also used by
                        ///  modules that historically called this 'Prompt'.
    Bash,               ///< Shell-first (leading '!').  TS: 'bash'.
    SlashCommand,       ///< Slash-command mode (leading '/').  text_input
                        ///  called this 'Command'.
    HistorySearch,      ///< Ctrl+R reverse history search.
    PlanMode,           ///< Plan-only mode.
    FileRef,            ///< File-reference (leading '@').
    Agent,              ///< Agent mention (leading '*').
    BgRun,              ///< '&' background run.
    MCP,                ///< MCP resource mode.
    VimNormal,          ///< Vim normal mode.
    VimInsert,          ///< Vim insert mode.
    VimVisual,          ///< Vim visual mode.
    OrphanedPermission, ///< Orphaned permission prompt.  TS: 'orphaned-permission'.
    TaskNotification,   ///< Task notification overlay.  TS: 'task-notification'.
    FastMode,           ///< Fast mode (CPP extension).
    Search,             ///< Generic search mode (text_input.cppm used this
                        ///  separately from HistorySearch).
};

// NOTE: inputModes.ts helpers (prependModeCharacterToInput, getModeFromInput,
// getValueFromInput, isInputModeCharacter) live in cc::ui::design::figures —
// that module is the single source of truth for prompt-prefix glyphs and
// mode-detection utilities.  See figures.cppm PromptMode enum + 4 functions.

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

// ============================================================
// From: src/components/PromptInput/PromptInputFooter.tsx (PermissionMode)
// From: src/types/queryOptions.ts (EffortLevel)
//
// UNIFIED CANONICAL ENUMS — previously duplicated in:
//   - prompt_input_full.cppm (PermissionMode + EffortLevel)
//   - prompt_input_footer.cppm (PermissionMode)
// ============================================================

/// Permission mode for tool execution.
/// TS REF: src/types/Tool.ts — ToolPermissionContext.mode
enum class PermissionMode {
    Default,        ///< default — confirm each tool use
    AcceptEdits,    ///< 🔓 auto-accept file edits
    AcceptAll,      ///< 🔓🔓 auto-accept all tools
    Plan            ///< 📋 plan-only mode
};

/// Effort level for query execution.
/// TS REF: src/types/queryOptions.ts — effort option
enum class EffortLevel {
    Low,
    Medium,
    High,
    Auto
};

// ============================================================
// Canonical VimMode enum — re-exported from cc.vim.vim_types
//
// Canonical definition lives in cc_vim (vim/vim_types.cppm) to avoid
// circular deps (cc_hooks needs VimMode but cc_ui depends on cc_hooks).
//
// UNIFIED: replaces 5 incompatible VimMode definitions scattered across
//   - src/ui/prompt/vim_input.cppm (6 values)
//   - src/ui/prompt_input.cppm (3 values)
//   - src/vim/vim_mode.cppm (6 values)
//   - src/hooks/vim_input.cppm (5 values)
//   - src/ui/components/text_input.cppm (bool enable_vim)
//
// TS REF: src/types/textInputTypes.ts:222 — public VimMode type is
//   'INSERT' | 'NORMAL'.  Internal state machine tracks richer modes.
// TS REF: src/hooks/useVimInput.ts:36 — mode starts at 'INSERT'.
// ============================================================
using cc::vim::VimMode;
using cc::vim::is_editing_mode;
using cc::vim::is_navigation_mode;
using cc::vim::vim_mode_label;
using cc::vim::vim_mode_short_label;

} // namespace cc::ui::common
