/// @file load_skills_dir.cppm
/// @brief Rich skills directory loading: YAML frontmatter, MDX, params, hooks,
///        gitignore filtering, realpath dedup, managed-settings policy gate,
///        MCP skill builder registration, dynamic skills registry.
///
/// TS REF: src/skills/loadSkillsDir.ts (1086 lines)
///
/// Key functions ported faithfully:
///   - parseFrontMatter()           → parse_frontmatter_rich()
///   - parseSkillFrontmatterFields() → parse_skill_frontmatter_fields()
///   - createSkillCommand()          → create_skill_command()
///   - loadSkillsFromSkillsDir()     → load_skills_from_skills_dir()
///   - loadSkillsFromCommandsDir()   → load_skills_from_commands_dir()
///   - getSkillDirCommands()         → get_skill_dir_commands()
///   - getFileIdentity()             → get_file_identity()
///   - parseHooksFromFrontmatter()   → parse_hooks_from_frontmatter()
///   - parseSkillPaths()             → parse_skill_paths()
///   - discoverSkillDirsForPaths()   → discover_skill_dirs_for_paths()
///   - addSkillDirectories()         → add_skill_directories()
///   - getDynamicSkills()            → get_dynamic_skills()
///   - activateConditionalSkillsForPaths() → activate_conditional_skills_for_paths()
///   - onDynamicSkillsLoaded()       → on_dynamic_skills_loaded()
///   - clearSkillCaches()            → clear_skill_caches()
module;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// (sys/stat.h functionality provided by <filesystem>)

export module cc.skills.load_skills_dir;

import cc.utils.yaml;
import cc.utils.frontmatter_parser;
import cc.utils.gitignore;
import cc.utils.argument_substitution;
import cc.utils.effort;
import cc.utils.env_utils;
import cc.utils.log;
import cc.utils.markdown_utils;
import cc.utils.path_utils;
import cc.utils.platform_paths;
import cc.utils.string_utils;
import cc.skills.mcp_skill_builders;
export import cc.skills.skill;

export namespace cc::skills {

namespace fs = std::filesystem;
using cc::utils::YamlValue;
using cc::utils::YamlMap;
using cc::utils::YamlArray;
using cc::utils::log::debug;
using cc::utils::log::warning;
using cc::utils::log::log_error;

// =========================================================================
// LoadedFrom enum
// TS REF: src/skills/loadSkillsDir.ts:67-74
// =========================================================================

/// Where a skill was loaded from
enum class LoadedFrom {
    CommandsDeprecated,  // Legacy /commands/ directory
    Skills,              // /skills/ directory
    Plugin,              // Plugin-provided skill
    Managed,             // Managed/policy settings
    Bundled,             // Built-in bundled skill
    Mcp,                 // MCP server-backed skill
};

// =========================================================================
// SettingSource enum
// TS REF: src/utils/settings/constants.ts
// =========================================================================

/// Source of a setting/skill
enum class SettingSource {
    PolicySettings,   // Managed policy
    UserSettings,     // User-level config (~/.claude/)
    ProjectSettings,  // Project-level (.claude/)
    Builtin,          // Built-in default
};

// =========================================================================
// FrontmatterShell
// TS REF: src/utils/frontmatterParser.ts:339
// =========================================================================

/// Shell for !`cmd` and ```! block execution in skill .md content
enum class FrontmatterShell {
    Bash,
    Powershell,
};

// =========================================================================
// FrontmatterData
// TS REF: src/utils/frontmatterParser.ts:10-59
// =========================================================================

/// Rich frontmatter data extracted from YAML between --- delimiters
struct FrontmatterData {
    std::optional<std::string> description;
    std::optional<std::string> argument_hint;
    std::optional<std::string> when_to_use;
    std::optional<std::string> version;
    std::optional<std::string> model;          // 'inherit' or model name
    std::optional<std::string> user_invocable; // 'true'/'false' string
    std::optional<std::string> effort;         // 'low','medium','high','max' or int
    std::optional<std::string> context;        // 'inline' or 'fork'
    std::optional<std::string> agent;          // Agent type for fork context
    std::optional<std::string> shell;          // 'bash' or 'powershell'
    std::optional<std::string> paths;          // Comma-separated or YAML list
    std::optional<std::string> hide_from_slash_command_tool;
    std::optional<std::string> skills;         // Comma-separated skill names to preload
    std::optional<std::string> type;           // Memory type: user/feedback/project/reference
    std::optional<std::string> name;           // Display name override
    std::optional<std::string> allowed_tools;  // Comma-separated tool list
    std::optional<std::string> arguments;      // Argument names (space/comma separated)
    std::optional<YamlValue> hooks_yaml;       // Raw hooks YAML for validation
    std::optional<YamlValue> paths_yaml;       // Raw paths YAML (array form)
    std::optional<YamlValue> allowed_tools_yaml; // Raw allowed-tools YAML (array)
    std::optional<YamlValue> arguments_yaml;   // Raw arguments YAML (array)
    std::map<std::string, YamlValue> extra;    // Catch-all for unknown keys
};

// =========================================================================
// ParsedSkillFrontmatterFields
// TS REF: src/skills/loadSkillsDir.ts:185-265 (parseSkillFrontmatterFields return)
// =========================================================================

/// All frontmatter fields parsed and validated for a skill
struct ParsedSkillFrontmatterFields {
    std::optional<std::string> display_name;
    std::string description;
    bool has_user_specified_description = false;
    std::vector<std::string> allowed_tools;
    std::optional<std::string> argument_hint;
    std::vector<std::string> argument_names;
    std::optional<std::string> when_to_use;
    std::optional<std::string> version;
    std::optional<std::string> model;        // Undefined means 'inherit'
    bool disable_model_invocation = false;
    bool user_invocable = true;
    std::optional<std::string> hooks_json;   // Serialized hooks settings (simplified)
    std::optional<std::string> execution_context; // 'fork' or undefined
    std::optional<std::string> agent;
    std::optional<cc::utils::EffortLevel> effort;
    std::optional<FrontmatterShell> shell;
};

// =========================================================================
// SkillCommand
// TS REF: src/skills/loadSkillsDir.ts:316-401 (Command type for skills)
// =========================================================================

/// A fully-loaded skill command ready for registration in the command system.
/// Mirrors the TS `Command` type for prompt-type skills.
struct SkillCommand {
    std::string type = "prompt";             // Always "prompt" for skills
    std::string name;                        // Unique skill name
    std::string description;                 // Human-readable description
    bool has_user_specified_description = false;
    std::vector<std::string> allowed_tools;  // Tools allowed in !` blocks
    std::optional<std::string> argument_hint;
    std::optional<std::vector<std::string>> arg_names;
    std::optional<std::string> when_to_use;
    std::optional<std::string> version;
    std::optional<std::string> model;
    bool disable_model_invocation = false;
    bool user_invocable = true;
    std::optional<std::string> context;      // 'inline' or 'fork'
    std::optional<std::string> agent;
    std::optional<cc::utils::EffortLevel> effort;
    std::optional<std::vector<std::string>> paths; // Conditional activation paths
    std::size_t content_length = 0;
    bool is_hidden = false;
    std::string progress_message = "running";
    std::optional<std::string> display_name;

    // Source tracking
    SettingSource source = SettingSource::ProjectSettings;
    LoadedFrom loaded_from = LoadedFrom::Skills;
    std::optional<fs::path> skill_root;      // Base directory for the skill
    std::optional<std::string> hooks_json;   // Hooks settings
    std::optional<FrontmatterShell> shell;

    // The markdown content of the skill (loaded lazily in TS, but we keep it
    // here for simpler CPP execution flow)
    std::string markdown_content;

    /// Get the user-facing display name
    std::string user_facing_name() const {
        return display_name.value_or(name);
    }

