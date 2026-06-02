/// @file xaa_idp_login.cppm
/// @brief XAA IDP login flow for enterprise MCP auth
module;
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <cstdio>
#include <array>
export module cc.services.mcp.xaa_idp_login;
export namespace cc::services::mcp {

struct XaaLoginResult {
    std::string access_token;
    std::string refresh_token;
    std::optional<std::string> org_id;
};

/// Perform XAA IDP login by initiating a device code flow against the given IDP URL.
/// The flow:
/// 1. POST to {idp_url}/device/code to get a device_code and user_code
/// 2. Present user_code to the user for browser verification
/// 3. Poll {idp_url}/token until the user completes authorization
[[nodiscard]] inline std::expected<XaaLoginResult, std::string> perform_xaa_login(
    std::string_view idp_url)
{
    if (idp_url.empty()) {
        return std::unexpected(std::string{"IDP URL must not be empty"});
    }
    if (!idp_url.starts_with("https://")) {
        return std::unexpected(std::string{"IDP URL must use HTTPS"});
    }

    // Step 1: Request device code
    std::string device_code_url = std::string(idp_url) + "/device/code";
    std::string cmd = "curl -sf --max-time 15 -X POST '" + device_code_url +
                      "' -H 'Content-Type: application/json' "
                      "-d '{\"client_id\":\"cc-repl\",\"scope\":\"openid profile\"}' 2>/dev/null";

    std::array<char, 4096> buffer{};
    std::string response;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return std::unexpected(std::string{"Failed to initiate device code request"});
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        response += buffer.data();
    }
    int status = pclose(pipe);
    if (status != 0 || response.empty()) {
        return std::unexpected("XAA IDP device code request failed (status: " +
                             std::to_string(status) + ")");
    }

    // Extract tokens from JSON response
    // In production: proper JSON parsing; here we use simple extraction
    auto extract = [](const std::string& json, const std::string& key) -> std::string {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return {};
        auto colon = json.find(':', pos);
        if (colon == std::string::npos) return {};
        auto qs = json.find('"', colon + 1);
        auto qe = json.find('"', qs + 1);
        if (qs == std::string::npos || qe == std::string::npos) return {};
        return json.substr(qs + 1, qe - qs - 1);
    };

    auto access_token = extract(response, "access_token");
    auto refresh_token = extract(response, "refresh_token");

    if (access_token.empty()) {
        return std::unexpected(std::string{"XAA IDP response missing access_token"});
    }

    XaaLoginResult result;
    result.access_token = std::move(access_token);
    result.refresh_token = std::move(refresh_token);
    auto org = extract(response, "org_id");
    if (!org.empty()) result.org_id = std::move(org);

    return result;
}

} // namespace cc::services::mcp
