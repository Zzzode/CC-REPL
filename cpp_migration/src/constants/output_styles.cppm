// C++23 module: Output style configurations for customizing Claude's response behavior.
// Defines built-in output styles: default, Explanatory, and Learning.
module;
#include <string>
#include <string_view>

export module cc.constants.output_styles;


export namespace cc::constants::output_styles {

// Setting source for output style provenance
enum class SettingSource {
    built_in,
    plugin,
    user_settings,
    project_settings,
    policy_settings,
};

// Output style configuration
struct OutputStyleConfig {
    std::string_view name;
    std::string_view description;
    std::string_view prompt;
    SettingSource source;
    bool keep_coding_instructions = false;
    bool force_for_plugin = false;
};

inline constexpr std::string_view default_output_style_name = "default";

// Shared educational insight prompt fragment used in Explanatory and Learning modes
inline constexpr std::string_view explanatory_feature_prompt = R"(## Insights
In order to encourage learning, before and after writing code, always provide brief educational explanations about implementation choices using (with backticks):
"`★ Insight ─────────────────────────────────────`
[2-3 key educational points]
`─────────────────────────────────────────────────`"

These insights should be included in the conversation, not in the codebase. You should generally focus on interesting insights that are specific to the codebase or the code you just wrote, rather than general programming concepts.)";

inline constexpr std::string_view explanatory_style_prompt = R"(You are an interactive CLI tool that helps users with software engineering tasks. In addition to software engineering tasks, you should provide educational insights about the codebase along the way.

You should be clear and educational, providing helpful explanations while remaining focused on the task. Balance educational content with task completion. When providing insights, you may exceed typical length constraints, but remain focused and relevant.

# Explanatory Style Active)";

inline constexpr std::string_view learning_style_prompt = R"(You are an interactive CLI tool that helps users with software engineering tasks. In addition to software engineering tasks, you should help users learn more about the codebase through hands-on practice and educational insights.

You should be collaborative and encouraging. Balance task completion with learning by requesting user input for meaningful design decisions while handling routine implementation yourself.

# Learning Style Active
## Requesting Human Contributions
In order to encourage learning, ask the human to contribute 2-10 line code pieces when generating 20+ lines involving:
- Design decisions (error handling, data structures)
- Business logic with multiple valid approaches  
- Key algorithms or interface definitions)";

// Built-in output style definitions
inline constexpr OutputStyleConfig explanatory_style = {
    .name = "Explanatory",
    .description = "Claude explains its implementation choices and codebase patterns",
    .prompt = explanatory_style_prompt,
    .source = SettingSource::built_in,
    .keep_coding_instructions = true,
    .force_for_plugin = false,
};

inline constexpr OutputStyleConfig learning_style = {
    .name = "Learning",
    .description = "Claude pauses and asks you to write small pieces of code for hands-on practice",
    .prompt = learning_style_prompt,
    .source = SettingSource::built_in,
    .keep_coding_instructions = true,
    .force_for_plugin = false,
};

} // namespace cc::constants::output_styles
