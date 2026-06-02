module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <algorithm>

export module cc.skills.load_skills_dir;

export namespace cc::skills {

// Manifest describing a discovered skill
struct SkillManifest {
    std::string name;
    std::string description;
    std::optional<std::string> version;
    std::vector<std::string> triggers;
    std::filesystem::path directory;
};

// Get the list of directories to search for skills
std::vector<std::filesystem::path> get_skills_search_paths();

// Internal: parse a skill manifest file
inline std::optional<SkillManifest> parse_manifest(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& skill_dir);

// Load all skills from a given directory
std::vector<SkillManifest> load_skills_directory(std::filesystem::path dir) {
    std::vector<SkillManifest> skills;

    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        return skills;
    }

    // Each subdirectory or .md file in the skills directory represents a skill
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_directory()) {
            // Look for manifest file (skill.json or skill.yaml)
            auto manifest_path = entry.path() / "skill.json";
            if (!std::filesystem::exists(manifest_path)) {
                manifest_path = entry.path() / "manifest.json";
            }

            if (std::filesystem::exists(manifest_path)) {
                auto manifest = parse_manifest(manifest_path, entry.path());
                if (manifest.has_value()) {
                    skills.push_back(std::move(*manifest));
                }
            } else {
                // Fallback: treat directory name as skill name, look for prompt.md
                auto prompt_path = entry.path() / "prompt.md";
                if (std::filesystem::exists(prompt_path)) {
                    SkillManifest manifest;
                    manifest.name = entry.path().filename().string();
                    manifest.description = "Skill: " + manifest.name;
                    manifest.directory = entry.path();
                    skills.push_back(std::move(manifest));
                }
            }
        } else if (entry.is_regular_file() && entry.path().extension() == ".md") {
            // Single-file skill (markdown with frontmatter)
            SkillManifest manifest;
            manifest.name = entry.path().stem().string();
            manifest.description = "Skill: " + manifest.name;
            manifest.directory = dir;
            skills.push_back(std::move(manifest));
        }
    }

    return skills;
}

// Find a skill by its exact name
std::optional<SkillManifest> find_skill_by_name(std::string_view name) {
    auto paths = get_skills_search_paths();

    for (const auto& search_path : paths) {
        auto skills = load_skills_directory(search_path);
        for (auto& skill : skills) {
            if (skill.name == name) {
                return std::move(skill);
            }
        }
    }

    return std::nullopt;
}

// Find a skill by matching user input against trigger patterns
std::optional<SkillManifest> find_skill_by_trigger(std::string_view input) {
    auto paths = get_skills_search_paths();
    std::string input_lower(input);
    std::transform(input_lower.begin(), input_lower.end(), input_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& search_path : paths) {
        auto skills = load_skills_directory(search_path);
        for (auto& skill : skills) {
            for (const auto& trigger : skill.triggers) {
                std::string trigger_lower = trigger;
                std::transform(trigger_lower.begin(), trigger_lower.end(),
                             trigger_lower.begin(),
                             [](unsigned char c) { return std::tolower(c); });

                // Check if any trigger phrase matches the input
                if (input_lower.find(trigger_lower) != std::string::npos) {
                    return std::move(skill);
                }
            }
        }
    }

    return std::nullopt;
}

// Get the list of directories to search for skills
std::vector<std::filesystem::path> get_skills_search_paths() {
    std::vector<std::filesystem::path> paths;

    // Project-local skills
    auto cwd = std::filesystem::current_path();
    auto project_skills = cwd / ".claude" / "skills";
    if (std::filesystem::exists(project_skills)) {
        paths.push_back(project_skills);
    }

    // User-level skills
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        auto user_skills = std::filesystem::path(home) / ".config" / "claude-code" / "skills";
        if (std::filesystem::exists(user_skills)) {
            paths.push_back(user_skills);
        }
    }

    // Custom paths from environment
    const char* skill_path = std::getenv("CLAUDE_SKILLS_PATH");
    if (skill_path && skill_path[0] != '\0') {
        std::string paths_str(skill_path);
        size_t pos = 0;
        while (pos < paths_str.size()) {
            auto sep = paths_str.find(':', pos);
            std::string path_segment = (sep == std::string::npos)
                ? paths_str.substr(pos)
                : paths_str.substr(pos, sep - pos);

            if (!path_segment.empty() && std::filesystem::exists(path_segment)) {
                paths.push_back(path_segment);
            }
            pos = (sep == std::string::npos) ? paths_str.size() : sep + 1;
        }
    }

    return paths;
}

// Internal: parse a skill manifest file
inline std::optional<SkillManifest> parse_manifest(
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& skill_dir
) {
    std::ifstream ifs(manifest_path);
    if (!ifs.is_open()) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());

    SkillManifest manifest;
    manifest.directory = skill_dir;

    // Simple JSON field extraction
    auto extract = [&content](const std::string& key) -> std::optional<std::string> {
        std::string search = "\"" + key + "\"";
        auto pos = content.find(search);
        if (pos == std::string::npos) return std::nullopt;
        pos += search.size();
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == ':')) ++pos;
        if (pos >= content.size() || content[pos] != '"') return std::nullopt;
        ++pos;
        std::string value;
        while (pos < content.size() && content[pos] != '"') {
            if (content[pos] == '\\') ++pos;
            if (pos < content.size()) value += content[pos++];
        }
        return value;
    };

    if (auto name = extract("name")) manifest.name = *name;
    else manifest.name = skill_dir.filename().string();

    if (auto desc = extract("description")) manifest.description = *desc;
    if (auto ver = extract("version")) manifest.version = *ver;

    // Extract triggers array (simplified)
    auto triggers_pos = content.find("\"triggers\"");
    if (triggers_pos != std::string::npos) {
        auto arr_start = content.find('[', triggers_pos);
        auto arr_end = content.find(']', arr_start);
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string arr = content.substr(arr_start + 1, arr_end - arr_start - 1);
            size_t pos = 0;
            while (pos < arr.size()) {
                auto q1 = arr.find('"', pos);
                if (q1 == std::string::npos) break;
                auto q2 = arr.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                manifest.triggers.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
                pos = q2 + 1;
            }
        }
    }

    return manifest;
}

} // namespace cc::skills
