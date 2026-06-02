module;

#include <string>
#include <string_view>
#include <random>
#include <chrono>
#include <array>
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <cctype>

export module cc.utils.uuid_utils;

export namespace cc::utils {

namespace detail {

// Thread-local random engine for UUID generation
inline std::mt19937_64& get_rng() {
    thread_local std::mt19937_64 rng(
        std::chrono::high_resolution_clock::now().time_since_epoch().count() ^
        reinterpret_cast<uint64_t>(&rng)
    );
    return rng;
}

// Convert a byte to two hex characters
inline void byte_to_hex(uint8_t byte, char* out) {
    static constexpr char hex_chars[] = "0123456789abcdef";
    out[0] = hex_chars[(byte >> 4) & 0x0F];
    out[1] = hex_chars[byte & 0x0F];
}

} // namespace detail

// Generate a random UUID v4
inline std::string generate_uuid_v4() {
    auto& rng = detail::get_rng();

    // Generate 16 random bytes
    std::array<uint8_t, 16> bytes{};
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (auto& b : bytes) {
        b = static_cast<uint8_t>(dist(rng));
    }

    // Set version to 4 (bits 6-7 of byte 6 = 0100)
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    // Set variant to RFC 4122 (bits 6-7 of byte 8 = 10xx)
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as UUID string: 8-4-4-4-12
    char uuid[37]{};
    int pos = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid[pos++] = '-';
        }
        detail::byte_to_hex(bytes[i], &uuid[pos]);
        pos += 2;
    }
    uuid[36] = '\0';

    return std::string(uuid, 36);
}

// Generate a time-ordered UUID v7 (RFC 9562)
inline std::string generate_uuid_v7() {
    auto& rng = detail::get_rng();

    // Get Unix timestamp in milliseconds
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::array<uint8_t, 16> bytes{};

    // First 48 bits: Unix timestamp in ms (big-endian)
    bytes[0] = static_cast<uint8_t>((ms >> 40) & 0xFF);
    bytes[1] = static_cast<uint8_t>((ms >> 32) & 0xFF);
    bytes[2] = static_cast<uint8_t>((ms >> 24) & 0xFF);
    bytes[3] = static_cast<uint8_t>((ms >> 16) & 0xFF);
    bytes[4] = static_cast<uint8_t>((ms >> 8) & 0xFF);
    bytes[5] = static_cast<uint8_t>(ms & 0xFF);

    // Fill remaining bytes with random data
    std::uniform_int_distribution<uint16_t> dist(0, 255);
    for (int i = 6; i < 16; ++i) {
        bytes[i] = static_cast<uint8_t>(dist(rng));
    }

    // Set version to 7 (bits 4-7 of byte 6 = 0111)
    bytes[6] = (bytes[6] & 0x0F) | 0x70;
    // Set variant to RFC 4122 (bits 6-7 of byte 8 = 10xx)
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    // Format as UUID string
    char uuid[37]{};
    int pos = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            uuid[pos++] = '-';
        }
        detail::byte_to_hex(bytes[i], &uuid[pos]);
        pos += 2;
    }
    uuid[36] = '\0';

    return std::string(uuid, 36);
}

// Validate a UUID string format
inline bool is_valid_uuid(std::string_view uuid) {
    if (uuid.size() != 36) return false;

    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return false;
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(uuid[i]))) return false;
        }
    }

    return true;
}

// Generate a short alphanumeric ID
inline std::string generate_short_id(size_t length = 8) {
    static constexpr char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr size_t char_count = sizeof(chars) - 1;

    auto& rng = detail::get_rng();
    std::uniform_int_distribution<size_t> dist(0, char_count - 1);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += chars[dist(rng)];
    }
    return result;
}

} // namespace cc::utils
