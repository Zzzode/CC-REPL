/// @file app.cppm
/// @brief Application entry point — thin adapter that drives repl_screen from
///        the production QueryEngine.
module;

#include <string>
#include <vector>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <expected>
#include <functional>
#include <chrono>
#include <format>
#include <fstream>
#include <initializer_list>
#include <deque>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <variant>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <filesystem>

// Terminal control for VLNEXT disable (macOS line-discipline workaround —
// see DisableVlnext RAII in RunApp). Plain C headers, kept in the global
// module fragment so they don't leak into the module interface.
#if defined(__APPLE__) || defined(__linux__)
#include <termios.h>  // tcgetattr/tcsetattr/termios/VLNEXT
#include <unistd.h>   // STDIN_FILENO
#endif

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

export module cc.ui.app;

import cc.types.types;
import cc.query.query_engine;
import cc.types.command;
import cc.commands.command;
import cc.commands.registry;
import cc.utils.session_storage;
import cc.ui.components;
import cc.ui.components_extended;
import cc.ui.markdown;
import cc.vim.vim_mode;
import cc.hooks.tool_permissions;
import cc.tools.agent_runtime;
import cc.ui.repl_screen;
import cc.ui.autocomplete_sources;
// P0-2: 7-stage message pipeline utilities (dedup / tag filter / tool augment).
import cc.ui.messages.message_pipeline;
import cc.ui.agents.agent_cards;
import cc.utils.settings_manager;
import cc.utils.statusline_runner;
import cc.utils.model.model;
import cc.constants.constants;
import cc.hooks.lifecycle_hooks;
import cc.state.store;
import cc.state.app_state;

export namespace cc::ui {

// SL-11: defined in app_prompt_suggestion_wiring.cpp (impl unit) to keep the
// heavy cc.services.prompt_suggestion import out of this thin module (clang
// 2GB source-location budget).
void wire_prompt_suggestion_hook(cc::hooks::LifecycleHookRegistry& hooks,
                                 core::QueryEngine* engine,
                                 std::shared_ptr<cc::ui::repl_screen::ReplScreenState> state);

using namespace ftxui;
using namespace cc::ui::components;
using namespace cc::core;

namespace repl = cc::ui::repl_screen;
namespace agent_runtime = cc::tools::agent_runtime;
namespace agent_cards = cc::ui::agents::cards;
namespace acsrc = cc::ui::autocomplete_sources;

[[nodiscard]] inline std::optional<std::string> non_empty_env(const char* name) {
    if (const char* value = std::getenv(name); value && *value) {
        return std::string(value);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> first_non_empty_env(std::initializer_list<const char*> names) {
    for (const auto* name : names) {
        if (auto value = non_empty_env(name)) return value;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<bool> parse_bool_text(const std::string& value) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<int> parse_int_text(const std::string& value) {
    try {
        return std::stoi(value);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] inline std::string trim_ascii_copy(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] inline std::string summarize_agent_description(
    std::string_view description) {
    auto newline = description.find('\n');
    if (newline != std::string_view::npos) {
        description = description.substr(0, newline);
    }
    std::string out = trim_ascii_copy(description);
    constexpr std::size_t kMaxSummaryBytes = 160;
    if (out.size() > kMaxSummaryBytes) {
        out.resize(kMaxSummaryBytes);
        out += "...";
    }
    return out;
}

[[nodiscard]] inline std::string lowercase_ascii(std::string_view value) {
    std::string out(value);
    for (char& ch : out) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

[[nodiscard]] agent_cards::AgentCardData project_agent_definition_card(
    const agent_runtime::AgentDefinition& agent);

struct AutocompleteToken {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
};

[[nodiscard]] inline bool ascii_isspace(char ch) {
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

[[nodiscard]] inline AutocompleteToken token_around_cursor(
    std::string_view input,
    std::size_t cursor) {
    if (cursor == std::string::npos || cursor > input.size()) {
        cursor = input.size();
    }

    std::size_t start = cursor;
    while (start > 0 && !ascii_isspace(input[start - 1])) --start;

    std::size_t end = cursor;
    while (end < input.size() && !ascii_isspace(input[end])) ++end;

    // TS REF: src/hooks/useTypeahead.tsx:272-286 — quoted @ mention detection.
    // If the token starts with @", extend end to include the full quoted content
    // (up to closing quote or end of input). This allows @"path with spaces"
    // to be treated as a single token for autocomplete.
    std::string text_before = std::string(input.substr(start, cursor - start));
    std::size_t token_end = cursor;
    if (text_before.starts_with("@\"")) {
        // Find the closing quote after cursor, or end of input.
        std::size_t close = input.find('"', cursor);
        if (close != std::string_view::npos) {
            token_end = close + 1;  // include the closing quote
        } else {
            token_end = input.size();  // unterminated quote — extend to end
        }
    }

    return AutocompleteToken{
        .start = start,
        .end = token_end,
        .text = std::string(input.substr(start, cursor - start)),
    };
}

// AT-12: fuzzy_match_ascii / fuzzy_rank_ascii removed — all autocomplete
// ranking now delegates to cc::ui::prompt::fuzzy_rank_nucleo (frn::), which
// ports the nucleo/fzf-v2 scorer (boundary/camel/consecutive/gap/path bonuses)
// while preserving the exact {0..3} base range so the tier offsets (alias +1,
// skill +4, plugin +6) and the rank-ascending sort stay unchanged. See
// ui/prompt/fuzzy_rank_nucleo.cppm. lowercase_ascii() above is retained.

[[nodiscard]] inline std::vector<Message> compact_runtime_messages(void* state) {
    auto* engine = static_cast<core::QueryEngine*>(state);
    return engine ? engine->get_conversation() : std::vector<Message>{};
}

[[nodiscard]] inline VoidResult compact_runtime_apply(void* state) {
    auto* engine = static_cast<core::QueryEngine*>(state);
    if (!engine) {
        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            "No active query engine is available for compaction"));
    }
    auto compacted = engine->compact_conversation();
    if (!compacted) {
        return std::unexpected(Error::make(
            ErrorCode::InternalError,
            compacted.error().format()));
    }
    return VoidResult{};
}

// ============================================================
// AppStore bridge for CommandContext
// ============================================================

/// dispatch_fn implementation: casts void* back to AppStore*, int back to
/// ActionType, and dispatches.  Payload types are inferred from the action
/// type (the common bool / optional-string / enum cases); anything
/// unrecognised falls back to a payload-less dispatch.
inline void app_store_dispatch(void* store_ptr, int action_type_int,
                               const void* payload) {
    using cc::state::ActionType;
    auto* store = static_cast<cc::state::AppStore*>(store_ptr);
    if (!store) return;
    const auto at = static_cast<ActionType>(action_type_int);

    using cc::state::Action;
    switch (at) {
        // ── Bool-payload actions ──────────────────────────────────
        case ActionType::SetLoading:
        case ActionType::SetStreaming:
        case ActionType::SetVerbose:
        case ActionType::SetBriefOnly:
        case ActionType::SetFastMode:
        case ActionType::ToggleCompactMode:
        case ActionType::ToggleThinking:
            if (payload) {
                store->dispatch(Action{at, *static_cast<const bool*>(payload)});
            } else {
                store->dispatch(Action{at});
            }
            break;

        // ── String-payload actions ────────────────────────────────
        // Reducer expects std::string directly (not optional).
        case ActionType::SetError:
        case ActionType::GrantPermission:
        case ActionType::RevokePermission:
        case ActionType::SetWorkingDirectory:
        case ActionType::SetOutputStyle:
        case ActionType::AddNotification:
        case ActionType::DismissNotification:
            if (payload) {
                store->dispatch(Action{at,
                    *static_cast<const std::string*>(payload)});
            } else {
                store->dispatch(Action{at});
            }
            break;

        // ── Optional-string-payload actions ───────────────────────
        // Reducer expects std::optional<std::string>; caller passes a
        // std::string* which we wrap.
        case ActionType::SetStatusLineText:
        case ActionType::SetSpinnerTip:
        case ActionType::SetSlashCommand:
        case ActionType::SetMainLoopModel:
        case ActionType::SetAdvisorModel:
        case ActionType::SetEffortValue:
            if (payload) {
                store->dispatch(Action{at,
                    std::optional<std::string>{*static_cast<const std::string*>(payload)}});
            } else {
                store->dispatch(Action{at, std::optional<std::string>{}});
            }
            break;

        // ── ExpandedView enum payload ─────────────────────────────
        case ActionType::SetExpandedView:
            if (payload) {
                store->dispatch(Action{at,
                    *static_cast<const cc::state::ExpandedView*>(payload)});
            } else {
                store->dispatch(Action{at, cc::state::ExpandedView::None});
            }
            break;

        // ── PermissionMode enum payload ──────────────────────────
        case ActionType::SetPermissionMode:
            if (payload) {
                store->dispatch(Action{at,
                    *static_cast<const cc::state::PermissionMode*>(payload)});
            } else {
                store->dispatch(Action{at, cc::state::PermissionMode::Default});
            }
            break;

        // ── Payload-less actions ──────────────────────────────────
        case ActionType::ClearMessages:
        case ActionType::ResetSession:
        case ActionType::ClearError:
        case ActionType::SaveState:
        case ActionType::LoadState:
        case ActionType::ClearSavedState:
        default:
            store->dispatch(Action{at});
            break;
    }
}

/// get_state_fn implementation: returns a thread-local snapshot of AppState
/// so the returned pointer stays valid until the next call on this thread.
inline const void* app_store_get_state(void* store_ptr) {
    auto* store = static_cast<cc::state::AppStore*>(store_ptr);
    if (!store) return nullptr;
    thread_local static cc::state::AppState snapshot;
    snapshot = store->get_state();
    return &snapshot;
}

[[nodiscard]] inline CommandContext command_context_for_engine(
    core::QueryEngine* engine,
    cc::state::AppStore* app_store = nullptr,
    std::string cwd = {}) {
    if (cwd.empty() && engine) cwd = engine->working_directory();
    return CommandContext{
        .args = {},
        .raw_input = {},
        .cwd = std::move(cwd),
        .runtime_state = engine,
        .compact_message_provider = compact_runtime_messages,
        .compact_applier = compact_runtime_apply,
        .app_store = static_cast<void*>(app_store),
        .dispatch_fn = app_store ? app_store_dispatch : nullptr,
        .get_state_fn = app_store ? app_store_get_state : nullptr,
    };
}

// ============================================================
// Projection: Engine state -> ReplScreenState
// ============================================================

[[nodiscard]] inline repl::MessageDisplayEntry project_message(const Message& msg) {
    return std::visit([](const auto& m) -> repl::MessageDisplayEntry {
        using T = std::decay_t<decltype(m)>;
        repl::MessageDisplayEntry e;
        e.timestamp = (m.timestamp == std::chrono::system_clock::time_point{})
            ? std::chrono::system_clock::now() : m.timestamp;

        if constexpr (std::is_same_v<T, UserMessage>) {
            e.role = "user";
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    e.content_preview += tb->text;
                } else if (const auto* ib = std::get_if<ImageBlock>(&block)) {
                    // Single-entry fallback (user only attached images, no
                    // text).  When project_messages runs below with the
                    // full multi-row split, each ImageBlock becomes its own
                    // entry with is_image=true; this path just ensures the
                    // legacy / one-entry case doesn't render a completely
                    // blank card.
                    if (e.content_preview.empty() && m.content.size() == 1) {
                        e.is_image = true;
                        e.image_block = *ib;
                        e.image_display_id = 1;  // single image = #1
                    }
                    e.content_preview += e.content_preview.empty() ? "" : "\n";
                    e.content_preview += "[Image";
                    if (ib->width && ib->height) {
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), " %zux%zu",
                                      *ib->width, *ib->height);
                        e.content_preview += buf;
                    }
                    e.content_preview += "]";
                } else if (const auto* trb =
                               std::get_if<ToolResultBlock>(&block)) {
                    // Single-entry fallback for ToolResultBlock in user
                    // message.  project_messages handles this properly
                    // (emits role="tool" entry); this ensures the
                    // legacy one-entry path doesn't render blank.
                    //
                    // TS PARITY FIX (2026-07-05): is_tool_use must be
                    // false for tool results.  The repl_screen dispatcher
                    // checks is_tool_use FIRST (line 858), so a tool_result
                    // with is_tool_use=true gets routed to AssistantToolUse
                    // rendering instead of UserToolResult — the committed
                    // result card never appears as a separate block.
                    if (e.content_preview.empty() && m.content.size() == 1) {
                        e.role = "tool";
                        e.is_tool_use = false;
                        e.tool_status = trb->is_error ? "error" : "success";
                    }
                    e.content_preview += e.content_preview.empty() ? "" : "\n";
                    e.content_preview += tool_result_content_text(*trb);
                }
            }
        } else if constexpr (std::is_same_v<T, AssistantMessage>) {
            e.role = "assistant";
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    e.content_preview += tb->text;
                } else if (const auto* thk = std::get_if<ThinkingBlock>(&block)) {
                    e.is_thinking = true;
                    if (e.content_preview.empty())
                        e.content_preview = thk->thinking.substr(0, 200);
                } else if (const auto* tool = std::get_if<ToolUseBlock>(&block)) {
                    e.is_tool_use = true;
                    e.tool_name = tool->name;
                    e.tool_input_json = tool->input_json;
                }
            }
        } else if constexpr (std::is_same_v<T, SystemMessage>) {
            e.role = "system";
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block))
                    e.content_preview += tb->text;
            }
        } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
            e.role = "tool";
            // TS PARITY FIX (2026-07-05): is_tool_use=false for tool results.
            // ToolResultMessage is the committed result (role="tool"), NOT
            // the tool_use request (role="assistant").  Setting this true
            // routes the entry to AssistantToolUse rendering in repl_screen,
            // so the result never appears as its own transcript card.
            e.is_tool_use = false;
            e.tool_status = m.is_error ? "error" : "success";
            // TS parity: propagate the tool name from the result message so
            // the renderer can show "Bash" / "Edit" instead of generic "tool".
            // The old code left tool_name unset → BuildMessagesList fell back
            // to "tool", making every tool result look anonymous.
            if (!m.tool_name.empty()) {
                e.tool_name = m.tool_name;
            }
            // TS PARITY (2026-07-04): collect structured content items so the
            // faithful renderer can iterate them (text + image separately),
            // matching TS renderToolResultMessage's Array.isArray branch.
            std::vector<cc::core::ToolResultContentItem> content_items;
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    e.content_preview += tb->text;
                    content_items.push_back(cc::core::ToolResultContentItem{
                        .type = "text", .text = tb->text, .media_type = {}, .data = {}});
                } else if (const auto* ib = std::get_if<ImageBlock>(&block)) {
                    // Tool results may return images (e.g. analyze_image).
                    // Append a [Image] marker so the row isn't empty.
                    if (!e.content_preview.empty()) e.content_preview += '\n';
                    e.content_preview += "[Image]";
                    e.is_image = true;
                    e.image_block = *ib;
                    content_items.push_back(cc::core::ToolResultContentItem{
                        .type = "image", .text = {}, .media_type = ib->media_type, .data = ib->data});
                } else if (const auto* db = std::get_if<DocumentBlock>(&block)) {
                    if (!e.content_preview.empty()) e.content_preview += '\n';
                    e.content_preview += "[Document]";
                }
            }
            if (!content_items.empty()) {
                e.tool_result_content_items = std::move(content_items);
            }
        }
        // NOTE: content_preview IS the rendered message body, not a
        // "preview".  Do NOT truncate here — VirtualMessageList handles
        // display clipping.  Truncating caused "text vanishes after
        // streaming": the live stream showed the tail (last 500 chars)
        // but the committed view showed only the head (first 500 chars),
        // so for >500-char responses the content the user was reading
        // disappeared on completion.
        return e;
    }, msg);
}

