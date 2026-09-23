/// @file repl_screen.cppm
/// @brief Main REPL screen skeleton: enums, state, layout orchestration,
///        dialog routing, and event binding.  Rendering sub-modules delegate
///        to dedicated UIx agents (see ownership matrix below).
/// Migrated from src/screens/REPL.tsx (5005 lines)
///
/// =========================================================
/// PHASE 4 COMPONENT MATRIX — Sub-Component -> Agent Ownership
/// =========================================================
/// UI2  PromptInput full              UI11 Onboarding / wizards
/// UI3  Settings / model picker      UI12 Tasks panel UI
/// UI4  Assistant msg (markdown)     UI13 Agents panel / editor
/// UI5  User msg / attachments       UI14 Teams panel
/// UI6  CustomSelect / dropdowns     UI15 Install wizards
/// UI7  Structured diff viewer       UI16 Plugin dialogs / recs
/// UI8  Trust + sandbox dialogs      UI17 Code rendering (hl/md/term)
/// UI9  Permissions system           UI18 Feedback surveys
/// UI10 MCP dialogs (elicit/OAuth)   UI19 Spinner animations + tree
/// =========================================================
///
/// RENDERING DELEGATION (this file does NOT contain render bodies):
///   status bar   -> cc.ui.prompt.prompt_input_footer (UI1, user-configurable command output)
///   spinner      -> cc.ui.components.spinner_widget (UI19)
///   msg list     -> cc.ui.messages.messages (RenderMessages wrapper, UI4/5)
///   prompt input -> cc.ui.prompt.prompt_input_full (UI2)
///   dialogs      -> cc.ui.dialogs.* (DialogQueue 4-slot system, UI8-UI11/UI16)
module;

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <variant>
#include <format>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <deque>
#include <random>
#include <array>
#include <cctype>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/string.hpp>  // for string_width

export module cc.ui.screens.repl_screen;

export import cc.ui.screens.repl_state;

// Core engine types (Role, Message, ContentBlock, ImageBlock, etc.)
import cc.types.types;

// --- Sub-modules we DEPEND ON (skeleton-wired, bodies delegated) ---
import cc.ui.features.tasks.task_list_ui;
import cc.ui.features.teams.team_status;
import cc.ui.features.teams.live_teammates;
import cc.ui.messages.message_row;
import cc.ui.messages.message_image;
import cc.ui.messages.messages_list;
import cc.ui.visual.markdown;   // StreamingMarkdown for streaming-tail
import cc.ui.messages.user_text_message;
import cc.ui.messages.assistant_text_message;
import cc.ui.messages.system_text_message;
import cc.ui.messages.thinking_message;
import cc.ui.messages.tool_use_message;
import cc.ui.messages.message_tool_result;
import cc.ui.messages.local_command_output_message;
import cc.ui.messages.api_error_message;  // GAP 3: SystemAPIError rich card + retry
import cc.ui.dialogs.system;
import cc.ui.dialogs.cost_threshold_dialog;
import cc.ui.dialogs.idle_return_dialog;
import cc.ui.dialogs.settings_dialog;
import cc.config.config;
import cc.ui.dialogs.trust_dialog;
import cc.ui.dialogs.sandbox_dialog;
import cc.ui.features.agents.agent_cards;
import cc.ui.features.agents.agent_wizard;
import cc.tools.agent_display;
// Welcome header: Loom mascot mark + animated asterisk, wired into RenderReplScreen
// for fresh sessions.
import cc.ui.foundation.design_logo;
// M2: theme::current_theme() for the loom_body colour used by the banner.
import cc.ui.foundation.theme_provider;
// M8 (P0-1 glyph unification): shared figures/constants + palette tokens.
import cc.ui.foundation.design_figures;
import cc.ui.foundation.design_tokens;
// M2: format_welcome_message + kWelcomeTips feed for the welcome header.
import cc.ui.foundation.logo;
// P0-4: LogoV2 3-mode dispatch + WelcomeV2 58-col static card + full
// notice stack (Voice/Opus1m/Channels/Debug/Emergency/Tmux/Org/Sandbox/
// StatusNotices ×6 / GuestPasses / OverageCredit).  Condensed mode is
// the default (no changelog/onboarding/force_full_logo available yet);
// Compact & Horizontal modes are used when the caller opts in via the
// force_full_logo = true flag (see RenderWelcomeHeader below).
import cc.ui.foundation.logo_v2;
// Terminal size probe for adaptive layout (welcome header centering + future
// message-scroll height clamping).
import cc.ui.chrome.ink_utils;
// Runtime terminal feature detection (fullscreen mode, mouse tracking, etc.)
import cc.utils.terminal_helpers;
// M1: FullscreenLayout slot-system — faithful port of TS FullscreenLayout's
// region model (scrollable / bottom / overlay / modal / bottomFloat).  The
// shell is now composed via this slot composer instead of a flat vbox.
import cc.ui.chrome.fullscreen_layout;
// M3: the REAL text editor component (ui::components::TextInputImpl).  This is
// the faithful counterpart of TS BaseTextInput.tsx's inputState — it owns
// cursor tracking, multi-line layout, selection rendering, and the suggestions
// dropdown.  Previously the prompt was rendered by a hand-rolled ~90-line
// RenderPromptInput body that IGNORED this component ("shelfware").  M3 wires
// it in: each render we sync a TextInputImpl from ReplScreenState and delegate
// the caret/multiline/selection painting to it (mirroring TS
// useDeclaredCursor, which parks the terminal cursor at the insertion point).
import cc.ui.widgets.text_input;
// M5: Declared cursor support — parks the real terminal cursor at the text
// input's insertion point so IME preedit renders inline and screen readers /
// magnifiers can follow the input.  Faithful port of TS useDeclaredCursor +
// CursorDeclarationContext.
import cc.ui.foundation.declared_cursor;
// M3: vim mode badge / mode_display helper (Normal/Insert/Visual/VisualLine/
// Command).  Faithful to TS VimInput's -- INSERT -- / -- NORMAL -- / -- VISUAL
// indicator driven by vim_input state.
import cc.ui.prompt.vim_input;
// M5: PromptInputFooter — faithful port of TS PromptInputFooter.tsx +
// PromptInputFooterLeftSide.tsx.  Renders the area below the prompt input
// with left/right columns: ModeIndicator, tasks, teams, hints, bridge status.
import cc.ui.prompt.prompt_input_footer;
// TS-faithful voice footer indicator state enum (VoiceIndicator.tsx /
// src/context/voice.tsx).  Only the enum/renderer are needed here; the
// indicator itself renders through prompt_input_footer's notifications.
import cc.ui.prompt.voice_indicator;
import cc.ui.prompt.prompt_stash_notice;  // GAP 2: stashed prompt restore notice
import cc.ui.prompt.placeholder_cascade;  // P1: 4-tier contextual placeholder cascade
// M7: Core dialog framework — DialogQueue, DialogRendererRegistry, DialogFrame.
// Faithful port of TS dialog system architecture (DialogType, DialogSlot, priority bands).
import cc.ui.dialogs.system;
import cc.ui.dialogs.frame;
// M7: Faithful HelpView dialog — 3-tab help panel (general / commands / custom).
import cc.ui.dialogs.help_view;
// M7: Faithful SettingsView dialog — read-only settings panel.
import cc.ui.dialogs.settings_view;
// M7: Faithful About dialog.
import cc.ui.dialogs.about;
// Spinner verb list (TS src/constants/spinnerVerbs.ts) used by RenderSpinner
// to pick a random playful loading label per request.
import cc.constants.spinner_verbs;
// P0-3: VirtualMessageList — O(viewport) windowed rendering for transcripts
// with >80 rows.  Exports VirtualListHandle / VirtualListState types used
// below in ReplScreenState and ScrollTranscript.
import cc.ui.messages.virtual_list;

// M6: Faithful permission panels — bash / file_edit / file_write.
// These replace the old crude permission_dialog.cppm string renderer.
import cc.ui.permissions.permission_bash;
import cc.ui.permissions.permission_file_edit;
import cc.ui.permissions.permission_file_write;
import cc.ui.permissions.single_prompt;
// Unified canonical PromptInputMode enum (replaces local InputMode definition).
import cc.ui.foundation.ui_types;

// Forward imports (implement bodies in owning agent modules):
//   cc.ui.dialogs.{permission_prompts,mcp_dialogs,trust_dialog,
//                  sandbox_dialog,settings_dialog,model_picker,
//                  ide_dialogs,plugin_dialog,
//                  feedback_survey,config_dialog,mcp_dialogs}
//   cc.ui.prompt.{prompt_input_full,autocomplete,vim_input}
//   cc.ui.messages.{assistant_message,user_message,structured_diff}
//   cc.ui.components.{custom_select,diff_view,spinner_widget,
//                     file_tree,text_input_widget,notification,
//                     cost_display,dev_bar,status_line}
//   cc.ui.{design.dialog,hooks.hooks_ui,permissions.permission_views,
//          agents.agent_editor,tasks.task_list_ui,markdown,terminal}



