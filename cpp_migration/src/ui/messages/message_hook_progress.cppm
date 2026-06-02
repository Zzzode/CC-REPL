/// @file message_hook_progress.cppm
/// @brief Hook execution progress message rendering
module;

#include <string>
#include <vector>
#include <optional>
#include <format>
#include <chrono>
#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_hook_progress;

export namespace cc::ui::messages {

using namespace ftxui;

/// Hook execution state
enum class HookState {
    Pending,
    Running,
    Completed,
    Failed,
    Skipped,
};

/// Hook progress entry
struct HookProgressEntry {
    std::string hook_name;
    HookState state{HookState::Pending};
    std::optional<double> duration_ms;
    std::optional<std::string> error;
};

/// Render hook progress
[[nodiscard]] inline Element render_hook_progress(const std::vector<HookProgressEntry>& hooks) {
    if (hooks.empty()) return text("");

    std::vector<Element> elements;
    for (const auto& hook : hooks) {
        auto [indicator, ind_color] = [&]() -> std::pair<std::string, Color> {
            switch (hook.state) {
                case HookState::Pending: return {"○", Color::GrayDark};
                case HookState::Running: return {"◐", Color::Yellow};
                case HookState::Completed: return {"●", Color::Green};
                case HookState::Failed: return {"✗", Color::Red};
                case HookState::Skipped: return {"⊘", Color::GrayDark};
            }
            return {"?", Color::White};
        }();

        std::string suffix;
        if (hook.duration_ms) suffix = std::format(" ({:.0f}ms)", *hook.duration_ms);

        elements.push_back(hbox({
            text(indicator) | color(ind_color),
            text(" " + hook.hook_name + suffix),
        }));

        if (hook.error) {
            elements.push_back(text("    " + *hook.error) | color(Color::Red) | dim);
        }
    }

    return vbox(elements);
}

} // namespace cc::ui::messages
