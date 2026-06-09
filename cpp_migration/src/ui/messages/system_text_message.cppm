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
             | borderEmpty()
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

}  // namespace cc::ui::messages