export namespace cc::ui::repl_screen {
using namespace ftxui;

// =========================================================
// Rendering helpers — DELEGATED (owning agent: see UIx tags)
// =========================================================

// UI1: status bar — delegates to cc.ui.components.status_line.
// For now we provide a semantic assembler that status_line will style.
[[nodiscard]] inline Element RenderStatusBar(const StatusBarData& d) {
    Elements L = { text(" ") };
    L.push_back(text(d.model_name) | bold | color(Color::Cyan));
    if (d.is_fast_mode)  L.push_back(text(" fast") | color(Color::Yellow) | dim);
    if (d.is_auto_mode)  L.push_back(text(" [auto]") | color(Color::Magenta));
    if (d.agent_name)   { L.push_back(text(" @") | dim);
        L.push_back(text(*d.agent_name) | color(Color::Blue)); }
    if (d.effort_level) L.push_back(text(" [" + *d.effort_level + "]") | dim);
    if (d.is_brief_mode)L.push_back(text(" [brief]") | dim | color(Color::GrayLight));
    Elements R;
    if (d.context_token_count > 0) {
        R.push_back(text(std::format("ctx:{}", d.context_token_count)) | dim);
        R.push_back(text(" | ") | dim); }
    if (d.input_tokens || d.output_tokens) R.push_back(
        text(std::format("{}v {}^", d.input_tokens, d.output_tokens)) | dim);
    if (d.cost_usd) { R.push_back(text(" ") | dim); R.push_back(
        text(std::format("${:.4f}", *d.cost_usd)) | dim | color(Color::Green)); }
    if (d.swarm_session_count > 0) {
        R.push_back(text(" | ") | dim);
        R.push_back(text(std::format("{} swarm", d.swarm_session_count))
                        | dim | color(Color::Magenta)); }
    if (d.session_name) { R.push_back(text(" | ") | dim);
        R.push_back(text(*d.session_name) | dim); }
    if (d.bridge_connected) {
        R.push_back(text(" | ") | dim);
        R.push_back(text("<-> bridge") | color(Color::Cyan) | dim); }
    R.push_back(text(" "));
    return hbox({ hbox(L), filler(), hbox(R) })
         | bgcolor(Color::RGB(20, 20, 22));
}

// UI19: spinner line shell.  Faithful port of TS Spinner.tsx + BriefSpinner.
// Single loom-gold theme, cycling TEARDROP_ASTERISK glyph, random playful
// verb sampled from spinner_verbs list, 3-dot blink cadence.
[[nodiscard]] inline Element RenderSpinner(
    SpinnerMode m, const std::optional<std::string>& verb,
    const std::optional<std::string>& tip,
    int frame = 0) {
    if (m == SpinnerMode::Hidden) return text("");
    // TS Spinner.tsx: defaultColor='loom' (amber/gold), shimmer animation.
    // Single theme token regardless of mode — no per-mode color switch.
    const Color kLoomGold = Color::RGB(217, 154, 56);  // ~loom token

    // Pick a random playful verb once "per mount".  We don't track mount
    // state here, so hash the mode + pid and sample the verbs list.
    using cc::constants::spinner_verbs::SPINNER_VERBS;
    static std::mt19937 rng{std::random_device{}()};
    static thread_local std::uniform_int_distribution<std::size_t> dist(
        0, SPINNER_VERBS.size() - 1);
    static thread_local std::string_view cached_verb = SPINNER_VERBS[dist(rng)];
    // Re-sample when mode transitions from Hidden -> something (frame==0
    // from caller signals first visible frame of a new request).
    if (frame == 1) cached_verb = SPINNER_VERBS[dist(rng)];

    // 3-dot blink cycle: Math.floor(time/300)%3 per TS BriefSpinner.
    // Frame increments ~per render at ~60Hz; use frame/18 as ~300ms tick.
    const int dot_idx = std::max(0, frame / 18) % 3;
    const std::string dots = std::string(static_cast<std::size_t>(dot_idx + 1), '.') +
                             std::string(static_cast<std::size_t>(3 - dot_idx - 1), ' ');

    // SpinnerGlyph cycle (frames of TEARDROP_ASTERISK animation); 8 frames.
    constexpr std::array<std::string_view, 8> kGlyphs = {
        "✻", "❋", "✦", "✧", "✶", "✷", "✸", "✹"
    };
    const auto glyph = kGlyphs[static_cast<std::size_t>(std::max(0, frame / 6)) % kGlyphs.size()];

    std::string_view selected_verb = verb ? std::string_view(*verb) : cached_verb;
    std::string label = std::string(selected_verb) + "\xE2\x80\xA6";  // …
    Elements p = {
        text("  ") | size(WIDTH, EQUAL, 2),  // paddingLeft=2
        text(std::string(glyph)) | color(kLoomGold),
        text(" "),
        text(label) | color(kLoomGold),
        text(dots)  | color(kLoomGold) | dim,
    };
    if (tip) p.push_back(text("  -- " + *tip) | dim);
    return hbox(p);
}

[[nodiscard]] inline std::string truncate_columns(std::string text, int max_cols) {
    if (max_cols <= 0) return {};
    if (string_width(text) <= max_cols) return text;
    while (!text.empty() && string_width(text + "…") > max_cols) {
        text.pop_back();
    }
    return text + "…";
}

[[nodiscard]] inline std::string repeat_welcome_segment(std::string_view text, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += text;
    return out;
}

// ── UnseenDivider helpers ──────────────────────────────────────────────
// TS REF: FullscreenLayout.tsx countUnseenAssistantTurns (L200-216) +
//         computeUnseenDivider (L239-256).  Counts new assistant turns
//         that arrived after the user scrolled away from bottom, and
//         builds the UnseenDivider struct passed to RenderMessages.

namespace unseen_detail {

/// Whether an assistant entry has visible text content (TS:
/// assistantHasVisibleText L217-223).  Tool-use-only and thinking-only
/// entries don't count as "new messages" to the user.
[[nodiscard]] inline bool assistant_has_visible_text(
    const MessageDisplayEntry& e) {
    if (e.role != "assistant") return false;
    // Tool-use entries: never have visible text content to the user.
    // TS: tool_use blocks are not 'text' type, so they fail the check.
    if (e.is_tool_use) return false;
    // Thinking entries: TS checks for b.type === 'text', not 'thinking'.
    // Thinking blocks don't count as visible text for turn counting.
    if (e.is_thinking) return false;
    // If we reach here, it's an assistant text entry with content.
    return !e.content_preview.empty();
}

/// Count assistant turns in entries[start_idx..end).  A "turn" is a
/// non-assistant→assistant transition, skipping system/progress rows
/// and tool-use-only entries (TS REF: countUnseenAssistantTurns L200).
[[nodiscard]] inline std::size_t count_unseen_assistant_turns(
    const std::vector<MessageDisplayEntry>& entries,
    std::size_t start_idx) {
    std::size_t count = 0;
    bool prev_was_assistant = false;
    for (std::size_t i = start_idx; i < entries.size(); ++i) {
        const auto& e = entries[i];
        // Skip system rows (TS: progress type)
        if (e.role == "system") continue;
        // Skip tool-use-only assistant entries
        if (e.role == "assistant" && !assistant_has_visible_text(e)) {
            continue;  // don't update prev_was_assistant
        }
        const bool is_assistant = (e.role == "assistant");
        if (is_assistant && !prev_was_assistant) ++count;
        prev_was_assistant = is_assistant;
    }
    return count;
}

}  // namespace unseen_detail

/// Compute the UnseenDivider from state.divider_index + messages.
/// Returns nullopt when divider_index is unset, out of range, or no
/// messages have arrived past the divider.  TS REF: computeUnseenDivider
/// (FullscreenLayout.tsx L239-256).
[[nodiscard]] inline std::optional<::cc::ui::messages_list::UnseenDivider>
ComputeUnseenDivider(const ReplScreenState& s) {
    if (!s.divider_index.has_value()) return std::nullopt;
    const auto idx = *s.divider_index;
    if (idx >= s.messages.size()) return std::nullopt;

    // Find first non-system entry at or after divider_index (TS: anchorIdx
    // skips progress + null attachments).
    std::size_t anchor_idx = idx;
    while (anchor_idx < s.messages.size() &&
           s.messages[anchor_idx].role == "system") {
        ++anchor_idx;
    }
    if (anchor_idx >= s.messages.size()) return std::nullopt;

    const auto& anchor = s.messages[anchor_idx];
    const std::size_t count = std::max(
        std::size_t{1},
        unseen_detail::count_unseen_assistant_turns(s.messages, idx));

    ::cc::ui::messages_list::UnseenDivider ud;
    ud.first_unseen_uuid_prefix = anchor.id;
    ud.count = count;
    return ud;
}

[[nodiscard]] inline std::vector<MessageDisplayEntry> BuildVisibleMessages(
    const ReplScreenState& s) {
    auto entries = s.messages;
    if (!s.active_local_jsx_command) return entries;

    MessageDisplayEntry command;
    command.role = "user";
    command.is_local_command_input = true;
    command.content_preview = "/" + s.active_local_jsx_command_name;
    if (!s.active_local_jsx_command_args.empty()) {
        command.content_preview += " " + s.active_local_jsx_command_args;
    }
    command.timestamp = std::chrono::system_clock::now();
    entries.push_back(std::move(command));

    std::string content = s.active_local_jsx_content;
    if (!content.empty() && content.back() != '\n') content.push_back('\n');
    if (content.find("Esc to close") == std::string::npos &&
        content.find("Esc to go back") == std::string::npos) {
        content += "\nEsc to close";
    }
    MessageDisplayEntry local_jsx;
    local_jsx.role = "system";
    local_jsx.is_local_jsx_output = true;
    local_jsx.content_preview = std::move(content);
    local_jsx.timestamp = std::chrono::system_clock::now();
    entries.push_back(std::move(local_jsx));

    return entries;
}

// UI4/UI5: message list.  Delegates to messages_list.cppm (UI21).
// `spinner_frame` drives the tool-use header spinner animation (fix #10).
// `unseen_divider` is the optional in-transcript "N new messages" anchor
// set by useUnseenDivider (FullscreenLayout.tsx L86-190); nullopt when
// pinned to bottom or the session has no scroll-away yet.
[[nodiscard]] inline Element RenderMessages(
    const std::vector<MessageDisplayEntry>& entries,
    int sel = -1, int vlines = 40,
    int offs = 0, bool pinned = true,
    int spinner_frame = 0,
    std::optional<cc::ui::messages_list::UnseenDivider> unseen_divider =
        std::nullopt,
    Elements leading_elements = {},
    bool is_brief_mode = false,
    const std::unordered_set<std::string>& expanded_keys = {},
    bool is_transcript_mode = false,
    bool show_all_in_transcript = false,
    // TS REF: Messages.tsx L382-389 + L395-419  isStreamingThinkingVisible.
    // When true, build_visible_rows hides ALL completed thinking rows so
    // only the streaming-thinking tail is visible (TS lastThinkingBlockId
    // = 'streaming').
    bool streaming_thinking_globally_visible = false,
    // GAP 3: msg-system-api-error-retry — callback for the Retry button on
    // SystemAPIError rich cards.  When set, the API error card renders a
    // clickable Retry pill that invokes this to re-send the last user message.
    std::function<void()> on_retry = nullptr,
    // P2 gap api-error-retry: callback for "Clear session" button on
    // session-expired error cards.  TS REF: SystemAPIErrorMessage.tsx
    //   onClearSession prop — invoked when auth has expired and user
    //   chooses to clear the session to re-authenticate.
    std::function<void()> on_clear_session = nullptr,
    // TS REF: Messages.tsx L703-712 + Markdown.tsx L186-235 — StreamingMarkdown
    // stable-prefix cache for the streaming-text tail row.  When non-null,
    // RenderAssistantTextMessageFaithful uses update() instead of full
    // render_markdown() for is_streaming rows.
    ::cc::ui::StreamingMarkdown* streaming_md = nullptr) {
    // NOTE: We no longer early-return on empty entries.  The leading_element
    // (welcome/logo card) must always be rendered inside the yframe so it
    // scrolls with messages.  The messages_list handles empty rows gracefully
    // via its own visible.empty() path which prepends leading elements.

    namespace ml = cc::ui::messages_list;
    namespace image = cc::ui::messages::image;
    ml::MessagesListInput input;
    input.rows.reserve(entries.size());
    input.shapes.reserve(entries.size());
    input.uuids.reserve(entries.size());
    input.unseen_divider = std::move(unseen_divider);

    for (const auto& m : entries) {
        // TS REF: Messages.tsx L549-553  uuid → 24-char prefix anchor.
        // Populated parallel to rows/shapes; empty strings are harmless
        // (find_divider_before_visible_index skips them).
        input.uuids.push_back(m.id);
        if (m.is_local_command_input) {
            input.shapes.push_back(messages::MessageShape::UserCommand);
            input.rows.push_back(messages::UserTextMessageData{
                .content = m.content_preview,
                .timestamp = m.timestamp,
                .quoted_reply = std::nullopt,
                .is_transcript_mode = is_transcript_mode,
                .command_name = std::nullopt});
        } else if (m.is_local_jsx_output) {
            input.shapes.push_back(messages::MessageShape::UserLocalJsxOutput);
            input.rows.push_back(messages::UserTextMessageData{
                .content = m.content_preview,
                .timestamp = m.timestamp,
                .quoted_reply = std::nullopt,
                .is_transcript_mode = is_transcript_mode,
                .command_name = std::nullopt});
        } else if (m.is_local_command_output) {
            input.shapes.push_back(messages::MessageShape::UserLocalCommandOutput);
            messages::local_cmd::LocalCommandOptions opts;
            opts.show_line_numbers = false;
            opts.data.exit_code = m.is_error ? 1 : 0;

            std::size_t start = 0;
            while (start <= m.content_preview.size()) {
                auto nl = m.content_preview.find('\n', start);
                std::string line = nl == std::string::npos
                    ? m.content_preview.substr(start)
                    : m.content_preview.substr(start, nl - start);
                opts.data.lines.push_back(messages::local_cmd::OutputLine{
                    .kind = m.is_error
                        ? messages::local_cmd::StreamKind::Stderr
                        : messages::local_cmd::StreamKind::Stdout,
                    .text = std::move(line),
                });
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            input.rows.push_back(std::move(opts));
        } else if (m.role == "user") {
            if (m.is_image && m.image_block) {
                // TS parity: each user-attached image is its own UserImage row.
                // See TS src/components/UserImageMessage.tsx full file.
                // The data flow is: project_messages() → is_image=true +
                // image_block; we translate the block metadata into
                // image::ImageMessageData which message_image.cppm already
                // knows how to render.
                using image::ImageMessageData;
                using image::ImageSource;
                ImageMessageData d;
                const auto& ib = *m.image_block;
                d.timestamp = m.timestamp;
                d.media_type = ib.media_type;
                // TS parity: [Image #N] label — use the display id assigned
                // by project_messages from content-block order (matches
                // user's paste order shown in the input placeholder).
                if (m.image_display_id) {
                    d.image_id = std::to_string(*m.image_display_id);
                }
                if (ib.file_name)        d.file_name = *ib.file_name;
                if (ib.source_path)      d.source = *ib.source_path;
                if (ib.width)            d.width = *ib.width;
                if (ib.height)           d.height = *ib.height;
                if (ib.size_bytes)       d.file_size = *ib.size_bytes;
                switch (ib.source) {
                    using IS = ::cc::core::ImageBlockSource;
                    case IS::Clipboard: d.source_type = ImageSource::Clipboard; break;
                    case IS::File:      d.source_type = ImageSource::File; break;
                    case IS::Base64:    d.source_type = ImageSource::Base64; break;
                    case IS::Unknown:
                    default:
                        // Heuristic: empty source + non-empty data means a
                        // raw inline paste (no file path known).
                        d.source_type = d.source.empty()
                            ? ImageSource::Clipboard
                            : ImageSource::File;
                        break;
                }
                // NOTE: deliberately do NOT stuff ib.data into alt_text.
                // The base64 PNG prefix "iVBORw0KGgo..." rendered as "Alt: ..."
                // is worse than useless — wastes a line and confuses users.
                // Removed 2026-07-04 per spacing bug report.
                input.shapes.push_back(messages::MessageShape::UserImage);
                input.rows.push_back(std::move(d));
            } else {
                input.shapes.push_back(messages::MessageShape::UserText);
                input.rows.push_back(messages::UserTextMessageData{
                    .content = m.content_preview,
                    .timestamp = m.timestamp,
                    .quoted_reply = std::nullopt,
                    .is_transcript_mode = is_transcript_mode,
                    .command_name = std::nullopt});
            }
        } else if (m.role == "assistant") {
            if (m.is_thinking) {
                input.shapes.push_back(messages::MessageShape::AssistantThinking);
                messages::thinking_message::ThinkingMessageOptions opts;
                opts.data.raw_text = m.content_preview;
                // Static / unselected view must render the collapsed "Thinking"
                // label (TS: AssistantThinkingMessage.tsx collapsed state)
                // rather than hiding the row.  The messages-list fast path
                // hides rows where thinking is neither selected nor "active".
                if (m.thinking_active) {
                    opts.data.state =
                        messages::thinking_message::ThinkingState::Active;
                }
                input.rows.push_back(std::move(opts));
            } else if (m.is_tool_use) {
                input.shapes.push_back(messages::MessageShape::AssistantToolUse);
                messages::tool_use_message::ToolUseRenderOptions opts;
                opts.call.tool_name = m.tool_name.value_or("tool");
                // Fix #8/#9: thread the real parsed tool input + status from
                // the shared projection contract into the renderer (previously
                // raw_parameters fell back to content_preview and status was
                // hard-coded Pending).
                opts.call.raw_parameters =
                    m.tool_input_json.value_or(m.content_preview);
                // M6 result_preview: thread live streaming result preview into
                // the call data so ToolUIRegistry.progress() can format a
                // dynamic progress line (e.g. last output line for Bash tools).
                if (m.tool_result_preview) {
                    opts.call.result_preview = *m.tool_result_preview;
                }
                opts.call.parameters_language = "json";
                opts.call.status = messages::tool_use_message::parse_tool_status(
                    m.tool_status.value_or("pending"));
                input.rows.push_back(std::move(opts));
            } else {
                input.shapes.push_back(messages::MessageShape::AssistantText);
                input.rows.push_back(messages::AssistantTextMessageData{
                    .content = m.content_preview,
                    .timestamp = m.timestamp,
                    .model_name = std::nullopt,
                    .is_streaming = m.is_streaming});
            }
        } else if (m.role == "tool") {
            input.shapes.push_back(messages::MessageShape::UserToolResult);
            input.rows.push_back(messages::ToolResultOptions{
                .tool_name = m.tool_name.value_or("tool"),
                .status = m.is_error
                    ? messages::ToolResultStatus::Error
                    : messages::ToolResultStatus::Success,
                .output = m.content_preview,
                .error_message = std::nullopt,
                .duration_ms = std::nullopt,
                .is_truncated = false,
                .is_transcript_mode = is_transcript_mode,
                .content_items = m.tool_result_content_items});
        } else if (m.is_error) {
            // GAP 3: msg-system-api-error-retry — route system error messages
            // through the rich SystemAPIError card (severity borders + pills)
            // instead of the plain SystemText glyph.  The Retry button calls
            // on_retry to re-send the last user message.
            // TS REF: SystemAPIErrorMessage.tsx — rich error card with retry.
            namespace aem = cc::ui::messages::api_error_message;
            aem::APIErrorData err_data;
            err_data.message = m.content_preview;
            err_data.provider = "API";
            err_data.severity = aem::ErrorSeverity::Error;
            // P2 gap api-error-retry: thread retry metadata from the entry.
            // TS REF: SystemAPIErrorMessage.tsx — retryInMs, retryAttempt,
            //   maxRetries, sessionExpired destructured from message prop.
            err_data.retry_after_ms   = m.retry_after_ms;
            err_data.current_attempt  = m.retry_attempt;
            err_data.max_attempts     = m.max_retries;
            err_data.session_expired  = m.session_expired;
            aem::APIErrorOptions err_opts;
            err_opts.error = std::move(err_data);
            err_opts.on_retry = on_retry;
            // P2 gap: clear-session callback for expired auth sessions.
            // Wired through ReplScreenCallbacks.on_clear_session below.
            err_opts.on_clear_session = on_clear_session;
            err_opts.show_buttons = true;
            input.shapes.push_back(messages::MessageShape::SystemAPIError);
            input.rows.push_back(std::move(err_opts));
        } else {
            input.shapes.push_back(messages::MessageShape::SystemText);
            // Bridge the system-row subtype so the LIVE faithful renderer
            // (RenderSystemTextMessageFaithful dispatches per subtype) shows
            // the right glyph (※ away_summary / ✻ event / ⏺ generic).  When
            // the engine hasn't set system_subtype we derive a best-effort
            // subtype from the preview text — same labels TS SystemTextMessage
            // keys on (turn_duration / memory_saved / etc.).
            auto derive_subtype = [](const std::string& s,
                const std::optional<std::string>& hint) {
                using ST = messages::SystemMessageSubtype;
                if (hint) {
                    if (*hint == "away_summary")    return ST::AwaySummary;
                    if (*hint == "turn_duration")   return ST::TurnDuration;
                    if (*hint == "memory_saved")    return ST::MemorySaved;
                    if (*hint == "bridge_status")   return ST::BridgeStatus;
                    if (*hint == "thinking_summary")return ST::ThinkingSummary;
                    if (*hint == "hook_summary")    return ST::HookSummary;
                    if (*hint == "model_switch")    return ST::ModelSwitch;
                    if (*hint == "background_task") return ST::BackgroundTask;
                }
                // Heuristic fallback: scan preview text for TS-style keywords.
                if (s.find("away for") != std::string::npos ||
                    s.find("Welcome back") != std::string::npos)
                    return ST::AwaySummary;
                if (s.find("took") != std::string::npos ||
                    s.find("duration") != std::string::npos ||
                    s.find("seconds") != std::string::npos)
                    return ST::TurnDuration;
                if (s.find("memory") != std::string::npos ||
                    s.find("saved") != std::string::npos)
                    return ST::MemorySaved;
                if (s.find("bridge") != std::string::npos ||
                    s.find("IDE") != std::string::npos)
                    return ST::BridgeStatus;
                if (s.find("model") != std::string::npos &&
                    (s.find("switch") != std::string::npos ||
                     s.find("changed") != std::string::npos))
                    return ST::ModelSwitch;
                return ST::Plain;
            };
            input.rows.push_back(messages::SystemTextMessageData{
                .subtype = derive_subtype(m.content_preview, m.system_subtype),
                .summary = m.content_preview,
                .detail = {},
                .timestamp = m.timestamp,
                .is_transcript_mode = is_transcript_mode});
        }
    }

    if (sel >= 0)
        input.selected_row_idx = static_cast<std::size_t>(sel);

    const auto N = entries.size();
    // TS REF: Messages.tsx L703-719 — streaming text + streaming thinking
    // tails are rendered after all committed messages.  The tail row is
    // whichever comes last: a streaming text entry (is_streaming) or an
    // active thinking entry (thinking_active, set while streaming or
    // within 30s grace).  The tail row drives the "Running" status badge
    // and keeps thinking rows visible in build_visible_rows.
    bool has_streaming = !entries.empty() &&
        (entries.back().is_streaming || entries.back().thinking_active);
    input.streaming_tail_row = has_streaming ? N - 1 : N;
    input.pin_to_bottom = pinned;
    input.scroll_offset = std::max(0, offs);
    input.viewport_rows = std::max(1, vlines);
    input.is_brief_mode = is_brief_mode;
    // TS REF: Messages.tsx L382-389 + L395-419 — thread isStreamingThinkingVisible
    // to the messages list so it can hide ALL completed thinking rows when
    // the streaming-thinking tail is on screen.
    input.streaming_thinking_globally_visible = streaming_thinking_globally_visible;
    // TS REF: Messages.tsx L459 (isTranscriptMode) + L223 (showAllInTranscript).
    // In transcript mode the 3-tier filter shows all message types; cap at
    // 30 unless show_all_in_transcript lifts it.
    input.is_transcript_mode     = is_transcript_mode;
    input.show_all_in_transcript = show_all_in_transcript;
    // TS REF: Messages.tsx expandedKeys (L563) — user-expanded rows show
    // verbose full content.  Passed by copy (cheap for small sets).
    input.expanded_keys = expanded_keys;
    // GAP 3: thread on_retry through to the messages list so SystemAPIError
    // rows can render a working Retry button.
    input.on_retry = on_retry;
    // P2 gap api-error-retry: thread on_clear_session for session-expired
    // error cards.
    input.on_clear_session = on_clear_session;
    // TS REF: Messages.tsx L703-712 + Markdown.tsx L186-235 — thread the
    // shared StreamingMarkdown instance so the streaming-text tail row uses
    // stable-prefix caching instead of full re-parse per token.
    input.streaming_md = streaming_md;
    namespace ml = cc::ui::messages_list;
    // TS REF: FullscreenLayout <Box flexGrow={1} /> at the bottom of the
    // message list — absorbs remaining viewport space so short content stays
    // compact at the top (logo + messages adjacent, no blank gap between).
    // Without this filler, the yframe viewport is full-height but the inner
    // vbox is content-sized; when pin-to-bottom is off, FTXUI top-aligns the
    // inner vbox which is correct, but the filler ensures the scroll
    // indicator reflects "content fits" rather than "content is short".
    return ml::render_messages_list_view(
        std::move(input),
        static_cast<std::size_t>(spinner_frame),
        ml::kMaxRenderedLastN,
        {filler()},  // trailing_elements — elastic spacer (TS flexGrow={1})
        true,        // wrap_in_yframe
        std::move(leading_elements)) | flex;
}

[[nodiscard]] inline int CountTextLines(std::string_view text) {
    if (text.empty()) return 1;
    return static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
}

[[nodiscard]] inline int EstimateTranscriptRows(
    const std::vector<MessageDisplayEntry>& entries) {
    int rows = 0;
    for (const auto& entry : entries) {
        // TS PARITY (2026-07-05): content_preview for tool entries is often
        // just a short label ("Bash", "tool-use") while actual rendered
        // content can be dozens of lines.  Extract real content for accurate
        // scroll bounds (fixes "can't scroll to latest message" bug).
        int content_lines = 0;
        if (entry.is_tool_use) {
            // Tool-use card: header + input JSON + optional result preview.
            std::string combined;
            if (entry.tool_input_json && !entry.tool_input_json->empty()) {
                combined += *entry.tool_input_json;
            }
            if (entry.tool_result_preview && !entry.tool_result_preview->empty()) {
                if (!combined.empty()) combined += '\n';
                combined += *entry.tool_result_preview;
            }
            if (combined.empty()) combined = entry.content_preview;
            content_lines = CountTextLines(combined);
            // Cap input lines at 8 (collapsed args show first few) + 2 for chrome
            content_lines = std::min(content_lines, 8) + 3;
        } else if (entry.tool_result_content_items &&
                   !entry.tool_result_content_items->empty()) {
            // Structured tool result (MCP): concatenate text items.
            std::string full;
            for (const auto& item : *entry.tool_result_content_items) {
                if (item.type == "text") {
                    if (!full.empty()) full += '\n';
                    full += item.text;
                } else if (item.type == "image") {
                    if (!full.empty()) full += '\n';
                    full += "[Image]";
                }
            }
            content_lines = CountTextLines(full);
            content_lines += 2;  // header + status row
        } else if (entry.is_image) {
            content_lines = 4;  // label + metadata rows (no fake thumbnail)
        } else {
            content_lines = CountTextLines(entry.content_preview);
        }
        rows += content_lines;
        // Message list inserts one empty separator after each rendered row
        // (marginTop from addMargin=true).  Tool results skip this (flush).
        rows += 1;
    }
    return rows;
}

inline bool ScrollTranscript(const std::shared_ptr<ReplScreenState>& state,
                             int delta) {
    if (!state || delta == 0) return false;

    const int viewport_rows = std::max(1, state->viewport_height_lines);

    // P0-3 path: if the VirtualMessageList is active for this frame, use
    // its JumpHandle (prefix-sum table of exact visual lines) for O(log N)
    // scroll bounds instead of the crude EstimateTranscriptRows heuristic.
    if (state->virtual_list_active) {
        namespace vl = cc::ui::messages::virtual_list;
        const vl::JumpHandle& jh = state->virtual_jh;
        const int total_lines = jh.total();
        if (total_lines <= viewport_rows) return false;
        const int max_top = total_lines - viewport_rows;

        const int old_top = std::clamp(state->scroll_offset, 0, max_top);
        int target = old_top + delta;
        // Guarantee at least one row moves on PageUp/PageDown style deltas:
        // if target equals old_top, step by one row in the requested
        // direction using binary search.
        if (delta > 0 && target <= old_top) {
            size_t cur = jh.find_row_at_visual_line(old_top);
            if (cur + 1 < jh.size()) target = jh.find_visual_top_for_row(cur + 1);
        } else if (delta < 0 && target >= old_top) {
            size_t cur = jh.find_row_at_visual_line(old_top);
            if (cur > 0) target = jh.find_visual_top_for_row(cur - 1);
        }
        target = std::clamp(target, 0, max_top);
        if (target == old_top) return false;

        state->scroll_offset = target;
        const bool was_pinned = state->scroll_pinned_to_bottom;
        state->scroll_pinned_to_bottom = (target >= max_top);

        // TS REF: useUnseenDivider onScrollAway — on FIRST scroll-away from
        // bottom, snapshot message count as divider_index.  On repin, clear.
        if (was_pinned && !state->scroll_pinned_to_bottom) {
            state->divider_index = state->messages.size();
            state->message_count_at_scroll_away = state->messages.size();
        } else if (!was_pinned && state->scroll_pinned_to_bottom) {
            state->divider_index.reset();
            state->unseen_divider.reset();
            state->unseen_message_count = 0;
            state->pill_visible = false;
        }

        // If we are maintaining a live VirtualListState (Component-mode
        // wiring), also update its scroll_top so Render() reuses it.
        if (state->virtual_list_state) {
            state->virtual_list_state->scroll_top = target;
            vl::update_sticky_after_scroll(*state->virtual_list_state,
                                            old_top);
        }
        return true;
    }

    const auto visible_messages = BuildVisibleMessages(*state);
    if (visible_messages.empty()) return false;
    const int max_offset =
        std::max(0, EstimateTranscriptRows(visible_messages) - viewport_rows);
    if (max_offset == 0) return false;

    const int next =
        std::clamp(state->scroll_offset + delta, 0, max_offset);
    state->scroll_offset = next;
    const bool was_pinned = state->scroll_pinned_to_bottom;
    state->scroll_pinned_to_bottom = next >= max_offset;
    // TS REF: useUnseenDivider onScrollAway/onRepin.
    if (was_pinned && !state->scroll_pinned_to_bottom) {
        state->divider_index = state->messages.size();
        state->message_count_at_scroll_away = state->messages.size();
    } else if (!was_pinned && state->scroll_pinned_to_bottom) {
        state->divider_index.reset();
        state->unseen_divider.reset();
        state->unseen_message_count = 0;
        state->pill_visible = false;
    }
    return true;
}

[[nodiscard]] inline Element RenderLoomMascotMark(Color body, Color bg) {
    return vbox({
        hbox({
            text(" ▐") | color(body),
            text("▛███▜") | color(body) | bgcolor(bg),
            text("▌") | color(body),
        }),
        hbox({
            text("▝▜") | color(body),
            text("█████") | color(body) | bgcolor(bg),
            text("▛▘") | color(body),
        }),
        text("  ▘▘ ▝▝  ") | color(body),
    });
}

[[nodiscard]] inline Element RenderWelcomeFeed(std::string title,
                                               std::string message,
                                               int width,
                                               Color accent,
                                               Color muted) {
    width = std::max(width, 10);
    Elements rows;
    rows.push_back(text(std::move(title)) | bold | color(accent));
    rows.push_back(text(truncate_columns(std::move(message), width)) | color(muted) | dim);
    return vbox(std::move(rows)) | size(WIDTH, EQUAL, width);
}

[[nodiscard]] inline Element RenderWelcomeFeedColumn(int width,
                                                     Color accent,
                                                     Color muted) {
    width = std::max(width, 30);
    return vbox({
        RenderWelcomeFeed(
            "Recent activity",
            "No recent activity",
            width,
            accent,
            muted),
        separator() | color(accent),
        RenderWelcomeFeed(
            "What's new",
            "Check the Loom changelog for updates",
            width,
            accent,
            muted),
    }) | size(WIDTH, EQUAL, width);
}

[[nodiscard]] inline Element RenderWelcomeWindow(Element title,
                                                 Element body,
                                                 int width,
                                                 int title_width,
                                                 Color accent) {
    width = std::max(width, 4);
    const int title_offset = std::min(3, std::max(0, width - 2));
    const int right_rule_width =
        std::max(0, width - 2 - title_offset - title_width);

    Element top = hbox({
        text("╭") | color(accent),
        text(repeat_welcome_segment("─", title_offset)) | color(accent),
        std::move(title),
        text(repeat_welcome_segment("─", right_rule_width)) | color(accent),
        text("╮") | color(accent),
    });
    Element middle = hbox({
        separator() | color(accent),
        std::move(body) | size(WIDTH, EQUAL, width - 2),
        separator() | color(accent),
    });
    Element bottom = hbox({
        text("╰") | color(accent),
        text(repeat_welcome_segment("─", width - 2)) | color(accent),
        text("╯") | color(accent),
    });
    return vbox({
        std::move(top),
        std::move(middle),
        std::move(bottom),
    }) | size(WIDTH, EQUAL, width);
}

[[nodiscard]] inline Element RenderWelcomeLeftPanel(const ReplScreenState& s,
                                                    int spinner_frame,
                                                    int width,
                                                    Color accent,
                                                    Color muted,
                                                    Color text_color,
                                                    Color bg) {
    const std::string welcome = cc::ui::logo::format_welcome_message(
        s.user_display_name);
    const std::string model_line = !s.model_display_name.empty()
        ? s.model_display_name
        : s.settings_model;
    std::string cwd_line = s.cwd;
    if (!cwd_line.empty()) {
        cwd_line = truncate_columns(std::move(cwd_line), std::max(10, width - 2));
    }

    Elements meta;
    if (!model_line.empty()) {
        meta.push_back(text(truncate_columns(model_line, std::max(10, width - 2)))
            | color(muted) | dim);
    }
    if (!cwd_line.empty()) {
        meta.push_back(text(std::move(cwd_line)) | color(muted) | dim);
    }
    if (meta.empty()) meta.push_back(text(""));

    return vbox({
        text(""),
        hbox({
            cc::ui::design::logo::welcome_animated_asterisk(spinner_frame),
            text(" "),
            text(welcome) | bold | color(text_color),
        }) | center,
        text(""),
        RenderLoomMascotMark(accent, bg) | center,
        text(""),
        vbox(std::move(meta)) | center,
    }) | size(WIDTH, EQUAL, width) | size(HEIGHT, GREATER_THAN, 9);
}

// UI0: welcome header.  Faithful to TS LogoV2/CondensedLogo + Opus1mMergeNotice:
//   Row 1: orange AnimatedLoomMascot + "Loom" bold + "vX.X.X" dim
//   Row 2: model · billing_type dim
//   Row 3: [@agent · ] cwd dim
//   Row 4: ↑ "Opus now defaults to 1M context" banner
// Shown only on a fresh idle session (messages empty + spinner hidden).
// The old ASCII-art bordered card / left-panel / FeedColumn helpers are
// preserved in this file but no longer called by this renderer.
//
// P0-4 Faithful dispatch (TS LogoV2.tsx return paths):
//   is_condensed_mode (default)  → CondensedLogo + 10 notices flat stack
//   force_full_logo + cols<70    → Compact round card + flat notice stack
//   force_full_logo + cols>=70   → Horizontal left|divider|feed card + stack
// The caller may set s.debug_* / s.tmux_* / s.sandboxing_enabled fields to
// drive notice activation; they default to off so the header renders the
// same minimal 4-row look the TS default-condensed branch produces.
[[nodiscard]] inline Element RenderWelcomeHeader(const ReplScreenState& s,
                                                 int /*spinner_frame*/ = 0,
                                                 int term_cols = 80,
                                                 bool force_full_logo = false) {
    namespace lv2 = cc::ui::logo_v2;

    const std::string model_line = !s.model_display_name.empty()
        ? s.model_display_name
        : s.settings_model;
    const std::string version = s.app_version.empty()
        ? std::string("0.0.0") : s.app_version;

    // Build the LogoV2Options. Defaults mirror the TS LogoV2 component's
    // initial props (no onboarding, no release-notes → condensed branch).
    lv2::LogoV2Options opts;
    opts.version              = version;
    opts.cwd                  = s.cwd;
    opts.billing_type         = s.billing_type;
    // TS REF: logoV2Utils.ts:259 — agentName from getInitialSettings().agent
    opts.agent_name           = s.settings_agent_name.empty()
                              ? std::nullopt
                              : std::make_optional(s.settings_agent_name);
    opts.model_display_name   = model_line;
    opts.username             = s.user_display_name.empty()
        ? std::nullopt
        : std::make_optional(s.user_display_name);
    opts.org_name             = std::nullopt;
    opts.is_condensed_mode    = !force_full_logo && !s.show_onboarding;
                                                                          // TS early-return gate (L123):
                                                                          // isCondensedMode = !hasReleaseNotes
                                                                          //   && !showOnboarding && !forceFullLogo
    opts.show_onboarding     = s.show_onboarding;        // TS L56
    opts.show_sandbox_status  = false;                    // TODO(engine-wire)
    opts.show_guest_passes    = s.show_guest_passes_upsell;  // TS L70
    opts.show_overage_credit  = s.show_overage_credit_upsell; // TS L71
    opts.is_debug_mode        = false;               // TODO(engine-wire)
    opts.tmux_session         = std::nullopt;        // TODO(engine-wire)
    opts.company_announcement = std::nullopt;        // TODO(engine-wire)
    opts.emergency_tip        = std::nullopt;        // TODO(engine-wire)
    // StatusNotices: 6 TS definitions (memory/agent/subscriber/apikey/both/
    // jetbrains). All stubs inactive until engine wiring provides data.
    opts.status_notices       = {};

    // When force_full_logo is set and term_cols >= 70 (horizontal threshold),
    // build feeds for the right column.  The 4-branch feed priority chain
    // (TS LogoV2.tsx L421) is resolved inside the logo_v2 module when feeds
    // is empty.  We only build explicit feeds for the DEFAULT branch (no
    // onboarding / no guest / no overage) so we can inject real data from
    // s.recent_activity_lines and s.changelog_lines.  For the other branches,
    // the module builds placeholder feeds until engine wiring provides real
    // onboarding steps / guest pass counts / overage data.
    std::vector<lv2::FeedConfig> feeds;
    const bool priority_branch_active = s.show_onboarding
        || s.show_guest_passes_upsell || s.show_overage_credit_upsell;
    if (force_full_logo && !priority_branch_active) {
      {
        lv2::FeedConfig recent;
        recent.title = "Recent activity";
        if (s.recent_activity_lines.empty()) {
          recent.empty_message =
              "No recent conversations — start a new chat above";
        } else {
          recent.lines.reserve(s.recent_activity_lines.size());
          for (const auto& a : s.recent_activity_lines) {
            recent.lines.push_back(lv2::FeedLine{
                /*text=*/std::string(a),
                /*timestamp=*/std::nullopt});
          }
        }
        feeds.push_back(std::move(recent));
      }
      {
        lv2::FeedConfig whats_new;
        whats_new.title = "What's new";
        if (s.changelog_lines.empty()) {
          whats_new.lines = {
            lv2::FeedLine{/*text=*/"Paste images into the prompt with Ctrl+V",
                          /*timestamp=*/std::nullopt},
            lv2::FeedLine{/*text=*/"3-mode logo: condensed / compact / horizontal",
                          /*timestamp=*/std::nullopt},
            lv2::FeedLine{/*text=*/"Bash sandboxing via /sandbox toggle",
                          /*timestamp=*/std::nullopt},
          };
        } else {
          whats_new.lines.reserve(s.changelog_lines.size());
          for (const auto& c : s.changelog_lines) {
            whats_new.lines.push_back(lv2::FeedLine{
                /*text=*/std::string(c), /*timestamp=*/std::nullopt});
          }
        }
        whats_new.footer = "Full changelog at /changelog";
        feeds.push_back(std::move(whats_new));
      }
    }

    return lv2::render_logo_v2(opts, term_cols, std::move(feeds));
  }

// UI2: prompt input shell.  Full feature parity in prompt_input_full.cppm.
//
// M3 — WIRED TO THE REAL COMPONENT.  Previously this was a ~90-line
// hand-rolled body that IGNORED the real ui::components::TextInputImpl
// (cursor tracking, selection, vim modes, autocomplete, multiline, masking)
// and was flagged 0/25 faithful by the 1:1 audit.  We now delegate the
// caret/multiline/selection painting to a TextInputImpl that is SYNCED from
// ReplScreenState each render — mirroring TS BaseTextInput.tsx's
// useDeclaredCursor (which parks the real terminal cursor at the insertion
// point and lets screen readers follow the input).
//
// The pure-function signature `Element RenderPromptInput(const
// ReplScreenState&)` is PRESERVED so app.cppm's input handling (which writes
// s.input_text / s.input_mode / s.autocomplete_* between frames and forwards
// keystrokes via ReplScreen's CatchEvent) is untouched.  The TextInputImpl
// is used purely as a render primitive here — it is rebuilt per-frame from
// the projection, never as the interactive event target.
//
// Rendered faithful to TS BaseTextInput.tsx:
//   * TS prompt glyph figures.pointer "❯" (green) for normal mode,
//     "!" (red) for bash, "❮" (yellow/magenta) for vim Normal/Visual —
//     driven by s.input_mode.
//   * DECLARED CARET at the insertion point: TextInputImpl.Render() draws an
//     inverted glyph at the cursor offset (TS parks the real terminal cursor
//     there via useDeclaredCursor; we render a visible caret that lands on
//     the same byte offset).  Multi-line content lays out as a vbox.
//   * Contextual placeholder when empty (TS renderPlaceholder), styled dim.
//   * Selection highlight (TS HighlightedInput path) — provided by the real
//     impl when a selection range is set.
//   * Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) like TS,
//     driven by the existing vim_input::mode_display() helper.
//   * Prompt chrome uses top and bottom horizontal rules, matching TS
//     borderStyle="round" with left/right borders disabled.
[[nodiscard]] inline bool is_utf8_continuation_byte(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

[[nodiscard]] inline std::size_t clamp_input_cursor(
    const std::string& text,
    std::size_t pos) {
    if (pos == std::string::npos || pos > text.size()) return text.size();
    while (pos > 0 &&
           pos < text.size() &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        --pos;
    }
    return pos;
}

[[nodiscard]] inline std::size_t input_cursor_or_end(
    const ReplScreenState& s) {
    return clamp_input_cursor(s.input_text, s.input_cursor);
}

[[nodiscard]] inline std::size_t previous_utf8_boundary(
    const std::string& text,
    std::size_t pos) {
    pos = clamp_input_cursor(text, pos);
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        --pos;
    }
    return pos;
}

[[nodiscard]] inline std::size_t next_utf8_boundary(
    const std::string& text,
    std::size_t pos) {
    pos = clamp_input_cursor(text, pos);
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() &&
           is_utf8_continuation_byte(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
    return pos;
}

inline void set_prompt_input_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string value,
    std::size_t cursor) {
    state->input_text = std::move(value);
    state->input_cursor = clamp_input_cursor(state->input_text, cursor);
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

inline void insert_prompt_text(
    const std::shared_ptr<ReplScreenState>& state,
    std::string_view value) {
    auto cursor = input_cursor_or_end(*state);
    state->input_text.insert(cursor, value);
    set_prompt_input_text(state, std::move(state->input_text), cursor + value.size());
}

// AT-09: drain inbound IDE at_mentioned tokens staged by the MCP receive
// thread. MUST be called on the render thread (it mutates input_text/cursor).
// Returns the number of tokens inserted. Empty under the lock fast-path when
// nothing is pending so per-frame cost is negligible (keeps app.cppm thin).
inline std::size_t DrainPendingAtMentionInserts(
    const std::shared_ptr<ReplScreenState>& state) {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(state->pending_at_mention_mutex);
        if (state->pending_at_mention_inserts.empty()) return 0;
        batch.swap(state->pending_at_mention_inserts);
    }
    for (const auto& token : batch) {
        if (!token.empty()) insert_prompt_text(state, token);
    }
    return batch.size();
}

// ── Stashed prompt restore (GAP 2) ──────────────────────────────────────
// TS REF: src/screens/REPL.tsx L1373-1377 — stashedPrompt state:
//   {text, cursorOffset, pastedContents}.  When the user has typed input
//   and a background agent finishes or a permission request interrupts, the
//   current input is stashed so it can be restored after the request
//   completes.  Restore at:
//     - TS L3251-3255 (after local-jsx result returns)
//     - TS L3344-3348 (on submit when not slash-command)
//     - TS L3527-3531 (after handlePromptSubmit for slash/loading)
//   The stash notice (PromptInputStashNotice.tsx) renders
//   "{figures.pointerSmall} Stashed (auto-restores after submit)" when
//   hasStash is true.

/// Stash the current input text and cursor position.  Called when a
/// background agent finishes or a permission request interrupts the user's
/// typing flow, or when the user presses Ctrl+S (chat:stash).
/// `pasted_images` and `pasted_texts` are the pasted-content maps from the
/// engine layer (app.cppm) so image/text refs in the stashed text survive
/// the stash/restore cycle.
/// Returns true if something was actually stashed (input was non-empty or
/// pasted contents were provided).
inline bool StashCurrentPrompt(
    const std::shared_ptr<ReplScreenState>& state,
    std::unordered_map<int, ::cc::core::ImageBlock> pasted_images = {},
    std::unordered_map<int, std::string> pasted_texts = {}) {
    if (state->input_text.empty() && pasted_images.empty() && pasted_texts.empty())
        return false;
    ReplScreenState::StashedPrompt sp;
    sp.text = state->input_text;
    sp.cursor_offset = state->input_cursor;
    sp.pasted_images = std::move(pasted_images);
    sp.pasted_texts = std::move(pasted_texts);
    state->stashed_prompt = std::move(sp);
    return true;
}

/// Restore the stashed prompt into the input area.  Called after a submit
/// completes or when the user explicitly requests restore (Ctrl+S on empty
/// input).  Also returns the stashed pasted-contents maps via out-params so
/// the engine layer can restore [Image #N] / [...Truncated text #N] refs.
/// Returns true if a stash was restored.
inline bool RestoreStashedPrompt(
    const std::shared_ptr<ReplScreenState>& state,
    std::unordered_map<int, ::cc::core::ImageBlock>* out_images = nullptr,
    std::unordered_map<int, std::string>* out_texts = nullptr) {
    if (!state->stashed_prompt.has_value()) return false;
    auto stash = std::move(*state->stashed_prompt);
    state->stashed_prompt.reset();
    // Return pasted contents to the caller (engine layer).
    if (out_images) *out_images = std::move(stash.pasted_images);
    if (out_texts)  *out_texts  = std::move(stash.pasted_texts);
    set_prompt_input_text(state, std::move(stash.text),
        stash.cursor_offset == std::string::npos
            ? std::string::npos
            : stash.cursor_offset);
    return true;
}

/// True when a stashed prompt exists (for UI notice rendering).
inline bool HasStashedPrompt(const std::shared_ptr<ReplScreenState>& state) {
    return state->stashed_prompt.has_value();
}

inline bool backspace_prompt_text(const std::shared_ptr<ReplScreenState>& state) {
    auto cursor = input_cursor_or_end(*state);
    if (cursor == 0) return false;
    const auto prev = previous_utf8_boundary(state->input_text, cursor);
    state->input_text.erase(prev, cursor - prev);
    set_prompt_input_text(state, std::move(state->input_text), prev);
    return true;
}

// TS REF: src/components/PromptInput/PromptInput.tsx:1904-1908 —
//   `if (cursorOffset === 0 && (key.escape || key.backspace || key.delete ||
//        (key.ctrl && char === 'u'))) { onModeChange('prompt'); }`
//
// When the caret is at the very start of the buffer, Backspace/Escape/Delete/
// Ctrl+U exit any special input mode (Bash) back to Prompt.  This is what lets
// the user leave bash mode after typing a bare '!' into an empty prompt — the
// '!' is swallowed as a mode trigger (see the char handler below), so without
// this the buffer stays empty and Backspace would otherwise be a no-op,
// trapping the user in bash mode.  Returns true iff a mode reset occurred.
inline bool exit_input_mode_if_at_start(const std::shared_ptr<ReplScreenState>& state) {
    if (input_cursor_or_end(*state) != 0) return false;
    if (state->input_mode == InputMode::Normal) return false;
    state->input_mode = InputMode::Normal;
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
    // Mode change alters the autocomplete provider context (TS parity with the
    // char-handler mode toggle) — drop any dismissed-suggestion memory.
    state->dismissed_autocomplete_for_input.clear();
    return true;
}

inline bool delete_prompt_text(const std::shared_ptr<ReplScreenState>& state) {
    auto cursor = input_cursor_or_end(*state);
    if (cursor >= state->input_text.size()) return false;
    const auto next = next_utf8_boundary(state->input_text, cursor);
    state->input_text.erase(cursor, next - cursor);
    set_prompt_input_text(state, std::move(state->input_text), cursor);
    return true;
}

inline void move_prompt_cursor_left(const std::shared_ptr<ReplScreenState>& state) {
    state->input_cursor = previous_utf8_boundary(
        state->input_text,
        input_cursor_or_end(*state));
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

inline void move_prompt_cursor_right(const std::shared_ptr<ReplScreenState>& state) {
    state->input_cursor = next_utf8_boundary(
        state->input_text,
        input_cursor_or_end(*state));
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
}

// ─── Effective bash-mode detection (TS getInputMode equivalent) ──────────
//
// TS PromptInput.tsx computes `inputMode = getInputMode(value)` on every
// render, where getInputMode checks the first character of the text value.
// This means the mode is TEXT-DERIVED: pasting "!ls" or typing '!' into
// non-empty input immediately flips the effective mode to Bash, even though
// the user never pressed bare-'!' to toggle.  The state-mode toggle
// (s.input_mode) only matters when the input buffer is empty — it persists
// the visual "! " prefix after a bare-'!' keystroke that was swallowed.
//
// Use this helper for ALL behavioural gates (autocomplete, @-mention
// suppression, shell-command $PATH scan) and for the prefix/border
// rendering to stay faithful to TS semantics.
[[nodiscard]] inline bool effective_is_bash(const ReplScreenState& s) {
    if (!s.input_text.empty() && s.input_text.front() == '!') return true;
    return s.input_mode == InputMode::Bash;
}

// ─── Placeholder cascade (TS REF: usePromptInputPlaceholder.ts + PromptInput.tsx) ──
//
// Faithful port of the TS contextual placeholder system.  Priority order:
//
//   1. Input non-empty          → std::nullopt (no placeholder)
//   2. AI prompt suggestion     → next_action_suggestion (override layer from
//                                  PromptInput.tsx line 2014)
//   3. Viewing teammate         → "Message @{name}..."
//   4. Queued commands hint     → "Press up to edit queued messages"
//                                  (shown ≤3 times, only if editable queued cmds exist)
//   5. Onboarding example       → "Try \"{example command}\""
//                                  (only before first submit, with suggestions enabled)
//   6. Fallback                 → std::nullopt (no placeholder shown)
//
// Implementation lives in cc.ui.prompt.placeholder_cascade module for
// reusability by standalone TextInputImpl and dialog widgets.  This thin
// adapter projects ReplScreenState onto PlaceholderContext.

[[nodiscard]] inline std::optional<std::string> ComputePlaceholder(
    const ReplScreenState& s) {
    namespace ph = cc::ui::placeholder;

    ph::PlaceholderContext ctx;
    ctx.input_text                    = s.input_text;
    ctx.input_mode                    = s.input_mode;
    ctx.submit_count                  = s.submit_count;
    ctx.queued_hint_shown_count       = s.queued_command_hint_shown_count;
    ctx.has_editable_queued           = s.has_editable_queued_commands;
    ctx.prompt_suggestion_enabled     = s.prompt_suggestion_enabled;
    ctx.autocomplete_suggestions_empty = s.autocomplete_suggestions.empty();

    if (s.viewing_agent_name.has_value()) {
        ctx.viewing_agent_name = std::string_view(*s.viewing_agent_name);
    }
    if (s.next_action_suggestion.has_value()) {
        ctx.next_action_suggestion = std::string_view(*s.next_action_suggestion);
    }

    return ph::ComputePlaceholder(ctx);
}

[[nodiscard]] inline Element RenderPromptInput(const ReplScreenState& s,
                                                  int term_cols) {
    namespace uic   = ::ui::components;
    namespace vim   = cc::ui::prompt::vim_input;
    namespace figs  = cc::ui::design::figures;

    // --- 1. Prompt glyph + accent colour (TS faithfulness, unified) ---------
    //
    // REFERENCE (TS files):
    //   PromptInputModeIndicator.tsx + inputModes.ts + theme.ts
    //
    // SEMANTICS (simplified from TS — the CPP InputMode enum is kept
    // intact for backward compat with autocomplete gates in app.cppm,
    // but the PREFIX GLYPH COLLAPSES to exactly TWO visual variants per TS,
    // with priority matching PromptInputModeIndicator.tsx line 82):
    //
    //   PRIORITY 1 — viewingAgentName set:
    //                                        glyph = kPointer     "❯"
    //                                        color = teammate_prefix_color
    //                                                or palette.text
    //   PRIORITY 2 — mode == Bash (no viewing agent):
    //                                        glyph = kBashGlyph   "!"
    //                                        color = bashBorder  rgb(255,0,135)
    //   PRIORITY 3 — ALL OTHER modes:     glyph = kPointer     "❯"
    //                                        color = teammate_prefix_color
    //                                                or palette.text
    //                 (teammate_prefix_color is the engine-resolved
    //                 AGENT_COLOR_TO_THEME_COLOR for both the viewing-agent
    //                 path and the swarms-enabled default path)
    //
    // The old CPP-only per-mode glyphs (Slash "/", History "?", Plan "▣",
    // VimNormal "❮", VimVisual "❮", Permission "!", Task "*") are ELIMINATED
    // from the prefix position per TS:
    //   - Slash / History are routing semantics, not visual glyphs — TS's
    //     PromptInputModeIndicator falls through to ❯ even for those.
    //   - Vim mode is shown as a SEPARATE badge below the prefix (the
    //     "-- INSERT --" / "-- NORMAL --" row rendered later in this fn).
    //   - Plan mode is shown as a badge in the footer (StatusLine) or as a
    //     bubble marker, never as a replacement prefix glyph.
    //   - OrphanedPermission / TaskNotification fall through to the default
    //     "❯" pointer per TS.
    //
    // Fetch the currently active palette via the theme provider (TS ThemeContext
    // equivalent).  This respects ThemeVariant::Dark / Light / Daltonized /
    // Monochrome plus the force_monochrome a11y flag.  theme::current_theme()
    // is a cheap value copy (2 pointers + 3 booleans) with a short mutex grab.
    namespace thm = cc::ui::design::theme;
    namespace tok = cc::ui::design::tokens;
    const tok::Palette& pal = *thm::current_theme().palette;
    // TS getInputMode(value) equivalent: text-derived when text is present,
    // state-toggle when empty.  See effective_is_bash() for rationale.
    const bool is_bash_mode = effective_is_bash(s);
    std::string prefix_str;     // passed into TextInputOptions.prefix;
    Color       prefix_color;   // applied to the prefix inside renderInputArea.

    // Step 1a: pick glyph.
    // Priority (TS REF: PromptInputModeIndicator.tsx line 82):
    //   1. viewingAgentName set  → ❯ (always, regardless of bash mode)
    //   2. mode === 'bash'       → !
    //   3. otherwise             → ❯
    // When a viewing agent is active, the prefix is ALWAYS ❯ (never !),
    // matching TS where `viewingAgentName ?` is checked BEFORE
    // `mode === 'bash'`.
    const bool has_viewing_agent = s.viewing_agent_name.has_value()
        && !s.viewing_agent_name->empty();
    const bool show_bash_glyph = is_bash_mode && !has_viewing_agent;
    prefix_str += show_bash_glyph
        ? std::string(figs::kBashGlyph)
        : std::string(figs::kPointer);
    prefix_str += " ";   // trailing NBSP/space — 2 display cells total (TS).

    // Step 1b: pick color.
    //
    // Priority matches the glyph selection above:
    //   1. viewingAgentName set  → teammate_prefix_color (engine-resolved
    //                               agent color) or palette.text
    //   2. bash mode (no viewing agent) → bashBorder
    //   3. otherwise             → teammate_prefix_color or palette.text
    //
    // Bash mode always uses bashBorder (TS: dark rgb(255,0,135), daltonized
    // blue variants, light same).  All other modes: use the teammate color if
    // the engine has supplied one via s.teammate_prefix_color (TS
    // AGENT_COLOR_TO_THEME_COLOR map in agentColorManager.ts), otherwise fall
    // through to palette.text (dark: pure white, light: pure black).
    if (show_bash_glyph) {
        prefix_color = pal.bash_border;
    } else if (s.teammate_prefix_color.has_value()) {
        prefix_color = *s.teammate_prefix_color;
    } else {
        prefix_color = pal.text;
    }
    // (Prefix rendering happens INSIDE TextInputImpl via opts.prefix — see
    // below — so cursor_display_col returns correct values automatically
    // and the declared cursor lands at the right screen column.)

    // --- 2. Sync a TextInputImpl from the projection --------------------
    uic::TextInputOptions opts;
    // Compute contextual placeholder via the TS-faithful cascade.
    // If the cascade returns nullopt (no condition matched), fall back to
    // the static input_placeholder string for backward compatibility with
    // standalone TextInputImpl usage.
    if (auto computed = ComputePlaceholder(s); computed.has_value()) {
        opts.placeholder = *std::move(computed);
    } else {
        opts.placeholder = s.input_placeholder;
    }
    opts.prefix       = std::move(prefix_str);   // ← RENDERED INSIDE now (BUG-2 fix)
    opts.prefix_color = prefix_color;            // ← new field: explicit color for prefix
    opts.multiline    = true;
    opts.show_line_numbers = false;
    opts.enable_undo_redo   = false;
    opts.cursor_blink_ms    = 0;   // deterministic snapshot (no flicker)
    opts.show_history       = false;
    opts.argument_hint      = s.pending_argument_hint;  // SL-03
    opts.inline_ghost_text = s.pending_ghost_text;      // SL-05
    auto impl = std::make_shared<uic::TextInputImpl>(std::move(opts));
    impl->set_text(s.input_text);  // parks cursor at end of buffer
    const auto cursor = input_cursor_or_end(s);
    const auto end = s.input_text.size();
    if (cursor < end) {
        impl->move_cursor(
            -static_cast<int>(end - cursor),
            /*extend_selection=*/false);
    }

    // --- 3. Render the input area from the REAL component ---------------
    Element input_area = impl->RenderInputAreaPub();

    // --- 3b. Declared cursor (IME / accessibility) ----------------------
    // Faithful port of TS useDeclaredCursor: park the real terminal cursor at
    // the insertion point so IME preedit renders inline and screen readers /
    // magnifiers can follow the input.
    //
    // NOTE on cursor-display math (BUG-2 FIXED):
    //   We previously rendered the prefix OUTSIDE TextInputImpl in a separate
    //   hbox, but left opts.prefix empty.  declared_cursor then used
    //   `string_width(opts.prefix) = 0` as the prefix width, so the native
    //   cursor parked 3-4 display columns to the LEFT of actual text start.
    //
    //   After this commit: opts.prefix contains the rendered 2-cell glyph,
    //   impl->cursor_display_col() includes prefix width in its return, and
    //   declared_cursor below positions the terminal cursor EXACTLY over the
    //   character where the next keystroke will insert.  No arithmetic tricks
    //   are required — TextInputImpl's own prefix logic (see RenderInputArea
    //   inside text_input.cppm) already accounts for it.
    //
    //   We ALSO add 1 column for the left-side " " padding space rendered in
    //   hbox #5 (the `text(" ")` before the text area hbox row — this is a
    //   pure layout margin that TextInputImpl does NOT know about so we
    //   account for it here manually).
    {
        // Native terminal cursor parks at the screen bottom-right (Hidden) via
        // the root CursorResetNode in app.cppm.  We intentionally do NOT declare
        // the prompt caret position here: FTXUI's ScreenInteractive emits a
        // cursor-MOVE sequence every frame (from bottom-right to the declared
        // position) even when nothing else changed, and many terminals render
        // those hidden-cursor moves as visible flicker during the ~20Hz idle
        // re-render.  Leaving the cursor at bottom-right makes the move delta
        // zero, so FTXUI emits no move and the idle frame is flicker-free.
        // The visible caret is still drawn by TextInputImpl (inverted glyph), so
        // the user sees their caret; only the hidden native cursor (IME/a11y
        // anchor) parks at the corner instead of over the caret.
    }

    Elements box_body;

    // --- 4. Vim-mode badge (-- INSERT -- / -- NORMAL -- / -- VISUAL --) -
    // Faithful to TS: drawn as a separate row (NOT a prefix glyph swap),
    // dim+bold, per-mode color (see vim_input.cppm mode_display).
    std::optional<std::pair<std::string, Color>> vim_badge;
    if (s.input_mode == InputMode::VimInsert)
        vim_badge = {"-- INSERT --", vim::mode_display(vim::VimMode::Insert).second};
    else if (s.input_mode == InputMode::VimNormal)
        vim_badge = {"-- NORMAL --", vim::mode_display(vim::VimMode::Normal).second};
    else if (s.input_mode == InputMode::VimVisual)
        vim_badge = {"-- VISUAL --", vim::mode_display(vim::VimMode::Visual).second};
    if (vim_badge) {
        box_body.push_back(hbox({
            text("  "),
            text(vim_badge->first) | color(vim_badge->second) | bold | dim,
        }));
    }

    // --- 4b. Stash notice (GAP 2: stashed-prompt-restore-logic-missing) ---
    // TS REF: PromptInputStashNotice.tsx — renders
    //   "{figures.pointerSmall} Stashed (auto-restores after submit)"
    //   when hasStash is true.  Shown above the input area so the user knows
    //   their typed input was saved and will be restored after the current
    //   request completes.
    if (s.stashed_prompt.has_value()) {
        namespace psn = cc::ui::prompt;
        psn::StashNotice notice;
        notice.stashed_text = s.stashed_prompt->text;
        notice.char_count = s.stashed_prompt->text.size();
        // TS REF: <Box paddingLeft={2}> — render_stash_notice handles the
        // 2-space left padding internally, matching TS paddingLeft={2}.
        box_body.push_back(psn::render_stash_notice(notice));
    }

    // --- 5. Compose -----------------------------------------------------
    // Per TS layout: a leading space (`text(" ")`) followed by the
    // TextInputImpl's rendered output (which itself is `prefix_glyph + space
    // + text`).  The leading space was originally introduced so the glyph
    // doesn't hug the left edge; we keep it for visual parity.
    box_body.push_back(hbox({
        text(" "),
        input_area,
    }));

    auto content = vbox(std::move(box_body));

    // TS PromptInput.tsx:2237/2268: <Box borderStyle="round" borderLeft={false}
    // borderRight={false} borderBottom>.  Ink defaults borderTop to TRUE when
    // borderStyle is set (render-background.js: `borderTop !== false ? 1 : 0`).
    // The top border may carry `borderText` (fast-mode cooldown), but normally
    // it's just a plain horizontal rule — the prompt glyph `❯` lives INSIDE the
    // input area as the TextInput prefix, NOT in the border.
    //
    // FTXUI separator() renders as box-drawing characters, matching Ink's
    // border lines.  Both top and bottom rules use the mode-appropriate
    // border colour: bashBorder in Bash mode (TS: rgb(255,0,135) pink),
    // promptBorder otherwise (TS: grey).
    const Color frame_color = is_bash_mode ? pal.bash_border : pal.prompt_border;
    Element top_rule    = separator() | color(frame_color);
    Element bottom_rule = separator() | color(frame_color);

    // ── Declared cursor (IME / accessibility) ──────────────────────────────
    // Faithful port of TS useDeclaredCursor: park the real terminal cursor at
    // the insertion point so IME preedit renders inline and screen readers /
    // magnifiers can follow the input.
    //
    // Cursor position relative to the returned vbox:
    //   rel_y = 1 (top_rule) + (vim_badge ? 1 : 0)
    //   rel_x = 1 (leading space in hbox) + prefix_width + cursor_display_col()
    //
    // NOTE: cursor_display_col() returns width of text up to caret (NOT
    // including prefix), so we add prefix_width manually.
    namespace dc = cc::ui::common::declared_cursor;
    const int prefix_width = ftxui::string_width(opts.prefix);
    const int caret_col = impl->cursor_display_col();
    const int rel_y = 1 + (vim_badge ? 1 : 0);
    const int rel_x = 1 + prefix_width + caret_col;

    auto result = vbox({
        std::move(top_rule),
        content,
        std::move(bottom_rule),
    }) | size(WIDTH, EQUAL, std::max(term_cols, 40));

    // Apply declared_cursor so the hidden native cursor parks at the caret
    // position (TS: useDeclaredCursor).  This overrides cursor_reset()'s
    // bottom-right parking.  Shape=Hidden because the visible caret is drawn
    // by TextInputImpl itself (inverted glyph), not the terminal cursor.
    return std::move(result) | dc::declared_cursor(
        /*active=*/true, rel_x, rel_y,
        ftxui::Screen::Cursor::Shape::Hidden);
}

[[nodiscard]] inline std::string pad_to_columns(std::string text, int width) {
    const int pad = width - string_width(text);
    if (pad > 0) text.append(static_cast<std::size_t>(pad), ' ');
    return text;
}

/// TS REF: PromptInputFooterSuggestions.tsx — renders autocomplete suggestion
/// items in a vertical list.
///
/// Two rendering modes (matching TS):
///   - Non-fullscreen (inline in footer): adaptive maxVisibleItems =
///     min(6, max(1, term_rows - 3)), items bottom-aligned (flex-end).
///   - Fullscreen (overlay portal): floating overlay above the prompt with
///     opaque background, OVERLAY_MAX_ITEMS = 5, no flex-end alignment.
///
/// TS REF: FullscreenLayout.tsx L591-607 — overlay uses position="absolute"
/// bottom="100%" opaque={true} to escape the bottom-slot overflowY:hidden clip.
/// In FTXUI there's no CSS overflow clip, so we render inline but apply
/// overlay visual styling (background + top border) when is_overlay=true.
///
/// @param is_overlay  When true, apply fullscreen overlay styling.
/// @param term_rows   Terminal height in rows (for adaptive maxVisibleItems).
[[nodiscard]] inline Element RenderPromptSuggestions(const ReplScreenState& s,
                                                     int term_cols,
                                                     bool is_overlay = false,
                                                     int term_rows = 24) {
    if (s.autocomplete_suggestions.empty()) return Element{};

    // TS REF: PromptInputFooterSuggestions.tsx L224 — maxVisibleItems differs
    // between overlay (fixed 5) and inline (adaptive to terminal height).
    constexpr int kOverlayMaxItems = 5;  // TS: OVERLAY_MAX_ITEMS
    const int kInlineMaxItems = std::min(6, std::max(1, term_rows - 3));
    const int kMaxVisibleItems = is_overlay ? kOverlayMaxItems : kInlineMaxItems;

    const int total = static_cast<int>(s.autocomplete_suggestions.size());
    const int selected = std::clamp(
        s.autocomplete_index < 0 ? 0 : s.autocomplete_index,
        0,
        total - 1);
    const int visible = std::min(kMaxVisibleItems, total);
    const int start = std::max(
        0,
        std::min(selected - visible / 2, total - visible));
    const int end = std::min(start + visible, total);

    int widest = 0;
    if (s.autocomplete_stable_name_width > 0) {
        // INF-03: stable precomputed width (slash-command path) — no jitter.
        widest = s.autocomplete_stable_name_width;
    } else {
        for (int i = start; i < end; ++i) {
            const auto& item = s.autocomplete_suggestions[static_cast<std::size_t>(i)];
            // TS REF: PromptInputFooterSuggestions.tsx — icon takes display width
            // before the label. Add icon width to the name column so labels
            // align vertically when some rows have icons and others don't.
            const int icon_w = item.icon.empty() ? 0 : string_width(item.icon) + 1;
            widest = std::max(
                widest,
                icon_w + string_width(item.display_text));
        }
    }
    const int max_name_width = std::max(10, term_cols * 2 / 5);
    const int name_width = std::min(widest + 5, max_name_width);
    const int desc_width = std::max(0, term_cols - name_width - 4);

    namespace thm = cc::ui::design::theme;
    const auto& pal = *thm::current_theme().palette;

    Elements rows;
    rows.reserve(static_cast<std::size_t>(visible));
    for (int i = start; i < end; ++i) {
        const auto& item = s.autocomplete_suggestions[static_cast<std::size_t>(i)];
        const bool is_selected = i == selected;

        // Build the label: optional colored dot + icon + display_text,
        // padded to name_width.
        // TS REF: PromptInputFooterSuggestions.tsx renderRow — icon glyph then
        // the label, both styled together.
        // TS REF: src/hooks/unifiedSuggestions.ts:77-108 — agent defs include
        //   a color field used to tint the avatar dot.
        Elements label_parts;
        int used = 0;
        // Colored dot for agent/teammate suggestions.
        if (!item.color_name.empty()) {
            auto agent_color = [&]() -> Color {
                if (item.color_name == "red") return Color::Red;
                if (item.color_name == "blue") return Color::Blue;
                if (item.color_name == "green") return Color::Green;
                if (item.color_name == "yellow") return Color::Yellow;
                if (item.color_name == "purple") return Color::Magenta;
                if (item.color_name == "orange") return Color::Yellow;
                if (item.color_name == "pink") return Color::MagentaLight;
                if (item.color_name == "cyan") return Color::Cyan;
                return Color::Default;
            }();
            label_parts.push_back(text("● ") | color(agent_color) | bold);
            used += 2;  // "● " is 2 display columns
        }
        if (!item.icon.empty()) {
            label_parts.push_back(text(item.icon + " "));
            used += string_width(item.icon) + 1;
        }
        const int text_budget = std::max(1, name_width - used);
        auto display = truncate_columns(item.display_text, text_budget);
        label_parts.push_back(text(pad_to_columns(std::move(display), text_budget)));

        Element name = hbox(std::move(label_parts));
        Element detail = text(truncate_columns(item.description, desc_width));
        if (is_selected) {
            // TS REF: selected item uses suggestion color (lavender in dark).
            name = name | color(pal.suggestion) | bold;
            detail = detail | color(pal.suggestion);
        } else {
            name = name | dim;
            detail = detail | dim;
        }
        Element row_el = hbox({
            text("  "),
            std::move(name),
            std::move(detail),
            filler(),
        });
        // TS REF: FullscreenLayout.tsx L607 — overlay items get the surface
        // background so the floating list doesn't show messages through it.
        if (is_overlay && is_selected) {
            row_el = row_el | bgcolor(pal.message_actions_background);
        }
        rows.push_back(std::move(row_el));
    }

    Element content = vbox(std::move(rows));

    // TS REF: FullscreenLayout.tsx L607 — overlay wrapper:
    //   <Box position="absolute" bottom="100%" ... opaque={true}>
    // In FTXUI we apply: background fill + top separator line to visually
    // separate the floating overlay from scrollback messages above it.
    if (is_overlay) {
        // Build a top separator line using the chrome color (TS border-top
        // equivalent).  The separator spans the full width so the overlay
        // reads as a distinct floating panel.
        Element top_sep = separator() | color(pal.chrome);
        content = vbox({
            text("") | size(HEIGHT, EQUAL, 1),  // marginTop=1 above overlay
            top_sep,
            hbox({text("  "), content, filler()}) | bgcolor(pal.background),
        });
    }

    return content;
}

[[nodiscard]] inline std::optional<std::string> accept_selected_prompt_suggestion(
    const std::shared_ptr<ReplScreenState>& state) {
    if (state->autocomplete_suggestions.empty()) return std::nullopt;

    const int total = static_cast<int>(state->autocomplete_suggestions.size());
    const int selected = std::clamp(
        state->autocomplete_index < 0 ? 0 : state->autocomplete_index,
        0,
        total - 1);
    std::string accepted =
        state->autocomplete_suggestions[static_cast<std::size_t>(selected)].insert_text;
    if (accepted.empty()) {
        accepted =
            state->autocomplete_suggestions[static_cast<std::size_t>(selected)].display_text;
    }

    const auto& suggestion =
        state->autocomplete_suggestions[static_cast<std::size_t>(selected)];
    std::size_t start = suggestion.replacement_start;
    std::size_t end = suggestion.replacement_end;
    if (start == std::string::npos || end == std::string::npos ||
        start > end || end > state->input_text.size()) {
        start = 0;
        end = state->input_text.size();
    }

    state->input_text.replace(start, end - start, accepted);
    state->input_cursor = clamp_input_cursor(state->input_text, start + accepted.size());
    state->autocomplete_suggestions.clear();
    state->autocomplete_index = -1;
    state->is_prompt_input_active = true;
    state->last_keystroke = std::chrono::steady_clock::now();
    return state->input_text;
}

// =========================================================
// M7: Queue-based dialog rendering
// =========================================================
//
// The legacy dialog routing system (ReplMode → RouteDialog → dialog_stubs
// SimpleDialog builders) was REMOVED in M7 Task #124.
//
// ALL dialogs now flow through the DialogQueue:
//   - Engine side calls PushXxx() triggers in app.cppm / query_engine.cppm
//     (there are ZERO writes to a "DialogContext" bridge struct anywhere).
//   - Engine sets ReplMode for some legacy mode-aware UI chrome (mode opts,
//     vim indicator, etc.) but no longer uses it to determine dialog content.
//   - SyncReplModeToQueue was also REMOVED — audit confirmed there are ZERO
//     ReplMode→PushXxx() gaps in the engine call path; every engine-side
//     ReplMode assignment is paired with a matching DialogQueue push.
//
// Render path for queue:
//   RenderStandaloneDialog() (highest — fullscreen takeover)
//   RenderModalDialog()       (panel overlay — /settings, /tasks...)
//   RenderOverlayDialog()     (floating ToolPermission in scroll area)
//   RenderBottomDialog()      (prompt-affixed focused input dialogs)
//
// Event path:
//   HandleDialogQueueEvent() dispatches in priority order:
//     standalone > modal > overlay > bottom
//   with appropriate typing/animation suppression checks.
//
// Suppression rules (mirror TS REPL.tsx):
//   - is_prompt_input_active = typing in progress → Bands 2..6 hidden
//   - is_tool_animation_active = JSX tool animation running → Band3 hidden

// M7: Queue-based dialog rendering
// =========================================================

// =========================================================
// M7 Dialog Framework: dialog_queue renderer dispatchers
// =========================================================
//
// Four slots (DialogSlot enum) in priority order:
//   Standalone > Modal > Overlay > Bottom
//
// The Render* functions return Element (wrapped in optional or
// Element{} to signal "nothing to render").  Handle* events return
// true if they consumed the event.
namespace dialog_queue_render {

namespace dsys = dsys_fw;

/// Build the render-time width/height context for the registry.
/// TS REF: FullscreenLayout.tsx L422-426 — ModalContext provides
///   rows = terminalRows - MODAL_TRANSCRIPT_PEEK - 1
///   cols = columns - 4
/// When `is_modal` is true, the context's `modal_available_cols/rows`
/// are populated using the same formula so modal renderers can size
/// content to the actual available pane area.
[[nodiscard]] inline dsys::DialogRenderContext MakeContext(
    int term_w = 120, int term_h = 40, bool is_modal = false,
    const void* repl_state = nullptr)
{
    dsys::DialogRenderContext c;
    c.term_cols  = term_w;
    c.term_rows  = term_h;
    c.repl_state = repl_state;
    if (is_modal) {
        // TS REF: FullscreenLayout.tsx L423-424
        //   rows: terminalRows - MODAL_TRANSCRIPT_PEEK - 1
        //   columns: columns - 4
        // MODAL_TRANSCRIPT_PEEK = 2 (fullscreen_layout.cppm kModalTranscriptPeek)
        // The -1 accounts for the ▔ divider row.
        constexpr int kModalTranscriptPeek =
            cc::ui::layout::fullscreen::kModalTranscriptPeek;
        c.modal_available_cols = std::max(10, term_w - 4);
        c.modal_available_rows = std::max(4, term_h - kModalTranscriptPeek - 1);
    }
    return c;
}

/// Standalone dialog: full-takeover render (no chrome).
/// Passes full terminal dimensions (no modal adjustments) since standalone
/// dialogs own the entire screen.
[[nodiscard]] inline Element RenderStandaloneDialog(ReplScreenState& s,
                                                    int w = 120, int h = 40) {
    auto peek = s.dialog_queue.peek_standalone_mut();
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    return s.dialog_renderers.render(payload, MakeContext(w, h));
}

/// Modal dialog (stack top): rendered full-width dbox above the rest.
/// TS REF: FullscreenLayout.tsx L422-426 — wraps modal content in
///   <ModalContext value={{rows: ..., columns: ..., scrollRef: ...}}>
/// Computes modal-available dimensions (cols-4, rows-PEEK-1) and passes
/// them via DialogRenderContext.modal_available_cols/rows so renderers
/// can use actual pane geometry instead of hardcoded fallbacks.
[[nodiscard]] inline Element RenderModalDialog(ReplScreenState& s,
                                               int w = 120, int h = 40) {
    auto peek = s.dialog_queue.peek_modal_mut();
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    // is_modal=true → populate modal_available_cols/rows from TS formula.
    auto ctx = MakeContext(w, h, /*is_modal=*/true, &s);
    auto el = s.dialog_renderers.render(payload, ctx);
    if (!el) return Element{};
    // Clamp modal content to its available height (TS maxHeight enforcement).
    if (ctx.modal_available_rows > 0) {
        el = std::move(el) | size(HEIGHT, LESS_THAN, ctx.modal_available_rows);
    }
    return dbox({
        vbox({ filler(),
               hbox({ filler(), el, filler() }) | flex_shrink,
               filler() }) | flex,
    });
}

/// Overlay dialog (ToolPermission, Band3): centered floating dbox
/// clamped to 3/5 of terminal height so it never blocks the prompt.
/// Suppressed when prompt has input active or when allow_animation
/// dialogs are disabled (mid-tool-animation).
[[nodiscard]] inline Element RenderOverlayDialog(
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation = true,
    int w = 120, int h = 40)
{
    auto peek = s.dialog_queue.peek_overlay_mut();
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    if (!dsys::should_show_dialog(payload, is_prompt_input_active,
                                   allow_dialogs_with_animation)) {
        return Element{};
    }
    auto el = s.dialog_renderers.render(payload, MakeContext(w, h));
    if (!el) return Element{};
    // Size clamp: 60% of rows max.
    const int max_h = std::max(12, h * 3 / 5);
    el = std::move(el) | size(HEIGHT, LESS_THAN, max_h);
    // Inject inside a centered dbox above the messages area.
    return dbox({
        vbox({ filler(),
               hbox({ filler(), std::move(el) | flex_shrink, filler() })
                   | flex_shrink,
               filler() }) | flex,
    });
}

/// Bottom slot: banner-style dialogs pushed to the bottom of the screen.
/// TS REF: FullscreenLayout.tsx L414 — bottom slot wraps content in
///   maxHeight="50%" (half the terminal rows).  Pass actual dimensions.
[[nodiscard]] inline Element RenderBottomDialog(
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation,
    int w = 120, int h = 40)
{
    auto peek = s.dialog_queue.peek_bottom_mut(is_prompt_input_active,
                                               allow_dialogs_with_animation);
    if (!peek) return Element{};
    dsys::DialogPayloadVariant& payload = peek->get();
    if (std::holds_alternative<std::monostate>(payload)) return Element{};
    if (!dsys::should_show_dialog(payload, is_prompt_input_active,
                                   allow_dialogs_with_animation)) {
        return Element{};
    }
    auto el = s.dialog_renderers.render(payload, MakeContext(w, h));
    if (!el) return Element{};
    return std::move(el) | size(WIDTH, EQUAL, w);
}

// ── Event dispatch: priority Standalone > Modal > Overlay > Bottom ──
inline bool DispatchDialogQueueEvents(ReplScreenState& s,
                                      const ftxui::Event& ev,
                                      bool is_prompt_input_active,
                                      bool allow_dialogs_with_animation) {
    namespace dsys = dsys_fw;

    // Standalone always takes every event.
    {
        auto peek = s.dialog_queue.peek_standalone_mut();
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (s.dialog_renderers.handle_event(payload, ev)) return true;
                // Standalone Escape fallback — closes as Abort.
                if (ev == ftxui::Event::Escape) {
                    s.dialog_queue.pop_standalone();
                    return true;
                }
            }
        }
    }

    // Modal stack top.
    {
        auto peek = s.dialog_queue.peek_modal_mut();
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (s.dialog_renderers.handle_event(payload, ev)) return true;
                if (ev == ftxui::Event::Escape) {
                    s.dialog_queue.pop_modal();
                    return true;
                }
            }
        }
    }
    // Overlay (Band3).  Skip when suppressed so typing can continue.
    {
        auto peek = s.dialog_queue.peek_overlay_mut();
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (dsys::should_show_dialog(payload, is_prompt_input_active,
                                             allow_dialogs_with_animation)) {
                    if (s.dialog_renderers.handle_event(payload, ev)) return true;
                }
            }
        }
    }

    // Bottom.
    {
        auto peek = s.dialog_queue.peek_bottom_mut(is_prompt_input_active,
                                                   allow_dialogs_with_animation);
        if (peek) {
            dsys::DialogPayloadVariant& payload = peek->get();
            if (!std::holds_alternative<std::monostate>(payload)) {
                if (dsys::should_show_dialog(payload, is_prompt_input_active,
                                             allow_dialogs_with_animation)) {
                    if (s.dialog_renderers.handle_event(payload, ev)) return true;
                }
            }
        }
    }
    return false;
}

