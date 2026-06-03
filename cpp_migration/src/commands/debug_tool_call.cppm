module;
#include <format>
#include <string>
#include <string_view>
#include <vector>
export module cc.commands.debug_tool_call;

import cc.utils.json;

export namespace cc::commands::debug_tool_call {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "debug_tool_call"; }

[[nodiscard]] inline auto run(std::string_view payload = {}) -> CommandResponse {
    if (payload.empty()) {
        return {.ok = false, .message = "debug-tool-call requires a JSON tool-call payload"};
    }

    auto parsed = cc::utils::json::parse(payload);
    if (!parsed) {
        return {.ok = false, .message = "Invalid JSON payload: " + parsed.error().message()};
    }

    auto root = parsed->root();
    if (!root.is_obj()) {
        return {.ok = false, .message = "Tool-call payload must be a JSON object"};
    }

    std::vector<std::string> keys;
    root.iter_obj([&](cc::utils::json::JsonVal key, cc::utils::json::JsonVal) {
        if (key.is_str()) keys.emplace_back(key.as_str());
    });

    const auto name = root.get("name");
    const auto input = root.get("input");
    std::string message = std::format(
        "Tool-call payload valid: {} top-level field(s)", keys.size());
    if (name.is_str()) message += "\nTool: " + std::string(name.as_str());
    if (input.valid()) {
        message += std::format("\nInput type: {}", input.is_obj() ? "object" : input.is_arr() ? "array" : input.is_str() ? "string" : input.is_num() ? "number" : input.is_bool() ? "boolean" : "null");
    }
    message += "\nFields:";
    for (const auto& key : keys) message += " " + key;

    return {.ok = true, .message = std::move(message)};
}
}
