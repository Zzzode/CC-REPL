/// @file plugin_helpers.cppm
/// @brief Shared pure helpers for the plugin command system.
///
/// Consolidates three TS modules:
///   - PluginErrors.tsx          (formatErrorMessage, getErrorGuidance)
///   - pluginDetailsHelpers.tsx  (InstallablePlugin, extractGitHubRepo,
///                                buildPluginDetailsMenuOptions)
///   - usePagination.ts         (PaginationState helper class — no React hooks)
///
/// All functions are pure: no filesystem, no global state.

module;

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.commands.plugin_helpers;

export namespace cc::commands::plugin_helpers {

// ═════════════════════════════════════════════════════════════════════════════
// 1.  Plugin Error Formatting  (from PluginErrors.tsx)
// ═════════════════════════════════════════════════════════════════════════════

/// Mirrors TS `PluginError.type` discriminant — kept in the commands layer
/// so we can format errors without importing the full plugin type system.
/// The actual error struct lives in `cc.utils.plugin_validation`; this enum
/// is the wire-compatible subset we need for display.
enum class ErrorKind : unsigned char {
    PathNotFound,
    GitAuthFailed,
    GitTimeout,
    NetworkError,
    ManifestParseError,
    ManifestValidationError,
    PluginNotFound,
    MarketplaceNotFound,
    MarketplaceLoadFailed,
    McpConfigInvalid,
    McpServerSuppressedDuplicate,
    HookLoadFailed,
    ComponentLoadFailed,
    McpbDownloadFailed,
    McpbExtractFailed,
    McpbInvalidManifest,
    MarketplaceBlockedByPolicy,
    DependencyUnsatisfied,
    LspConfigInvalid,
    LspServerStartFailed,
    LspServerCrashed,
    LspRequestTimeout,
    LspRequestFailed,
    PluginCacheMiss,
    GenericError,
};

/// A display-ready plugin error.  Field presence mirrors the TS union so the
/// switch-ladder below maps 1:1 to the original.
struct DisplayPluginError {
    ErrorKind type = ErrorKind::GenericError;

    // ── discriminant-specific payloads (only one set is meaningful) ───────
    // PathNotFound / ComponentLoadFailed / HookLoadFailed
    std::string component;
    std::string path;

    // GitAuthFailed / GitTimeout
    std::string auth_type; // "ssh" | "https" | ...
    std::string operation; // "clone" | "fetch" | ...
    std::string git_url;

    // NetworkError / McpbDownloadFailed
    std::string url;
    std::string details;

    // Manifest*
    std::string manifest_path;
    std::string parse_error;
    std::vector<std::string> validation_errors;

    // PluginNotFound / PluginCacheMiss / DependencyUnsatisfied
    std::string plugin_id;
    std::string plugin;          // alternate plugin-id field
    std::string dependency;      // for DependencyUnsatisfied
    std::string install_path;    // for PluginCacheMiss
    std::string reason;          // "not-enabled" | "not-installed" | ...

    // Marketplace*
    std::string marketplace;
    std::vector<std::string> available_marketplaces; // MarketplaceNotFound
    std::vector<std::string> allowed_sources;        // BlockedByPolicy
    bool blocked_by_blocklist = false;               // BlockedByPolicy

    // Mcp*
    std::string server_name;
    std::string validation_error;
    std::string duplicate_of; // McpServerSuppressedDuplicate

    // HookLoadFailed
    std::string hook_path;

    // Mcpb*
    std::string mcpb_path;

    // Lsp*
    std::optional<int> signal;
    std::optional<int> exit_code;
    std::string method;
    std::optional<int> timeout_ms;
    std::string lsp_error;

