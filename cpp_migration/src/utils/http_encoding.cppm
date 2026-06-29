// Centralized URL / URI encoding utilities (RFC 3986).
// Replaces ~10 duplicate implementations scattered across the codebase.
// Two strictness levels per AWS/Azure/GCP spec:
//   * uri_encode_path   — RFC 3986 section 2.3, keep '/' (AWS canonical URI)
//   * uri_encode        — RFC 3986 section 2.2, unreserved only (AWS query
//                         params, OAuth form fields, GCP OAuth exchange)
// Unreserved (never pct-encoded): A-Z a-z 0-9 - _ . ~
module;
#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

export module cc.utils.http_encoding;

export namespace cc::utils::http {

// RFC 3986 unreserved charset: ALPHA / DIGIT / "-" / "." / "_" / "~"
[[nodiscard]] constexpr bool is_unreserved(unsigned char c) noexcept {
    return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

// Strict URI encode per RFC 3986: every byte that is NOT in the unreserved
// set is percent-encoded as %XX with UPPERCASE hex digits (AWS SigV4 spec
// mandates uppercase). When `keep_slash` is true, forward slashes are passed
// through unencoded (used for canonical URI path of S3-style resources).
[[nodiscard]] inline std::string uri_encode(std::string_view s,
                                             bool keep_slash = false) {
    std::string out;
    out.reserve(s.size() + (s.size() >> 1));
    for (unsigned char c : s) {
        if (is_unreserved(c) || (keep_slash && c == '/')) {
            out.push_back(static_cast<char>(c));
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

// AWS SigV4 canonical path variant: always keep '/'.
[[nodiscard]] inline std::string uri_encode_path(std::string_view s) {
    return uri_encode(s, /*keep_slash=*/true);
}

// application/x-www-form-urlencoded (HTML forms, OAuth token exchange).
// Differs from RFC 3986 in one way: spaces become '+' not %20.
// Reserved: everything except unreserved chars; note ' ' (space) becomes '+'.
[[nodiscard]] inline std::string form_encode(std::string_view s) {
    std::string out;
    out.reserve(s.size() + (s.size() >> 1));
    for (unsigned char c : s) {
        if (c == ' ') {
            out.push_back('+');
        } else if (is_unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

// Percent-decode a string. Accepts both uppercase and lowercase hex.
// On invalid percent-sequences, the malformed bytes are passed through
// verbatim (lenient decoder, consistent with curl / browsers).
// '+' is decoded as space ONLY when `plus_is_space` is true (forms mode).
[[nodiscard]] inline std::string url_decode(std::string_view s,
                                             bool plus_is_space = false) {
    std::string out;
    out.reserve(s.size());
    const auto xdigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (plus_is_space && c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size()) {
            int hi = xdigit(s[i + 1]);
            int lo = xdigit(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace cc::utils::http
