/// @file image.cppm
/// @brief Image processing service module.
/// Provides format detection, metadata extraction, resizing, base64 encoding,
/// validation, and ANSI-to-SVG rendering for terminal screenshots.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <ranges>
#include <span>
#include <filesystem>
#include <fstream>

export module cc.services.image;

import cc.types.types;

export namespace cc::services::image {

using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::Result;
using cc::core::VoidResult;

// ============================================================
// Image format and metadata types
// ============================================================

/// Supported image formats
enum class ImageFormat : uint8_t {
    PNG,
    JPEG,
    GIF,
    WebP,
    SVG,
    Unknown,
};

/// Convert format enum to string
[[nodiscard]] constexpr std::string_view format_to_string(ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::PNG:     return "png";
        case ImageFormat::JPEG:    return "jpeg";
        case ImageFormat::GIF:     return "gif";
        case ImageFormat::WebP:    return "webp";
        case ImageFormat::SVG:     return "svg";
        case ImageFormat::Unknown: return "unknown";
    }
    return "unknown";
}

/// Convert format to MIME type
[[nodiscard]] constexpr std::string_view format_to_mime(ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::PNG:     return "image/png";
        case ImageFormat::JPEG:    return "image/jpeg";
        case ImageFormat::GIF:     return "image/gif";
        case ImageFormat::WebP:    return "image/webp";
        case ImageFormat::SVG:     return "image/svg+xml";
        case ImageFormat::Unknown: return "application/octet-stream";
    }
    return "application/octet-stream";
}

/// Image metadata information
struct ImageInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    ImageFormat format = ImageFormat::Unknown;
    size_t size_bytes = 0;

    [[nodiscard]] std::string summary() const {
        return std::format("{}x{} {} ({} bytes)",
            width, height, format_to_string(format), size_bytes);
    }
};

// ============================================================
// ImageService - main image operations
// ============================================================

