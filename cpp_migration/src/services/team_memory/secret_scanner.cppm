/// @file secret_scanner.cppm
/// @brief Secret detection in team memory content
module;
#include <string>
#include <string_view>
#include <vector>
#include <regex>
export module cc.services.team_memory.secret_scanner;
export namespace cc::services::team_memory {
struct SecretMatch { std::string type; std::size_t start{0}; std::size_t end{0}; };
[[nodiscard]] inline std::vector<SecretMatch> scan_for_secrets(std::string_view content) {
    std::vector<SecretMatch> matches;
    // Simple pattern checks
    if (content.find("sk-") != std::string_view::npos) matches.push_back({"api_key", 0, 0});
    if (content.find("ghp_") != std::string_view::npos) matches.push_back({"github_token", 0, 0});
    if (content.find("-----BEGIN") != std::string_view::npos) matches.push_back({"private_key", 0, 0});
    return matches;
}
[[nodiscard]] inline bool contains_secrets(std::string_view content) { return !scan_for_secrets(content).empty(); }
} // namespace
