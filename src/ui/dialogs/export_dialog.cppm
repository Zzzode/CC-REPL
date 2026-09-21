/// @file export_dialog.cppm
/// @brief Session/conversation export dialog
module;
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.dialogs.export_dialog;
export namespace cc::ui::dialogs {
using namespace ftxui;
enum class ExportFormat { Markdown, Json, Html };
struct ExportOptions { ExportFormat format{ExportFormat::Markdown}; bool include_tool_results{true}; bool include_thinking{false}; std::string output_path; };
[[nodiscard]] inline Element render_export_dialog(const ExportOptions& opts) {
    auto format_str = [&] { switch (opts.format) { case ExportFormat::Markdown: return "Markdown"; case ExportFormat::Json: return "JSON"; case ExportFormat::Html: return "HTML"; } return ""; }();
    return vbox({
        text("Export Conversation") | bold,
        separator(),
        text(std::string("  Format: ") + format_str),
        text(std::string("  Tool results: ") + (opts.include_tool_results ? "yes" : "no")),
        text(std::string("  Thinking: ") + (opts.include_thinking ? "yes" : "no")),
        text("  Output: " + opts.output_path) | dim,
    }) | border;
}
} // namespace
