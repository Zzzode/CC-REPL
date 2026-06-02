module;

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <span>
#include <cstdint>
#include <algorithm>
#include <sstream>

export module cc.utils.file_read;

namespace fs = std::filesystem;

export namespace cc::utils {

// 读取整个文件内容
inline std::expected<std::string, std::string> read_file(const fs::path& filepath) {
    std::error_code ec;
    if (!fs::exists(filepath, ec)) {
        return std::unexpected("File not found: " + filepath.string());
    }

    std::ifstream ifs(filepath, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        return std::unexpected("Cannot open file: " + filepath.string());
    }

    auto size = ifs.tellg();
    if (size < 0) {
        return std::unexpected("Cannot determine file size: " + filepath.string());
    }

    std::string content(static_cast<size_t>(size), '\0');
    ifs.seekg(0);
    ifs.read(content.data(), size);

    if (!ifs) {
        return std::unexpected("Read error: " + filepath.string());
    }

    return content;
}

// 按行读取文件
inline std::expected<std::vector<std::string>, std::string> read_file_lines(const fs::path& filepath) {
    auto content = read_file(filepath);
    if (!content) {
        return std::unexpected(content.error());
    }

    std::vector<std::string> lines;
    std::istringstream stream(*content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

// 读取文件指定行范围 [start_line, end_line]（1-based）
inline std::expected<std::string, std::string> read_file_range(
    const fs::path& filepath,
    size_t start_line,
    size_t end_line
) {
    if (start_line == 0 || end_line < start_line) {
        return std::unexpected("Invalid line range");
    }

    auto lines_result = read_file_lines(filepath);
    if (!lines_result) {
        return std::unexpected(lines_result.error());
    }

    const auto& lines = *lines_result;
    if (start_line > lines.size()) {
        return std::unexpected("Start line exceeds file length");
    }

    // 调整 end_line 不超过文件总行数
    size_t actual_end = std::min(end_line, lines.size());
    std::string result;

    for (size_t i = start_line - 1; i < actual_end; ++i) {
        result += lines[i];
        if (i + 1 < actual_end) {
            result += '\n';
        }
    }
    return result;
}

// 检测字节序列的编码类型
inline std::string detect_encoding(std::span<const uint8_t> data) {
    if (data.empty()) {
        return "utf-8";
    }

    // 检查 BOM (Byte Order Mark)
    if (data.size() >= 3 &&
        data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        return "utf-8-bom";
    }
    if (data.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        return "utf-16-be";
    }
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        return "utf-16-le";
    }

    // 尝试验证 UTF-8
    size_t i = 0;
    bool valid_utf8 = true;
    while (i < data.size() && valid_utf8) {
        uint8_t byte = data[i];
        size_t seq_len = 0;

        if (byte <= 0x7F) {
            seq_len = 1;
        } else if ((byte & 0xE0) == 0xC0) {
            seq_len = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            seq_len = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            seq_len = 4;
        } else {
            valid_utf8 = false;
            break;
        }

        // 检查后续字节
        if (i + seq_len > data.size()) {
            valid_utf8 = false;
            break;
        }
        for (size_t j = 1; j < seq_len; ++j) {
            if ((data[i + j] & 0xC0) != 0x80) {
                valid_utf8 = false;
                break;
            }
        }
        i += seq_len;
    }

    if (valid_utf8) {
        return "utf-8";
    }

    // 非有效 UTF-8，假设为 Latin-1
    return "latin-1";
}

// 判断文件是否为二进制文件（检测前 8KB 是否有 NUL 字节）
inline bool is_binary_file(const fs::path& filepath) {
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    constexpr size_t check_size = 8192;
    std::vector<uint8_t> buffer(check_size);
    ifs.read(reinterpret_cast<char*>(buffer.data()), check_size);
    auto bytes_read = static_cast<size_t>(ifs.gcount());
    buffer.resize(bytes_read);

    // 存在 NUL 字节则判定为二进制
    return std::ranges::any_of(buffer, [](uint8_t b) { return b == 0; });
}

// 带大小限制的文件读取
inline std::expected<std::string, std::string> read_file_with_limit(
    const fs::path& filepath,
    size_t max_bytes
) {
    std::error_code ec;
    if (!fs::exists(filepath, ec)) {
        return std::unexpected("File not found: " + filepath.string());
    }

    auto file_size = fs::file_size(filepath, ec);
    if (ec) {
        return std::unexpected("Cannot get file size: " + filepath.string());
    }

    if (file_size > max_bytes) {
        return std::unexpected(
            "File too large: " + std::to_string(file_size) +
            " bytes (limit: " + std::to_string(max_bytes) + ")"
        );
    }

    return read_file(filepath);
}

} // namespace cc::utils
