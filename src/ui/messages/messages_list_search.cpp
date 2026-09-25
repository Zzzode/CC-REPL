// messages_list_search.cpp - impl unit for cc.ui.messages.messages_list
// (RFC 0001 Phase C batch 7). The std::visit payload_preview and the rich
// two-tier extract_search_text. This is the DEDICATED TU for the ~20-module
// MessageRowPayload variant closure so it lands only once in the BMI graph.
module;

module cc.ui.messages.messages_list;

import std;

import cc.ui.messages.message_row;
import cc.ui.messages.user_text_message;
import cc.ui.messages.message_user_command;
import cc.ui.messages.message_bash_io;
import cc.ui.messages.user_message;
import cc.ui.messages.message_image;
import cc.ui.messages.message_tool_result;
import cc.ui.messages.local_command_output_message;
import cc.ui.messages.attachment_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.messages.tool_use_message;
import cc.ui.messages.thinking_message;
import cc.ui.messages.system_text_message;
import cc.ui.messages.error_message;
import cc.ui.messages.api_error_message;
import cc.ui.messages.collapsed_content_message;
import cc.ui.messages.message_components;
import cc.ui.messages.message_plan_approval;
import cc.ui.messages.message_hook_progress;
import cc.ui.messages.message_shutdown;
import cc.ui.messages.message_advisor;
import cc.ui.tools.registry;
import cc.ui.tools.generic;

namespace cc::ui::messages_list {

namespace detail {

auto payload_preview(const MessageRowPayload& p) -> std::string {
    return std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;

        auto take_first = [](const auto& a, const auto& b) -> std::string {
            using A = std::decay_t<decltype(a)>;
            using B = std::decay_t<decltype(b)>;
            if constexpr (!std::is_same_v<A, std::nullptr_t>) {
                if constexpr (std::is_convertible_v<A, std::string>) return std::string(a);
                if constexpr (requires{ std::to_string(a); }) return std::to_string(a);
            }
            if constexpr (!std::is_same_v<B, std::nullptr_t>) {
                if constexpr (std::is_convertible_v<B, std::string>) return std::string(b);
            }
            return {};
        };

        // --- User family ---
        if constexpr (std::is_same_v<T, UserTextMessageData>) {
            if constexpr (requires{ v.content; }) return take_first(v.content, nullptr);
            return "user-text";
        }
        else if constexpr (std::is_same_v<T, user_command::UserCommandData>) {
            if constexpr (requires{ v.command_line; }) return take_first(v.command_line, nullptr);
            return "user-command";
        }
        else if constexpr (std::is_same_v<T, BashIOEntry>) {
            if constexpr (requires{ v.content; }) return take_first(v.content, nullptr);
            return "bash-io";
        }
        else if constexpr (std::is_same_v<T, UserMessageData>) {
            if constexpr (requires{ v.title; }) return take_first(v.title, nullptr);
            return "user-message";
        }
        else if constexpr (std::is_same_v<T, image::ImageMessageData>) {
            if constexpr (requires{ v.file_name; }) {
                return std::string{"image "} + take_first(v.file_name, nullptr);
            }
            return "image";
        }
        else if constexpr (std::is_same_v<T, ToolResultOptions>) {
            if constexpr (requires{ v.tool_name; }) return take_first(v.tool_name, nullptr);
            return "tool-result";
        }
        else if constexpr (std::is_same_v<T, local_cmd::LocalCommandOptions>) {
            return "local-command";
        }
        else if constexpr (std::is_same_v<T, attachment_message::AttachmentGridOptions>) {
            return "attachments";
        }
        // --- Assistant family ---
        else if constexpr (std::is_same_v<T, AssistantTextMessageData>) {
            if constexpr (requires{ v.content; }) return take_first(v.content, nullptr);
            return "assistant-text";
        }
        else if constexpr (std::is_same_v<T, tool_use_message::ToolUseRenderOptions>) {
            return "tool-use";
        }
        else if constexpr (std::is_same_v<T, tool_use_message::GroupedToolsOptions>) {
            return "grouped-tools";
        }
        else if constexpr (std::is_same_v<T, thinking_message::ThinkingMessageOptions>) {
            return "thinking";
        }
        // --- System family ---
        else if constexpr (std::is_same_v<T, SystemTextMessageData>) {
            if constexpr (requires{ v.summary; }) return take_first(v.summary, nullptr);
            return "system-text";
        }
        else if constexpr (std::is_same_v<T, ErrorMessageData>) {
            return "error";
        }
        else if constexpr (std::is_same_v<T, api_error_message::APIErrorOptions>) {
            return "api-error";
        }
        else if constexpr (std::is_same_v<T, collapsed_content::CollapsedContentOptions>) {
            return "collapsed-content";
        }
        else if constexpr (std::is_same_v<T, RateLimitInfo>) {
            if constexpr (requires{ v.reason; }) {
                return take_first(v.reason, nullptr) + " " +
                       take_first(v.message, nullptr);
            }
            return "rate-limit";
        }
        else if constexpr (std::is_same_v<T, PlanApprovalOptions>) {
            return "plan-approval";
        }
        else if constexpr (std::is_same_v<T, std::vector<HookProgressEntry>>) {
            return "hook-progress";
        }
        else if constexpr (std::is_same_v<T, shutdown::ShutdownMessageData>) {
            return "shutdown";
        }
        else if constexpr (std::is_same_v<T, AdvisorMessage>) {
            return "advisor";
        }
        else if constexpr (std::is_same_v<T, UI5HandledTag>) {
            return {};
        }
        else {
            static_assert(!sizeof(T*), "Unreachable: new variant alternative in payload_preview.");
            return {};
        }
    }, p);
}

