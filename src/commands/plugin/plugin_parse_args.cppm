/// @file plugin_parse_args.cppm
/// @brief Parses /plugin subcommand arguments into structured command variants.
///
/// Translated from TypeScript: src/commands/plugin/parseArgs.ts
/// Pure function: no filesystem I/O, no global state.

module;

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.commands.plugin_parse_args;

export namespace cc::commands::plugin {

// ─────────────────────────────────────────────────────────────────────────────
// Subcommand variant types
// ─────────────────────────────────────────────────────────────────────────────

enum class SubcommandType : unsigned char {
    Menu,       // No args → show main menu
    Help,       // help | --help | -h
    Install,    // install / i
    Manage,     // manage
    Uninstall,  // uninstall
    Enable,     // enable
    Disable,    // disable
    Validate,   // validate
    Marketplace // marketplace / market
};

enum class MarketplaceAction : unsigned char {
    None,   // No sub-action → marketplace menu
    Add,    // add
    Remove, // remove / rm
    Update, // update
    List    // list
};

/// Parsed representation of a /plugin command invocation.
struct ParsedSubcommand {
    SubcommandType type = SubcommandType::Menu;

    // ── Install fields ────────────────────────────────────────────────────
    std::optional<std::string> marketplace; // install: marketplace URL/name
    std::optional<std::string> plugin_name; // install: plugin name

    // ── Uninstall/Enable/Disable fields ───────────────────────────────────
    std::optional<std::string> target_plugin; // uninstall|enable|disable target

    // ── Validate fields ───────────────────────────────────────────────────
    std::optional<std::string> validate_path; // validate target

    // ── Marketplace fields ────────────────────────────────────────────────
    MarketplaceAction market_action = MarketplaceAction::None;
    std::string market_target; // add|remove|update target (URL/name)
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    std::string tok;
    for (char ch : s) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!tok.empty()) {
                out.push_back(std::move(tok));
                tok.clear();
            }
        } else {
            tok.push_back(ch);
        }
    }
    if (!tok.empty()) out.push_back(std::move(tok));
    return out;
}

inline std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool looks_like_marketplace(std::string_view target) {
    return target.starts_with("http://") ||
           target.starts_with("https://") ||
           target.starts_with("file://") ||
           target.find('/') != std::string_view::npos ||
           target.find('\\') != std::string_view::npos;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// Parse plugin subcommand arguments (the string after "/plugin ").
/// Empty input yields Menu.
[[nodiscard]] inline ParsedSubcommand parse_plugin_args(std::optional<std::string_view> raw_args) {
    ParsedSubcommand out{};

    if (!raw_args || raw_args->empty()) {
        out.type = SubcommandType::Menu;
        return out;
    }

    auto parts = detail::split_ws(*raw_args);
    if (parts.empty()) {
        out.type = SubcommandType::Menu;
        return out;
    }

    const std::string cmd = detail::to_lower(parts[0]);

    // ── help ──────────────────────────────────────────────────────────────
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        out.type = SubcommandType::Help;
        return out;
    }

    // ── install / i ───────────────────────────────────────────────────────
    if (cmd == "install" || cmd == "i") {
        out.type = SubcommandType::Install;
        if (parts.size() < 2) return out; // bare "install" → browse UI later

        const std::string& target = parts[1];
        const auto at = target.find('@');
        if (at != std::string::npos) {
            // plugin@marketplace format
            out.plugin_name   = target.substr(0, at);
            out.marketplace   = target.substr(at + 1);
            return out;
        }
        if (detail::looks_like_marketplace(target)) {
            out.marketplace = target;
            return out;
        }
        out.plugin_name = target;
        return out;
    }

    // ── manage ────────────────────────────────────────────────────────────
    if (cmd == "manage") {
        out.type = SubcommandType::Manage;
        return out;
    }

    // ── uninstall ─────────────────────────────────────────────────────────
    if (cmd == "uninstall") {
        out.type = SubcommandType::Uninstall;
        if (parts.size() >= 2) out.target_plugin = parts[1];
        return out;
    }

    // ── enable ────────────────────────────────────────────────────────────
    if (cmd == "enable") {
        out.type = SubcommandType::Enable;
        if (parts.size() >= 2) out.target_plugin = parts[1];
        return out;
    }

    // ── disable ───────────────────────────────────────────────────────────
    if (cmd == "disable") {
        out.type = SubcommandType::Disable;
        if (parts.size() >= 2) out.target_plugin = parts[1];
        return out;
    }

    // ── validate ──────────────────────────────────────────────────────────
    if (cmd == "validate") {
        out.type = SubcommandType::Validate;
        std::string joined;
        for (std::size_t i = 1; i < parts.size(); ++i) {
            if (i > 1) joined.push_back(' ');
            joined += parts[i];
        }
        if (!joined.empty()) out.validate_path = std::move(joined);
        return out;
    }

    // ── marketplace / market ──────────────────────────────────────────────
    if (cmd == "marketplace" || cmd == "market") {
        out.type = SubcommandType::Marketplace;
        const std::string action =
            parts.size() >= 2 ? detail::to_lower(parts[1]) : std::string{};

        if (action == "add") {
            out.market_action = MarketplaceAction::Add;
            std::string tgt;
            for (std::size_t i = 2; i < parts.size(); ++i) {
                if (i > 2) tgt.push_back(' ');
                tgt += parts[i];
            }
            out.market_target = std::move(tgt);
        } else if (action == "remove" || action == "rm") {
            out.market_action = MarketplaceAction::Remove;
            std::string tgt;
            for (std::size_t i = 2; i < parts.size(); ++i) {
                if (i > 2) tgt.push_back(' ');
                tgt += parts[i];
            }
            out.market_target = std::move(tgt);
        } else if (action == "update") {
            out.market_action = MarketplaceAction::Update;
            std::string tgt;
            for (std::size_t i = 2; i < parts.size(); ++i) {
                if (i > 2) tgt.push_back(' ');
                tgt += parts[i];
            }
            out.market_target = std::move(tgt);
        } else if (action == "list") {
            out.market_action = MarketplaceAction::List;
        }
        // else: None → marketplace menu
        return out;
    }

    // ── unknown → menu (default) ──────────────────────────────────────────
    out.type = SubcommandType::Menu;
    return out;
}

} // namespace cc::commands::plugin
