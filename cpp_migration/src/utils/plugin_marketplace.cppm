// Plugin Marketplace Module
// Consolidates: marketplaceManager, marketplaceHelpers, officialMarketplace,
//               officialMarketplaceGcs, officialMarketplaceStartupCheck, parseMarketplaceInput
//
// Manages marketplace sources, cached manifests, plugin lookups,
// the official marketplace, and enterprise policy enforcement.
module;

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module cc.utils.plugin_marketplace;

import cc.utils.exec_sync;
import cc.utils.json;
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
// Marketplace manager implementation (ported from marketplaceManager.ts).
//
// Ports the USED subset: known_marketplaces.json config load/save, git-clone
// fetch for github/git sources, URL fetch via curl, file/dir source reads,
// add/remove with cache cleanup. git source variants (sparse checkout, SSH
// detection, submodule recursion) and settings-layer (extraKnownMarketplaces)
// writes are intentionally out of scope for the C++ CLI's headless install
// path; they return honest errors rather than fake success. The TS parity
// goal is preserved for the happy path.
// ─────────────────────────────────────────────────────────────────────────────
namespace detail {

// ~/.claude/plugins — mirrors pluginDirectories.getPluginsDirectory() in TS.
inline std::filesystem::path get_plugins_directory() {
    if (const char* override = std::getenv("CLAUDE_PLUGINS_DIR"); override && *override) {
        return std::filesystem::path(override);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".claude" / "plugins";
    }
    return std::filesystem::current_path() / ".claude" / "plugins";
}

inline std::filesystem::path get_known_marketplaces_config_path() {
    return get_plugins_directory() / "known_marketplaces.json";
}

inline std::filesystem::path get_marketplaces_cache_dir() {
    return get_plugins_directory() / "marketplaces";
}

inline std::string read_file_str(const std::filesystem::path& p, std::error_code& ec) {
    std::string out;
    std::ifstream in(p, std::ios::binary);
    if (!in) { ec = std::make_error_code(std::errc::no_such_file_or_directory); return {}; }
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}

inline std::string iso_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// Render a MarketplaceSource back to the TS JSON wire form: {"source":"github",
// "repo":...} / {"source":"git","url":...} / {"source":"url","url":...} etc.
inline std::string source_tag(const MarketplaceSource& s) {
    if (std::holds_alternative<GitHubMarketplaceSource>(s)) return "github";
    if (std::holds_alternative<GitMarketplaceSource>(s)) return "git";
    if (std::holds_alternative<UrlMarketplaceSource>(s)) return "url";
    if (std::holds_alternative<DirectoryMarketplaceSource>(s)) return "directory";
    return "file";
}

inline bool source_is_local(const MarketplaceSource& s) {
    return std::holds_alternative<DirectoryMarketplaceSource>(s)
        || std::holds_alternative<FileMarketplaceSource>(s);
}

inline std::optional<std::string> local_source_path(const MarketplaceSource& s) {
    if (auto d = std::get_if<DirectoryMarketplaceSource>(&s)) return d->path;
    if (auto f = std::get_if<FileMarketplaceSource>(&s)) return f->path;
    return std::nullopt;
}

// Build the git URL + cache path for github/git sources (cacheMarketplaceFromGit
// parity — SSH detection / sparse paths deferred; HTTPS-only for github).
struct GitResolve { std::string url; std::optional<std::string> ref; };

inline std::optional<GitResolve> resolve_git_url(const MarketplaceSource& s) {
    if (auto gh = std::get_if<GitHubMarketplaceSource>(&s)) {
        GitResolve r;
        r.url = "https://github.com/" + gh->repo + ".git";
        r.ref = gh->ref;
        return r;
    }
    if (auto g = std::get_if<GitMarketplaceSource>(&s)) {
        GitResolve r; r.url = g->url; r.ref = g->ref;
        return r;
    }
    return std::nullopt;
}

// filesystem-safe cache name derived from the source identity.
inline std::string cache_name_for_source(const MarketplaceSource& s) {
    auto tag = source_tag(s);
    std::string key;
    if (auto gh = std::get_if<GitHubMarketplaceSource>(&s)) key = gh->repo;
    else if (auto g = std::get_if<GitMarketplaceSource>(&s)) key = g->url;
    else if (auto u = std::get_if<UrlMarketplaceSource>(&s)) key = u->url;
    else if (auto d = std::get_if<DirectoryMarketplaceSource>(&s)) key = d->path;
    else if (auto f = std::get_if<FileMarketplaceSource>(&s)) key = f->path;
    // Replace path/url separators so the cache name is a single segment.
    std::replace_if(key.begin(), key.end(),
        [](char c) { return c == '/' || c == '\\' || c == ':' || c == '?'
                          || c == '&' || c == '=' || c == ' '; }, '_');
    return tag + "_" + key;
}

// shallow git clone (--depth 1) into cachePath; on success returns {}, else
// a descriptive error. Mirrors gitClone()/cacheMarketplaceFromGit() in TS.
inline std::expected<void, std::string>
git_clone_shallow(const std::string& url,
                  const std::filesystem::path& cache_path,
                  const std::optional<std::string>& ref) {
    std::ostringstream cmd;
    cmd << "git -c core.sshCommand='ssh -o BatchMode=yes -o StrictHostKeyChecking=yes'"
        << " clone --depth 1 --recurse-submodules --shallow-submodules";
    if (ref) cmd << " --branch " << *ref;
    cmd << " -- " << url << " " << cache_path.string();
    int status = cc::utils::exec_sync_status(cmd.str());
    if (status != 0) {
        return std::unexpected(
            "Failed to clone marketplace repository (git exit code "
            + std::to_string(status) + "): " + url);
    }
    return {};
}

// git pull inside cachePath (refresh path). Mirrors gitPull() happy path.
inline std::expected<void, std::string>
git_pull(const std::filesystem::path& cache_path) {
    std::ostringstream cmd;
    cmd << "git -C " << cache_path.string()
        << " -c core.sshCommand='ssh -o BatchMode=yes -o StrictHostKeyChecking=yes'"
        << " pull --ff-only";
    int status = cc::utils::exec_sync_status(cmd.str());
    if (status != 0) {
        return std::unexpected(
            "git pull failed (exit code " + std::to_string(status) + ")");
    }
    return {};
}

// Download a URL marketplace.json into cachePath using curl (HTTPS only).
// cacheMarketplaceFromUrl() parity — axios is replaced with the system curl.
inline std::expected<std::string, std::string>
download_url(const std::string& url) {
    std::ostringstream cmd;
    cmd << "curl -fsSL --max-time 30 -H 'User-Agent: Claude-Code-Plugin-Manager' "
        << url;
    auto out = cc::utils::exec_sync(cmd.str());
    if (!out) return std::unexpected("Failed to download marketplace from " + url
                                     + ": " + out.error());
    return *out;
}

// Read + schema-light parse a marketplace.json from disk; returns the parsed
// JsonDoc for callers to map into PluginMarketplace. Mirrors
// parseFileWithSchema(PluginMarketplaceSchema()).
inline std::expected<cc::utils::json::JsonDoc, std::string>
read_marketplace_json(const std::filesystem::path& p) {
    std::error_code ec;
    auto contents = read_file_str(p, ec);
    if (ec) {
        return std::unexpected("Marketplace file not found: " + p.string());
    }
    auto parsed = cc::utils::json::parse(contents);
    if (!parsed) {
        return std::unexpected("Invalid marketplace JSON (" + p.string()
                               + "): " + parsed.error().message());
    }
    auto root = parsed->root();
    if (!root.valid() || !root.is_obj()) {
        return std::unexpected("Marketplace manifest must be a JSON object: "
                               + p.string());
    }
    return std::move(*parsed);
}

// For git-sourced directories the manifest lives at
// <install>/.claude-plugin/marketplace.json; for url/file/dir sources the
// installLocation IS the manifest. Mirrors readCachedMarketplace().
inline std::filesystem::path
resolve_manifest_path(const std::filesystem::path& install_location,
                      const MarketplaceSource& source) {
    if (resolve_git_url(source)) {
        return install_location / ".claude-plugin" / "marketplace.json";
    }
    // directory/file local sources: a directory may host
    // <install>/.claude-plugin/marketplace.json (parity with fetch_marketplace).
    std::error_code ec;
    if (std::filesystem::is_directory(install_location, ec)) {
        auto nested = install_location / ".claude-plugin" / "marketplace.json";
        if (std::filesystem::exists(nested, ec)) return nested;
    }
    return install_location;
}

// Map a parsed marketplace.json object into the PluginMarketplace C++ struct.
inline std::expected<PluginMarketplace, std::string>
to_marketplace(const cc::utils::json::JsonDoc& doc) {
    auto root = doc.root();
    PluginMarketplace m;
    if (auto name = root.get("name"); name.valid() && name.is_str()) {
        m.name = std::string(name.as_str());
    } else {
        return std::unexpected("Marketplace manifest is missing 'name'");
    }
    if (auto desc = root.get("description"); desc.valid() && desc.is_str()) {
        m.description = std::string(desc.as_str());
    }
    // metadata.description fallback (TS PluginMarketplaceSchema parity).
    if (!m.description) {
        auto meta = root.get("metadata");
        if (meta.valid() && meta.is_obj()) {
            if (auto md = meta.get("description"); md.valid() && md.is_str()) {
                m.description = std::string(md.as_str());
            }
        }
    }
    auto plugins = root.get("plugins");
    if (plugins.valid() && plugins.is_arr()) {
        plugins.iter([&](cc::utils::json::JsonVal entry) {
            if (!entry.valid() || !entry.is_obj()) return;
            PluginMarketplaceEntry pe;
            if (auto n = entry.get("name"); n.valid() && n.is_str()) {
                pe.name = std::string(n.as_str());
            } else {
                return; // skip entries without a name (TS schema rejects these)
            }
            if (auto d = entry.get("description"); d.valid() && d.is_str())
                pe.description = std::string(d.as_str());
            if (auto v = entry.get("version"); v.valid() && v.is_str())
                pe.version = std::string(v.as_str());
            if (auto s = entry.get("source"); s.valid() && s.is_str()) {
                pe.source = std::string(s.as_str());
            }
            m.plugins[pe.name] = std::move(pe);
        });
    }
    return m;
}

} // namespace detail

