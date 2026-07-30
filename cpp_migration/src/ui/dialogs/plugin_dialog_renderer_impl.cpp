/// @file plugin_dialog_renderer_impl.cpp
/// @brief PluginDialog dialog renderer implementation — module impl unit.
///
/// This file contains the ACTUAL implementation (holder, data preparation,
/// view-state derivation, renderer lambda).  It is a module implementation
/// unit (`module cc.ui.dialogs.plugin_dialog_renderer;` without `export`),
/// so its heavy imports (plugin_dialog, plugin_ui_data, plugin_helpers,
/// plugin_marketplace, manage_plugins) have their own independent
/// source-location budget.  Importers of the module interface never see
/// this closure.
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

module cc.ui.dialogs.plugin_dialog_renderer;

import cc.ui.dialogs.system;
import cc.ui.dialogs.plugin_dialog;
import cc.commands.plugin.manage_plugins;
import cc.commands.plugin_ui_data;
import cc.commands.plugin_helpers;
import cc.utils.plugin_marketplace;

namespace cc::ui::dialogs::plugin_dialog_renderer {

namespace dsys = cc::ui::dialogs::system;
namespace ui   = cc::commands::plugin_ui;
namespace pf   = cc::commands::plugin_helpers;
namespace pm   = cc::utils::plugin_marketplace;
namespace pd   = cc::ui::dialogs::plugin_dialog;

using namespace ftxui;

// ============================================================
// Holder component
// ============================================================

/// Holder component that wraps a MakePluginDialog component so the dialog
/// system can render it and route events to its CatchEvent handler.
/// Mirrors DoctorDialogHolder (ComponentBase::OnEvent is protected).
struct PluginDialogHolder : public ComponentBase {
    Component screen;

    explicit PluginDialogHolder(Component s) : screen(std::move(s)) {
        ComponentBase::Add(screen);
    }

    Element Render() override { return screen->Render(); }

