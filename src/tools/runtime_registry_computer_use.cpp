// Implementation unit for cc.tools.runtime_registry — native computer-use
// routing: the local command backend, action parsing, the connected
// computer-use MCP server check, and the exported testing override setters.
module;

#include <cctype>   // std::isalnum in normalize_name_for_mcp
#include <cstdlib>  // std::getenv for LOOM_COMPUTER_USE_CMD

module cc.tools.runtime_registry;

import std;

import cc.tools.tool;
import cc.tools.computer_use;
import cc.tools.runtime_computer_use;
import cc.tools.mcp;
import cc.utils.json;
import cc.utils.bash_execution;
import cc.services.image;

namespace cc::tools::detail {

namespace {

/// Name of the conventional computer-use MCP server. TS REF:
/// src/utils/computerUse/common.ts:4 COMPUTER_USE_MCP_SERVER_NAME.
constexpr std::string_view kComputerUseMcpServerName = "computer-use";

} // namespace

[[nodiscard]] std::optional<cc::core::computer_use::ActionType> parse_computer_action(
    std::string_view action) {
    using cc::core::computer_use::ActionType;
    if (action == "screenshot" || action == "cursor_position") return ActionType::Screenshot;
    if (action == "move" || action == "mouse_move") return ActionType::MouseMove;
    // Anthropic computer_20241022 wire names plus local aliases.
    if (action == "click" || action == "mouse_click" ||
        action == "left_click") return ActionType::MouseClick;
    if (action == "double_click") return ActionType::MouseDoubleClick;
    if (action == "right_click") return ActionType::MouseRightClick;
    if (action == "drag" || action == "left_click_drag") return ActionType::MouseDrag;
    if (action == "type") return ActionType::KeyType;
    if (action == "press" || action == "key") return ActionType::KeyPress;
    if (action == "hotkey") return ActionType::KeyHotkey;
    if (action == "scroll") return ActionType::Scroll;
    return std::nullopt;
}

[[nodiscard]] std::expected<std::string, std::string> run_computer_use_command_backend(
    const cc::core::computer_use::ComputerAction& action
) {
    auto* command_env = std::getenv("LOOM_COMPUTER_USE_CMD");
    if (!command_env || std::string_view(command_env).empty()) {
        return std::unexpected("Computer-use command backend is not configured");
    }

    auto payload = runtime_computer_use::command_request_json(action);
    auto quoted_payload = runtime_shell_quote(payload);
    std::string command = command_env;
    if (command.find("{request}") != std::string::npos) {
        runtime_computer_use::replace_all(command, "{request}", quoted_payload);
    } else {
        command += ' ';
        command += quoted_payload;
    }

    auto cap = cc::utils::bash::exec_capture(command);
    if (!cap) return std::unexpected("Failed to start computer-use command backend");
    std::string output = std::move(cap->output);
    if (output.size() > 1024 * 512) {
        return std::unexpected("Computer-use command backend output exceeded 512 KiB");
    }
    auto status = cap->status;
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    if (status != 0) {
        return std::unexpected(std::format("Computer-use command backend failed with status {}", status));
    }
    if (output.empty()) return std::unexpected("Computer-use command backend returned no JSON");
    return output;
}

[[nodiscard]] std::optional<std::string> computer_json_optional_string(
    cc::utils::json::JsonVal root,
    std::string_view key
) {
    auto value = root.get(key);
    if (!value || !value.is_str()) return std::nullopt;
    return std::string(value.as_str());
}

[[nodiscard]] std::expected<ComputerUseCommandBackendResult, std::string> parse_computer_command_result(
    std::string_view output
) {
    auto parsed = cc::utils::json::parse(output);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Computer-use command backend returned invalid JSON");
    }
    auto root = parsed->root();
    if (auto success = root.get("success"); success && success.is_bool() && !success.as_bool()) {
        return std::unexpected(
            computer_json_optional_string(root, "error")
                .or_else([&] { return computer_json_optional_string(root, "message"); })
                .value_or("Computer-use command backend reported failure"));
    }
    ComputerUseCommandBackendResult result{
        .screenshot_base64 = computer_json_optional_string(root, "screenshot_base64")
            .or_else([&] { return computer_json_optional_string(root, "base64"); })
            .or_else([&] { return computer_json_optional_string(root, "data"); }),
        .format = computer_json_optional_string(root, "format"),
        .width = std::nullopt,
        .height = std::nullopt,
    };
    if (auto width = root.get("width"); width && width.is_num()) result.width = width.as_int();
    if (auto height = root.get("height"); height && height.is_num()) result.height = height.as_int();
    return result;
}