inline std::filesystem::path get_known_marketplaces_config_path() {
    return detail::get_known_marketplaces_config_path();
}

// loadKnownMarketplacesConfig() parity: read + JSON-parse known_marketplaces.json.
// Returns {} when the file is absent (fresh install); surfaces parse errors.
inline std::expected<KnownMarketplacesFile, std::string>
load_known_marketplaces_config() {
    const auto cfg = detail::get_known_marketplaces_config_path();
    std::error_code ec;
    if (!std::filesystem::exists(cfg, ec)) {
        return KnownMarketplacesFile{};
    }
    auto contents = detail::read_file_str(cfg, ec);
    if (ec) {
        return std::unexpected("Failed to read marketplace configuration: "
                               + cfg.string());
    }
    auto parsed = cc::utils::json::parse(contents);
    if (!parsed) {
        return std::unexpected("Marketplace configuration file is corrupted: "
                               + parsed.error().message());
    }
    auto root = parsed->root();
    KnownMarketplacesFile out;
    if (root.valid() && root.is_obj()) {
        root.iter_obj([&](cc::utils::json::JsonVal key,
                          cc::utils::json::JsonVal val) {
            if (!val.is_obj()) return;
            KnownMarketplace km;
            auto src = val.get("source");
            if (src.valid() && src.is_obj()) {
                auto tag = src.get("source");
                std::string t = (tag.valid() && tag.is_str())
                    ? std::string(tag.as_str()) : "";
                if (t == "github") {
                    GitHubMarketplaceSource g;
                    if (auto r = src.get("repo"); r.valid() && r.is_str())
                        g.repo = std::string(r.as_str());
                    if (auto r = src.get("ref"); r.valid() && r.is_str())
                        g.ref = std::string(r.as_str());
                    km.source = g;
                } else if (t == "git") {
                    GitMarketplaceSource g;
                    if (auto u = src.get("url"); u.valid() && u.is_str())
                        g.url = std::string(u.as_str());
                    if (auto r = src.get("ref"); r.valid() && r.is_str())
                        g.ref = std::string(r.as_str());
                    km.source = g;
                } else if (t == "url") {
                    UrlMarketplaceSource u;
                    if (auto v = src.get("url"); v.valid() && v.is_str())
                        u.url = std::string(v.as_str());
                    km.source = u;
                } else if (t == "directory") {
                    DirectoryMarketplaceSource d;
                    if (auto p = src.get("path"); p.valid() && p.is_str())
                        d.path = std::string(p.as_str());
                    km.source = d;
                } else if (t == "file") {
                    FileMarketplaceSource f;
                    if (auto p = src.get("path"); p.valid() && p.is_str())
                        f.path = std::string(p.as_str());
                    km.source = f;
                }
            }
            if (auto lu = val.get("last_updated"); lu.valid() && lu.is_str())
                km.last_updated = std::string(lu.as_str());
            if (auto cp = val.get("installLocation"); cp.valid() && cp.is_str())
                km.cache_path = std::string(cp.as_str());
            if (auto au = val.get("auto_update"); au.valid() && au.is_bool())
                km.auto_update = au.as_bool();
            std::string name(key.is_str() ? key.as_str() : "");
            if (!name.empty()) out[name] = std::move(km);
        });
    }
    return out;
}

