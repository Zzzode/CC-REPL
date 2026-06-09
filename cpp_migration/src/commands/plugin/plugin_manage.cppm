/// @file plugin_manage.cppm
/// @brief Pure-logic pieces of plugin management and validation.
///
/// Translated from two TS modules:
///   - ManagePlugins.tsx    (state, filtering, bulk operations on installed
///                          plugins — all *non-React* parts)
///   - ValidatePlugin.tsx   (validation output formatting and the
///                          "validate a path" CLI orchestration — the React
///                          wrapper is deferred)
///
/// All heavy lifting (manifest validation, plugin enable/disable, etc.) is
/// delegated to the shared `utils/plugin_*` modules.  This layer only
/// bridges command I/O to those utilities.

module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.commands.plugin_manage;

import cc.commands.plugin_helpers;
import cc.utils.plugin_validation;
import cc.utils.plugin_manager;

export namespace cc::commands::plugin_manage {

namespace fs = std::filesystem;
using namespace cc::commands::plugin_helpers;

// ═════════════════════════════════════════════════════════════════════════════
// 1.  Installed-plugin list helpers  (pure data transforms from TS ManagePlugins)
// ═════════════════════════════════════════════════════════════════════════════

/// A display row for the "Installed" tab.  UI rendering (FTXUI) is Phase 4.
struct InstalledPluginRow {
    std::string plugin_id;          // "name@marketplace"
    std::string display_name;       // plain name
    std::string marketplace;
    std::optional<std::string> version;
    std::optional<std::string> description;
    bool enabled = false;
    std::string scope;              // "user" | "project" | "local" | "managed"
    std::optional<std::string> last_updated;
    bool has_pending_update = false;
    bool has_config_options = false;
};

/// Return the human-readable scope label.
[[nodiscard]] inline std::string scope_label(cc::utils::plugin_manager::PluginScope s) {
    using S = cc::utils::plugin_manager::PluginScope;
    switch (s) {
        case S::User:    return "user";
        case S::Project: return "project";
        case S::Local:   return "local";
        case S::Managed: return "managed";
    }
    return "unknown";
}

/// Predicate used by the search filter in ManagePlugins (TS).
/// Matches against name, marketplace, or description (case-insensitive).
[[nodiscard]] inline bool row_matches_query(const InstalledPluginRow& row,
                                             std::string_view query) {
    if (query.empty()) return true;
    auto ci = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string q = ci(std::string{query});
    return ci(row.display_name).find(q) != std::string::npos ||
           ci(row.marketplace).find(q)  != std::string::npos ||
           (row.description && ci(*row.description).find(q) != std::string::npos);
}

/// Bulk action result for enable/disable/uninstall of multiple plugins.
struct BulkActionResult {
    std::size_t succeeded = 0;
    std::size_t failed    = 0;
    std::vector<std::pair<std::string, std::string>> errors; // id -> message
};

// ═════════════════════════════════════════════════════════════════════════════
// 2.  Validate-plugin CLI formatting  (from TS ValidatePlugin)
// ═════════════════════════════════════════════════════════════════════════════

/// Characters used by the TS code (figures cross/tick/pointer + warning).
/// Using plain ASCII / common Unicode so this module stays header-light.
inline constexpr std::string_view k_cross    = "\xe2\x9c\x97";   // ✗
inline constexpr std::string_view k_tick     = "\xe2\x9c\x93";   // ✓
inline constexpr std::string_view k_pointer  = "\xe2\x9d\xaf";   // ❯
inline constexpr std::string_view k_warning  = "\xe2\x9a\xa0";   // ⚠

/// Help text printed when /plugin validate is invoked with no path.
inline constexpr std::string_view k_validate_usage =
R"(Usage: /plugin validate <path>

Validate a plugin or marketplace manifest file or directory.

Examples:
  /plugin validate .claude-plugin/plugin.json
  /plugin validate /path/to/plugin-directory
  /plugin validate .

When given a directory, automatically validates .claude-plugin/marketplace.json
or .claude-plugin/plugin.json (prefers marketplace if both exist).

Or from the command line:
  claude plugin validate <path>)";

