/// @file prompt_paste_handler.cppm
/// @brief Multi-line paste detection and handling UI
module;
#include <string>
#include <optional>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
export module cc.ui.prompt.prompt_paste_handler;
export namespace cc::ui::prompt {
using namespace ftxui;
struct PastePreview { std::string content; std::size_t line_count{0}; bool is_large{false}; };
[[nodiscard]] inline Element render_paste_preview(const PastePreview& preview) {
    std::vector<Element> elements;
    elements.push_back(text(std::format("Pasted {} lines", preview.line_count)) | bold);
    if (preview.is_large) elements.push_back(text("(large paste - press Enter to confirm)") | dim);
    auto snippet = preview.content.substr(0, 200);
    elements.push_back(text(snippet) | dim);
    return vbox(elements) | border;
}
} // namespace
