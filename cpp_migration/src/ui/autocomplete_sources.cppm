/// @file autocomplete_sources.cppm
/// @brief Thin data-source facade for prompt autocomplete.
module;

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.ui.autocomplete_sources;

export namespace cc::ui::autocomplete_sources {

struct SkillSuggestionData {
    std::string name;
    std::string description;
    std::string source;
    std::string source_detail;
    std::string kind;                     // SL-08: skill kind (e.g. "workflow")
    std::size_t token_estimate = 0;
    bool enabled = true;
    bool user_invocable = true;           // SL-10: false => reject /name invocation
    std::string content;
};

struct PluginCommandSuggestionData {
    std::string command;
    std::string plugin_name;
};

struct McpResourceSuggestionData {
    std::string display;
    std::string description;
    std::string insert_text;
    std::string server_name;
    bool channel_like = false;
};

[[nodiscard]] std::vector<SkillSuggestionData> collect_skill_suggestions(
    std::string_view cwd);

[[nodiscard]] std::optional<SkillSuggestionData> find_skill_suggestion(
    std::string_view cwd,
    std::string_view name);

[[nodiscard]] std::string skill_invocation_prompt(
    const SkillSuggestionData& skill,
    std::string_view user_text);

[[nodiscard]] std::vector<PluginCommandSuggestionData> collect_plugin_commands(
    std::string_view cwd);

[[nodiscard]] std::vector<McpResourceSuggestionData> collect_mcp_resource_suggestions();

} // namespace cc::ui::autocomplete_sources
