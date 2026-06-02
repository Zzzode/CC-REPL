module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <chrono>

export module cc.utils.image_store;

export namespace cc::utils::image_store {

struct StoredImage {
    std::string id;
    std::string path;
    std::string mime_type;
    uint32_t width;
    uint32_t height;
    size_t size_bytes;
};

struct PasteResult {
    bool success;
    std::optional<std::string> image_id;
    std::optional<std::string> error;
};

namespace detail {

struct ImageRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, StoredImage> images;
    uint64_t next_id{1};
};

inline auto get_registry() -> ImageRegistry& {
    static ImageRegistry reg;
    return reg;
}

inline auto get_store_dir() -> std::filesystem::path {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::filesystem::path(home) / ".cc-repl" / "images";
}

inline auto generate_id() -> std::string {
    auto& reg = get_registry();
    return "img-" + std::to_string(reg.next_id++);
}

inline auto detect_mime(std::string_view data) -> std::string {
    if (data.size() >= 8) {
        // PNG signature
        if (data[0] == '\x89' && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
            return "image/png";
        }
        // JPEG signature
        if (static_cast<unsigned char>(data[0]) == 0xFF &&
            static_cast<unsigned char>(data[1]) == 0xD8) {
            return "image/jpeg";
        }
        // GIF signature
        if (data.starts_with("GIF8")) {
            return "image/gif";
        }
        // WebP signature
        if (data.size() >= 12 && data.substr(0, 4) == "RIFF" && data.substr(8, 4) == "WEBP") {
            return "image/webp";
        }
    }
    return "application/octet-stream";
}

} // namespace detail

inline std::expected<PasteResult, std::string> handle_image_paste(std::string_view clipboard_data) {
    if (clipboard_data.empty()) {
        return PasteResult{false, std::nullopt, "Empty clipboard data"};
    }

    namespace fs = std::filesystem;
    auto store_dir = detail::get_store_dir();
    std::error_code ec;
    fs::create_directories(store_dir, ec);
    if (ec) {
        return PasteResult{false, std::nullopt, "Failed to create image store: " + ec.message()};
    }

    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);

    auto id = detail::generate_id();
    auto mime = detail::detect_mime(clipboard_data);

    std::string ext = ".bin";
    if (mime == "image/png") ext = ".png";
    else if (mime == "image/jpeg") ext = ".jpg";
    else if (mime == "image/gif") ext = ".gif";
    else if (mime == "image/webp") ext = ".webp";

    auto filepath = store_dir / (id + ext);
    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs) {
        return PasteResult{false, std::nullopt, "Failed to write image file"};
    }
    ofs.write(clipboard_data.data(), static_cast<std::streamsize>(clipboard_data.size()));
    ofs.close();

    StoredImage img{
        .id = id,
        .path = filepath.string(),
        .mime_type = mime,
        .width = 0,
        .height = 0,
        .size_bytes = clipboard_data.size(),
    };
    reg.images[id] = img;

    return PasteResult{true, id, std::nullopt};
}

inline std::expected<StoredImage, std::string> store_image(std::string_view path) {
    namespace fs = std::filesystem;
    if (!fs::exists(path)) {
        return std::unexpected("File not found: " + std::string(path));
    }

    auto file_size = fs::file_size(path);
    std::ifstream ifs(std::string(path), std::ios::binary);
    if (!ifs) {
        return std::unexpected("Cannot read file: " + std::string(path));
    }

    // Read first few bytes for MIME detection
    std::string header(16, '\0');
    ifs.read(header.data(), 16);
    ifs.seekg(0);

    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);

    auto id = detail::generate_id();
    auto store_dir = detail::get_store_dir();
    std::error_code ec;
    fs::create_directories(store_dir, ec);

    auto ext = fs::path(std::string(path)).extension().string();
    auto dest = store_dir / (id + ext);
    fs::copy_file(path, dest, ec);
    if (ec) {
        return std::unexpected("Failed to copy image: " + ec.message());
    }

    StoredImage img{
        .id = id,
        .path = dest.string(),
        .mime_type = detail::detect_mime(header),
        .width = 0,
        .height = 0,
        .size_bytes = static_cast<size_t>(file_size),
    };
    reg.images[id] = img;
    return img;
}

inline std::optional<StoredImage> get_image(std::string_view id) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    auto it = reg.images.find(std::string(id));
    if (it == reg.images.end()) return std::nullopt;
    return it->second;
}

inline std::expected<void, std::string> delete_image(std::string_view id) {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    auto it = reg.images.find(std::string(id));
    if (it == reg.images.end()) {
        return std::unexpected("Image not found: " + std::string(id));
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(it->second.path, ec);
    reg.images.erase(it);
    return {};
}

inline std::vector<StoredImage> list_images() {
    auto& reg = detail::get_registry();
    std::lock_guard lock(reg.mutex);
    std::vector<StoredImage> result;
    result.reserve(reg.images.size());
    for (const auto& [_, img] : reg.images) {
        result.push_back(img);
    }
    return result;
}

} // namespace cc::utils::image_store
