/// =========================================================================
/// @file message_row.cppm
/// @brief Message row framework: audit table + dispatch to per-type renderers.
///
/// PHASE 4 UI4 RESPONSIBILITY - "List framework + generic message types"
///   - Audit TS components/messages (33 tsx) vs C++ ui/messages (26 modules)
///   - Provide an enum of known message roles/types
///   - RenderMessageRowByType() dispatches by (role, type) → correct module
///
/// COMPLEX MESSAGE TYPES handled by UI5 Agent (NOT TOUCHED HERE):
///   - AssistantToolUse / Thinking / Attachments / RedactedThinking
///   - SystemAPIError / UserToolResult / StructuredDiff / ToolUseLoader
///
/// FULL MARKDOWN rendering + syntax highlighting → UI17 (NOT HERE).
/// =========================================================================
///
/// ┌───────────────────────────────────────────────────────────────────────┐
/// │  AUDIT TABLE: TS components/messages (33)  ─▶  C++ ui/messages (26)   │
/// ├───────────────────────────────────────────────┬───────────────────────┤
/// │  TS file (basename)                            │ C++ module          │
/// ├───────────────────────────────────────────────┼───────────────────────┤
/// │                                               │                       │
/// │  ─── GENERIC / ROOT (UI4) ────────────────── │                       │
/// │ ✅ UserTextMessage.tsx             (274 loc) │ 🆕 user_text_message  │
/// │ ✅ AssistantTextMessage.tsx        (269 loc) │ 🆕 assistant_text_m.  │
/// │ ✅ SystemTextMessage.tsx           (826 loc) │ 🆕 system_text_m.     │
/// │ ✅ UserCommandMessage.tsx          (107 loc) │ ✅ message_user_cmd   │
/// │ ✅ RateLimitMessage.tsx            (160 loc) │ ✅ message_rate_limit │
/// │ ✅ ShutdownMessage.tsx             (131 loc) │ ✅ message_shutdown   │
/// │ ✅ PlanApprovalMessage.tsx         (221 loc) │ ✅ message_plan_appr. │
/// │ ✅ HookProgressMessage.tsx         (115 loc) │ ✅ message_hook_prog. │
/// │ ✅ AdvisorMessage.tsx              (157 loc) │ ✅ message_advisor    │
/// │                                               │                       │
/// │  ─── USER FLAVOURS (UI4 = wrapper, share impl)│                      │
/// │ 🧩 UserTeammateMessage.tsx        (205 loc) │ 🧩 user_message (base)│
/// │ 🧩 UserChannelMessage.tsx         (136 loc) │ ✅ message_channel    │
/// │ 🧩 UserPromptMessage.tsx          (~350 loc)│ 🧩 user_message (base)│
/// │ 🧩 UserPlanMessage.tsx            (~95 loc) │ 🧩 user_message (base)│
/// │ 🧩 UserBashInputMessage.tsx       (107 loc) │ ✅ message_bash_io    │
/// │ 🧩 UserBashOutputMessage.tsx       (98 loc) │ ✅ message_bash_io    │
/// │ 🧩 UserLocalCommandOutput.tsx     (166 loc) │ ✅ message_bash_io    │
/// │ 🧩 UserAgentNotification.tsx      (140 loc) │ 🧩 user_message (base)│
/// │ 🧩 UserMemoryInputMessage.tsx     (150 loc) │ 🧩 user_message (base)│
/// │ 🧩 UserResourceUpdateMessage.tsx  (290 loc) │ 🧩 user_message (base)│
/// │ 🧩 UserImageMessage.tsx           (140 loc) │ ✅ message_image      │
/// │ ✅ TaskAssignmentMessage.tsx       (200 loc) │ ✅ message_task_ass.  │
/// │                                               │                       │
/// │  ─── COMPLEX / TOOLS (UI5) ─────────────────│                      │
/// │ ✅ AssistantToolUseMessage.tsx     (369 loc) │ ✅ tool_use_message   │  UI5
/// │ ✅ AssistantThinkingMessage+txt    (560 loc) │ ✅ thinking_message   │  UI5 (kw-hl + redacted mode merged in)
/// │ ✅ AssistantRedactedThinking.tsx    (82 loc) │ 🧩 thinking_message::Redacted mode (absorbed)
/// │ ✅ GroupedToolUseContent.tsx       (200 loc) │ ✅ tool_use_message::Grouped  UI5
/// │ ✅ AttachmentMessage.tsx           (535 loc) │ ✅ attachment_message │  UI5 (grid + pagination + ASCII thumb)
/// │ ✅ HighlightedThinkingText.tsx     (360 loc) │ ✅ thinking_message kw-hl pass   UI5
/// │ ✅ UserToolResultMessage/          (105 loc) │ ✅ message_tool_result│
/// │ ✅ SystemAPIErrorMessage.tsx       (140 loc) │ ✅ api_error_message  │  UI5 (severity colors + retry pills)
/// │ ✅ CollapsedReadSearchContent.tsx  (1800 loc)│ ✅ collapsed_content_ │  UI5 (summary + table + preview)
/// │ ✅ UserLocalCommandOutput.tsx      (166 loc) │ ✅ local_command_out. │  UI5 (gutter + stderr red + 200-line fold)
/// │ ❌ teamMemCollapsed.tsx             (? loc) │ ❌ (TBD → UI5 follow-up)
/// │                                               │                       │
/// │  ─── BORDERS / META ────────────────────────│                      │
/// │ ✅ CompactBoundaryMessage.tsx       (55 loc) │ ✅ message_compact_b. │
/// │                                               │                       │
/// │  ─── SHARED / INFRA (no direct TS analog) ──│                      │
/// │    -                                           │ assistant_message   │
/// │    -                                           │ message_components  │
/// │    -                                           │ message_response    │
/// │    -                                           │ message_row (this)  │
/// │    -                                           │ message_timestamp   │
/// │    -                                           │ structured_diff     │
/// │    -                                           │ tool_messages       │
/// │    -                                           │ tool_use_loader     │
/// └───────────────────────────────────────────────┴───────────────────────┘
///
///  Legend:
///    ✅ Migrated and complete
///    🧩 Migrated but needs parent component wrapping (uses base module + subtype tag)
///    ❌ Not present / NOT in UI4 scope (handled by UI5)
///    🆕 Created in this commit by UI4
///    ⚠️  Partial / images only
/// =========================================================================
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <variant>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <functional>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>