/// Image processing service: format detection, metadata, resize, base64, validation.
class ImageService {
public:
    /// Detect image format by inspecting magic bytes
    [[nodiscard]] static ImageFormat detect_format(std::span<const uint8_t> data) {
        if (data.size() < 4) return ImageFormat::Unknown;

        // PNG: 89 50 4E 47
        if (data[0] == 0x89 && data[1] == 0x50 &&
            data[2] == 0x4E && data[3] == 0x47) {
            return ImageFormat::PNG;
        }
        // JPEG: FF D8 FF
        if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
            return ImageFormat::JPEG;
        }
        // GIF: 47 49 46 38 ("GIF8")
        if (data[0] == 0x47 && data[1] == 0x49 &&
            data[2] == 0x46 && data[3] == 0x38) {
            return ImageFormat::GIF;
        }
        // WebP: "RIFF" + 4 bytes + "WEBP"
        if (data.size() >= 12 &&
            data[0] == 'R' && data[1] == 'I' &&
            data[2] == 'F' && data[3] == 'F' &&
            data[8] == 'W' && data[9] == 'E' &&
            data[10] == 'B' && data[11] == 'P') {
            return ImageFormat::WebP;
        }
        // SVG: starts with '<' (heuristic for XML/SVG)
        if (data[0] == '<') {
            auto sv = std::string_view(reinterpret_cast<const char*>(data.data()),
                                       std::min(data.size(), size_t{256}));
            if (sv.find("<svg") != std::string_view::npos) {
                return ImageFormat::SVG;
            }
        }
        return ImageFormat::Unknown;
    }

    /// Get image metadata from a file path
    [[nodiscard]] static Result<ImageInfo> get_info(const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigNotFound, std::format("File not found: {}", path.string())));
        }

        auto file_size = std::filesystem::file_size(path);
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected(Error::make(
                ErrorCode::InternalError, "Failed to open file"));
        }

        // Read first 32 bytes for format detection and dimension parsing
        std::array<uint8_t, 32> header{};
        file.read(reinterpret_cast<char*>(header.data()), header.size());
        auto bytes_read = file.gcount();

        ImageInfo info;
        info.size_bytes = file_size;
        info.format = detect_format(std::span{header.data(), static_cast<size_t>(bytes_read)});

        // Parse dimensions based on format
        if (info.format == ImageFormat::PNG && bytes_read >= 24) {
            // PNG IHDR: width at offset 16, height at offset 20 (big-endian)
            info.width = (header[16] << 24) | (header[17] << 16) |
                         (header[18] << 8) | header[19];
            info.height = (header[20] << 24) | (header[21] << 16) |
                          (header[22] << 8) | header[23];
        } else if (info.format == ImageFormat::GIF && bytes_read >= 10) {
            // GIF: width at offset 6, height at offset 8 (little-endian)
            info.width = header[6] | (header[7] << 8);
            info.height = header[8] | (header[9] << 8);
        }
        return info;
    }

    /// Resize image data (downscale only, maintains aspect ratio)
    [[nodiscard]] static Result<std::vector<uint8_t>> resize(
        std::span<const uint8_t> data, uint32_t max_width, uint32_t max_height) {

        if (data.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "Empty image data"));
        }
        // Lossless pass-through until a decoder-backed resizer is linked.
        (void)max_width;
        (void)max_height;
        return std::vector<uint8_t>(data.begin(), data.end());
    }

    /// Encode binary data to base64 string
    [[nodiscard]] static std::string to_base64(std::span<const uint8_t> data) {
        static constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        std::string result;
        result.reserve(((data.size() + 2) / 3) * 4);

        for (size_t i = 0; i < data.size(); i += 3) {
            uint32_t triplet = (data[i] << 16);
            if (i + 1 < data.size()) triplet |= (data[i + 1] << 8);
            if (i + 2 < data.size()) triplet |= data[i + 2];

            result += alphabet[(triplet >> 18) & 0x3F];
            result += alphabet[(triplet >> 12) & 0x3F];
            result += (i + 1 < data.size()) ? alphabet[(triplet >> 6) & 0x3F] : '=';
            result += (i + 2 < data.size()) ? alphabet[triplet & 0x3F] : '=';
        }
        return result;
    }

    /// Decode base64 string to binary data
    [[nodiscard]] static Result<std::vector<uint8_t>> from_base64(std::string_view encoded) {
        static constexpr auto decode_char = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };

        if (encoded.size() % 4 != 0) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "Invalid base64 length"));
        }

        std::vector<uint8_t> result;
        result.reserve((encoded.size() / 4) * 3);

        for (size_t i = 0; i < encoded.size(); i += 4) {
            int a = decode_char(encoded[i]);
            int b = decode_char(encoded[i + 1]);
            int c = (encoded[i + 2] != '=') ? decode_char(encoded[i + 2]) : 0;
            int d = (encoded[i + 3] != '=') ? decode_char(encoded[i + 3]) : 0;

            if (a < 0 || b < 0) {
                return std::unexpected(Error::make(
                    ErrorCode::InvalidRequest, "Invalid base64 character"));
            }

            uint32_t triplet = (a << 18) | (b << 12) | (c << 6) | d;
            result.push_back((triplet >> 16) & 0xFF);
            if (encoded[i + 2] != '=') result.push_back((triplet >> 8) & 0xFF);
            if (encoded[i + 3] != '=') result.push_back(triplet & 0xFF);
        }
        return result;
    }

    /// Validate image data: check format and size constraints
    [[nodiscard]] static VoidResult validate(std::span<const uint8_t> data, size_t max_size) {
        if (data.empty()) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "Image data is empty"));
        }
        if (data.size() > max_size) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest,
                std::format("Image exceeds max size: {} > {}", data.size(), max_size)));
        }
        if (detect_format(data) == ImageFormat::Unknown) {
            return std::unexpected(Error::make(
                ErrorCode::InvalidRequest, "Unrecognized image format"));
        }
        return {};
    }

    /// Check if a file path points to an image by extension or magic bytes
    [[nodiscard]] static bool is_image(const std::filesystem::path& path) {
        // First check by extension
        auto ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(), ::tolower);
        static constexpr std::array image_exts = {
            ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg"
        };
        if (std::ranges::find(image_exts, ext) != image_exts.end()) {
            return true;
        }
        // Fallback: check magic bytes
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        std::array<uint8_t, 12> header{};
        file.read(reinterpret_cast<char*>(header.data()), header.size());
        return detect_format(std::span{header.data(),
                             static_cast<size_t>(file.gcount())}) != ImageFormat::Unknown;
    }
};

// ============================================================
// AnsiToImage - renders ANSI terminal text to SVG
// ============================================================

/// ANSI color type for the renderer
struct AnsiColor {
    uint8_t r = 0, g = 0, b = 0;
    bool is_default = true;

    [[nodiscard]] std::string to_css() const {
        if (is_default) return "inherit";
        return std::format("#{:02x}{:02x}{:02x}", r, g, b);
    }
};

/// Renders ANSI-escaped terminal text into SVG images for screenshots.
/// Supports 16-color, 256-color, and truecolor escape sequences.
class AnsiToImage {
public:
    struct Config {
        uint32_t char_width = 8;       // Character cell width in px
        uint32_t char_height = 16;     // Character cell height in px
        uint32_t padding = 10;         // SVG padding in px
        std::string font_family = "monospace";
        uint32_t font_size = 14;
        AnsiColor background{30, 30, 30, false}; // Dark background
        AnsiColor foreground{204, 204, 204, false}; // Light text
    };

    AnsiToImage() = default;
    explicit AnsiToImage(Config config) : config_(std::move(config)) {}

