module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <functional>
#include <cstdint>

export module cc.utils.tool_management;

export namespace cc::utils::tool_management {

struct ToolError {
    std::string tool_name;
    std::string error_code;
    std::string message;
    std::optional<std::string> suggestion;
};

struct ToolPoolEntry {
    std::string name;
    bool available{true};
    uint32_t usage_count{0};
};

struct ToolResult {
    std::string tool_name;
    std::string result_data;
    std::optional<std::string> storage_key;
};

struct ToolSchema {
    std::string name;
    std::string description;
    std::vector<std::string> parameters;
};

inline std::expected<std::vector<ToolPoolEntry>, std::string> get_tool_pool() {
    return {};
}

inline std::expected<void, std::string> store_tool_result(const ToolResult&) {
    return {};
}

inline std::optional<ToolResult> get_stored_result(std::string_view) {
    return std::nullopt;
}

inline std::optional<ToolSchema> get_tool_schema(std::string_view) {
    return std::nullopt;
}

inline std::vector<ToolSchema> search_tools(std::string_view) {
    return {};
}

inline std::vector<std::string> get_embedded_tool_names() {
    return {};
}

inline std::string render_tool_lookup(std::string_view tool_name) {
    return std::string(tool_name);
}

inline std::string format_tool_error(const ToolError& err) {
    return err.message;
}

} // namespace cc::utils::tool_management