    // Generic
    std::string error;              // raw error text
    std::string source;             // fallback label for display
};

/// Produce a one-line human-readable description of a plugin error.
/// Exact translation of `formatErrorMessage` (TS).
[[nodiscard]] inline std::string format_error_message(const DisplayPluginError& e) {
    using K = ErrorKind;
    switch (e.type) {
        case K::PathNotFound:
            return e.component + " path not found: " + e.path;
        case K::GitAuthFailed: {
            std::string up = e.auth_type;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return "Git " + up + " authentication failed for " + e.git_url;
        }
        case K::GitTimeout:
            return "Git " + e.operation + " timed out for " + e.git_url;
        case K::NetworkError:
            return "Network error accessing " + e.url +
                   (e.details.empty() ? std::string{} : ": " + e.details);
        case K::ManifestParseError:
            return "Failed to parse manifest at " + e.manifest_path + ": " + e.parse_error;
        case K::ManifestValidationError: {
            std::string joined;
            for (std::size_t i = 0; i < e.validation_errors.size(); ++i) {
                if (i) joined += ", ";
                joined += e.validation_errors[i];
            }
            return "Invalid manifest at " + e.manifest_path + ": " + joined;
        }
        case K::PluginNotFound:
            return "Plugin \"" + e.plugin_id + "\" not found in marketplace \"" + e.marketplace + "\"";
        case K::MarketplaceNotFound:
            return "Marketplace \"" + e.marketplace + "\" not found";
        case K::MarketplaceLoadFailed:
            return "Failed to load marketplace \"" + e.marketplace + "\": " + e.reason;
        case K::McpConfigInvalid:
            return "Invalid MCP server config for \"" + e.server_name + "\": " + e.validation_error;
        case K::McpServerSuppressedDuplicate: {
            std::string dup;
            if (e.duplicate_of.starts_with("plugin:")) {
                auto tail = e.duplicate_of.substr(std::string{"plugin:"}.size());
                auto col  = tail.find(':');
                auto name = (col == std::string::npos) ? tail : tail.substr(0, col);
                dup = "server provided by plugin \"" + (name.empty() ? "?" : name) + "\"";
            } else {
                dup = "already-configured \"" + e.duplicate_of + "\"";
            }
            return "MCP server \"" + e.server_name + "\" skipped \xe2\x80\x94 same command/URL as " + dup;
        }
        case K::HookLoadFailed:
            return "Failed to load hooks from " + e.hook_path + ": " + e.reason;
        case K::ComponentLoadFailed:
            return "Failed to load " + e.component + " from " + e.path + ": " + e.reason;
        case K::McpbDownloadFailed:
            return "Failed to download MCPB from " + e.url + ": " + e.reason;
        case K::McpbExtractFailed:
            return "Failed to extract MCPB " + e.mcpb_path + ": " + e.reason;
        case K::McpbInvalidManifest:
            return "MCPB manifest invalid at " + e.mcpb_path + ": " + e.validation_error;
        case K::MarketplaceBlockedByPolicy:
            return e.blocked_by_blocklist
                ? ("Marketplace \"" + e.marketplace + "\" is blocked by enterprise policy")
                : ("Marketplace \"" + e.marketplace + "\" is not in the allowed marketplace list");
        case K::DependencyUnsatisfied:
            return e.reason == "not-enabled"
                ? ("Dependency \"" + e.dependency + "\" is disabled")
                : ("Dependency \"" + e.dependency + "\" is not installed");
        case K::LspConfigInvalid:
            return "Invalid LSP server config for \"" + e.server_name + "\": " + e.validation_error;
        case K::LspServerStartFailed:
            return "LSP server \"" + e.server_name + "\" failed to start: " + e.reason;
        case K::LspServerCrashed:
            return e.signal
                ? ("LSP server \"" + e.server_name + "\" crashed with signal " + std::to_string(*e.signal))
                : ("LSP server \"" + e.server_name + "\" crashed with exit code " +
                   (e.exit_code ? std::to_string(*e.exit_code) : std::string{"unknown"}));
        case K::LspRequestTimeout:
            return "LSP server \"" + e.server_name + "\" timed out on " + e.method +
                   " after " + (e.timeout_ms ? std::to_string(*e.timeout_ms) : std::string{"?"}) + "ms";
        case K::LspRequestFailed:
            return "LSP server \"" + e.server_name + "\" " + e.method + " failed: " + e.lsp_error;
        case K::PluginCacheMiss:
            return "Plugin \"" + e.plugin + "\" not cached at " + e.install_path;
        case K::GenericError:
            return e.error;
    }
    return "(unknown plugin error)"; // unreachable
}

/// Optional follow-up hint string.  Returns nullopt when no guidance applies.
/// Exact translation of `getErrorGuidance` (TS).
[[nodiscard]] inline std::optional<std::string> get_error_guidance(const DisplayPluginError& e) {
    using K = ErrorKind;
    switch (e.type) {
        case K::PathNotFound:
            return "Check that the path in your manifest or marketplace config is correct";
        case K::GitAuthFailed:
            return e.auth_type == "ssh"
                ? std::string{"Configure SSH keys or use HTTPS URL instead"}
                : std::string{"Configure credentials or use SSH URL instead"};
        case K::GitTimeout:
        case K::NetworkError:
            return "Check your internet connection and try again";
        case K::ManifestParseError:
            return "Check manifest file syntax in the plugin directory";
        case K::ManifestValidationError:
            return "Check manifest file follows the required schema";
        case K::PluginNotFound:
            return "Plugin may not exist in marketplace \"" + e.marketplace + "\"";
        case K::MarketplaceNotFound:
            if (!e.available_marketplaces.empty()) {
                std::string joined;
                for (std::size_t i = 0; i < e.available_marketplaces.size(); ++i) {
                    if (i) joined += ", ";
                    joined += e.available_marketplaces[i];
                }
                return "Available marketplaces: " + joined;
            }
            return "Add the marketplace first using /plugin marketplace add";
        case K::McpConfigInvalid:
            return "Check MCP server configuration in .mcp.json or manifest";
        case K::McpServerSuppressedDuplicate: {
            if (e.duplicate_of.starts_with("plugin:")) {
                auto tail = e.duplicate_of.substr(std::string{"plugin:"}.size());
                auto col  = tail.find(':');
                auto name = (col == std::string::npos) ? tail : tail.substr(0, col);
                if (name.empty()) name = "the other plugin";
                return "Disable plugin \"" + name + "\" if you want this plugin's version instead";
            }
            return "Remove \"" + e.duplicate_of +
                   "\" from your MCP config if you want the plugin's version instead";
        }
        case K::HookLoadFailed:
            return "Check hooks.json file syntax and structure";
        case K::ComponentLoadFailed:
            return "Check " + e.component + " directory structure and file permissions";
        case K::McpbDownloadFailed:
            return "Check your internet connection and URL accessibility";
        case K::McpbExtractFailed:
            return "Verify the MCPB file is valid and not corrupted";
        case K::McpbInvalidManifest:
            return "Contact the plugin author about the invalid manifest";
        case K::MarketplaceBlockedByPolicy:
            if (e.blocked_by_blocklist)
                return "This marketplace source is explicitly blocked by your administrator";
            if (!e.allowed_sources.empty()) {
                std::string joined;
                for (std::size_t i = 0; i < e.allowed_sources.size(); ++i) {
                    if (i) joined += ", ";
                    joined += e.allowed_sources[i];
                }
                return "Allowed sources: " + joined;
            }
            return "Contact your administrator to configure allowed marketplace sources";
        case K::DependencyUnsatisfied:
            return e.reason == "not-enabled"
                ? ("Enable \"" + e.dependency + "\" or uninstall \"" + e.plugin + "\"")
                : ("Install \"" + e.dependency + "\" or uninstall \"" + e.plugin + "\"");
        case K::LspConfigInvalid:
            return "Check LSP server configuration in the plugin manifest";
        case K::LspServerStartFailed:
        case K::LspServerCrashed:
        case K::LspRequestTimeout:
        case K::LspRequestFailed:
            return "Check LSP server logs with --debug for details";
        case K::PluginCacheMiss:
            return "Run /plugins to refresh the plugin cache";
        case K::MarketplaceLoadFailed:
        case K::GenericError:
            return std::nullopt;
    }
    return std::nullopt; // unreachable
}

// ═════════════════════════════════════════════════════════════════════════════
// 2.  Plugin Details Helpers  (from pluginDetailsHelpers.tsx)
// ═════════════════════════════════════════════════════════════════════════════

/// A plugin available for installation from some marketplace.
struct InstallablePlugin {
    // Mirrors PluginMarketplaceEntry — kept local to avoid import cycles.
    // Only fields required by command-layer data prep are included.
    struct Entry {
        std::string name;
        std::optional<std::string> description;
        std::optional<std::string> homepage;
        // source: "github" object — repo string, or local path string
        std::optional<std::string> github_repo;      // set iff source is github
        std::optional<std::string> local_source_path; // set iff source is local
    };
    Entry entry;
    std::string marketplace_name;
    std::string plugin_id;
    bool is_installed = false;
};

/// Menu option presented in the "plugin details" view.
struct DetailsMenuOption {
    std::string label;
    std::string action;  // e.g. "install-user", "homepage", "github", "back"
};

/// Extract the GitHub "owner/repo" string from an InstallablePlugin when the
/// source is a GitHub object; nullopt otherwise.
[[nodiscard]] inline std::optional<std::string> extract_github_repo(const InstallablePlugin& p) {
    return p.entry.github_repo;
}

/// Build the full details menu: three install scopes are always present,
/// followed by optional homepage / github rows, and a final "Back to list".
[[nodiscard]] inline std::vector<DetailsMenuOption> build_details_menu(
    std::optional<std::string_view> homepage,
    std::optional<std::string_view> github_repo
) {
    std::vector<DetailsMenuOption> opts{
        {"Install for you (user scope)",          "install-user"},
        {"Install for all collaborators on this repository (project scope)", "install-project"},
        {"Install for you, in this repo only (local scope)",                 "install-local"},
    };
    if (homepage && !homepage->empty()) {
        opts.push_back({"Open homepage", "homepage"});
    }
    if (github_repo && !github_repo->empty()) {
        opts.push_back({"View on GitHub", "github"});
    }
    opts.push_back({"Back to plugin list", "back"});
    return opts;
}

// ═════════════════════════════════════════════════════════════════════════════
// 3.  Pagination  (from usePagination.ts)
// ═════════════════════════════════════════════════════════════════════════════

/// Hook-free pagination state.  The original `usePagination` is a React hook;
/// this is its imperative state machine.  Call `set_selected(new_index)`
/// whenever the user moves the cursor, then query `visible_range()` /
/// `scroll_position()` for layout.
class Pagination {
public:
    static constexpr std::size_t k_default_max_visible = 5;

