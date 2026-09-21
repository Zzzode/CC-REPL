/// @file plugin_ui_data.cppm
/// @brief Pure data-preparation logic extracted from React components in
///        `src/commands/plugin/`.  No FTXUI / rendering — that is Phase 4.
///
/// Components mined for pure functions:
///   - PluginSettings.tsx     — ViewState, getInitialViewState, getInitialTab,
///                              error-row builder, extra-marketplace removal plan,
///                              tab definitions, help text.
///   - BrowseMarketplace.tsx  — MarketplaceInfo aggregation, back-navigation
///                              routing logic, install-target pre-computation.
///   - DiscoverPlugins.tsx    — filtering / popularity-sort / empty-state
///                              detection plan.
///   - AddMarketplace.tsx     — input state machine (already mirrored as
///                              `classify_marketplace_input` in plugin_manage;
///                              this file keeps higher-level flow data).
///   - ManageMarketplaces.tsx — marketplace CRUD state transitions.
///   - PluginOptionsDialog.tsx — option-schema flattening, required-field list.
///   - PluginOptionsFlow.tsx  — step-list construction (ConfigStep struct +
///                              build_steps pure helper).
///   - PluginTrustWarning.tsx — trust-level → display-warning mapping.
///   - UnifiedInstalledCell.tsx — cell field aggregation.
///
/// UI rendering annotations throughout use the convention:
///   // UI: Phase 4 FTXUI -> cpp_migration/src/ui/plugins/<file>

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.commands.plugin_ui_data;

import cc.commands.plugin_parse_args;
import cc.commands.plugin_helpers;
import cc.commands.plugin_manage;

export namespace cc::commands::plugin_ui {

using ParsedArgs = cc::commands::plugin::ParsedSubcommand;
using SubType    = cc::commands::plugin::SubcommandType;
using MarketAct  = cc::commands::plugin::MarketplaceAction;
using namespace  cc::commands::plugin_helpers;

// ═════════════════════════════════════════════════════════════════════════════
// 1.  ViewState & routing  (from PluginSettings.tsx)
// ═════════════════════════════════════════════════════════════════════════════

/// Mirrors the TS `ViewState` discriminated union.  Each variant carries just
/// enough information to drive data preparation — the actual component tree
/// lives in Phase 4's FTXUI layer.
enum class ViewKind : unsigned char {
    Menu,               // Main menu (default when no args match a direct action)
    Help,               // /plugin help
    DiscoverPlugins,    // Discover tab (plugin list across all marketplaces)
    BrowseMarketplace,  // Inside a single marketplace
    ManagePlugins,      // Installed-tab list
    ManageMarketplaces, // Marketplaces-tab list
    AddMarketplace,     // Add-marketplace input
    MarketplaceMenu,    // Legacy menu for /plugin marketplace (→ Menu)
    MarketplaceList,    // /plugin marketplace list  (textual listing)
    Validate,           // /plugin validate <path>
    Errors,             // Errors tab (auto-routed from header)
};

inline constexpr std::string_view view_kind_name(ViewKind k) {
    switch (k) {
        case ViewKind::Menu:             return "menu";
        case ViewKind::Help:             return "help";
        case ViewKind::DiscoverPlugins:  return "discover-plugins";
        case ViewKind::BrowseMarketplace:return "browse-marketplace";
        case ViewKind::ManagePlugins:    return "manage-plugins";
        case ViewKind::ManageMarketplaces: return "manage-marketplaces";
        case ViewKind::AddMarketplace:   return "add-marketplace";
        case ViewKind::MarketplaceMenu:  return "marketplace-menu";
        case ViewKind::MarketplaceList:  return "marketplace-list";
        case ViewKind::Validate:         return "validate";
        case ViewKind::Errors:           return "errors";
    }
    return "?";
}

/// Target-action hint carried by manage-plugins / manage-marketplaces views
/// so Phase 4 can auto-focus a row (e.g. auto-open uninstall on a specific
/// plugin when the user typed "/plugin uninstall foo").
enum class TargetAction : unsigned char {
    None,
    Uninstall,
    Enable,
    Disable,
    Remove,
    Update,
};

struct ViewState {
    ViewKind kind = ViewKind::Menu;

    // ── Common navigation targets ────────────────────────────────────────
    std::optional<std::string> target_plugin;
    std::optional<std::string> target_marketplace;
    TargetAction              action = TargetAction::None;

    // ── Add-marketplace ──────────────────────────────────────────────────
    std::optional<std::string> add_initial_value;

    // ── Validate ─────────────────────────────────────────────────────────
    std::optional<std::string> validate_path;

