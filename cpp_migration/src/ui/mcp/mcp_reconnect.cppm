/// @file mcp_reconnect.cppm
/// @brief MCP reconnection UI with retry status
module;
#include <string>
#include <optional>
#include <cstdint>
#include <format>
#include <ftxui/dom/elements.hpp>
export module cc.ui.mcp.mcp_reconnect;
export namespace cc::ui::mcp {
using namespace ftxui;
struct ReconnectState { std::string server_name; uint32_t attempt{0}; uint32_t max_attempts{5}; bool is_reconnecting{false}; std::optional<std::string> last_error; };
[[nodiscard]] inline Element render_reconnect_status(const ReconnectState& state) {
    if (!state.is_reconnecting) return text("");
    std::vector<Element> elements;
    elements.push_back(text(std::format("Reconnecting to {} ({}/{})", state.server_name, state.attempt, state.max_attempts)) | color(Color::Yellow));
    if (state.last_error) elements.push_back(text("  Error: " + *state.last_error) | color(Color::Red) | dim);
    return vbox(elements);
}
} // namespace
