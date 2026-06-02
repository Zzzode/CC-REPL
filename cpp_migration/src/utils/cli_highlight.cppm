module;
#include <string>
#include <string_view>

export module cc.utils.cli_highlight;

export namespace cc::utils {

namespace ansi {
    inline constexpr std::string_view reset   = "\033[0m";
    inline constexpr std::string_view red     = "\033[31m";
    inline constexpr std::string_view green   = "\033[32m";
    inline constexpr std::string_view yellow  = "\033[33m";
    inline constexpr std::string_view blue    = "\033[34m";
    inline constexpr std::string_view magenta = "\033[35m";
    inline constexpr std::string_view cyan    = "\033[36m";
    inline constexpr std::string_view dim     = "\033[2m";
    inline constexpr std::string_view bold    = "\033[1m";
} // namespace ansi

// Syntax-highlight code for terminal display
std::string highlight_code(std::string_view code, std::string_view language) {
    std::string result;
    result.reserve(code.size() * 2);

    // Simple keyword-based highlighting
    bool is_c_family = (language == "cpp" || language == "c" || language == "c++" ||
                        language == "java" || language == "rust" || language == "go");
    bool is_script = (language == "python" || language == "javascript" || language == "typescript" ||
                      language == "js" || language == "ts");

    std::size_t pos = 0;
    while (pos < code.size()) {
        // Handle strings
        if (code[pos] == '"' || code[pos] == '\'') {
            char quote = code[pos];
            result += ansi::green;
            result += code[pos++];
            while (pos < code.size() && code[pos] != quote) {
                if (code[pos] == '\\' && pos + 1 < code.size()) {
                    result += code[pos++];
                }
                result += code[pos++];
            }
            if (pos < code.size()) result += code[pos++];
            result += ansi::reset;
            continue;
        }

        // Handle single-line comments
        if (pos + 1 < code.size() && code[pos] == '/' && code[pos + 1] == '/') {
            result += ansi::dim;
            while (pos < code.size() && code[pos] != '\n') {
                result += code[pos++];
            }
            result += ansi::reset;
            continue;
        }

        // Handle # comments (Python, shell)
        if (code[pos] == '#' && is_script) {
            result += ansi::dim;
            while (pos < code.size() && code[pos] != '\n') {
                result += code[pos++];
            }
            result += ansi::reset;
            continue;
        }

        // Handle numbers
        if (std::isdigit(static_cast<unsigned char>(code[pos]))) {
            result += ansi::yellow;
            while (pos < code.size() && (std::isdigit(static_cast<unsigned char>(code[pos])) ||
                   code[pos] == '.' || code[pos] == 'x' || code[pos] == 'f')) {
                result += code[pos++];
            }
            result += ansi::reset;
            continue;
        }

        // Handle keywords (simplified)
        if (std::isalpha(static_cast<unsigned char>(code[pos])) || code[pos] == '_') {
            std::size_t start = pos;
            while (pos < code.size() && (std::isalnum(static_cast<unsigned char>(code[pos])) || code[pos] == '_')) {
                pos++;
            }
            std::string_view word = code.substr(start, pos - start);

            // C-family keywords
            if (is_c_family && (word == "if" || word == "else" || word == "for" || word == "while" ||
                word == "return" || word == "struct" || word == "class" || word == "namespace" ||
                word == "template" || word == "auto" || word == "const" || word == "void" ||
                word == "int" || word == "bool" || word == "true" || word == "false" ||
                word == "export" || word == "module" || word == "import" || word == "using")) {
                result += ansi::magenta;
                result += word;
                result += ansi::reset;
            }
            // Script keywords
            else if (is_script && (word == "function" || word == "const" || word == "let" ||
                     word == "var" || word == "return" || word == "if" || word == "else" ||
                     word == "def" || word == "class" || word == "import" || word == "from" ||
                     word == "async" || word == "await" || word == "true" || word == "false")) {
                result += ansi::magenta;
                result += word;
                result += ansi::reset;
            } else {
                result += word;
            }
            continue;
        }

        result += code[pos++];
    }

    return result;
}

// Highlight unified diff output
std::string highlight_diff(std::string_view diff) {
    std::string result;
    result.reserve(diff.size() * 2);

    std::size_t pos = 0;
    while (pos < diff.size()) {
        auto line_end = diff.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = diff.size();
        std::string_view line = diff.substr(pos, line_end - pos);

        if (line.starts_with("+++") || line.starts_with("---")) {
            result += ansi::bold;
            result += line;
            result += ansi::reset;
        } else if (line.starts_with("+")) {
            result += ansi::green;
            result += line;
            result += ansi::reset;
        } else if (line.starts_with("-")) {
            result += ansi::red;
            result += line;
            result += ansi::reset;
        } else if (line.starts_with("@@")) {
            result += ansi::cyan;
            result += line;
            result += ansi::reset;
        } else {
            result += line;
        }
        result += "\n";

        pos = line_end + 1;
    }

    return result;
}

// Highlight JSON with colors
std::string highlight_json(std::string_view json) {
    std::string result;
    result.reserve(json.size() * 2);

    bool in_string = false;
    bool is_key = false;

    for (std::size_t i = 0; i < json.size(); ++i) {
        char c = json[i];

        if (c == '"' && (i == 0 || json[i - 1] != '\\')) {
            if (!in_string) {
                in_string = true;
                // Check if this is a key (followed eventually by ':')
                auto close = json.find('"', i + 1);
                if (close != std::string_view::npos) {
                    auto after = json.find_first_not_of(" \t\n\r", close + 1);
                    is_key = (after != std::string_view::npos && json[after] == ':');
                }
                result += is_key ? std::string(ansi::blue) : std::string(ansi::green);
            } else {
                in_string = false;
                result += c;
                result += ansi::reset;
                continue;
            }
        }

        if (!in_string) {
            if (c == ':' || c == '{' || c == '}' || c == '[' || c == ']') {
                result += ansi::dim;
                result += c;
                result += ansi::reset;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.') {
                result += ansi::yellow;
                result += c;
                // Continue consuming number
                while (i + 1 < json.size() && (std::isdigit(static_cast<unsigned char>(json[i + 1])) ||
                       json[i + 1] == '.' || json[i + 1] == 'e' || json[i + 1] == 'E' || json[i + 1] == '-')) {
                    result += json[++i];
                }
                result += ansi::reset;
                continue;
            }
        }

        result += c;
    }

    return result;
}

// Highlight file paths with directory dimming
std::string highlight_path(std::string_view path_str) {
    auto last_slash = path_str.rfind('/');
    if (last_slash == std::string_view::npos) {
        return std::string(ansi::bold) + std::string(path_str) + std::string(ansi::reset);
    }

    std::string result;
    result += ansi::dim;
    result += path_str.substr(0, last_slash + 1);
    result += ansi::reset;
    result += ansi::bold;
    result += path_str.substr(last_slash + 1);
    result += ansi::reset;
    return result;
}

} // namespace cc::utils
