module;
#include <string>
#include <string_view>
#include <expected>
#include <optional>
#include <map>
#include <chrono>
#include <vector>
#include <cstring>

export module cc.bridge.jwt_utils;

export namespace cc::bridge {

// JWT payload structure (decoded claims)
struct JwtPayload {
    std::string sub;       // Subject
    std::string iss;       // Issuer
    std::chrono::system_clock::time_point exp;  // Expiration
    std::chrono::system_clock::time_point iat;  // Issued at
    std::map<std::string, std::string> claims;  // Additional claims
};

// Base64url decode helper (no padding required)
inline std::expected<std::string, std::string> base64url_decode(std::string_view input) {
    static constexpr const char* B64_CHARS =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // Convert base64url to standard base64
    std::string b64(input);
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Add padding
    while (b64.size() % 4 != 0) b64 += '=';

    // Decode
    std::string result;
    result.reserve(b64.size() * 3 / 4);

    int val = 0, bits = -8;
    for (char c : b64) {
        if (c == '=') break;
        const char* p = std::strchr(B64_CHARS, c);
        if (!p) return std::unexpected("Invalid base64 character");

        val = (val << 6) + static_cast<int>(p - B64_CHARS);
        bits += 6;
        if (bits >= 0) {
            result += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }

    return result;
}

// Extract a JSON string value by key from a flat JSON object
inline std::optional<std::string> extract_json_string(std::string_view json, std::string_view key) {
    std::string search = "\"" + std::string(key) + "\"";
    auto pos = json.find(search);
    if (pos == std::string_view::npos) return std::nullopt;

    pos += search.size();
    // Skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;

    if (pos >= json.size()) return std::nullopt;

    if (json[pos] == '"') {
        // String value
        ++pos;
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
                value += json[pos];
            } else {
                value += json[pos];
            }
            ++pos;
        }
        return value;
    }

    // Numeric value
    std::string value;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ') {
        value += json[pos];
        ++pos;
    }
    return value;
}

// Decode a JWT token (header.payload.signature) without verifying signature
std::expected<JwtPayload, std::string> decode_jwt(std::string_view token) {
    // Split token into parts
    auto first_dot = token.find('.');
    if (first_dot == std::string_view::npos) {
        return std::unexpected("Invalid JWT: missing first dot separator");
    }

    auto second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos) {
        return std::unexpected("Invalid JWT: missing second dot separator");
    }

    // Decode the payload (second segment)
    auto payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    auto decoded = base64url_decode(payload_b64);
    if (!decoded.has_value()) {
        return std::unexpected("Failed to decode JWT payload: " + decoded.error());
    }

    std::string_view payload_json = decoded.value();
    JwtPayload result;

    // Extract standard claims
    if (auto sub = extract_json_string(payload_json, "sub")) {
        result.sub = *sub;
    }
    if (auto iss = extract_json_string(payload_json, "iss")) {
        result.iss = *iss;
    }
    if (auto exp = extract_json_string(payload_json, "exp")) {
        try {
            long long exp_ts = std::stoll(*exp);
            result.exp = std::chrono::system_clock::from_time_t(static_cast<time_t>(exp_ts));
        } catch (...) {}
    }
    if (auto iat = extract_json_string(payload_json, "iat")) {
        try {
            long long iat_ts = std::stoll(*iat);
            result.iat = std::chrono::system_clock::from_time_t(static_cast<time_t>(iat_ts));
        } catch (...) {}
    }

    // Store all claims in the map
    result.claims["sub"] = result.sub;
    result.claims["iss"] = result.iss;

    return result;
}

// Check if a JWT token has expired
bool is_jwt_expired(std::string_view token) {
    auto decoded = decode_jwt(token);
    if (!decoded.has_value()) {
        return true; // Invalid tokens are considered expired
    }

    auto now = std::chrono::system_clock::now();
    return now >= decoded.value().exp;
}

// Get a specific claim from a JWT token
std::optional<std::string> get_jwt_claim(std::string_view token, std::string_view claim) {
    auto decoded = decode_jwt(token);
    if (!decoded.has_value()) {
        return std::nullopt;
    }

    // Check standard fields first
    if (claim == "sub") return decoded.value().sub;
    if (claim == "iss") return decoded.value().iss;

    // Check claims map
    auto it = decoded.value().claims.find(std::string(claim));
    if (it != decoded.value().claims.end()) {
        return it->second;
    }

    return std::nullopt;
}

} // namespace cc::bridge
