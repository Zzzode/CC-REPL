/// @file skill.cppm
/// @brief Skill system module - concept, loading, matching and execution.
/// Skills are reusable workflow templates injected into the system prompt.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <optional>
#include <expected>
#include <concepts>
#include <format>
#include <algorithm>
#include <regex>
#include <filesystem>
#include <fstream>
#include <sstream>

export module cc.skills.skill;

import cc.types.types;

export namespace cc::skills {

using cc::core::Result;
using cc::core::Error;
using cc::core::ErrorCode;
using cc::core::VoidResult;

// ============================================================
// Skill Definition
// ============================================================

/// Represents a single skill's metadata and content
struct SkillDefinition {
    std::string name;                          // Unique skill identifier
    std::string description;                   // Human-readable description
    std::vector<std::string> trigger_patterns; // Regex patterns for matching
    std::string content;                       // Skill prompt content (markdown workflow)
    bool is_builtin = false;                   // Whether this is a built-in skill
    std::optional<std::string> author;         // Skill author (for custom skills)
    std::optional<std::string> version;        // Skill version string

    /// Serialize to JSON string for API transport
    [[nodiscard]] std::string to_json() const {
        return std::format(
            R"({{"name":"{}","description":"{}","is_builtin":{},"trigger_count":{}}})",
            name, description, is_builtin ? "true" : "false", trigger_patterns.size());
    }
};

// ============================================================
// Skill Concept - contract for skill implementations
// ============================================================

/// Concept defining what constitutes a valid Skill type
template <typename T>
concept Skill = requires(const T& skill) {
    { skill.definition() } -> std::same_as<const SkillDefinition&>;
    { skill.name() } -> std::convertible_to<std::string_view>;
    { skill.content() } -> std::convertible_to<std::string_view>;
};

// ============================================================
// Skill Match Result
// ============================================================

/// Result of matching user input against skill triggers
struct SkillMatch {
    std::string skill_name;    // Matched skill name
    double confidence = 0.0;   // Match confidence score [0.0, 1.0]
    std::string matched_pattern; // The pattern that triggered the match

    /// Compare by confidence for sorting
    [[nodiscard]] auto operator<=>(const SkillMatch& other) const {
        return other.confidence <=> confidence; // Descending order
    }
};

// ============================================================
// SkillLoader - loads skills from filesystem
// ============================================================

/// Loads and discovers skills from filesystem directories
class SkillLoader {
    std::vector<std::filesystem::path> search_paths_;

public:
    SkillLoader() {
        // Default search paths: ~/.claude/skills/ and project-local .claude/skills/
        if (auto home = std::getenv("HOME")) {
            search_paths_.emplace_back(
                std::filesystem::path(home) / ".claude" / "skills");
        }
        search_paths_.emplace_back(
            std::filesystem::current_path() / ".claude" / "skills");
    }

    /// Add additional search path for skill discovery
    void add_search_path(std::filesystem::path path) {
        search_paths_.push_back(std::move(path));
    }

    /// Load all skills from a directory, parsing markdown frontmatter
    [[nodiscard]] Result<std::vector<SkillDefinition>> load_from_directory(
        const std::filesystem::path& dir) const {

        std::vector<SkillDefinition> skills;
        if (!std::filesystem::exists(dir)) {
            return skills; // Empty but not an error
        }

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_directory()) {
                auto skill = parse_skill_directory(entry.path());
                if (skill) {
                    skills.push_back(std::move(*skill));
                }
                continue;
            }

            if (entry.path().extension() != ".md") continue;

            auto skill = parse_skill_file(entry.path());
            if (skill) {
                skills.push_back(std::move(*skill));
            }
        }
        return skills;
    }

    /// Discover all custom skills from configured search paths
    [[nodiscard]] Result<std::vector<SkillDefinition>> discover_all() const {
        std::vector<SkillDefinition> all_skills;
        for (const auto& path : search_paths_) {
            auto result = load_from_directory(path);
            if (result) {
                for (auto& skill : *result) {
                    all_skills.push_back(std::move(skill));
                }
            }
        }
        return all_skills;
    }

