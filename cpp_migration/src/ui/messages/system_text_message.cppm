/// @file system_text_message.cppm
/// @brief FTXUI component for system text messages (SystemTextMessage.tsx)
///
/// System messages are small, muted notices about session lifecycle:
///   - turn_duration, memory_saved, bridge_status, thinking_summary, etc.
/// Visual:
///   ┌──────────────────────────────────────┐
///   │ ⚙ System  [turn_duration]  ⏱ HH:MM   │
///   │  ─────────────────────────────        │
///   │  ▸ Turn took 4.2s · 12 tools          │
///   │  ▾ [expanded details if open]         │
///   └──────────────────────────────────────┘
/// Default collapsed; toggle expands to show full detail text.
// ────────────────────────────────────────────────────────────────────────
module;

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>

export module cc.ui.messages.system_text_message;

import cc.ui.messages.message_components;
import cc.ui.messages.message_timestamp;

export namespace cc::ui::messages {

using namespace ftxui;

// ─── Input data ────────────────────────────────────────────────────────

enum class SystemMessageSubtype {
    Plain,
    TurnDuration,
    MemorySaved,
    BridgeStatus,
    ThinkingSummary,
    HookSummary,
    ModelSwitch,
    AwaySummary,
    BackgroundTask,
    Other,
};

struct SystemTextMessageData {
    SystemMessageSubtype subtype = SystemMessageSubtype::Plain;
    std::string summary;            ///< Always visible, single line
    std::string detail;             ///< Shown when expanded (multi-line ok)
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now();
    bool start_expanded = false;
    bool is_transcript_mode = false;
};

// ─── Subtype helpers ───────────────────────────────────────────────────

inline auto SubtypeIcon(SystemMessageSubtype t) -> std::string_view {
    switch (t) {
        case SystemMessageSubtype::TurnDuration:    return "⏱";
        case SystemMessageSubtype::MemorySaved:     return "🧠";
        case SystemMessageSubtype::BridgeStatus:    return "🔌";
        case SystemMessageSubtype::ThinkingSummary: return "💭";
        case SystemMessageSubtype::HookSummary:     return "🪝";
        case SystemMessageSubtype::ModelSwitch:     return "🔀";
        case SystemMessageSubtype::AwaySummary:     return "☕";
        case SystemMessageSubtype::BackgroundTask:  return "📋";
        case SystemMessageSubtype::Plain:
        case SystemMessageSubtype::Other:           return "⚙";
    }
    return "⚙";
}

inline auto SubtypeLabel(SystemMessageSubtype t) -> std::string_view {
    switch (t) {
        case SystemMessageSubtype::TurnDuration:    return "turn_duration";
        case SystemMessageSubtype::MemorySaved:     return "memory_saved";
        case SystemMessageSubtype::BridgeStatus:    return "bridge_status";
        case SystemMessageSubtype::ThinkingSummary: return "thinking_summary";
        case SystemMessageSubtype::HookSummary:     return "hook_summary";
        case SystemMessageSubtype::ModelSwitch:     return "model_switch";
        case SystemMessageSubtype::AwaySummary:     return "away_summary";
        case SystemMessageSubtype::BackgroundTask:  return "background_task";
        case SystemMessageSubtype::Plain:           return "system";
        case SystemMessageSubtype::Other:           return "system";
    }
    return "system";
}

// ─── Component ─────────────────────────────────────────────────────────

class SystemTextMessageComponent : public ComponentBase {
  public:
    using OnToggleFn = std::function<void(bool expanded)>;

    explicit SystemTextMessageComponent(SystemTextMessageData data,
                                        OnToggleFn on_toggle = nullptr)
        : data_(std::move(data)),
          on_toggle_(std::move(on_toggle)),
          expanded_(data_.start_expanded) {}

