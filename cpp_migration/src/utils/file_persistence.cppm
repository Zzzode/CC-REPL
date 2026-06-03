module;

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <expected>
#include <random>
#include <chrono>
#include <system_error>

export module cc.utils.file_persistence;

namespace fs = std::filesystem;

export namespace cc::utils {


inline bool ensure_parent_dir(const fs::path& filepath) {
    auto parent = filepath.parent_path();
    if (parent.empty()) return true;

    std::error_code ec;
    fs::create_directories(parent, ec);
    return !ec;
}


inline std::expected<void, std::string> atomic_write(
    const fs::path& filepath,
    std::string_view content
) {
    if (!ensure_parent_dir(filepath)) {
        return std::unexpected("Cannot create parent directory: " + filepath.parent_path().string());
    }


    auto parent = filepath.parent_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<uint64_t>(now));
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t random_val = dist(rng);

    std::string tmp_name = ".tmp_" + std::to_string(random_val);
    fs::path tmp_path = parent / tmp_name;


    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            return std::unexpected("Cannot create temp file: " + tmp_path.string());
        }
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        if (!ofs) {
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return std::unexpected("Write failed to temp file: " + tmp_path.string());
        }
    }


    std::error_code ec;
    fs::rename(tmp_path, filepath, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        return std::unexpected("Rename failed: " + ec.message());
    }

    return {};
}



inline std::expected<void, std::string> atomic_write_json(
    const fs::path& filepath,
    std::string_view json_content
) {

    std::string with_newline{json_content};
    if (!with_newline.empty() && with_newline.back() != '\n') {
        with_newline.push_back('\n');
    }
    return atomic_write(filepath, with_newline);
}


inline std::expected<void, std::string> safe_append(
    const fs::path& filepath,
    std::string_view content
) {
    if (!ensure_parent_dir(filepath)) {
        return std::unexpected("Cannot create parent directory: " + filepath.parent_path().string());
    }

    std::ofstream ofs(filepath, std::ios::binary | std::ios::app);
    if (!ofs.is_open()) {
        return std::unexpected("Cannot open file for append: " + filepath.string());
    }

    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.flush();

    if (!ofs) {
        return std::unexpected("Append write failed: " + filepath.string());
    }

    return {};
}

} // namespace cc::utils