    explicit Pagination(std::size_t total_items,
                        std::size_t max_visible = k_default_max_visible,
                        std::size_t selected_index = 0)
        : total_(total_items), max_(max_visible ? max_visible : k_default_max_visible),
          selected_(std::min(selected_index, total_ ? total_ - 1 : 0)) {}

    /// Change selected index, clamping to [0, total-1] and re-computing the
    /// scroll window so the selection is always visible.
    void set_selected(std::size_t new_index) {
        if (total_ == 0) { selected_ = 0; return; }
        if (new_index >= total_) new_index = total_ - 1;
        selected_ = new_index;

        // Recompute scroll offset so selection stays in view.
        if (!needs_pagination()) { offset_ = 0; return; }

        if (selected_ < offset_) {
            offset_ = selected_;                 // scroll up
        } else if (selected_ >= offset_ + max_) {
            offset_ = selected_ - max_ + 1;      // scroll down
        }
        // clamp offset to legal maximum
        const std::size_t max_offset = total_ - max_;
        if (offset_ > max_offset) offset_ = max_offset;
    }

    void set_total(std::size_t total_items) {
        total_ = total_items;
        if (selected_ >= total_ && total_ > 0) selected_ = total_ - 1;
        if (!needs_pagination()) offset_ = 0;
        else {
            const std::size_t max_offset = total_ - max_;
            if (offset_ > max_offset) offset_ = max_offset;
        }
    }