/// Convenience: combine Overlay + Modal + Bottom into a single dbox
/// that callers can overlay onto the base chrome.  Standalone is handled
/// separately (full-takeover, replaces the entire render).
/// TS REF: FullscreenLayout.tsx L422-426 — passes actual terminal dims.
[[nodiscard]] inline Element LayerAllDialogs(
    Element base_chrome,
    ReplScreenState& s,
    bool is_prompt_input_active,
    bool allow_dialogs_with_animation,
    int w = 120, int h = 40)
{
    Elements layers;
    layers.push_back(std::move(base_chrome));
    Element bottom = RenderBottomDialog(
        s, is_prompt_input_active, allow_dialogs_with_animation, w, h);
    if (bottom) {
        layers.push_back(vbox({
            filler(),
            std::move(bottom),
        }) | flex);
    }
    Element overlay = RenderOverlayDialog(
        s, is_prompt_input_active, allow_dialogs_with_animation, w, h);
    if (overlay) layers.push_back(std::move(overlay));
    Element modal = RenderModalDialog(s, w, h);
    if (modal)   layers.push_back(std::move(modal));
    if (layers.size() == 1) return layers.front();
    return dbox(std::move(layers));
}

} // namespace dialog_queue_render

/// Top-level dialog router: ReplMode -> overlay Element.
/// Panel modes (Normal / Tasks / Teams / Help / QuickOpen) return nullopt.
/// Priority matches TS getFocusedInputDialog() (REPL.tsx:2013):
///   Exit > message-selector > sandbox > permissions/hook/elicit >
///   cost/idle/ultraplan > onboarding > recs > panels.
[[nodiscard]] inline std::optional<Element> RouteDialog(
    ReplMode m, const ReplScreenState& s) {
    (void)s;
    // M7: All legacy ReplMode dialog routing is retired.  Dialogs now flow
    // exclusively via the 4-slot DialogQueue with priority
    // Standalone > Modal > Overlay > Bottom.  Any remaining ReplMode-driven
    // chrome (IdleReturn return-from-idle prompt, onboarding, etc.) is
    // rendered by the owning slot in dialog_queue_render.  This stub
    // preserves the file's historical RouteDialog() API so downstream
    // callers (RenderReplScreen) don't need another rewrite.
    switch (m) {
      default: break; }
    return std::nullopt;
}