// ============================================================
// project_messages — TS-faithful projection that splits a single
// AssistantMessage into MULTIPLE display rows when it mixes a ThinkingBlock
// with a TextBlock / ToolUseBlock.  TS renders these as separate sibling
// messages (a collapsed `∴ Thinking` row followed by the visible answer /
// tool-use row); the legacy single-entry projection collapsed them into one
// thinking row, which hid the visible answer once M4 routed thinking rows
// through RenderThinkingMessageFaithful (collapsed → raw text hidden).
//
// Non-assistant messages and assistant messages with a single block kind
// still project to exactly one entry (identical to project_message).
// ============================================================
[[nodiscard]] inline std::vector<repl::MessageDisplayEntry>
project_messages(const Message& msg) {
    std::vector<repl::MessageDisplayEntry> out;
    // Use the message's own timestamp (set when the engine appended it) so
    // chronological sorting against local-command rows is correct.  Fallback
    // to now() for messages with epoch-zero timestamps (shouldn't happen but
    // guards against uninitialized fields).
    const auto msg_ts = std::visit([](const auto& m) {
        return m.timestamp;
    }, msg);
    const auto now = (msg_ts == std::chrono::system_clock::time_point{})
        ? std::chrono::system_clock::now() : msg_ts;

    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, AssistantMessage>) {
            // TS PARITY FIX (2026-07-04): iterate content blocks in
            // ORIGINAL ORDER instead of grouping by kind.  TS renders
            // each block as a separate sibling component via
            // message.message.content.map((block, i) =>
            //   <AssistantMessageBlock key={i} block={block} .../>).
            //
            // The old code grouped all text into one accumulator and
            // all tools into a separate vector, then emitted [thinking,
            // merged_text, tool1, tool2, ...].  For [text1, tool_use,
            // text2] this produced [text1+text2, tool_use] — text was
            // merged and the order was wrong, so the model's final
            // response text (text2) appeared glued to the pre-tool
            // announcement (text1) instead of being a separate block.
            //
            // Consecutive text blocks ARE merged (TS also does this
            // implicitly since adjacent <Text> nodes render inline),
            // but any non-text block (thinking, tool_use) flushes the
            // text accumulator and emits its own row.
            std::string text_acc;

            auto flush_text = [&] {
                if (text_acc.empty()) return;
                repl::MessageDisplayEntry a;
                a.role = "assistant";
                a.content_preview = text_acc;
                a.timestamp = now;
                out.push_back(std::move(a));
                text_acc.clear();
            };

            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    // Consecutive text blocks merge (TS sibling <Text>
                    // nodes render inline without separation).
                    if (!text_acc.empty() && !tb->text.empty() &&
                        tb->text.front() != '\n') {
                        text_acc += '\n';
                    }
                    text_acc += tb->text;
                } else if (const auto* thk =
                               std::get_if<ThinkingBlock>(&block)) {
                    flush_text();
                    repl::MessageDisplayEntry t;
                    t.role = "assistant";
                    t.is_thinking = true;
                    t.content_preview = thk->thinking.substr(0, 200);
                    t.timestamp = now;
                    out.push_back(std::move(t));
                } else if (const auto* tool =
                               std::get_if<ToolUseBlock>(&block)) {
                    flush_text();
                    repl::MessageDisplayEntry tu;
                    tu.role = "assistant";
                    tu.is_tool_use = true;
                    tu.tool_name = tool->name;
                    tu.tool_input_json = tool->input_json;
                    // Committed tool-use blocks always have a matching result
                    // in the conversation (they only commit after execution).
                    // TS resolvedToolUseIDs.has(id) is always true here.
                    tu.tool_status = "success";
                    tu.timestamp = now;
                    out.push_back(std::move(tu));
                }
                // ToolResultBlock in assistant messages: shouldn't
                // happen (API invariant), but if it does we skip it
                // — tool results are projected from ToolResultMessage
                // or UserMessage with ToolResultBlock below.
            }
            flush_text();  // emit any trailing text
        } else if constexpr (std::is_same_v<T, UserMessage>) {
            // ── TS parity: each ImageBlock in the user message becomes its
            //    own transcript row (UserImageMessage), interleaved with text
            //    rows in the same order as m.content.  The TS renderer
            //    maps every user-pasted attachment to an <UserImageMessage/>
            //    sibling followed/followed by text rows.
            std::string text_acc;
            int img_id_counter = 0;  // TS parity: imageIds assigned per content block order
            auto flush_text = [&] {
                if (text_acc.empty()) return;
                repl::MessageDisplayEntry u;
                u.role = "user";
                u.content_preview = text_acc;
                // Do NOT truncate — user needs to see their full message
                u.timestamp = now;
                out.push_back(std::move(u));
                text_acc.clear();
            };
            for (const auto& block : m.content) {
                if (const auto* tb = std::get_if<TextBlock>(&block)) {
                    if (!text_acc.empty() && !tb->text.empty() &&
                        tb->text.front() != '\n') text_acc += '\n';
                    text_acc += tb->text;
                } else if (const auto* ib = std::get_if<ImageBlock>(&block)) {
                    flush_text();
                    repl::MessageDisplayEntry img;
                    img.role = "user";
                    img.is_image = true;
                    img.image_block = *ib;
                    img.image_display_id = ++img_id_counter;  // TS: imageIds from paste order
                    img.timestamp = now;
                    // Human-readable preview for list views / debugger tools.
                    // Mirrors TS format "[Image W×H]" shown in history previews.
                    std::string preview = "[Image";
                    if (ib->width && ib->height) {
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), " %zux%zu",
                                      *ib->width, *ib->height);
                        preview += buf;
                    }
                    if (ib->file_name) {
                        preview += " ";
                        preview += *ib->file_name;
                    }
                    preview += "]";
                    img.content_preview = std::move(preview);
                    img.estimated_height_lines = 2; // compact card: label + optional source (TS parity)
                    out.push_back(std::move(img));
                } else if (const auto* trb =
                               std::get_if<ToolResultBlock>(&block)) {
                    // TS PARITY FIX (2026-07-04): handle ToolResultBlock
                    // in user messages.  The API returns tool results as
                    // role=user messages with tool_result content blocks.
                    // The old code ignored these, so committed tool
                    // results vanished from the transcript after
                    // streaming ended (streaming_tools_ was cleared but
                    // the committed UserMessage with ToolResultBlock
                    // was never projected).
                    flush_text();
                    repl::MessageDisplayEntry tr;
                    tr.role = "tool";
                    // TS PARITY FIX (2026-07-05): is_tool_use=false.
                    // This is a committed tool result (role="tool"), not
                    // the assistant's tool_use request.  repl_screen checks
                    // is_tool_use before role, so true here would swallow
                    // the result into the tool_use card's Output section
                    // instead of rendering it as a separate card.
                    tr.is_tool_use = false;
                    tr.tool_status = trb->is_error ? "error" : "success";
                    // tool_name not available from ToolResultBlock (it
                    // only has tool_use_id); renderer falls back to
                    // "tool" via m.tool_name.value_or("tool").
                    //
                    // TS PARITY (2026-07-04): content may be string or
                    // array of content items.
                    if (std::holds_alternative<std::string>(trb->content)) {
                        tr.content_preview = std::get<std::string>(trb->content);
                    } else {
                        const auto& items = std::get<std::vector<cc::core::ToolResultContentItem>>(trb->content);
                        std::vector<cc::core::ToolResultContentItem> ci_copy;
                        for (const auto& item : items) {
                            if (item.type == "text") {
                                if (!tr.content_preview.empty()) tr.content_preview += '\n';
                                tr.content_preview += item.text;
                            } else if (item.type == "image") {
                                if (!tr.content_preview.empty()) tr.content_preview += '\n';
                                tr.content_preview += "[Image]";
                                tr.is_image = true;
                            }
                            ci_copy.push_back(item);
                        }
                        tr.tool_result_content_items = std::move(ci_copy);
                    }
                    tr.timestamp = now;
                    out.push_back(std::move(tr));
                }
                // ThinkingBlock/ToolUseBlock in a user message are API
                // invariants; ignore if present (project_message doesn't
                // render them either).
            }
            flush_text();
            if (out.empty()) {
                // Degenerate case: user message with zero renderable blocks.
                // Fall back to the one-entry project_message so we never
                // emit an empty list.
                out.push_back(project_message(msg));
            }
        } else {
            // Non-assistant → identical to the single-entry projection.
            out.push_back(project_message(msg));
        }
    }, msg);

    if (out.empty()) out.push_back(project_message(msg));
    return out;
}

