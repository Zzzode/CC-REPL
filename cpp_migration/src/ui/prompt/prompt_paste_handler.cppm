/// @file prompt_paste_handler.cppm
/// @brief Multi-line paste detection and handling UI, including image paste
/// chip rendering helpers.
///
/// TS REF: src/components/PromptInput/inputPaste.ts (text paste truncation)
///         src/components/PromptInput/PromptInput.tsx L581-616 (image chip
///         detection + cursor-at-start inversion)
///         src/utils/imagePaste.ts (clipboard image read)
module;
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <format>
#include <cstddef>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.prompt.prompt_paste_handler;

import cc.utils.parse_references;

export namespace cc::ui::prompt {
using namespace ftxui;

// ── Text paste preview ──────────────────────────────────────────────────

/// @brief State for a multi-line text paste confirmation overlay.
struct PastePreview {
    std::string content;
    std::size_t line_count{0};
    bool is_large{false};
};

/// Render a text paste confirmation preview box.
/// TS REF: inputPaste.ts maybeTruncateMessageForInput — the preview shows
/// line count and a content snippet so the user can confirm before the
/// full text is injected.
[[nodiscard]] inline Element render_paste_preview(const PastePreview& preview) {
    std::vector<Element> elements;
    elements.push_back(text(std::format("Pasted {} lines", preview.line_count)) | bold);
    if (preview.is_large) {
        elements.push_back(text("(large paste - press Enter to confirm)") | dim);
    }
    auto snippet = preview.content.substr(0, 200);
    elements.push_back(text(snippet) | dim);
    return vbox(elements) | border;
}

// ── Image paste chip ────────────────────────────────────────────────────

/// @brief Display metadata for a single [Image #N] chip in the prompt.
///
/// Mirrors the TS ReferenceMatch (history.ts parseReferences) but adds
/// display-oriented fields used by the FTXUI renderer.  `start` and `end`
/// are UTF-8 byte offsets within the input text (same semantics as
/// String.prototype.matchAll index on ASCII placeholders).
struct ImageChipInfo {
    int id;                  ///< Paste id (e.g. 1 for [Image #1])
    std::size_t start;       ///< Byte offset of '[' in the input text
    std::size_t end;         ///< Byte offset one past ']'
    std::string label;       ///< The rendered text, e.g. "[Image #1]"
};

/// Parse all [Image #N] references from `text` and return display-ready
/// chip info.  Non-image refs (Pasted text #N, ...Truncated text #N) are
/// filtered out.
///
/// TS REF: PromptInput.tsx L581-584
///   const imageRefPositions = useMemo(() =>
///     parseReferences(displayedValue)
///       .filter(r => r.match.startsWith('[Image'))
///       .map(r => ({ start: r.index, end: r.index + r.match.length })),
///     [displayedValue]);
[[nodiscard]] inline std::vector<ImageChipInfo> collect_image_chips(
    std::string_view text) {
    const auto refs = cc::utils::parse_references(text);
    std::vector<ImageChipInfo> chips;
    chips.reserve(refs.size());
    for (const auto& r : refs) {
        if (!r.match.starts_with("[Image")) continue;
        chips.push_back(ImageChipInfo{
            .id    = r.id,
            .start = r.index,
            .end   = r.index + r.match.size(),
            .label = r.match,
        });
    }
    return chips;
}

/// Render a single [Image #N] chip as an FTXUI Element.
///
/// When `inverted` is true the chip is drawn with foreground/background
/// swapped — the visual "selected" state that appears when the cursor
/// parks at the chip's start position (TS REF: PromptInput.tsx L604-616,
/// inverse: true highlight applied when cursorOffset === ref.start).
///
/// No bold is applied — TS only sets `inverse: true` on the highlight,
/// which maps to ANSI reverse video (SGR 7).  The inversion alone is
/// sufficient to make the chip visually distinct.
///
/// @param label   The chip text, e.g. "[Image #1]" (from format_image_ref
///                or collect_image_chips label field).
/// @param inverted  When true, swap fg/bg (cursor-at-start selected state).
/// @param chip_color  Foreground colour (default white for dark themes).
///
/// TS REF: ShimmeredInput.tsx L115
///   <Text ... inverse={part.highlight?.inverse}><Ansi>{part.text}</Ansi></Text>
[[nodiscard]] inline Element render_image_chip(
    std::string_view label,
    bool is_inverted = false,
    Color chip_color = Color::White) {
    // FTXUI's `inverted` maps to ANSI SGR 7 (reverse video), exactly what
    // chalk.inverse produces in the TS reference.  No bold — TS doesn't
    // apply it to image chips.
    Decorator decor = color(chip_color);
    if (is_inverted) {
        decor = decor | ftxui::inverted;
    }
    return text(std::string(label)) | decor;
}

/// Convenience overload: render an ImageChipInfo as an Element.
/// `cursor_pos` is the absolute byte offset of the text cursor; when it
/// equals chip.start the chip is rendered inverted (selected state).
[[nodiscard]] inline Element render_image_chip(
    const ImageChipInfo& chip,
    std::size_t cursor_pos) {
    const bool is_selected = (cursor_pos == chip.start);
    return render_image_chip(chip.label, is_selected);
}

// ── Image paste detection helpers ───────────────────────────────────────

/// @brief Result classifying a clipboard paste operation.
enum class PasteKind {
    Text,           ///< Plain text paste (possibly multi-line)
    Image,          ///< Clipboard contains image data (PNG, etc.)
    ImageFilePath,  ///< Clipboard text is a path to an image file
    Empty,          ///< Nothing usable in the clipboard
};

/// Heuristic: determine if `clipboard_text` looks like an image file path
/// that was pasted as text (e.g. dragging a file into the terminal).
/// Matches the TS regex IMAGE_EXTENSION_REGEX from imagePaste.ts L270:
///   /\.(png|jpe?g|gif|webp)$/i
///
/// TS REF: imagePaste.ts isImageFilePath()
[[nodiscard]] inline bool is_image_file_path(std::string_view text) {
    // Find the last dot
    const auto dot = text.rfind('.');
    if (dot == std::string_view::npos) return false;
    auto ext = text.substr(dot + 1);
    // Case-insensitive compare for png/jpg/jpeg/gif/webp
    // (TS regex uses /i flag)
    auto to_lower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    std::string ext_lower;
    ext_lower.reserve(ext.size());
    for (char c : ext) ext_lower.push_back(to_lower(c));

    return ext_lower == "png" || ext_lower == "jpg" || ext_lower == "jpeg" ||
           ext_lower == "gif" || ext_lower == "webp";
}

/// @brief State for tracking an in-flight async image paste (clipboard read
/// happening on a background thread while the [Image #N] placeholder is
/// already visible in the input).
///
/// TS REF: PromptInput.tsx L1151-1183 — onImagePaste inserts the placeholder
/// synchronously (instant feedback) while the actual clipboard read runs
/// async via getImageFromClipboard().
struct ImagePasteInFlight {
    int id;                  ///< Paste id matching the [Image #N] placeholder
    std::string placeholder; ///< The "[Image #N]" text inserted into input
    bool started{false};     ///< Background worker has been spawned
    bool completed{false};   ///< Worker finished (success or failure)
    bool success{false};     ///< True if image data was retrieved
    std::string error;       ///< Error message if failed
};

} // namespace cc::ui::prompt