// =========================================================
// Full layout composition
// =========================================================

/// M1: Composed via the FullscreenLayout slot-system (faithful port of TS
/// FullscreenLayout.tsx).  The previously-flat top-to-bottom vbox is now
/// slotted:
///   scrollable slot (flexGrow region) =
///     [WelcomeHeader (fresh session)] | Messages | Spinner | Tasks/Teams
///   bottom slot (pinned, flexShrink=0) =
    ///     PromptInput | Footer
///   modal slot (dbox overlay, bottom-anchored) =
///     RouteDialog() when non-null (with MODAL_TRANSCRIPT_PEEK peek)
///   overlay slot =
///     (reserved — engine wires PermissionRequest here in a future milestone;
///      currently permission flows through RouteDialog as the modal slot,
///      matching how it rendered before.)
///
    /// All existing render functions are PRESERVED (RenderWelcomeHeader,
    /// RenderMessages, RenderSpinner, RenderPromptInput,
/// RouteDialog) — only HOW they are composed changed.  Visual order is
/// preserved: messages scroll above, status/prompt pinned below.
///
/// Fix #1: welcome header atop the list on a fresh session.
    /// Fix #7: StatusLine lives inside the prompt footer, matching TS.
/// Fix #11: terminal size probed once per frame for adaptive clamping.
[[nodiscard]] inline Element RenderReplScreen(ReplScreenState& s,
    // GAP 3: msg-system-api-error-retry — retry callback threaded through
    // to RenderMessages so SystemAPIError rows can render a working Retry pill.
    std::function<void()> on_retry = nullptr,
    // P2 gap api-error-retry: clear-session callback threaded through to
    // RenderMessages for session-expired error cards.
    // TS REF: SystemAPIErrorMessage.tsx onClearSession.
    std::function<void()> on_clear_session = nullptr,
    // TS REF: Messages.tsx L703-712 + Markdown.tsx L186-235 — shared
    // StreamingMarkdown instance for the streaming-text tail row.
    ::cc::ui::StreamingMarkdown* streaming_md = nullptr) {
    // Probe terminal size once per frame for adaptive layout (fix #11).
    auto [term_cols, term_rows] = cc::ui::ink_utils::query_terminal_size();
    if (term_cols <= 0) term_cols = 80;
    if (term_rows <= 0) term_rows = 24;
    s.viewport_height_lines = std::max(1, term_rows - 5);

    // Spinner frame tick: monotonically increments per render call so the
    // tool-use header spinner animates.  (The interactive ToolUseMessage
    // component also drives its own counter; this feeds the static render
    // path used by message_list row dispatch.)
    static int spinner_frame = 0;
    ++spinner_frame;

    // ── UnseenDivider computation (TS: useUnseenDivider) ────────────────
    // When the user has scrolled away from bottom, compute the in-transcript
    // "N new messages" divider anchor + count.  Cleared on repin by
    // ScrollTranscript / on_pill_click (divider_index.reset()).
    if (s.divider_index.has_value()) {
        s.unseen_divider = ComputeUnseenDivider(s);
        if (s.unseen_divider.has_value()) {
            s.unseen_message_count = static_cast<int>(s.unseen_divider->count);
            s.pill_visible = true;
        }
    } else {
        s.unseen_divider.reset();
    }

    namespace fl = cc::ui::layout::fullscreen;
    fl::FullscreenLayoutSlots slots;
    slots.term_cols = term_cols;
    slots.term_rows = term_rows;

    // ── scrollable slot (flexGrow region) ───────────────────────────────
    // Builds the same top→bottom order the old flat vbox had for the
    // message transcript area: welcome header (fresh session), messages.
    // Spinner is NOT a scroll row — it lives in the pinned chrome between
    // the messages list and the prompt input (TS BriefSpinner marginTop=1).
    //
    // TS PARITY (Fix 2026-07-02): Logo/welcome lives INSIDE the scrollable
    // area, not in a pinned header.  In TS Messages.tsx the LogoHeader is a
    // thin 1-row bar; the full welcome card (LogoV2 condensed / compact) is
    // rendered inside the VirtualMessageList scrollback.  Putting it here
    // means: (a) blank space appears BELOW messages, not between logo and
    // messages; (b) as messages arrive and pin-to-bottom engages, the logo
    // scrolls out of the visible viewport naturally.
    Elements L;
    Elements scroll_rows; scroll_rows.reserve(2);
    const auto visible_messages = BuildVisibleMessages(s);

    // ── Welcome / logo card (passed as leading element inside yframe) ───
    // TS PARITY (2026-07-03 fix): LogoV2 welcome card is the first element
    // inside the VirtualMessageList scrollback.  It is ALWAYS present in
    // the scroll content — when messages overflow the viewport and
    // pin-to-bottom engages, the logo scrolls above the visible window but
    // is reachable by scrolling up.
    //
    // EXCEPTION: when a local command overlay (/skills, /help, etc.) is
    // active with no real conversation messages (s.messages empty), we
    // skip the logo so the command output has full viewport space.  This
    // matches TS where command overlays are not "real" transcript entries.
    Elements logo_leading;
    const bool has_real_messages = !s.messages.empty();
    const bool has_command_overlay = s.active_local_jsx_command;
    if (has_real_messages || !has_command_overlay) {
        logo_leading.push_back(
            RenderWelcomeHeader(s, spinner_frame, term_cols)
            | size(WIDTH, EQUAL, term_cols));
    }

    // ── Messages yframe (logo prepended INSIDE, fills scrollable) ───────
    // render_messages_list_view returns yframe | vscroll_indicator | flex
    // with the logo card as its first scroll child.  Pin-to-bottom logic
    // in messages_list.cppm keeps short content top-aligned (no blank
    // space above messages), and scrolls to bottom only when content
    // exceeds viewport.
    scroll_rows.push_back(RenderMessages(
        visible_messages, s.selected_message_idx,
        s.viewport_height_lines, s.scroll_offset,
        s.scroll_pinned_to_bottom, spinner_frame,
        s.unseen_divider,
        std::move(logo_leading),
        s.is_brief_mode,
        s.expanded_keys,
        s.is_transcript_mode,
        s.show_all_in_transcript,
        // TS REF: Messages.tsx L382-389  isStreamingThinkingVisible.
        // Threaded from app.cppm's is_streaming_thinking_visible() helper.
        s.streaming_thinking_globally_visible,
        on_retry,
        // P2 gap api-error-retry: thread on_clear_session for session-expired
        // error cards.
        on_clear_session,
        // TS REF: Messages.tsx L703-712 + Markdown.tsx L186-235 — thread
        // the shared StreamingMarkdown instance to the messages list.
        streaming_md));
    // Spinner lives in the chrome BETWEEN messages list and prompt input
    // (TS BriefSpinner marginTop=1, NOT a message row inside scroll content).
    Element spinner_chrome = text("");
    if (s.spinner_mode != SpinnerMode::Hidden)
        spinner_chrome = RenderSpinner(s.spinner_mode, s.spinner_verb,
                                       s.spinner_tip, spinner_frame);
    // M7.5: Panel views (Tasks/Teams/Help/Settings/About/QuickOpen) are
    // now rendered as modal dialogs via DialogQueue — no longer inlined
    // in the scrollable slot.
    (void)L;
    slots.scrollable = vbox(std::move(scroll_rows));

    // ── Pinned header (non-scroll) ─────────────────────────────────────
    // TS Messages.tsx has a thin LogoHeader bar above VirtualMessageList
    // that stays visible even when the welcome card scrolls off.  Without
    // this, pin-to-bottom scrolls the full welcome card out of view and
    // the user sees "logo 也没了" (user report 2026-07-04).
    //
    // We render a compact 1-line logo bar here so the app identity is
    // always visible at the top of the terminal.  The full welcome card
    // still lives inside the scrollable area (first child of yframe).
    {
        namespace lv2 = cc::ui::logo_v2;
        const std::string version = s.app_version.empty()
            ? std::string("0.0.0") : s.app_version;
        const std::string model_line = !s.model_display_name.empty()
            ? s.model_display_name
            : s.settings_model;
        slots.header = lv2::render_logo_header_bar(version, model_line, term_cols);
    }

    if (!s.active_local_jsx_command) {
        // ── bottom slot (pinned, flexShrink=0) ──────────────────────────────
        // Chrome order: [spinner (marginTop=1)] → [suggestions overlay?] →
        //               [prompt input] → [footer]
        //
        // Faithful to TS PromptInputFooter structure:
        //   suggestions overlay?  →  prompt input  →  footer (left/right columns)
        //
        // The footer contains StatusLine (optional, user-configurable) +
        // PromptInputFooterLeftSide (mode indicator, tasks, teams, hints) on
        // the left, and bridge/notifications on the right.
        //
        // STABLE HEIGHT: The LeftSide row is always exactly 1 row so scroll
        // content never shifts when hints change.  StatusLine adds a row when
        // present but is conditionally shown only in prompt mode + not short.
        namespace pif = cc::ui::prompt::footer;

        // Build StatusLine options (user-configurable command-driven status).
        //
        // Faithful to TS StatusLine.tsx:
        //   - Configured by settings.statusLine, rendered only in prompt mode
        //     and hidden in short fullscreen layouts
        //   - content comes from executing the user's shell command
        //   - In fullscreen, reserves a row even while loading (stable height)
        //   - Text may contain ANSI escape codes for coloring
        const bool is_fullscreen = cc::utils::is_fullscreen_enabled();
        const bool is_short = is_fullscreen && term_rows < 24;
        const bool status_line_configured =
            s.status_line_enabled && !s.status_line_command.empty();

        // P0-6 builtin statusline: populate fallback data from screen state.
        // When the user's command returns empty (or no command configured),
        // RenderStatusLine() uses this to show folder/git/model/token info.
        pif::BuiltinStatusLineData builtin_data;
        builtin_data.cwd = s.cwd;
        builtin_data.git_branch = s.git_branch;
        // Prefer model_display_name (human-friendly), fall back to model_name.
        builtin_data.model_name = !s.model_display_name.empty()
            ? s.model_display_name
            : s.status_bar.model_name;
        builtin_data.input_tokens = s.status_bar.input_tokens;
        builtin_data.output_tokens = s.status_bar.output_tokens;
        builtin_data.context_token_count = s.status_bar.context_token_count;
        builtin_data.cost_usd = s.status_bar.cost_usd;
        // Context window size: use 200k default; model-specific overrides
        // could be added later from model metadata.
        builtin_data.context_window_size = 200000;

        pif::StatusLineOptions status_line_opts;
        status_line_opts.content = s.status_line_text;
        status_line_opts.builtin = std::move(builtin_data);
        // Show statusline when:
        //   (a) user has a configured statusLine command, OR
        //   (b) builtin data is available (always, since cwd is set)
        // Same mode guards apply: prompt mode, not bash, not short terminal.
        const bool in_prompt_mode =
            s.input_mode == InputMode::Normal &&
            !effective_is_bash(s) &&
            !is_short;
        status_line_opts.should_display = in_prompt_mode &&
            (status_line_configured || !status_line_opts.content.empty() ||
             status_line_opts.builtin.has_value());
        status_line_opts.is_fullscreen = is_fullscreen;
        status_line_opts.padding_x = s.status_line_padding;

        // Map InputMode to footer PromptInputMode.  Text-derived mode takes
        // precedence over state-toggle (TS getInputMode semantics — see
        // effective_is_bash()).
        // NOTE: Both InputMode and pif::PromptInputMode are now the same
        // unified type (cc::ui::common::PromptInputMode), so this is a
        // direct assignment with bash-detection override.
        pif::PromptInputMode footer_mode =
            static_cast<pif::PromptInputMode>(s.input_mode);
        if (effective_is_bash(s)) {
            footer_mode = pif::PromptInputMode::Bash;
        } else if (footer_mode == pif::PromptInputMode::Bash) {
            // State says bash but text doesn't start with '!' — normalize
            // to Normal (consistent with old switch default behavior).
            footer_mode = pif::PromptInputMode::Normal;
        }
        // ── Assemble the bottom slot ──
        // Chrome order: [marginTop gap] → [spinner (marginTop=1)] →
        //               [suggestions overlay?] → [prompt input] → [footer]
        //
        // TS REF: PromptInput.tsx:2244 — marginTop={briefOwnsGap ? 0 : 1} on the
        // outermost container.  In non-brief mode this is a 1-row gap between the
        // scrollback area and the top border of the input box.  We emulate with a
        // leading text("") row.
        L.reserve(5);
        L.push_back(text(""));   // marginTop=1
        if (s.spinner_mode != SpinnerMode::Hidden) {
            L.push_back(hbox({spinner_chrome, filler()}) | flex_shrink);
        }
        // Live teammate strip (TS CoordinatorAgentStatus.tsx AgentLine list):
        // one status + output-tail row per teammate, pinned just above the
        // prompt input. Pure render of state-owned data.
        if (!s.live_teammates.empty()) {
            L.push_back(hbox({
                teams::live::RenderLiveTeammateStrip(s.live_teammates, term_cols),
                filler(),
            }) | flex_shrink);
        }
        if (!s.autocomplete_suggestions.empty()) {
            // TS REF: FullscreenLayout.tsx L591-607 + PromptInputFooter.tsx L124-129
            // In fullscreen mode, suggestions are portaled to FullscreenLayout
            // as a floating overlay (position:absolute bottom:100% opaque:true).
            // In FTXUI we apply overlay styling (background + top border) when
            // is_fullscreen, and pass term_rows for adaptive maxVisibleItems.
            L.push_back(RenderPromptSuggestions(s, term_cols,
                /*is_overlay=*/is_fullscreen,
                /*term_rows=*/term_rows));
        }
        L.push_back(RenderPromptInput(s, term_cols));
        // PromptInputFooter: LeftSide carries mode/tasks/teams via
        // ModeIndicatorOptions; StatusLine is its own nested struct.
        pif::LeftSideOptions left_opts;
        // Pasting hint is visible for 100ms after the last paste batch
        // (TS PASTE_COMPLETION_TIMEOUT_MS = 100).
        if (s.pasting_since) {
            const auto age = std::chrono::steady_clock::now() - *s.pasting_since;
            if (age <= std::chrono::milliseconds(100)) {
                left_opts.is_pasting = true;
            } else {
                s.pasting_since.reset();
            }
        }
        // Idle Ctrl+C double-press footer ("Press <key> again to exit"),
        // projected from the app-layer ExitHandler. Expiry is event-driven
        // exactly like the pasting hint (no ticker).
        // TS REF: PromptInputFooterLeftSide.tsx:150
        //   `Press {exitMessage.key} again to exit` — with key "Ctrl-C"
        //   RenderLeftSide composes the exact TS string.
        if (s.exit_message_until) {
            if (std::chrono::steady_clock::now() <= *s.exit_message_until) {
                left_opts.exit_message_show = true;
                left_opts.exit_message_key = s.exit_message_key;
            } else {
                s.exit_message_until.reset();
            }
        }
        left_opts.mode_indicator.mode                 = footer_mode;
        left_opts.mode_indicator.permission_mode      = s.permission_mode;
        left_opts.mode_indicator.background_task_count = s.background_task_count;
        left_opts.mode_indicator.teammate_count        = s.teammate_count;
        left_opts.mode_indicator.teams_selected        = s.teams_footer_selected;
        // Transcript/brief mode pills (TS REF: Messages.tsx isTranscriptMode + isBriefOnly).
        left_opts.mode_indicator.is_transcript_mode    = s.is_transcript_mode;
        left_opts.mode_indicator.is_brief_mode         = s.is_brief_mode;
        if (status_line_opts.should_display) {
            left_opts.mode_indicator.show_hint = false;
        }
        pif::FooterOptions footer_opts;
        footer_opts.status_line = std::move(status_line_opts);
        footer_opts.left_side   = std::move(left_opts);
        footer_opts.is_fullscreen = is_fullscreen;
        footer_opts.is_narrow = term_cols < 80;

        // Bridge status pill (TS REF: PromptInputFooter.tsx BridgeStatusIndicator
        // + bridgeStatusUtil.ts:124 getBridgeStatus).
        if (s.bridge_enabled) {
            namespace bs = cc::ui::prompt::footer;
            bs::BridgeOptions bopt;
            // Priority: reconnecting > connected(session) > connected,
            // mirroring getBridgeStatus (failed is surfaced via notification).
            if (s.bridge_reconnecting) {
                bopt.status = bs::BridgeStatus::Reconnecting;
            } else if (s.bridge_connected || s.bridge_session_active) {
                bopt.status = bs::BridgeStatus::Connected;
            } else {
                bopt.status = bs::BridgeStatus::Disconnected;
            }
            bopt.explicit_remote = s.bridge_explicit_remote;
            bopt.selected = s.bridge_selected;
            footer_opts.bridge = std::move(bopt);
        }

        // P1 Footer notifications — populate from ReplScreenState
        // TS REF: src/components/PromptInput/Notifications.tsx
        {
            auto& nd = footer_opts.notification;
            nd.api_key_status = s.api_key_status;
            // Voice indicator projection (TS Notifications.tsx
            // NotificationContent early-return).  Elapsed seconds are
            // wall-clock from the Processing transition anchor, matching
            // TS ProcessingShimmer elapsedSec = time / 1000.
            nd.voice_state = s.voice_footer_status;
            nd.voice_enabled = s.voice_enabled;
            nd.voice_processing_elapsed_sec = 0.0;
            if (s.voice_footer_status ==
                    cc::ui::prompt::FooterVoiceState::Processing &&
                s.voice_processing_since) {
                nd.voice_processing_elapsed_sec =
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() -
                        *s.voice_processing_since).count();
            }
            nd.is_remote = s.is_remote_session;
            nd.debug_mode = s.debug_mode;
            nd.verbose = s.verbose;
            nd.token_usage = s.status_bar.context_token_count;
            nd.is_overage_mode = s.show_overage_credit_upsell;
            nd.ide.connected = s.ide_connected;
            nd.ide.file_path = s.ide_file_path;
            nd.ide.selected_lines = s.ide_selected_lines;
            nd.dynamic_text = s.footer_dynamic_text;
            nd.dynamic_color = s.footer_dynamic_color;

            // P1: Advance the notification queue carousel — this is the
            // timer-based rotation through queued items.  Called here
            // (event-driven, on each render) rather than a constant ticker.
            // TS REF: src/context/notifications.tsx processQueue()
            namespace pif = cc::ui::prompt::footer;
            (void)pif::QueueAdvance(s.footer_notification_queue);
            nd.queue = s.footer_notification_queue;
        }

        // M4 faithful: outer chrome.
        //   * Clipboard image hint — stays nullopt until the engine wires up
        //     platform clipboard-image detection; no visual regression while
        //     empty.
        // NOTE: TS upstream does NOT render a brand pill in the footer
        // (PromptInputFooter.tsx has zero occurrences of "LOOM" /
        // "Loom" text).  Branding is rendered by CondensedLogo only
        // in the top header.
        L.push_back(pif::RenderPromptInputFooter(footer_opts));

        slots.bottom = vbox(std::move(L)) | flex_shrink;
    }

    // M7: Standalone slot (trust dialog, first-run onboarding) takes over
    // the entire terminal — no chrome, no prompt, no messages rendered.
    // TS REF: FullscreenLayout.tsx L422-426 — ModalContext provides actual
    // terminal dimensions to dialogs.  Pass real term_cols/term_rows instead
    // of the old hardcoded 120x40.
    if (s.dialog_queue.has_standalone()) {
        return dialog_queue_render::RenderStandaloneDialog(s, term_cols, term_rows);
    }

    // Legacy RouteDialog path — only used when dialog_queue has no
    // overlay/bottom/modal slots.  Eventually this will be phased out
    // in favour of the queue for all dialogs.

    // ── M1 FullscreenLayout: 3-state sticky prompt chrome ─────────────
    // TS REF: FullscreenLayout.tsx lines 339-351 (3-state discriminant,
    //        padCollapsed resolution, headerPrompt guard).
    slots.sticky_prompt         = s.sticky_prompt;
    slots.sticky_clicked        = s.sticky_prompt_clicked;
    slots.hide_sticky           = false;
    slots.pill_visible          = s.pill_visible;
    slots.hide_pill             = false;
    slots.new_message_count     = s.unseen_message_count;

    // on_sticky_click: the TS pattern "onClick={headerPrompt.scrollTo}"
    // (line 344) sets stickyPrompt='clicked' (the literal sentinel) via a
    // stable setState that reacts before scrollTo side-effects fire.  We
    // match that order in C++: (1) flip sticky_prompt_clicked to hide the
    // header + keep padCollapsed=true; (2) compute the delta between the
    // prompt's visual line and current scroll_top and ask ScrollTranscript
    // to jump there.
    //
    // Captures: `&s` is a ReplScreenState& whose lifetime is bound to the
    // outer `std::shared_ptr<ReplScreenState>` in MakeReplScreen; it is
    // stable across renders.  The callback is only invoked from within
    // FTXUI event dispatch (same thread), so no data races.
    slots.on_sticky_click = [&s](const fl::StickyPrompt& sp) {
        s.sticky_prompt_clicked = true;
        // Jump so the target visual line is at the TOP of the viewport.
        // scroll_target_row is measured from scroll_top=0 (content
        // coordinates); ScrollTranscript(delta) is relative — so delta =
        // target - current.  Clamp against viewport_rows to avoid
        // overshooting below min-scroll.
        int current = std::max(0, s.scroll_offset);
        int delta   = static_cast<int>(sp.scroll_target_row) - current;
        if (delta != 0) {
            // We need to call ScrollTranscript which takes
            // shared_ptr<ReplScreenState>.  Here we only have a bare ref;
            // but this callback is dispatched from FTXUI's event loop from
            // within the outer MakeReplScreen Component's OnEvent chain
            // which owns the shared_ptr.  For a purely visual change
            // (clicking the header is a scroll, not engine-state mutation),
            // a direct offset mutation achieves the same effect without
            // requiring the shared_ptr here.
            int viewport = std::max(1, s.viewport_height_lines);
            int total;
            if (s.virtual_list_active) {
                namespace vl = cc::ui::messages::virtual_list;
                total = s.virtual_jh.total();
            } else {
                const auto vm = BuildVisibleMessages(s);
                total = EstimateTranscriptRows(vm);
            }
            int max_top = std::max(0, total - viewport);
            int old_top = std::clamp(current, 0, max_top);
            int target  = std::clamp(old_top + delta, 0, max_top);
            if (target != old_top) {
                s.scroll_offset = target;
                s.scroll_pinned_to_bottom = (target >= max_top);
                if (s.virtual_list_state) {
                    namespace vl = cc::ui::messages::virtual_list;
                    s.virtual_list_state->scroll_top = target;
                    vl::update_sticky_after_scroll(*s.virtual_list_state,
                                                    old_top);
                }
            }
        }
        // TS note: after the click, stickyPrompt stays at 'clicked' until
        // the NEXT scroll event re-emits a fresh {text,scrollTo} from
        // StickyTracker.  Any movement (wheel, PageUp, click-to-select)
        // that moves the viewport will write a new sticky_prompt and
        // clear sticky_prompt_clicked.  We therefore do NOT clear the
        // flag ourselves here.
    };

    // on_pill_click: TS lines 371-381 — clicking the "N new messages" pill
    // re-pins to the bottom.  Same lifetime reasoning as on_sticky_click.
    slots.on_pill_click = [&s] {
        int viewport = std::max(1, s.viewport_height_lines);
        int total;
        if (s.virtual_list_active) {
            namespace vl = cc::ui::messages::virtual_list;
            total = s.virtual_jh.total();
        } else {
            const auto vm = BuildVisibleMessages(s);
            total = EstimateTranscriptRows(vm);
        }
        int max_top = std::max(0, total - viewport);
        int old_top = std::clamp(s.scroll_offset, 0, max_top);
        if (max_top != old_top) {
            s.scroll_offset = max_top;
            s.scroll_pinned_to_bottom = true;
            if (s.virtual_list_state) {
                namespace vl = cc::ui::messages::virtual_list;
                s.virtual_list_state->scroll_top = max_top;
                vl::update_sticky_after_scroll(*s.virtual_list_state, old_top);
            }
            // Clear the pill + unseen count on repin (mirrors TS onRepin
            // setting dividerIndex=null — the pill only shows while
            // pill_visible=true AND a divider snapshot exists.)
            s.pill_visible = false;
            s.unseen_message_count = 0;
            s.divider_index.reset();
            s.unseen_divider.reset();
        }
    };

    Element base = fl::ComposeFullscreen(std::move(slots));
    auto dlg = RouteDialog(s.mode, s);
    if (dlg) base = dbox({
        std::move(base) | dim,
        vbox({ filler(),
               hbox({ filler(), std::move(*dlg) | flex_shrink, filler() })
               | flex_shrink, filler() }) | flex });

    // M7: Layer Bottom + Overlay + Modal dialogs from the dialog_queue.
    // TS REF: FullscreenLayout.tsx L422-426 — ModalContext provides actual
    // terminal dimensions (cols-4, rows-PEEK-1) to modal dialogs.  Pass real
    // term_cols/term_rows here instead of the old hardcoded 120x40 so dialog
    // renderers get accurate viewport geometry.
    bool tool_animating = s.spinner_mode != SpinnerMode::Hidden;
    return dialog_queue_render::LayerAllDialogs(
        std::move(base), s, s.is_prompt_input_active,
        /*allow_dialogs_with_animation=*/!tool_animating, term_cols, term_rows);
}