// ============================================================
// Convenience: render a single core Message to an Element.
// Used by tests and callers that want a quick rendering of one message.
// ============================================================

[[nodiscard]] inline Element RenderMessage(const Message& msg) {
    return repl::RenderMessages(project_messages(msg), -1, 40);
}

// ============================================================
// App Adapter Component
// ============================================================

class AppAdapter : public ComponentBase {
private:
    core::QueryEngine* engine_;
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks_{nullptr};
    cc::commands::AppCommandRegistry* cmd_registry_;
    utils::SessionStorage* storage_;
    std::function<void()> on_exit_;

    std::shared_ptr<repl::ReplScreenState> screen_state_;
    Component repl_component_;
    std::vector<repl::MessageDisplayEntry> local_command_messages_;

    std::string current_session_id_;

    // Session start time for duration tracking (statusline cost.total_duration_ms)
    std::chrono::steady_clock::time_point session_start_time_;

    // Async query state
    std::jthread query_thread_;
    std::jthread spinner_thread_;
    // Local '!' bash command worker (TS processBashCommand.tsx). Runs the
    // command outside the LLM turn (shouldQuery:false), so it uses its own
    // thread rather than query_thread_ and never sets query_running_.
    std::jthread bash_thread_;
    std::atomic<bool> bash_running_{false};
    std::atomic<bool> query_running_{false};
    /// P2 gap api-error-retry: last user-submitted message text.  Used by
    /// the Retry button on SystemAPIError cards to re-send the same query.
    /// TS REF: SystemAPIErrorMessage.tsx onRetry → re-submits last prompt.
    std::string last_submitted_text_;
    // Cached autocomplete data (loaded once at startup to avoid repeated disk I/O
    // on every keystroke — TS memoizes these in useTypeahead).
    std::vector<acsrc::SkillSuggestionData> cached_skills_;
    std::vector<acsrc::PluginCommandSuggestionData> cached_plugin_commands_;
    std::atomic<std::uint64_t> ui_animation_tick_count_{0};
    std::mutex result_mutex_;
    std::optional<std::string> pending_error_;
    // Pasted clipboard images keyed by paste-id (TS pastedContents: Record).
    // Each ctrl+v image paste assigns a monotonically-increasing id and
    // inserts "[Image #N]" into input_text at the cursor.  Orphan cleanup
    // (see OnEvent + HandleSubmit) prunes entries whose placeholder is no
    // longer in the text.  TS REF: PromptInput.tsx L144 + L1151-1200.
    std::unordered_map<int, ImageBlock> pasted_contents_;
    // Pasted clipboard TEXT content keyed by paste-id.  When a >10K char
    // text paste is truncated, the middle (elided) content is stored here
    // so that HandleSubmit can expand [...Truncated text #N] refs back to
    // the full text before sending to the model.
    // TS REF: inputPaste.ts maybeTruncateInput — stores {id, type: 'text',
    //   content: placeholderContent} in pastedContents.
    std::unordered_map<int, std::string> pasted_text_contents_;
    int next_paste_id_ = 1;
    // Async paste: the placeholder "[Image #N]" is inserted into input_text
    // immediately on Ctrl+v (instant UI feedback), and the actual clipboard
    // read (osascript + PNG encode) runs on a background thread.  Results
    // are posted back via these queues and drained on the next OnEvent.
    std::mutex paste_mutex_;
    std::unordered_map<int, ImageBlock> pending_paste_results_;  // bg→UI
    std::unordered_set<int> pending_paste_failures_;             // bg→UI
    // Async text paste results: raw clipboard text keyed by paste-id.
    // Posted by SpawnPasteWorker when the clipboard has no image but does
    // have text.  ProcessCompletedPastes replaces the "[Image #N]"
    // placeholder with the text (truncating if >10K).
    std::unordered_map<int, std::string> pending_paste_text_results_;  // bg→UI
    // Track ids whose SpawnPasteWorker thread is still in flight (read hasn't
    // posted to pending_paste_results_/failures_ yet). HandleSubmit waits on
    // these so a fast Ctrl+V→Enter doesn't submit before the image data lands
    // (which would send a text-only message with [Image #N] refs but no PNG).
    std::unordered_set<int> in_flight_pastes_;
    // Local '!' bash command output posted back from bash_thread_ (bg→UI),
    // drained on the render thread in ConsumePendingResult().  Mirrors the
    // pending_paste_results_ handoff pattern so local_command_messages_ /
    // SyncState are only ever mutated on the render thread.
    std::mutex bash_result_mutex_;
    struct PendingBashResult {
        std::string output;   // combined stdout+stderr (already trimmed)
        bool is_error = false;
    };
    std::optional<PendingBashResult> pending_bash_result_;  // bg→UI
    std::string streaming_text_;
    /// TS REF: Markdown.tsx L186-235 — StreamingMarkdown stable-prefix cache
    /// for the streaming-text tail row.  Reset alongside streaming_text_ so
    /// each new model response starts with a fresh stable prefix.  Used by
    /// RenderAssistantTextMessageFaithful via MessagesListInput.streaming_md.
    ::cc::ui::StreamingMarkdown streaming_markdown_;
    struct StreamingToolPreview {
        std::string tool_name;
        std::string tool_use_id;  ///< M6: matches ToolExecution* events
        std::string input_json;
        std::string result_preview;  ///< M6: live streaming result preview
        // P0-2 Stage 5: Augmented tool result fields (lazy computed once on
        // ToolExecutionEnd — used by collapsed tool card + transcript.
        std::string compact_preview;   /// 200-char one-liner
        int         error_code      = 0;  /// 0=none, >0 shell exit/HTTP code
        bool        truncated       = false;/// result > 4 KiB threshold
        bool complete = false;       ///< ContentBlockStop: input_json fully streamed
        bool exec_done = false;      ///< ToolExecutionEnd: tool has finished executing
        bool is_error = false;
    };
    // TS REF: src/utils/messages.ts L2921-2925  StreamingThinking type
    //   { thinking, isStreaming, streamingEndedAt }
    // streaming_ended_at enables the 30s grace period after thinking stops
    // (TS REF: Messages.tsx L382-389  isStreamingThinkingVisible).
    struct StreamingThinkingPreview {
        std::string text;
        bool complete = false;
        std::optional<std::chrono::steady_clock::time_point> streaming_ended_at;
    };
    std::map<std::uint32_t, StreamingToolPreview> streaming_tools_;
    std::map<std::uint32_t, StreamingThinkingPreview> streaming_thinking_;

