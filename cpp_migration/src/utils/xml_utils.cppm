module;
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <sstream>

export module cc.utils.xml_utils;

export namespace cc::utils {


struct XmlElement {
    std::string tag;
    std::string content;
    std::map<std::string, std::string> attributes;
    std::vector<XmlElement> children;
};


[[nodiscard]] inline std::string xml_escape(std::string_view sv) {
    std::string result;
    result.reserve(sv.size() + sv.size() / 8);
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


[[nodiscard]] inline std::string xml_unescape(std::string_view sv) {
    std::string result;
    result.reserve(sv.size());
    size_t i = 0;
    while (i < sv.size()) {
        if (sv[i] == '&') {

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

    inline size_t skip_whitespace(std::string_view sv, size_t pos) {
        while (pos < sv.size() && (sv[pos] == ' ' || sv[pos] == '\t' ||
               sv[pos] == '\n' || sv[pos] == '\r')) {
            ++pos;
        }
        return pos;
    }


    inline std::string parse_name(std::string_view sv, size_t& pos) {
        size_t start = pos;
        while (pos < sv.size() && sv[pos] != '=' && sv[pos] != ' ' &&
               sv[pos] != '>' && sv[pos] != '/' && sv[pos] != '\t' &&
               sv[pos] != '\n') {
            ++pos;
        }
        return std::string(sv.substr(start, pos - start));
    }


    inline std::string parse_quoted(std::string_view sv, size_t& pos) {
        if (pos >= sv.size()) return {};
        char quote = sv[pos++];
        size_t start = pos;
        while (pos < sv.size() && sv[pos] != quote) ++pos;
        std::string val(sv.substr(start, pos - start));
        if (pos < sv.size()) ++pos;
        return val;
    }
}


[[nodiscard]] inline std::optional<XmlElement> parse_xml_tag(std::string_view sv) {
    using namespace xml_detail;
    size_t pos = skip_whitespace(sv, 0);
    if (pos >= sv.size() || sv[pos] != '<') return std::nullopt;
    ++pos;


    XmlElement elem;
    elem.tag = parse_name(sv, pos);
    if (elem.tag.empty()) return std::nullopt;


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


    if (pos < sv.size() && sv[pos] == '/') {
        ++pos;
        if (pos < sv.size() && sv[pos] == '>') return elem;
        return std::nullopt;
    }

    if (pos >= sv.size() || sv[pos] != '>') return std::nullopt;
    ++pos;


    std::string close_tag = "</" + elem.tag + ">";
    auto close_pos = sv.find(close_tag, pos);
    if (close_pos != std::string_view::npos) {
        elem.content = xml_unescape(sv.substr(pos, close_pos - pos));
    } else {

        elem.content = xml_unescape(sv.substr(pos));
    }
    return elem;
}

} // namespace cc::utils