// =========================================================
// FTXUI Component factory
// =========================================================

// UI13 agent_wizard is fully implemented and wired here: the wizard Component
// is lazily created on first entry to CreateAgent / EditAgent mode, and
// events are forwarded to it via forward_agent().
namespace dialog_router {

// -------------------------------------------------------------------
// Agent wizard helpers
// -------------------------------------------------------------------

namespace wizard_ns = cc::ui::agents::wizard;

using wizard_ns::AgentWizardOptions;
using wizard_ns::WizardDraft;

/// Lazily create (or re-create) the agent wizard component.
/// The mode (create vs edit) and agent_id are read from state.
[[nodiscard]] inline std::shared_ptr<Component> get_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_agent) {
        AgentWizardOptions opts;
        // M7: Read agent_id from the Standalone-slot EditAgentWizardPayload
        // in dialog_queue (instead of legacy DialogContext bridge struct).
        namespace dsys_gw = cc::ui::dialogs::system;
        auto& q_gw = s->dialog_queue;
        if (cb->load_agent_for_wizard &&
            q_gw.contains_type(dsys_gw::DialogType::EditAgentWizard)) {
            auto st = q_gw.peek_standalone();
            if (st) {
                // peek_standalone() returns optional<reference_wrapper<const V>>.
                // Unwrap with .get() so std::get_if<T> sees a const V*.
                const auto& variant = st->get();
                auto* p = std::get_if<dsys_gw::EditAgentWizardPayload>(&variant);
                if (p && !p->agent_name.empty()) {
                    opts.edit_agent = cb->load_agent_for_wizard(p->agent_name);
                }
            }
        }
        opts.on_save = [s, cb](const WizardDraft& draft) {
            if (cb->save_agent_from_wizard) cb->save_agent_from_wizard(draft);
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        opts.on_cancel = [s, cb] {
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->wizard_agent = std::make_shared<Component>(
            wizard_ns::AgentWizard(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->wizard_agent);
}

/// Forward an event to the agent wizard component.
inline bool forward_agent(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto wiz = get_agent_wizard(s, cb);
    return wiz && (*wiz)->OnEvent(std::move(ev));
}

/// Render the agent wizard content as an Element.
[[nodiscard]] inline Element render_agent_wizard(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto wiz = get_agent_wizard(s, cb);
    return wiz ? (*wiz)->Render() : text("");
}

/// Reset (destroy) the agent wizard so the next entry starts fresh.
inline void reset_agent_wizard(const std::shared_ptr<ReplScreenState>& s) {
    s->wizard_agent.reset();
}

// -------------------------------------------------------------------
// Agents menu helpers (UI13)
// -------------------------------------------------------------------

namespace agents_menu {

namespace cards = cc::ui::agents::cards;
namespace agent_display = cc::tools::agent_display;

struct AgentMenuOptions {
    std::vector<cards::AgentCardData> agents;
    std::function<void()> on_create_new;
    std::function<void(const std::string& id)> on_select;
    std::function<void()> on_cancel;
};

[[nodiscard]] inline bool is_built_in(const cards::AgentCardData& agent) {
    return agent.source == "built-in";
}

[[nodiscard]] inline std::string resolved_model_label(
    const cards::AgentCardData& agent) {
    if (agent.model_override && !agent.model_override->empty()) {
        return *agent.model_override;
    }
    return is_built_in(agent) ? "inherit" : "";
}

[[nodiscard]] inline std::vector<std::size_t> selectable_indices(
    const std::vector<cards::AgentCardData>& agents) {
    std::vector<std::size_t> out;
    out.reserve(agents.size());
    for (std::size_t i = 0; i < agents.size(); ++i) {
        if (!is_built_in(agents[i])) out.push_back(i);
    }
    return out;
}

class AgentMenuListBase : public ComponentBase {
public:
    explicit AgentMenuListBase(AgentMenuOptions opts)
        : opts_(std::move(opts)) {}

    Element Render() override {
        auto selectable = selectable_indices(opts_.agents);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (selected_position_ < 0 || selected_position_ >= item_count) {
            selected_position_ = 0;
        }

        const auto theme = cc::ui::design::theme::current_theme();
        const auto accent = theme.palette->primary;
        const auto muted = theme.palette->muted;
        const auto suggestion = theme.palette->suggestion;
        const int active_count = static_cast<int>(opts_.agents.size());

        Elements rows;
        rows.push_back(render_create_row(selected_position_ == 0, suggestion));

        for (const auto& group : agent_display::agent_source_groups()) {
            if (group.source == "built-in") continue;
            Elements group_rows;
            for (std::size_t i = 0; i < opts_.agents.size(); ++i) {
                const auto& agent = opts_.agents[i];
                if (agent.source != group.source) continue;
                group_rows.push_back(render_agent_row(
                    agent,
                    selected_position_for_index(selectable, i) == selected_position_,
                    /*selectable=*/true,
                    suggestion,
                    muted));
            }
            if (group_rows.empty()) continue;
            rows.push_back(text(""));
            rows.push_back(text("  " + group.label) | bold | color(muted) | dim);
            for (auto& row : group_rows) rows.push_back(std::move(row));
        }

        Elements built_in;
        for (const auto& agent : opts_.agents) {
            if (!is_built_in(agent)) continue;
            built_in.push_back(render_agent_row(
                agent,
                /*selected=*/false,
                /*selectable=*/false,
                suggestion,
                muted));
        }
        if (!built_in.empty()) {
            rows.push_back(text(""));
            rows.push_back(hbox({
                text("  Built-in agents") | bold | color(muted) | dim,
                text(" (always available)") | color(muted) | dim,
            }));
            for (auto& row : built_in) rows.push_back(std::move(row));
        }

        Element body = vbox(std::move(rows))
            | vscroll_indicator
            | yframe
            | size(HEIGHT, LESS_THAN, 26);

        Element title = vbox({
            text("Agents") | bold | color(accent),
            text(std::format("{} agents", active_count)) | color(muted) | dim,
        });

        return vbox({
            title,
            text(""),
            std::move(body),
        }) | flex;
    }

    bool OnEvent(Event ev) override {
        if (ev == Event::Escape) {
            if (opts_.on_cancel) opts_.on_cancel();
            return true;
        }

        auto selectable = selectable_indices(opts_.agents);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (item_count <= 0) return false;

        if (ev == Event::ArrowDown || ev == Event::Character('j')) {
            selected_position_ = (selected_position_ + 1) % item_count;
            return true;
        }
        if (ev == Event::ArrowUp || ev == Event::Character('k')) {
            selected_position_ = (selected_position_ - 1 + item_count) % item_count;
            return true;
        }
        if (ev == Event::Return) {
            if (selected_position_ == 0) {
                if (opts_.on_create_new) opts_.on_create_new();
                return true;
            }
            const int idx = selected_position_ - 1;
            if (idx >= 0 && idx < static_cast<int>(selectable.size())) {
                const auto& agent = opts_.agents[selectable[static_cast<std::size_t>(idx)]];
                if (opts_.on_select) opts_.on_select(agent.id);
            }
            return true;
        }
        return false;
    }

private:
    [[nodiscard]] static int selected_position_for_index(
        const std::vector<std::size_t>& selectable,
        std::size_t index) {
        for (std::size_t i = 0; i < selectable.size(); ++i) {
            if (selectable[i] == index) return static_cast<int>(i) + 1;
        }
        return -1;
    }

    [[nodiscard]] static Element render_create_row(bool selected, Color suggestion) {
        const auto c = selected ? suggestion : Color::Default;
        return hbox({
            text(selected ? "  › " : "    ") | color(c) | bold,
            text("Create new agent") | color(c),
        });
    }

    [[nodiscard]] static Element render_agent_row(
        const cards::AgentCardData& agent,
        bool selected,
        bool selectable,
        Color suggestion,
        Color muted) {
        const bool dimmed = !selectable;
        const auto c = selected ? suggestion : Color::Default;
        const auto model = resolved_model_label(agent);
        Elements parts;
        parts.push_back(text(selectable ? (selected ? "  › " : "    ") : "    ")
            | color(c) | bold);
        Element name = text(agent.name) | color(c);
        if (dimmed) name = name | dim;
        parts.push_back(std::move(name));
        if (!model.empty()) {
            parts.push_back(text(" · " + model)
                | color(selected ? c : muted)
                | dim);
        }
        return hbox(std::move(parts));
    }

    AgentMenuOptions opts_;
    int selected_position_ = 0;  // 0 is "Create new agent".
};

[[nodiscard]] inline Component AgentMenuList(AgentMenuOptions opts) {
    return Make<AgentMenuListBase>(std::move(opts));
}

} // namespace agents_menu

inline void close_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    s->mode = ReplMode::Normal;
    s->agents_component.reset();
    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
}

[[nodiscard]] inline std::shared_ptr<Component> get_agents_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->agents_component) {
        agents_menu::AgentMenuOptions opts;
        opts.agents = s->agent_cards;
        opts.on_create_new = [s, cb] {
            close_agents_menu(s, cb);
            if (cb->enqueue_slash_command) cb->enqueue_slash_command("/agents create");
        };
        opts.on_select = [s, cb](const std::string& id) {
            close_agents_menu(s, cb);
            if (cb->enqueue_slash_command) cb->enqueue_slash_command("/agents configure " + id);
        };
        opts.on_cancel = [s, cb] {
            close_agents_menu(s, cb);
        };
        s->agents_component = std::make_shared<Component>(
            agents_menu::AgentMenuList(std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->agents_component);
}

[[nodiscard]] inline Element render_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto comp = get_agents_component(s, cb);
    Element footer = text("  Press ↑↓ to navigate · Enter to select · Esc to go back")
        | color(cc::ui::design::theme::current_theme().palette->muted)
        | dim;
    return comp ? vbox({(*comp)->Render(), std::move(footer)}) | flex : text("");
}

inline bool forward_agents_menu(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto comp = get_agents_component(s, cb);
    if (comp && (*comp)->OnEvent(ev)) return true;
    if (ev == Event::Escape) {
        close_agents_menu(s, cb);
        return true;
    }
    return false;
}

// -------------------------------------------------------------------
// Settings dialog helpers (UI3)
// -------------------------------------------------------------------

namespace settings_ns = cc::ui::dialogs::settings_dialog;

using settings_ns::CommandResultDisplay;
using settings_ns::SettingsDialogOptions;
using settings_ns::SettingsTabId;
using settings_ns::MakeSettingsDialog;

/// Lazily create (or re-create) the settings dialog component.
/// If `state->settings_config` is non-null it is used for reads/writes,
/// otherwise a fresh internal ConfigManager is used (snapshot only).
[[nodiscard]] inline std::shared_ptr<Component> get_settings_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->settings_component) {
        // Use the supplied config manager, otherwise manufacture a default.
        static thread_local cc::core::ConfigManager fallback_config;
        cc::core::ConfigManager* cfg = s->settings_config
                                            ? static_cast<cc::core::ConfigManager*>(
                                                  s->settings_config)
                                            : &fallback_config;
        SettingsDialogOptions opts;
        opts.initial_tab = static_cast<SettingsTabId>(s->settings_initial_tab);
        opts.on_close = [s, cb](std::optional<std::string>, CommandResultDisplay) {
            s->mode = ReplMode::Normal;
            s->settings_component.reset();
            s->settings_initial_tab = 0;  // reset to General for next open
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
        };
        s->settings_component = std::make_shared<Component>(
            MakeSettingsDialog(*cfg, std::move(opts)));
    }
    return std::static_pointer_cast<Component>(s->settings_component);
}

