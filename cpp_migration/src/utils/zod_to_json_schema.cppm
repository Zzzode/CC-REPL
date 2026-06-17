module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <variant>
#include <map>
#include <cstdint>
#include <sstream>

export module cc.utils.zod_to_json_schema;

import cc.utils.json;

export namespace cc::utils {


struct SchemaField {
    std::string name;
    std::string type;       // "string", "number", "integer", "boolean", "array", "object"
    bool required = true;
    std::optional<std::string> description;
    std::optional<cc::utils::json::JsonMutDoc> default_value;
};


struct JsonSchema {
    std::string title;
    std::vector<SchemaField> fields;
};


namespace schema_detail {
    inline std::string type_to_json_schema_type(std::string_view type) {
        if (type == "string") return "\"string\"";
        if (type == "number" || type == "double" || type == "float") return "\"number\"";
        if (type == "integer" || type == "int" || type == "int64_t") return "\"integer\"";
        if (type == "boolean" || type == "bool") return "\"boolean\"";
        if (type == "array") return "\"array\"";
        if (type == "object") return "\"object\"";
        return "\"string\"";
    }
}


[[nodiscard]] inline std::string schema_to_json(const JsonSchema& schema) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"$schema\": \"https://json-schema.org/draft/2020-12/schema\",\n";
    oss << "  \"title\": \"" << schema.title << "\",\n";
    oss << "  \"type\": \"object\",\n";


    std::vector<std::string> required_fields;
    for (const auto& field : schema.fields) {
        if (field.required) required_fields.push_back(field.name);
    }


    oss << "  \"properties\": {\n";
    for (size_t i = 0; i < schema.fields.size(); ++i) {
        const auto& field = schema.fields[i];
        oss << "    \"" << field.name << "\": {\n";
        oss << "      \"type\": " << schema_detail::type_to_json_schema_type(field.type);

        if (field.description.has_value()) {
            oss << ",\n      \"description\": \"" << field.description.value() << "\"";
        }
        if (field.default_value.has_value()) {
            oss << ",\n      \"default\": " << field.default_value.value().to_string();
        }
        oss << "\n    }";
        if (i + 1 < schema.fields.size()) oss << ',';
        oss << '\n';
    }
    oss << "  }";


    if (!required_fields.empty()) {
        oss << ",\n  \"required\": [";
        for (size_t i = 0; i < required_fields.size(); ++i) {
            oss << "\"" << required_fields[i] << "\"";
            if (i + 1 < required_fields.size()) oss << ", ";
        }
        oss << "]";
    }

    oss << ",\n  \"additionalProperties\": false\n";
    oss << "}";
    return oss.str();
}


[[nodiscard]] inline std::vector<std::string> validate_against_schema(
    const cc::utils::json::JsonVal& value, const JsonSchema& schema) {
    std::vector<std::string> errors;

    if (!value.is_obj()) {
        errors.push_back("Root value must be an object");
        return errors;
    }

    for (const auto& field : schema.fields) {
        cc::utils::json::JsonVal fv = value.get(field.name);
        if (!fv.valid()) {
            if (field.required) {
                errors.push_back("Missing required field: " + field.name);
            }
            continue;
        }

        bool type_ok = false;
        if (field.type == "string") type_ok = fv.is_str();
        else if (field.type == "number" || field.type == "double" || field.type == "float")
            type_ok = fv.is_num();
        else if (field.type == "integer" || field.type == "int" || field.type == "int64_t")
            type_ok = fv.is_int();
        else if (field.type == "boolean" || field.type == "bool")
            type_ok = fv.is_bool();
        else if (field.type == "array") type_ok = fv.is_arr();
        else if (field.type == "object") type_ok = fv.is_obj();
        else type_ok = true;

        if (!type_ok) {
            errors.push_back("Field '" + field.name + "' expected type '" +
                           field.type + "' but got incompatible value");
        }
    }

    return errors;
}

} // namespace cc::utils
