module;
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.session_url;

export namespace cc::utils {

inline constexpr std::string_view kBaseUrl = "https://claude.ai/code/session/";
inline constexpr std::string_view kShareBaseUrl = "https://claude.ai/share/";

// Generate a direct URL for a session
std::string generate_session_url(std::string_view session_id) {
    return std::string(kBaseUrl) + std::string(session_id);
}

// Parse a session ID from a session URL
std::optional<std::string> parse_session_url(std::string_view url) {
    // Check if it's a session URL
    if (url.starts_with(kBaseUrl)) {
        auto id = url.substr(kBaseUrl.size());
        if (!id.empty()) return std::string(id);
    }
    // Check share URL format
    if (url.starts_with(kShareBaseUrl)) {
        auto id = url.substr(kShareBaseUrl.size());
        if (!id.empty()) return std::string(id);
    }
    return std::nullopt;
}

// Generate a shareable URL for a session
std::string get_share_url(std::string_view session_id) {
    return std::string(kShareBaseUrl) + std::string(session_id);
}

} // namespace cc::utils