/// Reset (destroy) the settings component so the next entry starts fresh
/// (empty dirty flag, pristine snapshot, tab=General).
inline void reset_settings_component(const std::shared_ptr<ReplScreenState>& s) {
    s->settings_component.reset();
    s->settings_initial_tab = 0;  // General
}

/// Render the settings dialog content as an Element.
[[nodiscard]] inline Element render_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto comp = get_settings_component(s, cb);
    return comp ? (*comp)->Render() : text("");
}

/// Forward an event to the settings dialog component.
inline bool forward_settings(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto comp = get_settings_component(s, cb);
    return comp && (*comp)->OnEvent(std::move(ev));
}

// -------------------------------------------------------------------
// Trust dialog helpers (UI8)
// -------------------------------------------------------------------
//
// Trust dialog lives in the STANDALONE slot (full-takeover).  We also
// keep a ReplMode-driven path as a bridge for callers that haven't
// migrated to the queue yet.  The Component is lazy-created via
// MakeWorkspaceTrustDialog() and stored as an opaque handle so
// ReplScreenState doesn't need to import the trust_dialog types.

namespace trust_ns = cc::ui::trust_dialog;
using trust_ns::TrustChoice;
using trust_ns::WorkspaceTrustProps;
using trust_ns::SecuritySources;
using trust_ns::MakeWorkspaceTrustDialog;

