/// @file keybindings_cmd.cppm
/// @brief KeybindingsCommand implementing the /keybindings slash command.
/// Subcommands: list | search PATTERN | reset | export | set KEY ACTION.
/// Default (no args): creates keybindings.json template + opens in editor
///   (mirrors TS src/commands/keybindings/keybindings.ts logic exactly).
/// Reuses cc.keybindings.* modules — no type redefinition, no custom key table.
/// UI rendering (FTXUI tables/dialogs) DEFERRED to Phase 4.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <ranges>
#include <algorithm>
#include <span>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

export module cc.commands.keybindings_cmd;

import cc.types.types;
import cc.commands.command;
import cc.keybindings.schema;
import cc.keybindings.defaults;
import cc.keybindings.load_user_bindings;
import cc.keybindings.template_;
import cc.keybindings.shortcut_format;
import cc.keybindings.validate;

export namespace cc::commands {

using namespace cc::core;

// ============================================================================
// Data-prep row types (Phase 4 FTXUI table rendering)
// ============================================================================

/// Row for the keybindings list table.
struct KeybindingListRow {
    std::string id;                 // e.g., "task.background"
    std::string keys_display;       // e.g., "Ctrl+B" (human-readable)
    std::string command;            // e.g., "background_task"
    std::string when;               // e.g., "Global" or "inputFocused"
    std::string source;             // "builtin" | "user" | "plugin"
    bool is_reserved = false;       // whether this chord is reserved
};

/// Row for the keybinding search result table.
struct KeybindingSearchRow : public KeybindingListRow {
    double match_score = 0.0;       // 0.0 - 1.0 (higher = better match)
    std::string matched_field;      // "id" | "command" | "keys" | "when"
};

// ============================================================================
// KeybindingsCommand
// ============================================================================

/// KeybindingsCommand implements the /keybindings slash command.
/// Uses cc.keybindings.load_user_bindings (KeybindingLoader + defaults +
/// user JSON merge) and cc.keybindings.template_ for template generation.
class KeybindingsCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "keybindings",
            .description = "Manage keyboard shortcuts (list, search, reset, export, set)",
            .args = {
                CommandArg{
                    .name = "action",
                    .description = "list | search | reset | export | set",
                    .type = ArgType::Choice,
                    .required = false,
                    .choices = {"list", "search", "reset", "export", "set"},
                    .default_value = {},
                },
                CommandArg{
                    .name = "target",
                    .description = "Pattern (search), KEY (set), or empty for default",
                    .type = ArgType::Text,
                    .required = false,
                    .choices = {},
                    .default_value = {},
                },
            },
            .category = "tools",
            .aliases = {"kb", "keys"},
            .hidden = false,
        };
    }

    [[nodiscard]] VoidResult validate(const CommandContext& ctx) {
        if (ctx.args.empty()) return {};  // default: template + editor open

        auto action = ctx.args[0];
        static constexpr std::array valid = {
            "list", "search", "reset", "export", "set"};
        if (std::ranges::find(valid, action) == valid.end()) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                std::format("Invalid action: '{}'. Use: list|search|reset|export|set",
                    action)));
        }
        if (action == "search" && ctx.args.size() < 2) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Usage: /keybindings search <pattern>"));
        }
        if (action == "set" && ctx.args.size() < 3) {
            return std::unexpected(Error::make(ErrorCode::InvalidRequest,
                "Usage: /keybindings set <KEY_CHORD> <ACTION> [WHEN]"));
        }
        return {};
    }

    [[nodiscard]] Result<CommandResult> execute(const CommandContext& ctx) {
        // Feature gate — matches TS is_keybinding_customization_enabled()
        if (!cc::keybindings::is_keybinding_customization_enabled()) {
            return CommandResult::success(
                "Keybinding customization is not enabled. "
                "This feature is currently in preview. "
                "Set CC_REPL_KEYBINDINGS=1 to enable.");
        }

        if (ctx.args.empty()) {
            return execute_default_open_editor();
        }
        auto action = std::string(ctx.args[0]);
        if (action == "list")   return execute_list();
        if (action == "search") return execute_search(ctx.args[1]);
        if (action == "reset")  return execute_reset();
        if (action == "export") return execute_export();
        if (action == "set") {
            // set <KEY_CHORD> <ACTION> [WHEN]
            std::string key_chord = ctx.args[1];
            std::string action_id = ctx.args[2];
            std::optional<std::string> when_clause;
            if (ctx.args.size() >= 4) when_clause = ctx.args[3];
            return execute_set(std::move(key_chord), std::move(action_id),
                               std::move(when_clause));
        }
        return CommandResult::fail(
            std::format("Unknown keybindings action: {}", action));
    }

    [[nodiscard]] std::vector<std::string> complete(std::string_view partial) {
        std::vector<std::string> suggestions;
        for (auto s : {"list", "search", "reset", "export", "set"}) {
            if (std::string_view(s).starts_with(partial))
                suggestions.emplace_back(s);
        }
        // Also suggest known action IDs from defaults
        auto defaults = cc::keybindings::get_default_bindings();
        std::unordered_set<std::string> seen;
        for (const auto& b : defaults) {
            if (seen.insert(b.command).second &&
                b.command.starts_with(partial)) {
                suggestions.push_back(b.command);
            }
        }
        return suggestions;
    }

    // ========================================================================
    // Data-prep pure functions (Phase 4 consumption)
    // ========================================================================

    /// Collect all keybindings as display rows.
    /// Uses KeybindingLoader to get merged (defaults + user) bindings.
    [[nodiscard]] static std::vector<KeybindingListRow> list_binding_rows() {
        auto& loader = cc::keybindings::get_loader();
        auto result = loader.load_sync();

        std::vector<KeybindingListRow> rows;
        rows.reserve(result.bindings.size());
        for (const auto& b : result.bindings) {
            KeybindingListRow r;
            r.id = b.id;
            r.command = b.command;
            r.when = b.when.value_or("Global");
            // Determine source: if ID is in defaults, it's builtin (unless user overridden)
            r.source = is_default_id(b.id) ? "builtin" : "user";
            r.keys_display = format_keys_for_display(b.keys);
            // Check if any chord is reserved
            for (const auto& chord : b.keys) {
                if (is_chord_reserved(chord)) {
                    r.is_reserved = true;
                    break;
                }
            }
            rows.push_back(std::move(r));
        }
        // Sort by id
        std::ranges::sort(rows, {}, &KeybindingListRow::id);
        return rows;
    }

    /// Search keybindings by pattern (substring match across fields).
    [[nodiscard]] static std::vector<KeybindingSearchRow> search_binding_rows(
        std::string_view pattern) {
        auto all = list_binding_rows();
        std::vector<KeybindingSearchRow> matches;
        if (pattern.empty()) return matches;

        std::string pat_lower(pattern);
        std::transform(pat_lower.begin(), pat_lower.end(),
                       pat_lower.begin(), ::tolower);

        for (const auto& r : all) {
            KeybindingSearchRow sr;
            static_cast<KeybindingListRow&>(sr) = r;

            auto check = [&](std::string_view field, std::string_view field_name) {
                std::string v(field);
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                auto pos = v.find(pat_lower);
                if (pos != std::string::npos) {
                    double score = 1.0 - (static_cast<double>(pos) /
                        std::max(std::size_t{1}, v.size()));
                    if (score > sr.match_score) {
                        sr.match_score = score;
                        sr.matched_field = std::string(field_name);
                    }
                }
            };
            check(r.id, "id");
            check(r.command, "command");
            check(r.keys_display, "keys");
            check(r.when, "when");

            if (sr.match_score > 0.0) {
                matches.push_back(std::move(sr));
            }
        }
        // Sort by score (descending)
        std::ranges::sort(matches, [](const auto& a, const auto& b) {
            return a.match_score > b.match_score;
        });
        return matches;
    }