    // ── Helpers ──────────────────────────────────────────────────────────
    [[nodiscard]] bool has_target_plugin() const {
        return target_plugin.has_value();
    }
    [[nodiscard]] bool has_target_marketplace() const {
        return target_marketplace.has_value();
    }
};

/// Top-level tabs of the PluginSettings dialog.
enum class TabId : unsigned char {
    Discover,
    Installed,
    Marketplaces,
    Errors,
};

inline TabId initial_tab_for(ViewKind k) {
    if (k == ViewKind::ManagePlugins)    return TabId::Installed;
    if (k == ViewKind::ManageMarketplaces) return TabId::Marketplaces;
    return TabId::Discover;
}

/// Translate the output of `parse_plugin_args` into the initial ViewState
/// the UI should present.  1:1 with TS `getInitialViewState`.
[[nodiscard]] inline ViewState get_initial_view_state(const ParsedArgs& cmd) {
    ViewState vs{};

    switch (cmd.type) {
        case SubType::Help:
            vs.kind = ViewKind::Help;
            return vs;

        case SubType::Validate:
            vs.kind          = ViewKind::Validate;
            vs.validate_path = cmd.validate_path;
            return vs;

        case SubType::Install:
            if (cmd.marketplace) {
                vs.kind              = ViewKind::BrowseMarketplace;
                vs.target_marketplace = *cmd.marketplace;
                vs.target_plugin     = cmd.plugin_name;
                return vs;
            }
            vs.kind          = ViewKind::DiscoverPlugins;
            vs.target_plugin = cmd.plugin_name; // optional — narrows selection
            return vs;

        case SubType::Manage:
            vs.kind = ViewKind::ManagePlugins;
            return vs;

        case SubType::Uninstall:
            vs.kind          = ViewKind::ManagePlugins;
            vs.target_plugin = cmd.target_plugin;
            vs.action        = TargetAction::Uninstall;
            return vs;

        case SubType::Enable:
            vs.kind          = ViewKind::ManagePlugins;
            vs.target_plugin = cmd.target_plugin;
            vs.action        = TargetAction::Enable;
            return vs;

        case SubType::Disable:
            vs.kind          = ViewKind::ManagePlugins;
            vs.target_plugin = cmd.target_plugin;
            vs.action        = TargetAction::Disable;
            return vs;

        case SubType::Marketplace: {
            switch (cmd.market_action) {
                case MarketAct::List:
                    vs.kind = ViewKind::MarketplaceList;
                    return vs;
                case MarketAct::Add:
                    vs.kind               = ViewKind::AddMarketplace;
                    if (!cmd.market_target.empty())
                        vs.add_initial_value = cmd.market_target;
                    return vs;
                case MarketAct::Remove:
                    vs.kind               = ViewKind::ManageMarketplaces;
                    vs.target_marketplace = cmd.market_target.empty()
                        ? std::nullopt : std::optional{cmd.market_target};
                    vs.action             = TargetAction::Remove;
                    return vs;
                case MarketAct::Update:
                    vs.kind               = ViewKind::ManageMarketplaces;
                    vs.target_marketplace = cmd.market_target.empty()
                        ? std::nullopt : std::optional{cmd.market_target};
                    vs.action             = TargetAction::Update;
                    return vs;
                case MarketAct::None:
                    vs.kind = ViewKind::MarketplaceMenu;
                    return vs;
            }
            vs.kind = ViewKind::MarketplaceMenu;
            return vs;
        }

        case SubType::Menu:
        default:
            // Default landing page — show the discovery tab.
            vs.kind = ViewKind::DiscoverPlugins;
            return vs;
    }
}

/// The full help-text block rendered by the `Help` view.
/// Byte-for-byte match of the TS PluginSettings.tsx help JSX (note: plain
/// hyphens "-", not em-dashes).  Kept here as a single constant so the UI
/// layer can display it verbatim.
inline constexpr std::string_view k_plugin_help_text =
R"(Plugin Command Usage:

Installation:
 /plugin install                           - Browse and install plugins
 /plugin install <marketplace>             - Install from specific marketplace
 /plugin install <plugin>                  - Install specific plugin
 /plugin install <plugin>@<market>         - Install plugin from marketplace

Management:
 /plugin manage                            - Manage installed plugins
 /plugin enable <plugin>                   - Enable a plugin
 /plugin disable <plugin>                  - Disable a plugin
 /plugin uninstall <plugin>                - Uninstall a plugin

Marketplaces:
 /plugin marketplace                       - Marketplace management menu
 /plugin marketplace add                   - Add a marketplace
 /plugin marketplace add <path/url>        - Add marketplace directly
 /plugin marketplace update                - Update marketplaces
 /plugin marketplace update <name>         - Update specific marketplace
 /plugin marketplace remove                - Remove a marketplace
 /plugin marketplace remove <name>         - Remove specific marketplace
 /plugin marketplace list                  - List all marketplaces

Validation:
 /plugin validate <path>                   - Validate a manifest file or directory

Other:
 /plugin                                   - Main plugin menu
 /plugin help                              - Show this help
 /plugins                                  - Alias for /plugin
)";

