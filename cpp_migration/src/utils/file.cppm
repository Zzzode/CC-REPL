// C++23 File Utilities Module
// Provides enhanced file operations
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <array>

export module cc.utils.file;

import cc.utils.path;

export namespace cc::utils::file {

namespace fs = std::filesystem;
using path::expand_path;

// Forward declaration
[[nodiscard]] inline std::expected<std::uintmax_t, std::string> get_file_size(const fs::path& path);

// 读取整个文件为字符串
[[nodiscard]] inline std::expected<std::string, std::string> read_file(const fs::path& path) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::unexpected(std::format("Failed to open file: {}", path.string()));
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error reading file: {}", e.what()));
    }
}

// 读取文件，带有安全的大小限制
[[nodiscard]] inline std::expected<std::string, std::string> read_file_with_limit(const fs::path& path, std::uintmax_t max_size = 256 * 1024) {
    auto size_result = get_file_size(path);
    if (!size_result) {
        return std::unexpected(size_result.error());
    }
    
    if (*size_result > max_size) {
        return std::unexpected(std::format("File exceeds size limit: {} > {}", *size_result, max_size));
    }
    
    return read_file(path);
}

// 写入字符串到文件
[[nodiscard]] inline std::expected<void, std::string> write_file(const fs::path& path, std::string_view content) {
    try {
        // 确保父目录存在
        if (!path.parent_path().empty()) {
            fs::create_directories(path.parent_path());
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            return std::unexpected(std::format("Failed to open file for writing: {}", path.string()));
        }

        file << content;
        if (!file.good()) {
            return std::unexpected(std::format("Failed to write to file: {}", path.string()));
        }

        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error writing file: {}", e.what()));
    }
}

// 追加内容到文件
[[nodiscard]] inline std::expected<void, std::string> append_to_file(const fs::path& path, std::string_view content) {
    try {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::app);
        if (!file.is_open()) {
            return std::unexpected(std::format("Failed to open file for appending: {}", path.string()));
        }
        file << content;
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error appending to file: {}", e.what()));
    }
}

// 检查文件是否存在
[[nodiscard]] inline bool file_exists(const fs::path& path) {
    return fs::exists(path) && fs::is_regular_file(path);
}

// 检查目录是否存在
[[nodiscard]] inline bool directory_exists(const fs::path& path) {
    return fs::exists(path) && fs::is_directory(path);
}

// 检查路径是否存在（文件或目录）
[[nodiscard]] inline bool path_exists(const fs::path& path) {
    return fs::exists(path);
}

// 获取文件扩展名（包含点）
[[nodiscard]] inline std::string get_extension(const fs::path& path) {
    return path.extension().string();
}

// 获取无扩展名的文件名
[[nodiscard]] inline std::string get_basename(const fs::path& path) {
    return path.stem().string();
}

// 获取文件名（包含扩展名）
[[nodiscard]] inline std::string get_filename(const fs::path& path) {
    return path.filename().string();
}

// 获取父目录
[[nodiscard]] inline fs::path get_parent_dir(const fs::path& path) {
    return path.parent_path();
}

// 列出目录中的文件
[[nodiscard]] inline std::expected<std::vector<fs::path>, std::string> list_files(const fs::path& directory, bool recursive = false) {
    try {
        std::vector<fs::path> files;

        if (!directory_exists(directory)) {
            return std::unexpected(std::format("Directory not found: {}", directory.string()));
        }

        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path());
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path());
                }
            }
        }

        return files;
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error listing files: {}", e.what()));
    }
}

// 列出目录中的子目录
[[nodiscard]] inline std::expected<std::vector<fs::path>, std::string> list_directories(const fs::path& directory) {
    try {
        std::vector<fs::path> dirs;

        if (!directory_exists(directory)) {
            return std::unexpected(std::format("Directory not found: {}", directory.string()));
        }

        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_directory()) {
                dirs.push_back(entry.path());
            }
        }

        return dirs;
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error listing directories: {}", e.what()));
    }
}