    // TS REF: Messages.tsx L382-389  isStreamingThinkingVisible useMemo.
    // Returns true when any streaming thinking block is still being streamed,
    // OR when a recently-completed thinking block is within the 30-second
    // grace period (TS: Date.now() - streamingEndedAt < 30000).
    // Drives G3 (hide all completed thinking when streaming visible) and
    // keeps the tail visible after ContentBlockStop fires.
    bool is_streaming_thinking_visible() const {
        auto now = std::chrono::steady_clock::now();
        for (const auto& [idx, stp] : streaming_thinking_) {
            if (!stp.complete) return true;
            if (stp.streaming_ended_at &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - *stp.streaming_ended_at).count() < 30)
                return true;
        }
        return false;
    }
    // P0-2 Stage 1: per-turn dedup tracker for ContentBlock index transitions.
    // One per App (one instantiation per repl lifetime; cleared on each turn start.
    cc::ui::messages::pipeline::DedupTracker event_dedup_;
    std::atomic<ScreenInteractive*> screen_{nullptr};

    // Permission confirmation
    std::mutex permission_mutex_;
    std::condition_variable permission_cv_;
    std::optional<bool> permission_response_;
    std::set<std::string> always_allowed_tools_;

    // MCP Elicitation (synchronous dialog response pattern,
    // same as tool permission — blocks worker thread on UI response).
    std::mutex elicitation_mutex_;
    std::condition_variable elicitation_cv_;
    std::optional<bool> elicitation_response_;

    // Ask-user prompt (same synchronous dialog response pattern).
    // Used by the ask_user_question tool to show a PromptDialog instead
    // of falling back to stdio.
    std::mutex ask_user_mutex_;
    std::condition_variable ask_user_cv_;
    std::optional<std::optional<std::string>> ask_user_response_;

    // Vim mode
    bool vim_enabled_ = false;
    cc::vim::VimStateMachine vim_sm_;

    // Settings manager — loads settings from disk and watches for changes.
    // Projections into screen_state_ are applied on init and on file change.
    std::unique_ptr<cc::utils::settings_manager::SettingsManager> settings_manager_;
    cc::utils::settings_manager::UnsubscribeFn settings_unsubscribe_;
    std::function<void()> skills_changed_unsubscribe_;  // SkillRegistry dynamic discovery

    // Cost threshold hook — listener ID + shown guard to avoid re-prompting.
    int cost_listener_id_ = -1;
    bool cost_threshold_shown_ = false;

    // Redux-like AppState store (commands dispatch actions / read state via
    // CommandContext.app_store bridge). Created in constructor.
    std::shared_ptr<cc::state::AppStore> app_store_;

    // Statusline runner — async execution of user-configurable shell command.
    // Triggered on mount, after messages change, and when settings change.
    // Faithful to TS StatusLine.tsx's debounced doUpdate() pattern.
    std::jthread statusline_thread_;
    std::atomic<bool> statusline_dirty_{false};
    std::atomic<bool> statusline_running_{false};
    std::mutex statusline_mutex_;
    std::condition_variable statusline_cv_;
    int statusline_debounce_ms_ = 300;  // TS: 300ms debounce
    // Memo cache: skip re-exec when command + JSON input are identical and
    // last run was < 30s ago.  Matches TS StatusLine.tsx memo dependency tuple
    // (lastAssistantMessageId, permissionMode, vimMode, mainLoopModel).
    std::string statusline_last_cmd_;
    std::string statusline_last_input_json_;
    std::chrono::steady_clock::time_point statusline_last_run_{};
    // P0-6 builtin statusline: git branch detection cache.  We only re-run
    // `git rev-parse --abbrev-ref HEAD` when the cwd changes (cd events are
    // rare).  This avoids spawning a subprocess on every render tick.
    std::string last_branch_cwd_;
    std::string cached_git_branch_;

    void StartUiAnimationTicker() {
        spinner_thread_ = std::jthread([this](std::stop_token st) {
            constexpr auto kTick = std::chrono::milliseconds(50);
            // TS is event-driven: Ink re-renders only on state changes, never
            // on a fixed timer.  This ticker exists solely to advance ANIMATIONS
            // (the welcome-intro asterisk hue sweep, the query spinner).  Once
            // the welcome intro has played (asterisk_sweep_ms × sweep_count =
            // 1500 × 2 = 3000ms ≈ 60 ticks) the screen is static, so we stop
            // forcing re-renders at idle — FTXUI otherwise re-emits the whole
            // frame + cursor-move sequences 20×/s, which flickers on terminals
            // that paint hidden-cursor movement.  Event-driven re-renders
            // (input, queries, statusline, cost hooks) still work normally.
            constexpr int kWelcomeIntroTicks = 80;  // 80 × 50ms = 4s (3s sweep + margin)
            int query_statusline_tick = 0;
            int welcome_render_ticks = 0;
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(kTick);
                if (st.stop_requested()) break;

                const bool query_active = query_running_.load();
                const bool welcome_active =
                    screen_state_ &&
                    screen_state_->messages.empty() &&
                    screen_state_->spinner_mode == repl::SpinnerMode::Hidden;
                if (!welcome_active) welcome_render_ticks = 0;

                // Re-render only while an animation is actually advancing:
                // an active query (spinner) or the welcome-intro sweep.  At
                // static idle we skip — no animation to drive.
                if (query_active) {
                    // spinner animation: keep ticking
                } else if (welcome_active &&
                           welcome_render_ticks < kWelcomeIntroTicks) {
                    ++welcome_render_ticks;
                } else {
                    query_statusline_tick = 0;
                    continue;
                }

                ui_animation_tick_count_.fetch_add(1, std::memory_order_relaxed);
                PostRenderEvent();

                if (query_active && ++query_statusline_tick % 20 == 0) {
                    this->TriggerStatuslineUpdate();
                }
            }
        });
    }

    void PostRenderEvent() {
        if (auto* screen = screen_.load(std::memory_order_acquire)) {
            screen->Post(Event::Custom);
        }
    }

    void AppendLocalMessagesToScreenState() {
        // Ensure local-command entries have a synthetic 24-char uuids so the
        // UnseenDivider anchor match still lands consistently.  Each local
        // command row is self-contained (not part of any source Message) so
        // each gets its own unique prefix.  A monotonically counter ensures
        // no collisions.
        static std::uint64_t s_local_seq = 0;
        for (auto it = local_command_messages_.begin();
             it != local_command_messages_.end(); ++it) {
            if (it->id.empty()) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "loc_%016llx",
                              (unsigned long long)s_local_seq++);
                it->id = std::string(buf, 24);
            }
        }
        screen_state_->messages.insert(
            screen_state_->messages.end(),
            local_command_messages_.begin(),
            local_command_messages_.end());
    }

    void AppendLocalCommandInputMessage(std::string command) {
        if (command.empty()) return;
        repl::MessageDisplayEntry entry;
        entry.role = "user";
        entry.content_preview = std::move(command);
        entry.is_local_command_input = true;
        entry.timestamp = std::chrono::system_clock::now();
        local_command_messages_.push_back(std::move(entry));
    }

    void AppendLocalCommandMessage(std::string message, bool is_error = false) {
        if (message.empty()) return;
        repl::MessageDisplayEntry entry;
        entry.role = "system";
        entry.content_preview = std::move(message);
        entry.is_local_command_output = true;
        entry.is_error = is_error;
        entry.timestamp = std::chrono::system_clock::now();
        local_command_messages_.push_back(std::move(entry));
        this->SyncState();
        PostRenderEvent();
    }

    void AppendCommandResult(const CommandResult& result) {
        AppendLocalCommandMessage(
            result.message,
            !result.ok || result.status == CommandStatus::Failed);
    }

    // TS REF: src/utils/processUserInput/processBashCommand.tsx
    //
    // Run a user-initiated `!` command LOCALLY (never an LLM turn).  TS does
    // BashTool.call({command, dangerouslyDisableSandbox:true}) with
    // shouldQuery:false, renders a <bash-input> user row plus a <bash-stdout>/
    // <bash-stderr> output row, and NEVER sends the command to the model.
    //
    // We mirror that: append the input row immediately (like TS's initial
    // setToolJSX(<BashModeProgress>)), then run `/bin/sh -c` on a worker thread
    // (combined stdout+stderr via popen_spawn, run in the session cwd) and post
    // the output back to the render thread via pending_bash_result_.  The
    // engine / query path is never touched, so no Bash *tool-use* card and no
    // assistant summary are produced — matching the TS transcript exactly.
    void RunLocalBashCommand(std::string command);

    void ClearActiveLocalJsxCommand() {
        screen_state_->active_local_jsx_command = false;
        screen_state_->active_local_jsx_command_name.clear();
        screen_state_->active_local_jsx_command_args.clear();
        screen_state_->active_local_jsx_content.clear();
        screen_state_->active_agents_selection_position = 0;
    }

    void DismissLocalJsxCommand(std::string result_message) {
        if (!screen_state_->active_local_jsx_command) return;
        std::string command = "/" + screen_state_->active_local_jsx_command_name;
        if (!screen_state_->active_local_jsx_command_args.empty()) {
            command += " " + screen_state_->active_local_jsx_command_args;
        }

        ClearActiveLocalJsxCommand();
        AppendLocalCommandInputMessage(std::move(command));
        AppendLocalCommandMessage(std::move(result_message), false);
    }

    [[nodiscard]] static std::string lowercase_ascii(std::string_view value) {
        std::string out(value);
        for (char& ch : out) {
            ch = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch)));
        }
        return out;
    }

    void RefreshAutocompleteSuggestions();


    [[nodiscard]] static bool is_built_in_agent(
        const agent_cards::AgentCardData& agent) {
        return agent.source == "built-in";
    }

    [[nodiscard]] static std::vector<std::size_t> selectable_agent_indices(
        const std::vector<agent_cards::AgentCardData>& agents) {
        std::vector<std::size_t> out;
        out.reserve(agents.size());
        for (std::size_t i = 0; i < agents.size(); ++i) {
            if (!is_built_in_agent(agents[i])) out.push_back(i);
        }
        return out;
    }

    [[nodiscard]] static std::string agent_model_label(
        const agent_cards::AgentCardData& agent) {
        if (agent.model_override && !agent.model_override->empty()) {
            return *agent.model_override;
        }
        return is_built_in_agent(agent) ? "inherit" : "";
    }

    [[nodiscard]] static std::string FormatAgentsMenuOutput(
        const std::vector<agent_cards::AgentCardData>& agents,
        int selected_position);

    void RefreshAgentsMenuOutput() {
        screen_state_->active_local_jsx_content = FormatAgentsMenuOutput(
            screen_state_->agent_cards,
            screen_state_->active_agents_selection_position);
    }

    void LoadAgentCardsForMenu();

    void OpenAgentsMenu() {
        LoadAgentCardsForMenu();
        screen_state_->mode = repl::ReplMode::AgentsView;
        screen_state_->agents_component.reset();
        this->TriggerStatuslineUpdate();
        PostRenderEvent();
    }

    bool HandleLocalJsxEvent(const Event& ev) {
        if (!screen_state_->active_local_jsx_command ||
            screen_state_->active_local_jsx_command_name != "agents") {
            return false;
        }

        const auto selectable = selectable_agent_indices(screen_state_->agent_cards);
        const int item_count = 1 + static_cast<int>(selectable.size());
        if (item_count <= 0) return false;

        auto refresh_selection = [&] {
            RefreshAgentsMenuOutput();
            PostRenderEvent();
        };

        if (ev == Event::ArrowDown || ev == Event::Character('j')) {
            screen_state_->active_agents_selection_position =
                (screen_state_->active_agents_selection_position + 1) % item_count;
            refresh_selection();
            return true;
        }
        if (ev == Event::ArrowUp || ev == Event::Character('k')) {
            screen_state_->active_agents_selection_position =
                (screen_state_->active_agents_selection_position - 1 + item_count) %
                item_count;
            refresh_selection();
            return true;
        }
        if (ev == Event::Return) {
            const int selected = std::clamp(
                screen_state_->active_agents_selection_position,
                0,
                item_count - 1);
            std::string command = "/agents create";
            if (selected > 0) {
                const auto agent_index =
                    selectable[static_cast<std::size_t>(selected - 1)];
                command = "/agents configure " +
                    screen_state_->agent_cards[agent_index].id;
            }
            ClearActiveLocalJsxCommand();
            screen_state_->scroll_offset = 0;
            screen_state_->scroll_pinned_to_bottom = true;
            HandleCommand(command);
            PostRenderEvent();
            return true;
        }
        return false;
    }

    [[nodiscard]] static int skill_source_order(std::string_view source) {
        if (source == "project") return 0;
        if (source == "user") return 1;
        if (source == "plugin") return 2;
        if (source == "mcp") return 3;
        return 4;
    }

    [[nodiscard]] static bool is_visible_skills_menu_source(
        std::string_view source) {
        return source == "project" ||
               source == "user" ||
               source == "plugin" ||
               source == "mcp";
    }

    [[nodiscard]] static bool utf8_continuation(unsigned char ch) {
        return (ch & 0xC0) == 0x80;
    }

    [[nodiscard]] static std::size_t utf16_code_unit_count(
        std::string_view value) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < value.size();) {
            const auto c0 = static_cast<unsigned char>(value[i]);
            std::uint32_t codepoint = c0;
            std::size_t length = 1;

            if (c0 < 0x80) {
                codepoint = c0;
            } else if ((c0 & 0xE0) == 0xC0 &&
                       i + 1 < value.size() &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 1]))) {
                codepoint =
                    (static_cast<std::uint32_t>(c0 & 0x1F) << 6) |
                    static_cast<std::uint32_t>(
                        static_cast<unsigned char>(value[i + 1]) & 0x3F);
                length = 2;
            } else if ((c0 & 0xF0) == 0xE0 &&
                       i + 2 < value.size() &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 1])) &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 2]))) {
                codepoint =
                    (static_cast<std::uint32_t>(c0 & 0x0F) << 12) |
                    (static_cast<std::uint32_t>(
                         static_cast<unsigned char>(value[i + 1]) & 0x3F) << 6) |
                    static_cast<std::uint32_t>(
                        static_cast<unsigned char>(value[i + 2]) & 0x3F);
                length = 3;
            } else if ((c0 & 0xF8) == 0xF0 &&
                       i + 3 < value.size() &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 1])) &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 2])) &&
                       utf8_continuation(static_cast<unsigned char>(value[i + 3]))) {
                codepoint =
                    (static_cast<std::uint32_t>(c0 & 0x07) << 18) |
                    (static_cast<std::uint32_t>(
                         static_cast<unsigned char>(value[i + 1]) & 0x3F) << 12) |
                    (static_cast<std::uint32_t>(
                         static_cast<unsigned char>(value[i + 2]) & 0x3F) << 6) |
                    static_cast<std::uint32_t>(
                        static_cast<unsigned char>(value[i + 3]) & 0x3F);
                length = 4;
            }

            count += codepoint > 0xFFFF ? 2 : 1;
            i += length;
        }
        return count;
    }

    [[nodiscard]] static std::size_t rough_js_token_count(
        std::string_view value) {
        return static_cast<std::size_t>(
            std::llround(static_cast<double>(utf16_code_unit_count(value)) / 4.0));
    }

    [[nodiscard]] static std::size_t skills_menu_token_estimate(
        const acsrc::SkillSuggestionData& skill) {
        std::string frontmatter = skill.name;
        if (!skill.description.empty()) {
            frontmatter.push_back(' ');
            frontmatter += skill.description;
        }
        return rough_js_token_count(frontmatter);
    }

    [[nodiscard]] static std::string collapse_home_path(std::string path) {
        if (const char* home = std::getenv("HOME"); home && *home) {
            const std::string home_path(home);
            if (path == home_path) return "~";
            if (path.starts_with(home_path + "/")) {
                return "~" + path.substr(home_path.size());
            }
        }
        return path;
    }

    [[nodiscard]] static std::string skill_source_group_title(
        const acsrc::SkillSuggestionData& skill) {
        if (skill.source == "project") {
            return skill.source_detail.empty()
                ? "Project skills"
                : "Project skills (" + collapse_home_path(skill.source_detail) + ")";
        }
        if (skill.source == "user") return "User skills (~/.claude/skills)";
        if (skill.source == "plugin") {
            return skill.source_detail.empty()
                ? "Plugin skills"
                : "Plugin skills (" + skill.source_detail + ")";
        }
        if (skill.source == "mcp") return "MCP skills";
        return "Other skills";
    }

    [[nodiscard]] static std::string FormatSkillsMenuOutput(
        std::vector<acsrc::SkillSuggestionData> skills) {
        std::erase_if(skills, [](const auto& skill) {
            return !is_visible_skills_menu_source(skill.source);
        });

        std::ranges::sort(skills, [](const auto& a, const auto& b) {
            const int ao = skill_source_order(a.source);
            const int bo = skill_source_order(b.source);
            if (ao != bo) return ao < bo;
            if (a.source_detail != b.source_detail) {
                return a.source_detail < b.source_detail;
            }
            return a.name < b.name;
        });

        std::string out;
        out += "Skills\n";
        out += std::format(
            "{} skill{}\n",
            skills.size(),
            skills.size() == 1 ? "" : "s");

        if (skills.empty()) {
            out += "\nNo skills found.\n";
            out += "Create skills under `.claude/skills` or `~/.claude/skills`.\n";
            return out;
        }

        std::string current_group;
        bool first_group = true;
        for (const auto& skill : skills) {
            const std::string group = skill_source_group_title(skill);
            if (group != current_group) {
                if (!first_group) out += "\n";
                first_group = false;
                current_group = group;
                out += "\n" + current_group + "\n";
            }

            out += skill.name;
            out += std::format(
                " · ~{} description tokens",
                skills_menu_token_estimate(skill));
            out += "\n";
        }
        return out;
    }

    void OpenSkillsMenu() {
        const auto& skills = cached_skills_;
        screen_state_->mode = repl::ReplMode::Normal;
        screen_state_->active_local_jsx_command = true;
        screen_state_->active_local_jsx_command_name = "skills";
        screen_state_->active_local_jsx_command_args.clear();
        screen_state_->active_local_jsx_content =
            FormatSkillsMenuOutput(std::move(skills));
        screen_state_->scroll_offset = 0;
        screen_state_->scroll_pinned_to_bottom = false;
        PostRenderEvent();
    }

