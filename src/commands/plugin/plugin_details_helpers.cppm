/// @file plugin_details_helpers.cppm
/// @brief Shared helper types and pure functions for plugin details views.
///
/// Extracted from src/commands/plugin/pluginDetailsHelpers.tsx.
/// Contains: InstallablePlugin / PluginDetailsMenuOption types,
/// extractGitHubRepo(), and buildPluginDetailsMenuOptions().
/// No React/FTXUI rendering code is included.

module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <variant>
#include <format>

export module cc.commands.plugin_details_helpers;

export namespace cc::commands::plugin {

// ---------------------------------------------------------------------------
// Forward-declared minimal marketplace entry shape.
// The real PluginMarketplaceEntry lives in utils/plugins/schemas.
// We only need the `source` field here, which is a tagged union.
// ---------------------------------------------------------------------------

/// Source of a marketplace plugin entry. Mirrors TS: PluginMarketplaceEntry.source
enum class EntrySourceKind : unsigned char {
    None,        // No source information available
    GitHub,      // `{ source: "github", repo: "owner/repo" }`
    Url,         // `{ source: "url", url: "https://..." }`
    Local,       // `{ source: "local", path: "/path/to/plugin" }`
};

struct EntrySource {
    EntrySourceKind kind{EntrySourceKind::None};
    std::string repo;     // when kind == GitHub
    std::string url;      // when kind == Url
    std::string path;     // when kind == Local
};

/// Minimal PluginMarketplaceEntry view used by helpers here.
struct MarketplaceEntryView {
    EntrySource source{};
    // Other fields (name, version, etc.) are not referenced by the helpers.
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/// Represents a plugin available for installation from a marketplace.
struct InstallablePlugin {
    MarketplaceEntryView entry;
    std::string marketplace_name;
    std::string plugin_id;
    bool is_installed = false;
};

/// Menu option for plugin details view.
struct PluginDetailsMenuOption {
    std::string label;
    std::string action;
};

// ---------------------------------------------------------------------------
// extractGitHubRepo
// ---------------------------------------------------------------------------

/// Extract GitHub repo ("owner/repo") from a plugin's entry.source when the
/// source kind is GitHub. Returns nullopt otherwise.
/// Mirrors TS: extractGitHubRepo(plugin: InstallablePlugin): string | null
[[nodiscard]] inline std::optional<std::string> extract_github_repo(
    const InstallablePlugin& plugin)
{
    if (plugin.entry.source.kind == EntrySourceKind::GitHub
        && !plugin.entry.source.repo.empty())
    {
        return plugin.entry.source.repo;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// buildPluginDetailsMenuOptions
// ---------------------------------------------------------------------------

/// Build the ordered list of context-menu options for the plugin details
/// screen. Always includes the three install-scope options; conditionally
/// adds homepage, github, and back options.
///
/// Mirrors TS: buildPluginDetailsMenuOptions(hasHomepage, githubRepo)
[[nodiscard]] inline std::vector<PluginDetailsMenuOption>
build_plugin_details_menu_options(
    std::optional<std::string_view> has_homepage,
    std::optional<std::string_view> github_repo)
{
    std::vector<PluginDetailsMenuOption> options{
        {"Install for you (user scope)", "install-user"},
        {"Install for all collaborators on this repository (project scope)", "install-project"},
        {"Install for you, in this repo only (local scope)", "install-local"},
    };

    if (has_homepage.has_value() && !has_homepage->empty()) {
        options.push_back({"Open homepage", "homepage"});
    }

    if (github_repo.has_value() && !github_repo->empty()) {
        options.push_back({"View on GitHub", "github"});
    }

    options.push_back({"Back to plugin list", "back"});
    return options;
}

} // namespace cc::commands::plugin