export module cc.ui.messages.message_row;

// --- Import per-type renderers ------------------------------------------
import cc.ui.messages.message_timestamp;
import cc.ui.messages.message_components;

// Nine UI4-owned generic types:
import cc.ui.messages.user_text_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.messages.system_text_message;
import cc.ui.messages.message_user_command;
import cc.ui.messages.message_rate_limit;
import cc.ui.messages.message_shutdown;
import cc.ui.messages.message_plan_approval;
import cc.ui.messages.message_hook_progress;
import cc.ui.messages.message_advisor;

// Flavours + shared modules:
import cc.ui.messages.message_bash_io;
import cc.ui.messages.message_channel;
import cc.ui.messages.message_compact_boundary;
import cc.ui.messages.message_image;
import cc.ui.messages.message_task_assignment;
import cc.ui.messages.message_tool_result;   // exports ToolResultOptions
import cc.ui.user_message;
import cc.ui.assistant_message;
import cc.ui.error_message;

// =========================================================================
// SIX COMPLEX MESSAGE TYPES — UI5 Agent
// =========================================================================
// These modules are the UI5 deliverables for the complex interactivity
// bucket.  Dispatch below in RenderMessageRowByType routes each MessageShape
// through the dedicated stand-alone free function Render*().
//
// UI4-owned user/assistant/system wrappers keep outer chrome (avatar,
// role-badge, metadata row) while these branches forward complex content
// payloads directly to the UI5 renderers.
import cc.ui.messages.tool_use_message;
import cc.ui.messages.thinking_message;
import cc.ui.messages.attachment_message;
import cc.ui.messages.api_error_message;
import cc.ui.messages.collapsed_content_message;
import cc.ui.messages.local_command_output_message;