public:
    ~AppAdapter() override;

    AppAdapter(core::QueryEngine* engine,
               cc::hooks::LifecycleHookRegistry* lifecycle_hooks,
               cc::commands::AppCommandRegistry* cmd_registry,
               utils::SessionStorage* storage,
               std::function<void()> on_exit);

    void HandleSubmit(const std::string& text,
                      repl::InputMode submit_mode = repl::InputMode::Normal);

    void HandleCommand(std::string_view cmd);

    void ProjectRuntimeMetadataToScreenState();

    /// Project settings from SettingsManager into screen_state_.
    /// Mirrors how the TS engine projects AppState.settings into the REPL
    /// screen's model/status-line fields.  Only the subset needed by the
    /// renderer is projected — the engine owns the full settings object.
    void ProjectSettingsToScreenState() {
        if (!settings_manager_) return;

        namespace sm = cc::utils::settings_manager;
        auto settings = settings_manager_->get_initial_settings();

        // --- default model ---
        auto model_it = settings.find("model");
        if (model_it != settings.end() &&
            std::holds_alternative<std::string>(model_it->second)) {
            screen_state_->settings_model = std::get<std::string>(model_it->second);
        } else {
            screen_state_->settings_model.clear();
        }

        // --- status line config (settings.statusLine) ---
        std::optional<std::string> status_line_type;
        std::string status_line_command;
        std::optional<bool> status_line_enabled;
        int status_line_padding = 0;

        auto sl_it = settings.find("statusLine");
        if (sl_it != settings.end() &&
            std::holds_alternative<std::map<std::string, std::string>>(sl_it->second)) {
            const auto& sl_map = std::get<std::map<std::string, std::string>>(sl_it->second);

            auto type_it = sl_map.find("type");
            if (type_it != sl_map.end()) {
                status_line_type = type_it->second;
            }

            // enabled flag
            auto enabled_it = sl_map.find("enabled");
            if (enabled_it != sl_map.end()) {
                status_line_enabled = parse_bool_text(enabled_it->second);
            }

            // shell command
            auto cmd_it = sl_map.find("command");
            if (cmd_it != sl_map.end()) {
                status_line_command = cmd_it->second;
            }

            // horizontal padding
            auto pad_it = sl_map.find("padding");
            if (pad_it != sl_map.end()) {
                if (auto parsed = parse_int_text(pad_it->second)) {
                    status_line_padding = *parsed;
                }
            }
        }

        if (auto command = first_non_empty_env({
                "CC_REPL_STATUS_LINE_COMMAND",
                "CLAUDE_CODE_STATUS_LINE_COMMAND"})) {
            status_line_command = *command;
            status_line_type = "command";
        }
        if (auto enabled = first_non_empty_env({
                "CC_REPL_STATUS_LINE_ENABLED",
                "CLAUDE_CODE_STATUS_LINE_ENABLED"})) {
            status_line_enabled = parse_bool_text(*enabled);
        }
        if (auto padding = first_non_empty_env({
                "CC_REPL_STATUS_LINE_PADDING",
                "CLAUDE_CODE_STATUS_LINE_PADDING"})) {
            if (auto parsed = parse_int_text(*padding)) {
                status_line_padding = *parsed;
            }
        }

        const bool type_allows_command = !status_line_type || *status_line_type == "command";
        const bool enabled = status_line_enabled.value_or(
            !status_line_command.empty() && type_allows_command);
        screen_state_->status_line_command = std::move(status_line_command);
        screen_state_->status_line_padding = status_line_padding;
        screen_state_->status_line_enabled =
            enabled && type_allows_command && !screen_state_->status_line_command.empty();
        if (!screen_state_->status_line_enabled) {
            screen_state_->status_line_text.clear();
        }
    }

    /// Trigger an async statusline update (debounced).
    /// Faithful to TS scheduleUpdate() — sets a dirty flag and wakes the
    /// worker thread; the actual command runs after the debounce period.
    void TriggerStatuslineUpdate() {
        if (screen_state_->status_line_command.empty()) return;
        statusline_dirty_.store(true);
        statusline_cv_.notify_one();
    }

    /// Build the StatusLineCommandInput payload from current engine state.
    /// Faithful to TS buildStatusLineCommandInput() — populates model info,
    /// workspace, cost, context window, version, etc.
    [[nodiscard]] cc::utils::statusline::StatusLineCommandInput BuildStatuslineInput() {
        namespace sl = cc::utils::statusline;

        sl::StatusLineCommandInput input;

        // Version
        input.version = std::string(cc::core::constants::kVersion);

        // Model info
        const auto& model = engine_->model_params().model;
        input.model.id = model;
        input.model.display_name = cc::utils::get_model_display_name(model);

        // Workspace
        const auto cwd = engine_->working_directory();
        input.workspace.current_dir = cwd;
        input.workspace.project_dir = cwd;
        // added_dirs: not easily accessible at the app level; populated by
        // tool permission context when additional directories are configured.
        // Left empty (empty vector) to match TS semantics for default config.
        input.workspace.added_dirs = {};

        // Output style from settings
        if (settings_manager_) {
            auto settings = settings_manager_->get_initial_settings();
            auto os_it = settings.find("outputStyle");
            if (os_it != settings.end() &&
                std::holds_alternative<std::string>(os_it->second)) {
                input.output_style_name = std::get<std::string>(os_it->second);
            } else {
                input.output_style_name = "full";  // default
            }
        } else {
            input.output_style_name = "full";  // default
        }

        // Cost / usage
        const auto& usage = engine_->get_usage();
        const auto& budget = engine_->budget_tracker();
        input.cost.total_cost_usd = budget.current_spend_usd;
        // Session duration: time since AppAdapter construction
        auto session_dur = std::chrono::steady_clock::now() - session_start_time_;
        input.cost.total_duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(session_dur).count();
        // total_api_duration_ms: not separately tracked at the app layer
        // (would require summing individual API call durations).
        input.cost.total_api_duration_ms = 0;
        // total_lines_added / total_lines_removed: not tracked at this level
        // (would need to aggregate from FileEditTool results).
        input.cost.total_lines_added = 0;
        input.cost.total_lines_removed = 0;

        // Context window
        input.context_window.total_input_tokens = usage.input_tokens;
        input.context_window.total_output_tokens = usage.output_tokens;
        input.context_window.context_window_size =
            static_cast<std::int64_t>(engine_->max_context_tokens());
        const bool has_usage = usage.input_tokens > 0 || usage.output_tokens > 0 ||
            usage.cache_creation_tokens > 0 || usage.cache_read_tokens > 0;
        if (has_usage) {
            input.context_window.current_usage = sl::StatusLineCurrentUsageInfo{
                .input_tokens = usage.input_tokens,
                .output_tokens = usage.output_tokens,
                .cache_creation_input_tokens = usage.cache_creation_tokens,
                .cache_read_input_tokens = usage.cache_read_tokens,
            };
            const auto input_context_tokens =
                static_cast<std::int64_t>(usage.input_tokens) +
                static_cast<std::int64_t>(usage.cache_creation_tokens) +
                static_cast<std::int64_t>(usage.cache_read_tokens);
            if (input.context_window.context_window_size > 0) {
                auto pct = static_cast<int>(std::llround(
                    static_cast<double>(input_context_tokens) /
                    static_cast<double>(input.context_window.context_window_size) *
                    100.0));
                pct = std::clamp(pct, 0, 100);
                input.context_window.used_percentage = static_cast<double>(pct);
                input.context_window.remaining_percentage = static_cast<double>(100 - pct);
            }
        }

        // 200k threshold flag
        input.exceeds_200k_tokens =
            (usage.input_tokens + usage.output_tokens) > 200'000;

        // Session name: use session id as identifier (TS uses getCurrentSessionTitle
        // which derives from first user message; session id is always available)
        input.session_name = current_session_id_;
        // session_id: TS StatusLineCommandInput.session_id — used by user scripts
        // for the #hashtag display (e.g. #a1b2c3). Same value as session_name.
        input.session_id = current_session_id_;

        // Vim mode (optional — only populated if vim enabled)
        if (vim_enabled_) {
            std::string mode_str;
            switch (vim_sm_.get_mode()) {
                case cc::vim::VimMode::Normal:     mode_str = "NORMAL"; break;
                case cc::vim::VimMode::Insert:     mode_str = "INSERT"; break;
                case cc::vim::VimMode::Visual:     mode_str = "VISUAL"; break;
                case cc::vim::VimMode::VisualLine: mode_str = "VISUAL LINE"; break;
                case cc::vim::VimMode::Command:    mode_str = "COMMAND"; break;
                case cc::vim::VimMode::Replace:    mode_str = "REPLACE"; break;
                default:                           mode_str = "INSERT"; break;
            }
            input.vim = sl::StatusLineVimInfo{.mode = std::move(mode_str)};
        }

        // rate_limits, agent, remote, worktree: not available at the app level
        // (would require additional service wiring). Left unpopulated (nullopt)
        // which matches TS semantics where undefined fields are omitted from JSON.

        return input;
    }

    // TS REF: src/components/Messages.tsx L519-520 — the render `useMemo`
    // applies a chain of collapse passes to the message list before projecting
    // rows:
    //   collapseBackgroundBashNotifications(collapseHookSummaries(
    //     collapseTeammateShutdowns(collapseReadSearchGroups(grouped, tools))))
    //
    // We run the same chain here, on the raw conversation, before the
    // per-message projection loop in SyncState()/Render().  Only the passes
    // that have a faithful CPP port are wired so far:
    //   * collapseBackgroundBashNotifications — DONE (this call).
    //   * collapseHookSummaries / collapseTeammateShutdowns / collapseReadSearch
    //     — pending (need richer SystemMessage / AttachmentMessage types).
    // As each pass lands it slots in here, preserving the TS ordering.
    //
    // `fullscreen=true`: the CPP transcript is always the fullscreen-equivalent
    // view (TS gates collapse on isFullscreenEnvEnabled()).  `verbose=false`:
    // there is no ctrl+O verbose transcript toggle at this layer yet, so we use
    // the default collapsed presentation (TS shows each item only in verbose).
    [[nodiscard]] std::vector<Message> ApplyMessageCollapsePipeline(
        std::vector<Message> messages) const;

    void SyncState();

    void ConsumePendingResult();

    Element Render() override;

    bool OnEvent(Event event) override;

    Component ActiveChild() override;

    void set_screen(ScreenInteractive* screen) {
        screen_.store(screen, std::memory_order_release);
    }

    // ── Async clipboard paste worker ──────────────────────────────────────
    // Spawns a detached thread that reads the clipboard image.  On success
    // the ImageBlock is posted to pending_paste_results_; on failure the id
    // is posted to pending_paste_failures_.  ProcessCompletedPastes() drains
    // both queues on the render/event thread.
    //
    // Why async?  std::system() + osascript fork + PNG-to-file + base64
    // encode takes 100-500ms on macOS.  Doing that synchronously in OnEvent
    // blocks the FTXUI render loop, causing visible UI freeze and (worse)
    // terminal raw-mode state corruption that can take seconds to recover
    // from.  The placeholder "[Image #N]" is inserted synchronously so the
    // user gets instant feedback; the image data fills in shortly after.
    void SpawnPasteWorker(int id);

    /// Drain background paste results onto pasted_contents_ (render thread).
    /// Called at the top of every OnEvent so results are picked up as soon as
    /// possible without blocking.  Failed pastes have their "[Image #N]"
    /// placeholder removed from input_text.  Text pastes replace "[Image #N]"
    /// with the actual text (truncating if >10K chars).
    void ProcessCompletedPastes();

    /// Block (main thread, brief) until every [Image #N] referenced in `text`
    /// that still has an in-flight paste worker has either landed in
    /// pasted_contents_ / pending_paste_results_ / pending_paste_failures_.
    /// This closes the Ctrl+V→Enter race where a fast submit would snapshot
    /// pasted_contents_ before the PNG data arrived.
    ///
    /// Bounded wait (default ~3s) so a stuck/leaked worker never wedges the UI.
    /// Drains completed results on each tick so pasted_contents_ is fresh when
    /// HandleSubmit reads it immediately after this returns.
    void WaitForInFlightPastes(const std::string& text);

    [[nodiscard]] std::function<bool(std::string_view, std::string_view)> get_permission_callback();

    [[nodiscard]] bool is_query_running_for_testing() const noexcept {
        return query_running_.load();
    }

    // Drive a prompt submission through the full HandleSubmit path (slash /
    // bash / LLM routing) exactly as the Enter key would.
    void submit_for_testing(const std::string& text) {
        this->HandleSubmit(text);
    }

    // True while a local '!' bash command worker is still running.
    [[nodiscard]] bool is_local_bash_running_for_testing() const noexcept {
        return bash_running_.load();
    }

    // Block until the local '!' bash worker finishes, then drain its output
    // into the transcript (mirrors what the render loop does each frame).
    void wait_for_local_bash_for_testing() {
        if (bash_thread_.joinable()) bash_thread_.join();
        this->ConsumePendingResult();
    }

    [[nodiscard]] bool is_loading_for_testing() const noexcept {
        return screen_state_->spinner_mode != repl::SpinnerMode::Hidden;
    }

    [[nodiscard]] std::uint64_t ui_animation_tick_count_for_testing() const noexcept {
        return ui_animation_tick_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string status_message_for_testing() const {
        return screen_state_->spinner_tip.value_or(std::string{});
    }

    [[nodiscard]] bool status_line_enabled_for_testing() const noexcept {
        return screen_state_->status_line_enabled;
    }

    [[nodiscard]] std::string status_line_command_for_testing() const {
        return screen_state_->status_line_command;
    }

    [[nodiscard]] int status_line_padding_for_testing() const noexcept {
        return screen_state_->status_line_padding;
    }

    [[nodiscard]] std::string status_bar_model_for_testing() const {
        return screen_state_->status_bar.model_name;
    }

    [[nodiscard]] std::size_t autocomplete_suggestion_count_for_testing() const noexcept {
        return screen_state_->autocomplete_suggestions.size();
    }

    [[nodiscard]] std::vector<std::string> autocomplete_suggestions_for_testing() const {
        std::vector<std::string> out;
        out.reserve(screen_state_->autocomplete_suggestions.size());
        for (const auto& suggestion : screen_state_->autocomplete_suggestions) {
            out.push_back(suggestion.display_text);
        }
        return out;
    }

    [[nodiscard]] int autocomplete_index_for_testing() const noexcept {
        return screen_state_->autocomplete_index;
    }

    // Debug/testing: snapshot screen_state_->messages as "label:preview" rows
    // to verify transcript ordering (local-command vs user vs assistant).
    [[nodiscard]] std::vector<std::string> messages_for_testing() const {
        std::vector<std::string> out;
        out.reserve(screen_state_->messages.size());
        for (const auto& m : screen_state_->messages) {
            std::string label = m.role;
            if (m.is_local_command_input) label = "lc-input";
            else if (m.is_local_command_output) label = "lc-output";
            else if (m.is_thinking) label = "thinking";
            std::string pv = m.content_preview.substr(
                0, std::min<std::size_t>(30, m.content_preview.size()));
            out.push_back(label + ":" + pv);
        }
        return out;
    }

    [[nodiscard]] std::string input_text_for_testing() const {
        return screen_state_->input_text;
    }

    /// Number of entries in pasted_contents_ (for testing orphan cleanup).
    [[nodiscard]] std::size_t pasted_contents_size_for_testing() const noexcept {
        return pasted_contents_.size();
    }

    /// Check if a specific paste-id is still in pasted_contents_ (for testing
    /// orphan cleanup after placeholder deletion).
    [[nodiscard]] bool has_pasted_content_for_testing(int id) const noexcept {
        return pasted_contents_.contains(id);
    }

    /// Inject a pasted image directly (bypasses clipboard read — for testing
    /// HandleSubmit's referenced-ids filter and empty-text+images guard).
    void inject_pasted_image_for_testing(int id, ImageBlock ib) {
        pasted_contents_[id] = std::move(ib);
    }

    /// When true, SpawnPasteWorker injects a tiny fake PNG synchronously into
    /// pending_paste_results_ instead of spawning a detached thread that reads
    /// the real clipboard. Lets tests exercise the Ctrl+V → placeholder →
    /// submit path without the lifetime hazard of a detached thread outliving
    /// the test's AppAdapter.
    bool no_real_paste_worker_for_testing_ = false;
    void set_no_real_paste_worker_for_testing(bool v) {
        no_real_paste_worker_for_testing_ = v;
    }

    /// Set input_text directly (for testing orphan cleanup and submit guards
    /// without going through the text input component).
    void set_input_text_for_testing(std::string text) {
        screen_state_->input_text = std::move(text);
        screen_state_->input_cursor = screen_state_->input_text.size();
    }

    /// Expose HandleSubmit for direct test invocation (the real submit path
    /// goes through the text input component's on_submit callback).
    void handle_submit_for_testing(std::string text) {
        this->HandleSubmit(text);
    }

    /// Run the orphan-cleanup logic (TS PromptInput.tsx L1185-1200 useEffect)
    /// against the current screen_state_->input_text.  For testing only.
    void trigger_orphan_cleanup_for_testing();

    [[nodiscard]] bool is_agents_view_for_testing() const noexcept {
        return screen_state_->mode == repl::ReplMode::AgentsView;
    }

    [[nodiscard]] bool is_local_jsx_command_for_testing(
        std::string_view command_name) const noexcept {
        return screen_state_->active_local_jsx_command &&
               screen_state_->active_local_jsx_command_name == command_name;
    }

    [[nodiscard]] int active_agents_selection_position_for_testing() const noexcept {
        return screen_state_->active_agents_selection_position;
    }

    [[nodiscard]] std::size_t agent_card_count_for_testing() const noexcept {
        return screen_state_->agent_cards.size();
    }

    [[nodiscard]] bool has_pending_dialog_for_testing() const noexcept {
        return screen_state_->dialog_queue.has_overlay() ||
               screen_state_->dialog_queue.has_any_bottom() ||
               screen_state_->dialog_queue.has_modal() ||
               screen_state_->dialog_queue.has_standalone();
    }
};

