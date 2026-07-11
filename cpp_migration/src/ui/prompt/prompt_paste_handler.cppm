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
#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>

export module cc.ui.prompt.prompt_paste_handler;

import cc.utils.parse_references;
// OS clipboard image read (macOS osascript «class PNGf»).
// TS REF: src/utils/imagePaste.ts getImageFromClipboard / hasImageInClipboard
import cc.utils.clipboard;

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

// ── Image magic-byte detection ──────────────────────────────────────────

/// Image format detected from magic-byte signature.
/// Mirrors TS ImageMediaType from imageResizer.ts L22.
enum class ImageFormat {
    PNG,   ///< \x89\x50\x4e\x47 (89 50 4E 47)
    JPEG,  ///< \xff\xd8\xff      (FF D8 FF)
    GIF,   ///< \x47\x49\x46      ("GIF8")
    WebP,  ///< RIFF....WEBP      (bytes 0-3="RIFF", 8-11="WEBP")
};

/// Detect image format from the first bytes of a buffer using magic bytes.
/// Returns nullopt if the buffer is too short (< 4 bytes) or no known
/// signature matches.
///
/// Signature table (TS REF: imageResizer.ts detectImageFormatFromBuffer
/// L769-812 — character-for-character port):
///   PNG:  bytes[0..3] == 0x89 0x50 0x4E 0x47
///   JPEG: bytes[0..2] == 0xFF 0xD8 0xFF
///   GIF:  bytes[0..2] == 0x47 0x49 0x46  ("GIF8" prefix)
///   WebP: bytes[0..3] == "RIFF" && bytes[8..11] == "WEBP"
///
/// NOTE: JPEG check requires only 3 bytes but we guard with size >= 4 so
/// every branch can safely index bytes[3] for the PNG/WebP checks.
[[nodiscard]] inline std::optional<ImageFormat>
detect_image_magic_bytes(std::string_view buffer) {
    if (buffer.size() < 4) return std::nullopt;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer.data());
    const auto size   = buffer.size();

    // PNG: 89 50 4E 47 — TS REF L773-779
    if (bytes[0] == 0x89 && bytes[1] == 0x50 &&
        bytes[2] == 0x4E && bytes[3] == 0x47) {
        return ImageFormat::PNG;
    }
    // JPEG: FF D8 FF — TS REF L783-785
    if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return ImageFormat::JPEG;
    }
    // GIF: 47 49 46 ("GIF87a" / "GIF89a") — TS REF L788-790
    if (bytes[0] == 0x47 && bytes[1] == 0x49 && bytes[2] == 0x46) {
        return ImageFormat::GIF;
    }
    // WebP: "RIFF" at offset 0, "WEBP" at offset 8 — TS REF L793-808
    if (size >= 12 &&
        bytes[0] == 0x52 && bytes[1] == 0x49 &&
        bytes[2] == 0x46 && bytes[3] == 0x46 &&
        bytes[8] == 0x57 && bytes[9] == 0x45 &&
        bytes[10] == 0x42 && bytes[11] == 0x50) {
        return ImageFormat::WebP;
    }
    return std::nullopt;
}

// ── Paste classification ────────────────────────────────────────────────

/// Classify a paste buffer into a PasteKind.
///
/// Decision priority (highest first):
///   1. has_image_data — system clipboard reports raw image (PNGf etc.)
///   2. detect_image_magic_bytes — pasted text starts with image signature
///      (rare edge case: binary paste delivered as text)
///   3. is_image_file_path — pasted text looks like /path/to/photo.png
///   4. Text — plain text paste (possibly multi-line)
///   5. Empty — nothing usable
///
/// TS REF: imagePaste.ts isImageFilePath() + PromptInput.tsx onPaste
/// dispatch logic — the TS side checks hasImageInClipboard() before the
/// text paste path, which maps to has_image_data here.
[[nodiscard]] inline PasteKind classify_paste_kind(
    std::string_view text_buffer,
    bool has_image_data) {
    // Raw clipboard image wins over text content.
    if (has_image_data) return PasteKind::Image;

    // Empty clipboard — nothing to paste.
    if (text_buffer.empty()) return PasteKind::Empty;

    // Edge case: raw binary image bytes somehow arrived as a text paste.
    // Detecting this prevents a garbled text injection from a PNG/JPEG.
    if (detect_image_magic_bytes(text_buffer).has_value()) {
        return PasteKind::Image;
    }

    // File-path heuristic: dragged-in image from Finder/Explorer.
    if (is_image_file_path(text_buffer)) return PasteKind::ImageFilePath;

    return PasteKind::Text;
}

