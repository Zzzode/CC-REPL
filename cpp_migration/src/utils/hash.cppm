module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.hash;

import cc.utils.crypto;

export namespace cc::utils::hash {

namespace detail {
    [[nodiscard]] inline std::vector<std::uint32_t> utf16_code_units(std::string_view utf8) {
        std::vector<std::uint32_t> units;
        units.reserve(utf8.size());

        for (std::size_t i = 0; i < utf8.size();) {
            const auto byte = static_cast<unsigned char>(utf8[i]);
            std::uint32_t cp = 0xfffdu;
            std::size_t consumed = 1;

            if (byte < 0x80u) {
                cp = byte;
            } else if ((byte & 0xe0u) == 0xc0u && i + 1 < utf8.size()) {
                cp = ((byte & 0x1fu) << 6u) |
                     (static_cast<unsigned char>(utf8[i + 1]) & 0x3fu);
                consumed = 2;
            } else if ((byte & 0xf0u) == 0xe0u && i + 2 < utf8.size()) {
                cp = ((byte & 0x0fu) << 12u) |
                     ((static_cast<unsigned char>(utf8[i + 1]) & 0x3fu) << 6u) |
                     (static_cast<unsigned char>(utf8[i + 2]) & 0x3fu);
                consumed = 3;
            } else if ((byte & 0xf8u) == 0xf0u && i + 3 < utf8.size()) {
                cp = ((byte & 0x07u) << 18u) |
                     ((static_cast<unsigned char>(utf8[i + 1]) & 0x3fu) << 12u) |
                     ((static_cast<unsigned char>(utf8[i + 2]) & 0x3fu) << 6u) |
                     (static_cast<unsigned char>(utf8[i + 3]) & 0x3fu);
                consumed = 4;
            }

            if (cp > 0xffffu) {
                units.push_back(0xd800u + ((cp - 0x10000u) >> 10u));
                units.push_back(0xdc00u + ((cp - 0x10000u) & 0x3ffu));
            } else {
                units.push_back(cp);
            }
            i += consumed;
        }

        return units;
    }
} // namespace detail

[[nodiscard]] inline std::int32_t djb2_hash(std::string_view value) {
    std::uint32_t hash = 0;
    for (const auto unit : detail::utf16_code_units(value)) {
        hash = (hash << 5u) - hash + unit;
    }
    return static_cast<std::int32_t>(hash);
}

[[nodiscard]] inline std::string hash_content(std::string_view content) {
    return cc::utils::crypto::sha256(content);
}

[[nodiscard]] inline std::string hash_pair(std::string_view a, std::string_view b) {
    std::string combined;
    combined.reserve(a.size() + 1 + b.size());
    combined.append(a);
    combined.push_back('\0');
    combined.append(b);
    return cc::utils::crypto::sha256(combined);
}

} // namespace cc::utils::hash