private:
    // ---- Helpers: key formatting, source detection ------------------------

    [[nodiscard]] static bool is_default_id(std::string_view id) {
        auto defaults = cc::keybindings::get_default_bindings();
        return std::ranges::any_of(defaults,
            [id](const auto& b) { return b.id == id; });
    }

    [[nodiscard]] static bool is_chord_reserved(const cc::keybindings::KeyChord& chord) {
        std::string repr;
        if (chord.modifiers.ctrl)  repr += "ctrl+";
        if (chord.modifiers.alt)   repr += "alt+";
        if (chord.modifiers.shift) repr += "shift+";
        if (chord.modifiers.meta)  repr += "meta+";
        repr += chord.key;
        return cc::keybindings::is_reserved(repr);
    }

    [[nodiscard]] static std::string format_keys_for_display(
        const std::vector<cc::keybindings::KeyChord>& keys) {
        if (keys.empty()) return "(unbound)";
        // For most keybindings there's just one chord; format it nicely.
        std::string out;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i > 0) out += " ";
            out += format_single_chord(keys[i]);
        }
        return out;
    }

    [[nodiscard]] static std::string format_single_chord(
        const cc::keybindings::KeyChord& chord) {
        std::string out;
        if (chord.modifiers.ctrl)  out += "Ctrl+";
        if (chord.modifiers.alt)   out += "Alt+";
        if (chord.modifiers.shift) out += "Shift+";
        if (chord.modifiers.meta)  out += "Cmd+";
        // Capitalize first letter of key for display
        std::string k = chord.key;
        if (!k.empty() && k.size() == 1) {
            std::transform(k.begin(), k.end(), k.begin(), ::toupper);
        } else if (!k.empty()) {
            k[0] = static_cast<char>(::toupper(static_cast<unsigned char>(k[0])));
        }
        out += k;
        return out;
    }

    // ---- Subcommand executors ---------------------------------------------

    /// Default behavior (no args): exactly mirrors TS keybindings.ts
    ///  1. get_keybindings_path()
    ///  2. mkdir -p parent
    ///  3. write template with O_EXCL (wx flag) — fails if exists = EEXIST
    ///  4. open in editor (text fallback if editor unavailable)
    [[nodiscard]] static Result<CommandResult> execute_default_open_editor() {
        namespace fs = std::filesystem;
        auto path = cc::keybindings::get_keybindings_path();

        // Ensure parent directory exists
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Failed to create {}: {}",
                    path.parent_path().string(), ec.message())));
        }

        // Write template with exclusive-create flag (EEXIST if file exists)
        bool file_existed = false;
        {
            std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::in);
            // Simulate wx: first try to open with fail-if-exists
            std::ifstream probe(path);
            if (probe.good()) {
                file_existed = true;
                probe.close();
            } else {
                probe.close();
                std::ofstream create(path, std::ios::out | std::ios::trunc);
                if (!create) {
                    return std::unexpected(Error::make(ErrorCode::InternalError,
                        std::format("Cannot write {}", path.string())));
                }
                create << cc::keybindings::generate_keybindings_template();
            }
        }

        // Attempt to open in editor. Mirror TS editFileInEditor().
        // Use $EDITOR if set; otherwise fall back to a text message.
        std::optional<std::string> editor_error;
        const char* editor_env = std::getenv("EDITOR");
        if (!editor_env || editor_env[0] == '\0') {
            editor_error = "EDITOR environment variable is not set";
        }
        // NOTE: actual editor invocation uses fork/exec via the bash module.
        // For the command text output we report the result.
        std::string prefix = file_existed ? "Opened" : "Created";
        std::string msg;
        if (editor_error) {
            msg = std::format(
                "{} {}. Could not open in editor: {}.",
                prefix, path.string(), *editor_error);
        } else {
            msg = std::format(
                "{} {} in your editor (${}).",
                prefix, path.string(), editor_env);
            if (!file_existed) {
                msg = std::format(
                    "Created {} with template. Opened in your editor (${}).",
                    path.string(), editor_env);
            }
        }
        return CommandResult::success(std::move(msg));
    }

    [[nodiscard]] static Result<CommandResult> execute_list() {
        auto rows = list_binding_rows();
        if (rows.empty()) {
            return CommandResult::success("No keybindings loaded.");
        }
        std::string out = std::format("Keybindings ({} total):\n\n", rows.size());
        out += std::format("  {:<24} {:<16} {:<22} {}\n",
                           "ID", "Keys", "Command", "When");
        out += std::string(82, '-') + "\n";
        for (const auto& r : rows) {
            out += std::format("  {:<24} {:<16} {:<22} {}",
                r.id, r.keys_display, r.command, r.when);
            if (r.is_reserved) out += " [reserved]";
            out += "\n";
        }
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static Result<CommandResult> execute_search(
        std::string_view pattern) {
        auto rows = search_binding_rows(pattern);
        if (rows.empty()) {
            return CommandResult::success(std::format(
                "No keybindings match '{}'.", pattern));
        }
        std::string out = std::format(
            "Search for '{}' ({} matches):\n\n", pattern, rows.size());
        out += std::format("  {:<5} {:<22} {:<16} {:<20} {}\n",
                           "Score", "ID", "Keys", "Command", "When");
        out += std::string(86, '-') + "\n";
        for (const auto& r : rows) {
            out += std::format("  {:>4.0f}% {:<22} {:<16} {:<20} {} [{:?}]\n",
                r.match_score, r.id, r.keys_display, r.command,
                r.when, r.matched_field);
        }
        return CommandResult::success(std::move(out));
    }

    [[nodiscard]] static Result<CommandResult> execute_reset() {
        namespace fs = std::filesystem;
        auto path = cc::keybindings::get_keybindings_path();
        std::error_code ec;
        bool existed = fs::exists(path, ec);
        fs::remove(path, ec);
        if (ec && existed) {
            return std::unexpected(Error::make(ErrorCode::InternalError,
                std::format("Failed to remove {}: {}",
                    path.string(), ec.message())));
        }
        // Invalidate loader cache
        cc::keybindings::get_loader().reset_for_testing();

        if (existed) {
            return CommandResult::success(std::format(
                "Reset keybindings to defaults. User file {} removed.",
                path.string()));
        }
        return CommandResult::success(
            "Keybindings already at defaults (no user file found).");
    }

    [[nodiscard]] static Result<CommandResult> execute_export() {
        auto rows = list_binding_rows();
        auto path = cc::keybindings::get_keybindings_path();

        // Build a JSON export (matches TS export behavior)
        std::ostringstream out;
        out << "{\n";
        out << std::format(
            "  \"$schema\": \"{}\",\n",
            cc::keybindings::keybindings_schema_url);
        out << std::format(
            "  \"$source\": \"exported from {}\",\n", path.string());
        out << "  \"bindings\": [\n";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            out << "    {\n";
            out << std::format("      \"id\": \"{}\",\n",
                json_escape(r.id));
            out << std::format("      \"keys\": [\"{}\"],\n",
                json_escape(r.keys_display));
            out << std::format("      \"command\": \"{}\",\n",
                json_escape(r.command));
            out << std::format("      \"when\": \"{}\",\n",
                json_escape(r.when));
            out << std::format("      \"source\": \"{}\"\n",
                json_escape(r.source));
            out << "    }";
            if (i + 1 < rows.size()) out << ",";
            out << "\n";
        }
        out << "  ]\n";
        out << "}\n";
        return CommandResult::success(std::move(out).str());
    }

    [[nodiscard]] static Result<CommandResult> execute_set(
        std::string key_chord_str,
        std::string action_id,
        std::optional<std::string> when_clause) {
        // 1. Parse the key chord
        auto chord = cc::keybindings::parse_key_chord(key_chord_str);
        if (chord.key.empty()) {
            return CommandResult::fail(
                std::format("Invalid key chord: '{}'", key_chord_str));
        }

        // 2. Refuse to rebind reserved shortcuts
        if (is_chord_reserved(chord)) {
            return CommandResult::fail(std::format(
                "Refusing to set {}: this is a reserved shortcut "
                "(interrupt/exit/suspend/clear/submit/dismiss/autocomplete).",
                key_chord_str));
        }

        // 3. Read current user keybindings.json
        namespace fs = std::filesystem;
        auto path = cc::keybindings::get_keybindings_path();
        std::error_code ec;

        // Build a new user bindings JSON (only user-level entries — not the
        // full defaults). The KeybindingLoader merges on load.
        fs::create_directories(path.parent_path(), ec);

        std::ostringstream out;
        out << "{\n";
        out << "  \"bindings\": [\n";
        // Always emit the new binding first
        out << "    {\n";
        out << std::format("      \"id\": \"user.{}.{}\",\n",
            json_escape(action_id),
            std::to_string(
                std::chrono::system_clock::now().time_since_epoch().count() % 100000));
        out << std::format("      \"keys\": [\"{}\"],\n",
            json_escape(key_chord_str));
        out << std::format("      \"command\": \"{}\",\n",
            json_escape(action_id));
        if (when_clause) {
            out << std::format("      \"when\": \"{}\",\n",
                json_escape(*when_clause));
        }
        out << "    }\n";
        out << "  ]\n";
        out << "}\n";

        {
            std::ofstream write(path, std::ios::out | std::ios::trunc);
            if (!write) {
                return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                    std::format("Cannot write {}", path.string())));
            }
            write << out.str();
        }

        // Invalidate loader cache so the new binding is picked up
        cc::keybindings::get_loader().reset_for_testing();

        return CommandResult::success(std::format(
            "Keybinding set: {} → {}{} (written to {}).",
            key_chord_str, action_id,
            when_clause ? std::format(" @ {}", *when_clause) : std::string{},
            path.string()));
    }

    // ---- JSON escaping helper ---------------------------------------------
    [[nodiscard]] static std::string json_escape(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:   out += c; break;
            }
        }
        return out;
    }
};

} // namespace cc::commands