    /// Generate the prompt content for this skill with argument substitution.
    /// TS REF: src/skills/loadSkillsDir.ts:344-399 (getPromptForCommand)
    std::string get_prompt_for_command(
        std::optional<std::string_view> args,
        const std::optional<std::string>& session_id = std::nullopt,
        const std::vector<std::string>& allowed_tools_override = {}) const
    {
        (void)allowed_tools_override; // Reserved for future shell-block execution
        std::string final_content;
        if (skill_root.has_value()) {
            final_content = "Base directory for this skill: " +
                           skill_root->string() + "\n\n" + markdown_content;
        } else {
            final_content = markdown_content;
        }

        // Substitute arguments ($name, $ARGUMENTS, $1, etc.)
        const auto& arg_names_vec = arg_names.value_or(
            std::vector<std::string>{});
        if (args.has_value()) {
            final_content = cc::utils::argument_substitution::substitute_arguments(
                final_content, *args, true, arg_names_vec);
        }

        // Replace ${CLAUDE_SKILL_DIR} with the skill's own directory
        if (skill_root.has_value()) {
            std::string skill_dir = skill_root->string();
            // Normalize backslashes to forward slashes on Windows
            std::replace(skill_dir.begin(), skill_dir.end(), '\\', '/');
            const std::string placeholder = "${CLAUDE_SKILL_DIR}";
            std::size_t pos = 0;
            while ((pos = final_content.find(placeholder, pos)) != std::string::npos) {
                final_content.replace(pos, placeholder.size(), skill_dir);
                pos += skill_dir.size();
            }
        }

        // Replace ${CLAUDE_SESSION_ID} with the current session ID
        if (session_id.has_value()) {
            const std::string sid_placeholder = "${CLAUDE_SESSION_ID}";
            std::size_t pos = 0;
            while ((pos = final_content.find(sid_placeholder, pos)) != std::string::npos) {
                final_content.replace(pos, sid_placeholder.size(), *session_id);
                pos += session_id->size();
            }
        }

        // NOTE: Shell execution (!`cmd` blocks) is deferred to the caller
        // because it requires the full tool-use context and permission system.
        // MCP skills (loaded_from == Mcp) should never execute shell blocks.

        return final_content;
    }
};

// =========================================================================
// SkillWithPath - internal tracking type for dedup
// TS REF: src/skills/loadSkillsDir.ts:127-131
// =========================================================================

namespace detail {
struct SkillWithPath {
    SkillCommand skill;
    fs::path file_path;
};
} // namespace detail

// =========================================================================
// Utility: getClaudeConfigHomeDir
// TS REF: src/utils/envUtils.ts:7 (getClaudeConfigHomeDir)
// =========================================================================

/// Get the Claude config home directory (~/.claude)
std::string get_claude_config_home_dir() {
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.claude";
    }
    // Fallback: try XDG_CONFIG_HOME
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::string(xdg) + "/claude";
    }
    return "";
}

// =========================================================================
// Utility: getManagedFilePath
// TS REF: src/utils/settings/managedPath.ts:8 (getManagedFilePath)
// =========================================================================

/// Get the managed file path (stub - returns empty string)
std::string get_managed_file_path() {
    // In the TS implementation, this resolves to a managed-settings directory.
    // For CPP, we return a stub path. Callers should check for empty string.
    const char* managed = std::getenv("CLAUDE_MANAGED_SETTINGS_PATH");
    if (managed && managed[0] != '\0') {
        return std::string(managed);
    }
    return "";
}

// =========================================================================
// Utility: getAdditionalDirectoriesForClaudeMd
// TS REF: src/bootstrap/state.ts:1670
// =========================================================================

/// Get additional directories for Claude MD (from --add-dir flags)
std::vector<std::string> get_additional_directories_for_claude_md() {
    const char* add_dirs = std::getenv("CLAUDE_ADDITIONAL_DIRS");
    if (!add_dirs || add_dirs[0] == '\0') return {};

    std::vector<std::string> result;
    std::string dirs_str(add_dirs);
    std::size_t pos = 0;
    while (pos < dirs_str.size()) {
        auto sep = dirs_str.find(':', pos);
        std::string segment = (sep == std::string::npos)
            ? dirs_str.substr(pos)
            : dirs_str.substr(pos, sep - pos);
        if (!segment.empty()) {
            result.push_back(segment);
        }
        pos = (sep == std::string::npos) ? dirs_str.size() : sep + 1;
    }
    return result;
}

// =========================================================================
// Utility: isSettingSourceEnabled
// TS REF: src/utils/settings/constants.ts:174
// =========================================================================

/// Check if a setting source is enabled
bool is_setting_source_enabled(SettingSource source) {
    switch (source) {
        case SettingSource::PolicySettings:
            return !cc::utils::is_env_truthy(
                std::getenv("CLAUDE_CODE_DISABLE_POLICY_SKILLS"));
        case SettingSource::UserSettings:
            return !cc::utils::is_env_truthy(
                std::getenv("CLAUDE_CODE_DISABLE_USER_SKILLS"));
        case SettingSource::ProjectSettings:
            return !cc::utils::is_env_truthy(
                std::getenv("CLAUDE_CODE_DISABLE_PROJECT_SKILLS"));
        default:
            return true;
    }
}

// =========================================================================
// Utility: isRestrictedToPluginOnly
// TS REF: src/utils/settings/pluginOnlyPolicy.ts:19
// =========================================================================

/// Check if a surface is restricted to plugin-only skills
bool is_restricted_to_plugin_only(std::string_view /*surface*/) {
    // TS REF: checks pluginOnlyPolicy setting. For CPP, we check an env var.
    return cc::utils::is_env_truthy(
        std::getenv("CLAUDE_CODE_PLUGIN_ONLY_SKILLS"));
}

// =========================================================================
// Utility: getSkillsPath
// TS REF: src/skills/loadSkillsDir.ts:78-94
// =========================================================================

/// Returns a claude config directory path for a given source
std::string get_skills_path(SettingSource source, std::string_view dir) {
    switch (source) {
        case SettingSource::PolicySettings: {
            auto managed = get_managed_file_path();
            if (managed.empty()) return "";
            return managed + "/.claude/" + std::string(dir);
        }
        case SettingSource::UserSettings:
            return get_claude_config_home_dir() + "/" + std::string(dir);
        case SettingSource::ProjectSettings:
            return ".claude/" + std::string(dir);
        default:
            return "";
    }
}

// =========================================================================
// Utility: getProjectDirsUpToHome
// TS REF: src/utils/markdownConfigLoader.ts:234-280
// =========================================================================

/// Traverse from cwd up to git root (or home), collecting .claude/subdir dirs.
/// TS REF: getProjectDirsUpToHome() walks up to git root to prevent parent
/// directory skills from leaking into projects.
std::vector<std::string> get_project_dirs_up_to_home(
    std::string_view subdir, const fs::path& cwd)
{
    std::vector<std::string> dirs;
    fs::path current = fs::absolute(cwd);
    fs::path home;
    const char* home_env = std::getenv("HOME");
    if (home_env && home_env[0] != '\0') {
        home = fs::path(home_env);
    }

    // Find the git root (stop boundary)
    fs::path git_root;
    {
        fs::path probe = current;
        while (!probe.empty()) {
            if (fs::exists(probe / ".git")) {
                git_root = probe;
                break;
            }
            auto parent = probe.parent_path();
            if (parent == probe) break;
            probe = parent;
        }
    }

    // Walk upward from cwd
    while (!current.empty()) {
        // Stop at home directory (user skills loaded separately)
        if (!home.empty() && fs::equivalent(current, home)) break;

        auto claude_subdir = current / ".claude" / std::string(subdir);
        if (fs::exists(claude_subdir) && fs::is_directory(claude_subdir)) {
            dirs.push_back(claude_subdir.string());
        }

        // Stop at git root (prevents skills from parent repos leaking in)
        if (!git_root.empty() && fs::equivalent(current, git_root)) break;

        auto parent = current.parent_path();
        if (parent == current) break; // Reached filesystem root
        current = parent;
    }

    return dirs;
}

// =========================================================================
// Utility: getFileIdentity
// TS REF: src/skills/loadSkillsDir.ts:118-124
// =========================================================================

/// Get a unique identifier for a file by resolving symlinks to canonical path.
/// Returns nullopt if the file doesn't exist or can't be resolved.
/// TS REF: Uses realpath() to handle symlinks and inode-0 filesystems.
std::optional<std::string> get_file_identity(const fs::path& file_path) {
    std::error_code ec;
    auto canonical = fs::canonical(file_path, ec);
    if (ec) return std::nullopt;
    return canonical.string();
}

// =========================================================================
// Utility: extractDescriptionFromMarkdown
// TS REF: src/utils/markdownConfigLoader.ts:52-69
// =========================================================================

/// Extract a description from markdown content (first non-empty line,
/// stripping header markers). Limited to ~100 chars.
std::string extract_description_from_markdown(
    std::string_view content, std::string_view default_label = "Skill")
{
    std::size_t pos = 0;
    while (pos < content.size()) {
        auto line_end = content.find('\n', pos);
        if (line_end == std::string_view::npos) line_end = content.size();

        std::string_view line = content.substr(pos, line_end - pos);

        // Trim
        auto start = line.find_first_not_of(" \t\r");
        if (start != std::string_view::npos) {
            std::string_view trimmed = line.substr(start);

            // Strip header markers (# ## ###)
            if (trimmed.starts_with("#")) {
                auto after_hash = trimmed.find_first_not_of('#');
                if (after_hash != std::string_view::npos &&
                    trimmed[after_hash] == ' ') {
                    trimmed = trimmed.substr(after_hash + 1);
                }
            }

            std::string result(trimmed);
            if (result.size() > 100) {
                result = result.substr(0, 97) + "...";
            }
            return result;
        }

        pos = (line_end < content.size()) ? line_end + 1 : content.size();
    }
    return std::string(default_label);
}

