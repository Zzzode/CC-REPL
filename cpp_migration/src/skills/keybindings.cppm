/// @file keybindings.cppm
/// @brief Keybindings skill - keyboard shortcut management workflow.
module;
#include <string>
#include <vector>
#include <optional>
#include <expected>

export module cc.skills.keybindings;

import cc.skills.skill;

export namespace cc::skills::keybindings {

/// Keybindings skill definition
[[nodiscard]] inline SkillDefinition make_keybindings_skill() {
    return SkillDefinition{
        .name = "keybindings",
        .description = "Keyboard shortcut reference and customization workflow",
        .trigger_patterns = {
            R"(keybind(?:ing)?s?)",
            R"(shortcut)",
            R"(key\s*(?:map|combo|binding))",
            R"(hotkey)",
        },
        .content = R"(## Keybindings Reference

### Navigation
- Ctrl+C: Cancel current request / clear input
- Ctrl+D: Exit (when input is empty)
- Ctrl+L: Clear screen
- Up/Down: Navigate history
- Ctrl+R: Reverse history search

### Editing
- Ctrl+A: Move to beginning of line
- Ctrl+E: Move to end of line
- Ctrl+W: Delete word backward
- Ctrl+U: Delete to beginning of line
- Ctrl+K: Delete to end of line

### Features
- Ctrl+Shift+V: Paste from clipboard
- Tab: Accept suggestion / trigger completion
- Esc: Dismiss suggestions / cancel action
- Ctrl+Space: Force trigger suggestions

### Customization
- Edit ~/.cc-repl/keybindings.json to override defaults
- Use /keybindings command to view current bindings
- Context-specific bindings override global ones
- Vim mode available via /vim command
)",
        .is_builtin = true,
        .author = std::nullopt,
        .version = "1.0.0",
    };
}

} // namespace cc::skills::keybindings
