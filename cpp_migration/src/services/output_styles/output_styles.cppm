// OutputStyles - Loads custom output styles from .claude/output-styles/*.md
module;

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.services.output_styles;

export namespace cc::services::output_styles {

namespace fs = std::filesystem;

/// Output style definition loaded from markdown files
struct OutputStyle {
    std::string name;                          // Derived from filename
    std::string description;                   // From frontmatter "description:" field
    std::string prompt;                        // Markdown body (instructions for the model)
    bool keep_coding_instructions{true};       // From frontmatter "keepCodingInstructions:"
    bool is_builtin{false};                    // Built-in styles vs user-defined
    fs::path source_path;                      // File this was loaded from
};

/// Frontmatter parsing result
struct Frontmatter {
    std::string description;
    bool keep_coding_instructions = true;
};

/// Parse YAML-like frontmatter from a markdown file's header
[[nodiscard]] inline Frontmatter parse_frontmatter(std::string_view content) {
    Frontmatter fm;
    
    // Check for --- delimiter
    if (!content.starts_with("---")) return fm;
    
    auto end_pos = content.find("\n---", 3);
    if (end_pos == std::string_view::npos) return fm;
    
    auto header = content.substr(4, end_pos - 4);  // Skip first "---\n"
    
    // Simple line-by-line key: value parsing
    size_t pos = 0;
    while (pos < header.size()) {
        auto nl = header.find('\n', pos);
        auto line = (nl == std::string_view::npos) 
            ? header.substr(pos) 
            : header.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? header.size() : nl + 1;
        
        // Trim whitespace
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line = line.substr(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
            line = line.substr(0, line.size() - 1);
        }
        
        auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        
        auto key = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        
        // Trim value whitespace
        while (!value.empty() && value.front() == ' ') value = value.substr(1);
        
        // Strip surrounding quotes
        if (value.size() >= 2 && 
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
        
        if (key == "description") {
            fm.description = std::string(value);
        } else if (key == "keepCodingInstructions") {
            fm.keep_coding_instructions = (value != "false" && value != "no" && value != "0");
        }
    }
    
    return fm;
}

/// Parse a single style markdown file
[[nodiscard]] inline std::optional<OutputStyle> parse_style_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
        return std::nullopt;
    }
    
    // Read file contents
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    
    if (content.empty()) return std::nullopt;
    
    OutputStyle style;
    style.source_path = path;
    style.name = path.stem().string();  // filename without extension
    
    // Parse frontmatter if present
    if (content.starts_with("---")) {
        auto fm = parse_frontmatter(content);
        style.description = fm.description;
        style.keep_coding_instructions = fm.keep_coding_instructions;
        
        // Extract body after frontmatter
        auto end_pos = content.find("\n---", 3);
        if (end_pos != std::string::npos) {
            auto body_start = content.find('\n', end_pos + 4);
            if (body_start != std::string::npos) {
                style.prompt = content.substr(body_start + 1);
            }
        }
    } else {
        // No frontmatter - entire file is the prompt
        style.prompt = content;
    }
    
    // Trim leading/trailing whitespace from prompt
    while (!style.prompt.empty() && 
           (style.prompt.front() == '\n' || style.prompt.front() == '\r')) {
        style.prompt.erase(style.prompt.begin());
    }
    while (!style.prompt.empty() && 
           (style.prompt.back() == '\n' || style.prompt.back() == '\r')) {
        style.prompt.pop_back();
    }
    
    return style;
}

/// Load all output styles from a directory
[[nodiscard]] inline std::vector<OutputStyle> load_output_styles_dir(std::string_view dir_path) {
    std::vector<OutputStyle> styles;
    
    fs::path dir(dir_path);
    std::error_code ec;
    
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        return styles;
    }
    
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        
        auto ext = entry.path().extension().string();
        if (ext != ".md" && ext != ".markdown") continue;
        
        auto style = parse_style_file(entry.path());
        if (style) {
            styles.push_back(std::move(*style));
        }
    }
    
    // Sort by name for consistent ordering
    std::sort(styles.begin(), styles.end(),
        [](const OutputStyle& a, const OutputStyle& b) { return a.name < b.name; });
    
    return styles;
}

/// Load output styles from project + user directories
[[nodiscard]] inline std::vector<OutputStyle> load_output_styles(const fs::path& project_root) {
    std::vector<OutputStyle> all_styles;
    
    // 1. Load from project: <project>/.claude/output-styles/
    auto project_dir = project_root / ".claude" / "output-styles";
    auto project_styles = load_output_styles_dir(project_dir.string());
    for (auto& s : project_styles) {
        all_styles.push_back(std::move(s));
    }
    
    // 2. Load from user home: ~/.claude/output-styles/
    if (auto home = std::getenv("HOME")) {
        auto user_dir = fs::path(home) / ".claude" / "output-styles";
        auto user_styles = load_output_styles_dir(user_dir.string());
        for (auto& s : user_styles) {
            // Don't override project-level styles with same name
            bool exists = false;
            for (const auto& existing : all_styles) {
                if (existing.name == s.name) { exists = true; break; }
            }
            if (!exists) {
                all_styles.push_back(std::move(s));
            }
        }
    }
    
    return all_styles;
}

/// Get the active output style by name
[[nodiscard]] inline std::optional<OutputStyle> get_active_style(
    std::string_view style_name, const std::vector<OutputStyle>& styles) {
    for (const auto& s : styles) {
        if (s.name == style_name) return s;
    }
    return std::nullopt;
}

/// Apply an output style to content (prepend style prompt as system instruction)
[[nodiscard]] inline std::string apply_style(const OutputStyle& style, std::string_view content) {
    if (style.prompt.empty()) return std::string(content);
    return style.prompt + "\n\n" + std::string(content);
}

} // namespace cc::services::output_styles
