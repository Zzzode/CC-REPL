module;

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

export module cc.utils.tagged_id;

export namespace cc::utils::tagged_id {

namespace detail {
    constexpr std::string_view base58_chars = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    constexpr std::string_view version = "01";
    constexpr std::size_t encoded_length = 22;

    [[nodiscard]] inline int hex_value(char ch) noexcept {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }

    [[nodiscard]] inline std::expected<unsigned __int128, std::string> uuid_to_uint128(std::string_view uuid) {
        std::string hex;
        hex.reserve(32);
        for (const char ch : uuid) {
            if (ch == '-') continue;
            hex.push_back(ch);
        }

        if (hex.size() != 32) {
            return std::unexpected("Invalid UUID hex length: " + std::to_string(hex.size()));
        }

        unsigned __int128 value = 0;
        for (const char ch : hex) {
            const int nibble = hex_value(ch);
            if (nibble < 0) {
                return std::unexpected("Invalid UUID hex character");
            }
            value = (value << 4u) | static_cast<unsigned>(nibble);
        }
        return value;
    }

    [[nodiscard]] inline std::string base58_encode(unsigned __int128 value) {
        std::array<char, encoded_length> result{};
        result.fill(base58_chars.front());

        int i = static_cast<int>(encoded_length) - 1;
        constexpr unsigned base = 58;
        while (value > 0 && i >= 0) {
            const auto rem = static_cast<unsigned>(value % base);
            result[static_cast<std::size_t>(i)] = base58_chars[rem];
            value /= base;
            --i;
        }

        return std::string(result.begin(), result.end());
    }
} // namespace detail

[[nodiscard]] inline std::expected<std::string, std::string> to_tagged_id(std::string_view tag, std::string_view uuid) {
    auto parsed = detail::uuid_to_uint128(uuid);
    if (!parsed.has_value()) return std::unexpected(parsed.error());

    std::string out;
    out.reserve(tag.size() + 1 + detail::version.size() + detail::encoded_length);
    out.append(tag);
    out.push_back('_');
    out.append(detail::version);
    out.append(detail::base58_encode(*parsed));
    return out;
}

} // namespace cc::utils::tagged_id
