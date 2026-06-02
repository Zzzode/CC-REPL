module;
#include <string>
#include <string_view>
#include <cstddef>
#include <algorithm>

export module cc.tools.mcp_classify;

export namespace cc::tools {

// MCP 工具输出类型分类
enum class McpOutputType {
    Text,      // 纯文本输出
    Code,      // 代码片段
    Table,     // 表格数据
    Error,     // 错误信息
    Progress,  // 进度/状态信息
    Unknown    // 无法确定
};

// 根据输出内容分类其类型
inline auto classify_mcp_output(std::string_view content) -> McpOutputType {
    if (content.empty()) {
        return McpOutputType::Unknown;
    }

    // 错误检测：以常见错误前缀开头
    if (content.starts_with("Error:") ||
        content.starts_with("error:") ||
        content.starts_with("ERROR") ||
        content.starts_with("fatal:") ||
        content.starts_with("Failed")) {
        return McpOutputType::Error;
    }

    // 进度信息检测
    if (content.find("...") != std::string_view::npos &&
        content.size() < 200) {
        return McpOutputType::Progress;
    }
    if (content.starts_with("[") && content.find(']') < 10) {
        // 形如 [1/5] 或 [INFO] 的进度格式
        return McpOutputType::Progress;
    }

    // 代码检测：包含缩进或代码特征
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

    // 表格检测：包含列对齐字符或 markdown 表格
    if (content.find('|') != std::string_view::npos &&
        content.find("---") != std::string_view::npos) {
        return McpOutputType::Table;
    }
    // Tab 分隔的多行数据
    size_t tab_count = std::count(content.begin(), content.end(), '\t');
    size_t line_count = std::count(content.begin(), content.end(), '\n');
    if (line_count > 2 && tab_count > line_count) {
        return McpOutputType::Table;
    }

    return McpOutputType::Text;
}

// 判断是否应该折叠（收起）输出
inline auto should_collapse_output(McpOutputType type, size_t length) -> bool {
    switch (type) {
        case McpOutputType::Text:
            return length > 2000;     // 长文本折叠
        case McpOutputType::Code:
            return length > 5000;     // 代码超过一定长度折叠
        case McpOutputType::Table:
            return length > 3000;     // 大表格折叠
        case McpOutputType::Error:
            return length > 1000;     // 错误信息较短也可能需要折叠
        case McpOutputType::Progress:
            return false;             // 进度信息不折叠
        case McpOutputType::Unknown:
            return length > 2000;
    }
    return length > 2000;
}

// 截断 MCP 输出至指定最大长度，保留首尾上下文
inline auto truncate_mcp_output(std::string_view content, size_t max_length) -> std::string {
    if (content.size() <= max_length) {
        return std::string(content);
    }

    // 保留前半和后半，中间插入截断提示
    static constexpr std::string_view truncation_marker =
        "\n\n... [output truncated, {} bytes omitted] ...\n\n";

    // 前后各保留约 40% 的空间
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