// ═════════════════════════════════════════════════════════════════════════════
// 2.  Error-tab row model  (from PluginSettings.tsx buildErrorRows / helpers)
// ═════════════════════════════════════════════════════════════════════════════

/// Action kind attached to an error row so Phase 4 knows what happens when
/// the user presses Enter on it.
enum class ErrorActionKind : unsigned char {
    None,
    Navigate,          // → switch tab + set ViewState
    RemoveExtraMP,     // remove from extraKnownMarketplaces + associated plugins
    RemoveInstalledMP, // remove from known_marketplaces.json via plugin_manager
    ManagedOnly,       // managed by admin — no-op
};

/// A single row in the Errors tab (data only).
struct ErrorRow {
    std::string label;
    std::string message;
    std::optional<std::string> guidance;
    ErrorActionKind action_kind = ErrorActionKind::None;
    std::optional<std::string> scope;   // "user" | "project" | "local" | "managed"

    // ── Navigate payload (action_kind == Navigate) ───────────────────────
    std::optional<TabId>    nav_tab;
    std::optional<ViewKind> nav_view;
    std::optional<std::string> nav_target_plugin;
    std::optional<std::string> nav_target_marketplace;
    TargetAction            nav_action = TargetAction::None;

    // ── Remove-Extra-MP payload ──────────────────────────────────────────
    std::optional<std::string> mp_name;
    std::vector<std::pair<std::string, std::string>> mp_sources; // (source_label, scope)
};

/// Determine whether an error's marketplace name appears only in policy and/or
/// in editable settings layers.  The real implementation consults
/// `getSettingsForSource`; this function is the data-flow contract Phase 4
/// calls *after* consulting those layers.
///
/// Editable sources are checked in order: user, project, local.
struct ExtraMPSourceInfo {
    std::vector<std::pair<std::string, std::string>> editable; // (source, scope)
    bool is_in_policy = false;
};

[[nodiscard]] inline ErrorActionKind build_marketplace_action_kind(
    const ExtraMPSourceInfo& info
) {
    if (!info.editable.empty()) return ErrorActionKind::RemoveExtraMP;
    if (info.is_in_policy)     return ErrorActionKind::ManagedOnly;
    // Marketplace is in known_marketplaces.json but not extraKnownMarketplaces
    return ErrorActionKind::Navigate;
}

/// Build the Navigate payload that routes a user from the Errors tab to a
/// specific marketplace row with the "remove" action pre-selected.
[[nodiscard]] inline ViewState navigate_remove_marketplace(std::string_view name) {
    ViewState vs;
    vs.kind               = ViewKind::ManageMarketplaces;
    vs.target_marketplace = std::string{name};
    vs.action             = TargetAction::Remove;
    return vs;
}

[[nodiscard]] inline ViewState navigate_plugin_action(std::string_view plugin,
                                                      TargetAction a) {
    ViewState vs;
    vs.kind          = ViewKind::ManagePlugins;
    vs.target_plugin = std::string{plugin};
    vs.action        = a;
    return vs;
}

// ═════════════════════════════════════════════════════════════════════════════
// 3.  DiscoverPlugins / BrowseMarketplace data prep
// ═════════════════════════════════════════════════════════════════════════════

/// Aggregated info shown in the BrowseMarketplace marketplace list.
struct MarketplaceInfo {
    std::string name;
    std::size_t total_plugins = 0;
    std::size_t installed_count = 0;
    std::optional<std::string> source_display; // e.g. "GitHub: anthropic/plugins"
    std::optional<std::string> warning;
};

/// Given a raw loaded marketplace, compute how many of its plugins are
/// currently installed (pure predicate callback — no side effects).
template <typename IsInstalledFn, typename PluginEntryRange>
[[nodiscard]] std::size_t count_installed_in_marketplace(
    std::string_view marketplace_name,
    const PluginEntryRange& entries,
    IsInstalledFn&& is_installed
) {
    std::size_t n = 0;
    for (const auto& entry : entries) {
        // createPluginId(entry.name, marketplace_name)
        const std::string id = std::string{entry.name} + '@' + std::string{marketplace_name};
        if (is_installed(id)) ++n;
    }
    return n;
}

