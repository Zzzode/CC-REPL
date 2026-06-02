module;
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.image_validation;

export namespace cc::utils {

namespace fs = std::filesystem;

enum class ImageFormat {
    PNG,
    JPEG,
    GIF,
    WebP,
    Unknown
};

// Detect image format from magic bytes
ImageFormat detect_image_format(std::span<uint8_t> data) {
    if (data.size() < 4) return ImageFormat::Unknown;

    // PNG: 89 50 4E 47
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        return ImageFormat::PNG;
    }
    // JPEG: FF D8 FF
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return ImageFormat::JPEG;
    }
    // GIF: 47 49 46
    if (data[0] == 0x47 && data[1] == 0x49 && data[2] == 0x46) {
        return ImageFormat::GIF;
    }
    // WebP: RIFF....WEBP
    if (data.size() >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return ImageFormat::WebP;
    }

    return ImageFormat::Unknown;
}

// Check if a file path points to a supported image
bool is_supported_image(fs::path filepath) {
    if (!fs::exists(filepath) || !fs::is_regular_file(filepath)) return false;

    // Check extension first
    auto ext = filepath.extension().string();
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
        ext == ".gif" || ext == ".webp") {
        return true;
    }

    // Check magic bytes
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    std::vector<uint8_t> header(12);
    file.read(reinterpret_cast<char*>(header.data()), 12);
    auto bytes_read = file.gcount();
    if (bytes_read < 4) return false;

    auto format = detect_image_format(std::span<uint8_t>(header.data(), bytes_read));
    return format != ImageFormat::Unknown;
}

// Validate an image for API submission
std::expected<void, std::string> validate_image_for_api(fs::path filepath) {
    if (!fs::exists(filepath)) {
        return std::unexpected("File not found: " + filepath.string());
    }

    auto file_size = fs::file_size(filepath);

    // Maximum size: 20MB for API
    constexpr std::size_t max_size = 20 * 1024 * 1024;
    if (file_size > max_size) {
        return std::unexpected("Image too large: " + std::to_string(file_size / 1024 / 1024) +
                             "MB (max 20MB)");
    }

    // Check format
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("Cannot open file: " + filepath.string());
    }

    std::vector<uint8_t> header(12);
    file.read(reinterpret_cast<char*>(header.data()), 12);
    auto format = detect_image_format(std::span<uint8_t>(header.data(), file.gcount()));

    if (format == ImageFormat::Unknown) {
        return std::unexpected("Unsupported image format. Supported: PNG, JPEG, GIF, WebP");
    }

    return {};
}

// Get MIME type for an image format
std::string get_mime_type(ImageFormat format) {
    switch (format) {
        case ImageFormat::PNG:  return "image/png";
        case ImageFormat::JPEG: return "image/jpeg";
        case ImageFormat::GIF:  return "image/gif";
        case ImageFormat::WebP: return "image/webp";
        case ImageFormat::Unknown: return "application/octet-stream";
    }
    return "application/octet-stream";
}

} // namespace cc::utils
