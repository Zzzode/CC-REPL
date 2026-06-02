module;

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>
#include <ranges>
#include <regex>
#include <system_error>

export module cc.utils.outputs_scanner;

namespace fs = std::filesystem;

export namespace cc::utils {

// 输出文件描述结构
struct OutputFile {
    fs::path file;
    std::string type;      // 基于扩展名推断的文件类型
    size_t size;
    std::chrono::system_clock::time_point created;
};

namespace detail {

// 根据文件扩展名推断文件类型
inline std::string infer_type(const fs::path& filepath) {
    auto ext = filepath.extension().string();
    if (ext == ".json") return "json";
    if (ext == ".log") return "log";
    if (ext == ".txt") return "text";
    if (ext == ".csv") return "csv";
    if (ext == ".html" || ext == ".htm") return "html";
    if (ext == ".xml") return "xml";
    if (ext == ".md") return "markdown";
    if (ext == ".yaml" || ext == ".yml") return "yaml";
    if (ext == ".png" || ext == ".jpg" || ext == ".gif") return "image";
    if (ext == ".pdf") return "pdf";
    return "unknown";
}

// 将 glob 风格的通配符模式转为正则表达式
inline std::regex glob_to_regex(std::string_view pattern) {
    std::string regex_str;
    for (char c : pattern) {
        switch (c) {
            case '*': regex_str += ".*"; break;
            case '?': regex_str += "."; break;
            case '.': regex_str += "\\."; break;
            default: regex_str.push_back(c); break;
        }
    }
    return std::regex(regex_str);
}

} // namespace detail

// 扫描目录中的输出文件，支持 glob 通配符过滤
inline std::vector<OutputFile> scan_outputs(
    const fs::path& dir,
    std::string_view pattern = "*"
) {
    std::vector<OutputFile> results;
    std::error_code ec;

    if (!fs::is_directory(dir, ec)) {
        return results;
    }

    auto regex_pattern = detail::glob_to_regex(pattern);

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        auto filename = entry.path().filename().string();
        if (!std::regex_match(filename, regex_pattern)) {
            continue;
        }

        auto ftime = fs::last_write_time(entry.path(), ec);
        std::chrono::system_clock::time_point created{};
        if (!ec) {
            auto d = ftime.time_since_epoch();
            created = std::chrono::system_clock::time_point{
                std::chrono::duration_cast<std::chrono::system_clock::duration>(d)};
        }

        auto file_size = entry.file_size(ec);
        if (ec) file_size = 0;

        results.push_back(OutputFile{
            .file = entry.path(),
            .type = detail::infer_type(entry.path()),
            .size = static_cast<size_t>(file_size),
            .created = created
        });
    }

    // 按创建时间倒序排列（最新的在前）
    std::ranges::sort(results, [](const OutputFile& a, const OutputFile& b) {
        return a.created > b.created;
    });

    return results;
}

// 获取目录中最新的输出文件
inline std::optional<OutputFile> get_latest_output(const fs::path& dir) {
    auto outputs = scan_outputs(dir);
    if (outputs.empty()) {
        return std::nullopt;
    }
    return outputs.front(); // 已按时间倒序排列
}

// 清理超过指定时间的旧输出文件，返回删除的文件数
inline size_t cleanup_old_outputs(const fs::path& dir, std::chrono::hours max_age) {
    size_t deleted_count = 0;
    std::error_code ec;

    if (!fs::is_directory(dir, ec)) {
        return 0;
    }

    auto now = std::chrono::system_clock::now();
    auto cutoff = now - max_age;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        auto ftime = fs::last_write_time(entry.path(), ec);
        if (ec) continue;

        auto file_time = std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                ftime.time_since_epoch())};
        if (file_time < cutoff) {
            if (fs::remove(entry.path(), ec)) {
                ++deleted_count;
            }
        }
    }

    return deleted_count;
}

} // namespace cc::utils