    bool InjectEvent(const Event& event) {
        return ComponentBase::OnEvent(event);
    }
};

// ============================================================
// View-state derivation
// ============================================================

/// Derive the initial ViewState from the payload id suffix.  The id is
/// formatted as "plugins-dialog:<suffix>" where <suffix> is the metadata tag
/// produced by triggers.cppm (e.g. "discover-plugins",
/// "browse-marketplace:acme-marketplace", "manage-plugins?action=uninstall").
/// 1:1 with TS getInitialViewState + the triggers.cppm metadata mapping.
[[nodiscard]] ui::ViewState derive_initial_view(std::string_view id) {
    ui::ViewState vs{};

    // Strip the "plugins-dialog:" prefix (with or without trailing colon).
    constexpr std::string_view kPrefix = "plugins-dialog:";
    std::string_view suffix = id;
    if (suffix.substr(0, kPrefix.size()) == kPrefix) {
        suffix = suffix.substr(kPrefix.size());
    }

    // browse-marketplace:<name> — scoped to a single marketplace.
    constexpr std::string_view kBrowse = "browse-marketplace:";
    if (suffix.substr(0, kBrowse.size()) == kBrowse) {
        vs.kind = ui::ViewKind::BrowseMarketplace;
        vs.target_marketplace = std::string{suffix.substr(kBrowse.size())};
        return vs;
    }

    // manage-plugins?action=<action> — preselect an action on the installed tab.
    constexpr std::string_view kManageQ = "manage-plugins?";
    if (suffix.substr(0, kManageQ.size()) == kManageQ) {
        vs.kind = ui::ViewKind::ManagePlugins;
        auto q = suffix.substr(kManageQ.size());
        if (q == "action=uninstall")     vs.action = ui::TargetAction::Uninstall;
        else if (q == "action=enable")   vs.action = ui::TargetAction::Enable;
        else if (q == "action=disable")  vs.action = ui::TargetAction::Disable;
        return vs;
    }

    // manage-marketplaces?action=<action> — preselect an action on settings.
    constexpr std::string_view kMarketQ = "manage-marketplaces?";
    if (suffix.substr(0, kMarketQ.size()) == kMarketQ) {
        vs.kind = ui::ViewKind::ManageMarketplaces;
        auto q = suffix.substr(kMarketQ.size());
        if (q == "action=remove")        vs.action = ui::TargetAction::Remove;
        else if (q == "action=update")   vs.action = ui::TargetAction::Update;
        return vs;
    }

    // Exact-match suffixes.
    if (suffix == "discover-plugins")    { vs.kind = ui::ViewKind::DiscoverPlugins;  return vs; }
    if (suffix == "manage-plugins")      { vs.kind = ui::ViewKind::ManagePlugins;    return vs; }
    if (suffix == "manage-marketplaces") { vs.kind = ui::ViewKind::ManageMarketplaces; return vs; }
    if (suffix == "add-marketplace")     { vs.kind = ui::ViewKind::AddMarketplace;   return vs; }

    // Fallback: no recognized suffix → Discover tab (TS has no card dashboard).
    vs.kind = ui::ViewKind::DiscoverPlugins;
    return vs;
}

// ============================================================
// Data preparation
// ============================================================

/// Convert an installed plugin (manifest-derived) into a display-ready cell.
[[nodiscard]] ui::InstalledCellData to_installed_cell(
    const cc::commands::InstalledPlugin& p) {
    ui::InstalledCellData cell;
    // id may be "name@marketplace"; split for display.
    auto at = p.id.find('@');
    if (at != std::string::npos) {
        cell.name = p.id.substr(0, at);
        cell.marketplace = p.id.substr(at + 1);
    } else {
        cell.name = p.id;
        cell.marketplace = "local";
    }
    cell.version = p.version;
    cell.enabled = p.enabled;
    cell.scope = "user";
    cell.description = std::nullopt;
    cell.has_pending_update = false;
    cell.has_error = false;
    cell.has_config_options = false;
    cell.tool_count = 0;
    cell.command_count = 0;
    cell.last_updated = std::nullopt;
    return cell;
}

/// Try to read a single marketplace's cached manifest (no network).  Returns
/// nullopt when no cache exists yet (fresh install / never fetched).
[[nodiscard]] std::optional<pm::PluginMarketplace> load_cached_marketplace(
    std::string_view name,
    const pm::KnownMarketplace& km) {
    namespace detail = pm::detail;
    std::error_code ec;
    std::filesystem::path cache_path;
    if (km.cache_path && !km.cache_path->empty()) {
        cache_path = *km.cache_path;
    } else {
        cache_path = detail::get_marketplaces_cache_dir() / std::string(name);
    }
    if (!std::filesystem::exists(cache_path, ec)) return std::nullopt;

    auto manifest = detail::resolve_manifest_path(cache_path, km.source);
    if (!std::filesystem::exists(manifest, ec)) return std::nullopt;

    auto doc = detail::read_marketplace_json(manifest);
    if (!doc) return std::nullopt;
    auto mp = detail::to_marketplace(*doc);
    if (!mp) return std::nullopt;
    return *mp;
}

/// Human-readable source tag for a marketplace (e.g. "GitHub: owner/repo").
[[nodiscard]] std::string source_display(const pm::MarketplaceSource& src) {
    if (auto g = std::get_if<pm::GitHubMarketplaceSource>(&src))
        return "GitHub: " + g->repo;
    if (auto g = std::get_if<pm::GitMarketplaceSource>(&src))
        return "Git: " + g->url;
    if (auto u = std::get_if<pm::UrlMarketplaceSource>(&src))
        return "URL: " + u->url;
    if (auto d = std::get_if<pm::DirectoryMarketplaceSource>(&src))
        return "Directory: " + d->path;
    if (auto f = std::get_if<pm::FileMarketplaceSource>(&src))
        return "File: " + f->path;
    return "";
}

/// Build the full PluginDialogInputs from disk + cache, given the initial
/// ViewState.  Installed plugins come from ~/.cc-repl/plugins manifests;
/// marketplaces + discover plugins come from known_marketplaces.json + the
/// per-marketplace cache (no network is performed here).
[[nodiscard]] pd::PluginDialogInputs build_plugin_dialog_inputs(
    ui::ViewState initial,
    std::function<void()> on_close) {
    pd::PluginDialogInputs inputs;
    inputs.initial_view = std::move(initial);

    // ── Installed plugins ──────────────────────────────────────────────────
    auto installed = cc::commands::get_installed_plugins();
    inputs.installed_plugins.reserve(installed.size());
    for (const auto& p : installed) {
        inputs.installed_plugins.push_back(to_installed_cell(p));
    }

    // ── Marketplaces + discover plugins ────────────────────────────────────
    auto config = pm::load_known_marketplaces_config_safe();
    inputs.marketplaces.reserve(config.size());
    for (const auto& [name, km] : config) {
        ui::MarketplaceInfo info;
        info.name = name;
        info.source_display = source_display(km.source);

        auto cached = load_cached_marketplace(name, km);
        if (cached) {
            info.total_plugins = cached->plugins.size();
            // Count how many installed plugins came from this marketplace.
            std::size_t inst_here = 0;
            for (const auto& cell : inputs.installed_plugins) {
                if (cell.marketplace == name) ++inst_here;
            }
            info.installed_count = inst_here;

            // Flatten each plugin entry into a discoverable InstallablePlugin.
            for (const auto& [entry_name, entry] : cached->plugins) {
                pf::InstallablePlugin ip;
                ip.entry.name = entry_name;
                ip.entry.description = entry.description;
                ip.marketplace_name = name;
                ip.plugin_id = entry_name + "@" + name;
                // is_installed: check against installed list by name+marketplace.
                for (const auto& cell : inputs.installed_plugins) {
                    if (cell.name == entry_name && cell.marketplace == name) {
                        ip.is_installed = true;
                        break;
                    }
                }
                inputs.discover_plugins.push_back(std::move(ip));
            }
        } else {
            info.total_plugins = 0;
            info.installed_count = 0;
        }
        inputs.marketplaces.push_back(std::move(info));
    }

    // Discover empty state.
    inputs.discover_empty = inputs.discover_plugins.empty();
    if (inputs.discover_empty) {
        inputs.discover_empty_reason =
            config.empty() ? "No marketplaces configured — add one to discover plugins"
                           : "No plugins found in configured marketplaces";
    }

    // ── Callbacks ──────────────────────────────────────────────────────────
    inputs.on_close = std::move(on_close);
    inputs.on_navigate = [](const ui::ViewState&) {}; // navigation is internal

    return inputs;
}

// ============================================================
// Component cache
// ============================================================

/// Static cache of PluginDialog components, keyed by payload id.  We cannot
/// store the component on the payload (PluginDialogPayload has no holder field
/// — adding one would recompile app.pcm and crash app_constructor.cpp), so we
/// keep a process-wide map here.  The payload id is unique per dialog instance
/// (e.g. "plugins-dialog:discover-plugins"), so caching by id gives the same
/// lazy-build + reuse-across-frames behaviour as an in-struct holder.
/// MIRRORS: MCPDialogPayload.component (which IS stored on the struct because
/// MCPDialog's field predates the app.pcm blow-up).
using ComponentCache = std::map<std::string, std::shared_ptr<PluginDialogHolder>>;

[[nodiscard]] ComponentCache& component_cache() {
    static ComponentCache cache;
    return cache;
}

/// Drop the cached component for a payload id (e.g. when the dialog closes).
void evict_component(const std::string& id) {
    component_cache().erase(id);
}

// ============================================================
// Renderer registration
// ============================================================

void register_plugin_dialog_renderer(dsys::DialogRendererRegistry& registry) {
    registry.register_dialog(
        dsys::DialogType::PluginDialog,
        /*renderer=*/
        [](dsys::DialogPayloadVariant& payload,
           const dsys::DialogRenderContext& /*ctx*/) -> Element {
            auto* p = std::get_if<dsys::PluginDialogPayload>(&payload);
            if (!p) return text("");

            // Lazily create the PluginDialog component on first render.
            auto& cache = component_cache();
            auto it = cache.find(p->id);
            if (it == cache.end()) {
                auto initial = derive_initial_view(p->id);
                // Wrap on_close so the cached component is evicted on dialog
                // dismissal (prevents unbounded cache growth; the next open
                // rebuilds fresh state).
                std::string id_copy = p->id;
                auto on_close = [id_copy, oc = p->on_close]() {
                    evict_component(id_copy);
                    if (oc) oc();
                };
                auto inputs  = build_plugin_dialog_inputs(
                    std::move(initial), std::move(on_close));

                auto dialog = pd::MakePluginDialog(std::move(inputs));
                auto holder = std::make_shared<PluginDialogHolder>(
                    std::move(dialog));
                it = cache.emplace(std::move(id_copy), std::move(holder)).first;
            }

            return it->second ? it->second->Render() : text("");
        },
        /*event_handler=*/
        [](dsys::DialogPayloadVariant& payload, const Event& event) -> bool {
            auto* p = std::get_if<dsys::PluginDialogPayload>(&payload);
            if (!p) return false;

            auto& cache = component_cache();
            auto it = cache.find(p->id);
            if (it == cache.end() || !it->second) return false;
            return it->second->InjectEvent(event);
        }
    );
}

} // namespace cc::ui::dialogs::plugin_dialog_renderer
