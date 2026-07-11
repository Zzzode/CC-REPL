/// @file prompt_stash_notice.cppm
/// @brief Notice when user input is stashed during processing.
/// TS REF: src/components/PromptInput/PromptInputStashNotice.tsx —
///   renders "{figures.pointerSmall} Stashed (auto-restores after submit)"
///   with dimColor when hasStash is true.  The notice sits above the input
///   area so the user knows their typed text was saved and will be restored
///   after the current request completes.
module;

#include <string>
#include <format>

#include <ftxui/dom/elements.hpp>

export module cc.ui.prompt.prompt_stash_notice;

import cc.ui.design.figures;  // kPointerSmall (figures.pointerSmall '›')

export namespace cc::ui::prompt {
using namespace ftxui;

/// Data for the stash notice (rendered above the prompt input).
struct StashNotice {
    std::string stashed_text;   ///< The stashed input text (for context display)
    std::size_t char_count{0};  ///< Character count of the stashed text
};

/// Render the stash notice element.
/// TS REF: PromptInputStashNotice.tsx —
///   <Box paddingLeft={2}><Text dimColor>
///     {figures.pointerSmall} Stashed (auto-restores after submit)
///   </Text></Box>
///
/// The CPP version also shows the character count in dim text so the user
/// knows how much was stashed (useful when the stashed input was very long).
[[nodiscard]] inline Element render_stash_notice(const StashNotice& notice) {
    if (notice.stashed_text.empty()) return text("");

    namespace figs = cc::ui::design::figures;
    // TS REF: figures.pointerSmall = '›' U+203A (kPointerSmall)
    return hbox({
        text("  ") | dim,  // paddingLeft={2}
        text(std::string(figs::kPointerSmall) + " Stashed (auto-restores after submit)")
            | dim,
    });
}

} // namespace cc::ui::prompt