/// Lazily create (or re-create) the trust dialog component.
[[nodiscard]] inline std::shared_ptr<Component> get_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    if (!s->wizard_trust) {
        WorkspaceTrustProps props;
        props.workspace_path = s->cwd.empty() ? "." : s->cwd;
        props.on_done = [s, cb](TrustChoice choice) {
            // Bridge choices onto the permission + exit callbacks.  The
            // enum actually ships 5 values (AllowOnce/AlwaysAllow/ViewFile/
            // Cancel/EnableAnyway) — the "Exit" variant is handled by the
            // on_exit callback directly inside the trust dialog component
            // when the user chooses the corresponding option, not via
            // choice enum here.
            using C = TrustChoice;
            if (cb->on_permission_response) {
                switch (choice) {
                  case C::AllowOnce:    cb->on_permission_response(true, false); break;
                  case C::AlwaysAllow:  cb->on_permission_response(true, true);  break;
                  case C::EnableAnyway: cb->on_permission_response(true, true);  break;
                  case C::ViewFile:     cb->on_permission_response(true, false); break;
                  case C::Cancel:
                  default:              cb->on_permission_response(false, std::nullopt); break;
                }
            }
            s->mode = ReplMode::Normal;
            if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
            s->wizard_trust.reset();
        };
        s->wizard_trust = std::make_shared<Component>(
            MakeWorkspaceTrustDialog(std::move(props)));
    }
    return std::static_pointer_cast<Component>(s->wizard_trust);
}

/// Render the trust dialog content as an Element.
[[nodiscard]] inline Element render_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto d = get_trust_dialog(s, cb);
    return d ? (*d)->Render() : text("(trust dialog unavailable)");
}

/// Forward an event to the trust dialog component.
inline bool forward_trust_dialog(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb,
    Event ev) {
    auto d = get_trust_dialog(s, cb);
    return d && (*d)->OnEvent(std::move(ev));
}

// -------------------------------------------------------------------
// Tool-permission rich panel helpers (dlg-permission-legacy)
// -------------------------------------------------------------------
// TS-faithful panels for the dormant ReplMode::ToolPermission branch.
// TS REF: PermissionRequest.tsx:47-82 dispatches on tool identity.
// wizard_trust ownership keeps Component/PromptState alive across frames.
namespace tperm_bash  = cc::ui::permissions::bash_prompt;
namespace tperm_edit  = cc::ui::permissions::file_edit;
namespace tperm_write = cc::ui::permissions::file_write;
namespace tperm_one   = cc::ui::permissions::single_prompt;

enum class PermissionPanelKind { Bash, FileEdit, FileWrite, Generic };

// Case-insensitive tool classifier. Uses EXACT canonical names, not
// prefix/suffix matching: loose starts_with("bash")/ends_with("edit") would
// misdispatch unrelated tools ("bashful", "credit", "NotebookEdit",
// "MultiEdit", "BashOutputTool") to the wrong panel.
// TS canonical names: Bash (BashTool/toolName.ts), Edit
// (FileEditTool/constants.ts), Write (FileWriteTool/prompt.ts). NotebookEdit
// and MultiEdit are distinct TS tools with their own UI and must stay
// Generic here. A few historical CPP identifiers are kept as explicit
// aliases (not suffixes).
[[nodiscard]] inline PermissionPanelKind classify_permission_tool(
    std::string_view name) {
    std::string n;
    n.reserve(name.size());
    for (char c : name)
        n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto in = [&](std::initializer_list<const char*> names) {
        for (const char* x : names) {
            if (n == x) return true;
        }
        return false;
    };
    if (in({"bash"})) return PermissionPanelKind::Bash;
    if (in({"edit", "fileedit", "edittool", "fileedittool"}))
        return PermissionPanelKind::FileEdit;
    if (in({"write", "filewrite", "writetool", "filewritetool"}))
        return PermissionPanelKind::FileWrite;
    return PermissionPanelKind::Generic;
}

[[nodiscard]] inline tperm_one::RiskLevel tperm_risk(const PermissionRequestInfo& i) {
    std::string l = i.risk_labels.empty() ? "medium" : i.risk_labels.front();
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "low") return tperm_one::RiskLevel::Low;
    if (l == "high") return tperm_one::RiskLevel::High;
    if (l == "critical") return tperm_one::RiskLevel::Critical;
    return tperm_one::RiskLevel::Medium;
}

[[nodiscard]] inline std::string tperm_base(std::string_view p) {
    auto pos = p.find_last_of("/\\");
    return std::string{pos == std::string_view::npos ? p : p.substr(pos + 1)};
}

// Lazily build the panel, keyed on request identity so focus/props are
// never reused across requests.  State is reset BEFORE signalling the
// blocked permission worker (app_agent_menu get_permission_callback).
// One-shot guard: the bash/edit/write panels invoke on_abort AND
// on_decide(Abort) on one Esc — the TS contract is one terminal reply.
[[nodiscard]] inline std::shared_ptr<Component> get_tool_permission_component(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    const auto& i = *s->permission_request;
    const std::string leaf = i.bash_command.value_or(i.file_path.value_or(""));
    const std::string key = i.tool_name + "\x1f" + i.description + "\x1f" + leaf;
    if (s->tool_permission_component && key == s->tool_permission_key)
        return std::static_pointer_cast<Component>(s->tool_permission_component);
    s->tool_permission_component.reset();

    auto fired = std::make_shared<bool>(false);
    auto respond = [s, cb, fired](bool ok, std::optional<bool> always) {
        if (*fired) return;
        *fired = true;
        if (cb->on_permission_response) cb->on_permission_response(ok, always);
        s->mode = ReplMode::Normal;
        s->permission_request.reset();
        s->tool_permission_component.reset();
        s->tool_permission_key.clear();
        if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
    };
    auto deny = [respond] { respond(false, std::nullopt); };

    const std::string path = i.file_path.value_or("");
    const std::string rel  = i.file_relative_path.value_or(path);
    const std::string base = i.file_filename.value_or(tperm_base(path));

    switch (classify_permission_tool(i.tool_name)) {
      case PermissionPanelKind::Bash: {
        tperm_bash::BashPromptProps p;
        p.command = i.bash_command.value_or("");
        if (!i.bash_command && !i.description.empty()) p.command = i.description;
        if (i.bash_working_dir) p.working_dir = i.bash_working_dir;
        if (!i.description.empty()) p.description = i.description;
        p.is_destructive = i.bash_is_destructive;
        p.destructive_reason = i.bash_destructive_reason;
        p.show_always_allow = i.can_always_allow;
        if (!p.command.empty()) {  // token + ":*", mirrors RenderBashPermissionPromptForTest (permission_bash.cppm)
            auto sp = p.command.find_first_of(" \t");
            p.editable_prefix = (sp == std::string::npos ? p.command : p.command.substr(0, sp)) + ":*";
        }
        p.on_decide = [respond](tperm_bash::Decision d, std::string_view, std::string_view) {
            using D = tperm_bash::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AllowWithPrefix) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_bash::MakeBashPermissionPrompt(std::move(p)));
        break;
      }
      case PermissionPanelKind::FileEdit: {
        tperm_edit::FileEditPermissionProps p;
        p.file_path = path;
        p.old_string = i.file_old_content.value_or("");
        p.new_string = i.file_new_content.value_or("");
        p.replace_all = i.file_replace_all;
        p.file_content = i.file_old_content.value_or("");  // never null; sparse diff must not throw
        p.relative_path = rel;
        p.filename = base;
        if (i.file_language) p.language = *i.file_language;
        p.on_decide = [respond](tperm_edit::Decision d, tperm_edit::SessionScope, std::string_view) {
            using D = tperm_edit::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AllowSession) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_edit::MakeFileEditPermissionPrompt(std::move(p)));
        break;
      }
      case PermissionPanelKind::FileWrite: {
        tperm_write::FileWritePermissionProps p;
        p.file_path = path;
        p.content = i.file_new_content.value_or("");
        p.old_content = i.file_old_content.value_or("");
        p.file_exists = i.file_exists;
        p.relative_path = rel;
        p.filename = base;
        if (i.file_language) p.language = *i.file_language;
        p.on_decide = [respond](tperm_write::Decision d, tperm_write::SessionScope, std::string_view) {
            using D = tperm_write::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AllowSession) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_write::MakeFileWritePermissionPrompt(std::move(p)));
        break;
      }
      case PermissionPanelKind::Generic: {
        tperm_one::SinglePromptProps p;
        p.tool_name = i.tool_name;
        p.action_kind = tperm_one::ActionKind::Other;
        p.risk_level = tperm_risk(i);
        p.description = i.description;
        if (i.file_path) p.affected_paths.push_back(*i.file_path);
        p.detail = tperm_one::DetailGeneric{i.description};
        p.on_decide = [respond](tperm_one::Decision d, bool /*sandbox_requested*/) {
            using D = tperm_one::Decision;
            if (d == D::AllowOnce) respond(true, false);
            else if (d == D::AlwaysAllow) respond(true, true);
            else respond(false, std::nullopt);
        };
        p.on_abort = deny;
        s->tool_permission_component = std::make_shared<Component>(
            tperm_one::MakeSinglePromptDialog(std::move(p)));
        break;
      }
    }
    s->tool_permission_key = key;
    return std::static_pointer_cast<Component>(s->tool_permission_component);
}

[[nodiscard]] inline Element render_tool_permission(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb) {
    auto d = get_tool_permission_component(s, cb);
    return d ? (*d)->Render() : text("");
}

inline bool forward_tool_permission(
    const std::shared_ptr<ReplScreenState>& s,
    const std::shared_ptr<ReplScreenCallbacks>& cb, Event ev) {
    auto d = get_tool_permission_component(s, cb);
    return d && (*d)->OnEvent(std::move(ev));
}

} // namespace dialog_router