private:
    /// Parse a directory-form skill, normally .claude/skills/<name>/SKILL.md.
    [[nodiscard]] std::optional<SkillDefinition> parse_skill_directory(
        const std::filesystem::path& dirpath) const {

        auto skill_path = dirpath / "SKILL.md";
        if (!std::filesystem::exists(skill_path)) {
            skill_path = dirpath / "skill.md";
        }
        if (!std::filesystem::exists(skill_path)) {
            skill_path = dirpath / "prompt.md";
        }
        if (!std::filesystem::exists(skill_path)) return std::nullopt;

        auto skill = parse_skill_file(skill_path);
        if (skill && (skill->name == "SKILL" || skill->name == "skill" || skill->name == "prompt")) {
            skill->name = dirpath.filename().string();
        }
        return skill;
    }

    /// Parse a single skill markdown file with YAML-like frontmatter
    [[nodiscard]] std::optional<SkillDefinition> parse_skill_file(
        const std::filesystem::path& filepath) const {

        std::ifstream file(filepath);
        if (!file.is_open()) return std::nullopt;

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string raw = buffer.str();

        SkillDefinition def;
        def.name = filepath.stem().string();
        def.is_builtin = false;

        // Parse frontmatter between --- markers
        if (raw.starts_with("---")) {
            auto end_pos = raw.find("---", 3);
            if (end_pos != std::string::npos) {
                std::string frontmatter = raw.substr(3, end_pos - 3);
                parse_frontmatter(frontmatter, def);
                def.content = raw.substr(end_pos + 3);
            } else {
                def.content = raw;
            }
        } else {
            def.content = raw;
            def.description = std::format("Custom skill: {}", def.name);
        }
        return def;
    }

    /// Extract key-value pairs from YAML-like frontmatter
    void parse_frontmatter(const std::string& frontmatter, SkillDefinition& def) const {
        std::istringstream stream(frontmatter);
        std::string line;
        while (std::getline(stream, line)) {
            // Simple key: value parsing
            auto colon_pos = line.find(':');
            if (colon_pos == std::string::npos) continue;

            auto key = line.substr(0, colon_pos);
            auto value = line.substr(colon_pos + 1);
            // Trim whitespace
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());

            if (key == "description") def.description = value;
            else if (key == "name") def.name = value;
            else if (key == "author") def.author = value;
            else if (key == "version") def.version = value;
            else if (key == "trigger") def.trigger_patterns.push_back(value);
        }
    }
};

// ============================================================
// SkillExecutor - matches and injects skills into prompts
// ============================================================

/// Matches user input to skills and produces system prompt injections
class SkillExecutor {
    std::vector<SkillDefinition> registered_skills_;
    // Compiled regex cache: skill_name -> compiled patterns
    std::unordered_map<std::string, std::vector<std::regex>> pattern_cache_;

public:
    /// Register a skill for matching
    void register_skill(SkillDefinition skill) {
        // Compile trigger patterns into regex
        std::vector<std::regex> compiled;
        compiled.reserve(skill.trigger_patterns.size());
        for (const auto& pattern : skill.trigger_patterns) {
            try {
                compiled.emplace_back(pattern, std::regex::icase | std::regex::optimize);
            } catch (const std::regex_error&) {
                // Skip invalid patterns silently
            }
        }
        pattern_cache_[skill.name] = std::move(compiled);
        registered_skills_.push_back(std::move(skill));
    }

    /// Match user input against all registered skills
    [[nodiscard]] std::vector<SkillMatch> match(std::string_view input) const {
        std::vector<SkillMatch> matches;

        for (const auto& skill : registered_skills_) {
            auto it = pattern_cache_.find(skill.name);
            if (it == pattern_cache_.end()) continue;

            for (std::size_t i = 0; i < it->second.size(); ++i) {
                if (std::regex_search(input.begin(), input.end(), it->second[i])) {
                    matches.push_back(SkillMatch{
                        .skill_name = skill.name,
                        .confidence = 1.0, // Full regex match => high confidence
                        .matched_pattern = skill.trigger_patterns[i],
                    });
                    break; // One match per skill is enough
                }
            }
        }

        // Sort by confidence (descending)
        std::sort(matches.begin(), matches.end());
        return matches;
    }

    /// Inject matched skill content into system prompt
    [[nodiscard]] std::string inject_into_prompt(
        std::string_view base_prompt,
        const std::vector<SkillMatch>& matches) const {

        if (matches.empty()) return std::string(base_prompt);

        std::string result(base_prompt);
        result += "\n\n<skills_context>\n";

        for (const auto& match : matches) {
            auto skill = find_skill(match.skill_name);
            if (!skill) continue;
            result += std::format("## Skill: {}\n{}\n\n", skill->name, skill->content);
        }
        result += "</skills_context>";
        return result;
    }

    /// Get a specific skill by name
    [[nodiscard]] const SkillDefinition* find_skill(std::string_view name) const {
        auto it = std::ranges::find_if(registered_skills_,
            [name](const SkillDefinition& s) { return s.name == name; });
        return (it != registered_skills_.end()) ? &(*it) : nullptr;
    }

    /// Get all registered skill definitions
    [[nodiscard]] const std::vector<SkillDefinition>& skills() const noexcept {
        return registered_skills_;
    }

    /// Number of registered skills
    [[nodiscard]] std::size_t size() const noexcept {
        return registered_skills_.size();
    }
};

} // namespace cc::skills
