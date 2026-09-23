// app_message_projection.cpp — impl unit for the Engine-state →
// ReplScreenState message projection free functions, kept OUT of
// app.cppm to shrink the interface BMI (clang source-location / memory
// budget; a fat interface is instantiated into every importer).
//
// Contains: project_message, project_messages, RenderMessage.
module;

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <ftxui/dom/elements.hpp>

module cc.ui.app.app;

import cc.types.types;
import cc.ui.screens.repl_screen;

namespace cc::ui {

// ============================================================
// Projection: Engine state -> ReplScreenState
// ============================================================

[[nodiscard]] repl::MessageDisplayEntry project_message(const Message& msg) {
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
[[nodiscard]] std::vector<repl::MessageDisplayEntry>
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

[[nodiscard]] Element RenderMessage(const Message& msg) {
    return repl::RenderMessages(project_messages(msg), -1, 40);
}

}  // namespace cc::ui
