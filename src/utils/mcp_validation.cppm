// C++23 MCP Validation Module
// Date/time parsing and elicitation schema validation for MCP protocol
module;

#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.mcp_validation;

export namespace cc::utils::mcp_validation {

// ---------------------------------------------------------------------------
// Date/Time Parsing
// ---------------------------------------------------------------------------

/// Format for date/time parsing: date-only or full date-time
enum class DateTimeFormat {
    Date,     // YYYY-MM-DD
    DateTime  // YYYY-MM-DDTHH:MM:SS±HH:MM
};

/// Result of a natural language date/time parse attempt
struct DateTimeParseResult {
    bool success;
    std::string value;  // ISO 8601 string on success
    std::string error;  // Error message on failure
};

/// Callback type for the LLM-based date/time parsing backend.
/// Implementations should query a lightweight model (e.g. Haiku) to convert
/// natural language into ISO 8601.
using DateTimeParseBackend = std::function<
    std::expected<std::string, std::string>(
        std::string_view input,
        DateTimeFormat format,
        const std::atomic<bool>& cancelled)>;

/// Check if a string looks like an ISO 8601 date or date-time.
/// Used to decide whether natural-language parsing should be attempted.
[[nodiscard]] inline bool looks_like_iso8601(std::string_view input) {
    // Trim leading whitespace
    auto trimmed = input;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }
    // Must match YYYY-MM-DD at minimum
    if (trimmed.size() < 10) return false;
    return std::isdigit(static_cast<unsigned char>(trimmed[0])) &&
           std::isdigit(static_cast<unsigned char>(trimmed[1])) &&
           std::isdigit(static_cast<unsigned char>(trimmed[2])) &&
           std::isdigit(static_cast<unsigned char>(trimmed[3])) &&
           trimmed[4] == '-' &&
           std::isdigit(static_cast<unsigned char>(trimmed[5])) &&
           std::isdigit(static_cast<unsigned char>(trimmed[6])) &&
           trimmed[7] == '-' &&
           std::isdigit(static_cast<unsigned char>(trimmed[8])) &&
           std::isdigit(static_cast<unsigned char>(trimmed[9])) &&
           (trimmed.size() == 10 || trimmed[10] == 'T');
}

/// Parse a natural language date/time string into ISO 8601 format
/// using the provided LLM backend.
[[nodiscard]] inline DateTimeParseResult parse_natural_language_datetime(
    std::string_view input,
    DateTimeFormat format,
    const DateTimeParseBackend& backend,
    const std::atomic<bool>& cancelled) {

    auto result = backend(input, format, cancelled);
    if (!result.has_value()) {
        return {.success = false, .value = {}, .error = result.error()};
    }

    const auto& parsed = result.value();
    if (parsed.empty() || parsed == "INVALID") {
        return {.success = false, .value = {},
                .error = "Unable to parse date/time from input"};
    }

    // Basic sanity check: should start with a year (4 digits)
    if (parsed.size() < 4 ||
        !std::isdigit(static_cast<unsigned char>(parsed[0])) ||
        !std::isdigit(static_cast<unsigned char>(parsed[1])) ||
        !std::isdigit(static_cast<unsigned char>(parsed[2])) ||
        !std::isdigit(static_cast<unsigned char>(parsed[3]))) {
        return {.success = false, .value = {},
                .error = "Unable to parse date/time from input"};
    }

    return {.success = true, .value = parsed, .error = {}};
}

// ---------------------------------------------------------------------------
// Elicitation Schema Types
// ---------------------------------------------------------------------------

/// Supported primitive schema types for elicitation
enum class SchemaType {
    String,
    Number,
    Integer,
    Boolean,
    Enum,
    MultiSelectEnum,
    Array
};

/// String format constraints (subset of JSON Schema formats)
enum class StringFormat {
    None,
    Email,
    Uri,
    Date,
    DateTime
};

/// A single enum option with value and display label
struct EnumOption {
    std::string value;
    std::string label;
};

