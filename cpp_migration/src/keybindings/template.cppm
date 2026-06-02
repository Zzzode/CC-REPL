/// @file template.cppm
/// @brief Keybindings template generator.
/// Migrated from src/keybindings/template.ts
///
/// Generates a well-documented template file for ~/.claude/keybindings.json
/// that users can customize. Reserved shortcuts are excluded from the template
/// to avoid /doctor warnings.
module;

#include <string>
#include <string_view>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unordered_set>

export module cc.keybindings.template_;

import cc.keybindings.schema;
import cc.keybindings.defaults;

export namespace cc::keybindings {

/// Schema URL for keybindings.json validation
inline constexpr std::string_view keybindings_schema_url =
    "https://www.schemastore.org/claude-code-keybindings.json";

/// Documentation URL for keybindings reference
inline constexpr std::string_view keybindings_docs_url =
    "https://code.claude.com/docs/en/keybindings";

/// Normalize a key string for comparison (lowercase, strip whitespace)
[[nodiscard]] inline std::string normalize_key_for_comparison(std::string_view key) {
    std::string result;
    result.reserve(key.size());
    for (char c : key) {
        if (c != ' ') {
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return result;
}

/// Filter out reserved shortcuts that cannot be rebound.
/// These would cause /doctor to warn, so we exclude them from the template.
[[nodiscard]] inline std::vector<Keybinding> filter_reserved_shortcuts(
    const std::vector<Keybinding>& bindings
) {
    const auto& reserved = reserved_shortcuts();

    std::vector<Keybinding> filtered;
    filtered.reserve(bindings.size());

    for (const auto& binding : bindings) {
        bool is_reserved_binding = false;

        for (const auto& chord : binding.keys) {
            // Reconstruct the shortcut string for comparison
            std::string repr;
            if (chord.modifiers.ctrl) repr += "ctrl+";
            if (chord.modifiers.alt) repr += "alt+";
            if (chord.modifiers.shift) repr += "shift+";
            if (chord.modifiers.meta) repr += "meta+";
            repr += chord.key;

            if (reserved.contains(normalize_key_for_comparison(repr))) {
                is_reserved_binding = true;
                break;
            }
        }

        if (!is_reserved_binding) {
            filtered.push_back(binding);
        }
    }

    return filtered;
}

/// Escape a string for JSON output
[[nodiscard]] inline std::string json_escape(std::string_view str) {
    std::string result;
    result.reserve(str.size() + 2);
    for (char c : str) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

/// Generate a template keybindings.json file content.
/// Creates a fully valid JSON file with all default bindings (excluding reserved)
/// that users can customize.
[[nodiscard]] inline std::string generate_keybindings_template() {
    auto bindings = filter_reserved_shortcuts(get_default_bindings());

    std::ostringstream out;
    out << "{\n";
    out << "  \"$schema\": \"" << keybindings_schema_url << "\",\n";
    out << "  \"$docs\": \"" << keybindings_docs_url << "\",\n";
    out << "  \"bindings\": [\n";

    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& binding = bindings[i];
        out << "    {\n";
        out << "      \"context\": \"" << json_escape(binding.when.value_or("Global")) << "\",\n";
        out << "      \"bindings\": {\n";

        // Format each key chord as "shortcut": "command"
        for (std::size_t k = 0; k < binding.keys.size(); ++k) {
            const auto& chord = binding.keys[k];
            std::string key_str;
            if (chord.modifiers.ctrl) key_str += "ctrl+";
            if (chord.modifiers.alt) key_str += "alt+";
            if (chord.modifiers.shift) key_str += "shift+";
            if (chord.modifiers.meta) key_str += "cmd+";
            key_str += chord.key;

            out << "        \"" << json_escape(key_str) << "\": \""
                << json_escape(binding.command) << "\"";
            if (k + 1 < binding.keys.size()) out << ",";
            out << "\n";
        }

        out << "      }\n";
        out << "    }";
        if (i + 1 < bindings.size()) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    return out.str();
}

} // namespace cc::keybindings
