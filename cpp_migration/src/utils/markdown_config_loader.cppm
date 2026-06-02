module;
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.markdown_config_loader;

export namespace cc::utils {

namespace fs = std::filesystem;

struct MarkdownConfig {
    std::vector<std::string> sections;
    std::map<std::string, std::string> metadata;
    std::string raw_content;
};

// Parse CLAUDE.md sections from content
MarkdownConfig parse_claude_md_sections(std::string_view content) {
    MarkdownConfig config;
    config.raw_content = std::string(content);

    std::string current_section;
    std::string current_content;
    bool in_frontmatter = false;
    bool frontmatter_done = false;

    std::size_t pos = 0;
    while (pos < content.size()) {
        auto line_end = content.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = content.size();
        std::string_view line = content.substr(pos, line_end - pos);

        // Handle YAML frontmatter
        if (line == "---" && !frontmatter_done) {
            if (!in_frontmatter) {
                in_frontmatter = true;
                pos = line_end + 1;
                continue;
            } else {
                in_frontmatter = false;
                frontmatter_done = true;
                pos = line_end + 1;
                continue;
            }
        }

        if (in_frontmatter) {
            auto colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string key(line.substr(0, colon));
                std::string_view val = line.substr(colon + 1);
                // Trim leading space from value
                if (!val.empty() && val[0] == ' ') val.remove_prefix(1);
                config.metadata[key] = std::string(val);
            }
            pos = line_end + 1;
            continue;
        }

        // Detect section headers (## level)
        if (line.starts_with("## ") || line.starts_with("# ")) {
            if (!current_section.empty()) {
                config.sections.push_back(current_section);
            }
            current_section = std::string(line);
            current_content.clear();
        } else {
            if (!current_section.empty()) {
                current_content += line;
                current_content += "\n";
            }
        }

        pos = line_end + 1;
    }

    // Push last section
    if (!current_section.empty()) {
        config.sections.push_back(current_section);
    }

    return config;
}

// Search upward from start directory for CLAUDE.md
std::optional<fs::path> find_claude_md(fs::path start) {
    // Search candidates in order
    static const std::vector<std::string> candidates = {
        ".claude/CLAUDE.md",
        "CLAUDE.md",
        "claude.md",
    };

    fs::path current = start;
    while (true) {
        for (auto& candidate : candidates) {
            fs::path check = current / candidate;
            if (fs::exists(check) && fs::is_regular_file(check)) {
                return check;
            }
        }

        auto parent = current.parent_path();
        if (parent == current) break; // Reached filesystem root
        current = parent;
    }

    return std::nullopt;
}

// Load and parse CLAUDE.md from project root
std::optional<MarkdownConfig> load_claude_md(fs::path project_root) {
    auto found = find_claude_md(project_root);
    if (!found) return std::nullopt;

    std::ifstream file(*found);
    if (!file.is_open()) return std::nullopt;

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    return parse_claude_md_sections(content);
}

} // namespace cc::utils