/// Schema definition for elicitation inputs
struct SchemaDefinition {
    SchemaType type = SchemaType::String;
    StringFormat format = StringFormat::None;

    // String constraints
    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;

    // Numeric constraints
    std::optional<double> minimum;
    std::optional<double> maximum;

    // Enum options (for Enum and MultiSelectEnum types)
    std::vector<EnumOption> options;
};

/// The validated value: either a string, number, or boolean
using ValidatedValue = std::variant<std::string, double, bool>;

/// Result of validating an elicitation input
struct ValidationResult {
    bool is_valid = false;
    std::optional<ValidatedValue> value;
    std::optional<std::string> error;
};

// ---------------------------------------------------------------------------
// Schema Inspection
// ---------------------------------------------------------------------------

/// Check if a schema represents a single-select enum
[[nodiscard]] inline bool is_enum_schema(const SchemaDefinition& schema) {
    return schema.type == SchemaType::Enum;
}

/// Check if a schema represents a multi-select enum
[[nodiscard]] inline bool is_multi_select_enum_schema(const SchemaDefinition& schema) {
    return schema.type == SchemaType::MultiSelectEnum;
}

/// Check if a schema is a date or date-time format supporting NL parsing
[[nodiscard]] inline bool is_date_time_schema(const SchemaDefinition& schema) {
    return schema.type == SchemaType::String &&
           (schema.format == StringFormat::Date ||
            schema.format == StringFormat::DateTime);
}

/// Get enum values from a schema
[[nodiscard]] inline std::vector<std::string> get_enum_values(
    const SchemaDefinition& schema) {
    std::vector<std::string> values;
    values.reserve(schema.options.size());
    for (const auto& opt : schema.options) {
        values.push_back(opt.value);
    }
    return values;
}

/// Get enum display labels from a schema
[[nodiscard]] inline std::vector<std::string> get_enum_labels(
    const SchemaDefinition& schema) {
    std::vector<std::string> labels;
    labels.reserve(schema.options.size());
    for (const auto& opt : schema.options) {
        labels.push_back(opt.label);
    }
    return labels;
}

/// Get a display label for a specific enum value
[[nodiscard]] inline std::string get_enum_label(
    const SchemaDefinition& schema, std::string_view value) {
    for (const auto& opt : schema.options) {
        if (opt.value == value) return opt.label;
    }
    return std::string{value};
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

/// Validate an elicitation input string against a schema definition.
/// Returns a ValidationResult indicating whether the input is valid
/// and, if so, the coerced value.
[[nodiscard]] inline ValidationResult validate_elicitation_input(
    std::string_view input, const SchemaDefinition& schema) {

    switch (schema.type) {
    case SchemaType::Enum:
    case SchemaType::MultiSelectEnum: {
        // Check if input is a valid option
        for (const auto& opt : schema.options) {
            if (opt.value == input) {
                return {.is_valid = true,
                        .value = std::string{input},
                        .error = std::nullopt};
            }
        }
        return {.is_valid = false, .value = std::nullopt,
                .error = "Not a valid option"};
    }

    case SchemaType::String: {
        std::string s{input};
        if (schema.min_length && s.size() < *schema.min_length) {
            return {.is_valid = false, .value = std::nullopt,
                    .error = "Must be at least " +
                             std::to_string(*schema.min_length) + " characters"};
        }
        if (schema.max_length && s.size() > *schema.max_length) {
            return {.is_valid = false, .value = std::nullopt,
                    .error = "Must be at most " +
                             std::to_string(*schema.max_length) + " characters"};
        }
        // Format-specific validation would go here (email regex, URI check, etc.)
        return {.is_valid = true, .value = s, .error = std::nullopt};
    }

    case SchemaType::Number:
    case SchemaType::Integer: {
        double num = 0.0;
        try {
            std::size_t pos = 0;
            num = std::stod(std::string{input}, &pos);
            if (pos != input.size()) {
                return {.is_valid = false, .value = std::nullopt,
                        .error = "Must be a valid number"};
            }
        } catch (...) {
            return {.is_valid = false, .value = std::nullopt,
                    .error = "Must be a valid number"};
        }
        if (schema.type == SchemaType::Integer &&
            num != static_cast<double>(static_cast<long long>(num))) {
            return {.is_valid = false, .value = std::nullopt,
                    .error = "Must be an integer"};
        }
        if (schema.minimum && num < *schema.minimum) {
            return {.is_valid = false, .value = std::nullopt,
                    .error = "Must be >= " + std::to_string(*schema.minimum)};
        }
        if (schema.maximum && num > *schema.maximum) {
            return {.is_valid = false, .value = std::nullopt,
                    .error = "Must be <= " + std::to_string(*schema.maximum)};
        }
        return {.is_valid = true, .value = num, .error = std::nullopt};
    }

    case SchemaType::Boolean: {
        std::string lower{input};
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "true" || lower == "1" || lower == "yes") {
            return {.is_valid = true, .value = true, .error = std::nullopt};
        }
        if (lower == "false" || lower == "0" || lower == "no") {
            return {.is_valid = true, .value = false, .error = std::nullopt};
        }
        return {.is_valid = false, .value = std::nullopt,
                .error = "Must be true or false"};
    }

    case SchemaType::Array:
        // Array type uses multi-select enum validation
        return {.is_valid = false, .value = std::nullopt,
                .error = "Array type requires multi-select validation"};
    }

    return {.is_valid = false, .value = std::nullopt, .error = "Unknown schema type"};
}

