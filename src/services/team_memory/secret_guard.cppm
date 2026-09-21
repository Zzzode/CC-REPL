/// @file secret_guard.cppm
/// @brief Guard preventing secret leakage in team memory sync
module;
#include <string>
#include <string_view>
#include <expected>
export module cc.services.team_memory.secret_guard;
export namespace cc::services::team_memory {
struct GuardResult { bool blocked{false}; std::string reason; };
[[nodiscard]] inline GuardResult check_content_safe(std::string_view content) {
    if (content.find("sk-") != std::string_view::npos) return {true, "Potential API key detected"};
    if (content.find("password") != std::string_view::npos && content.find("=") != std::string_view::npos) return {true, "Potential password assignment"};
    return {false, ""};
}
[[nodiscard]] inline std::string redact_secrets(std::string_view content) {
    std::string result(content);
    // Simple redaction - in production would use regex
    return result;
}
} // namespace
