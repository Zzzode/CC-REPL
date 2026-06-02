/// @file update_config.cppm
/// @brief Update Config skill - configuration modification workflow.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.update_config;

import cc.skills.skill;

export namespace cc::skills::update_config {

/// Update config skill definition
[[nodiscard]] inline SkillDefinition make_update_config_skill() {
    return SkillDefinition{
        .name = "update-config",
        .description = "Safely modify configuration files with validation and rollback",
        .trigger_patterns = {
            R"(update.*config)",
            R"(change.*(?:setting|configuration))",
            R"(modify.*(?:config|settings))",
            R"(set.*(?:option|preference|config))",
            R"(configure\s+\w+)",
        },
        .content = R"(## Update Config Skill

### Process
1. **Identify target**: Which config file/setting needs to change?
2. **Read current**: Show the current value and its context
3. **Validate new value**: Check the new value is valid for the setting
4. **Preview change**: Show a diff of what will change
5. **Apply**: Make the change with appropriate file permissions
6. **Verify**: Confirm the change took effect

### Config File Locations
- **Global**: ~/.cc-repl/config.json
- **Project**: .cc-repl/config.json (in project root)
- **Session**: In-memory overrides (lost on exit)

### Safety Rules
- Always show current value before changing
- Validate against known schema if available
- Create backup before modifying (config.json.bak)
- Never modify files outside known config paths without confirmation
- Preserve comments and formatting where possible

### Common Settings
- model: Default model selection
- theme: UI theme (dark/light/auto)
- keybindings: Custom key mappings
- permissions: Tool permission defaults
- plugins: Enabled plugin list
- features: Feature flag overrides

### Rollback
If something goes wrong:
1. Check ~/.cc-repl/config.json.bak
2. Use /config reset to restore defaults
3. Individual settings can be unset with /config unset <key>
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::update_config
