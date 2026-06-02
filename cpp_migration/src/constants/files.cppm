/// @file files.cppm
/// @brief Binary file extension detection and classification.
/// Migrated from src/constants/files.ts
module;

#include <string>
#include <string_view>
#include <unordered_set>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <span>

export module cc.constants.files;

export namespace cc::constants::files {

/// Binary file extensions to skip for text-based operations
[[nodiscard]] inline const std::unordered_set<std::string_view>& binary_extensions() {
    static const std::unordered_set<std::string_view> exts = {
        // Images
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp", ".tiff", ".tif",
        // Videos
        ".mp4", ".mov", ".avi", ".mkv", ".webm", ".wmv", ".flv", ".m4v", ".mpeg", ".mpg",
        // Audio
        ".mp3", ".wav", ".ogg", ".flac", ".aac", ".m4a", ".wma", ".aiff", ".opus",
        // Archives
        ".zip", ".tar", ".gz", ".bz2", ".7z", ".rar", ".xz", ".z", ".tgz", ".iso",
        // Executables/binaries
        ".exe", ".dll", ".so", ".dylib", ".bin", ".o", ".a", ".obj", ".lib",
        ".app", ".msi", ".deb", ".rpm",
        // Documents
        ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp",
        // Fonts
        ".ttf", ".otf", ".woff", ".woff2", ".eot",
        // Bytecode/VM
        ".pyc", ".pyo", ".class", ".jar", ".war", ".ear", ".node", ".wasm", ".rlib",
        // Database
        ".sqlite", ".sqlite3", ".db", ".mdb", ".idx",
        // Design/3D
        ".psd", ".ai", ".eps", ".sketch", ".fig", ".xd", ".blend", ".3ds", ".max",
        // Flash
        ".swf", ".fla",
        // Lock/profiling
        ".lockb", ".dat", ".data",
    };
    return exts;
}

/// Check if a file path has a binary extension
[[nodiscard]] inline bool has_binary_extension(std::string_view file_path) {
    auto dot_pos = file_path.rfind('.');
    if (dot_pos == std::string_view::npos) return false;
    
    std::string ext(file_path.substr(dot_pos));
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return binary_extensions().contains(ext);
}

/// Number of bytes to read for binary content detection
inline constexpr std::size_t BINARY_CHECK_SIZE = 8192;

/// Check if a buffer contains binary content
[[nodiscard]] inline bool is_binary_content(std::span<const std::uint8_t> buffer) {
    auto check_size = std::min(buffer.size(), BINARY_CHECK_SIZE);
    std::size_t non_printable = 0;
    
    for (std::size_t i = 0; i < check_size; ++i) {
        auto byte = buffer[i];
        // Null byte is a strong binary indicator
        if (byte == 0) return true;
        // Count non-printable, non-whitespace bytes
        if (byte < 32 && byte != 9 && byte != 10 && byte != 13) {
            non_printable++;
        }
    }
    
    // >10% non-printable means likely binary
    return static_cast<double>(non_printable) / static_cast<double>(check_size) > 0.1;
}

} // namespace cc::constants::files
