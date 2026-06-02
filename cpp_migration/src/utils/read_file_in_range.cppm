module;

#include <expected>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

export module cc.utils.read_file_in_range;

export namespace cc::utils::read_file_in_range {

struct ReadFileRangeResult {
    std::string content;
    std::size_t line_count = 0;
    std::size_t total_lines = 0;
    std::size_t total_bytes = 0;
    std::size_t read_bytes = 0;
    double mtime_ms = 0;
    bool truncated_by_bytes = false;
};

enum class ReadFileRangeErrorKind {
    IoError,
    IsDirectory,
    FileTooLarge,
};

struct ReadFileRangeError {
    ReadFileRangeErrorKind kind = ReadFileRangeErrorKind::IoError;
    std::string message;
    std::size_t size_in_bytes = 0;
    std::size_t max_size_bytes = 0;
};

using ReadFileRangeExpected = std::expected<ReadFileRangeResult, ReadFileRangeError>;

namespace detail {
    [[nodiscard]] inline std::size_t byte_length(std::string_view value) noexcept { return value.size(); }

    [[nodiscard]] inline std::string format_file_size(std::size_t size) {
        return std::to_string(size) + " B";
    }

    [[nodiscard]] inline double file_mtime_ms(const std::filesystem::path& path) {
        std::error_code ec;
        const auto ft = std::filesystem::last_write_time(path, ec);
        if (ec) return 0;
        const auto system_time = std::chrono::system_clock::now() + (ft - std::filesystem::file_time_type::clock::now());
        return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(system_time.time_since_epoch()).count());
    }

    [[nodiscard]] inline ReadFileRangeError file_too_large(std::size_t size, std::size_t max) {
        return {
            .kind = ReadFileRangeErrorKind::FileTooLarge,
            .message = "File content (" + format_file_size(size) + ") exceeds maximum allowed size (" + format_file_size(max) + "). Use offset and limit parameters to read specific portions of the file, or search for specific content instead of reading the whole file.",
            .size_in_bytes = size,
            .max_size_bytes = max,
        };
    }

    [[nodiscard]] inline ReadFileRangeResult read_fast(
        std::string raw,
        double mtime_ms,
        std::size_t offset,
        std::optional<std::size_t> max_lines,
        std::optional<std::size_t> truncate_at_bytes
    ) {
        const std::size_t end_line = max_lines ? offset + *max_lines : static_cast<std::size_t>(-1);
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
            raw.erase(0, 3);
        }

        std::vector<std::string> selected_lines;
        std::size_t line_index = 0;
        std::size_t start_pos = 0;
        std::size_t selected_bytes = 0;
        bool truncated = false;

        auto try_push = [&](std::string line) {
            if (truncate_at_bytes) {
                const std::size_t sep = selected_lines.empty() ? 0 : 1;
                const std::size_t next = selected_bytes + sep + byte_length(line);
                if (next > *truncate_at_bytes) {
                    truncated = true;
                    return false;
                }
                selected_bytes = next;
            }
            selected_lines.push_back(std::move(line));
            return true;
        };