/// Flatten all marketplaces into a single InstallablePlugin vector, filtering
/// out anything already globally-installed or blocked by policy.
/// Then sort by `install_count` descending, falling back to name ascending.
///
/// Pure: the caller supplies the is_installed / is_blocked predicates and
/// the counts map.
struct SortedDiscoverListResult {
    std::vector<InstallablePlugin> plugins;
    // true if the result set is empty and the caller should show an
    // empty-state message (see detectEmptyMarketplaceReason in TS).
    bool needs_empty_state = false;
    // human-readable empty reason (e.g. "All marketplaces failed to load")
    std::optional<std::string> empty_reason;
};

template <typename IsInstalledFn, typename IsBlockedFn, typename InstallCountsMap>
[[nodiscard]] SortedDiscoverListResult build_discover_list(
    const std::vector<std::pair<std::string, std::vector<InstallablePlugin::Entry>>>&
        per_marketplace_entries, // (marketplace_name, entries[])
    IsInstalledFn&& is_globally_installed,
    IsBlockedFn&&    is_blocked_by_policy,
    const InstallCountsMap& install_counts
) {
    SortedDiscoverListResult out;
    auto& list = out.plugins;

    for (const auto& [mp_name, entries] : per_marketplace_entries) {
        for (const auto& entry : entries) {
            const std::string id = entry.name + '@' + mp_name;
            const bool installed = is_globally_installed(id);
            if (installed)            continue;
            if (is_blocked_by_policy(id)) continue;
            InstallablePlugin p;
            p.entry            = entry;
            p.marketplace_name = mp_name;
            p.plugin_id        = id;
            p.is_installed     = installed;
            list.push_back(std::move(p));
        }
    }

    // Sort: popularity desc, then name asc.
    std::sort(list.begin(), list.end(), [&](const InstallablePlugin& a,
                                            const InstallablePlugin& b) {
        const auto ca = install_counts.contains(a.plugin_id)
            ? install_counts.at(a.plugin_id) : 0;
        const auto cb = install_counts.contains(b.plugin_id)
            ? install_counts.at(b.plugin_id) : 0;
        if (ca != cb) return ca > cb;
        return a.entry.name < b.entry.name;
    });

    return out;
}

