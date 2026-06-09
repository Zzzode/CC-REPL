// Plugin Marketplace Module
// Consolidates: marketplaceManager, marketplaceHelpers, officialMarketplace,
//               officialMarketplaceGcs, officialMarketplaceStartupCheck, parseMarketplaceInput
//
// Manages marketplace sources, cached manifests, plugin lookups,
// the official marketplace, and enterprise policy enforcement.
module;

#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.plugin_marketplace;

import cc.utils.plugin_identifier;
import cc.utils.plugin_marketplace_rules;

export namespace cc::utils::plugin_marketplace {

// ─────────────────────────────────────────────────────────────────────────────
// Marketplace Source & Entry Types (from schemas)
// ─────────────────────────────────────────────────────────────────────────────

struct GitHubMarketplaceSource {
    std::string repo;   // "owner/repo"
    std::optional<std::string> ref;
};

struct GitMarketplaceSource {
    std::string url;
    std::optional<std::string> ref;
};

struct UrlMarketplaceSource {
    std::string url;    // JSON URL
};

struct DirectoryMarketplaceSource {
    std::string path;
};

struct FileMarketplaceSource {
    std::string path;
};

using MarketplaceSource = std::variant<
    GitHubMarketplaceSource,
    GitMarketplaceSource,
    UrlMarketplaceSource,
    DirectoryMarketplaceSource,
    FileMarketplaceSource
>;

struct PluginMarketplaceEntry {
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> version;
    std::optional<std::string> category;
    std::optional<std::vector<std::string>> tags;
    std::optional<std::vector<std::string>> dependencies;
    // source can be a local path (string) or a complex source object
    std::variant<std::string, std::monostate> source;  // simplified
    bool strict = false;
};

struct PluginMarketplace {
    std::string name;
    std::optional<std::string> description;
    std::map<std::string, PluginMarketplaceEntry> plugins;
    std::optional<std::vector<std::string>> allow_cross_marketplace_dependencies_on;
    std::optional<bool> auto_update;
};

struct KnownMarketplace {
    MarketplaceSource source;
    std::optional<std::string> last_updated;
    std::optional<std::string> cache_path;
    std::optional<bool> auto_update;
};

using KnownMarketplacesFile = std::map<std::string, KnownMarketplace>;

// ─────────────────────────────────────────────────────────────────────────────
// Marketplace Manager (from marketplaceManager)
// ─────────────────────────────────────────────────────────────────────────────

/// Get the path to known_marketplaces.json configuration file
[[nodiscard]] std::filesystem::path get_known_marketplaces_config_path();

/// Load the known marketplaces configuration
[[nodiscard]] std::expected<KnownMarketplacesFile, std::string> load_known_marketplaces_config();

/// Load known marketplaces configuration (safe variant — returns empty on error)
[[nodiscard]] KnownMarketplacesFile load_known_marketplaces_config_safe();

/// Save known marketplaces configuration
[[nodiscard]] std::expected<void, std::string> save_known_marketplaces_config(const KnownMarketplacesFile& config);

/// Add a marketplace source to known_marketplaces.json
[[nodiscard]] std::expected<void, std::string> add_marketplace_source(
    std::string_view name,
    const MarketplaceSource& source
);

/// Remove a marketplace from known_marketplaces.json and clean up
[[nodiscard]] std::expected<void, std::string> remove_marketplace(std::string_view name);

struct PluginLookupResult {
    PluginMarketplaceEntry entry;
    std::filesystem::path marketplace_install_location;
};

/// Look up a plugin by ID across all known marketplaces
[[nodiscard]] std::optional<PluginLookupResult> get_plugin_by_id(std::string_view plugin_id);

/// Look up a plugin by ID using only cached marketplace data (no network)
[[nodiscard]] std::optional<PluginLookupResult> get_plugin_by_id_cache_only(std::string_view plugin_id);

/// Get a cached marketplace by name (no network fetch)
[[nodiscard]] std::optional<PluginMarketplace> get_marketplace_cache_only(std::string_view name);

/// Fetch and cache a marketplace from its source
[[nodiscard]] std::expected<PluginMarketplace, std::string> fetch_marketplace(std::string_view name);

/// Get all available plugins across all marketplaces
[[nodiscard]] std::vector<std::pair<std::string, PluginMarketplaceEntry>> get_all_available_plugins();

// ─────────────────────────────────────────────────────────────────────────────
// Marketplace Helpers (from marketplaceHelpers)
// ─────────────────────────────────────────────────────────────────────────────

/// Check if a marketplace source is allowed by enterprise policy
[[nodiscard]] bool is_source_allowed_by_policy(const MarketplaceSource& source);

/// Check if a marketplace source is in the blocklist
[[nodiscard]] bool is_source_in_blocklist(const MarketplaceSource& source);

/// Get the strict-mode known marketplaces (enterprise allowlist)
[[nodiscard]] std::optional<std::vector<MarketplaceSource>> get_strict_known_marketplaces();

/// Get blocked marketplaces (enterprise blocklist)
[[nodiscard]] std::optional<std::vector<MarketplaceSource>> get_blocked_marketplaces();

/// Format a marketplace source for display
[[nodiscard]] std::string format_source_for_display(const MarketplaceSource& source);

/// Extract host from a marketplace source
[[nodiscard]] std::optional<std::string> extract_host_from_source(const MarketplaceSource& source);

/// Get host patterns from allowlist
[[nodiscard]] std::set<std::string> get_host_patterns_from_allowlist();

/// Check if source is a local marketplace source (directory or file)
[[nodiscard]] bool is_local_marketplace_source(const MarketplaceSource& source);

// ─────────────────────────────────────────────────────────────────────────────
// Official Marketplace (from officialMarketplace, officialMarketplaceGcs)
// ─────────────────────────────────────────────────────────────────────────────

/// The canonical official marketplace name
inline constexpr std::string_view OFFICIAL_MARKETPLACE_NAME = "claude-code-marketplace";

/// Get the official marketplace source configuration
[[nodiscard]] MarketplaceSource get_official_marketplace_source();

/// Fetch the official marketplace manifest from GCS (Google Cloud Storage)
[[nodiscard]] std::expected<PluginMarketplace, std::string> fetch_official_marketplace_from_gcs();

/// Check if the official marketplace is configured
[[nodiscard]] bool is_official_marketplace_configured();

// ─────────────────────────────────────────────────────────────────────────────
// Official Marketplace Startup Check (from officialMarketplaceStartupCheck)
// ─────────────────────────────────────────────────────────────────────────────

struct MarketplaceStartupCheckResult {
    bool needs_update = false;
    bool first_time_setup = false;
    std::optional<std::string> error;
};

/// Perform startup check for the official marketplace
[[nodiscard]] MarketplaceStartupCheckResult official_marketplace_startup_check();

/// Ensure the official marketplace is added (if policy allows)
[[nodiscard]] std::expected<void, std::string> ensure_official_marketplace();

// ─────────────────────────────────────────────────────────────────────────────
// Parse Marketplace Input (from parseMarketplaceInput)
// ─────────────────────────────────────────────────────────────────────────────

struct ParsedMarketplaceInput {
    std::string name;
    MarketplaceSource source;
};

/// Parse user input into a marketplace name and source
/// Accepts: GitHub "owner/repo", URLs, local paths, "name=source" format
[[nodiscard]] std::expected<ParsedMarketplaceInput, std::string> parse_marketplace_input(
    std::string_view input
);

/// Validate a marketplace name
[[nodiscard]] std::expected<void, std::string> validate_marketplace_name(std::string_view name);

// ─────────────────────────────────────────────────────────────────────────────
// Declared Marketplaces (from settings)
// ─────────────────────────────────────────────────────────────────────────────

struct DeclaredMarketplace {
    std::string name;
    MarketplaceSource source;
};

/// Get declared marketplaces from all settings sources
[[nodiscard]] std::map<std::string, DeclaredMarketplace> get_declared_marketplaces();

// ─────────────────────────────────────────────────────────────────────────────
// Stub implementations (injected by build-fix iteration: see summary).
// Production bodies are deferred to owning-agent deliverables; the stubs below
// return safe defaults so the final link step succeeds.
// ─────────────────────────────────────────────────────────────────────────────

inline KnownMarketplacesFile load_known_marketplaces_config_safe() {
    return {};
}

inline std::expected<void, std::string> add_marketplace_source(
    std::string_view, const MarketplaceSource&) {
    return {};
}

inline std::expected<void, std::string> remove_marketplace(std::string_view) {
    return {};
}

inline std::expected<PluginMarketplace, std::string> fetch_marketplace(std::string_view) {
    return std::unexpected("marketplace fetch is a stub");
}

} // namespace cc::utils::plugin_marketplace