// ── Clipboard image read helpers ────────────────────────────────────────

/// Read PNG image bytes from the system clipboard (macOS only).
/// Thin wrapper around cc::utils::clipboard::read_image_png().  Returns
/// nullopt off-macOS or on failure.
///
/// Implementation (TS REF: imagePaste.ts getImageFromClipboard L124-242):
///   - Runs osascript to extract «class PNGf» from NSPasteboard into a
///     temp file via run_detached() (fork+setsid+exec isolates osascript
///     from cc-repl's raw-mode terminal — see clipboard.cppm gotcha).
///   - Falls back to HTML data-URL extraction for web-app clipboard
///     images (Lark/Feishu, Google Docs embed base64 in «class HTML»).
///   - Non-PNG HTML-embedded images are converted via `sips -s format png`.
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>>
read_clipboard_image_png() {
    return cc::utils::clipboard::read_image_png();
}

/// Read JPEG image bytes from the system clipboard.
///
/// macOS osascript «class PNGf» always returns PNG, so this attempts a
/// PNG read first and inspects the result's magic bytes.  If the data is
/// actually JPEG (FF D8 FF — can happen via the HTML fallback when a web
/// app pastes raw JPEG without converting), the bytes are returned.
/// Returns nullopt when no JPEG is available.
///
/// TS REF parity: imagePaste.ts getImageFromClipboard() — TS always
/// returns PNG from osascript; JPEG handling is only via the HTML
/// fallback (which also converts to PNG via sips on the TS side).  This
/// helper exists for API completeness and future cross-platform support
/// (Linux xclip can return image/jpeg directly).
[[nodiscard]] inline std::optional<std::vector<std::uint8_t>>
read_clipboard_image_jpeg() {
    auto bytes = cc::utils::clipboard::read_image_png();
    if (!bytes || bytes->size() < 3) return std::nullopt;
    // Inspect magic bytes: FF D8 FF = JPEG.
    const auto& b = *bytes;
    if (static_cast<std::uint8_t>(b[0]) == 0xFF &&
        static_cast<std::uint8_t>(b[1]) == 0xD8 &&
        static_cast<std::uint8_t>(b[2]) == 0xFF) {
        return bytes;
    }
    return std::nullopt;
}

// ── Footer hint rendering ───────────────────────────────────────────────

/// Render a "Image in clipboard · Ctrl+V to paste" footer hint.
///
/// Shown when the terminal regains focus and the system clipboard holds
/// an image (detected via cc::utils::clipboard::has_image()).  The hint
/// tells the user they can press the paste keybinding to attach the
/// image as an [Image #N] reference.
///
/// TS REF: useClipboardImageHint.ts L56-61
///   addNotification({
///     key: 'clipboard-image-hint',
///     text: `Image in clipboard · ${getShortcutDisplay(
///             'chat:imagePaste', 'Chat', 'ctrl+v')} to paste`,
///     priority: 'immediate',
///     timeoutMs: 8000,
///   })
///
/// @param shortcut  Keybinding display string (default "Ctrl+V").  Pass
///                  the platform-appropriate string from the keybinding
///                  system (e.g. "⌘V" on macOS when resolved).
[[nodiscard]] inline Element render_clipboard_image_hint(
    std::string_view shortcut = "Ctrl+V") {
    // \xC2\xB7 = UTF-8 middle dot (·), matching the TS separator.
    auto hint = std::format("Image in clipboard \xC2\xB7 {} to paste", shortcut);
    return text(hint) | dim;
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
