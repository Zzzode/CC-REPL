module;
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.claudemd;

export namespace cc::utils {

namespace fs = std::filesystem;

struct ClaudeMd {
    std::string content;
    std::map<std::string, std::string> frontmatter;
    std::vector<std::string> rules;
};

namespace detail {
    inline std::optional<ClaudeMd> parse_claude_md(const fs::path& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return std::nullopt;

        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        ClaudeMd md;
        md.content = content;

        // Parse frontmatter (between --- delimiters)
        if (content.starts_with("---\n")) {
            auto end = content.find("\n---\n", 4);
            if (end != std::string::npos) {
                std::string_view fm(content.data() + 4, end - 4);
                std::size_t pos = 0;
                while (pos < fm.size()) {
                    auto line_end = fm.find('\n', pos);
                    if (line_end == std::string_view::npos) line_end = fm.size();
                    auto line = fm.substr(pos, line_end - pos);
                    auto colon = line.find(':');
                    if (colon != std::string_view::npos) {
                        std::string key(line.substr(0, colon));
                        auto val = line.substr(colon + 1);
                        if (!val.empty() && val[0] == ' ') val.remove_prefix(1);
                        md.frontmatter[key] = std::string(val);
                    }
                    pos = line_end + 1;
                }
            }
        }

        // Extract rules (lines starting with "- " under a rules section)
        bool in_rules = false;
        std::size_t pos = 0;
        while (pos < content.size()) {
            auto line_end = content.find('\n', pos);
            if (line_end == std::string::npos) line_end = content.size();
            std::string_view line(content.data() + pos, line_end - pos);

            if (line.find("## Rules") != std::string_view::npos ||
                line.find("## rules") != std::string_view::npos) {
                in_rules = true;
            } else if (line.starts_with("## ") || line.starts_with("# ")) {
                in_rules = false;
            } else if (in_rules && line.starts_with("- ")) {
                md.rules.emplace_back(line.substr(2));
            }

            pos = line_end + 1;
        }

        return md;
    }

    inline std::optional<fs::path> find_file(const fs::path& dir, const std::vector<std::string>& candidates) {
        for (auto& c : candidates) {
            auto path = dir / c;
            if (fs::exists(path)) return path;
        }
        return std::nullopt;
    }
} // namespace detail

// Load project-level CLAUDE.md
std::optional<ClaudeMd> load_project_claude_md(fs::path project_root) {
    static const std::vector<std::string> candidates = {
        ".claude/CLAUDE.md",
        "CLAUDE.md",
        "claude.md",
    };

    auto found = detail::find_file(project_root, candidates);
    if (!found) return std::nullopt;
    return detail::parse_claude_md(*found);
}

// Load user-level CLAUDE.md
std::optional<ClaudeMd> load_user_claude_md() {
    const char* home = std::getenv("HOME");
    if (!home) return std::nullopt;

    static const std::vector<std::string> candidates = {
        ".claude/CLAUDE.md",
        ".config/claude-code/CLAUDE.md",
    };

    auto found = detail::find_file(fs::path(home), candidates);
    if (!found) return std::nullopt;
    return detail::parse_claude_md(*found);
}

// Merge project and user CLAUDE.md (project takes precedence for conflicts)
ClaudeMd merge_claude_md(const ClaudeMd& project, const ClaudeMd& user) {
    ClaudeMd merged;

    // Content: project first, then user
    merged.content = project.content + "\n\n" + user.content;

    // Frontmatter: user as base, project overrides
    merged.frontmatter = user.frontmatter;
    for (auto& [k, v] : project.frontmatter) {
        merged.frontmatter[k] = v;
    }

    // Rules: combine both (project rules first)
    merged.rules = project.rules;
    for (auto& rule : user.rules) {
        merged.rules.push_back(rule);
    }

    return merged;
}

} // namespace cc::utils