        while (true) {
            const std::size_t newline = raw.find('\n', start_pos);
            if (newline == std::string::npos) break;
            if (line_index >= offset && line_index < end_line && !truncated) {
                std::string line = raw.substr(start_pos, newline - start_pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                try_push(std::move(line));
            }
            ++line_index;
            start_pos = newline + 1;
        }

        if (line_index >= offset && line_index < end_line && !truncated) {
            std::string line = raw.substr(start_pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            try_push(std::move(line));
        }
        ++line_index;

        std::string content;
        for (std::size_t i = 0; i < selected_lines.size(); ++i) {
            if (i) content.push_back('\n');
            content += selected_lines[i];
        }
        return {
            .content = content,
            .line_count = selected_lines.size(),
            .total_lines = line_index,
            .total_bytes = byte_length(raw),
            .read_bytes = byte_length(content),
            .mtime_ms = mtime_ms,
            .truncated_by_bytes = truncated,
        };
    }

    [[nodiscard]] inline ReadFileRangeResult read_streaming(
        std::istream& input,
        double mtime_ms,
        std::size_t offset,
        std::optional<std::size_t> max_lines,
        std::optional<std::size_t> max_bytes,
        bool truncate_on_byte_limit
    ) {
        const std::size_t end_line_initial = max_lines ? offset + *max_lines : static_cast<std::size_t>(-1);
        std::size_t end_line = end_line_initial;
        std::vector<std::string> selected_lines;
        std::string partial;
        std::size_t current_line = 0;
        std::size_t total_bytes = 0;
        std::size_t selected_bytes = 0;
        bool truncated = false;
        bool first_chunk = true;

        auto try_push = [&](std::string line) {
            if (truncate_on_byte_limit && max_bytes) {
                const std::size_t sep = selected_lines.empty() ? 0 : 1;
                const std::size_t next = selected_bytes + sep + byte_length(line);
                if (next > *max_bytes) {
                    truncated = true;
                    return false;
                }
                selected_bytes = next;
            }
            selected_lines.push_back(std::move(line));
            return true;
        };

        std::string chunk(512 * 1024, '\0');
        while (input) {
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = input.gcount();
            if (got <= 0) break;
            std::string data(chunk.data(), static_cast<std::size_t>(got));
            if (first_chunk) {
                first_chunk = false;
                if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
                    data.erase(0, 3);
                }
            }
            total_bytes += data.size();

            if (!truncate_on_byte_limit && max_bytes && total_bytes > *max_bytes) {
                break;
            }

            if (!partial.empty()) {
                data.insert(0, partial);
                partial.clear();
            }
            std::size_t start = 0;
            while (true) {
                const std::size_t newline = data.find('\n', start);
                if (newline == std::string::npos) break;
                if (current_line >= offset && current_line < end_line) {
                    std::string line = data.substr(start, newline - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!try_push(std::move(line))) end_line = current_line;
                }
                ++current_line;
                start = newline + 1;
            }
            if (start < data.size() && current_line >= offset && current_line < end_line) {
                const std::string fragment = data.substr(start);
                if (truncate_on_byte_limit && max_bytes) {
                    const std::size_t sep = selected_lines.empty() ? 0 : 1;
                    if (selected_bytes + sep + fragment.size() > *max_bytes) {
                        truncated = true;
                        end_line = current_line;
                        continue;
                    }
                }
                partial = fragment;
            }
        }

        if (current_line >= offset && current_line < end_line) {
            if (!partial.empty() && partial.back() == '\r') partial.pop_back();
            try_push(std::move(partial));
        }
        ++current_line;

        std::string content;
        for (std::size_t i = 0; i < selected_lines.size(); ++i) {
            if (i) content.push_back('\n');
            content += selected_lines[i];
        }
        return {
            .content = content,
            .line_count = selected_lines.size(),
            .total_lines = current_line,
            .total_bytes = total_bytes,
            .read_bytes = byte_length(content),
            .mtime_ms = mtime_ms,
            .truncated_by_bytes = truncated,
        };
    }
} // namespace detail

[[nodiscard]] inline ReadFileRangeExpected read_file_in_range(
    const std::string& file_path,
    std::size_t offset = 0,
    std::optional<std::size_t> max_lines = std::nullopt,
    std::optional<std::size_t> max_bytes = std::nullopt,
    bool truncate_on_byte_limit = false
) {
    std::error_code ec;
    const std::filesystem::path path(file_path);
    auto status = std::filesystem::status(path, ec);
    if (ec) return std::unexpected(ReadFileRangeError{.kind = ReadFileRangeErrorKind::IoError, .message = ec.message()});
    if (std::filesystem::is_directory(status)) {
        return std::unexpected(ReadFileRangeError{.kind = ReadFileRangeErrorKind::IsDirectory, .message = "EISDIR: illegal operation on a directory, read '" + file_path + "'"});
    }

    const bool regular = std::filesystem::is_regular_file(status);
    constexpr std::uintmax_t fast_path_max_size = 10 * 1024 * 1024;
    std::uintmax_t file_size = 0;
    if (regular) {
        file_size = std::filesystem::file_size(path, ec);
        if (ec) return std::unexpected(ReadFileRangeError{.kind = ReadFileRangeErrorKind::IoError, .message = ec.message()});
        if (!truncate_on_byte_limit && max_bytes && file_size > *max_bytes) {
            return std::unexpected(detail::file_too_large(static_cast<std::size_t>(file_size), *max_bytes));
        }
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(ReadFileRangeError{.kind = ReadFileRangeErrorKind::IoError, .message = "failed to open file"});
    const double mtime_ms = detail::file_mtime_ms(path);

    if (!regular || file_size >= fast_path_max_size) {
        auto result = detail::read_streaming(input, mtime_ms, offset, max_lines, max_bytes, truncate_on_byte_limit);
        if (!truncate_on_byte_limit && max_bytes && result.total_bytes > *max_bytes) {
            return std::unexpected(detail::file_too_large(result.total_bytes, *max_bytes));
        }
        return result;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    return detail::read_fast(buffer.str(), mtime_ms, offset, max_lines, truncate_on_byte_limit ? max_bytes : std::nullopt);
}

} // namespace cc::utils::read_file_in_range
