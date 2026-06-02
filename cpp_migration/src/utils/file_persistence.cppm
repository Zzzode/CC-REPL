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

// 确保父目录存在
inline bool ensure_parent_dir(const fs::path& filepath) {
    auto parent = filepath.parent_path();
    if (parent.empty()) return true;

    std::error_code ec;
    fs::create_directories(parent, ec);
    return !ec;
}

// 原子写入：先写入临时文件再 rename，确保写入的原子性和持久性
inline std::expected<void, std::string> atomic_write(
    const fs::path& filepath,
    std::string_view content
) {
    if (!ensure_parent_dir(filepath)) {
        return std::unexpected("Cannot create parent directory: " + filepath.parent_path().string());
    }

    // 生成同目录下的临时文件名
    auto parent = filepath.parent_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<uint64_t>(now));
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t random_val = dist(rng);

    std::string tmp_name = ".tmp_" + std::to_string(random_val);
    fs::path tmp_path = parent / tmp_name;

    // 写入临时文件
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

    // 原子 rename
    std::error_code ec;
    fs::rename(tmp_path, filepath, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        return std::unexpected("Rename failed: " + ec.message());
    }

    return {};
}

// 原子写入 JSON 内容（接收已序列化的 JSON 字符串）
// 注：此处使用 string_view 表示 JSON 值，因为 C++23 标准库没有内置 JSON 类型
inline std::expected<void, std::string> atomic_write_json(
    const fs::path& filepath,
    std::string_view json_content
) {
    // JSON 文件写入时追加换行符
    std::string with_newline{json_content};
    if (!with_newline.empty() && with_newline.back() != '\n') {
        with_newline.push_back('\n');
    }
    return atomic_write(filepath, with_newline);
}

// 安全追加：以 append 模式写入，带错误处理
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