[[nodiscard]] std::optional<cc::core::computer_use::CaptureProvider> computer_use_command_capture_provider() {
    auto* command_env = std::getenv("LOOM_COMPUTER_USE_CMD");
    if (!command_env || std::string_view(command_env).empty()) return std::nullopt;
    return [](std::optional<cc::core::computer_use::Rect> region)
        -> std::expected<cc::core::computer_use::ImageData, std::string> {
        cc::core::computer_use::ComputerAction action{
            .type = cc::core::computer_use::ActionType::Screenshot,
            .position = std::nullopt,
            .drag_end = std::nullopt,
            .text = std::nullopt,
            .region = region,
            .keys = {},
        };
        auto output = run_computer_use_command_backend(action);
        if (!output) return std::unexpected(output.error());
        auto root = parse_computer_command_result(*output);
        if (!root) return std::unexpected(root.error());

        if (!root->screenshot_base64) return std::unexpected("Computer-use command backend did not return screenshot_base64");
        auto decoded = cc::services::image::ImageService::from_base64(*root->screenshot_base64);
        if (!decoded) return std::unexpected(decoded.error().message);

        if (!root->width || !root->height || *root->width <= 0 || *root->height <= 0) {
            return std::unexpected("Computer-use command backend screenshot requires positive width and height");
        }
        return cc::core::computer_use::ImageData{
            .pixels = std::move(*decoded),
            .width = static_cast<std::uint32_t>(*root->width),
            .height = static_cast<std::uint32_t>(*root->height),
            .format = root->format.value_or("rgba"),
        };
    };
}

[[nodiscard]] std::optional<cc::core::computer_use::InputProvider> computer_use_command_input_provider() {
    auto* command_env = std::getenv("LOOM_COMPUTER_USE_CMD");
    if (!command_env || std::string_view(command_env).empty()) return std::nullopt;
    return [](const cc::core::computer_use::ComputerAction& action) -> std::expected<void, std::string> {
        auto output = run_computer_use_command_backend(action);
        if (!output) return std::unexpected(output.error());
        auto root = parse_computer_command_result(*output);
        if (!root) return std::unexpected(root.error());
        return {};
    };
}

/// TS REF: src/utils/computerUse/common.ts:59 isComputerUseMCPServer +
/// src/services/mcp/normalization.ts:17 normalizeNameForMCP.
[[nodiscard]] std::string normalize_name_for_mcp(std::string name) {
    std::ranges::replace_if(name, [](char c) {
        return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
    }, '_');
    return name;
}