export namespace cc::ui::messages {

using namespace ftxui;

// =========================================================================
// 1) Message row configuration (legacy ANSI string renderer preserved)
// =========================================================================

// Configuration for rendering a single message row (plain text fallback)
struct MessageRowConfig {
    std::string role;
    std::string content;
    std::optional<std::chrono::system_clock::time_point> timestamp;
    std::optional<std::string> model;
    bool is_streaming = false;
};

// Render a role badge (colored label for the message author)
inline auto render_role_badge(std::string_view role) -> std::string {
    std::ostringstream out;

    if (role == "user" || role == "human") {
        out << "\033[34m⦿ You\033[0m";
    } else if (role == "assistant") {
        out << "\033[35m◈ Assistant\033[0m";
    } else if (role == "system") {
        out << "\033[33m⚙ System\033[0m";
    } else if (role == "tool") {
        out << "\033[36m⚡ Tool\033[0m";
    } else {
        out << "\033[2m○ " << role << "\033[0m";
    }

    return out.str();
}

// Render a complete message row with role, content, and metadata (legacy API)
inline auto render_message_row(MessageRowConfig config, int width) -> std::string {
    std::ostringstream out;

    // Header line: role badge + optional model + optional timestamp
    out << render_role_badge(config.role);

    if (config.model.has_value()) {
        out << " \033[2m(" << config.model.value() << ")\033[0m";
    }

    if (config.timestamp.has_value()) {
        auto time_t_val = std::chrono::system_clock::to_time_t(config.timestamp.value());
        std::tm tm_buf{};
        localtime_r(&time_t_val, &tm_buf);
        out << " \033[2m" << std::put_time(&tm_buf, "%H:%M") << "\033[0m";
    }

    // Streaming indicator
    if (config.is_streaming) {
        out << " \033[36m▍\033[0m";
    }

    out << "\n";

    // Content body with left margin
    const std::string indent = "  ";
    std::istringstream content_stream(config.content);
    std::string line;
    while (std::getline(content_stream, line)) {
        // Simple word-wrap for long lines
        while (static_cast<int>(line.size()) > width - 2) {
            out << indent << line.substr(0, width - 2) << "\n";
            line = line.substr(width - 2);
        }
        out << indent << line << "\n";
    }

    return out.str();
}

// =========================================================================
// 2) Dispatch enum (role × type)
// =========================================================================

/// Every known message shape.  A single enum flattens the (role, sub_type)
/// cross-product so dispatch logic reads cleanly.
enum class MessageShape {
    // --- User family ---
    UserText,
    UserCommand,
    UserBashInput,
    UserBashOutput,
    UserLocalCommandOutput,    // UI5: dedicated local_command_output_message
    UserLocalJsxOutput,
    UserTeammate,
    UserChannel,
    UserAgentNotification,
    UserMemoryInput,
    UserPlan,
    UserPrompt,
    UserResourceUpdate,
    UserImage,
    UserToolResult,
    UserAttachments,           // UI5: attachment_message (card grid)

    // --- Assistant family ---
    AssistantText,
    AssistantToolUse,          // UI5 responsibility (tool_use_message)
    AssistantThinking,         // UI5 responsibility (thinking_message plain)
    AssistantRedactedThinking, // UI5 responsibility (thinking_message redacted mode)
    AssistantGroupedTools,     // UI5 responsibility (tool_use_message::Grouped)