/// Get a format hint/placeholder string for a schema
[[nodiscard]] inline std::optional<std::string> get_format_hint(
    const SchemaDefinition& schema) {

    if (schema.type == SchemaType::String) {
        switch (schema.format) {
        case StringFormat::Email:
            return "email address, e.g. user@example.com";
        case StringFormat::Uri:
            return "URI, e.g. https://example.com";
        case StringFormat::Date:
            return "date, e.g. 2024-03-15";
        case StringFormat::DateTime:
            return "date-time, e.g. 2024-03-15T14:30:00Z";
        case StringFormat::None:
            return std::nullopt;
        }
    }

    if (schema.type == SchemaType::Number || schema.type == SchemaType::Integer) {
        std::string type_name = (schema.type == SchemaType::Integer) ? "integer" : "number";
        if (schema.minimum && schema.maximum) {
            return "(" + type_name + " between " +
                   std::to_string(*schema.minimum) + " and " +
                   std::to_string(*schema.maximum) + ")";
        }
        if (schema.minimum) {
            return "(" + type_name + " >= " + std::to_string(*schema.minimum) + ")";
        }
        if (schema.maximum) {
            return "(" + type_name + " <= " + std::to_string(*schema.maximum) + ")";
        }
        std::string example = (schema.type == SchemaType::Integer) ? "42" : "3.14";
        return "(" + type_name + ", e.g. " + example + ")";
    }

    return std::nullopt;
}

/// Async validation that attempts NL date/time parsing when the input
/// doesn't look like ISO 8601.
[[nodiscard]] inline ValidationResult validate_elicitation_input_with_datetime(
    std::string_view input,
    const SchemaDefinition& schema,
    const DateTimeParseBackend& backend,
    const std::atomic<bool>& cancelled) {

    auto sync_result = validate_elicitation_input(input, schema);
    if (sync_result.is_valid) return sync_result;

    // Attempt NL date/time parsing if applicable
    if (is_date_time_schema(schema) && !looks_like_iso8601(input)) {
        DateTimeFormat fmt = (schema.format == StringFormat::Date)
                                ? DateTimeFormat::Date
                                : DateTimeFormat::DateTime;

        auto parse_result = parse_natural_language_datetime(input, fmt, backend, cancelled);
        if (parse_result.success) {
            auto validated = validate_elicitation_input(parse_result.value, schema);
            if (validated.is_valid) return validated;
        }
    }

    return sync_result;
}

} // namespace cc::utils::mcp_validation