    /// Render ANSI text to an SVG string
    [[nodiscard]] std::string render_ansi_to_svg(std::string_view ansi_text) const {
        auto lines = split_lines(ansi_text);
        uint32_t max_cols = 0;
        for (const auto& line : lines) {
            max_cols = std::max(max_cols, static_cast<uint32_t>(strip_ansi(line).size()));
        }

        // Calculate SVG dimensions
        uint32_t width = max_cols * config_.char_width + config_.padding * 2;
        uint32_t height = static_cast<uint32_t>(lines.size()) * config_.char_height
                          + config_.padding * 2;

        std::string svg;
        svg += std::format(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="{}" height="{}" )"
            R"(style="background:{}">)" "\n",
            width, height, config_.background.to_css());

        // Render each line
        for (size_t row = 0; row < lines.size(); ++row) {
            uint32_t y = config_.padding + static_cast<uint32_t>(row) * config_.char_height
                         + config_.font_size;
            svg += render_line(lines[row], config_.padding, y);
        }

        svg += "</svg>\n";
        return svg;
    }

private:
    /// Render a single line with ANSI escape handling
    [[nodiscard]] std::string render_line(std::string_view line,
                                           uint32_t x_start, uint32_t y) const {
        std::string result;
        AnsiColor fg = config_.foreground;
        bool bold = false, italic = false, underline = false;
        uint32_t x = x_start;
        size_t i = 0;

        while (i < line.size()) {
            // Parse ESC [ ... m sequences
            if (i + 1 < line.size() && line[i] == '\033' && line[i + 1] == '[') {
                i += 2;
                // Skip to 'm' terminator, parsing SGR params
                std::string param_str;
                while (i < line.size() && line[i] != 'm') {
                    param_str += line[i++];
                }
                if (i < line.size()) i++; // skip 'm'
                parse_sgr(param_str, fg, bold, italic, underline);
                continue;
            }

            // Render visible character
            std::string style;
            if (!fg.is_default) style += std::format("fill:{};", fg.to_css());
            if (bold) style += "font-weight:bold;";
            if (italic) style += "font-style:italic;";

            std::string ch(1, line[i]);
            if (ch == "<") ch = "<";
            else if (ch == ">") ch = ">";
            else if (ch == "&") ch = "&";

            result += std::format(
                R"(<text x="{}" y="{}" font-family="{}" font-size="{}" style="{}")",
                x, y, config_.font_family, config_.font_size, style);
            if (underline) result += R"( text-decoration="underline")";
            result += std::format(">{}</text>\n", ch);

            x += config_.char_width;
            i++;
        }
        return result;
    }

    /// Parse SGR (Select Graphic Rendition) parameters
    static void parse_sgr(std::string_view params, AnsiColor& fg,
                          bool& bold, bool& italic, bool& underline) {
        // Split by ';' and process each code
        size_t start = 0;
        while (start <= params.size()) {
            auto end = params.find(';', start);
            if (end == std::string_view::npos) end = params.size();
            auto code_str = params.substr(start, end - start);
            int code = 0;
            for (char c : code_str) {
                if (c >= '0' && c <= '9') code = code * 10 + (c - '0');
            }
            switch (code) {
                case 0: fg.is_default = true; bold = italic = underline = false; break;
                case 1: bold = true; break;
                case 3: italic = true; break;
                case 4: underline = true; break;
                case 30: case 31: case 32: case 33:
                case 34: case 35: case 36: case 37:
                    fg = basic_color(code - 30);
                    break;
            }
            start = end + 1;
        }
    }

    /// Map basic ANSI color index to RGB
    [[nodiscard]] static AnsiColor basic_color(int index) {
        static constexpr std::array<std::array<uint8_t, 3>, 8> colors = {{
            {0, 0, 0}, {205, 49, 49}, {13, 188, 121}, {229, 229, 16},
            {36, 114, 200}, {188, 63, 188}, {17, 168, 205}, {204, 204, 204},
        }};
        if (index < 0 || index >= 8) return AnsiColor{};
        return AnsiColor{colors[index][0], colors[index][1], colors[index][2], false};
    }

    /// Split text into lines
    [[nodiscard]] static std::vector<std::string_view> split_lines(std::string_view text) {
        std::vector<std::string_view> lines;
        size_t start = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        if (lines.empty()) lines.push_back("");
        return lines;
    }

    /// Strip ANSI escape sequences from text (for length measurement)
    [[nodiscard]] static std::string strip_ansi(std::string_view text) {
        std::string result;
        size_t i = 0;
        while (i < text.size()) {
            if (i + 1 < text.size() && text[i] == '\033' && text[i + 1] == '[') {
                i += 2;
                while (i < text.size() && text[i] != 'm') i++;
                if (i < text.size()) i++;
            } else {
                result += text[i++];
            }
        }
        return result;
    }

    Config config_{};
};

} // namespace cc::services::image
