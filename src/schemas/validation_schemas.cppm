module;
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <sstream>
#include <algorithm>
#include <regex>

export module cc.schemas.validation_schemas;

export namespace cc::schemas {

// A single validation rule for a field
struct ValidationRule {
    std::string field;
    std::string type;        // "string", "int", "bool", "email", "url"
    bool required = false;
    std::optional<std::string> pattern;  // Regex pattern
    std::optional<int> min;              // Minimum value/length
    std::optional<int> max;              // Maximum value/length
};

// A schema is a collection of validation rules
struct Schema {
    std::string name;
    std::vector<ValidationRule> rules;
};

// Validate data against a schema, returning a list of error messages
inline auto validate(std::map<std::string, std::string> data, Schema schema)
    -> std::vector<std::string> {
    std::vector<std::string> errors;

    for (const auto& rule : schema.rules) {
        auto it = data.find(rule.field);
        bool has_value = (it != data.end() && !it->second.empty());

        // Required check
        if (rule.required && !has_value) {
            errors.push_back("Field '" + rule.field + "' is required");
            continue;
        }

        // Skip further checks if field is absent and not required
        if (!has_value) continue;

        const std::string& value = it->second;

        // Type-specific validation
        if (rule.type == "int") {
            bool is_int = !value.empty() && std::all_of(value.begin(), value.end(),
                [first = true](char c) mutable {
                    if (first && c == '-') { first = false; return true; }
                    first = false;
                    return std::isdigit(static_cast<unsigned char>(c)) != 0;
                });
            if (!is_int) {
                errors.push_back("Field '" + rule.field + "' must be an integer");
                continue;
            }

            int int_val = std::stoi(value);
            if (rule.min.has_value() && int_val < rule.min.value()) {
                errors.push_back("Field '" + rule.field + "' must be >= " +
                                 std::to_string(rule.min.value()));
            }
            if (rule.max.has_value() && int_val > rule.max.value()) {
                errors.push_back("Field '" + rule.field + "' must be <= " +
                                 std::to_string(rule.max.value()));
            }
        } else if (rule.type == "string") {
            int len = static_cast<int>(value.size());
            if (rule.min.has_value() && len < rule.min.value()) {
                errors.push_back("Field '" + rule.field + "' must have at least " +
                                 std::to_string(rule.min.value()) + " characters");
            }
            if (rule.max.has_value() && len > rule.max.value()) {
                errors.push_back("Field '" + rule.field + "' must have at most " +
                                 std::to_string(rule.max.value()) + " characters");
            }
        } else if (rule.type == "bool") {
            if (value != "true" && value != "false" && value != "0" && value != "1") {
                errors.push_back("Field '" + rule.field + "' must be a boolean");
            }
        } else if (rule.type == "email") {
            if (value.find('@') == std::string::npos || value.find('.') == std::string::npos) {
                errors.push_back("Field '" + rule.field + "' must be a valid email");
            }
        } else if (rule.type == "url") {
            if (value.substr(0, 7) != "http://" && value.substr(0, 8) != "https://") {
                errors.push_back("Field '" + rule.field + "' must be a valid URL");
            }
        }

        // Pattern validation
        if (rule.pattern.has_value()) {
            try {
                std::regex re(rule.pattern.value());
                if (!std::regex_match(value, re)) {
                    errors.push_back("Field '" + rule.field +
                                     "' does not match pattern: " + rule.pattern.value());
                }
            } catch (const std::regex_error&) {
                // Invalid regex pattern - skip validation
            }
        }
    }

    return errors;
}

// Get the schema for application configuration
inline auto get_config_schema() -> Schema {
    return {
        "config",
        {
            {"api_key", "string", true, std::nullopt, 20, 200},
            {"model", "string", false, std::nullopt, 1, 100},
            {"max_tokens", "int", false, std::nullopt, 1, 200000},
            {"temperature", "string", false, std::nullopt, std::nullopt, std::nullopt},
            {"theme", "string", false, std::nullopt, 1, 50},
        }
    };
}

// Get the schema for user settings
inline auto get_settings_schema() -> Schema {
    return {
        "settings",
        {
            {"vim_mode", "bool", false, std::nullopt, std::nullopt, std::nullopt},
            {"auto_compact", "bool", false, std::nullopt, std::nullopt, std::nullopt},
            {"compact_threshold", "int", false, std::nullopt, 1000, 100000},
            {"history_size", "int", false, std::nullopt, 10, 10000},
            {"default_model", "string", false, std::nullopt, 1, 100},
            {"color_scheme", "string", false, std::nullopt, 1, 50},
            {"font_size", "int", false, std::nullopt, 8, 72},
        }
    };
}

// Get validation schema for a specific tool's input
inline auto get_tool_input_schema(std::string_view tool_name) -> Schema {
    std::string name = "tool_input_" + std::string(tool_name);

    if (tool_name == "bash") {
        return {name, {
            {"command", "string", true, std::nullopt, 1, 10000},
            {"timeout", "int", false, std::nullopt, 1, 600},
        }};
    }
    if (tool_name == "file_read") {
        return {name, {
            {"path", "string", true, std::nullopt, 1, 4096},
            {"offset", "int", false, std::nullopt, 0, std::nullopt},
            {"limit", "int", false, std::nullopt, 1, 100000},
        }};
    }
    if (tool_name == "file_write") {
        return {name, {
            {"path", "string", true, std::nullopt, 1, 4096},
            {"content", "string", true, std::nullopt, std::nullopt, std::nullopt},
        }};
    }
    if (tool_name == "file_edit") {
        return {name, {
            {"path", "string", true, std::nullopt, 1, 4096},
            {"old_string", "string", true, std::nullopt, 1, std::nullopt},
            {"new_string", "string", true, std::nullopt, std::nullopt, std::nullopt},
        }};
    }
    if (tool_name == "glob") {
        return {name, {
            {"pattern", "string", true, std::nullopt, 1, 1000},
            {"path", "string", false, std::nullopt, 1, 4096},
        }};
    }
    if (tool_name == "grep") {
        return {name, {
            {"pattern", "string", true, std::nullopt, 1, 1000},
            {"path", "string", false, std::nullopt, 1, 4096},
            {"include", "string", false, std::nullopt, 1, 500},
        }};
    }

    // Default: no specific schema
    return {name, {}};
}

} // namespace cc::schemas
