module;
#include <string>
#include <string_view>
#include <sstream>
#include <span>
#include <vector>

export module cc.tools.lsp_formatters;

import cc.tools.script_diagnostics;

export namespace cc::tools {


inline auto format_hover_info(
    std::string_view content,
    std::string_view language
) -> std::string {
    std::ostringstream oss;

    if (content.empty()) {
        return "(no hover information available)";
    }


    if (content.find("```") != std::string_view::npos) {
        oss << content;
    } else {

        if (!language.empty()) {
            oss << "```" << language << "\n";
        } else {
            oss << "```\n";
        }
        oss << content << "\n```";
    }

    return oss.str();
}


inline auto format_completion_item(
    std::string_view label,
    std::string_view detail,
    std::string_view kind
) -> std::string {
    std::ostringstream oss;


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


inline auto format_signature_help(
    std::string_view signature,
    int active_param
) -> std::string {
    std::ostringstream oss;
    oss << signature << "\n";



    auto paren_start = signature.find('(');
    auto paren_end = signature.rfind(')');

    if (paren_start == std::string_view::npos ||
        paren_end == std::string_view::npos) {
        return oss.str();
    }


    auto params_str = signature.substr(paren_start + 1, paren_end - paren_start - 1);


    std::string indicator(signature.size(), ' ');
    int param_index = 0;
    size_t pos = paren_start + 1;
    size_t current_start = pos;
    int depth = 0;

    for (size_t i = paren_start + 1; i < paren_end; ++i) {
        char c = signature[i];
        if (c == '(' || c == '<' || c == '[') ++depth;
        else if (c == ')' || c == '>' || c == ']') --depth;
        else if (c == ',' && depth == 0) {
            if (param_index == active_param) {

                for (size_t j = current_start; j < i; ++j) {
                    indicator[j] = '^';
                }
                break;
            }
            ++param_index;
            current_start = i + 1;

            while (current_start < paren_end && signature[current_start] == ' ') {
                ++current_start;
            }
        }
    }


    if (param_index == active_param) {
        for (size_t j = current_start; j < paren_end; ++j) {
            indicator[j] = '^';
        }
    }

    oss << indicator;
    return oss.str();
}


inline auto format_diagnostic_list(std::span<const Diagnostic> diagnostics) -> std::string {
    std::ostringstream oss;

    if (diagnostics.empty()) {
        return "No diagnostics reported.";
    }

    for (const auto& diag : diagnostics) {

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
