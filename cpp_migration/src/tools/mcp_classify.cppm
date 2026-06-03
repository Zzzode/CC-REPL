module;
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>

export module cc.tools.mcp_classify;

export namespace cc::tools {


enum class McpOutputType {
    Text,
    Code,
    Table,
    Error,
    Progress,
    Unknown
};


inline auto classify_mcp_output(std::string_view content) -> McpOutputType {
    if (content.empty()) {
        return McpOutputType::Unknown;
    }


    if (content.starts_with("Error:") ||
        content.starts_with("error:") ||
        content.starts_with("ERROR") ||
        content.starts_with("fatal:") ||
        content.starts_with("Failed")) {
        return McpOutputType::Error;
    }


    if (content.find("...") != std::string_view::npos &&
        content.size() < 200) {
        return McpOutputType::Progress;
    }
    if (content.starts_with("[") && content.find(']') < 10) {

        return McpOutputType::Progress;
    }


    size_t code_indicators = 0;
    if (content.find("```") != std::string_view::npos) code_indicators += 3;
    if (content.find("    ") != std::string_view::npos) code_indicators += 1;
    if (content.find("function ") != std::string_view::npos) code_indicators += 2;
    if (content.find("class ") != std::string_view::npos) code_indicators += 2;
    if (content.find("import ") != std::string_view::npos) code_indicators += 1;
    if (content.find("const ") != std::string_view::npos) code_indicators += 1;
    if (content.find("return ") != std::string_view::npos) code_indicators += 1;
    if (content.find('{') != std::string_view::npos &&
        content.find('}') != std::string_view::npos) code_indicators += 1;

    if (code_indicators >= 3) {
        return McpOutputType::Code;
    }


    if (content.find('|') != std::string_view::npos &&
        content.find("---") != std::string_view::npos) {
        return McpOutputType::Table;
    }

    size_t tab_count = std::count(content.begin(), content.end(), '\t');
    size_t line_count = std::count(content.begin(), content.end(), '\n');
    if (line_count > 2 && tab_count > line_count) {
        return McpOutputType::Table;
    }

    return McpOutputType::Text;
}


inline auto should_collapse_output(McpOutputType type, size_t length) -> bool {
    switch (type) {
        case McpOutputType::Text:
            return length > 2000;
        case McpOutputType::Code:
            return length > 5000;
        case McpOutputType::Table:
            return length > 3000;
        case McpOutputType::Error:
            return length > 1000;
        case McpOutputType::Progress:
            return false;
        case McpOutputType::Unknown:
            return length > 2000;
    }
    return length > 2000;
}


inline auto truncate_mcp_output(std::string_view content, size_t max_length) -> std::string {
    if (content.size() <= max_length) {
        return std::string(content);
    }


    static constexpr std::string_view truncation_marker =
        "\n\n... [output truncated, {} bytes omitted] ...\n\n";


    size_t head_size = max_length * 2 / 5;
    size_t tail_size = max_length * 2 / 5;
    size_t omitted = content.size() - head_size - tail_size;

    std::string result;
    result.reserve(max_length + 100);
    result.append(content.substr(0, head_size));
    result.append("\n\n... [output truncated, ");
    result.append(std::to_string(omitted));
    result.append(" bytes omitted] ...\n\n");
    result.append(content.substr(content.size() - tail_size));

    return result;
}

} // namespace cc::tools