// ============================================================
// Main Application Runner
// ============================================================

[[nodiscard]] int RunApp(
    core::QueryEngine& engine,
    cc::commands::AppCommandRegistry& cmd_registry,
    utils::SessionStorage& storage,
    cc::hooks::ToolPermissionHook* permission_hook = nullptr,
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks = nullptr
) {
    // Use the alternate-screen fullscreen like TS (AlternateScreen) - the REPL owns the terminal.
    auto screen = ScreenInteractive::Fullscreen();

    // ── macOS/BSD line-discipline workaround: disable VLNEXT ─────────────
    // VLNEXT (the "literal-next" char, Ctrl+V by default) is processed by the
    // terminal line discipline EVEN in non-canonical mode (ICANON off) on
    // macOS/BSD. FTXUI puts the terminal in non-canonical mode (ICANON|ECHO
    // off) but does NOT clear c_cc[VLNEXT], so every Ctrl+V the user presses
    // gets consumed as an lnext escape: a pair of \x16 bytes collapses into a
    // single literal \x16. Net effect: pressing Ctrl+V 8× registers only 4×
    // (floor(N/2)) — half the image-paste keystrokes are silently dropped
    // before FTXUI's event loop ever sees them.
    //
    // Fix: clear VLNEXT ourselves before entering the loop. We do this BEFORE
    // screen.Loop() because FTXUI's Install() (called inside Loop) does
    // tcgetattr()+save-then-restore: it will read our VLNEXT=0, preserve it
    // for the session, and restore that same value on exit. To still give the
    // parent shell back its original Ctrl+V lnext on exit, we snapshot the
    // true original termios here and re-apply it after Loop() returns.
    //
    // Verified: sending N×\x16 through a pty with VLNEXT=0 delivers all N
    // bytes; with VLNEXT at its default, only floor(N/2) arrive. This is
    // independent of the osascript/clipboard path (setsid/closefrom there
    // remain good hygiene but were NOT the cause of keystroke loss).
#if defined(__APPLE__) || defined(__linux__)
    struct termios orig_termios;
    const bool have_orig = (tcgetattr(STDIN_FILENO, &orig_termios) == 0);
    if (have_orig) {
        struct termios t = orig_termios;
        t.c_cc[VLNEXT] = 0;  // 0 == _POSIX_VDISABLE: disable literal-next
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
#endif

    bool should_exit = false;

    auto app = Make<AppAdapter>(
        &engine,
        lifecycle_hooks,
        &cmd_registry,
        &storage,
        [&screen, &should_exit]() {
            should_exit = true;
            screen.Exit();
        }
    );

    app->set_screen(&screen);

    if (permission_hook && !permission_hook->is_auto_approve_mode()) {
        auto ui_callback = app->get_permission_callback();
        permission_hook->set_ask_user_fn(
            [ui_callback](const cc::hooks::PermissionContext& ctx) -> cc::hooks::PermissionDecision {
                bool allowed = ui_callback(ctx.tool_name, ctx.args);
                return allowed ? cc::hooks::PermissionDecision::allow
                               : cc::hooks::PermissionDecision::deny;
            }
        );
    }

    app->SyncState();

    screen.Loop(app);

    // Restore the parent shell's original termios (FTXUI's on_exit restored
    // what IT read, which carries VLNEXT=0; re-apply the true original so
    // Ctrl+V lnext works again in the user's shell after cc-repl exits).
#if defined(__APPLE__) || defined(__linux__)
    if (have_orig) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
#endif

    return should_exit ? 0 : 1;
}

} // namespace cc::ui

extern "C" int cc_ui_run_app_bridge(
    cc::core::QueryEngine* engine,
    cc::hooks::LifecycleHookRegistry* lifecycle_hooks,
    cc::commands::AppCommandRegistry* cmd_registry,
    cc::utils::SessionStorage* storage,
    cc::hooks::ToolPermissionHook* permission_hook
) {
    return cc::ui::RunApp(*engine, *cmd_registry, *storage, permission_hook, lifecycle_hooks);
}
