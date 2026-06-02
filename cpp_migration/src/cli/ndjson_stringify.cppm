module;
#include <string>
#include <string_view>
#include <map>
#include <expected>
#include <sstream>

export module cc.cli.ndjson_stringify;

export namespace cc::cli {

// Escape a string for safe inclusion in JSON
inline std::string escape_for_json(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (char c : input) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

// Convert a key-value map to a single NDJSON line
std::string ndjson_stringify(std::map<std::string, std::string> data) {
    std::ostringstream oss;
    oss << "{";

    bool first = true;
    for (const auto& [key, value] : data) {
        if (!first) oss << ",";
        oss << "\"" << escape_for_json(key) << "\":\"" << escape_for_json(value) << "\"";
        first = false;
    }

    oss << "}";
    return oss.str();
}

// Parse a single NDJSON line into a key-value map
std::expected<std::map<std::string, std::string>, std::string> ndjson_parse(std::string_view line) {
    std::map<std::string, std::string> result;

    // Trim whitespace
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                             line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }

    if (line.empty() || line.front() != '{' || line.back() != '}') {
        return std::unexpected("Invalid NDJSON: must be a JSON object");
    }

    // Remove outer braces
    line.remove_prefix(1);
    line.remove_suffix(1);

    // Simple parser for flat JSON objects with string values
    size_t pos = 0;
    std::string content(line);

    while (pos < content.size()) {
        // Skip whitespace
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == ',')) ++pos;
        if (pos >= content.size()) break;

        // Expect opening quote for key
        if (content[pos] != '"') {
            return std::unexpected("Expected '\"' at position " + std::to_string(pos));
        }
        ++pos;

        // Read key
        std::string key;
        while (pos < content.size() && content[pos] != '"') {
            if (content[pos] == '\\' && pos + 1 < content.size()) {
                ++pos;
                switch (content[pos]) {
                    case 'n': key += '\n'; break;
                    case 'r': key += '\r'; break;
                    case 't': key += '\t'; break;
                    case '"': key += '"'; break;
                    case '\\': key += '\\'; break;
                    default: key += content[pos]; break;
                }
            } else {
                key += content[pos];
            }
            ++pos;
        }
        if (pos >= content.size()) return std::unexpected("Unterminated key string");
        ++pos; // closing quote

        // Skip colon and whitespace
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == ':')) ++pos;

        // Expect opening quote for value
        if (pos >= content.size() || content[pos] != '"') {
            return std::unexpected("Expected '\"' for value at position " + std::to_string(pos));
        }
        ++pos;

        // Read value
        std::string value;
        while (pos < content.size() && content[pos] != '"') {
            if (content[pos] == '\\' && pos + 1 < content.size()) {
                ++pos;
                switch (content[pos]) {
                    case 'n': value += '\n'; break;
                    case 'r': value += '\r'; break;
                    case 't': value += '\t'; break;
                    case '"': value += '"'; break;
                    case '\\': value += '\\'; break;
                    default: value += content[pos]; break;
                }
            } else {
                value += content[pos];
            }
            ++pos;
        }
        if (pos >= content.size()) return std::unexpected("Unterminated value string");
        ++pos; // closing quote

        result[key] = value;
    }

    return result;
}

// Escape newlines in a string to make it safe for NDJSON (single line)
std::string ndjson_safe_string(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (char c : input) {
        switch (c) {
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            default: result += c; break;
        }
    }
    return result;
}

} // namespace cc::cli
