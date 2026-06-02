module;
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <sstream>

export module cc.utils.xml_utils;

export namespace cc::utils {

// XML 元素结构体
struct XmlElement {
    std::string tag;
    std::string content;
    std::map<std::string, std::string> attributes;
    std::vector<XmlElement> children;
};

// 对 XML 特殊字符进行转义
[[nodiscard]] inline std::string xml_escape(std::string_view sv) {
    std::string result;
    result.reserve(sv.size() + sv.size() / 8); // 预估额外空间
    for (char c : sv) {
        switch (c) {
            case '&':  result += "&"; break;
            case '<':  result += "<"; break;
            case '>':  result += ">"; break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:   result += c; break;
        }
    }
    return result;
}

// 反转义 XML 实体
[[nodiscard]] inline std::string xml_unescape(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    size_t i = 0;
    while (i < sv.size()) {
        if (sv[i] == '&') {
            // 检查已知实体
            if (sv.substr(i, 4) == "<") { result += '<'; i += 4; }
            else if (sv.substr(i, 4) == ">") { result += '>'; i += 4; }
            else if (sv.substr(i, 5) == "&") { result += '&'; i += 5; }
            else if (sv.substr(i, 6) == "&quot;") { result += '"'; i += 6; }
            else if (sv.substr(i, 6) == "&apos;") { result += '\''; i += 6; }
            else { result += sv[i]; ++i; }
        } else {
            result += sv[i];
            ++i;
        }
    }
    return result;
}

// 创建 XML 标签（含属性和内容）
[[nodiscard]] inline std::string create_xml_tag(std::string_view tag, std::string_view content,
                                                 const std::map<std::string, std::string>& attrs = {}) {
    std::string result = "<";
    result += tag;
    for (const auto& [key, value] : attrs) {
        result += ' ';
        result += key;
        result += "=\"";
        result += xml_escape(value);
        result += '"';
    }

    if (content.empty()) {
        result += " />";
    } else {
        result += '>';
        result += content;
        result += "</";
        result += tag;
        result += '>';
    }
    return result;
}

namespace xml_detail {
    // 跳过空白字符
    inline size_t skip_whitespace(std::string_view sv, size_t pos) {
        while (pos < sv.size() && (sv[pos] == ' ' || sv[pos] == '\t' ||
               sv[pos] == '\n' || sv[pos] == '\r')) {
            ++pos;
        }
        return pos;
    }

    // 解析属性名
    inline std::string parse_name(std::string_view sv, size_t& pos) {
        size_t start = pos;
        while (pos < sv.size() && sv[pos] != '=' && sv[pos] != ' ' &&
               sv[pos] != '>' && sv[pos] != '/' && sv[pos] != '\t' &&
               sv[pos] != '\n') {
            ++pos;
        }
        return std::string(sv.substr(start, pos - start));
    }

    // 解析引号中的值
    inline std::string parse_quoted(std::string_view sv, size_t& pos) {
        if (pos >= sv.size()) return {};
        char quote = sv[pos++];
        size_t start = pos;
        while (pos < sv.size() && sv[pos] != quote) ++pos;
        std::string val(sv.substr(start, pos - start));
        if (pos < sv.size()) ++pos; // 跳过结束引号
        return val;
    }
}

// 解析单个 XML 标签（简易解析，适用于工具输出格式）
[[nodiscard]] inline std::optional<XmlElement> parse_xml_tag(std::string_view sv) {
    using namespace xml_detail;
    size_t pos = skip_whitespace(sv, 0);
    if (pos >= sv.size() || sv[pos] != '<') return std::nullopt;
    ++pos; // 跳过 '<'

    // 解析标签名
    XmlElement elem;
    elem.tag = parse_name(sv, pos);
    if (elem.tag.empty()) return std::nullopt;

    // 解析属性
    while (pos < sv.size() && sv[pos] != '>' && sv[pos] != '/') {
        pos = skip_whitespace(sv, pos);
        if (pos >= sv.size() || sv[pos] == '>' || sv[pos] == '/') break;

        std::string attr_name = parse_name(sv, pos);
        if (attr_name.empty()) break;

        pos = skip_whitespace(sv, pos);
        if (pos < sv.size() && sv[pos] == '=') {
            ++pos;
            pos = skip_whitespace(sv, pos);
            if (pos < sv.size() && (sv[pos] == '"' || sv[pos] == '\'')) {
                elem.attributes[attr_name] = parse_quoted(sv, pos);
            }
        }
    }

    // 自闭合标签
    if (pos < sv.size() && sv[pos] == '/') {
        ++pos;
        if (pos < sv.size() && sv[pos] == '>') return elem;
        return std::nullopt;
    }

    if (pos >= sv.size() || sv[pos] != '>') return std::nullopt;
    ++pos; // 跳过 '>'

    // 查找关闭标签并提取内容
    std::string close_tag = "</" + elem.tag + ">";
    auto close_pos = sv.find(close_tag, pos);
    if (close_pos != std::string_view::npos) {
        elem.content = xml_unescape(sv.substr(pos, close_pos - pos));
    } else {
        // 没有关闭标签，取剩余所有内容
        elem.content = xml_unescape(sv.substr(pos));
    }
    return elem;
}

} // namespace cc::utils
