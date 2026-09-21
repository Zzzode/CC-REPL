module;
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>
export module cc.services.oauth.crypto;

import cc.utils.crypto;

export namespace cc::services::oauth {

namespace detail {

[[nodiscard]] inline bool fill_from_urandom(std::span<uint8_t> bytes) {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.is_open()) return false;
    urandom.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return urandom.gcount() == static_cast<std::streamsize>(bytes.size());
}

[[nodiscard]] inline bool fill_from_random_device(std::span<uint8_t> bytes) noexcept {
    try {
        std::random_device rd;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto value = static_cast<uint32_t>(rd());
            for (int shift = 0; shift < 32 && offset < bytes.size(); shift += 8) {
                bytes[offset++] = static_cast<uint8_t>((value >> shift) & 0xff);
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline void fill_fallback(std::span<uint8_t> bytes) {
    static std::atomic<uint64_t> counter{0};
    const auto now = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto sequence = counter.fetch_add(1, std::memory_order_relaxed);
    const auto address = static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(bytes.data()));
    std::seed_seq seed{
        static_cast<uint32_t>(now),
        static_cast<uint32_t>(now >> 32),
        static_cast<uint32_t>(sequence),
        static_cast<uint32_t>(sequence >> 32),
        static_cast<uint32_t>(address),
        static_cast<uint32_t>(address >> 32),
    };
    std::mt19937_64 gen(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& byte : bytes) {
        byte = static_cast<uint8_t>(dist(gen));
    }
}

inline void fill_random_bytes(std::span<uint8_t> bytes) {
    if (fill_from_urandom(bytes)) return;
    if (fill_from_random_device(bytes)) return;
    fill_fallback(bytes);
}

} // namespace detail

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
    std::array<uint8_t, 32> random_bytes;
    detail::fill_random_bytes(random_bytes);
    return base64url_encode(random_bytes);
}

// Generate PKCE code challenge (S256 method)
auto generate_code_challenge(std::string_view verifier) -> std::string {
    return cc::utils::crypto::generate_code_challenge(verifier);
}

// Generate random state parameter for CSRF protection
auto generate_state() -> std::string {
    std::array<uint8_t, 16> random_bytes;
    detail::fill_random_bytes(random_bytes);
    return base64url_encode(random_bytes);
}

} // namespace cc::services::oauth