    Element Render() override {
        // Header: ⚙ System  [subtype]  ⏱ HH:MM  ▾ expand/hide button
        const auto icon = std::string(SubtypeIcon(data_.subtype));
        const auto label = std::string(SubtypeLabel(data_.subtype));
        const bool has_detail = !data_.detail.empty();

        auto header = hbox({
            text(icon + " ") | color(Color::Yellow),
            text("System") | dim | color(Color::Yellow),
            text("  [" + label + "]") | dim | color(Color::GrayDark),
            filler(),
            text(render_timestamp(data_.timestamp)) | dim | color(Color::GrayDark),
            text(" "),
            has_detail
                ? text(expanded_ ? "▾ hide" : "▸ expand")
                    | bold | color(Color::Cyan)
                : text(""),
        });

        // Summary (always visible)
        auto summary = text(data_.summary) | dim | color(Color::GrayLight);

        // Detail (only when expanded + present)
        Elements detail_box;
        if (expanded_ && has_detail) {
            for (auto& line : SplitLines(data_.detail)) {
                detail_box.push_back(text(std::move(line))
                                         | dim | color(Color::GrayLight));
            }
        }

        // Transcript: fully minimal
        if (data_.is_transcript_mode) {
            return vbox({
                text("· " + data_.summary) | dim | color(Color::GrayDark),
            });
        }

        Elements rows;
        rows.push_back(std::move(header));
        rows.push_back(separatorEmpty());
        rows.push_back(std::move(summary));
        if (!detail_box.empty()) {
            rows.push_back(separatorEmpty());
            rows.push_back(vbox(std::move(detail_box)) | indent(2));
        }

        // Grey "pill" background to differentiate from user/assistant
        return vbox(std::move(rows))
             | bgcolor(Color::GrayDark)
             | color(Color::GrayLight)
             | borderEmpty
             | padding(1);
    }

    bool OnEvent(Event event) override {
        if (event == Event::Return || event == Event::Character(' ') ||
            event == Event::Character('e') || event == Event::Character('E')) {
            expanded_ = !expanded_;
            if (on_toggle_) on_toggle_(expanded_);
            return true;
        }
        if (event.is_mouse() && event.mouse().button == Mouse::Left &&
            event.mouse().motion == Mouse::Released) {
            expanded_ = !expanded_;
            if (on_toggle_) on_toggle_(expanded_);
            return true;
        }
        return ComponentBase::OnEvent(event);
    }

  private:
    static auto SplitLines(std::string_view text) -> std::vector<std::string> {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start < text.size()) {
            auto nl = text.find('\n', start);
            if (nl == std::string_view::npos) {
                lines.emplace_back(text.substr(start));
                break;
            }
            lines.emplace_back(text.substr(start, nl - start));
            start = nl + 1;
        }
        if (lines.empty()) lines.emplace_back("");
        return lines;
    }

    SystemTextMessageData data_;
    OnToggleFn on_toggle_;
    bool expanded_;
};

// ─── Factories ─────────────────────────────────────────────────────────

[[nodiscard]] inline Component MakeSystemTextMessage(
    SystemTextMessageData data,
    SystemTextMessageComponent::OnToggleFn on_toggle = nullptr)
{
    return Make<SystemTextMessageComponent>(std::move(data), std::move(on_toggle));
}

[[nodiscard]] inline Element RenderSystemTextMessageBubble(const SystemTextMessageData& data) {
    return vbox({
        hbox({
            text(std::string(SubtypeIcon(data.subtype)) + " ") | color(Color::Yellow),
            text(std::string(SubtypeLabel(data.subtype))) | dim,
            filler(),
            text(render_timestamp(data.timestamp)) | dim,
        }),
        text(data.summary) | dim,
    });
}

// ─── M4: Faithful TS renderers (SystemTextMessage.tsx) ─────────────────
//
// TS system messages are FLAT one-line rows, NOT boxed/collapsing panels.
// Most app-event subtypes share the shape:
//   <Box flexDirection="row" marginTop={addMargin?1:0}
//        backgroundColor={bg} width="100%">
//     <Box minWidth={2}><Text {color|dimColor}>{GLYPH}</Text></Box>
//     <Text {color|dimColor}>{content}</Text>
//   </Box>
// and the generic info/warning fallback (SystemTextMessageInner) is:
//   <Box flexDirection="row" marginTop backgroundColor width="100%">
//     {dot && <Box minWidth={2}><Text color dimColor>{BLACK_CIRCLE}</Text></Box>}
//     <Box flexDirection="column" width={columns-10}>
//       <Text color dimColor>{content.trim()}</Text>
//     </Box>
//   </Box>
//
// NOTE: TS filters out the LLM system prompt upstream (isMeta / filtering);
// these renderers handle only the app-event subtypes that survive.

/// Glyphs used by TS system subtypes (constants/figures.ts).  (Darwin
/// BLACK_CIRCLE is "⏺" U+23FA; message_components.cppm already defines a
/// kBlackCircle using a different glyph, so we name ours distinctly.)
inline constexpr std::string_view kReferenceMark        = "\xE2\x80\xBB";  // ※ away_summary
inline constexpr std::string_view kTeardropAsterisk     = "\xE2\x9C\xBB";  // ✻ scheduled_task_fire / permission_retry
inline constexpr std::string_view kSystemBlackCircle    = "\xE2\x8F\xBA";  // ⏺ agents_killed / generic dot

