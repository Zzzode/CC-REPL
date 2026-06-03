/// @file xaa_idp_login.cppm
/// @brief XAA IDP login flow for enterprise MCP auth
module;
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <format>
#include <unordered_map>
export module cc.services.mcp.xaa_idp_login;

import cc.utils.http;
import cc.utils.json;

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

    std::string device_code_url = std::string(idp_url) + "/device/code";
    auto request = cc::utils::json::object();
    request.set("client_id", "cc-repl");
    request.set("scope", "openid profile");

    cc::utils::HttpClient client;
    auto response = client.post(device_code_url, request.serialize(), std::unordered_map<std::string, std::string>{
        {"Content-Type", "application/json"},
    });
    if (!response) {
        return std::unexpected(response.error().message);
    }
    if (!response->is_ok()) {
        return std::unexpected(std::format(
            "XAA IDP device code request failed with status {}: {}",
            response->status, response->body));
    }

    auto parsed = cc::utils::json::parse(response->body);
    if (!parsed) {
        return std::unexpected(std::format("XAA IDP response was not valid JSON: {}", parsed.error().message()));
    }

    auto root = parsed->root();
    if (!root || !root.is_obj()) {
        return std::unexpected("XAA IDP response must be a JSON object");
    }

    auto access_value = root.get("access_token");
    auto refresh_value = root.get("refresh_token");
    std::string access_token = access_value && access_value.is_str() ? std::string(access_value.as_str()) : "";
    std::string refresh_token = refresh_value && refresh_value.is_str() ? std::string(refresh_value.as_str()) : "";

    if (access_token.empty()) {
        auto user_code = root.get("user_code");
        auto verification_uri = root.get("verification_uri");
        if (user_code && user_code.is_str() && verification_uri && verification_uri.is_str()) {
            return std::unexpected(std::format(
                "XAA IDP authorization is pending. Open {} and enter code {}.",
                verification_uri.as_str(), user_code.as_str()));
        }
        return std::unexpected(std::string{"XAA IDP response missing access_token"});
    }

    XaaLoginResult result;
    result.access_token = std::move(access_token);
    result.refresh_token = std::move(refresh_token);
    if (auto org = root.get("org_id"); org && org.is_str()) {
        result.org_id = std::string(org.as_str());
    }

    return result;
}

} // namespace cc::services::mcp
