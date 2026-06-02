module;
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.markdown_utils;

export namespace cc::utils {

struct CodeBlock {
    std::string language;
    std::string code;
};

// Render markdown to terminal with ANSI color codes
std::string render_markdown_to_terminal(std::string_view md, int width) {
    std::string result;
    result.reserve(md.size() * 2);

    std::size_t pos = 0;
    while (pos < md.size()) {
        auto line_end = md.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = md.size();

        std::string_view line = md.substr(pos, line_end - pos);

        // Headers
        if (line.starts_with("### ")) {
            result += "\033[1;36m"; // Bold cyan
            result += line.substr(4);
            result += "\033[0m\n";
        } else if (line.starts_with("## ")) {
            result += "\033[1;33m"; // Bold yellow
            result += line.substr(3);
            result += "\033[0m\n";
        } else if (line.starts_with("# ")) {
            result += "\033[1;35m"; // Bold magenta
            result += line.substr(2);
            result += "\033[0m\n";
        }
        // Bold text markers
        else if (line.starts_with("**") && line.ends_with("**") && line.size() > 4) {
            result += "\033[1m";
            result += line.substr(2, line.size() - 4);
            result += "\033[0m\n";
        }
        // Bullet points
        else if (line.starts_with("- ") || line.starts_with("* ")) {
            result += "  \033[36m•\033[0m ";
            result += line.substr(2);
            result += "\n";
        }
        // Code block fence
        else if (line.starts_with("```")) {
            result += "\033[2m"; // Dim for code blocks
            result += "─";
            // Repeat to fill width
            for (int i = 1; i < width - 2 && i < 80; ++i) result += "─";
            result += "\033[0m\n";
        }
        // Normal text
        else {
            result += line;
            result += "\n";
        }

        pos = line_end + 1;
    }

    return result;
}

// Extract code blocks from markdown content
std::vector<CodeBlock> extract_code_blocks(std::string_view md) {
    std::vector<CodeBlock> blocks;
    std::size_t pos = 0;

    while (pos < md.size()) {
        auto fence_start = md.find("```", pos);
        if (fence_start == std::string_view::npos) break;

        // Extract language after opening fence
        auto lang_end = md.find('\n', fence_start + 3);
        if (lang_end == std::string_view::npos) break;

        std::string language(md.substr(fence_start + 3, lang_end - fence_start - 3));

        // Find closing fence
        auto fence_end = md.find("```", lang_end + 1);
        if (fence_end == std::string_view::npos) break;

        std::string code(md.substr(lang_end + 1, fence_end - lang_end - 1));

        // Trim trailing newline from code
        if (!code.empty() && code.back() == '\n') {
            code.pop_back();
        }

        blocks.push_back(CodeBlock{std::move(language), std::move(code)});
        pos = fence_end + 3;
    }

    return blocks;
}

// Strip all markdown formatting, returning plain text
std::string strip_markdown(std::string_view md) {
    std::string result;
    result.reserve(md.size());

    bool in_code_block = false;
    std::size_t pos = 0;

    while (pos < md.size()) {
        auto line_end = md.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = md.size();

        std::string_view line = md.substr(pos, line_end - pos);

        if (line.starts_with("```")) {
            in_code_block = !in_code_block;
            pos = line_end + 1;
            continue;
        }

        if (in_code_block) {
            result += line;
            result += "\n";
        } else {
            // Strip header markers
            std::string_view content = line;
            while (content.starts_with("#")) content.remove_prefix(1);
            if (content.starts_with(" ")) content.remove_prefix(1);

            // Strip bullet markers
            if (content.starts_with("- ") || content.starts_with("* ")) {
                content.remove_prefix(2);
            }

            // Strip bold/italic markers (simplified)
            std::string clean;
            for (std::size_t i = 0; i < content.size(); ++i) {
                if (content[i] == '*' || content[i] == '_') continue;
                clean += content[i];
            }
            result += clean;
            result += "\n";
        }

        pos = line_end + 1;
    }

    return result;
}

// Wrap content in a markdown code block
std::string wrap_in_code_block(std::string_view content, std::string_view lang) {
    std::string result = "```";
    result += lang;
    result += "\n";
    result += content;
    if (!content.empty() && content.back() != '\n') {
        result += "\n";
    }
    result += "```";
    return result;
}

} // namespace cc::utils
