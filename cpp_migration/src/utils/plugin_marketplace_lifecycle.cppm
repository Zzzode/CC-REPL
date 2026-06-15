// Plugin Marketplace Lifecycle Module
// Consolidates: pluginMarketplace, pluginRegistry, pluginSearch,
//               pluginTrust, pluginVerify
//
// Provides marketplace search, plugin discovery, trust/verification,
// category labeling, and plugin reporting capabilities.
module;

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.utils.plugin_marketplace_lifecycle;
import cc.utils.bash_execution;

export namespace cc::utils::plugins {

// ─────────────────────────────────────────────────────────────────────────────
// Trust & Category Enums (from pluginTrust, pluginVerify)
// ─────────────────────────────────────────────────────────────────────────────

enum class TrustLevel : unsigned char {
    Untrusted,
    Community,
    Verified,
    Official,
};

enum class PluginCategory : unsigned char {
    Tools,
    Commands,
    Themes,
    Languages,
    Integrations,
    Utilities,
};

// ─────────────────────────────────────────────────────────────────────────────
// Marketplace Entry (from pluginMarketplace, pluginRegistry)
// ─────────────────────────────────────────────────────────────────────────────

struct MarketplaceEntry {
    std::string id;
    std::string name;
    std::string description;
    std::string author;
    PluginCategory category;
    TrustLevel trust;
    std::string latest_version;
    std::size_t downloads = 0;
    double rating = 0.0;
    std::chrono::system_clock::time_point published_at;
};

// ─────────────────────────────────────────────────────────────────────────────
// Search Filter (from pluginSearch)
// ─────────────────────────────────────────────────────────────────────────────

struct SearchFilter {
    std::optional<PluginCategory> category;
    std::optional<TrustLevel> min_trust;
    std::optional<std::string> query;

    enum SortBy : unsigned char {
        Relevance,
        Downloads,
        Rating,
        Recent,
    };

