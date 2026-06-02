module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <functional>
#include <cstdint>

export module cc.utils.mcp_transport;

export namespace cc::utils::mcp_transport {

enum class TransportType { Stdio, WebSocket, InProcess };

struct TransportConfig {
    TransportType type;
    std::optional<std::string> endpoint;
    std::optional<std::uint16_t> port;
};

struct InstructionsDelta {
    std::string server_id;
    std::vector<std::string> added;
    std::vector<std::string> removed;
};

inline std::expected<void, std::string> connect_transport([[maybe_unused]] const TransportConfig& config) {
    return {};
}

inline std::expected<void, std::string> disconnect_transport([[maybe_unused]] std::string_view server_id) {
    return {};
}

inline std::expected<std::string, std::string> send_message([[maybe_unused]] std::string_view server_id, [[maybe_unused]] std::string_view message) {
    return "";
}

inline InstructionsDelta compute_instructions_delta([[maybe_unused]] const std::vector<std::string>& old_instructions, [[maybe_unused]] const std::vector<std::string>& new_instructions) {
    return {"", {}, {}};
}

inline std::expected<void, std::string> store_mcp_output([[maybe_unused]] std::string_view key, [[maybe_unused]] std::string_view data) {
    return {};
}

inline std::optional<std::string> get_mcp_output([[maybe_unused]] std::string_view key) {
    return std::nullopt;
}

} // namespace cc::utils::mcp_transport