/// Build the REPL screen as an FTXUI Component.
/// Engine updates the externally-held state between frames.
/// Event tiers (TS global+command keybindings):
///   Dialog(Esc/y/n/a/c/r/q) > Global(Ctrl+C/D/L/O) > Input(Enter/
///     Ctrl+J/Tab/Shift+Tab/Up/Down/Esc/printable/Backspace)
[[nodiscard]] inline Component ReplScreen(
    std::shared_ptr<ReplScreenState> state,
    ReplScreenCallbacks cbs) {
    auto cb = std::make_shared<ReplScreenCallbacks>(std::move(cbs));
    return Renderer([state, cb]() -> Element {
        // M7.5: All dialogs flow through DialogQueue — no legacy bridge needed.
        // Engine pushes via PushXxx() (app.cppm / query_engine.cppm).

        // M6: Faithful permission panels — bash / file_edit / file_write.
        // Rendered as a dbox overlay (matching TS overlay slot).
        if (state->mode == ReplMode::ToolPermission &&
            state->permission_request) {
            // dlg-permission-legacy: state-owned TS-faithful panel
            // (TS REF: PermissionRequest.tsx:47-82 dispatch by tool
            // identity) replaces the legacy paragraph(
            // render_permission_dialog(...)) ANSI string.
            Element base = RenderReplScreen(*state, cb->on_retry, cb->on_clear_session, cb->streaming_md);
            Element panel = dialog_router::render_tool_permission(state, cb);
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), panel | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        // UI3: SettingsView modal — render the tabbed settings dialog
        // over the dimmed REPL background.
        if (state->mode == ReplMode::SettingsView) {
            Element base = RenderReplScreen(*state, cb->on_retry, cb->on_clear_session, cb->streaming_md);
            Element settings_content = dialog_router::render_settings(state, cb);
            return dbox({
                base | dim,
                vbox({ filler(),
                       hbox({ filler(), settings_content | flex_shrink, filler() })
                           | flex_shrink,
                       filler() }) | flex });
        }
        if (state->mode == ReplMode::AgentsView) {
            Element agents_content = dialog_router::render_agents_menu(state, cb);
            return vbox({ filler(),
                          separator() |
                              color(cc::ui::design::theme::current_theme().palette->muted),
                          hbox({ text("  "),
                                 agents_content | flex,
                                 text("  ") }) | flex_shrink }) | flex;
        }
        // UI8: TrustDialog is a STANDALONE slot — it takes over the full
        // terminal.  No chrome, no prompt, no messages show behind it.
        if (state->mode == ReplMode::TrustDialog) {
            return vbox({
                filler(),
                hbox({ filler(),
                       dialog_router::render_trust_dialog(state, cb) | flex_shrink,
                       filler() }) | flex_shrink,
                filler() }) | flex;
        }
        return RenderReplScreen(*state, cb->on_retry, cb->on_clear_session, cb->streaming_md);
    })
         | CatchEvent([state, cb](Event ev) -> bool {
    // --- M7: dialog_queue event dispatch (priority 0) ---
    // Standalone > Modal > Overlay > Bottom.  This block runs FIRST
    // so that a ToolPermission overlay consumes y/n/a before the
    // legacy in_dialog / input paths see it.
    bool tool_animating = state->spinner_mode != SpinnerMode::Hidden;
    if (dialog_queue_render::DispatchDialogQueueEvents(
            *state, ev, state->is_prompt_input_active,
            /*allow_dialogs_with_animation=*/!tool_animating)) {
        return true;
    }

    // --- Panel-mode predicate ---
    auto is_panel = [](ReplMode m){ return
        m==ReplMode::Normal || m==ReplMode::TasksView
        || m==ReplMode::TeamsView || m==ReplMode::HelpView
        || m==ReplMode::QuickOpen; };
    const bool in_dialog = !is_panel(state->mode);

    // 0) Dialog queue — takes priority over legacy ReplMode dialogs
    //    (M7.5: migration path from ReplMode to DialogQueue).
    //    NOTE: has_standalone/modal/overlay/bottom are checked inline
    //    inside DispatchDialogQueueEvents() (called above), so this
    //    block is intentionally empty — the dedicated per-slot helper
    //    namespace functions are not re-exposed as free predicates here
    //    to avoid a duplicate definition with dialog_queue_render.
    (void)state;

    // 1) Dialog-context events
    if (in_dialog) {
        // UI8 TrustDialog: highest priority (standalone slot), takes every
        // event so that the 4-tier selection / countdown / YES-typing
        // gating can work reliably.
        if (state->mode == ReplMode::TrustDialog) {
            return dialog_router::forward_trust_dialog(state, cb, ev);
        }
        // Ctrl+L is a GLOBAL redraw (TS defaultBindings.ts:42, global
        // context) — it must work even while a tool-permission panel/dialog
        // is open, so handle it before forwarding the event to any panel
        // (which otherwise unconditionally consumes it).
        if (ev == Event::Character('\x0C')) {
            if (cb->on_redraw) cb->on_redraw();
            return true;
        }
        // dlg-permission-legacy: the panel owns all its documented keys;
        // runs BEFORE the Esc switch and legacy y/n/a block (left as
        // harmless dead fallback).  TS dispatch REF:
        // PermissionRequest.tsx:47-82.
        if (state->mode == ReplMode::ToolPermission) {
            return dialog_router::forward_tool_permission(state, cb, ev);
        }
        // UI13 agent wizard: forward every event to the wizard component
        // (it manages Esc/Enter/buttons internally).
        if (state->mode == ReplMode::CreateAgent ||
            state->mode == ReplMode::EditAgent) {
            return dialog_router::forward_agent(state, cb, ev);
        }
        if (state->mode == ReplMode::AgentsView) {
            return dialog_router::forward_agents_menu(state, cb, ev);
        }
        // UI15 wizard modes: forward every event to the wizard
        // component (they manage Esc/Enter/buttons internally).
        if (ev == Event::Escape) {
            // Critical dialogs defer to y/n/a/c/r/q handlers — EXCEPT
            // CostThreshold where Esc MUST ACKNOWLEDGE (never quit / data-loss).
            switch (state->mode) {
              case ReplMode::ToolPermission:
              case ReplMode::SandboxPermission:
              case ReplMode::WorkerSandboxPermission:
                break;
              case ReplMode::CostThreshold: {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                ct::HandleCostThresholdEvent(st, Event::Escape);
                return true; }
              default:
                state->mode = ReplMode::Normal;
                if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                return true; } }
        // CostThreshold: Enter / Space / shortcuts all fire on_done().
        // Delegate to the unified HandleCostThresholdEvent.
        if (state->mode == ReplMode::CostThreshold) {
            if (ev == Event::Return ||
                (ev.is_character() && ev.character() == " ")) {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                ct::HandleCostThresholdEvent(st, ev);
                return true;
            }
        }
        if (ev.is_character()) {
            char c = ev.character()[0];
            const bool is_perm =
                state->mode==ReplMode::ToolPermission ||
                state->mode==ReplMode::SandboxPermission ||
                state->mode==ReplMode::WorkerSandboxPermission ||
                state->mode==ReplMode::Elicitation ||
                state->mode==ReplMode::PromptHook;
            if (is_perm) {
                if (c=='y'||c=='Y'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(true,false);
                    return true;
                }
                if (c=='n'||c=='N'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(false,std::nullopt);
                    return true;
                }
                if (c=='a'||c=='A'){
                    if (cb->on_permission_response)
                        cb->on_permission_response(true,true);
                    return true;
                }
            }
            // CostThreshold: ALL characters are swallowed by the unified
            // handler.  Shortcuts (g/y/o/k) will fire on_done(); any other
            // character is silently consumed to prevent prompt-injection.
            if (state->mode == ReplMode::CostThreshold) {
                namespace ct = cc::ui::dialogs::cost_threshold;
                ct::CostThresholdState st;
                st.on_done = [&state, cb] {
                    state->mode = ReplMode::Normal;
                    if (cb->on_mode_change) cb->on_mode_change(ReplMode::Normal);
                    if (cb->on_dialog_action)
                        cb->on_dialog_action(ReplMode::CostThreshold, 0);
                };
                (void)ct::HandleCostThresholdEvent(st, ev);
                return true;
            }
        }
    }

    // 2) Global shortcuts
    if (ev == Event::Character('\x03'))
        { if (cb->on_interrupt) cb->on_interrupt(); return true; }
    if (ev == Event::Character('\x04'))
        { if (cb->on_exit) cb->on_exit(); return true; }
    // Ctrl+L: force terminal redraw WITHOUT mutating input.
    // TS REF: src/keybindings/defaultBindings.ts:42 'ctrl+l' -> 'app:redraw'
    //   (Global context, so it works while dialogs are open too) and
    //   useGlobalKeybindings.tsx:225-228 handleRedraw -> ink forceRedraw,
    //   which writes ERASE_SCREEN (CSI 2 J = '\x1b[2J') + CURSOR_HOME
    //   (CSI H = '\x1b[H') and repaints the current content; input_text,
    //   cursor and autocomplete suggestions are never touched.
    if (ev == Event::Character('\x0C')) {
        if (cb->on_redraw) cb->on_redraw();
        return true;
    }
    // Ctrl+O: toggle transcript mode (TS: app:toggleTranscript, global context).
    // In transcript mode the message list shows ALL message types (bypassing
    // brief/dropText filters), capped at last 30 unless show_all_in_transcript.
    // TS REF: Messages.tsx L459 (isTranscriptMode = screen === 'transcript')
    //         + REPL.tsx Ctrl+O → setScreen('transcript') toggle.
    if (!in_dialog && ev == Event::Character('\x0F')) {
        state->is_transcript_mode = !state->is_transcript_mode;
        // When exiting transcript mode, also reset show_all_in_transcript
        // so re-entering starts from the capped default (TS: showAllInTranscript
        // defaults false — user must press Ctrl+E each session to lift the cap).
        if (!state->is_transcript_mode) {
            state->show_all_in_transcript = false;
        }
        return true;
    }

    // Ctrl+R: enter history search mode by injecting "@history " into input.
    // This triggers the @history autocomplete branch in RefreshAutocompleteSuggestions
    // which reads persisted prompt history from ~/.loom/history.jsonl.
    // TS REF: src/hooks/useHistorySearch.ts:151 (handleStartSearch — Ctrl+R enters
    //   history search mode with substring matching against persisted history)
    // TS REF: src/components/PromptInput/PromptInput.tsx — Ctrl+R keyboard shortcut
    //   dispatches 'chat:openHistorySearch' which opens the HistorySearchDialog.
    if (!in_dialog && ev == Event::Character('\x12')) {
        if (!state->input_text.starts_with("@history")) {
            state->input_text = "@history ";
            state->input_cursor = state->input_text.size();
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
            state->dismissed_autocomplete_for_input.clear();
        }
        state->is_prompt_input_active = true;
        state->last_keystroke = std::chrono::steady_clock::now();
        return true;
    }

    // Ctrl+E: dual behavior depending on mode.
    //   - In transcript mode: toggle show_all_in_transcript (lift/restore 30-msg cap).
    //     TS: transcript:toggleShowAll (Transcript context, defaultBindings L163).
    //   - Otherwise: toggle expand/collapse of all tool rows in visible transcript.
    //     TS REF: Messages.tsx expandedKeys (L563) — user can expand tool results
    //     to see full output.  This shortcut toggles ALL tool rows at once.
    if (!in_dialog && ev == Event::Character('\x05')) {
        if (state->is_transcript_mode) {
            // Transcript mode: lift or restore the 30-message cap.
            state->show_all_in_transcript = !state->show_all_in_transcript;
        } else {
            // Normal mode: expand or collapse all tool rows.
            namespace ml = cc::ui::messages_list;
            namespace msg = cc::ui::messages;
            if (state->expanded_keys.empty()) {
                for (const auto& m : state->messages) {
                    if (m.tool_name && !m.tool_name->empty()) {
                        state->expanded_keys.insert(*m.tool_name);
                    }
                }
            } else {
                state->expanded_keys.clear();
            }
        }
        return true;
    }

    // Ctrl+S: stash / restore prompt (TS: 'chat:stash' action, defaultBindings L85).
    // TS REF: src/components/PromptInput/PromptInput.tsx:1356-1383 — handleStash():
    //   - If input is empty and stashedPrompt exists → pop stash (restore)
    //   - If input is non-empty → push stash (save text + cursorOffset + pastedContents),
    //     clear input, clear pastedContents.
    // The stash notice (prompt_stash_notice.cppm) renders above the input area
    // when HasStashedPrompt() is true, so the user knows their typed text was saved.
    // Auto-restore happens on the next non-slash-command submit (RestoreStashedPrompt
    // called at lines 3804 and 3828 below).
    if (!in_dialog && ev == Event::Character('\x13')) {
        if (state->input_text.empty() && HasStashedPrompt(state)) {
            // Input empty + stash exists → restore (pop stash into input)
            RestoreStashedPrompt(state);
        } else if (!state->input_text.empty()) {
            // Input non-empty → stash current input, then clear it
            StashCurrentPrompt(state);
            state->input_text.clear();
            state->input_cursor = std::string::npos;
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
        }
        state->last_keystroke = std::chrono::steady_clock::now();
        state->is_prompt_input_active = true;
        return true;
    }

    if (!in_dialog &&
        state->active_local_jsx_command &&
        cb->on_local_jsx_event &&
        cb->on_local_jsx_event(ev)) {
        return true;
    }

    if (!in_dialog && state->active_local_jsx_command && ev == Event::Escape) {
        if (cb->on_local_jsx_cancel) {
            cb->on_local_jsx_cancel();
        } else {
            state->active_local_jsx_command = false;
            state->active_local_jsx_command_name.clear();
            state->active_local_jsx_command_args.clear();
            state->active_local_jsx_content.clear();
        }
        return true;
    }

    if (!in_dialog && ev.is_mouse()) {
        if (ev.mouse().button == Mouse::WheelUp) {
            return ScrollTranscript(state, -3);
        }
        if (ev.mouse().button == Mouse::WheelDown) {
            return ScrollTranscript(state, 3);
        }
    }
    if (!in_dialog && state->autocomplete_suggestions.empty()) {
        const int page = std::max(1, state->viewport_height_lines / 2);
        if (ev == Event::PageUp) {
            return ScrollTranscript(state, -page);
        }
        if (ev == Event::PageDown) {
            return ScrollTranscript(state, page);
        }
    }

    // 3) Input-context events
    const bool accept_input = !in_dialog;
    if (accept_input) {
        const int asn = static_cast<int>(state->autocomplete_suggestions.size());
        // Enter
        if (ev == Event::Return && asn > 0) {
            const int selected = std::clamp(
                state->autocomplete_index < 0 ? 0 : state->autocomplete_index,
                0,
                asn - 1);
            const bool submit =
                state->autocomplete_suggestions[static_cast<std::size_t>(selected)]
                    .submit_on_return;
            auto accepted = accept_selected_prompt_suggestion(state);
            if (submit && accepted && cb->on_submit) {
                // TS REF: src/components/PromptInput/inputModes.ts:23-29
                //   (getValueFromInput)
                // Strip '!' mode prefix from accepted value before engine.
                namespace figs = cc::ui::design::figures;
                std::string submit_text =
                    std::string(figs::strip_mode_prefix(*accepted));
                cb->on_submit(submit_text, state->input_mode);
                // TS REF: src/components/PromptInput/inputModes.ts:4-14
                //   (prependModeCharacterToInput) + REPL.tsx:3318
                // History stores the mode-prefixed form for round-trip
                // mode detection on recall.  If the user toggled bash via
                // bare '!' (input_text has NO '!'), prepend it.  If the
                // text already carries '!' (direct "!cmd" typing), keep
                // it as-is to avoid double-prefix.
                {
                    namespace figs = cc::ui::design::figures;
                    const bool text_has_prefix =
                        !accepted->empty() &&
                        (*accepted)[0] == figs::kBashModeChar;
                    const bool is_bash =
                        state->input_mode == InputMode::Bash;
                    const std::string hist_entry =
                        (is_bash && !text_has_prefix)
                            ? figs::prepend_mode_char(
                                  *accepted,
                                  figs::PromptMode::kBash)
                            : std::string(*accepted);
                    state->input_history.push_back(hist_entry);
                }
                if (state->input_history.size() > 1000) state->input_history.pop_front();
                state->history_index = std::string::npos;
                state->input_text.clear();
                state->input_cursor = std::string::npos;
                state->is_prompt_input_active = false;
                // GAP 2: auto-restore stashed prompt after submit completes.
                // TS REF: REPL.tsx L3344-3348 — restore stashedPrompt when
                // the input is cleared by a non-slash-command submit.
                RestoreStashedPrompt(state);
            }
            return true;
        }
        if (ev == Event::Return && !state->input_text.empty()) {
            // TS REF: src/components/PromptInput/inputModes.ts:23-29 (getValueFromInput)
            // Strip the '!' mode prefix before passing to engine.
            // History keeps the prefix for round-tripping
            // (prependModeCharacterToInput semantics in inputModes.ts:4-14).
            namespace figs = cc::ui::design::figures;
            std::string submit_text =
                std::string(figs::strip_mode_prefix(state->input_text));
            if (cb->on_submit) cb->on_submit(submit_text, state->input_mode);
            // TS REF: src/components/PromptInput/inputModes.ts:4-14
            //   (prependModeCharacterToInput) + REPL.tsx:3318
            // History stores the mode-prefixed form for round-trip
            // mode detection on arrow-up recall.  If the user toggled
            // bash via bare '!' (input_text has NO '!'), prepend it.
            // If the text already carries '!' (direct "!cmd" typing),
            // keep it as-is to avoid double-prefix.
            {
                namespace figs = cc::ui::design::figures;
                const bool text_has_prefix =
                    !state->input_text.empty() &&
                    state->input_text[0] == figs::kBashModeChar;
                const bool is_bash =
                    state->input_mode == InputMode::Bash;
                const std::string hist_entry =
                    (is_bash && !text_has_prefix)
                        ? figs::prepend_mode_char(
                              state->input_text,
                              figs::PromptMode::kBash)
                        : std::string(state->input_text);
                state->input_history.push_back(hist_entry);
            }
            if (state->input_history.size() > 1000) state->input_history.pop_front();
            state->history_index = std::string::npos;
            state->input_text.clear();
            state->input_cursor = std::string::npos;
            state->autocomplete_suggestions.clear();
            state->autocomplete_index = -1;
            state->is_prompt_input_active = false;
            // GAP 2: auto-restore stashed prompt after submit.
            // TS REF: REPL.tsx L3344-3348 — restore stashedPrompt when
            // the input is cleared by a non-slash-command submit.
            RestoreStashedPrompt(state);
            return true; }
        // Ctrl+Enter -> newline (Ctrl+J in terminals)
        if (ev == Event::Character('\x0A')) {
            insert_prompt_text(state, "\n");
            return true; }
        // Shift+Enter -> newline. Two terminal encodings:
        //   xterm modifyOtherKeys / CSI-u : ESC [ 13 ; 2 u
        //   kitty keyboard protocol       : ESC [ 27 ; 2 ; 13 ~
        if (ev.input() == "\x1b[13;2u" || ev.input() == "\x1b[27;2;13~") {
            insert_prompt_text(state, "\n");
            return true; }
        // Tab / Shift+Tab -> autocomplete
        if (ev == Event::Tab && asn > 0) {
            // AT-07: complete to the common prefix of all visible suggestion
            // insert_texts when it strictly extends what's typed (e.g. "@sr"
            // with {@src/readme, @src/main} → "@src/"); otherwise accept the
            // selected suggestion. Faithful to TS typeahead Tab behavior.
            const auto& sugg = state->autocomplete_suggestions;
            std::string common = sugg[0].insert_text;
            for (int k = 1; k < asn && !common.empty(); ++k) {
                const auto& ins = sugg[k].insert_text;
                std::size_t j = 0;
                while (j < common.size() && j < ins.size() && common[j] == ins[j]) ++j;
                common.resize(j);
            }
            const int idx = std::clamp(state->autocomplete_index, 0, asn - 1);
            const auto& sel = sugg[idx];
            const std::size_t rs = (sel.replacement_start == std::string::npos)
                ? 0 : sel.replacement_start;
            const std::size_t re = (sel.replacement_end == std::string::npos)
                ? state->input_text.size() : sel.replacement_end;
            const std::string typed = (rs <= re && re <= state->input_text.size())
                ? state->input_text.substr(rs, re - rs) : std::string{};
            if (!common.empty() && common.size() > typed.size() &&
                common.compare(0, typed.size(), typed) == 0) {
                state->input_text.replace(rs, re - rs, common);
                state->input_cursor = rs + common.size();
                state->is_prompt_input_active = true;
                state->last_keystroke = std::chrono::steady_clock::now();
            } else {
                (void)accept_selected_prompt_suggestion(state);
            }
            return true;
        }
        // Shift+Tab (ISO backtab) = \x1B[Z
        if (ev.input() == "\x1B[Z" && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ? asn - 1
                : (state->autocomplete_index - 1 + asn) % asn; return true; }
        // Shift+Tab without suggestions → cycle permission mode.
        // TS REF: PromptInput.tsx:1667 'chat:cycleMode' shortcut → handleCycleMode
        // → cyclePermissionMode().  The footer renders "(shift+tab to cycle)"
        // when a non-default permission mode is active; this makes it actually work.
        if ((ev.input() == "\x1B[Z" || ev == Event::TabReverse) && asn == 0) {
            namespace pif = cc::ui::prompt::footer;
            state->permission_mode = pif::GetNextPermissionMode(state->permission_mode);
            if (cb->on_permission_cycle) {
                cb->on_permission_cycle(state->permission_mode);
            }
            return true;
        }
        // Up / Down navigate visible autocomplete suggestions before history.
        if (ev == Event::ArrowUp && asn > 0) {
            state->autocomplete_index = state->autocomplete_index <= 0 ? asn - 1
                : state->autocomplete_index - 1; return true; }
        if (ev == Event::ArrowDown && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ||
                state->autocomplete_index >= asn - 1
                ? 0 : state->autocomplete_index + 1; return true; }
        // Ctrl+N (\x0e) / Ctrl+P (\x10) navigate autocomplete suggestions.
        // TS REF: src/hooks/useTypeahead.tsx:1344-1353 (raw ctrl+n/ctrl+p
        //   dispatched to handleAutocompleteNext/Previous) and
        //   :1242-1255 — next wraps selected>=length-1 -> 0; previous wraps
        //   selected<=0 -> length-1. Both early-return when suggestions are
        //   empty (and when a chord is pending — the CPP port has no chord
        //   system, so that gate is omitted). When asn==0 the event
        //   intentionally falls through (TS readline cursor/history movement
        //   is not implemented in this port).
        if (ev == Event::Character('\x0e') && asn > 0) {
            state->autocomplete_index = state->autocomplete_index < 0 ||
                state->autocomplete_index >= asn - 1
                ? 0 : state->autocomplete_index + 1; return true; }
        if (ev == Event::Character('\x10') && asn > 0) {
            state->autocomplete_index = state->autocomplete_index <= 0 ? asn - 1
                : state->autocomplete_index - 1; return true; }
        // Up (history back) / Down (history forward)
        if (ev == Event::ArrowUp && state->input_text.empty()
            && !state->input_history.empty()) {
            state->history_index = state->history_index == std::string::npos
                ? state->input_history.size() - 1
                : std::max<std::size_t>(0, state->history_index - 1);
            state->input_text = state->input_history[state->history_index];
            // TS REF: src/components/PromptInput/inputModes.ts:16-21
            //   (getModeFromInput)
            // Sync input_mode from the recalled entry's leading char so that
            // the prefix glyph stays correct after the user clears the text.
            {
                namespace figs = cc::ui::design::figures;
                state->input_mode =
                    (figs::get_mode_from_input(state->input_text) ==
                     figs::PromptMode::kBash)
                        ? InputMode::Bash
                        : InputMode::Normal;
            }
            state->input_cursor = state->input_text.size();
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        if (ev == Event::ArrowDown
            && state->history_index != std::string::npos) {
            if (state->history_index + 1 >= state->input_history.size()) {
                state->history_index = std::string::npos;
                state->input_text.clear();
                state->input_cursor = std::string::npos;
            } else {
                state->input_text = state->input_history[++state->history_index];
                // TS REF: inputModes.ts:16-21 (getModeFromInput) — sync mode
                // from the recalled entry's leading prefix character.
                {
                    namespace figs = cc::ui::design::figures;
                    state->input_mode =
                        (figs::get_mode_from_input(state->input_text) ==
                         figs::PromptMode::kBash)
                            ? InputMode::Bash
                            : InputMode::Normal;
                }
                state->input_cursor = state->input_text.size(); }
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now(); return true; }
        // Esc
        if (ev == Event::Escape) {
            // TS REF: PromptInput.tsx:1904 — Escape at cursor 0 exits any
            // special (bash) mode.  Runs first and does NOT itself consume the
            // event, so the existing autocomplete/selection/clear-text
            // priorities below still apply exactly as before.
            const bool mode_exited = exit_input_mode_if_at_start(state);
            if (!state->autocomplete_suggestions.empty()) {
                // INF-05: remember the dismissed input so a later non-mutating
                // keystroke (e.g. arrow keys) doesn't reopen the popup.
                state->dismissed_autocomplete_for_input = state->input_text;
                state->autocomplete_suggestions.clear();
                state->autocomplete_index = -1; return true; }
            if (state->selected_message_idx >= 0)
                { state->selected_message_idx = -1; return true; }
            // Esc double-press to clear non-empty input.
            // TS REF: src/hooks/useTextInput.ts:126-153 (handleEscape) +
            // src/hooks/useDoublePress.ts:6 DOUBLE_PRESS_TIMEOUT_MS = 800.
            // The autocomplete dismiss above mirrors PromptInput.tsx
            // disableEscapeDoublePress = suggestions.length>0: while the
            // popup is open the first Esc dismisses instead of arming, so
            // clearing takes Esc (dismiss) + Esc (arm) + Esc (clear).
            if (!state->input_text.empty()) {
                namespace pif = cc::ui::prompt::footer;
                const auto now_dp = std::chrono::steady_clock::now();
                constexpr auto kDoublePressWindow =
                    std::chrono::milliseconds(800);
                const bool armed = state->escape_pending_since.has_value() &&
                    (now_dp - *state->escape_pending_since) <= kDoublePressWindow;
                if (armed) {
                    // Second press inside the window: clear timer state,
                    // remove the hint immediately, persist BEFORE clearing
                    // (TS addToHistory(originalValue) guarded by trim()!==''),
                    // then clear text/offset/history.
                    state->escape_pending_since.reset();
                    pif::QueueRemoveNotification(
                        state->footer_notification_queue,
                        "escape-again-to-clear");
                    bool has_non_space = false;
                    for (unsigned char c : state->input_text) {
                        if (c != ' ' && c != '\t' && c != '\n' &&
                            c != '\r' && c != '\v' && c != '\f') {
                            has_non_space = true;
                            break;
                        }
                    }
                    if (has_non_space && cb->on_save_to_history) {
                        cb->on_save_to_history(state->input_text);
                    }
                    state->input_text.clear();
                    state->input_cursor = std::string::npos;
                    state->history_index = std::string::npos;
                    return true;
                }
                // First press (or an expired previous press): arm and show
                // the hint; input is NOT cleared. Remove first because
                // QueueAddNotification dedups an already-current same-key
                // item and would otherwise not refresh the 1000ms timeout.
                state->escape_pending_since = now_dp;
                pif::NotificationItem item;
                item.key = "escape-again-to-clear";
                item.text = "Esc again to clear";  // TS exact string
                item.color = "";
                item.priority = pif::NotificationPriority::Immediate;
                item.timeout_ms = 1000;            // TS timeoutMs: 1000
                pif::QueueRemoveNotification(
                    state->footer_notification_queue,
                    "escape-again-to-clear");
                pif::QueueAddNotification(
                    state->footer_notification_queue, item);
                return true;
            }
            // Empty input: TS handleEscape's setPending callback early-
            // returns (no arming, no notification). The mode-exit (bash ->
            // prompt) above still coexists when it happened.
            // Only the mode-exit happened (empty input, no popup/selection):
            // still consume the event so the reset is reflected.
            if (mode_exited) return true; }
        if (ev == Event::ArrowLeft) {
            move_prompt_cursor_left(state);
            return true;
        }
        if (ev == Event::ArrowRight) {
            move_prompt_cursor_right(state);
            return true;
        }
        if (ev == Event::Home) {
            state->input_cursor = 0;
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
            return true;
        }
        if (ev == Event::End) {
            state->input_cursor = state->input_text.size();
            state->is_prompt_input_active = true;
            state->last_keystroke = std::chrono::steady_clock::now();
            return true;
        }
        if (ev == Event::Delete) {
            if (delete_prompt_text(state)) return true;
            // At cursor 0 with nothing to delete: exit bash/special mode
            // (TS parity, PromptInput.tsx:1904 lists key.delete).
            if (exit_input_mode_if_at_start(state)) return true;
        }
        // Ctrl+U (\x15): kill from cursor to start of line.  When the cursor is
        // already at position 0 (nothing to kill) this exits bash/special mode
        // instead (TS parity, PromptInput.tsx:1904 lists `key.ctrl && char==='u'`).
        if (ev == Event::Character("\x15")) {
            const auto cursor = input_cursor_or_end(*state);
            if (cursor > 0) {
                state->input_text.erase(0, cursor);
                set_prompt_input_text(state, std::move(state->input_text), 0);
                return true;
            }
            if (exit_input_mode_if_at_start(state)) return true;
            return true;  // consume Ctrl+U even when it's a no-op
        }
        // Printable chars — accepts both ASCII and multi-byte UTF-8 (CJK).
        // FTXUI delivers composed IME characters as a single character event
        // containing the full UTF-8 byte sequence in ev.character().
        if (ev.is_character()) {
            const std::string& ch = ev.character();
            if (!ch.empty()) {
                unsigned char first = static_cast<unsigned char>(ch[0]);
                // Accept printable ASCII (0x20..0x7E) and UTF-8 multi-byte
                // start bytes (0xC0..0xFF).  Continuation bytes (0x80..0xBF)
                // should never appear as the first byte of a character event.
                const bool is_printable =
                    (first >= 0x20 && first < 0x7F) || first >= 0xC0;
                if (is_printable) {
                    namespace figs = cc::ui::design::figures;

                    // Footer "Pasting text…" feedback (TS usePasteHandler.ts):
                    // terminals deliver a paste as one multi-char batch, while
                    // a single CJK keystroke is at most 4 UTF-8 bytes. Stamp
                    // the burst time; the footer hides the hint 100ms later.
                    if (ch.size() > 4) {
                        state->pasting_since =
                            std::chrono::steady_clock::now();
                    }
                    // ── P0-1: TS-equivalent single-char mode interception ──
                    //
                    // TS PromptInput.tsx lines 869-901: when the user types a
                    // single '!' with cursor at offset 0 into an EMPTY input
                    // buffer, that's a MODE TRANSITION — NOT a character to
                    // store.  The '!' is swallowed, InputMode flips, and the
                    // prefix glyph changes without the text ever landing in
                    // the input state (so history persists cleanly).
                    //
                    // All other cases (multi-byte paste of "!cmd", cursor
                    // nonzero, typing '!' into existing text) → fall through
                    // to insert_prompt_text as normal; HandleSubmit will
                    // strip the prefix on submit for those.
                    const bool cursor_at_zero =
                        state->input_cursor == std::string::npos ||
                        state->input_cursor == 0;
                    if (state->input_text.empty() && cursor_at_zero &&
                        figs::is_mode_character(ch) &&
                        ch.size() == 1) {
                        // Swallow the char, flip mode.  Toggle Normal↔Bash.
                        state->input_mode =
                            (state->input_mode == InputMode::Bash)
                                ? InputMode::Normal
                                : InputMode::Bash;
                        state->is_prompt_input_active = true;
                        state->last_keystroke =
                            std::chrono::steady_clock::now();
                        // Also wipe any dismissed-suggestion memory since
                        // mode change implicitly changes the autocomplete
                        // provider context.
                        state->dismissed_autocomplete_for_input.clear();
                        return true;
                    }
                    // ── P0-1: TS-equivalent multi-char "!cmd" interception ──
                    //
                    // TS PromptInput.tsx lines 878-886: when "!cmd" lands as a
                    // single multi-char insertion at cursor-0 into an EMPTY
                    // input (IME composition, bracketed paste, or any path
                    // that bypasses the single-char-by-single-char typing
                    // flow), the '!' is stripped, inputMode flips to 'bash',
                    // and the clean "cmd" text is stored — NOT "!cmd".
                    //
                    // Without this, the user sees "! !cmd" visually (prefix
                    // glyph + text both carrying '!') because the single-char
                    // interceptor only fires for ch.size()==1.
                    if (state->input_text.empty() && cursor_at_zero &&
                        figs::get_mode_from_input(ch) ==
                            figs::PromptMode::kBash &&
                        ch.size() > 1) {
                        // Strip '!', enter bash mode, insert clean text.
                        state->input_mode = InputMode::Bash;
                        state->is_prompt_input_active = true;
                        state->last_keystroke =
                            std::chrono::steady_clock::now();
                        state->dismissed_autocomplete_for_input.clear();
                        std::string clean(figs::strip_mode_prefix(ch));
                        insert_prompt_text(state, clean);
                        return true;
                    }
                    insert_prompt_text(state, ch);
                    return true;
                }
            }
        }
        // Backspace — handle multi-byte UTF-8 correctly by erasing a full
        // codepoint, not just the last byte.  CJK characters are 3 bytes in
        // UTF-8, so a plain pop_back() would leave a partial/invalid sequence.
        if (ev == Event::Backspace) {
            if (backspace_prompt_text(state)) return true;
            // At cursor 0 (nothing to erase): exit bash/special mode (TS parity,
            // PromptInput.tsx:1904).  This is the fix for being unable to leave
            // bash mode after a bare '!' left the buffer empty.
            if (exit_input_mode_if_at_start(state)) return true;
        }
    }
    return false; });
}

/// Convenience: self-owned state (demos/tests only).
/// Production: use externally-held shared_ptr<ReplScreenState> overload.
[[nodiscard]] inline Component ReplScreen(ReplScreenCallbacks cbs) {
    return ReplScreen(std::make_shared<ReplScreenState>(), std::move(cbs));
}

} // namespace cc::ui::repl_screen