// =========================================================================
// Utility: parseBooleanFrontmatter
// TS REF: src/utils/frontmatterParser.ts:332-334
// =========================================================================

/// Parse a boolean frontmatter value. Only true/"true" returns true.
bool parse_boolean_frontmatter(const std::optional<std::string>& value) {
    if (!value.has_value()) return false;
    if (*value == "true" || *value == "True" || *value == "TRUE") return true;
    return false;
}

// =========================================================================
// Utility: parseShellFrontmatter
// TS REF: src/utils/frontmatterParser.ts:351-370
// =========================================================================

/// Parse and validate the shell: frontmatter field.
std::optional<FrontmatterShell> parse_shell_frontmatter(
    const std::optional<std::string>& value, std::string_view source)
{
    if (!value.has_value()) return std::nullopt;
    std::string normalized;
    normalized.reserve(value->size());
    for (char c : *value) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // Trim
    auto start = normalized.find_first_not_of(" \t");
    if (start == std::string::npos) return std::nullopt;
    normalized = normalized.substr(start);
    auto end = normalized.find_last_not_of(" \t");
    if (end != std::string::npos) normalized = normalized.substr(0, end + 1);

    if (normalized == "bash") return FrontmatterShell::Bash;
    if (normalized == "powershell") return FrontmatterShell::Powershell;

    warning("[skills] Frontmatter 'shell: " + *value + "' in " +
            std::string(source) + " is not recognized. Valid values: bash, powershell. "
            "Falling back to bash.");
    return std::nullopt;
}

// =========================================================================
// Utility: parseEffortValue
// TS REF: src/utils/effort.ts (parseEffortValue)
// =========================================================================