/// Filter a plugin list by a case-insensitive text query (matches name,
/// marketplace, or description).  Used by DiscoverPlugins search bar.
[[nodiscard]] inline std::vector<InstallablePlugin> filter_by_query(
    const std::vector<InstallablePlugin>& in,
    std::string_view query
) {
    if (query.empty()) return in;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = lower(std::string{query});
    std::vector<InstallablePlugin> out;
    for (const auto& p : in) {
        if (lower(p.entry.name).find(q) != std::string::npos)          { out.push_back(p); continue; }
        if (p.entry.description &&
            lower(*p.entry.description).find(q) != std::string::npos) { out.push_back(p); continue; }
        if (lower(p.marketplace_name).find(q) != std::string::npos)    { out.push_back(p); continue; }
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// 4.  PluginOptions step builder  (from PluginOptionsFlow.tsx)
// ═════════════════════════════════════════════════════════════════════════════

/// A single configuration step the user must complete.  Mirrors the TS
/// `ConfigStep` struct; Phase 4 instantiates a FTXUI dialog per step.
struct ConfigStep {
    std::string key;          // "top-level" | "channel:<server>"
    std::string title;        // "Configure my-plugin"
    std::string subtitle;     // "Plugin options"
    // Schema / load / save are function references deferred to the Phase 4
    // module; here we only expose the *metadata* Phase 4 needs:
    std::size_t field_count = 0;
    std::vector<std::string> required_fields; // list of required option names
    // Has any existing saved value?  (Used to decide "Configure" vs "Reconfigure")
    bool has_saved_values = false;
};

/// Build the ordered list of steps from top-level manifest options and
/// per-channel unconfigured options.
[[nodiscard]] inline std::vector<ConfigStep> build_config_steps(
    std::size_t top_level_unconfigured_count,
    const std::vector<std::string>& top_level_required,
    bool top_level_has_saved,
    const std::string& plugin_display_name,
    const std::vector<std::pair<std::string, std::pair<std::string, std::size_t>>>&
        channel_items, // (server_name, (display_name, required_count))
    const std::vector<std::vector<std::string>>& channel_required_per_item
) {
    std::vector<ConfigStep> steps;
    if (top_level_unconfigured_count > 0) {
        ConfigStep s;
        s.key               = "top-level";
        s.title             = "Configure " + plugin_display_name;
        s.subtitle          = "Plugin options";
        s.field_count       = top_level_unconfigured_count;
        s.required_fields   = top_level_required;
        s.has_saved_values  = top_level_has_saved;
        steps.push_back(std::move(s));
    }
    for (std::size_t i = 0; i < channel_items.size(); ++i) {
        const auto& [server, meta]     = channel_items[i];
        const auto& [display, req_cnt] = meta;
        ConfigStep s;
        s.key         = "channel:" + server;
        s.title       = "Configure " + display;
        s.subtitle    = "Plugin: " + plugin_display_name;
        s.field_count = req_cnt;
        if (i < channel_required_per_item.size())
            s.required_fields = channel_required_per_item[i];
        steps.push_back(std::move(s));
    }
    return steps;
}

// ═════════════════════════════════════════════════════════════════════════════
// 5.  Trust-warning data contract  (from PluginTrustWarning.tsx)
// ═════════════════════════════════════════════════════════════════════════════

enum class TrustWarningLevel : unsigned char {
    None,        // verified / trusted
    Low,         // untrusted but no "unsafe" perms
    High,        // untrusted + unsafe perms present
    Unresolved,  // can't determine trust (shouldn't happen normally)
};

struct TrustWarningInfo {
    TrustWarningLevel level = TrustWarningLevel::None;
    std::string message;         // header line
    std::vector<std::string> bullets; // per-permission bullets
    // UI: Phase 4 FTXUI -> cpp_migration/src/ui/plugins/plugin_trust_warning.cppm
};

/// Map a trust level + permission list → warning info.
[[nodiscard]] inline TrustWarningInfo build_trust_warning(
    bool is_verified,
    bool is_trusted,
    const std::vector<std::string>& permissions
) {
    TrustWarningInfo w;
    if (is_verified) { return w; } // level::None

    const bool has_unsafe = [&] {
        for (const auto& p : permissions) {
            if (p.find("unsafe") != std::string::npos) return true;
        }
        return false;
    }();

    if (!is_trusted) {
        w.level   = has_unsafe ? TrustWarningLevel::High : TrustWarningLevel::Low;
        w.message = has_unsafe
            ? "This plugin has not been verified and requests elevated permissions."
            : "This plugin has not been independently verified.";
        w.bullets = permissions;
    }
    return w;
}

// ═════════════════════════════════════════════════════════════════════════════
// 6.  UnifiedInstalledCell aggregation  (pure data, zero render)
// ═════════════════════════════════════════════════════════════════════════════

/// Everything a single "installed plugin" cell needs to render.  FTXUI = P4.
struct InstalledCellData {
    std::string name;
    std::string marketplace;
    std::optional<std::string> version;
    std::optional<std::string> description;
    std::string scope;            // "user" | "project" | "local" | "managed"
    bool enabled = false;
    bool has_pending_update = false;
    bool has_error = false;
    bool has_config_options = false;
    std::size_t tool_count   = 0;
    std::size_t command_count = 0;
    std::optional<std::string> last_updated;
    // UI: Phase 4 FTXUI -> cpp_migration/src/ui/plugins/unified_installed_cell.cppm
};

// ═════════════════════════════════════════════════════════════════════════════
// 7.  BrowseMarketplace view-state router (back-navigation)
// ═════════════════════════════════════════════════════════════════════════════

/// Internal view of a BrowseMarketplace component.
enum class BrowseView : unsigned char {
    MarketplaceList, // user sees list of marketplaces
    PluginList,      // user sees plugins of a single marketplace
    PluginDetails,   // user sees a single plugin in detail
};

/// Compute the *next* view state when the user presses "back" (Esc).
/// Mirror of BrowseMarketplace handleBack callback (TS).
[[nodiscard]] inline ViewKind browse_back_destination(
    BrowseView current,
    bool has_target_marketplace_override, // true if routed directly from URL arg
    std::size_t total_marketplaces
) {
    if (current == BrowseView::PluginList) {
        if (has_target_marketplace_override) return ViewKind::ManageMarketplaces;
        if (total_marketplaces == 1)         return ViewKind::Menu;
        return ViewKind::BrowseMarketplace; // → marketplace-list sub-view
    }
    if (current == BrowseView::PluginDetails) {
        return ViewKind::BrowseMarketplace; // → plugin-list sub-view
    }
    // MarketplaceList — exit this tab
    return ViewKind::Menu;
}

} // namespace cc::commands::plugin_ui