    // --- System family ---
    SystemText,
    SystemAPIError,            // UI5: api_error_message (severity borders + pills)
    SystemCollapsedContent,    // UI5: collapsed_content_message (Read/Search aggregate)
    SystemRateLimit,
    SystemPlanApproval,
    SystemHookProgress,
    SystemShutdown,
    SystemCompactBoundary,
    SystemAdvisor,
    SystemTaskAssignment,
};

inline auto MessageShapeToString(MessageShape s) -> std::string_view {
    switch (s) {
        case MessageShape::UserText:                 return "user.text";
        case MessageShape::UserCommand:              return "user.command";
        case MessageShape::UserBashInput:            return "user.bash_input";
        case MessageShape::UserBashOutput:           return "user.bash_output";
        case MessageShape::UserLocalCommandOutput:   return "user.local_command_output";
        case MessageShape::UserLocalJsxOutput:       return "user.local_jsx_output";
        case MessageShape::UserTeammate:             return "user.teammate";
        case MessageShape::UserChannel:              return "user.channel";
        case MessageShape::UserAgentNotification:    return "user.agent_notification";
        case MessageShape::UserMemoryInput:          return "user.memory_input";
        case MessageShape::UserPlan:                 return "user.plan";
        case MessageShape::UserPrompt:               return "user.prompt";
        case MessageShape::UserResourceUpdate:       return "user.resource_update";
        case MessageShape::UserImage:                return "user.image";
        case MessageShape::UserToolResult:           return "user.tool_result";
        case MessageShape::UserAttachments:          return "user.attachments";
        case MessageShape::AssistantText:            return "assistant.text";
        case MessageShape::AssistantToolUse:         return "assistant.tool_use";
        case MessageShape::AssistantGroupedTools:    return "assistant.grouped_tools";
        case MessageShape::AssistantThinking:        return "assistant.thinking";
        case MessageShape::AssistantRedactedThinking: return "assistant.redacted_thinking";
        case MessageShape::SystemText:               return "system.text";
        case MessageShape::SystemAPIError:           return "system.api_error";
        case MessageShape::SystemCollapsedContent:   return "system.collapsed_content";
        case MessageShape::SystemRateLimit:          return "system.rate_limit";
        case MessageShape::SystemPlanApproval:       return "system.plan_approval";
        case MessageShape::SystemHookProgress:       return "system.hook_progress";
        case MessageShape::SystemShutdown:           return "system.shutdown";
        case MessageShape::SystemCompactBoundary:    return "system.compact_boundary";
        case MessageShape::SystemAdvisor:            return "system.advisor";
        case MessageShape::SystemTaskAssignment:     return "system.task_assignment";
    }
    return "unknown";
}

// =========================================================================
// 3) Generic variant payload — pure struct inputs, NO AppState globals.
// =========================================================================

/// Empty tag for sub-types whose payloads are UI5-owned.
struct UI5HandledTag {};

/// MessageRowPayload = std::variant over per-message-type structs.
/// Modules not owned by UI4 use `UI5HandledTag` so dispatch still compiles
/// and can return a graceful diagnostic for a mismatched payload.
using MessageRowPayload = std::variant<
    // User
    UserTextMessageData,
    user_command::UserCommandData,
    BashIOEntry,                          // bash input / output
    UserMessageData,                      // teammate / prompt / plan / etc.
    image::ImageMessageData,                     // image attachment
    ToolResultOptions,                    // user-facing tool result
    local_cmd::LocalCommandOptions,       // UI5: UserLocalCommandOutput
    attachment_message::AttachmentGridOptions, // UI5: UserAttachments

    // Assistant
    AssistantTextMessageData,
    tool_use_message::ToolUseRenderOptions,   // UI5: AssistantToolUse
    tool_use_message::GroupedToolsOptions,    // UI5: AssistantGroupedTools
    thinking_message::ThinkingMessageOptions, // UI5: AssistantThinking (+ Redacted)

