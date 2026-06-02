// Image handling: clipboard paste, storage, ANSI-to-SVG, ANSI-to-PNG rendering
// Sources: imagePaste.ts, imageStore.ts, ansiToSvg.ts, ansiToPng.ts
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.image_handling;

import cc.utils.async;

export namespace cc::utils::image {

// =========================================================================
// AnsiColor - RGB color value
// =========================================================================
struct AnsiColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

inline constexpr AnsiColor DEFAULT_FG{229, 229, 229};   // light gray
inline constexpr AnsiColor DEFAULT_BG{30, 30, 30};      // dark gray

// =========================================================================
// TextSpan - A span of styled text within a parsed ANSI line
// =========================================================================
struct TextSpan {
    std::string text;
    AnsiColor color;
    bool bold;
};

/// A parsed line is a sequence of text spans
using ParsedLine = std::vector<TextSpan>;

// =========================================================================
// ANSI parsing
// =========================================================================

/// Parse ANSI escape sequences from text into styled spans per line.
/// Supports: basic colors (30-37, 90-97), 256-color (38;5;n), true color (38;2;r;g;b)
[[nodiscard]] std::vector<ParsedLine> parse_ansi(std::string_view text);

/// Get a color from the 256-color palette by index (0-255)
[[nodiscard]] AnsiColor get_256_color(uint8_t index);

// =========================================================================
// AnsiToSvgOptions - Configuration for SVG rendering
// =========================================================================
struct AnsiToSvgOptions {
    std::string font_family;
    uint32_t font_size;
    uint32_t line_height;
    uint32_t padding_x;
    uint32_t padding_y;
    std::string background_color;
    uint32_t border_radius;
};

/// Overload: create AnsiToSvgOptions with defaults
[[nodiscard]] inline AnsiToSvgOptions make_ansi_to_svg_options() {
    return AnsiToSvgOptions{
        .font_family = "Menlo, Monaco, monospace",
        .font_size = 14,
        .line_height = 22,
        .padding_x = 24,
        .padding_y = 24,
        .background_color = "rgb(30, 30, 30)",
        .border_radius = 8,
    };
}

/// Convert ANSI-escaped text to an SVG string
[[nodiscard]] std::string ansi_to_svg(std::string_view ansi_text);

/// Convert ANSI-escaped text to SVG with custom options
[[nodiscard]] std::string ansi_to_svg(
    std::string_view ansi_text,
    const AnsiToSvgOptions& options
);

// =========================================================================
// AnsiToPngOptions - Configuration for PNG rendering
// =========================================================================
struct AnsiToPngOptions {
    uint32_t scale;               // pixel scale factor (default 1 = 24x48 cell)
    uint32_t padding_x;
    uint32_t padding_y;
    AnsiColor background_color;
};

/// Overload: create AnsiToPngOptions with defaults
[[nodiscard]] inline AnsiToPngOptions make_ansi_to_png_options() {
    return AnsiToPngOptions{
        .scale = 1,
        .padding_x = 24,
        .padding_y = 24,
        .background_color = DEFAULT_BG,
    };
}

/// Convert ANSI-escaped text directly to a PNG image buffer.
/// Uses a bundled 24x48 bitmap font (Fira Code Regular, SIL OFL 1.1).
/// No external dependencies — blits glyphs into RGBA then deflate-encodes to PNG.
[[nodiscard]] std::expected<std::vector<uint8_t>, std::string>
ansi_to_png(std::string_view ansi_text);

/// Convert ANSI-escaped text to PNG with custom options
[[nodiscard]] std::expected<std::vector<uint8_t>, std::string>
ansi_to_png(
    std::string_view ansi_text,
    const AnsiToPngOptions& options
);

// =========================================================================
// ImagePasteResult - Result of pasting an image from clipboard
// =========================================================================
struct ImagePasteResult {
    std::vector<uint8_t> data;
    std::string mime_type;         // e.g. "image/png"
    std::optional<std::string> filename;
};

/// Paste image data from the system clipboard
[[nodiscard]] async::Task<std::expected<ImagePasteResult, std::string>>
paste_image_from_clipboard();

// =========================================================================
// ImageStore - Persistent storage for images (save/load/delete/list)
// =========================================================================
class ImageStore {
public:
    explicit ImageStore(std::filesystem::path storage_dir);
    ~ImageStore();

    ImageStore(const ImageStore&) = delete;
    ImageStore& operator=(const ImageStore&) = delete;
    ImageStore(ImageStore&&) noexcept;
    ImageStore& operator=(ImageStore&&) noexcept;

    /// Save image data and return a unique identifier
    [[nodiscard]] async::Task<std::expected<std::string, std::string>>
    save(std::span<const uint8_t> data, std::string_view mime_type);

    /// Load image data by identifier
    [[nodiscard]] async::Task<std::expected<std::vector<uint8_t>, std::string>>
    load(std::string_view id);

    /// Delete an image by identifier
    [[nodiscard]] async::Task<std::expected<void, std::string>>
    remove(std::string_view id);

    /// List all stored image identifiers
    [[nodiscard]] async::Task<std::expected<std::vector<std::string>, std::string>>
    list();

    /// Get the storage directory path
    [[nodiscard]] const std::filesystem::path& storage_dir() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cc::utils::image