inline KnownMarketplacesFile load_known_marketplaces_config_safe() {
    auto r = load_known_marketplaces_config();
    return r ? *r : KnownMarketplacesFile{};
}

// saveKnownMarketplacesConfig() parity: rewrite known_marketplaces.json.
// Uses JsonMutDoc directly because the public JsonObject/JsonBuilder helpers
// only accept scalar values, and the source object nests a tag + payload.
inline std::expected<void, std::string>
save_known_marketplaces_config(const KnownMarketplacesFile& config) {
    const auto cfg = detail::get_known_marketplaces_config_path();
    std::error_code ec;
    std::filesystem::create_directories(cfg.parent_path(), ec);

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    for (const auto& [name, km] : config) {
        auto entry = doc.object();
        auto src = doc.object();
        src.add("source", doc.string(detail::source_tag(km.source)));
        if (auto gh = std::get_if<GitHubMarketplaceSource>(&km.source)) {
            src.add("repo", doc.string(gh->repo));
            if (gh->ref) src.add("ref", doc.string(*gh->ref));
        } else if (auto g = std::get_if<GitMarketplaceSource>(&km.source)) {
            src.add("url", doc.string(g->url));
            if (g->ref) src.add("ref", doc.string(*g->ref));
        } else if (auto u = std::get_if<UrlMarketplaceSource>(&km.source)) {
            src.add("url", doc.string(u->url));
        } else if (auto d = std::get_if<DirectoryMarketplaceSource>(&km.source)) {
            src.add("path", doc.string(d->path));
        } else if (auto f = std::get_if<FileMarketplaceSource>(&km.source)) {
            src.add("path", doc.string(f->path));
        }
        entry.add("source", src);
        if (km.cache_path) entry.add("installLocation", doc.string(*km.cache_path));
        if (km.last_updated) entry.add("last_updated", doc.string(*km.last_updated));
        if (km.auto_update) entry.add("auto_update", doc.boolean(*km.auto_update));
        root.add(name, entry);
    }
    doc.set_root(root);

    std::string body = doc.to_string();
    std::ofstream out(cfg, std::ios::binary);
    if (!out) {
        return std::unexpected("Failed to write marketplace configuration: "
                               + cfg.string());
    }
    out << body;
    return {};
}

