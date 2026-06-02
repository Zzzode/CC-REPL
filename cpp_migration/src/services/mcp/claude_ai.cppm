/// @file claude_ai.cppm
/// @brief Claude.ai web integration service
module;
#include <string>
#include <optional>
#include <expected>
export module cc.services.mcp.claude_ai;
export namespace cc::services::mcp {
struct ClaudeAiSession { std::string session_id; std::string org_id; bool is_active{false}; };
[[nodiscard]] inline std::expected<ClaudeAiSession, std::string> create_claude_ai_session() {
    return std::unexpected("Claude.ai integration not available in CLI mode");
}
} // namespace