// 删除文件
[[nodiscard]] inline std::expected<void, std::string> delete_file(const fs::path& path) {
    try {
        if (!file_exists(path)) {
            return std::unexpected(std::format("File not found: {}", path.string()));
        }

        fs::remove(path);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error deleting file: {}", e.what()));
    }
}

// 删除目录及其内容
[[nodiscard]] inline std::expected<void, std::string> delete_directory(const fs::path& path, bool recursive = false) {
    try {
        if (!directory_exists(path)) {
            return std::unexpected(std::format("Directory not found: {}", path.string()));
        }

        if (recursive) {
            fs::remove_all(path);
        } else {
            fs::remove(path);
        }
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error deleting directory: {}", e.what()));
    }
}

// 复制文件
[[nodiscard]] inline std::expected<void, std::string> copy_file(const fs::path& from, const fs::path& to) {
    try {
        fs::copy(from, to);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error copying file: {}", e.what()));
    }
}

// 移动/重命名文件
[[nodiscard]] inline std::expected<void, std::string> move_file(const fs::path& from, const fs::path& to) {
    try {
        fs::create_directories(to.parent_path());
        fs::rename(from, to);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error moving file: {}", e.what()));
    }
}

// 获取文件大小
[[nodiscard]] inline std::expected<std::uintmax_t, std::string> get_file_size(const fs::path& path) {
    try {
        return fs::file_size(path);
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error getting file size: {}", e.what()));
    }
}

// 获取文件修改时间
[[nodiscard]] inline std::expected<fs::file_time_type, std::string> get_file_modification_time(const fs::path& path) {
    try {
        return fs::last_write_time(path);
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error getting file modification time: {}", e.what()));
    }
}

// 触摸文件（更新最后修改时间或创建空文件）
[[nodiscard]] inline std::expected<void, std::string> touch_file(const fs::path& path) {
    try {
        if (!file_exists(path)) {
            // 创建空文件
            return write_file(path, "");
        } else {
            // 更新时间戳
            auto time = fs::file_time_type::clock::now();
            fs::last_write_time(path, time);
            return {};
        }
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error touching file: {}", e.what()));
    }
}

// 创建目录（包括必要的父目录）
[[nodiscard]] inline std::expected<void, std::string> create_directory(const fs::path& path) {
    try {
        fs::create_directories(path);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error creating directory: {}", e.what()));
    }
}

// 检查文件是否是二进制文件（基于内容检测）
[[nodiscard]] inline bool is_binary_file(const fs::path& path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        
        std::array<char, 4096> buffer;
        file.read(buffer.data(), buffer.size());
        std::streamsize bytes_read = file.gcount();
        
        // 如果有很多空字节，则认为是二进制文件
        int null_count = 0;
        for (std::streamsize i = 0; i < bytes_read; ++i) {
            if (buffer[i] == '\0') {
                null_count++;
                if (null_count > 4) return true;
            }
        }
        
        return false;
    } catch (...) {
        return false;
    }
}

// 读取文件的前几行
[[nodiscard]] inline std::expected<std::vector<std::string>, std::string> read_file_lines(const fs::path& path, std::size_t max_lines = 100) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::unexpected(std::format("Failed to open file: {}", path.string()));
        }
        
        std::vector<std::string> lines;
        std::string line;
        while (lines.size() < max_lines && std::getline(file, line)) {
            lines.push_back(std::move(line));
        }
        
        return lines;
    } catch (const std::exception& e) {
        return std::unexpected(std::format("Error reading file lines: {}", e.what()));
    }
}

// 带行号的内容格式化
[[nodiscard]] inline std::string add_line_numbers(const std::string& content, std::size_t start_line = 1) {
    std::istringstream iss(content);
    std::ostringstream oss;
    std::string line;
    std::size_t line_num = start_line;
    
    while (std::getline(iss, line)) {
        oss << std::format("{}\t{}\n", line_num++, line);
    }
    
    return oss.str();
}

// 检查目录是否为空
[[nodiscard]] inline bool is_directory_empty(const fs::path& path) {
    try {
        if (!directory_exists(path)) return true;
        return fs::is_empty(path);
    } catch (...) {
        return true;
    }
}

} // namespace cc::utils::file