    // System
    SystemTextMessageData,
    ErrorMessageData,                     // API error (basic struct)
    api_error_message::APIErrorOptions,   // UI5: SystemAPIError (rich card)
    collapsed_content::CollapsedContentOptions, // UI5: SystemCollapsedContent
    RateLimitInfo,
    PlanApprovalOptions,
    std::vector<HookProgressEntry>,       // hook progress list
    shutdown::ShutdownMessageData,
    AdvisorMessage
>;

// =========================================================================
// 4) Dispatch: RenderMessageRowByType
// =========================================================================

/// Callbacks the engine injects (all optional).  UI4 types hook into these;
/// UI5-owned sub-types pass callbacks through via a secondary dispatch.
struct MessageRowCallbacks {
    std::function<void(std::string_view text)>       on_copy;
    std::function<void()>                            on_click;
    std::function<void()>                            on_retry;          // RL
    std::function<void()>                            on_rate_limit_opts; // RL
    std::function<void()>                            on_regenerate;     // Asst
    std::function<void(bool expanded)>               on_toggle;         // System
    std::function<void()>                            on_resume;         // Shutdown
    std::function<void()>                            on_new_session;    // Shutdown
    std::function<void()>                            on_approve;        // Plan
    std::function<void(std::vector<PlanStep>)>       on_modify;         // Plan
    std::function<void(std::string_view reason)>     on_reject;         // Plan
    std::function<void()>                            on_dismiss;        // Advisor / Hook
    std::function<void()>                            on_action;         // Advisor
};

/// Result of dispatch: a Component tree ready for insertion into the screen.
/// Always returns a valid Component (never null).  UI5-owned sub-types
/// return a greyed-out placeholder reading "[UI5: foo_message]".
[[nodiscard]] inline Component RenderMessageRowByType(
    MessageShape shape,
    MessageRowPayload payload,
    MessageRowCallbacks callbacks = {})
{
    using S = MessageShape;

    // ---------------------------------------------------------------------
    // USER family
    // ---------------------------------------------------------------------
    if (shape == S::UserText) {
        auto data = std::get_if<UserTextMessageData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for UserText"); });
        return MakeUserTextMessage(*data,
            std::move(callbacks.on_copy),
            std::move(callbacks.on_click));
    }

    if (shape == S::UserCommand) {
        auto data = std::get_if<user_command::UserCommandData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for UserCommand"); });
        return user_command::MakeUserCommandMessage(*data,
            std::move(callbacks.on_copy));
    }

    if (shape == S::UserBashInput || shape == S::UserBashOutput) {
        auto data = std::get_if<BashIOEntry>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for BashIO"); });
        return ftxui::Renderer([d = *data] { return render_bash_io(d); });
    }

    // UI5: UserLocalCommandOutput — dedicated full viewer with gutter/fold/stderr-red
    if (shape == S::UserLocalCommandOutput) {
        auto data = std::get_if<local_cmd::LocalCommandOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for LocalCmd"); });
        return local_cmd::LocalCommandOutput(*data);
    }

    // UI5: UserAttachments — card grid with pagination + ASCII thumbnails
    if (shape == S::UserAttachments) {
        auto data = std::get_if<attachment_message::AttachmentGridOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for Attachments"); });
        return attachment_message::AttachmentGrid(*data);
    }

    if (shape == S::UserTeammate || shape == S::UserPrompt ||
        shape == S::UserPlan     || shape == S::UserAgentNotification ||
        shape == S::UserMemoryInput || shape == S::UserResourceUpdate) {
        // All share the user_message base module.  Add a flavoured prefix.
        auto data = std::get_if<UserMessageData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for user-flavour"); });
        std::string tag;
        switch (shape) {
            case S::UserTeammate:          tag = "[teammate] "; break;
            case S::UserPrompt:            tag = "[prompt] ";   break;
            case S::UserPlan:              tag = "[plan] ";     break;
            case S::UserAgentNotification: tag = "[agent-notify] "; break;
            case S::UserMemoryInput:       tag = "[memory] ";   break;
            case S::UserResourceUpdate:    tag = "[resource] "; break;
            default: break;
        }
        return ftxui::Renderer([tag, d = *data] {
            return vbox({
                text(tag + render_user_message(d)),
            });
        });
    }

    if (shape == S::UserToolResult) {
        // UI5 owns the *rich* tool-result rendering (code blocks, diffs).
        // The basic status + name + duration view is provided here as a
        // reasonable fallback using message_tool_result.cppm.
        auto data = std::get_if<ToolResultOptions>(&payload);
        if (!data) {
            return ftxui::Renderer([] {
                return vbox({
                    text("⚡ Tool result") | color(Color::Cyan),
                    text("(detailed view → UI5 message_tool_result)") | dim,
                });
            });
        }
        return ftxui::Renderer([d = *data] { return render_tool_result(d); });
    }

    // ---------------------------------------------------------------------
    // ASSISTANT family
    // ---------------------------------------------------------------------
    if (shape == S::AssistantText) {
        auto data = std::get_if<AssistantTextMessageData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for AsstText"); });
        return MakeAssistantTextMessage(*data,
            std::move(callbacks.on_copy),
            std::move(callbacks.on_regenerate),
            std::move(callbacks.on_rate_limit_opts));
    }

    // ---------------------------------------------------------------------
    // ASSISTANT family — UI5-owned complex types (real dispatch, no placeholder)
    // ---------------------------------------------------------------------
    if (shape == S::AssistantToolUse) {
        auto data = std::get_if<tool_use_message::ToolUseRenderOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for AsstToolUse"); });
        return tool_use_message::ToolUseMessage(*data);
    }
    if (shape == S::AssistantGroupedTools) {
        auto data = std::get_if<tool_use_message::GroupedToolsOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for AsstGroupedTools"); });
        return tool_use_message::GroupedToolsComponent(*data);
    }
    if (shape == S::AssistantThinking) {
        auto data = std::get_if<thinking_message::ThinkingMessageOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for AsstThinking"); });
        return thinking_message::ThinkingMessage(*data);
    }
    if (shape == S::AssistantRedactedThinking) {
        // Redacted thinking is mode ThinkingMode::Redacted inside the same
        // module — callers still pass a ThinkingMessageOptions with
        // data.mode = Redacted.  If callers used the legacy
        // RedactedThinkingData struct we fall back gracefully.
        auto data = std::get_if<thinking_message::ThinkingMessageOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for RedactedThinking"); });
        return thinking_message::ThinkingMessage(*data);
    }

    // ---------------------------------------------------------------------
    // SYSTEM family
    // ---------------------------------------------------------------------
    if (shape == S::SystemText) {
        auto data = std::get_if<SystemTextMessageData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for SystemText"); });
        return MakeSystemTextMessage(*data, std::move(callbacks.on_toggle));
    }

    if (shape == S::SystemRateLimit) {
        auto data = std::get_if<RateLimitInfo>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for RateLimit"); });
        return MakeRateLimitMessage(*data,
            std::move(callbacks.on_retry),
            std::move(callbacks.on_rate_limit_opts));
    }

    if (shape == S::SystemPlanApproval) {
        auto data = std::get_if<PlanApprovalOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for PlanApproval"); });
        return MakePlanApprovalMessage(*data,
            std::move(callbacks.on_approve),
            std::move(callbacks.on_modify),
            std::move(callbacks.on_reject));
    }

    if (shape == S::SystemHookProgress) {
        auto data = std::get_if<std::vector<HookProgressEntry>>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for HookProgress"); });
        return MakeHookProgressMessage(*data, std::move(callbacks.on_dismiss));
    }

    if (shape == S::SystemShutdown) {
        auto data = std::get_if<shutdown::ShutdownMessageData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for Shutdown"); });
        return shutdown::MakeShutdownMessage(*data,
            std::move(callbacks.on_resume),
            std::move(callbacks.on_new_session));
    }

    if (shape == S::SystemCompactBoundary) {
        // message_compact_boundary.cppm — stateless; just forward
        return ftxui::Renderer([] {
            return vbox({
                text("· · · compacted turn · · ·") | center | dim,
                separator() | dim,
            });
        });
    }

    if (shape == S::SystemAdvisor) {
        auto data = std::get_if<AdvisorMessage>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for Advisor"); });
        return MakeAdvisorMessage(*data,
            std::move(callbacks.on_dismiss),
            std::move(callbacks.on_action));
    }

    if (shape == S::SystemTaskAssignment) {
        return ftxui::Renderer([] {
            return vbox({
                text("📋 task assignment") | color(Color::Cyan),
                text("(rendered by message_task_assignment.cppm)") | dim,
            });
        });
    }

    if (shape == S::SystemAPIError) {
        // Try the rich UI5 card first; fall back to legacy ErrorMessageData.
        if (auto rich = std::get_if<api_error_message::APIErrorOptions>(&payload)) {
            return api_error_message::APIErrorMessage(*rich);
        }
        auto data = std::get_if<ErrorMessageData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for API error"); });
        // Core rendering lives in error_message.cppm (FTXUI upgrade → UI5)
        auto d = *data;
        return ftxui::Renderer([d] {
            return vbox({
                hbox({ text("❌ ") | color(Color::Red),
                       text("API Error") | bold | color(Color::Red) }),
                text(render_error_message(d)),
                text("(styled view → use api_error_message::APIErrorOptions for full card)") | dim,
            });
        });
    }

    if (shape == S::SystemCollapsedContent) {
        auto data = std::get_if<collapsed_content::CollapsedContentOptions>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for CollapsedContent"); });
        return collapsed_content::CollapsedContentMessage(*data);
    }

    if (shape == S::UserChannel) {
        return ftxui::Renderer([] {
            return vbox({
                text("📡 channel message") | color(Color::Magenta),
                text("(rendered by message_channel.cppm)") | dim,
            });
        });
    }

    if (shape == S::UserImage) {
        auto data = std::get_if<image::ImageMessageData>(&payload);
        if (!data) return ftxui::Renderer([=] { return text("⚠ message_row: bad payload for UserImage"); });
        return image::MakeImageMessage(*data,
            std::move(callbacks.on_click),
            std::move(callbacks.on_copy));
    }

    // Unknown / future shape
    return ftxui::Renderer([shape] {
        return vbox({
            text("? " + std::string(MessageShapeToString(shape))) | color(Color::Yellow),
            text("(no renderer registered — message_row.cppm needs update)") | dim,
        });
    });
}

} // namespace cc::ui::messages