/// Count-based plural (TS `plural(n, "error")` -> "error" | "errors").
inline std::string plural(std::size_t n, std::string_view noun) {
    std::string s{noun};
    if (n == 1) return s;
    // Simple plural rules — sufficient for error/warning.
    if (!s.empty() && s.back() == 's') s += "es";
    else                               s += 's';
    return s;
}

/// Format a ValidationResult (from plugin_validation) into a display string
/// exactly matching the TS output.  Also returns a suggested process exit
/// code: 0 = success, 1 = validation errors, 2 = unexpected exception.
struct ValidateOutput {
    std::string text;
    int exit_code = 0;
};

[[nodiscard]] inline ValidateOutput format_validation_result(
    const cc::utils::plugin_validation::ValidationResult& r
) {
    ValidateOutput out;
    std::ostringstream os;

    const char* type_label = "plugin";
    using T = cc::utils::plugin_validation::ValidatedFileType;
    switch (r.file_type) {
        case T::Plugin:      type_label = "plugin";      break;
        case T::Marketplace: type_label = "marketplace"; break;
        case T::Skill:       type_label = "skill";       break;
        case T::Agent:       type_label = "agent";       break;
        case T::Command:     type_label = "command";     break;
        case T::Hooks:       type_label = "hooks";       break;
    }

    os << "Validating " << type_label << " manifest: "
       << r.file_path.string() << "\n\n";

    if (!r.errors.empty()) {
        os << k_cross << " Found " << r.errors.size() << ' '
           << plural(r.errors.size(), "error") << ":\n\n";
        for (const auto& e : r.errors) {
            os << "  " << k_pointer << ' ' << e.path << ": " << e.message << '\n';
        }
        os << '\n';
    }

    if (!r.warnings.empty()) {
        os << k_warning << " Found " << r.warnings.size() << ' '
           << plural(r.warnings.size(), "warning") << ":\n\n";
        for (const auto& w : r.warnings) {
            os << "  " << k_pointer << ' ' << w.path << ": " << w.message << '\n';
        }
        os << '\n';
    }

    if (r.success) {
        if (!r.warnings.empty())
            os << k_tick << " Validation passed with warnings\n";
        else
            os << k_tick << " Validation passed\n";
        out.exit_code = 0;
    } else {
        os << k_cross << " Validation failed\n";
        out.exit_code = 1;
    }

    out.text = os.str();
    return out;
}