    SortBy sort{Relevance};
    std::size_t limit{20};
    std::size_t offset{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// Verification Result (from pluginVerify)
// ─────────────────────────────────────────────────────────────────────────────

struct VerificationResult {
    bool verified = false;
    TrustLevel trust_level = TrustLevel::Untrusted;
    std::optional<std::string> signer;
    std::optional<std::string> warning;
};

// ─────────────────────────────────────────────────────────────────────────────
// Internal State
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

struct TrustStore {
    std::mutex mutex;
    std::unordered_map<std::string, TrustLevel> overrides;
};

inline auto get_trust_store() -> TrustStore& {
    static TrustStore store;
    return store;
}

inline auto get_trust_config_path() -> std::filesystem::path {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::filesystem::path(home) / ".cc-repl" / "trust.json";
}

/// Execute a shell command and return stdout
inline auto exec_cmd(const std::string& cmd) -> std::expected<std::string, std::string> {
    std::array<char, 4096> buffer{};
    std::string result;
    FILE* pipe = cc::utils::bash::popen_spawn(cmd.c_str());
    if (!pipe) return std::unexpected("Failed to execute: " + cmd);
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    int status = cc::utils::bash::pclose_spawn(pipe);
    if (status != 0) return std::unexpected("Command failed with status " + std::to_string(status));
    return result;
}

/// Simple JSON string value extraction
inline auto extract_json_string(const std::string& json, const std::string& key) -> std::string {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return {};
    auto colon = json.find(':', pos);
    if (colon == std::string::npos) return {};
    auto quote_start = json.find('"', colon + 1);
    auto quote_end = json.find('"', quote_start + 1);
    if (quote_start == std::string::npos || quote_end == std::string::npos) return {};
    return json.substr(quote_start + 1, quote_end - quote_start - 1);
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Search & Discovery (from pluginMarketplace, pluginSearch)
// ─────────────────────────────────────────────────────────────────────────────

/// Search the marketplace with optional filters
[[nodiscard]] inline std::expected<std::vector<MarketplaceEntry>, std::string> search_marketplace(
    SearchFilter filter = {}
) {
    // Build search query for npm registry
    std::string query = filter.query.value_or("cc-repl-plugin");
    std::string url = "https://registry.npmjs.org/-/v1/search?text=" + query +
                      "&size=" + std::to_string(filter.limit) +
                      "&from=" + std::to_string(filter.offset);

    std::string cmd = "curl -sf --max-time 10 '" + url + "' 2>/dev/null";
    auto response = detail::exec_cmd(cmd);
    if (!response) {
        return std::unexpected("Marketplace search failed: " + response.error());
    }

    // Parse results (simplified: extract package names from objects array)
    std::vector<MarketplaceEntry> results;
    auto& json = *response;

    // Find "objects" array entries
    std::size_t pos = 0;
    while ((pos = json.find("\"name\"", pos)) != std::string::npos) {
        auto name = detail::extract_json_string(json.substr(pos - 1), "name");
        if (name.empty()) { pos++; continue; }

        auto desc_start = json.find("\"description\"", pos);
        std::string desc;
        if (desc_start != std::string::npos && desc_start < pos + 500) {
            desc = detail::extract_json_string(json.substr(desc_start - 1), "description");
        }

        MarketplaceEntry entry;
        entry.id = name;
        entry.name = name;
        entry.description = desc;
        entry.category = PluginCategory::Utilities;
        entry.trust = TrustLevel::Community;
        results.push_back(std::move(entry));
        pos++;

        if (results.size() >= filter.limit) break;
    }

    return results;
}

/// Get full details for a specific plugin
[[nodiscard]] inline std::expected<MarketplaceEntry, std::string> get_plugin_details(
    std::string_view plugin_id
) {
    std::string cmd = "curl -sf --max-time 10 'https://registry.npmjs.org/" +
                      std::string(plugin_id) + "' 2>/dev/null";
    auto response = detail::exec_cmd(cmd);
    if (!response) {
        return std::unexpected("Failed to fetch plugin details: " + response.error());
    }

    auto& json = *response;
    MarketplaceEntry entry;
    entry.id = std::string(plugin_id);
    entry.name = detail::extract_json_string(json, "name");
    entry.description = detail::extract_json_string(json, "description");
    entry.author = detail::extract_json_string(json, "author");
    entry.latest_version = detail::extract_json_string(json, "latest");
    entry.category = PluginCategory::Utilities;
    entry.trust = TrustLevel::Community;

    if (entry.name.empty()) entry.name = std::string(plugin_id);

    return entry;
}

/// Get a curated list of featured/recommended plugins
[[nodiscard]] inline std::expected<std::vector<MarketplaceEntry>, std::string> get_featured_plugins() {
    SearchFilter filter;
    filter.query = "cc-repl-plugin featured";
    filter.limit = 10;
    return search_marketplace(filter);
}

// ─────────────────────────────────────────────────────────────────────────────
// Verification (from pluginVerify)
// ─────────────────────────────────────────────────────────────────────────────

/// Verify a plugin's signature and integrity for a given version
[[nodiscard]] inline std::expected<VerificationResult, std::string> verify_plugin(
    std::string_view plugin_id,
    std::string_view version
) {
    // Check npm audit signatures
    std::string cmd = "npm audit signatures " + std::string(plugin_id) + "@" +
                      std::string(version) + " 2>&1";
    auto output = detail::exec_cmd(cmd);

    VerificationResult result;
    if (output && output->find("verified") != std::string::npos) {
        result.verified = true;
        result.trust_level = TrustLevel::Verified;
        result.signer = "npm-registry";
    } else {
        result.verified = false;
        result.trust_level = TrustLevel::Untrusted;
        result.warning = "Could not verify plugin signature";
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Trust Management (from pluginTrust)
// ─────────────────────────────────────────────────────────────────────────────

/// Get the trust level for a plugin
[[nodiscard]] inline TrustLevel get_trust_level(std::string_view plugin_id) {
    auto& store = detail::get_trust_store();
    std::lock_guard lock(store.mutex);
    auto it = store.overrides.find(std::string(plugin_id));
    if (it != store.overrides.end()) return it->second;
    return TrustLevel::Untrusted;
}

/// Set the trust level for a plugin (user override)
inline void set_plugin_trust(std::string_view plugin_id, TrustLevel level) {
    auto& store = detail::get_trust_store();
    std::lock_guard lock(store.mutex);
    store.overrides[std::string(plugin_id)] = level;

    // Persist to disk
    namespace fs = std::filesystem;
    auto path = detail::get_trust_config_path();
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::app);
    ofs << std::string(plugin_id) << ":" << static_cast<int>(level) << "\n";
}

/// Get a human-readable label for a trust level
[[nodiscard]] inline std::string_view get_trust_label(TrustLevel level) {
    switch (level) {
        case TrustLevel::Untrusted:   return "Untrusted";
        case TrustLevel::Community:   return "Community";
        case TrustLevel::Verified:    return "Verified";
        case TrustLevel::Official:    return "Official";
    }
    return "Unknown";
}

/// Get a human-readable label for a plugin category
[[nodiscard]] inline std::string_view get_category_label(PluginCategory category) {
    switch (category) {
        case PluginCategory::Tools:        return "Tools";
        case PluginCategory::Commands:     return "Commands";
        case PluginCategory::Themes:       return "Themes";
        case PluginCategory::Languages:    return "Languages";
        case PluginCategory::Integrations: return "Integrations";
        case PluginCategory::Utilities:    return "Utilities";
    }
    return "Unknown";
}

/// Check if a plugin meets the minimum trust threshold for auto-install
[[nodiscard]] inline bool is_plugin_trusted(std::string_view plugin_id) {
    auto level = get_trust_level(plugin_id);
    // Minimum threshold for auto-install is Community
    return level >= TrustLevel::Community;
}

// ─────────────────────────────────────────────────────────────────────────────
// Reporting (from pluginRegistry)
// ─────────────────────────────────────────────────────────────────────────────

/// Report a plugin for policy violation or malicious behavior
[[nodiscard]] inline std::expected<void, std::string> report_plugin(
    std::string_view plugin_id,
    std::string_view reason
) {
    if (plugin_id.empty()) return std::unexpected("Plugin ID required");
    if (reason.empty()) return std::unexpected("Reason required for report");

    // Log the report locally
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    auto reports_dir = fs::path(home ? home : "/tmp") / ".cc-repl" / "reports";
    fs::create_directories(reports_dir);

    std::ofstream ofs(reports_dir / (std::string(plugin_id) + ".report"), std::ios::app);
    ofs << "plugin: " << plugin_id << "\n"
        << "reason: " << reason << "\n"
        << "---\n";

    return {};
}

} // namespace cc::utils::plugins
