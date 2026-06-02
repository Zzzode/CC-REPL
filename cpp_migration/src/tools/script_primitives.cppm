module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <functional>

export module cc.tools.script_primitives;

export namespace cc::tools::script_primitives {

struct PrimitiveTool {
    std::string name;
    std::string description;
    std::vector<std::string> parameters;
    std::function<std::expected<std::string, std::string>(const std::vector<std::string>&)> execute;
};

inline std::vector<PrimitiveTool> get_primitive_tools() {
    return {};
}

inline std::expected<std::string, std::string> execute_primitive(std::string_view tool_name, const std::vector<std::string>& args) {
    return "";
}

inline bool is_primitive_tool(std::string_view name) {
    return false;
}

inline std::vector<std::string> list_primitive_names() {
    return {};
}

} // namespace cc::tools::script_primitives
