module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.format;


export namespace cc::utils {

// ANSI 颜色
namespace ansi {
    inline auto bold(std::string_view text) -> std::string { return "\033[1m" + std::string(text) + "\033[22m"; }
    inline auto dim(std::string_view text) -> std::string { return "\033[2m" + std::string(text) + "\033[22m"; }
    inline auto italic(std::string_view text) -> std::string { return "\033[3m" + std::string(text) + "\033[23m"; }
    inline auto underline(std::string_view text) -> std::string { return "\033[4m" + std::string(text) + "\033[24m"; }
    inline auto strikethrough(std::string_view text) -> std::string { return "\033[9m" + std::string(text) + "\033[29m"; }
    
    // 前景色
    inline auto red(std::string_view text) -> std::string { return "\033[31m" + std::string(text) + "\033[39m"; }
    inline auto green(std::string_view text) -> std::string { return "\033[32m" + std::string(text) + "\033[39m"; }
    inline auto yellow(std::string_view text) -> std::string { return "\033[33m" + std::string(text) + "\033[39m"; }
    inline auto blue(std::string_view text) -> std::string { return "\033[34m" + std::string(text) + "\033[39m"; }
    inline auto magenta(std::string_view text) -> std::string { return "\033[35m" + std::string(text) + "\033[39m"; }
    inline auto cyan(std::string_view text) -> std::string { return "\033[36m" + std::string(text) + "\033[39m"; }
    inline auto gray(std::string_view text) -> std::string { return "\033[90m" + std::string(text) + "\033[39m"; }
    
    // RGB 前景色
    inline auto rgb(std::string_view text, uint8_t r, uint8_t g, uint8_t b) -> std::string {
        return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" +
               std::to_string(b) + "m" + std::string(text) + "\033[39m";
    }
}

// 格式化数字 (1234 -> "1.2k", 1234567 -> "1.2M")
[[nodiscard]] inline auto format_number(int64_t n) -> std::string {
    if (n < 1000) return std::to_string(n);
    if (n < 1'000'000) {
        double v = static_cast<double>(n) / 1000.0;
        std::ostringstream oss; oss << std::fixed << std::setprecision(1) << v << "k";
        return oss.str();
    }
    double v = static_cast<double>(n) / 1'000'000.0;
    std::ostringstream oss; oss << std::fixed << std::setprecision(1) << v << "M";
    return oss.str();
}

// 格式化时长 (90s -> "1m 30s")
[[nodiscard]] inline auto format_duration(std::chrono::seconds duration) -> std::string {
    auto secs = duration.count();
    if (secs < 60) return std::to_string(secs) + "s";
    if (secs < 3600) return std::to_string(secs / 60) + "m " + std::to_string(secs % 60) + "s";
    return std::to_string(secs / 3600) + "h " + std::to_string((secs % 3600) / 60) + "m";
}

// 格式化文件大小
[[nodiscard]] inline auto format_bytes(size_t bytes) -> std::string {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) {
        std::ostringstream oss; oss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
        return oss.str();
    }
    if (bytes < 1024ULL * 1024 * 1024) {
        std::ostringstream oss; oss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) << " MB";
        return oss.str();
    }
    std::ostringstream oss; oss << std::fixed << std::setprecision(2) << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
    return oss.str();
}

// 格式化表格
[[nodiscard]] inline auto format_table(const std::vector<std::string>& headers,
    const std::vector<std::vector<std::string>>& rows) -> std::string {
    if (headers.empty()) return "";
    
    // 计算列宽
    std::vector<size_t> widths(headers.size());
    for (size_t i = 0; i < headers.size(); ++i) widths[i] = headers[i].size();
    for (const auto& row : rows) {
        for (size_t i = 0; i < std::min(row.size(), widths.size()); ++i)
            widths[i] = std::max(widths[i], row[i].size());
    }
    
    std::string result;
    // 表头
    for (size_t i = 0; i < headers.size(); ++i) {
        result += headers[i];
        if (i < headers.size() - 1)
            result += std::string(widths[i] - headers[i].size() + 2, ' ');
    }
    result += "\n";
    // 分隔线
    for (size_t i = 0; i < headers.size(); ++i) {
        result += std::string(widths[i], '-');
        if (i < headers.size() - 1) result += "  ";
    }
    result += "\n";
    // 数据行
    for (const auto& row : rows) {
        for (size_t i = 0; i < std::min(row.size(), widths.size()); ++i) {
            result += row[i];
            if (i < widths.size() - 1)
                result += std::string(widths[i] - row[i].size() + 2, ' ');
        }
        result += "\n";
    }
    return result;
}

// 去除 ANSI 转义序列
[[nodiscard]] inline auto strip_ansi(std::string_view text) -> std::string {
    std::string result;
    result.reserve(text.size());
    bool in_escape = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\033') { in_escape = true; continue; }
        if (in_escape) {
            if ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))
                in_escape = false;
            continue;
        }
        result += text[i];
    }
    return result;
}

// 截断并添加省略号
[[nodiscard]] inline auto truncate_with_ellipsis(std::string_view text, size_t max_width) -> std::string {
    if (text.size() <= max_width) return std::string(text);
    if (max_width < 4) return std::string(text.substr(0, max_width));
    return std::string(text.substr(0, max_width - 3)) + "...";
}

} // namespace cc::utils
