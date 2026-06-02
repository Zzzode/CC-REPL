module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.ssrf_guard;

export namespace cc::utils::ssrf_guard {

[[nodiscard]] inline std::optional<std::array<int, 4>> parse_ipv4(std::string_view address) {
    std::array<int, 4> parts{};
    std::size_t start = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto dot = address.find('.', start);
        const auto end = dot == std::string_view::npos ? address.size() : dot;
        if (end == start) return std::nullopt;
        int value = 0;
        const auto* first = address.data() + start;
        const auto* last = address.data() + end;
        auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last || value < 0 || value > 255) return std::nullopt;
        parts[i] = value;
        if (i < 3) {
            if (dot == std::string_view::npos) return std::nullopt;
            start = dot + 1;
        } else if (dot != std::string_view::npos) {
            return std::nullopt;
        }
    }
    return parts;
}

[[nodiscard]] inline bool is_blocked_v4_parts(const std::array<int, 4>& parts) {
    const int a = parts[0];
    const int b = parts[1];
    if (a == 127) return false;
    if (a == 0) return true;
    if (a == 10) return true;
    if (a == 169 && b == 254) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;
    if (a == 192 && b == 168) return true;
    return false;
}

[[nodiscard]] inline bool is_blocked_v4(std::string_view address) {
    auto parts = parse_ipv4(address);
    return parts ? is_blocked_v4_parts(*parts) : false;
}

[[nodiscard]] inline std::optional<std::uint16_t> parse_hextet(std::string_view value) {
    if (value.empty() || value.size() > 4) return std::nullopt;
    std::uint16_t out = 0;
    for (char ch : value) {
        out <<= 4;
        if (ch >= '0' && ch <= '9') out += static_cast<std::uint16_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') out += static_cast<std::uint16_t>(10 + ch - 'a');
        else if (ch >= 'A' && ch <= 'F') out += static_cast<std::uint16_t>(10 + ch - 'A');
        else return std::nullopt;
    }
    return out;
}

[[nodiscard]] inline std::vector<std::string_view> split_sv(std::string_view value, char delim) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto pos = value.find(delim, start);
        const auto end = pos == std::string_view::npos ? value.size() : pos;
        parts.push_back(value.substr(start, end - start));
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return parts;
}

[[nodiscard]] inline std::optional<std::array<std::uint16_t, 8>> expand_ipv6_groups(std::string_view input) {
    std::string addr(input);
    std::vector<std::uint16_t> tail_hextets;
    if (addr.find('.') != std::string::npos) {
        const auto last_colon = addr.rfind(':');
        if (last_colon == std::string::npos) return std::nullopt;
        auto octets = parse_ipv4(std::string_view(addr).substr(last_colon + 1));
        if (!octets) return std::nullopt;
        tail_hextets.push_back(static_cast<std::uint16_t>(((*octets)[0] << 8) | (*octets)[1]));
        tail_hextets.push_back(static_cast<std::uint16_t>(((*octets)[2] << 8) | (*octets)[3]));
        addr.resize(last_colon);
    }

    const auto dbl = addr.find("::");
    if (dbl != std::string::npos && addr.find("::", dbl + 2) != std::string::npos) return std::nullopt;

    std::vector<std::string_view> head;
    std::vector<std::string_view> tail;
    if (dbl == std::string::npos) {
        head = split_sv(addr, ':');
    } else {
        const auto head_str = std::string_view(addr).substr(0, dbl);
        const auto tail_str = std::string_view(addr).substr(dbl + 2);
        if (!head_str.empty()) head = split_sv(head_str, ':');
        if (!tail_str.empty()) tail = split_sv(tail_str, ':');
    }

    const std::size_t target = 8 - tail_hextets.size();
    if (head.size() + tail.size() > target) return std::nullopt;
    const std::size_t fill = dbl == std::string::npos ? 0 : target - head.size() - tail.size();
    if (dbl == std::string::npos && head.size() + tail_hextets.size() != 8) return std::nullopt;

    std::array<std::uint16_t, 8> groups{};
    std::size_t idx = 0;
    for (auto part : head) {
        auto h = parse_hextet(part);
        if (!h) return std::nullopt;
        groups[idx++] = *h;
    }
    idx += fill;
    for (auto part : tail) {
        auto h = parse_hextet(part);
        if (!h) return std::nullopt;
        groups[idx++] = *h;
    }
    for (auto h : tail_hextets) groups[idx++] = h;
    if (idx != 8) return std::nullopt;
    return groups;
}

[[nodiscard]] inline std::optional<std::array<int, 4>> extract_mapped_ipv4(std::string_view addr) {
    const auto groups = expand_ipv6_groups(addr);
    if (!groups) return std::nullopt;
    if ((*groups)[0] == 0 && (*groups)[1] == 0 && (*groups)[2] == 0 && (*groups)[3] == 0 &&
        (*groups)[4] == 0 && (*groups)[5] == 0xffff) {
        const auto hi = (*groups)[6];
        const auto lo = (*groups)[7];
        return std::array<int, 4>{hi >> 8, hi & 0xff, lo >> 8, lo & 0xff};
    }
    return std::nullopt;
}

[[nodiscard]] inline bool is_blocked_v6(std::string_view address) {
    std::string lower(address);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower == "::1") return false;
    if (lower == "::") return true;
    if (auto mapped = extract_mapped_ipv4(lower)) return is_blocked_v4_parts(*mapped);
    if (lower.starts_with("fc") || lower.starts_with("fd")) return true;
    const auto first_hextet_end = lower.find(':');
    const auto first_hextet = std::string_view(lower).substr(0, first_hextet_end);
    if (first_hextet.size() == 4 && first_hextet >= "fe80" && first_hextet <= "febf") return true;
    return false;
}

[[nodiscard]] inline bool is_blocked_address(std::string_view address) {
    if (address.find('.') != std::string_view::npos && address.find(':') == std::string_view::npos) {
        return is_blocked_v4(address);
    }
    if (address.find(':') != std::string_view::npos) {
        return is_blocked_v6(address);
    }
    return false;
}

} // namespace cc::utils::ssrf_guard