/// Helper: build the canonical system row `[glyph(minWidth=2)] content` with
/// the TS margin + width semantics.  `glyph_cell` is rendered into a 2-wide
/// column; `content` is the trimmed single-line message.
[[nodiscard]] inline Element RenderSystemEventRow(
    Element glyph_cell, Element content, bool add_margin,
    std::optional<Color> bg = std::nullopt) {
    Decorator bg_dec = bg ? bgcolor(*bg) : nothing;
    Element row = hbox({
        std::move(glyph_cell),
        std::move(content),
        filler(),
    });
    row = row | bg_dec;
    if (add_margin) return vbox({text(""), std::move(row)});
    return row;
}

/// away_summary subtype:  `※ <content>` dimColor.
[[nodiscard]] inline Element RenderSystemAwaySummary(
    const SystemTextMessageData& data, bool add_margin = true) {
    auto glyph = hbox({text(std::string(kReferenceMark)),
                       text(" ")}) | dim | size(WIDTH, EQUAL, 2);
    auto content = text(data.summary.empty() ? data.detail : data.summary) | dim;
    return RenderSystemEventRow(std::move(glyph), std::move(content), add_margin);
}

/// scheduled_task_fire / permission_retry subtype:  `✻ <content>` dimColor.
[[nodiscard]] inline Element RenderSystemTeardropEvent(
    const SystemTextMessageData& data, bool add_margin = true) {
    auto glyph = hbox({text(std::string(kTeardropAsterisk)),
                       text(" ")}) | dim | size(WIDTH, EQUAL, 2);
    auto content = text(data.summary.empty() ? data.detail : data.summary) | dim;
    return RenderSystemEventRow(std::move(glyph), std::move(content), add_margin);
}

/// agents_killed subtype:  `⏺ All background agents stopped` (dot in error,
/// text dim).
[[nodiscard]] inline Element RenderSystemAgentsKilled(bool add_margin = true) {
    auto glyph = hbox({text(std::string(kSystemBlackCircle)),
                       text(" ")}) | color(Color::Red) | size(WIDTH, EQUAL, 2);
    auto content = text("All background agents stopped") | dim;
    return RenderSystemEventRow(std::move(glyph), std::move(content), add_margin);
}

/// Generic info/warning fallback (SystemTextMessageInner):  `⏺ <content>`
/// with dot colored for warning, content dim for info.  `columns` defaults to
/// 80 (the render width); TS wraps at columns-10.
[[nodiscard]] inline Element RenderSystemGenericEvent(
    const SystemTextMessageData& data, bool add_margin = true,
    int columns = 80) {
    const bool is_warning = false;  // info path by default
    const bool is_info = true;
    auto glyph = hbox({text(std::string(kSystemBlackCircle)),
                       text(" ")}) | dim | size(WIDTH, EQUAL, 2);
    std::string content = data.summary.empty() ? data.detail : data.summary;
    // trim leading/trailing whitespace (TS content.trim())
    auto b = content.find_first_not_of(" \t\n\r");
    if (b != std::string::npos) {
        auto e = content.find_last_not_of(" \t\n\r");
        content = content.substr(b, e - b + 1);
    }
    Decorator content_dec = dim;
    Element content_el = text(content) | content_dec;
    (void)is_warning; (void)is_info; (void)columns;
    return RenderSystemEventRow(std::move(glyph), std::move(content_el), add_margin);
}

/// Top-level faithful dispatcher: pick the row shape by TS subtype.  Returns
/// an empty element for subtypes TS hides (e.g. "thinking" → null).
[[nodiscard]] inline Element RenderSystemTextMessageFaithful(
    const SystemTextMessageData& data, bool add_margin = true) {
    switch (data.subtype) {
        case SystemMessageSubtype::AwaySummary:
            return RenderSystemAwaySummary(data, add_margin);
        case SystemMessageSubtype::BackgroundTask:
        case SystemMessageSubtype::HookSummary:
        case SystemMessageSubtype::ModelSwitch:
        case SystemMessageSubtype::ThinkingSummary:
            // These reuse the teardrop-style event row (dim content).
            return RenderSystemTeardropEvent(data, add_margin);
        case SystemMessageSubtype::Plain:
        case SystemMessageSubtype::Other:
        case SystemMessageSubtype::TurnDuration:
        case SystemMessageSubtype::MemorySaved:
        case SystemMessageSubtype::BridgeStatus:
        default:
            return RenderSystemGenericEvent(data, add_margin);
    }
}

}  // namespace cc::ui::messages