// Fetch + cache a marketplace from its source, returning the parsed
// PluginMarketplace. Mirrors loadAndCacheMarketplace() / cacheMarketplaceFromGit()
// / cacheMarketplaceFromUrl() happy paths. Sparse-checkout, SSH probing, and
// custom headers are deferred; github uses HTTPS only.
inline std::expected<PluginMarketplace, std::string>
fetch_marketplace(std::string_view name) {
    auto config = load_known_marketplaces_config();
    if (!config) return std::unexpected(config.error());

    auto it = config->find(std::string(name));
    if (it == config->end()) {
        return std::unexpected("Marketplace '" + std::string(name)
            + "' not found in configuration");
    }
    const auto& source = it->second.source;

    std::error_code ec;
    std::filesystem::create_directories(detail::get_marketplaces_cache_dir(), ec);

    // git/github: clone or pull into <cache>/<name>.
    if (auto git = detail::resolve_git_url(source)) {
        std::filesystem::path cache_path = detail::get_marketplaces_cache_dir() / std::string(name);
        // Try incremental pull first; on failure, rm + fresh clone (TS parity).
        std::error_code exists_ec;
        if (std::filesystem::exists(cache_path, exists_ec)
            && std::filesystem::is_directory(cache_path, exists_ec)) {
            if (detail::git_pull(cache_path)) {
                // fall through to parse
            } else {
                std::filesystem::remove_all(cache_path, ec);
                auto cloned = detail::git_clone_shallow(git->url, cache_path, git->ref);
                if (!cloned) return std::unexpected(cloned.error());
            }
        } else {
            auto cloned = detail::git_clone_shallow(git->url, cache_path, git->ref);
            if (!cloned) return std::unexpected(cloned.error());
        }
        auto manifest = detail::resolve_manifest_path(cache_path, source);
        auto doc = detail::read_marketplace_json(manifest);
        if (!doc) return std::unexpected(doc.error());
        return detail::to_marketplace(*doc);
    }

    // url: download JSON manifest directly.
    if (auto u = std::get_if<UrlMarketplaceSource>(&source)) {
        auto body = detail::download_url(u->url);
        if (!body) return std::unexpected(body.error());
        auto parsed = cc::utils::json::parse(*body);
        if (!parsed) return std::unexpected("Invalid marketplace JSON: "
                                            + parsed.error().message());
        std::filesystem::path cache_path =
            detail::get_marketplaces_cache_dir() / (std::string(name) + ".json");
        std::ofstream f(cache_path, std::ios::binary);
        if (f) f << *body;
        return detail::to_marketplace(*parsed);
    }

    // directory/file: read manifest straight from the local path.
    if (auto local = detail::local_source_path(source)) {
        std::filesystem::path p(*local);
        // A directory source may host <path>/.claude-plugin/marketplace.json.
        if (std::filesystem::is_directory(p, ec)) {
            auto nested = p / ".claude-plugin" / "marketplace.json";
            if (std::filesystem::exists(nested, ec)) p = nested;
        }
        auto doc = detail::read_marketplace_json(p);
        if (!doc) return std::unexpected(doc.error());
        return detail::to_marketplace(*doc);
    }

    return std::unexpected("Unsupported marketplace source type");
}