    [[nodiscard]] std::size_t total_items()   const { return total_; }
    [[nodiscard]] std::size_t max_visible()   const { return max_; }
    [[nodiscard]] std::size_t selected()      const { return selected_; }
    [[nodiscard]] bool        needs_pagination() const { return total_ > max_; }

    // Visible window
    [[nodiscard]] std::size_t start_index()   const { return offset_; }
    [[nodiscard]] std::size_t end_index()     const {
        return std::min(offset_ + max_, total_);
    }
    [[nodiscard]] std::size_t page_size()     const { return max_; }

    // Page compatibility values (backwards-compatible with the TS API)
    [[nodiscard]] std::size_t total_pages()   const {
        return std::max<std::size_t>(1, (total_ + max_ - 1) / max_);
    }
    [[nodiscard]] std::size_t current_page()  const {
        return total_ == 0 ? 0 : offset_ / max_;
    }

    /// Slice a vector to the currently-visible window.
    template <typename T>
    [[nodiscard]] std::vector<T> visible_slice(const std::vector<T>& items) const {
        if (!needs_pagination()) return items;
        const auto a = start_index();
        const auto b = end_index();
        std::vector<T> out;
        out.reserve(b - a);
        for (auto i = a; i < b && i < items.size(); ++i) out.push_back(items[i]);
        return out;
    }

    /// Convert a visible-list index to the real index in the full list.
    [[nodiscard]] std::size_t to_actual_index(std::size_t visible_index) const {
        return offset_ + visible_index;
    }
    [[nodiscard]] bool is_on_current_page(std::size_t actual_index) const {
        return actual_index >= offset_ && actual_index < end_index();
    }

    // Scroll information for UI display
    struct ScrollPosition {
        std::size_t current;        // 1-based selection
        std::size_t total;          // total items
        bool        can_scroll_up;
        bool        can_scroll_down;
    };
    [[nodiscard]] ScrollPosition scroll_position() const {
        return ScrollPosition{
            .current         = total_ == 0 ? 0 : selected_ + 1,
            .total           = total_,
            .can_scroll_up   = offset_ > 0,
            .can_scroll_down = (offset_ + max_) < total_,
        };
    }

private:
    std::size_t total_    = 0;
    std::size_t max_      = k_default_max_visible;
    std::size_t selected_ = 0;
    std::size_t offset_   = 0;   // first-visible index
};

// ── Transient-error classification (used by error-tab builder) ────────────

inline bool is_transient_error(ErrorKind k) {
    static const std::set<ErrorKind> transient{
        ErrorKind::GitAuthFailed,
        ErrorKind::GitTimeout,
        ErrorKind::NetworkError,
    };
    return transient.contains(k);
}

} // namespace cc::commands::plugin_helpers
