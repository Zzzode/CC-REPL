module;
#include <string>
#include <string_view>
#include <map>

export module cc.utils.frontmatter_parser;

export namespace cc::utils {

// 前置元数据结构体
struct Frontmatter {
    std::map<std::string, std::string> metadata;
    std::string content;
};

// 检查文本是否包含 YAML frontmatter（以 "---" 开头）
[[nodiscard]] inline bool has_frontmatter(std::string_view sv) {
    // 必须以 "---" 开头（可选前导空白行）
    size_t pos = 0;
    while (pos < sv.size() && (sv[pos] == ' ' || sv[pos] == '\t' ||
           sv[pos] == '\r' || sv[pos] == '\n')) {
        ++pos;
    }
    return sv.substr(pos, 3) == "---" &&
           (pos + 3 >= sv.size() || sv[pos + 3] == '\n' || sv[pos + 3] == '\r');
}

namespace fm_detail {
    // 去除字符串首尾空白
    inline std::string_view trim(std::string_view sv) {
        auto start = sv.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) return {};
        auto end = sv.find_last_not_of(" \t\r\n");
        return sv.substr(start, end - start + 1);
    }

    // 去除引号
    inline std::string unquote(std::string_view sv) {
        if (sv.size() >= 2 &&
            ((sv.front() == '"' && sv.back() == '"') ||
             (sv.front() == '\'' && sv.back() == '\''))) {
            return std::string(sv.substr(1, sv.size() - 2));
        }
        return std::string(sv);
    }
}

// 解析 YAML frontmatter，返回元数据和内容
[[nodiscard]] inline Frontmatter parse_frontmatter(std::string_view sv) {
    Frontmatter result;

    if (!has_frontmatter(sv)) {
        result.content = std::string(sv);
        return result;
    }

    // 找到首个 "---" 的结束位置
    auto first_sep = sv.find("---");
    if (first_sep == std::string_view::npos) {
        result.content = std::string(sv);
        return result;
    }

    // 跳过首个 "---\n"
    size_t yaml_start = first_sep + 3;
    if (yaml_start < sv.size() && sv[yaml_start] == '\r') ++yaml_start;
    if (yaml_start < sv.size() && sv[yaml_start] == '\n') ++yaml_start;

    // 查找结束 "---"
    auto second_sep = sv.find("\n---", yaml_start);
    if (second_sep == std::string_view::npos) {
        // 也尝试 \r\n---
        second_sep = sv.find("\r\n---", yaml_start);
        if (second_sep == std::string_view::npos) {
            result.content = std::string(sv);
            return result;
        }
    }

    // 提取 YAML 部分
    auto yaml_section = sv.substr(yaml_start, second_sep - yaml_start);

    // 简单的 key: value 解析
    size_t line_start = 0;
    while (line_start < yaml_section.size()) {
        auto line_end = yaml_section.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = yaml_section.size();

        auto line = yaml_section.substr(line_start, line_end - line_start);
        // 去除可能的 \r
        if (!line.empty() && line.back() == '\r') line = line.substr(0, line.size() - 1);

        auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            auto key = fm_detail::trim(line.substr(0, colon));
            auto value = fm_detail::trim(line.substr(colon + 1));
            if (!key.empty()) {
                result.metadata[std::string(key)] = fm_detail::unquote(value);
            }
        }
        line_start = line_end + 1;
    }

    // 提取内容（在第二个 "---" 之后）
    size_t content_start = second_sep + 4; // "\n---"
    if (content_start < sv.size() && sv[content_start] == '\r') ++content_start;
    if (content_start < sv.size() && sv[content_start] == '\n') ++content_start;
    result.content = std::string(sv.substr(content_start));

    return result;
}

// 去除 frontmatter，仅返回正文内容
[[nodiscard]] inline std::string_view strip_frontmatter(std::string_view sv) {
    if (!has_frontmatter(sv)) return sv;

    auto first_sep = sv.find("---");
    if (first_sep == std::string_view::npos) return sv;

    size_t search_start = first_sep + 3;
    auto second_sep = sv.find("\n---", search_start);
    if (second_sep == std::string_view::npos) {
        second_sep = sv.find("\r\n---", search_start);
        if (second_sep == std::string_view::npos) return sv;
    }

    size_t content_start = second_sep + 4;
    if (content_start < sv.size() && sv[content_start] == '\r') ++content_start;
    if (content_start < sv.size() && sv[content_start] == '\n') ++content_start;
    return sv.substr(content_start);
}

} // namespace cc::utils