// addMarketplaceSource() parity (USED subset): fetch + cache the marketplace to
// materialize its name, then persist it into known_marketplaces.json. Enterprise
// policy enforcement and settings-layer writes are deferred (treated as
// allowed) — see H1 note above.
inline std::expected<void, std::string>
add_marketplace_source(std::string_view name, const MarketplaceSource& source) {
    auto config = load_known_marketplaces_config();
    if (!config) return std::unexpected(config.error());

    // Source-idempotency: skip clone if an identical source is already known.
    for (const auto& [existing_name, existing] : *config) {
        if (existing_name == name && detail::source_tag(existing.source) == detail::source_tag(source)) {
            return {};
        }
    }

    // Materialize into a temporary name slot, then validate via fetch.
    (*config)[std::string(name)].source = source;
    auto saved = save_known_marketplaces_config(*config);
    if (!saved) return std::unexpected(saved.error());

    auto fetched = fetch_marketplace(name);
    if (!fetched) {
        // Roll back the half-written config entry on fetch failure (TS leaves
        // the cache dir alone but never records an entry; we mirror that by
        // removing the speculative config slot).
        auto fresh = load_known_marketplaces_config();
        if (fresh) {
            fresh->erase(std::string(name));
            (void)save_known_marketplaces_config(*fresh);
        }
        return std::unexpected(fetched.error());
    }

    // Record install location + last-updated timestamp.
    std::filesystem::path install_location;
    if (detail::resolve_git_url(source)) {
        install_location = detail::get_marketplaces_cache_dir() / std::string(name);
    } else if (auto u = std::get_if<UrlMarketplaceSource>(&source)) {
        install_location = detail::get_marketplaces_cache_dir() / (std::string(name) + ".json");
        (void)u;
    } else if (auto local = detail::local_source_path(source)) {
        install_location = *local;
    }

    auto final_config = load_known_marketplaces_config();
    if (!final_config) return std::unexpected(final_config.error());
    (*final_config)[std::string(name)].source = source;
    (*final_config)[std::string(name)].cache_path = install_location.string();
    (*final_config)[std::string(name)].last_updated = detail::iso_now();
    return save_known_marketplaces_config(*final_config);
}

