module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

export module cc.utils.gitignore;

export namespace cc::utils {

namespace fs = std::filesystem;

// A parsed gitignore pattern
struct GitignorePattern {
    std::string pattern;
    bool negated = false;    // Pattern starts with !
    bool dir_only = false;   // Pattern ends with /
};

// Parse a single gitignore line into a pattern (or nullopt for comments/empty)
inline std::optional<GitignorePattern> parse_gitignore_line(std::string_view line) {
    // Trim trailing whitespace (unless escaped with backslash)
    std::string trimmed(line);
    while (!trimmed.empty() && trimmed.back() == ' ' &&
           (trimmed.size() < 2 || trimmed[trimmed.size() - 2] != '\\')) {
        trimmed.pop_back();
    }

    // Skip empty lines and comments
    if (trimmed.empty() || trimmed[0] == '#') return std::nullopt;

    GitignorePattern result;

    // Check for negation
    if (trimmed[0] == '!') {
        result.negated = true;
        trimmed = trimmed.substr(1);
    }

    // Check for directory-only pattern
    if (!trimmed.empty() && trimmed.back() == '/') {
        result.dir_only = true;
        trimmed.pop_back();
    }

    // Remove leading slash (anchors to base dir, but we store it in pattern)
    if (!trimmed.empty() && trimmed[0] == '/') {
        trimmed = trimmed.substr(1);
    }

    result.pattern = trimmed;
    return result;
}

// Gitignore filter that matches paths against loaded patterns
class GitignoreFilter {
public:
    explicit GitignoreFilter(fs::path repo_root)
        : repo_root_(std::move(repo_root)) {
        // Load the root .gitignore
        load_from_file(repo_root_ / ".gitignore");
    }

    // Add patterns from content, relative to a base directory
    void add_patterns(std::string_view content, fs::path base_dir) {
        std::istringstream stream{std::string(content)};
        std::string line;
        while (std::getline(stream, line)) {
            if (auto pattern = parse_gitignore_line(line)) {
                entries_.push_back({*pattern, std::move(base_dir)});
            }
        }
    }

    // Load patterns from a .gitignore file
    void load_from_file(const fs::path& gitignore_file) {
        if (!fs::exists(gitignore_file)) return;

        std::ifstream file(gitignore_file);
        if (!file.is_open()) return;

        std::string content((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

        fs::path base_dir = gitignore_file.parent_path();
        add_patterns(content, base_dir);
    }

    // Check if a path should be ignored
    bool is_ignored(const fs::path& path) const {
        // Get relative path from repo root
        fs::path rel_path;
        try {
            rel_path = fs::relative(path, repo_root_);
        } catch (...) {
            return false;
        }

        std::string path_str = rel_path.string();
        bool is_dir = fs::is_directory(path);
        bool ignored = false;

        // Process patterns in order (later patterns can override earlier ones)
        for (const auto& [pattern, base_dir] : entries_) {
            if (pattern.dir_only && !is_dir) continue;

            if (matches_pattern(path_str, pattern.pattern, base_dir)) {
                ignored = !pattern.negated;
            }
        }

        return ignored;
    }

private:
    // Simple glob pattern matching
    bool matches_pattern(const std::string& path, const std::string& pattern,
                        const fs::path& base_dir) const {
        // If pattern contains no slash, match against filename component
        if (pattern.find('/') == std::string::npos) {
            // Match against just the filename
            fs::path p(path);
            return matches_glob(p.filename().string(), pattern);
        }

        // Otherwise match against the relative path from base_dir
        fs::path rel_base;
        try {
            rel_base = fs::relative(base_dir, repo_root_);
        } catch (...) {
            rel_base = "";
        }

        std::string full_pattern = rel_base.empty() ? pattern : rel_base.string() + "/" + pattern;
        return matches_glob(path, full_pattern);
    }

    // Basic glob matching (supports * and ?)
    bool matches_glob(const std::string& text, const std::string& pattern) const {
        size_t t = 0, p = 0;
        size_t star_p = std::string::npos, star_t = 0;

        while (t < text.size()) {
            if (p < pattern.size() && (pattern[p] == text[t] || pattern[p] == '?')) {
                ++t; ++p;
            } else if (p < pattern.size() && pattern[p] == '*') {
                // Check for ** (matches path separators)
                if (p + 1 < pattern.size() && pattern[p + 1] == '*') {
                    ++p; // Skip second *
                }
                star_p = p++;
                star_t = t;
            } else if (star_p != std::string::npos) {
                p = star_p + 1;
                t = ++star_t;
            } else {
                return false;
            }
        }

        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    struct PatternEntry {
        GitignorePattern pattern;
        fs::path base_dir;
    };

    fs::path repo_root_;
    std::vector<PatternEntry> entries_;
};

} // namespace cc::utils
