module;
#include <compare>
#include <expected>
#include <string>
#include <string_view>

export module cc.utils.semver;

export namespace cc::utils {

struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;
    std::string build_metadata;

    auto operator<=>(const SemVer& other) const {
        if (auto cmp = major <=> other.major; cmp != 0) return cmp;
        if (auto cmp = minor <=> other.minor; cmp != 0) return cmp;
        if (auto cmp = patch <=> other.patch; cmp != 0) return cmp;
        // Pre-release versions have lower precedence than release
        if (prerelease.empty() && !other.prerelease.empty()) return std::strong_ordering::greater;
        if (!prerelease.empty() && other.prerelease.empty()) return std::strong_ordering::less;
        if (auto cmp = prerelease <=> other.prerelease; cmp != 0) return cmp;
        return std::strong_ordering::equal;
    }

    bool operator==(const SemVer& other) const = default;
};

// Parse a semantic version string (e.g., "1.2.3-beta+build")
std::expected<SemVer, std::string> parse_semver(std::string_view input) {
    SemVer ver;

    // Strip leading 'v' if present
    if (!input.empty() && input[0] == 'v') input.remove_prefix(1);

    // Parse major
    auto dot1 = input.find('.');
    if (dot1 == std::string_view::npos) {
        return std::unexpected("Missing minor version");
    }
    try {
        ver.major = std::stoi(std::string(input.substr(0, dot1)));
    } catch (...) {
        return std::unexpected("Invalid major version");
    }

    // Parse minor
    auto rest = input.substr(dot1 + 1);
    auto dot2 = rest.find('.');
    if (dot2 == std::string_view::npos) {
        return std::unexpected("Missing patch version");
    }
    try {
        ver.minor = std::stoi(std::string(rest.substr(0, dot2)));
    } catch (...) {
        return std::unexpected("Invalid minor version");
    }

    // Parse patch (may have prerelease/build suffix)
    rest = rest.substr(dot2 + 1);
    auto hyphen = rest.find('-');
    auto plus = rest.find('+');

    std::string_view patch_str;
    if (hyphen != std::string_view::npos) {
        patch_str = rest.substr(0, hyphen);
    } else if (plus != std::string_view::npos) {
        patch_str = rest.substr(0, plus);
    } else {
        patch_str = rest;
    }

    try {
        ver.patch = std::stoi(std::string(patch_str));
    } catch (...) {
        return std::unexpected("Invalid patch version");
    }

    // Parse prerelease
    if (hyphen != std::string_view::npos) {
        auto pre_end = rest.find('+', hyphen);
        if (pre_end != std::string_view::npos) {
            ver.prerelease = std::string(rest.substr(hyphen + 1, pre_end - hyphen - 1));
        } else {
            ver.prerelease = std::string(rest.substr(hyphen + 1));
        }
    }

    // Parse build metadata
    if (plus != std::string_view::npos) {
        ver.build_metadata = std::string(rest.substr(plus + 1));
    }

    return ver;
}

// Compare two semver values (-1, 0, 1)
int compare(const SemVer& a, const SemVer& b) {
    auto cmp = a <=> b;
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return 0;
}

// Check if a version satisfies a range expression (simplified npm-style)
bool satisfies(const SemVer& ver, std::string_view range) {
    // Handle ^x.y.z (compatible with version)
    if (range.starts_with("^")) {
        auto target = parse_semver(range.substr(1));
        if (!target) return false;
        // ^1.2.3 means >=1.2.3 and <2.0.0
        if (ver.major != target->major) return false;
        if (ver.major == 0) {
            // ^0.x.y is more restrictive
            return ver.minor == target->minor && ver.patch >= target->patch;
        }
        return (ver <=> *target) >= 0;
    }

    // Handle ~x.y.z (approximately equivalent)
    if (range.starts_with("~")) {
        auto target = parse_semver(range.substr(1));
        if (!target) return false;
        // ~1.2.3 means >=1.2.3 and <1.3.0
        return ver.major == target->major &&
               ver.minor == target->minor &&
               ver.patch >= target->patch;
    }

    // Handle >=x.y.z
    if (range.starts_with(">=")) {
        auto target = parse_semver(range.substr(2));
        if (!target) return false;
        return (ver <=> *target) >= 0;
    }

    // Handle >x.y.z
    if (range.starts_with(">") && !range.starts_with(">=")) {
        auto target = parse_semver(range.substr(1));
        if (!target) return false;
        return (ver <=> *target) > 0;
    }

    // Handle <=x.y.z
    if (range.starts_with("<=")) {
        auto target = parse_semver(range.substr(2));
        if (!target) return false;
        return (ver <=> *target) <= 0;
    }

    // Handle <x.y.z
    if (range.starts_with("<") && !range.starts_with("<=")) {
        auto target = parse_semver(range.substr(1));
        if (!target) return false;
        return (ver <=> *target) < 0;
    }

    // Handle =x.y.z or exact match
    std::string_view target_str = range;
    if (target_str.starts_with("=")) target_str.remove_prefix(1);
    auto target = parse_semver(target_str);
    if (!target) return false;
    return ver == *target;
}

// Serialize SemVer to string
std::string to_string(const SemVer& ver) {
    std::string result = std::to_string(ver.major) + "." +
                         std::to_string(ver.minor) + "." +
                         std::to_string(ver.patch);
    if (!ver.prerelease.empty()) {
        result += "-" + ver.prerelease;
    }
    if (!ver.build_metadata.empty()) {
        result += "+" + ver.build_metadata;
    }
    return result;
}

} // namespace cc::utils