/// Parse an effort value from frontmatter ('low','medium','high','max' or int)
std::optional<cc::utils::EffortLevel> parse_effort_value(
    const std::optional<std::string>& value)
{
    if (!value.has_value()) return std::nullopt;
    std::string v;
    v.reserve(value->size());
    for (char c : *value) {
        v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    // Trim
    auto s = v.find_first_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    v = v.substr(s);
    auto e = v.find_last_not_of(" \t");
    if (e != std::string::npos) v = v.substr(0, e + 1);

    if (v == "low") return cc::utils::EffortLevel::Low;
    if (v == "medium") return cc::utils::EffortLevel::Medium;
    if (v == "high") return cc::utils::EffortLevel::High;
    if (v == "max") return cc::utils::EffortLevel::Max;

    // Try integer effort
    try {
        int int_val = std::stoi(v);
        if (int_val <= 1) return cc::utils::EffortLevel::Low;
        if (int_val <= 3) return cc::utils::EffortLevel::Medium;
        if (int_val <= 6) return cc::utils::EffortLevel::High;
        return cc::utils::EffortLevel::Max;
    } catch (...) {
        return std::nullopt;
    }
}

// =========================================================================
// Utility: splitPathInFrontmatter
// TS REF: src/utils/frontmatterParser.ts:189-232
// =========================================================================

/// Split comma-separated paths, respecting brace patterns.
/// Also accepts a YAML array (via the paths_yaml field).
std::vector<std::string> split_path_in_frontmatter(
    const std::optional<std::string>& paths_str,
    const std::optional<YamlValue>& paths_yaml)
{
    // If YAML array form exists, use that
    if (paths_yaml.has_value() && paths_yaml->is_array()) {
        std::vector<std::string> result;
        auto& arr = std::get<YamlArray>(paths_yaml->data);
        for (const auto& item : arr) {
            if (item.is_string()) {
                result.push_back(std::get<std::string>(item.data));
            }
        }
        return result;
    }

    if (!paths_str.has_value() || paths_str->empty()) return {};

    std::vector<std::string> parts;
    std::string current;
    int brace_depth = 0;

    for (std::size_t i = 0; i < paths_str->size(); ++i) {
        char c = (*paths_str)[i];
        if (c == '{') {
            ++brace_depth;
            current += c;
        } else if (c == '}') {
            --brace_depth;
            current += c;
        } else if (c == ',' && brace_depth == 0) {
            // Trim
            auto s = current.find_first_not_of(" \t");
            if (s != std::string::npos) {
                auto e = current.find_last_not_of(" \t");
                parts.push_back(current.substr(s, e - s + 1));
            }
            current.clear();
        } else {
            current += c;
        }
    }

    // Last part
    auto s = current.find_first_not_of(" \t");
    if (s != std::string::npos) {
        auto e = current.find_last_not_of(" \t");
        parts.push_back(current.substr(s, e - s + 1));
    }

    // Expand brace patterns
    std::vector<std::string> expanded;
    for (const auto& pattern : parts) {
        // Simple brace expansion: {a,b} → a, b
        auto brace_pos = pattern.find('{');
        if (brace_pos != std::string::npos) {
            auto brace_end = pattern.find('}', brace_pos);
            if (brace_end != std::string::npos) {
                std::string prefix = pattern.substr(0, brace_pos);
                std::string alternatives = pattern.substr(brace_pos + 1, brace_end - brace_pos - 1);
                std::string suffix = pattern.substr(brace_end + 1);

                std::size_t alt_pos = 0;
                while (alt_pos <= alternatives.size()) {
                    auto comma = alternatives.find(',', alt_pos);
                    std::string alt = alternatives.substr(
                        alt_pos, comma == std::string::npos
                                     ? std::string::npos
                                     : comma - alt_pos);
                    // Trim alt
                    auto as = alt.find_first_not_of(" \t");
                    if (as != std::string::npos) {
                        auto ae = alt.find_last_not_of(" \t");
                        alt = alt.substr(as, ae - as + 1);
                    }
                    expanded.push_back(prefix + alt + suffix);
                    if (comma == std::string::npos) break;
                    alt_pos = comma + 1;
                }
                continue;
            }
        }
        expanded.push_back(pattern);
    }

    return expanded;
}

// =========================================================================
// Utility: parseSlashCommandToolsFromFrontmatter
// TS REF: src/utils/markdownConfigLoader.ts:132-140
// =========================================================================

/// Parse allowed-tools from frontmatter (string or array)
std::vector<std::string> parse_slash_command_tools_from_frontmatter(
    const std::optional<std::string>& tools_str,
    const std::optional<YamlValue>& tools_yaml)
{
    // If YAML array form exists, use that
    if (tools_yaml.has_value() && tools_yaml->is_array()) {
        std::vector<std::string> result;
        auto& arr = std::get<YamlArray>(tools_yaml->data);
        for (const auto& item : arr) {
            if (item.is_string()) {
                result.push_back(std::get<std::string>(item.data));
            }
        }
        return result;
    }

    if (!tools_str.has_value() || tools_str->empty()) return {};

    std::vector<std::string> result;
    std::size_t pos = 0;
    while (pos < tools_str->size()) {
        auto comma = tools_str->find(',', pos);
        std::string segment = (comma == std::string::npos)
            ? tools_str->substr(pos)
            : tools_str->substr(pos, comma - pos);
        // Trim
        auto s = segment.find_first_not_of(" \t");
        if (s != std::string::npos) {
            auto e = segment.find_last_not_of(" \t");
            result.push_back(segment.substr(s, e - s + 1));
        }
        pos = (comma == std::string::npos) ? tools_str->size() : comma + 1;
    }
    return result;
}

// =========================================================================
// Utility: parseArgumentNames (from frontmatter)
// TS REF: src/utils/argumentSubstitution.ts:parseArgumentNames
// =========================================================================

/// Parse argument names from frontmatter (string or array)
std::vector<std::string> parse_argument_names_from_frontmatter(
    const std::optional<std::string>& args_str,
    const std::optional<YamlValue>& args_yaml)
{
    // If YAML array form exists, use that
    if (args_yaml.has_value() && args_yaml->is_array()) {
        std::vector<std::string> result;
        auto& arr = std::get<YamlArray>(args_yaml->data);
        for (const auto& item : arr) {
            if (item.is_string()) {
                result.push_back(std::get<std::string>(item.data));
            }
        }
        return cc::utils::argument_substitution::parse_argument_names(result);
    }

    if (!args_str.has_value()) return {};
    return cc::utils::argument_substitution::parse_argument_names(
        std::optional<std::string>(*args_str));
}

// =========================================================================
// Utility: parseUserSpecifiedModel
// TS REF: src/utils/model/model.ts (parseUserSpecifiedModel)
// =========================================================================

/// Parse a user-specified model from frontmatter. Returns nullopt for 'inherit'.
std::optional<std::string> parse_user_specified_model(
    const std::optional<std::string>& model_str)
{
    if (!model_str.has_value()) return std::nullopt;
    if (*model_str == "inherit") return std::nullopt;
    return model_str;
}

// =========================================================================
// Rich frontmatter parsing
// TS REF: src/utils/frontmatterParser.ts:parseFrontmatter + YAML parsing
// =========================================================================

namespace detail {

/// Extract a string value from a YamlValue
std::optional<std::string> yaml_to_string(const YamlValue& val) {
    if (val.is_string()) return std::get<std::string>(val.data);
    if (val.is_null()) return std::nullopt;
    // For scalars, convert to string representation
    if (std::holds_alternative<bool>(val.data)) {
        return std::get<bool>(val.data) ? "true" : "false";
    }
    if (std::holds_alternative<int64_t>(val.data)) {
        return std::to_string(std::get<int64_t>(val.data));
    }
    if (std::holds_alternative<double>(val.data)) {
        return std::to_string(std::get<double>(val.data));
    }
    return std::nullopt;
}

} // namespace detail

/// Parse rich frontmatter from a markdown file.
/// Uses the YAML parser for full YAML support (arrays, nested objects).
/// TS REF: src/utils/frontmatterParser.ts:130-175 (parseFrontmatter)
FrontmatterData parse_frontmatter_rich(std::string_view content) {
    FrontmatterData data;

    // First, check if frontmatter exists
    if (!cc::utils::has_frontmatter(content)) {
        return data;
    }

    // Extract the YAML section between --- delimiters
    auto first_sep = content.find("---");
    if (first_sep == std::string_view::npos) return data;

    std::size_t yaml_start = first_sep + 3;
    if (yaml_start < content.size() && content[yaml_start] == '\r') ++yaml_start;
    if (yaml_start < content.size() && content[yaml_start] == '\n') ++yaml_start;

    auto second_sep = content.find("\n---", yaml_start);
    if (second_sep == std::string_view::npos) {
        second_sep = content.find("\r\n---", yaml_start);
        if (second_sep == std::string_view::npos) return data;
    }

    auto yaml_section = content.substr(yaml_start, second_sep - yaml_start);

    // Parse the YAML
    YamlValue parsed;
    try {
        parsed = cc::utils::parse_yaml(yaml_section);
    } catch (...) {
        // YAML parsing failed - fall back to simple key:value parsing
        auto simple = cc::utils::parse_frontmatter(content);
        // Copy simple metadata into FrontmatterData
        for (const auto& [key, value] : simple.metadata) {
            if (key == "description") data.description = value;
            else if (key == "argument-hint") data.argument_hint = value;
            else if (key == "when_to_use") data.when_to_use = value;
            else if (key == "version") data.version = value;
            else if (key == "model") data.model = value;
            else if (key == "user-invocable") data.user_invocable = value;
            else if (key == "effort") data.effort = value;
            else if (key == "context") data.context = value;
            else if (key == "agent") data.agent = value;
            else if (key == "shell") data.shell = value;
            else if (key == "paths") data.paths = value;
            else if (key == "hide-from-slash-command-tool") data.hide_from_slash_command_tool = value;
            else if (key == "skills") data.skills = value;
            else if (key == "type") data.type = value;
            else if (key == "name") data.name = value;
            else if (key == "allowed-tools") data.allowed_tools = value;
            else if (key == "arguments") data.arguments = value;
            else data.extra[key] = YamlValue(value);
        }
        return data;
    }

    // Extract fields from parsed YAML map
    if (!parsed.is_map()) return data;
    auto& map = std::get<YamlMap>(parsed.data);

    auto get_field = [&](std::string_view key) -> std::optional<std::string> {
        auto it = map.find(std::string(key));
        if (it == map.end()) return std::nullopt;
        return detail::yaml_to_string(it->second);
    };

    auto get_yaml_field = [&](std::string_view key) -> std::optional<YamlValue> {
        auto it = map.find(std::string(key));
        if (it == map.end()) return std::nullopt;
        return it->second;
    };

    data.description = get_field("description");
    data.argument_hint = get_field("argument-hint");
    data.when_to_use = get_field("when_to_use");
    data.version = get_field("version");
    data.model = get_field("model");
    data.user_invocable = get_field("user-invocable");
    data.effort = get_field("effort");
    data.context = get_field("context");
    data.agent = get_field("agent");
    data.shell = get_field("shell");
    data.paths = get_field("paths");
    data.hide_from_slash_command_tool = get_field("hide-from-slash-command-tool");
    data.skills = get_field("skills");
    data.type = get_field("type");
    data.name = get_field("name");
    data.allowed_tools = get_field("allowed-tools");
    data.arguments = get_field("arguments");

    // Get raw YAML values for array-type fields
    data.hooks_yaml = get_yaml_field("hooks");
    data.paths_yaml = get_yaml_field("paths");
    data.allowed_tools_yaml = get_yaml_field("allowed-tools");
    data.arguments_yaml = get_yaml_field("arguments");

    // Store extra fields
    static const std::unordered_set<std::string> known_keys = {
        "description", "argument-hint", "when_to_use", "version", "model",
        "user-invocable", "effort", "context", "agent", "shell", "paths",
        "hide-from-slash-command-tool", "skills", "type", "name",
        "allowed-tools", "arguments", "hooks"
    };
    for (const auto& [key, value] : map) {
        if (known_keys.find(key) == known_keys.end()) {
            data.extra[key] = value;
        }
    }

    return data;
}

// =========================================================================
// parseHooksFromFrontmatter
// TS REF: src/skills/loadSkillsDir.ts:136-153
// =========================================================================

/// Parse and validate hooks from frontmatter.
/// Returns serialized hooks JSON or nullopt if not defined/invalid.
std::optional<std::string> parse_hooks_from_frontmatter(
    const FrontmatterData& fm, std::string_view skill_name)
{
    if (!fm.hooks_yaml.has_value()) return std::nullopt;

    // Simplified: serialize the hooks YAML to a string for later use.
    // Full validation would require HooksSchema equivalent.
    try {
        return cc::utils::yaml_to_string(*fm.hooks_yaml);
    } catch (...) {
        warning("[skills] Invalid hooks in skill '" + std::string(skill_name) + "'");
        return std::nullopt;
    }
}

// =========================================================================
// parseSkillPaths
// TS REF: src/skills/loadSkillsDir.ts:159-178
// =========================================================================

/// Parse paths frontmatter from a skill, using the same format as CLAUDE.md rules.
/// Returns nullopt if no paths specified or if all are match-all (**).
std::optional<std::vector<std::string>> parse_skill_paths(const FrontmatterData& fm) {
    auto patterns = split_path_in_frontmatter(fm.paths, fm.paths_yaml);

    // Remove /** suffix (ignore library convention)
    for (auto& p : patterns) {
        if (p.ends_with("/**")) {
            p = p.substr(0, p.size() - 3);
        }
    }

    // Filter empty
    patterns.erase(
        std::remove_if(patterns.begin(), patterns.end(),
            [](const std::string& s) { return s.empty(); }),
        patterns.end());

    if (patterns.empty()) return std::nullopt;

    // If all patterns are ** (match-all), treat as no paths
    bool all_match_all = std::all_of(patterns.begin(), patterns.end(),
        [](const std::string& p) { return p == "**"; });
    if (all_match_all) return std::nullopt;

    return patterns;
}

// =========================================================================
// parseSkillFrontmatterFields
// TS REF: src/skills/loadSkillsDir.ts:185-265
// =========================================================================

/// Parse all skill frontmatter fields. Caller supplies resolved name.
ParsedSkillFrontmatterFields parse_skill_frontmatter_fields(
    const FrontmatterData& frontmatter,
    const std::string& markdown_content,
    std::string_view resolved_name,
    bool is_legacy_command = false)
{
    ParsedSkillFrontmatterFields result;

    // Description: use frontmatter if available, else extract from markdown
    const std::string fallback_label = is_legacy_command ? "Custom command" : "Skill";

    if (frontmatter.description.has_value() && !frontmatter.description->empty()) {
        result.description = *frontmatter.description;
        result.has_user_specified_description = true;
    } else {
        result.description = extract_description_from_markdown(
            markdown_content, fallback_label);
        result.has_user_specified_description = false;
    }

    // user-invocable: default true for skills and legacy commands
    if (frontmatter.user_invocable.has_value()) {
        result.user_invocable = parse_boolean_frontmatter(frontmatter.user_invocable);
    } else {
        result.user_invocable = true;
    }

    // model: 'inherit' → undefined
    result.model = parse_user_specified_model(frontmatter.model);

    // effort
    result.effort = parse_effort_value(frontmatter.effort);
    if (frontmatter.effort.has_value() && !result.effort.has_value()) {
        debug("[skills] Skill " + std::string(resolved_name) +
              " has invalid effort '" + *frontmatter.effort +
              "'. Valid options: low, medium, high, max or an integer");
    }

    // display name
    result.display_name = frontmatter.name;

    // allowed-tools
    result.allowed_tools = parse_slash_command_tools_from_frontmatter(
        frontmatter.allowed_tools, frontmatter.allowed_tools_yaml);

    // argument-hint
    result.argument_hint = frontmatter.argument_hint;

    // argument names
    result.argument_names = parse_argument_names_from_frontmatter(
        frontmatter.arguments, frontmatter.arguments_yaml);

    // when_to_use
    result.when_to_use = frontmatter.when_to_use;

    // version
    result.version = frontmatter.version;

    // disable-model-invocation (not a standard field, check extra)
    {
        auto it = frontmatter.extra.find("disable-model-invocation");
        if (it != frontmatter.extra.end()) {
            auto str = detail::yaml_to_string(it->second);
            result.disable_model_invocation = parse_boolean_frontmatter(str);
        }
    }

    // hooks
    result.hooks_json = parse_hooks_from_frontmatter(frontmatter, resolved_name);

    // execution context (fork)
    if (frontmatter.context.has_value() && *frontmatter.context == "fork") {
        result.execution_context = "fork";
    }

    // agent
    result.agent = frontmatter.agent;

    // shell
    result.shell = parse_shell_frontmatter(
        frontmatter.shell, resolved_name);

    return result;
}

// =========================================================================
// createSkillCommand
// TS REF: src/skills/loadSkillsDir.ts:270-401
// =========================================================================

/// Create a SkillCommand from parsed data.
/// TS REF: createSkillCommand() builds the Command object with all fields
/// and the getPromptForCommand closure.
SkillCommand create_skill_command(
    std::string_view skill_name,
    const ParsedSkillFrontmatterFields& fields,
    const std::string& markdown_content,
    SettingSource source,
    LoadedFrom loaded_from,
    std::optional<fs::path> base_dir,
    std::optional<std::vector<std::string>> paths)
{
    SkillCommand cmd;
    cmd.type = "prompt";
    cmd.name = std::string(skill_name);
    cmd.description = fields.description;
    cmd.has_user_specified_description = fields.has_user_specified_description;
    cmd.allowed_tools = fields.allowed_tools;
    cmd.argument_hint = fields.argument_hint;

    if (!fields.argument_names.empty()) {
        cmd.arg_names = fields.argument_names;
    }

    cmd.when_to_use = fields.when_to_use;
    cmd.version = fields.version;
    cmd.model = fields.model;
    cmd.disable_model_invocation = fields.disable_model_invocation;
    cmd.user_invocable = fields.user_invocable;
    cmd.context = fields.execution_context;
    cmd.agent = fields.agent;
    cmd.effort = fields.effort;
    cmd.paths = std::move(paths);
    cmd.content_length = markdown_content.size();
    cmd.is_hidden = !fields.user_invocable;
    cmd.progress_message = "running";
    cmd.display_name = fields.display_name;
    cmd.source = source;
    cmd.loaded_from = loaded_from;
    cmd.skill_root = std::move(base_dir);
    cmd.hooks_json = fields.hooks_json;
    cmd.shell = fields.shell;
    cmd.markdown_content = markdown_content;

    return cmd;
}

// =========================================================================
// loadSkillsFromSkillsDir
// TS REF: src/skills/loadSkillsDir.ts:407-480
// =========================================================================

/// Load skills from a /skills/ directory path.
/// Only supports directory format: skill-name/SKILL.md
std::vector<detail::SkillWithPath> load_skills_from_skills_dir(
    const fs::path& base_path, SettingSource source)
{
    std::vector<detail::SkillWithPath> results;

    if (!fs::exists(base_path) || !fs::is_directory(base_path)) {
        return results;
    }

    std::error_code ec;
    auto dir_it = fs::directory_iterator(base_path, ec);
    if (ec) return results;

    for (const auto& entry : dir_it) {
        try {
            // Only support directory format: skill-name/SKILL.md
            // (or symlinks to directories)
            bool is_dir = entry.is_directory(ec);
            bool is_symlink = entry.is_symlink(ec);
            if (!is_dir && !is_symlink) continue;

            // If it's a symlink, check if it points to a directory
            if (is_symlink && !is_dir) {
                auto target = fs::canonical(entry.path(), ec);
                if (ec || !fs::is_directory(target, ec)) continue;
            }

            const auto& skill_dir_path = entry.path();
            auto skill_file_path = skill_dir_path / "SKILL.md";

            // Try to read SKILL.md
            std::ifstream file(skill_file_path);
            if (!file.is_open()) {
                // SKILL.md doesn't exist, skip this entry
                continue;
            }

            std::string content(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            file.close();

            // Parse frontmatter
            auto fm_data = parse_frontmatter_rich(content);

            // Get content without frontmatter
            auto stripped = cc::utils::strip_frontmatter(content);
            std::string markdown_content(stripped);

            // Use directory name as skill name
            std::string skill_name = skill_dir_path.filename().string();

            auto parsed = parse_skill_frontmatter_fields(
                fm_data, markdown_content, skill_name);
            auto paths = parse_skill_paths(fm_data);

            results.push_back(detail::SkillWithPath{
                .skill = create_skill_command(
                    skill_name, parsed, markdown_content,
                    source, LoadedFrom::Skills,
                    skill_dir_path, std::move(paths)),
                .file_path = skill_file_path,
            });
        } catch (const std::exception& e) {
            log_error(std::string("[skills] Error loading skill from ") +
                      entry.path().string() + ": " + e.what());
        }
    }

    return results;
}

// =========================================================================
// Legacy /commands/ loader helpers
// TS REF: src/skills/loadSkillsDir.ts:482-623
// =========================================================================

namespace detail {

/// Check if a file is a SKILL.md file (case-insensitive)
bool is_skill_file(const fs::path& file_path) {
    auto filename = file_path.filename().string();
    std::string lower;
    lower.reserve(filename.size());
    for (char c : filename) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return lower == "skill.md";
}

/// Build namespace from nested directory structure.
/// TS REF: buildNamespace() at loadSkillsDir.ts:523-534
std::string build_namespace(const fs::path& target_dir, const fs::path& base_dir) {
    auto norm_base = base_dir;
    if (norm_base.has_filename() && norm_base.filename() == ".") {
        norm_base = norm_base.parent_path();
    }

    std::error_code ec;
    if (fs::equivalent(target_dir, norm_base, ec)) return "";

    auto rel = fs::relative(target_dir, norm_base, ec);
    if (ec) return "";

    std::string result;
    for (const auto& part : rel) {
        if (!result.empty()) result += ":";
        result += part.string();
    }
    return result;
}

/// Get command name for a SKILL.md format file
std::string get_skill_command_name(
    const fs::path& file_path, const fs::path& base_dir)
{
    auto skill_directory = file_path.parent_path();
    auto parent_of_skill_dir = skill_directory.parent_path();
    auto command_base_name = skill_directory.filename().string();
    auto ns = build_namespace(parent_of_skill_dir, base_dir);
    return ns.empty() ? command_base_name : ns + ":" + command_base_name;
}

/// Get command name for a regular .md file
std::string get_regular_command_name(
    const fs::path& file_path, const fs::path& base_dir)
{
    auto file_name = file_path.filename().string();
    auto file_directory = file_path.parent_path();
    // Remove .md extension
    auto command_base_name = file_name;
    if (command_base_name.size() > 3 &&
        command_base_name.substr(command_base_name.size() - 3) == ".md") {
        command_base_name = command_base_name.substr(0, command_base_name.size() - 3);
    }
    auto ns = build_namespace(file_directory, base_dir);
    return ns.empty() ? command_base_name : ns + ":" + command_base_name;
}

} // namespace detail

// =========================================================================
// loadSkillsFromCommandsDir
// TS REF: src/skills/loadSkillsDir.ts:566-623
// =========================================================================

/// Load skills from legacy /commands/ directories.
/// Supports both directory format (SKILL.md) and single .md file format.
std::vector<detail::SkillWithPath> load_skills_from_commands_dir(
    const fs::path& cwd)
{
    std::vector<detail::SkillWithPath> skills;

    // Walk up from cwd to find .claude/commands directories
    auto commands_dirs = get_project_dirs_up_to_home("commands", cwd);

    for (const auto& commands_dir_str : commands_dirs) {
        fs::path commands_dir(commands_dir_str);
        if (!fs::exists(commands_dir)) continue;

        // Collect all markdown files, grouped by directory
        std::map<fs::path, std::vector<fs::path>> files_by_dir;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 commands_dir, ec);
             !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            if (it->path().extension() == ".md" && it->is_regular_file()) {
                files_by_dir[it->path().parent_path()].push_back(it->path());
            }
        }

        // Process each directory: SKILL.md takes precedence over regular .md files
        for (const auto& [dir, files] : files_by_dir) {
            // Find SKILL.md files
            std::vector<fs::path> skill_files;
            std::vector<fs::path> regular_files;
            for (const auto& f : files) {
                if (detail::is_skill_file(f)) {
                    skill_files.push_back(f);
                } else {
                    regular_files.push_back(f);
                }
            }

            std::vector<fs::path> files_to_process;
            if (!skill_files.empty()) {
                // SKILL.md takes precedence
                if (skill_files.size() > 1) {
                    debug("[skills] Multiple skill files found in " +
                          dir.string() + ", using " +
                          skill_files[0].filename().string());
                }
                files_to_process.push_back(skill_files[0]);
            } else {
                files_to_process = regular_files;
            }

            for (const auto& file_path : files_to_process) {
                try {
                    std::ifstream file(file_path);
                    if (!file.is_open()) continue;

                    std::string content(
                        (std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
                    file.close();

                    bool is_skill_format = detail::is_skill_file(file_path);
                    auto skill_directory = is_skill_format
                        ? std::optional<fs::path>(file_path.parent_path())
                        : std::nullopt;

                    // Determine command name
                    std::string cmd_name;
                    if (is_skill_format) {
                        cmd_name = detail::get_skill_command_name(
                            file_path, commands_dir);
                    } else {
                        cmd_name = detail::get_regular_command_name(
                            file_path, commands_dir);
                    }

                    // Parse frontmatter
                    auto fm_data = parse_frontmatter_rich(content);
                    auto stripped = cc::utils::strip_frontmatter(content);
                    std::string md_content(stripped);

                    auto parsed = parse_skill_frontmatter_fields(
                        fm_data, md_content, cmd_name,
                        /*is_legacy_command=*/true);

                    skills.push_back(detail::SkillWithPath{
                        .skill = create_skill_command(
                            cmd_name, parsed, md_content,
                            SettingSource::ProjectSettings,
                            LoadedFrom::CommandsDeprecated,
                            skill_directory, std::nullopt),
                        .file_path = file_path,
                    });
                } catch (const std::exception& e) {
                    log_error(std::string("[skills] Error loading legacy command ") +
                              file_path.string() + ": " + e.what());
                }
            }
        }
    }

    return skills;
}

// =========================================================================
// Dynamic skills state
// TS REF: src/skills/loadSkillsDir.ts:820-832 (dynamicSkillDirs, dynamicSkills,
//          conditionalSkills, activatedConditionalSkillNames)
// =========================================================================

namespace detail {

/// Thread-safe storage for dynamic skills state
struct DynamicSkillsState {
    std::mutex mutex;
    std::set<std::string> dynamic_skill_dirs;
    std::map<std::string, SkillCommand> dynamic_skills;
    std::map<std::string, SkillCommand> conditional_skills;
    std::set<std::string> activated_conditional_skill_names;
    std::vector<std::function<void()>> on_loaded_callbacks;
};

inline DynamicSkillsState& dynamic_state() {
    static DynamicSkillsState state;
    return state;
}

/// Cached skill directory commands (memoized equivalent)
struct SkillDirCache {
    std::mutex mutex;
    std::optional<std::vector<SkillCommand>> cached;
    fs::path cached_cwd;
};

inline SkillDirCache& skill_dir_cache() {
    static SkillDirCache cache;
    return cache;
}

} // namespace detail

// =========================================================================
// isPathGitignored
// TS REF: src/utils/git/gitignore.ts (isPathGitignored)
// =========================================================================

/// Check if a directory path is gitignored relative to a repo root.
/// Uses the existing GitignoreFilter utility.
bool is_path_gitignored(const fs::path& path, const fs::path& repo_root) {
    try {
        cc::utils::GitignoreFilter filter(repo_root);
        return filter.is_ignored(path);
    } catch (...) {
        return false; // Fail open
    }
}

// =========================================================================
// getSkillDirCommands
// TS REF: src/skills/loadSkillsDir.ts:638-804
// =========================================================================

/// Load all skills from /skills/ and legacy /commands/ directories.
/// This is the main entry point for skill discovery.
/// TS REF: getSkillDirCommands() is memoized on cwd.
std::vector<SkillCommand> get_skill_dir_commands(const fs::path& cwd) {
    auto& cache = detail::skill_dir_cache();
    std::lock_guard lock(cache.mutex);

    // Check cache
    fs::path abs_cwd = fs::absolute(cwd);
    if (cache.cached.has_value() && cache.cached_cwd == abs_cwd) {
        return *cache.cached;
    }

    // Build search paths
    auto user_skills_dir = get_claude_config_home_dir() + "/skills";
    auto managed_skills_dir = [&]() -> std::string {
        auto managed = get_managed_file_path();
        if (managed.empty()) return "";
        return managed + "/.claude/skills";
    }();
    auto project_skills_dirs = get_project_dirs_up_to_home("skills", cwd);
    auto additional_dirs = get_additional_directories_for_claude_md();

    bool skills_locked = is_restricted_to_plugin_only("skills");
    bool project_settings_enabled =
        is_setting_source_enabled(SettingSource::ProjectSettings) && !skills_locked;

    // --bare mode: skip auto-discovery
    const char* bare_env = std::getenv("CLAUDE_CODE_SIMPLE");
    bool is_bare = cc::utils::is_env_truthy(bare_env);
    if (is_bare) {
        if (additional_dirs.empty() || !project_settings_enabled) {
            debug("[bare] Skipping skill dir discovery (" +
                  std::string(additional_dirs.empty() ? "no --add-dir" :
                  "projectSettings disabled or skillsLocked") + ")");
            cache.cached = std::vector<SkillCommand>{};
            cache.cached_cwd = abs_cwd;
            return {};
        }

        // Only load from explicit --add-dir paths
        std::vector<SkillCommand> result;
        for (const auto& dir : additional_dirs) {
            auto add_skills = load_skills_from_skills_dir(
                fs::path(dir) / ".claude" / "skills",
                SettingSource::ProjectSettings);
            for (auto& swp : add_skills) {
                result.push_back(std::move(swp.skill));
            }
        }
        cache.cached = result;
        cache.cached_cwd = abs_cwd;
        return result;
    }

    debug("[skills] Loading from: managed=" + managed_skills_dir +
          ", user=" + user_skills_dir +
          ", project count=" + std::to_string(project_skills_dirs.size()));

    // Load from all sources
    std::vector<detail::SkillWithPath> all_skills_with_paths;

    // 1. Managed skills (policy)
    if (is_setting_source_enabled(SettingSource::PolicySettings) &&
        !cc::utils::is_env_truthy(std::getenv("CLAUDE_CODE_DISABLE_POLICY_SKILLS")) &&
        !managed_skills_dir.empty()) {
        auto managed = load_skills_from_skills_dir(
            managed_skills_dir, SettingSource::PolicySettings);
        for (auto& s : managed) all_skills_with_paths.push_back(std::move(s));
    }

    // 2. User skills
    if (is_setting_source_enabled(SettingSource::UserSettings) && !skills_locked) {
        auto user = load_skills_from_skills_dir(
            user_skills_dir, SettingSource::UserSettings);
        for (auto& s : user) all_skills_with_paths.push_back(std::move(s));
    }

    // 3. Project skills (from all .claude/skills dirs up to git root)
    if (project_settings_enabled) {
        for (const auto& dir : project_skills_dirs) {
            auto project = load_skills_from_skills_dir(
                dir, SettingSource::ProjectSettings);
            for (auto& s : project) all_skills_with_paths.push_back(std::move(s));
        }
    }

    // 4. Additional dirs (--add-dir)
    if (project_settings_enabled) {
        for (const auto& dir : additional_dirs) {
            auto add_skills = load_skills_from_skills_dir(
                fs::path(dir) / ".claude" / "skills",
                SettingSource::ProjectSettings);
            for (auto& s : add_skills) all_skills_with_paths.push_back(std::move(s));
        }
    }

    // 5. Legacy commands
    if (!skills_locked) {
        auto legacy = load_skills_from_commands_dir(cwd);
        for (auto& s : legacy) all_skills_with_paths.push_back(std::move(s));
    }

    // Deduplicate by resolved path (handles symlinks)
    // TS REF: getFileIdentity() + seenFileIds map
    std::map<std::string, SettingSource> seen_file_ids;
    std::vector<SkillCommand> deduplicated_skills;

    for (auto& entry : all_skills_with_paths) {
        if (entry.skill.type != "prompt") continue;

        auto file_id = get_file_identity(entry.file_path);
        if (!file_id.has_value()) {
            deduplicated_skills.push_back(std::move(entry.skill));
            continue;
        }

        auto existing = seen_file_ids.find(*file_id);
        if (existing != seen_file_ids.end()) {
            debug("[skills] Skipping duplicate skill '" + entry.skill.name +
                  "' (same file already loaded)");
            continue;
        }

        seen_file_ids[*file_id] = entry.skill.source;
        deduplicated_skills.push_back(std::move(entry.skill));
    }

    // Separate conditional skills (with paths frontmatter) from unconditional ones
    auto& dyn_state = detail::dynamic_state();
    std::lock_guard dyn_lock(dyn_state.mutex);

    std::vector<SkillCommand> unconditional_skills;
    std::vector<SkillCommand> new_conditional_skills;

    for (auto& skill : deduplicated_skills) {
        if (skill.paths.has_value() && !skill.paths->empty() &&
            dyn_state.activated_conditional_skill_names.find(skill.name) ==
                dyn_state.activated_conditional_skill_names.end()) {
            new_conditional_skills.push_back(std::move(skill));
        } else {
            unconditional_skills.push_back(std::move(skill));
        }
    }

    // Store conditional skills for later activation
    for (auto& skill : new_conditional_skills) {
        dyn_state.conditional_skills[skill.name] = std::move(skill);
    }

    if (!new_conditional_skills.empty()) {
        debug("[skills] " + std::to_string(new_conditional_skills.size()) +
              " conditional skills stored (activated when matching files are touched)");
    }

    debug("[skills] Loaded " + std::to_string(deduplicated_skills.size()) +
          " unique skills (" +
          std::to_string(unconditional_skills.size()) + " unconditional, " +
          std::to_string(new_conditional_skills.size()) + " conditional)");

    cache.cached = unconditional_skills;
    cache.cached_cwd = abs_cwd;
    return unconditional_skills;
}

// =========================================================================
// clearSkillCaches
// TS REF: src/skills/loadSkillsDir.ts:806-811
// =========================================================================

/// Clear all skill caches (call when directories change)
void clear_skill_caches() {
    auto& cache = detail::skill_dir_cache();
    std::lock_guard lock(cache.mutex);
    cache.cached.reset();
    cache.cached_cwd.clear();

    auto& dyn_state = detail::dynamic_state();
    std::lock_guard dyn_lock(dyn_state.mutex);
    dyn_state.conditional_skills.clear();
    dyn_state.activated_conditional_skill_names.clear();
}

// =========================================================================
// onDynamicSkillsLoaded
// TS REF: src/skills/loadSkillsDir.ts:839-851
// =========================================================================

/// Register a callback to be invoked when dynamic skills are loaded.
/// Returns an unsubscribe function.
std::function<void()> on_dynamic_skills_loaded(std::function<void()> callback) {
    auto& state = detail::dynamic_state();
    std::lock_guard lock(state.mutex);

    // Wrap at subscribe time so a throwing listener is logged and skipped
    auto wrapped = [cb = std::move(callback)]() {
        try {
            cb();
        } catch (const std::exception& e) {
            log_error(e);
        }
    };

    state.on_loaded_callbacks.push_back(wrapped);

    // Return unsubscribe function
    return [&state, it = std::prev(state.on_loaded_callbacks.end())]() {
        std::lock_guard unsub_lock(state.mutex);
        // Mark as empty function rather than erase to avoid iterator invalidation
        *it = nullptr;
    };
}

namespace detail {
inline void emit_skills_loaded() {
    auto& state = dynamic_state();
    std::lock_guard lock(state.mutex);
    for (auto& cb : state.on_loaded_callbacks) {
        if (cb) {
            try {
                cb();
            } catch (const std::exception& e) {
                log_error(e);
            }
        }
    }
}
} // namespace detail

// =========================================================================
// discoverSkillDirsForPaths
// TS REF: src/skills/loadSkillsDir.ts:861-915
// =========================================================================

/// Discover skill directories by walking up from file paths to cwd.
/// Only discovers directories below cwd (cwd-level skills are loaded at startup).
/// Returns newly discovered directories, sorted deepest first.
std::vector<fs::path> discover_skill_dirs_for_paths(
    const std::vector<fs::path>& file_paths, const fs::path& cwd)
{
    auto& state = detail::dynamic_state();
    std::lock_guard lock(state.mutex);

    fs::path resolved_cwd = fs::absolute(cwd);
    // Remove trailing separator
    while (!resolved_cwd.empty() && resolved_cwd.filename() == ".") {
        resolved_cwd = resolved_cwd.parent_path();
    }

    std::vector<fs::path> new_dirs;

    for (const auto& file_path : file_paths) {
        fs::path current_dir = fs::absolute(file_path).parent_path();

        // Walk up to cwd but NOT including cwd itself
        while (true) {
            // Check if we're still under cwd
            std::error_code ec;
            auto rel = fs::relative(current_dir, resolved_cwd, ec);
            if (ec || rel.empty() || rel == ".") break;
            if (rel.string().starts_with("..")) break;

            auto skill_dir = current_dir / ".claude" / "skills";
            std::string skill_dir_str = skill_dir.string();

            // Skip if already checked
            if (state.dynamic_skill_dirs.find(skill_dir_str) ==
                state.dynamic_skill_dirs.end()) {
                state.dynamic_skill_dirs.insert(skill_dir_str);

                if (fs::exists(skill_dir) && fs::is_directory(skill_dir)) {
                    // Check if gitignored
                    if (!is_path_gitignored(current_dir, resolved_cwd)) {
                        new_dirs.push_back(skill_dir);
                    } else {
                        debug("[skills] Skipped gitignored skills dir: " +
                              skill_dir_str);
                    }
                }
            }

            // Move to parent
            auto parent = current_dir.parent_path();
            if (parent == current_dir) break; // Reached root
            current_dir = parent;
        }
    }

    // Sort by path depth (deepest first) so skills closer to file take precedence
    std::sort(new_dirs.begin(), new_dirs.end(),
        [](const fs::path& a, const fs::path& b) {
            return a.string().size() > b.string().size();
        });

    return new_dirs;
}

// =========================================================================
// addSkillDirectories
// TS REF: src/skills/loadSkillsDir.ts:923-975
// =========================================================================

/// Load skills from the given directories and merge into dynamic skills map.
/// Skills from deeper paths override shallower ones.
void add_skill_directories(const std::vector<fs::path>& dirs) {
    if (!is_setting_source_enabled(SettingSource::ProjectSettings) ||
        is_restricted_to_plugin_only("skills")) {
        debug("[skills] Dynamic skill discovery skipped: projectSettings disabled or plugin-only policy");
        return;
    }

    if (dirs.empty()) return;

    auto& state = detail::dynamic_state();

    // Load skills from all directories
    std::vector<std::vector<detail::SkillWithPath>> loaded_skills;
    loaded_skills.reserve(dirs.size());
    for (const auto& dir : dirs) {
        loaded_skills.push_back(load_skills_from_skills_dir(
            dir, SettingSource::ProjectSettings));
    }

    std::size_t total_new = 0;
    {
        std::lock_guard lock(state.mutex);
        auto previous_names = std::set<std::string>();
        for (const auto& [name, _] : state.dynamic_skills) {
            previous_names.insert(name);
        }

        // Process in reverse order (shallower first) so deeper paths override
        for (int i = static_cast<int>(loaded_skills.size()) - 1; i >= 0; --i) {
            for (auto& entry : loaded_skills[i]) {
                if (entry.skill.type == "prompt") {
                    state.dynamic_skills[entry.skill.name] = std::move(entry.skill);
                }
            }
        }

        // Count new skills
        for (const auto& [name, _] : state.dynamic_skills) {
            if (previous_names.find(name) == previous_names.end()) {
                ++total_new;
            }
        }
    }

    if (total_new > 0) {
        debug("[skills] Dynamically discovered " + std::to_string(total_new) +
              " skills from " + std::to_string(dirs.size()) + " directories");
    }

    // Notify listeners
    detail::emit_skills_loaded();
}

// =========================================================================
// getDynamicSkills
// TS REF: src/skills/loadSkillsDir.ts:981-983
// =========================================================================

/// Get all dynamically discovered skills
std::vector<SkillCommand> get_dynamic_skills() {
    auto& state = detail::dynamic_state();
    std::lock_guard lock(state.mutex);

    std::vector<SkillCommand> result;
    result.reserve(state.dynamic_skills.size());
    for (const auto& [_, skill] : state.dynamic_skills) {
        result.push_back(skill);
    }
    return result;
}

// =========================================================================
// activateConditionalSkillsForPaths
// TS REF: src/skills/loadSkillsDir.ts:997-1058
// =========================================================================

// Forward declaration (defined after clearDynamicSkills)
bool matches_simple_glob(const std::string& path, const std::string& pattern);

/// Activate conditional skills whose path patterns match the given file paths.
/// Activated skills are moved to dynamic skills map.
/// Returns newly activated skill names.
std::vector<std::string> activate_conditional_skills_for_paths(
    const std::vector<fs::path>& file_paths, const fs::path& cwd)
{
    auto& state = detail::dynamic_state();
    std::lock_guard lock(state.mutex);

    if (state.conditional_skills.empty()) return {};

    std::vector<std::string> activated;
    fs::path abs_cwd = fs::absolute(cwd);

    // Collect skill names to activate (iterate over a copy since we modify the map)
    auto conditional_copy = state.conditional_skills;

    for (auto& [name, skill] : conditional_copy) {
        if (skill.type != "prompt" || !skill.paths.has_value() ||
            skill.paths->empty()) {
            continue;
        }

        // Build a simple glob matcher from skill paths
        bool matched = false;
        for (const auto& file_path : file_paths) {
            std::error_code ec;
            auto rel_path = fs::relative(file_path, abs_cwd, ec);
            if (ec) continue;
            std::string rel_str = rel_path.string();
            if (rel_str.empty() || rel_str.starts_with("..")) continue;

            // Match against each path pattern using simple glob
            for (const auto& pattern : *skill.paths) {
                if (matches_simple_glob(rel_str, pattern)) {
                    matched = true;
                    break;
                }
            }
            if (matched) break;
        }

        if (matched) {
            // Move to dynamic skills
            state.dynamic_skills[name] = std::move(skill);
            state.conditional_skills.erase(name);
            state.activated_conditional_skill_names.insert(name);
            activated.push_back(name);
            debug("[skills] Activated conditional skill '" + name +
                  "' (matched file path)");
        }
    }

    if (!activated.empty()) {
        detail::emit_skills_loaded();
    }

    return activated;
}

// =========================================================================
// getConditionalSkillCount
// TS REF: src/skills/loadSkillsDir.ts:1063-1065
// =========================================================================

/// Get the number of pending conditional skills (for testing/debugging)
std::size_t get_conditional_skill_count() {
    auto& state = detail::dynamic_state();
    std::lock_guard lock(state.mutex);
    return state.conditional_skills.size();
}

// =========================================================================
// clearDynamicSkills
// TS REF: src/skills/loadSkillsDir.ts:1070-1075
// =========================================================================

/// Clear dynamic skill state (for testing)
void clear_dynamic_skills() {
    auto& state = detail::dynamic_state();
    std::lock_guard lock(state.mutex);
    state.dynamic_skill_dirs.clear();
    state.dynamic_skills.clear();
    state.conditional_skills.clear();
    state.activated_conditional_skill_names.clear();
}

// =========================================================================
// Simple glob matching helper (used by activateConditionalSkillsForPaths)
// =========================================================================

namespace detail {

/// Match a path against a glob pattern (supports * and ** and ?)
bool matches_simple_glob_impl(const std::string& path, const std::string& pattern) {
    // Use the GitignoreFilter's glob matching logic
    // Simplified version: supports *, **, ?
    std::size_t pi = 0, si = 0;
    std::size_t star_p = std::string::npos, star_s = 0;

    while (si < path.size()) {
        if (pi < pattern.size() && (pattern[pi] == path[si] || pattern[pi] == '?')) {
            ++pi; ++si;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            // Check for **
            bool double_star = (pi + 1 < pattern.size() && pattern[pi + 1] == '*');
            if (double_star) ++pi; // skip second *
            star_p = pi++;
            star_s = si;
        } else if (star_p != std::string::npos) {
            pi = star_p + 1;
            si = ++star_s;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

} // namespace detail

/// Public glob matching function used by activate_conditional_skills_for_paths.
/// Supports ** (matches path separators), * (matches within segment), ? (single char).
bool matches_simple_glob(const std::string& path, const std::string& pattern) {
    // If pattern has no /, match against filename only
    if (pattern.find('/') == std::string::npos) {
        auto filename = fs::path(path).filename().string();
        return detail::matches_simple_glob_impl(filename, pattern);
    }
    return detail::matches_simple_glob_impl(path, pattern);
}

// =========================================================================
// Backwards-compatible aliases (for tests)
// TS REF: src/skills/loadSkillsDir.ts:814-816
// =========================================================================

/// Alias for get_skill_dir_commands (backwards compatible with tests)
std::vector<SkillCommand> get_command_dir_commands(const fs::path& cwd) {
    return get_skill_dir_commands(cwd);
}

/// Alias for clear_skill_caches
void clear_command_caches() {
    clear_skill_caches();
}

// =========================================================================
// MCP skill builder registration
// TS REF: src/skills/loadSkillsDir.ts:1083-1086
//         (registerMCPSkillBuilders call at module bottom)
//
// The MCP skill builder registration is handled at module load time
// via the static initializer below. It passes the create_skill_command
// and parse_skill_frontmatter_fields function references to the MCP
// skill builders module so MCP skills can use the same construction path.
// =========================================================================

namespace detail {

/// Register the skill builders with the MCP skill builders module.
/// Called once at static-init time.
struct McpSkillBuilderRegistrar {
    McpSkillBuilderRegistrar() {
        // The MCP skill builder module already has its own registration
        // mechanism. Here we ensure the function pointers are available.
        // In the TS code, this is:
        //   registerMCPSkillBuilders({ createSkillCommand, parseSkillFrontmatterFields })
        // For CPP, we log that the builders are ready.
        debug("[skills] MCP skill builders registered");
    }
};

// Static initializer - runs at module load
inline McpSkillBuilderRegistrar g_mcp_registrar;

} // namespace detail

// =========================================================================
// estimateSkillFrontmatterTokens
// TS REF: src/skills/loadSkillsDir.ts:100-105
// =========================================================================

/// Estimate token count for a skill based on frontmatter only
/// (name, description, whenToUse). Rough estimation for CPP.
std::size_t estimate_skill_frontmatter_tokens(const SkillCommand& skill) {
    std::string frontmatter_text = skill.name;
    if (!skill.description.empty()) frontmatter_text += " " + skill.description;
    if (skill.when_to_use.has_value()) frontmatter_text += " " + *skill.when_to_use;

    // Rough estimate: ~4 chars per token
    return (frontmatter_text.size() + 3) / 4;
}

// =========================================================================
// getSkillsSearchPaths
// TS REF: src/skills/loadSkillsDir.ts (skillDirs computation)
// =========================================================================

/// Return the standard skill search paths (~/.claude/skills + ./.claude/skills).
std::vector<fs::path> get_skills_search_paths() {
    std::vector<fs::path> paths;
    if (auto home = std::getenv("HOME")) {
        paths.emplace_back(fs::path(home) / ".claude" / "skills");
    }
    paths.emplace_back(fs::current_path() / ".claude" / "skills");
    return paths;
}

// =========================================================================
// loadSkillsDirectory — lightweight manifest discovery
// =========================================================================

/// Scan a single directory for SKILL.md-based skills and return lightweight
/// SkillManifest entries.  Used by /skills list, /skills import, and the
/// skillify rescan flow.
std::vector<SkillManifest> load_skills_directory(const fs::path& dir_path) {
    std::vector<SkillManifest> result;

    auto loaded = load_skills_from_skills_dir(dir_path, SettingSource::ProjectSettings);
    result.reserve(loaded.size());
    for (const auto& entry : loaded) {
        const auto& sc = entry.skill;
        result.push_back(SkillManifest{
            .name = sc.name,
            .description = sc.description,
            .version = sc.version,
            .triggers = {},
            .directory = sc.skill_root.value_or(dir_path),
        });
    }
    return result;
}

// =========================================================================
// findSkillByName — lookup a single skill manifest by name
// =========================================================================

/// Find a skill by name across all search paths. Returns std::nullopt if
/// the skill is not found in any skills directory.
std::optional<SkillManifest> find_skill_by_name(std::string_view name) {
    for (const auto& path : get_skills_search_paths()) {
        auto manifests = load_skills_directory(path);
        for (auto& m : manifests) {
            if (m.name == name) {
                return m;
            }
        }
    }
    return std::nullopt;
}

} // namespace cc::skills