namespace search_detail {

/// Extract a string field value from a JSON object string.
/// Lightweight — no full JSON parser needed for known field names.
/// TS REF: transcriptSearch.ts toolUseSearchText() — extracts known input fields.
[[nodiscard]] std::string extract_json_field(
    std::string_view json, std::string_view field_name)
{
    auto pos = json.find(field_name);
    if (pos == std::string_view::npos) return {};
    // Find the colon after the field name
    auto colon = json.find(':', pos + field_name.size());
    if (colon == std::string_view::npos) return {};
    // Find the opening quote of the value
    auto quote = json.find('"', colon + 1);
    if (quote == std::string_view::npos) return {};
    // Find the closing quote (handle escaped quotes)
    std::size_t end = quote + 1;
    while (end < json.size() && json[end] != '"') {
        if (json[end] == '\\' && end + 1 < json.size()) {
            end += 2;  // skip escaped char
        } else {
            ++end;
        }
    }
    if (end >= json.size()) return {};
    return std::string(json.substr(quote + 1, end - quote - 1));
}

/// Extract searchable text from a tool-use input JSON string.
/// Mirrors TS toolUseSearchText() — known field names that renderToolUseMessage
/// shows as the primary argument (command, pattern, file_path, etc.).
/// TS REF: src/utils/transcriptSearch.ts L134-164  toolUseSearchText(input)
[[nodiscard]] std::string tool_use_search_text(std::string_view input_json) {
    if (input_json.empty()) return {};
    const std::string_view known_fields[] = {
        "\"command\"",   // Bash, Shell
        "\"pattern\"",   // Grep, Glob
        "\"file_path\"", // Read, Write, Edit
        "\"path\"",      // fallback for file_path
        "\"prompt\"",    // Agent
        "\"description\"",// Agent, Task
        "\"query\"",     // WebSearch, Grep
        "\"url\"",       // WebFetch
        "\"skill\"",     // SkillTool
    };
    std::string result;
    for (auto field : known_fields) {
        auto val = extract_json_field(input_json, field);
        if (!val.empty()) {
            if (!result.empty()) result += '\n';
            result += val;
        }
    }
    // Also try to extract arrays (args[], files[]) — TS joins with space.
    const std::string_view array_fields[] = {
        "\"args\"",   // Tmux, Tungsten
        "\"files\"",  // SendUserFile
    };
    for (auto field : array_fields) {
        auto pos = input_json.find(field);
        if (pos == std::string_view::npos) continue;
        auto bracket = input_json.find('[', pos);
        if (bracket == std::string_view::npos) continue;
        auto close_bracket = input_json.find(']', bracket);
        if (close_bracket == std::string_view::npos) continue;
        // Extract quoted strings inside the array
        std::string arr_text;
        std::size_t i = bracket + 1;
        while (i < close_bracket) {
            if (input_json[i] == '"') {
                auto end = input_json.find('"', i + 1);
                if (end == std::string_view::npos || end >= close_bracket) break;
                if (!arr_text.empty()) arr_text += ' ';
                arr_text += std::string(input_json.substr(i + 1, end - i - 1));
                i = end + 1;
            } else {
                ++i;
            }
        }
        if (!arr_text.empty()) {
            if (!result.empty()) result += '\n';
            result += arr_text;
        }
    }
    return result;
}

} // namespace search_detail

