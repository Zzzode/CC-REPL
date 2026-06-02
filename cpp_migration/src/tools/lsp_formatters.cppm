module;
#include <string>
#include <string_view>
#include <sstream>
#include <span>
#include <vector>

export module cc.tools.lsp_formatters;

import cc.tools.script_diagnostics;

export namespace cc::tools {

// 格式化 LSP hover 信息为终端可读格式
inline auto format_hover_info(
    std::string_view content,
    std::string_view language
) -> std::string {
    std::ostringstream oss;

    if (content.empty()) {
        return "(no hover information available)";
    }

    // 如果内容已经包含 markdown 代码块标记则直接输出
    if (content.find("```") != std::string_view::npos) {
        oss << content;
    } else {
        // 包裹为带语言标注的代码块
        if (!language.empty()) {
            oss << "```" << language << "\n";
        } else {
            oss << "```\n";
        }
        oss << content << "\n```";
    }

    return oss.str();
}

// 格式化 LSP 补全项
inline auto format_completion_item(
    std::string_view label,
    std::string_view detail,
    std::string_view kind
) -> std::string {
    std::ostringstream oss;

    // 用图标表示补全项类型
    if (kind == "function" || kind == "method") {
        oss << "ƒ ";
    } else if (kind == "variable" || kind == "field") {
        oss << "● ";
    } else if (kind == "class" || kind == "interface") {
        oss << "◆ ";
    } else if (kind == "module") {
        oss << "□ ";
    } else if (kind == "keyword") {
        oss << "⊞ ";
    } else if (kind == "snippet") {
        oss << "✂ ";
    } else {
        oss << "  ";
    }

    oss << label;

    if (!detail.empty()) {
        oss << "  — " << detail;
    }

    return oss.str();
}

// 格式化 LSP 签名帮助信息（高亮当前活跃参数）
inline auto format_signature_help(
    std::string_view signature,
    int active_param
) -> std::string {
    std::ostringstream oss;
    oss << signature << "\n";

    // 解析参数列表并高亮活跃参数
    // 找到括号内的参数部分
    auto paren_start = signature.find('(');
    auto paren_end = signature.rfind(')');

    if (paren_start == std::string_view::npos ||
        paren_end == std::string_view::npos) {
        return oss.str();
    }

    // 提取参数列表并按逗号分割
    auto params_str = signature.substr(paren_start + 1, paren_end - paren_start - 1);

    // 生成下划线指示器
    std::string indicator(signature.size(), ' ');
    int param_index = 0;
    size_t pos = paren_start + 1;
    size_t current_start = pos;
    int depth = 0; // 括号嵌套深度

    for (size_t i = paren_start + 1; i < paren_end; ++i) {
        char c = signature[i];
        if (c == '(' || c == '<' || c == '[') ++depth;
        else if (c == ')' || c == '>' || c == ']') --depth;
        else if (c == ',' && depth == 0) {
            if (param_index == active_param) {
                // 标记当前参数范围
                for (size_t j = current_start; j < i; ++j) {
                    indicator[j] = '^';
                }
                break;
            }
            ++param_index;
            current_start = i + 1;
            // 跳过逗号后的空格
            while (current_start < paren_end && signature[current_start] == ' ') {
                ++current_start;
            }
        }
    }

    // 处理最后一个参数
    if (param_index == active_param) {
        for (size_t j = current_start; j < paren_end; ++j) {
            indicator[j] = '^';
        }
    }

    oss << indicator;
    return oss.str();
}

// 格式化诊断信息列表（用于 LSP textDocument/publishDiagnostics）
inline auto format_diagnostic_list(std::span<const Diagnostic> diagnostics) -> std::string {
    std::ostringstream oss;

    if (diagnostics.empty()) {
        return "No diagnostics reported.";
    }

    for (const auto& diag : diagnostics) {
        // 级别图标
        switch (diag.level) {
            case Diagnostic::Level::Error:   oss << "✖ "; break;
            case Diagnostic::Level::Warning: oss << "⚠ "; break;
            case Diagnostic::Level::Info:    oss << "ℹ "; break;
        }

        oss << diag.file.filename().string()
            << ":" << diag.line << ":" << diag.column
            << " " << diag.message << "\n";
    }

    return oss.str();
}

} // namespace cc::tools
