/// @file prompt_stash_notice.cppm
/// @brief Notice when user input is stashed during processing
module;
#include <string>
#include <optional>
#include <format>
#include <ftxui/dom/elements.hpp>
export module cc.ui.prompt.prompt_stash_notice;
export namespace cc::ui::prompt {
using namespace ftxui;
struct StashNotice { std::string stashed_text; std::size_t char_count{0}; };
[[nodiscard]] inline Element render_stash_notice(const StashNotice& notice) {
    if (notice.stashed_text.empty()) return text("");
    return hbox({text("⏸ Input stashed") | dim, text(std::format(" ({} chars)", notice.char_count)) | dim});
}
} // namespace
