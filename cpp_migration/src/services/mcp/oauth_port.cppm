/// @file oauth_port.cppm
/// @brief OAuth callback port management for MCP auth
module;
#include <string>
#include <cstdint>
#include <expected>
export module cc.services.mcp.oauth_port;
export namespace cc::services::mcp {
inline constexpr uint16_t kDefaultOAuthPort = 8912;
inline constexpr uint16_t kOAuthPortRangeStart = 8900;
inline constexpr uint16_t kOAuthPortRangeEnd = 8999;
[[nodiscard]] inline std::expected<uint16_t, std::string> find_available_oauth_port() { return kDefaultOAuthPort; }
} // namespace