/// Orchestrate a single /plugin validate <path> call:
///   1. auto-detect file type via plugin_validation::validate_file
///   2. format via format_validation_result
/// Callers translate the returned `exit_code` to their environment
/// (process exit or a reply status); this function never throws.
[[nodiscard]] inline ValidateOutput run_validation(const std::string& path) {
    namespace pv = cc::utils::plugin_validation;
    try {
        const auto result = pv::validate_file(fs::path{path});
        return format_validation_result(result);
    } catch (const std::exception& e) {
        ValidateOutput v;
        std::ostringstream os;
        os << k_cross << " Unexpected error during validation: " << e.what();
        v.text      = os.str();
        v.exit_code = 2;
        return v;
    } catch (...) {
        ValidateOutput v;
        v.text      = std::string{k_cross} + " Unexpected error during validation";
        v.exit_code = 2;
        return v;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 3.  Plugin-option target lookup  (from TS PluginOptionsFlow.findPluginOptionsTarget)
// ═════════════════════════════════════════════════════════════════════════════

/// After a successful install, determine which LoadedPlugin corresponds to
/// the freshly-installed id (checking repository and source fields).
/// Returns the plugin's repository/source identifier string if found,
/// nullopt otherwise.
///
/// The full `LoadedPlugin` struct lives in cc.utils.plugin_loader; we return
/// only the string identifier here because Phase 4's dialog layer will
/// re-load the full struct when rendering.
[[nodiscard]] inline std::optional<std::string> find_options_target(
    std::string_view plugin_id,
    const std::vector<std::pair<std::string, std::string>>& all_plugins
    // ^-- pair<repository, source> for every loaded plugin (enabled + disabled)
) {
    for (const auto& [repo, src] : all_plugins) {
        if (repo == plugin_id || src == plugin_id) return repo;
    }
    return std::nullopt;
}

// ═════════════════════════════════════════════════════════════════════════════
// 4.  Marketplace add validation  (from TS AddMarketplace.handleAdd)
// ═════════════════════════════════════════════════════════════════════════════

/// Data-only result of preparing a "parse + add marketplace" flow.
/// Actual filesystem / network work stays in `utils/plugin_marketplace`.
struct MarketplaceAddPlan {
    bool ok = false;
    std::string error_message;
    std::string normalized_name;
    // source_kind: "github" | "git" | "url" | "directory" | "file"
    std::string source_kind;
    std::string source_detail;    // owner/repo, full URL, directory path...
};

/// Lightweight input classification — defers to `utils/plugin_marketplace`
/// for the real parse.  This mirrors the TS `parseMarketplaceInput`
/// preliminary branch that decides which kind of source the user gave us.
[[nodiscard]] inline MarketplaceAddPlan classify_marketplace_input(std::string_view input) {
    MarketplaceAddPlan p;
    if (input.empty()) {
        p.error_message = "Please enter a marketplace source";
        return p;
    }

    // 1) GitHub: "owner/repo"  (a single slash, no scheme, no leading dot/path)
    const auto slash = input.find('/');
    if (slash != std::string_view::npos &&
        !input.starts_with('.') &&
        !input.starts_with('/') &&
        !input.starts_with("http://") &&
        !input.starts_with("https://") &&
        !input.starts_with("git@") &&
        !input.starts_with("file://"))
    {
        // Only treat as github-repo if exactly one slash and both sides nonempty
        const auto second_slash = input.find('/', slash + 1);
        if (second_slash == std::string_view::npos &&
            slash > 0 && slash + 1 < input.size())
        {
            p.ok             = true;
            p.source_kind    = "github";
            p.source_detail  = std::string{input};
            p.normalized_name = std::string{input.substr(slash + 1)};
            return p;
        }
    }

    // 2) SSH git URL
    if (input.starts_with("git@")) {
        p.ok            = true;
        p.source_kind   = "git";
        p.source_detail = std::string{input};
        // derive name: strip "git@host:", trim ".git" if present
        auto colon = input.find(':');
        if (colon == std::string_view::npos) colon = 0; else colon += 1;
        auto tail = input.substr(colon);
        if (tail.ends_with(".git")) tail.remove_suffix(4);
        auto last_slash = tail.rfind('/');
        p.normalized_name = std::string{
            last_slash == std::string_view::npos ? tail : tail.substr(last_slash + 1)
        };
        return p;
    }

    // 3) URL (http / https / file)
    if (input.starts_with("http://") || input.starts_with("https://")) {
        p.ok            = true;
        p.source_kind   = "url";
        p.source_detail = std::string{input};
        // use last path component as name
        auto last = input.rfind('/');
        auto tail = (last == std::string_view::npos) ? input : input.substr(last + 1);
        if (tail.ends_with(".json")) tail.remove_suffix(5);
        p.normalized_name = std::string{tail};
        return p;
    }
    if (input.starts_with("file://")) {
        p.ok            = true;
        p.source_kind   = "file";
        p.source_detail = std::string{input.substr(7)};
        fs::path pp{p.source_detail};
        p.normalized_name = pp.filename().stem().string();
        return p;
    }

    // 4) Local path (./ or absolute or just a name that exists on disk)
    if (input.starts_with('.') || input.starts_with('/') || input.find('\\') != std::string_view::npos) {
        p.ok            = true;
        p.source_kind   = "directory";
        p.source_detail = std::string{input};
        fs::path pp{p.source_detail};
        p.normalized_name = pp.filename().string();
        if (p.normalized_name.empty()) p.normalized_name = "marketplace";
        return p;
    }

    // 5) Unknown
    p.error_message =
        "Invalid marketplace source format. Try: owner/repo, https://..., or ./path";
    return p;
}

} // namespace cc::commands::plugin_manage