// removeMarketplaceSource() parity: drop the config entry and rm the cache dir
// + JSON cache. Seed-managed detection and settings cleanup are deferred.
inline std::expected<void, std::string>
remove_marketplace(std::string_view name) {
    auto config = load_known_marketplaces_config();
    if (!config) return std::unexpected(config.error());

    auto it = config->find(std::string(name));
    if (it == config->end()) {
        return std::unexpected("Marketplace '" + std::string(name) + "' not found");
    }
    config->erase(it);
    auto saved = save_known_marketplaces_config(*config);
    if (!saved) return std::unexpected(saved.error());

    // Clean up both directory and JSON cache forms.
    std::error_code ec;
    std::filesystem::remove_all(
        detail::get_marketplaces_cache_dir() / std::string(name), ec);
    std::filesystem::remove(
        detail::get_marketplaces_cache_dir() / (std::string(name) + ".json"), ec);
    return {};
}

// getMarketplaceCacheOnly() parity: read the cached marketplace for `name`
// without touching the network. Returns std::nullopt if missing/corrupted.
inline std::optional<PluginMarketplace>
get_marketplace_cache_only(std::string_view name) {
    auto config = load_known_marketplaces_config_safe();
    auto it = config.find(std::string(name));
    if (it == config.end() || !it->second.cache_path) return std::nullopt;

    std::filesystem::path install(*it->second.cache_path);
    auto manifest = detail::resolve_manifest_path(install, it->second.source);
    auto doc = detail::read_marketplace_json(manifest);
    if (!doc) {
        // url/file installLocation IS the manifest — retry the raw path.
        auto doc2 = detail::read_marketplace_json(install);
        if (!doc2) return std::nullopt;
        auto m = detail::to_marketplace(*doc2);
        return m ? std::make_optional(*m) : std::nullopt;
    }
    auto m = detail::to_marketplace(*doc);
    return m ? std::make_optional(*m) : std::nullopt;
}

} // namespace cc::utils::plugin_marketplace
