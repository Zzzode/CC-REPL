// C++23 Cryptographic Utilities Module
// Provides hashing, encoding, random generation, and PKCE support
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.crypto;

import cc.utils.error;

export namespace cc::utils::crypto {

using cc::utils::Error;
using cc::utils::ErrorCode;
using cc::utils::Result;

// =========================================================================

// =========================================================================
namespace detail {


constexpr std::array<uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
constexpr uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
constexpr uint32_t sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
constexpr uint32_t sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
constexpr uint32_t gamma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
constexpr uint32_t gamma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

inline std::array<uint8_t, 32> sha256_raw(const uint8_t* data, std::size_t len) {

    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };


    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    std::size_t padded_len = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded_len, 0);
    std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;


    for (int i = 0; i < 8; ++i) {
        msg[padded_len - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    }


    for (std::size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[offset + i * 4]) << 24)
                  | (static_cast<uint32_t>(msg[offset + i * 4 + 1]) << 16)
                  | (static_cast<uint32_t>(msg[offset + i * 4 + 2]) << 8)
                  | (static_cast<uint32_t>(msg[offset + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = gamma1(w[i-2]) + w[i-7] + gamma0(w[i-15]) + w[i-16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + sigma1(e) + ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = sigma0(a) + maj(a, b, c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }


    std::array<uint8_t, 32> result;
    for (int i = 0; i < 8; ++i) {
        result[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
        result[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        result[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        result[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
    return result;
}

} // namespace detail

[[nodiscard]] inline std::string sha256(std::string_view data) {
    auto hash = detail::sha256_raw(
        reinterpret_cast<const uint8_t*>(data.data()), data.size());

    std::string hex;
    hex.reserve(64);
    for (uint8_t byte : hash) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

/// Returns lowercase hex of a SHA-256 raw byte array (or any 32-byte block).
/// Exposed as a detail helper so HMAC / SigV4 code shares the same printer.
[[nodiscard]] inline std::string sha256_bytes_to_hex(
    const std::array<uint8_t, 32>& hash) {
    std::string hex;
    hex.reserve(64);
    for (uint8_t byte : hash) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

// =========================================================================
// Random
// =========================================================================


[[nodiscard]] inline std::vector<uint8_t> random_bytes(std::size_t n) {
    std::vector<uint8_t> result(n);
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& byte : result) {
        byte = static_cast<uint8_t>(dist(rd));
    }
    return result;
}

// =========================================================================
// Base64
// =========================================================================
namespace detail {

constexpr std::string_view base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::string_view base64url_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

} // namespace detail


[[nodiscard]] inline std::string base64_encode(std::string_view data) {
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);

    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    std::size_t i = 0;

    while (i + 2 < data.size()) {
        uint32_t triple = (bytes[i] << 16) | (bytes[i+1] << 8) | bytes[i+2];
        result += detail::base64_chars[(triple >> 18) & 0x3F];
        result += detail::base64_chars[(triple >> 12) & 0x3F];
        result += detail::base64_chars[(triple >> 6) & 0x3F];
        result += detail::base64_chars[triple & 0x3F];
        i += 3;
    }

    if (i < data.size()) {
        uint32_t triple = bytes[i] << 16;
        if (i + 1 < data.size()) triple |= bytes[i+1] << 8;

        result += detail::base64_chars[(triple >> 18) & 0x3F];
        result += detail::base64_chars[(triple >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? detail::base64_chars[(triple >> 6) & 0x3F] : '=';
        result += '=';
    }
    return result;
}

/// Byte-span overload for signing outputs (JWT, etc.).
[[nodiscard]] inline std::string base64_encode(std::span<const uint8_t> bytes) {
    return base64_encode(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

/// Raw-pointer + length overload for convenience.
[[nodiscard]] inline std::string base64_encode(const uint8_t* data,
                                                std::size_t n) {
    return base64_encode(std::string_view(
        reinterpret_cast<const char*>(data), n));
}


[[nodiscard]] inline Result<std::vector<uint8_t>> base64_decode(std::string_view str) {

    auto decode_char = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };


    auto len = str.size();
    while (len > 0 && str[len - 1] == '=') --len;

    std::vector<uint8_t> result;
    result.reserve((len * 3) / 4);

    uint32_t accumulator = 0;
    int bits = 0;

    for (std::size_t i = 0; i < len; ++i) {
        int val = decode_char(str[i]);
        if (val < 0) {
            return std::unexpected(Error(ErrorCode::parse_error,
                std::format("Invalid base64 character at position {}", i)));
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return result;
}

// =========================================================================

// =========================================================================
[[nodiscard]] inline std::string generate_uuid() {
    auto bytes = random_bytes(16);


    bytes[6] = (bytes[6] & 0x0F) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80; // variant 1

    return std::format(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
}

// =========================================================================
// PKCE (RFC 7636) - OAuth 2.0 Proof Key for Code Exchange
// =========================================================================


[[nodiscard]] inline std::string generate_code_verifier() {
    constexpr std::string_view charset =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    constexpr std::size_t length = 64;

    auto bytes = random_bytes(length);
    std::string verifier;
    verifier.reserve(length);
    for (auto b : bytes) {
        verifier += charset[b % charset.size()];
    }
    return verifier;
}


[[nodiscard]] inline std::string generate_code_challenge(std::string_view verifier) {
    auto hash = detail::sha256_raw(
        reinterpret_cast<const uint8_t*>(verifier.data()), verifier.size());


    std::string result;
    result.reserve(44);
    std::size_t i = 0;
    while (i + 2 < hash.size()) {
        uint32_t triple = (hash[i] << 16) | (hash[i+1] << 8) | hash[i+2];
        result += detail::base64url_chars[(triple >> 18) & 0x3F];
        result += detail::base64url_chars[(triple >> 12) & 0x3F];
        result += detail::base64url_chars[(triple >> 6) & 0x3F];
        result += detail::base64url_chars[triple & 0x3F];
        i += 3;
    }

    if (i < hash.size()) {
        uint32_t triple = hash[i] << 16;
        if (i + 1 < hash.size()) triple |= hash[i+1] << 8;
        result += detail::base64url_chars[(triple >> 18) & 0x3F];
        result += detail::base64url_chars[(triple >> 12) & 0x3F];
        if (i + 1 < hash.size()) {
            result += detail::base64url_chars[(triple >> 6) & 0x3F];
        }
    }
    return result;
}

// =========================================================================

// =========================================================================
[[nodiscard]] inline bool constant_time_compare(
    std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;

    volatile uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    return diff == 0;
}

// =========================================================================
// HMAC-SHA256 (FIPS 198-1) — general purpose, 32-byte raw output + hex.
// Block size B = 64 for SHA-256. Inner pad = 0x36 repeated B times,
// outer pad = 0x5C repeated B times.  H(k XOR opad, H(k XOR ipad, msg)).
// =========================================================================
namespace detail {
inline std::array<uint8_t, 32> hmac_sha256_raw(
    const uint8_t* key, std::size_t key_len,
    const uint8_t* msg, std::size_t msg_len) {
    constexpr std::size_t B = 64; // SHA-256 block size
    std::array<uint8_t, 64> k_padded{};
    if (key_len <= B) {
        std::memcpy(k_padded.data(), key, key_len);
    } else {
        // Keys longer than block size get hashed first.
        auto digest = sha256_raw(key, key_len);
        std::memcpy(k_padded.data(), digest.data(), 32);
    }
    std::array<uint8_t, 64> inner;
    std::array<uint8_t, 64> outer;
    for (std::size_t i = 0; i < B; ++i) {
        inner[i] = static_cast<uint8_t>(k_padded[i] ^ 0x36);
        outer[i] = static_cast<uint8_t>(k_padded[i] ^ 0x5C);
    }
    // Concatenate: inner || msg, hash it, then outer || hash.
    std::vector<uint8_t> inner_input;
    inner_input.reserve(B + msg_len);
    inner_input.insert(inner_input.end(), inner.begin(), inner.end());
    inner_input.insert(inner_input.end(), msg, msg + msg_len);
    auto inner_hash = sha256_raw(inner_input.data(), inner_input.size());
    std::vector<uint8_t> outer_input;
    outer_input.reserve(B + 32);
    outer_input.insert(outer_input.end(), outer.begin(), outer.end());
    outer_input.insert(outer_input.end(), inner_hash.begin(), inner_hash.end());
    return sha256_raw(outer_input.data(), outer_input.size());
}
} // namespace detail

/// Returns 32 raw bytes of HMAC-SHA256(key, data).
[[nodiscard]] inline std::array<uint8_t, 32> hmac_sha256(
    std::span<const uint8_t> key, std::span<const uint8_t> data) {
    return detail::hmac_sha256_raw(
        key.data(), key.size(), data.data(), data.size());
}

/// Overload taking string_view for key + data — convenient for text inputs.
[[nodiscard]] inline std::array<uint8_t, 32> hmac_sha256(
    std::string_view key, std::string_view data) {
    return detail::hmac_sha256_raw(
        reinterpret_cast<const uint8_t*>(key.data()), key.size(),
        reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

/// Returns 64 lowercase hex chars of HMAC-SHA256(key, data).
[[nodiscard]] inline std::string hmac_sha256_hex(
    std::string_view key, std::string_view data) {
    auto raw = hmac_sha256(key, data);
    return sha256_bytes_to_hex(raw);
}

} // namespace cc::utils::crypto
