/// @file message_grouped_tools.cppm
/// @brief Grouped tool use message rendering (collapsed view)
module;

#include <string>
#include <vector>
#include <optional>
#include <format>
#include <ftxui/dom/elements.hpp>

export module cc.ui.messages.message_grouped_tools;

export namespace cc::ui::messages {

using namespace ftxui;

/// Grouped tool entry
struct GroupedToolEntry {
    std::string tool_name;
    std::size_t call_count{1};
    bool all_succeeded{true};
    std::optional<double> total_duration_ms;
};

/// Render grouped tools summary
[[nodiscard]] inline Element render_grouped_tools(const std::vector<GroupedToolEntry>& tools) {
    if (tools.empty()) return text("");

    std::vector<Element> elements;
    elements.push_back(text(std::format("{} tool calls:", tools.size())) | dim);

    for (const auto& tool : tools) {
        auto status = tool.all_succeeded ? "✓" : "✗";
        auto status_color = tool.all_succeeded ? Color::Green : Color::Red;
        std::string suffix;
        if (tool.call_count > 1) suffix = std::format(" (x{})", tool.call_count);
        if (tool.total_duration_ms) suffix += std::format(" {:.0f}ms", *tool.total_duration_ms);

        elements.push_back(hbox({
            text(status) | color(status_color),
            text(" " + tool.tool_name + suffix),
        }));
    }

    return vbox(elements);
}

} // namespace cc::ui::messages
