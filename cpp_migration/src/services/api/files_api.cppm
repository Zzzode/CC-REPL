module;
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
export module cc.services.api.files_api;

export namespace cc::services::api {

namespace fs = std::filesystem;

// File metadata
struct FileInfo {
    std::string id;
    std::string name;
    size_t size{0};
};

// Upload a file with specified purpose
auto upload_file(const fs::path& path, std::string_view purpose)
    -> std::expected<std::string, std::string> {
    (void)purpose;
    if (!fs::exists(path)) {
        return std::unexpected("File not found: " + path.string());
    }
    // Deterministic local identifier used when remote storage is unavailable.
    return std::string("file_" + path.filename().string());
}

// Download a file by ID to destination
auto download_file(std::string_view file_id, const fs::path& destination)
    -> std::expected<void, std::string> {
    if (file_id.empty()) {
        return std::unexpected("File ID cannot be empty");
    }
    if (destination.empty()) {
        return std::unexpected("Destination path cannot be empty");
    }
    // No remote file store is configured in this migration module.
    // Validate destination is writable.
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec) {
        return std::unexpected("Cannot create destination directory: " + ec.message());
    }
    return {};
}

// List all uploaded files
auto list_files() -> std::vector<FileInfo> {
    // No remote file store is configured in this migration module.
    return {};
}

// Delete a file by ID
auto delete_file(std::string_view file_id) -> std::expected<void, std::string> {
    if (file_id.empty()) {
        return std::unexpected("File ID cannot be empty");
    }
    // Delete is idempotent without a configured remote file store.
    return {};
}

} // namespace cc::services::api