/// Rich searchable text for indexing.  Returns detailed content from tool
/// results (file contents, bash output, grep matches) for search matching.
/// Falls back to payload_preview() for non-tool message types.
///
/// TS REF: src/components/Messages.tsx L650-676
///   2-tier: renderableSearchText(msg) then tool.extractSearchText?(out)
///
/// @param p       The message row payload variant.
/// @param shape   The message shape (for dispatch optimization).
/// @return Lowercase-rich searchable text (NOT lowered — caller lowers).
[[nodiscard]] auto extract_search_text(
    const MessageRowPayload& p,
    MessageShape shape) -> std::string
{
    using S = MessageShape;

    // ── Tool RESULT messages (UserToolResult) ──────────────────────────
    // TS: if msg.type === 'user' && msg.toolUseResult, look up tool by name
    // and call tool.extractSearchText(out).  Prefer that over the heuristic.
    if (shape == S::UserToolResult) {
        if (auto* opts = std::get_if<::cc::ui::messages::ToolResultOptions>(&p)) {
            // Build the rich output text from ToolResultOptions fields.
            std::string rich_output;
            if (opts->content_items && !opts->content_items->empty()) {
                // Structured content items (MCP tools): concatenate text.
                for (const auto& item : *opts->content_items) {
                    if (item.type == "text" && !item.text.empty()) {
                        if (!rich_output.empty()) rich_output += '\n';
                        rich_output += item.text;
                    } else if (item.type == "image") {
                        if (!rich_output.empty()) rich_output += '\n';
                        rich_output += "[Image]";
                    }
                }
            } else if (opts->output && !opts->output->empty()) {
                rich_output = *opts->output;
            }
            std::string_view error_text =
                (opts->error_message && !opts->error_message->empty())
                    ? std::string_view{*opts->error_message}
                    : std::string_view{};

            // Tier 2: try tool-owned extractSearchText from registry.
            // TS REF: Messages.tsx L660-666  findRenderableToolByName + extractSearchText
            const auto& reg = cc::ui::tools::global_tool_ui_registry();
            const auto* ui = reg.find(opts->tool_name);
            if (ui && ui->extract_search_text) {
                auto extracted = ui->extract_search_text(rich_output, error_text);
                if (extracted.has_value()) {
                    // Tool returned explicit result (may be "" for "nothing to index").
                    return *extracted;
                }
            }

            // Fallback: use the rich output text directly.
            // This covers tools whose output IS visible but have no specific
            // UI registered (e.g. custom MCP tools).
            if (!error_text.empty()) {
                if (!rich_output.empty()) rich_output += '\n';
                rich_output += error_text;
            }
            if (!rich_output.empty()) return rich_output;

            // Last resort: fall back to toy payload_preview (just tool_name).
            return payload_preview(p);
        }
    }

    // ── Tool USE messages (AssistantToolUse, AssistantGroupedTools) ────
    // TS: toolUseSearchText(b.input) — extracts command/pattern/path from
    // the tool's input JSON so users can search for "grep" or "file_path".
    if (shape == S::AssistantToolUse) {
        if (auto* opts = std::get_if<::cc::ui::messages::tool_use_message::ToolUseRenderOptions>(&p)) {
            std::string result = search_detail::tool_use_search_text(
                opts->call.raw_parameters);
            // Also include result_preview if available (partial output during streaming).
            if (opts->call.result_preview && !opts->call.result_preview->empty()) {
                if (!result.empty()) result += '\n';
                result += *opts->call.result_preview;
            }
            if (!result.empty()) return result;
        }
        return payload_preview(p);
    }
    if (shape == S::AssistantGroupedTools) {
        if (auto* grp = std::get_if<::cc::ui::messages::tool_use_message::GroupedToolsOptions>(&p)) {
            std::string result;
            for (const auto& call : grp->calls) {
                auto t = search_detail::tool_use_search_text(call.raw_parameters);
                if (!t.empty()) {
                    if (!result.empty()) result += '\n';
                    result += t;
                }
            }
            if (!result.empty()) return result;
        }
        return payload_preview(p);
    }

    // ── Bash I/O (UserBashInput, UserBashOutput) ───────────────────────
    // TS: user message content blocks include bash stdin/stdout as text.
    if (shape == S::UserBashInput || shape == S::UserBashOutput) {
        if (auto* entry = std::get_if<BashIOEntry>(&p)) {
            return entry->content;
        }
    }

    // ── Local command output ───────────────────────────────────────────
    if (shape == S::UserLocalCommandOutput) {
        if (auto* opts = std::get_if<local_cmd::LocalCommandOptions>(&p)) {
            std::string result;
            // Include the command line so users can search for "/help", etc.
            if (!opts->data.command_line.empty()) {
                result = opts->data.command_line;
            }
            for (const auto& line : opts->data.lines) {
                if (!result.empty()) result += '\n';
                result += line.text;
            }
            if (!result.empty()) return result;
        }
    }

    // ── Thinking messages ──────────────────────────────────────────────
    // TS: thinking blocks are hidden by hidePastThinking in transcript mount.
    // Only index thinking when it's the active streaming tail (not past).
    if (shape == S::AssistantThinking || shape == S::AssistantRedactedThinking) {
        if (auto* opts = std::get_if<thinking_message::ThinkingMessageOptions>(&p)) {
            using TM = thinking_message::ThinkingState;
            if (opts->data.state == TM::Complete) {
                // Completed thinking is hidden in transcript — don't index it
                // (TS: hidePastThinking = true for completed blocks).
                return {};
            }
            // Active thinking: index the thinking text so users can search
            // for what the model is currently thinking about.
            // TS: thinking blocks contain text content that users may want to find.
            std::string thinking_text = opts->data.raw_text;
            for (const auto& section : opts->data.sections) {
                if (!section.content.empty()) {
                    if (!thinking_text.empty()) thinking_text += '\n';
                    thinking_text += section.content;
                }
            }
            return thinking_text;
        }
    }

    // ── Fallback: toy payload_preview for all other types ──────────────
    // AssistantText, UserText, SystemText, etc. already return rich content
    // via payload_preview (their `content` fields).
    return payload_preview(p);
}

} // namespace detail

} // namespace cc::ui::messages_list
