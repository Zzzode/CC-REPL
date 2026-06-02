/// @file in_process_transport.cppm
/// @brief In-process MCP transport for embedded servers
module;
#include <string>
#include <functional>
#include <expected>
#include <memory>
export module cc.services.mcp.in_process_transport;
export namespace cc::services::mcp {
struct InProcessMessage { std::string method; std::string params_json; };
struct InProcessTransport {
    std::function<std::expected<std::string, std::string>(const InProcessMessage&)> send;
    std::function<void(std::function<void(const std::string&)>)> on_message;
};
[[nodiscard]] inline std::expected<InProcessTransport, std::string> create_in_process_transport() {
    return InProcessTransport{
        .send = [](const InProcessMessage&) -> std::expected<std::string, std::string> { return "{}"; },
        .on_message = [](auto) {},
    };
}
} // namespace
