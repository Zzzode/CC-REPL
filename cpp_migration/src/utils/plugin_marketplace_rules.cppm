module;

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

export module cc.utils.plugin_marketplace_rules;

import cc.utils.plugin_identifier;

export namespace cc::utils::plugin_marketplace_rules {

enum class MarketplaceSourceType : unsigned char {
    Github,
    Git,
    Other,
};

struct MarketplaceSource {
    MarketplaceSourceType type = MarketplaceSourceType::Other;
    std::string repo;
    std::string url;
};

[[nodiscard]] inline std::string lower_copy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

[[nodiscard]] inline bool is_marketplace_auto_update(std::string_view marketplace_name, std::optional<bool> configured_auto_update = std::nullopt) {
    if (configured_auto_update.has_value()) return *configured_auto_update;
    const auto normalized = lower_copy(marketplace_name);
    return cc::utils::plugin_identifier::is_official_marketplace_name(normalized) && normalized != "knowledge-work-plugins";
}

[[nodiscard]] inline bool contains_non_ascii(std::string_view value) noexcept {
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch > 0x7e) return true;
    }
    return false;
}

[[nodiscard]] inline bool has_separator_between(std::string_view name, std::string_view first, std::string_view second) {
    const auto first_pos = name.find(first);
    if (first_pos == std::string_view::npos) return false;
    const auto second_pos = name.find(second, first_pos + first.size());
    if (second_pos == std::string_view::npos) return false;
    for (std::size_t i = first_pos + first.size(); i < second_pos; ++i) {
        const char ch = name[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) return false;
    }
    return true;
}

[[nodiscard]] inline bool is_blocked_official_name(std::string_view name) {
    const auto normalized = lower_copy(name);
    if (cc::utils::plugin_identifier::is_official_marketplace_name(normalized)) return false;
    if (contains_non_ascii(name)) return true;
    if (has_separator_between(normalized, "official", "anthropic") || has_separator_between(normalized, "official", "claude")) return true;
    if (has_separator_between(normalized, "anthropic", "official") || has_separator_between(normalized, "claude", "official")) return true;
    for (std::string_view brand : {"anthropic", "claude"}) {
        if (normalized.starts_with(brand)) {
            auto rest = normalized.substr(brand.size());
            while (!rest.empty() && !((rest.front() >= 'a' && rest.front() <= 'z') || (rest.front() >= '0' && rest.front() <= '9'))) rest.erase(0, 1);
            if (rest.starts_with("marketplace") || rest.starts_with("plugins") || rest.starts_with("official")) return true;
        }
    }
    return false;
}

[[nodiscard]] inline std::optional<std::string> validate_official_name_source(std::string_view name, const MarketplaceSource& source) {
    const auto normalized = lower_copy(name);
    if (!cc::utils::plugin_identifier::is_official_marketplace_name(normalized)) return std::nullopt;
    constexpr std::string_view official_org = "anthropics";
    const auto message = "The name '" + std::string(name) + "' is reserved for official Anthropic marketplaces. Only repositories from 'github.com/anthropics/' can use this name.";
    if (source.type == MarketplaceSourceType::Github) {
        const auto repo = lower_copy(source.repo);
        if (repo.starts_with(std::string(official_org) + "/")) return std::nullopt;
        return message;
    }
    if (source.type == MarketplaceSourceType::Git) {
        const auto url = lower_copy(source.url);
        if (url.find("github.com/anthropics/") != std::string::npos || url.find("git@github.com:anthropics/") != std::string::npos) return std::nullopt;
        return message;
    }
    return message;
}

} // namespace cc::utils::plugin_marketplace_rules
