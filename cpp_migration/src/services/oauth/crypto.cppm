module;
#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
export module cc.services.oauth.crypto;

import cc.utils.crypto;

export namespace cc::services::oauth {

// Base64url encode a byte span (no padding, URL-safe)
auto base64url_encode(std::span<const uint8_t> data) -> std::string {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string result;
    result.reserve((data.size() * 4 + 2) / 3);

    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t triplet = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        result += table[(triplet >> 18) & 0x3F];
        result += table[(triplet >> 12) & 0x3F];
        result += table[(triplet >> 6) & 0x3F];
        result += table[triplet & 0x3F];
        i += 3;
    }
    if (i + 1 == data.size()) {
        uint32_t val = data[i] << 16;
        result += table[(val >> 18) & 0x3F];
        result += table[(val >> 12) & 0x3F];
    } else if (i + 2 == data.size()) {
        uint32_t val = (data[i] << 16) | (data[i + 1] << 8);
        result += table[(val >> 18) & 0x3F];
        result += table[(val >> 12) & 0x3F];
        result += table[(val >> 6) & 0x3F];
    }
    return result;
}

// Generate PKCE code verifier (43-128 chars, unreserved characters)
auto generate_code_verifier() -> std::string {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);

    std::array<uint8_t, 32> random_bytes;
    for (auto& byte : random_bytes) {
        byte = static_cast<uint8_t>(dist(gen));
    }
    return base64url_encode(random_bytes);
}

// Generate PKCE code challenge (S256 method)
auto generate_code_challenge(std::string_view verifier) -> std::string {
    return cc::utils::crypto::generate_code_challenge(verifier);
}

// Generate random state parameter for CSRF protection
auto generate_state() -> std::string {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);

    std::array<uint8_t, 16> random_bytes;
    for (auto& byte : random_bytes) {
        byte = static_cast<uint8_t>(dist(gen));
    }
    return base64url_encode(random_bytes);
}

} // namespace cc::services::oauth