/// Return the connected MCP server name that hosts the native computer tool,
/// if one is configured and ready. A deployment supplies a computer-use MCP
/// server (real screen capture + input injection, often a VM); when present,
/// computer actions are forwarded there instead of the local host adapter,
/// which only implements macOS capture.
[[nodiscard]] std::optional<std::string>
connected_computer_use_mcp_server() {
    for (const auto& server : NativeMcpRuntime::instance().all_statuses()) {
        if (server.status == "ready" &&
            normalize_name_for_mcp(server.name) ==
                kComputerUseMcpServerName) {
            return server.name;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Result<ToolResult> execute_computer_use(const ToolInput& input) {
    auto json = input.json();

    // Prefer a configured computer-use MCP server: forward the model's native
    // computer action verbatim (native action names like left_click/key
    // differ from the local adapter's vocabulary, so this check runs BEFORE
    // local action parsing) and preserve its screenshot image block.
    // TS REF: src/services/mcp/client.ts:924 in-process Computer Use MCP
    // server; src/utils/computerUse/wrapper.tsx .call() override.
    if (auto server = connected_computer_use_mcp_server()) {
        auto mcp_result = NativeMcpRuntime::instance().call_tool(
            *server, "computer", std::string{input.json()});
        if (mcp_result) {
            return mcp_result_to_tool_result(*mcp_result);
        }
        // Fail closed with the routing error rather than silently running
        // the wrong (local) backend.
        return ToolResult::error(std::format(
            "computer-use MCP server '{}' rejected the action: {}",
            *server, format_error(mcp_result.error())));
    }

    auto action_text = json_string(json, "action").value_or("screenshot");
    auto action = parse_computer_action(action_text);
    if (!action) {
        return ToolResult::error(std::format("Unsupported computer-use action: {}", action_text));
    }

    auto point_from_xy = [&] -> std::optional<cc::core::computer_use::Point> {
        auto x = json_int(json, "x");
        auto y = json_int(json, "y");
        // Native computer_20241022 sends "coordinate":[x,y].
        if ((!x || !y)) {
            if (auto parsed = cc::utils::json::parse(json); parsed) {
                auto coord = parsed->root().get("coordinate");
                if (coord.is_arr() && coord.size() >= 2) {
                    return cc::core::computer_use::Point{
                        .x = static_cast<int32_t>(coord.at(0).as_int()),
                        .y = static_cast<int32_t>(coord.at(1).as_int()),
                    };
                }
            }
        }
        if (!x || !y) return std::nullopt;
        return cc::core::computer_use::Point{.x = *x, .y = *y};
    };

    cc::core::computer_use::ComputerAction request{
        .type = *action,
        .position = point_from_xy(),
        .drag_end = std::nullopt,
        .text = json_string(json, "text").or_else([&] { return json_string(json, "key"); }),
        .region = std::nullopt,
        .keys = json_string_array(json, "keys"),
    };
    if (request.keys.empty() && *action == cc::core::computer_use::ActionType::KeyHotkey) {
        request.keys = json_string_array(json, "key");
        if (request.keys.empty()) request.keys = json_string_array(json, "text");
    }

    if (auto end_x = json_int(json, "to_x"), end_y = json_int(json, "to_y"); end_x && end_y) {
        request.drag_end = cc::core::computer_use::Point{.x = *end_x, .y = *end_y};
    }
    if (auto w = json_int(json, "width"), h = json_int(json, "height"); w && h && *w > 0 && *h > 0) {
        request.region = cc::core::computer_use::Rect{
            .x = json_int(json, "x").value_or(0),
            .y = json_int(json, "y").value_or(0),
            .width = static_cast<std::uint32_t>(*w),
            .height = static_cast<std::uint32_t>(*h),
        };
    }

    auto command_capture_provider = computer_use_command_capture_provider();
    auto command_input_provider = computer_use_command_input_provider();
    cc::core::computer_use::ComputerUseManager manager{
        computer_use_capture_provider_override
            ? cc::core::computer_use::ScreenCapture{*computer_use_capture_provider_override}
            : (command_capture_provider
                ? cc::core::computer_use::ScreenCapture{*command_capture_provider}
                : cc::core::computer_use::ScreenCapture{}),
        computer_use_input_provider_override
            ? *computer_use_input_provider_override
            : (command_input_provider
                ? *command_input_provider
                : cc::core::computer_use::make_native_input_provider())
    };
    auto result = manager.execute_action(request);
    if (!result.success) {
        return ToolResult::error(result.error_message);
    }
    if (result.screenshot) {
        auto data = cc::services::image::ImageService::to_base64(
            std::span<const std::uint8_t>(result.screenshot->pixels.data(), result.screenshot->pixels.size()));
        // Pass through the backend-declared encoding, restricted to the
        // media types the Anthropic API accepts (png/jpeg/webp/gif). Native
        // macOS capture and a well-formed computer-use MCP server return one
        // of these. The internal raw-pixel marker "rgba" (and anything
        // unrecognized) is not a wire type, so default to png rather than
        // emitting an image/rgba the API rejects.
        const auto& fmt = result.screenshot->format;
        const char* media_type =
            (fmt == "jpeg" || fmt == "jpg") ? "image/jpeg" :
            (fmt == "webp") ? "image/webp" :
            (fmt == "gif")  ? "image/gif"  :
            (fmt == "png")  ? "image/png" : "image/png";
        return ToolResult::success_multi({
            ToolOutputContent::text_output(std::format(
                "Captured screenshot {}x{}.",
                result.screenshot->width,
                result.screenshot->height)),
            ToolOutputContent::image_output(media_type, std::move(data)),
        });
    }
    return ToolResult::success(std::format("Computer-use action completed: {}", action_text));
}

} // namespace cc::tools::detail

namespace cc::tools {

void set_runtime_computer_use_capture_provider_for_testing(
    cc::core::computer_use::CaptureProvider provider) {
    detail::computer_use_capture_provider_override = std::move(provider);
}

void clear_runtime_computer_use_capture_provider_for_testing() {
    detail::computer_use_capture_provider_override.reset();
}

void set_runtime_computer_use_input_provider_for_testing(
    cc::core::computer_use::InputProvider provider) {
    detail::computer_use_input_provider_override = std::move(provider);
}

void clear_runtime_computer_use_input_provider_for_testing() {
    detail::computer_use_input_provider_override.reset();
}

} // namespace cc::tools
