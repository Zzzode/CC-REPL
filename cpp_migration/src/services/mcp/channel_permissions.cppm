// ============================================================================
/// @file channel_permissions.cppm
/// @brief MCP channel permission relay — prompts over channels (Telegram,
///        iMessage, Discord) that race against local UI / bridge / classifier.
///
/// TS REF: src/services/mcp/channelPermissions.ts (240 lines)
///
/// When CC hits a permission dialog, it ALSO sends the prompt via active
/// channels and races the reply against local UI / bridge / hooks / classifier.
/// First resolver wins via claim().
///
/// Inbound is a structured event: the server parses the user's "yes tbxkq"
/// reply and emits notifications/claude/channel/permission with
/// {request_id, behavior}. CC never sees the reply as text — approval
/// requires the server to deliberately emit that specific event, not just
/// relay content. Servers opt in by declaring
/// capabilities.experimental['claude/channel/permission'].
// ============================================================================
module;
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <yyjson.h>

export module cc.services.mcp.channel_permissions;

import cc.services.mcp.types;
import cc.utils.json;

export namespace cc::services::mcp {

// ============================================================================
// Constants
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:75
// Reply format spec for channel servers to implement:
//   /^\s*(y|yes|n|no)\s+([a-km-z]{5})\s*$/i
// 5 lowercase letters, no 'l' (looks like 1/I). Case-insensitive.
// No bare yes/no (conversational). No prefix/suffix chatter.
constexpr std::string_view PERMISSION_REPLY_PATTERN =
    R"(^\s*(y|yes|n|no)\s+([a-km-z]{5})\s*$)";

// TS REF: src/services/mcp/channelPermissions.ts:78
// 25-letter alphabet: a-z minus 'l' (looks like 1/I). 25^5 ≈ 9.8M space.
constexpr std::string_view ID_ALPHABET = "abcdefghijkmnopqrstuvwxyz";

// TS REF: src/services/mcp/channelPermissions.ts:85-110
// Substring blocklist — 5 random letters can spell things. Non-exhaustive,
// covers the send-to-your-boss-by-accident tier.
constexpr std::array<std::string_view, 27> ID_AVOID_SUBSTRINGS = {
    "fuck",  "shit",  "cunt",  "cock",  "dick",  "twat",  "piss",
    "crap",  "bitch", "whore", "ass",   "tit",   "cum",   "fag",
    "dyke",  "nig",   "kike",  "rape",  "nazi",  "damn",  "poo",
    "pee",   "wank",  "anus",
};

// ============================================================================
// Types
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:40-44
// Behavior returned by a channel permission response.
enum class ChannelPermissionBehavior : std::uint8_t {
    Allow = 0,  // 'allow' in TS
    Deny  = 1,  // 'deny'  in TS
};

// TS REF: src/services/mcp/channelPermissions.ts:40-44
// Response from a channel server resolving a pending permission request.
struct ChannelPermissionResponse {
    ChannelPermissionBehavior behavior;     ///< Allow or Deny
    std::string               from_server;  ///< Which channel server replied
};

// ============================================================================
// ChannelPermissionCallbacks
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:46-61, 209-240
// Manages pending permission requests keyed by short request ID.
// Thread-safe: all methods lock the internal mutex.
//
// Lifetime: constructed once per session (same as TS replBridgePermissionCallbacks
// pattern — created in a hook, stable reference stored in AppState).
class ChannelPermissionCallbacks {
public:
    using Handler = std::function<void(const ChannelPermissionResponse&)>;

    ChannelPermissionCallbacks() = default;
    ~ChannelPermissionCallbacks() = default;
    ChannelPermissionCallbacks(const ChannelPermissionCallbacks&) = delete;
    ChannelPermissionCallbacks& operator=(const ChannelPermissionCallbacks&) = delete;

    // TS REF: src/services/mcp/channelPermissions.ts:48-51, 216-226
    // Register a resolver for a request ID. Returns unsubscribe function.
    // Lowercases the key so matching is case-insensitive.
    auto on_response(std::string_view request_id, Handler handler)
        -> std::function<void()>
    {
        std::string key = to_lower(request_id);
        std::lock_guard lock(mutex_);
        pending_[key] = std::move(handler);
        // Return unsubscribe closure
        return [this, k = std::move(key)]() {
            std::lock_guard lock2(mutex_);
            pending_.erase(k);
        };
    }

    // TS REF: src/services/mcp/channelPermissions.ts:56-60, 228-238
    // Resolve a pending request from a structured channel event.
    // Returns true if the ID was pending (matched against the map).
    // Delete BEFORE calling — if resolver throws or re-enters, the entry
    // is already gone. Also handles duplicate events (second emission
    // falls through — server bug or network dup, ignore).
    auto resolve(std::string_view    request_id,
                 ChannelPermissionBehavior behavior,
                 std::string_view    from_server) -> bool
    {
        std::string key = to_lower(request_id);
        Handler resolver;
        {
            std::lock_guard lock(mutex_);
            auto it = pending_.find(key);
            if (it == pending_.end()) return false;
            // Extract before erase so we can call after unlock
            resolver = std::move(it->second);
            pending_.erase(it);
        }
        // Call outside the lock to avoid deadlock if resolver re-enters
        resolver(ChannelPermissionResponse{
            .behavior    = behavior,
            .from_server = std::string(from_server),
        });
        return true;
    }

    // TS REF: src/services/mcp/channelPermissions.ts:209-213
    // Number of pending requests (for diagnostics).
    auto pending_count() const -> std::size_t {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

private:
    // Helper: lowercase a string_view into a new string.
    static auto to_lower(std::string_view sv) -> std::string {
        std::string result(sv);
        for (auto& c : result) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }

    mutable std::mutex                                mutex_;
    std::unordered_map<std::string, Handler>          pending_;
};

// ============================================================================
// Factory
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:209-240
// Factory for the callbacks object. The pending Map is closed over — NOT
// module-level, NOT in AppState. Same lifetime pattern as
// replBridgePermissionCallbacks: constructed once per session inside a hook,
// stable reference stored in AppState.
inline auto create_channel_permission_callbacks()
    -> std::shared_ptr<ChannelPermissionCallbacks>
{
    return std::make_shared<ChannelPermissionCallbacks>();
}

// ============================================================================
// Feature gate
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:36-38
// GrowthBook runtime gate — separate from the channels gate (tengu_harbor)
// so channels can ship without permission-relay riding along.
// In CPP: stub returning false (no GrowthBook integration yet).
// Feature-flag surface can wire this up later.
inline auto is_channel_permission_relay_enabled() -> bool {
    return false;  // TODO: wire to feature flag system when available
}

// ============================================================================
// Short request ID generation
// ============================================================================

namespace detail {

// TS REF: src/services/mcp/channelPermissions.ts:112-128
// FNV-1a hash → uint32. Not crypto, just a stable short letters-only ID.
// 32 bits / log2(25) ≈ 6.9 letters of entropy; taking 5 wastes a little,
// plenty for this use case.
inline auto fnv1a_hash(std::string_view input) -> std::uint32_t {
    std::uint32_t h = 0x811c9dc5u;  // FNV-1a 32-bit offset basis
    for (unsigned char c : input) {
        h ^= static_cast<std::uint32_t>(c);
        h *= 0x01000193u;  // FNV-1a 32-bit prime
    }
    return h;
}

// TS REF: src/services/mcp/channelPermissions.ts:122-128
// Base-25 encode a uint32 hash into 5 letters from ID_ALPHABET.
inline auto base25_encode_5(std::uint32_t hash) -> std::string {
    std::string s;
    s.reserve(5);
    for (int i = 0; i < 5; ++i) {
        s += ID_ALPHABET[hash % 25];
        hash = hash / 25;
    }
    return s;
}

// TS REF: src/services/mcp/channelPermissions.ts:85-110, 144-148
// Check if the ID contains any blocklisted substring.
inline auto contains_blocked_substring(std::string_view id) -> bool {
    return std::ranges::any_of(ID_AVOID_SUBSTRINGS, [&](std::string_view bad) {
        return id.find(bad) != std::string_view::npos;
    });
}

} // namespace detail

// TS REF: src/services/mcp/channelPermissions.ts:140-152
// Short ID from a toolUseID. 5 letters from a 25-char alphabet (a-z minus
// 'l' — looks like 1/I in many fonts). 25^5 ≈ 9.8M space, birthday
// collision at 50% needs ~3K simultaneous pending prompts, absurd for a
// single interactive session. Letters-only so phone users don't switch
// keyboard modes. Re-hashes with a salt suffix if the result contains a
// blocklisted substring.
//
// toolUseIDs are `toolu_` + base64-ish; we hash rather than slice.
inline auto short_request_id(std::string_view tool_use_id) -> std::string {
    // Cap at 10 retries; (1/700)^10 is negligible.
    auto candidate = detail::base25_encode_5(detail::fnv1a_hash(tool_use_id));
    for (int salt = 0; salt < 10; ++salt) {
        if (!detail::contains_blocked_substring(candidate)) {
            return candidate;
        }
        std::string salted = std::string(tool_use_id) + ":" + std::to_string(salt);
        candidate = detail::base25_encode_5(detail::fnv1a_hash(salted));
    }
    return candidate;
}

// ============================================================================
// Truncate for preview
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:160-167
// Truncate tool input to a phone-sized JSON preview. 200 chars is roughly
// 3 lines on a narrow phone screen. Full input is in the local terminal
// dialog; the channel gets a summary so Write(5KB-file) doesn't flood your
// texts. Server decides whether/how to show it.
//
// In TS: jsonStringify(input) then truncate. In CPP: caller passes already-
// serialized JSON string (since we can't serialize arbitrary types like TS).
// If json_input is empty or unserializable marker, return fallback.
inline auto truncate_for_preview(std::string_view json_input) -> std::string {
    if (json_input.empty()) {
        return "(unserializable)";
    }
    constexpr std::size_t MAX_PREVIEW = 200;
    if (json_input.size() > MAX_PREVIEW) {
        std::string result(json_input.substr(0, MAX_PREVIEW));
        result += "…";  // ellipsis character (…)
        return result;
    }
    return std::string(json_input);
}

// ============================================================================
// Filter permission relay clients
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:177-194
// Filter MCP clients down to those that can relay permission prompts.
// Three conditions, ALL required:
//   1. Connected (state == Ready)
//   2. In the session's --channels allowlist
//   3. Declares BOTH capabilities: 'claude/channel' AND
//      'claude/channel/permission'
//
// The second capability is the server's explicit opt-in — a relay-only
// channel never becomes a permission surface by accident.
//
// Template: T must have .name (string), .state (ServerState), and
// .capabilities (ServerCapabilities with .experimental map).
template <typename T>
auto filter_permission_relay_clients(
    const std::vector<T>&                        clients,
    std::function<bool(std::string_view)>        is_in_allowlist)
    -> std::vector<T>
{
    std::vector<T> result;
    for (const auto& c : clients) {
        // Condition 1: connected / ready
        // TS REF: src/services/mcp/channelPermissions.ts:189
        //   c.type === 'connected'
        if (c.state != ServerState::Ready) continue;

        // Condition 2: in allowlist
        // TS REF: src/services/mcp/channelPermissions.ts:190
        //   isInAllowlist(c.name)
        if (!is_in_allowlist(c.name)) continue;

        // Condition 3: declares BOTH experimental capabilities
        // TS REF: src/services/mcp/channelPermissions.ts:191-192
        //   c.capabilities?.experimental?.['claude/channel'] !== undefined &&
        //   c.capabilities?.experimental?.['claude/channel/permission'] !== undefined
        const auto& exp = c.capabilities.experimental;
        bool has_channel = exp.contains("claude/channel");
        bool has_permission = exp.contains("claude/channel/permission");
        if (!has_channel || !has_permission) continue;

        result.push_back(c);
    }
    return result;
}

// ============================================================================
// Parse permission reply (utility)
// ============================================================================

// TS REF: src/services/mcp/channelPermissions.ts:75
// Parse a "yes tbxkq" / "no tbxkq" style reply against PERMISSION_REPLY_RE.
// Returns the request_id if matched, plus the behavior.
// Returns nullopt if the reply doesn't match the pattern.
//
// NOTE: In production, the SERVER parses the reply and emits the structured
// event — CC never regex-matches text. This utility is for testing and
// standalone tooling. Exported so plugins can use the exact same logic.
struct ParsedPermissionReply {
    std::string               request_id;
    ChannelPermissionBehavior behavior;
};

inline auto parse_permission_reply(std::string_view reply)
    -> std::optional<ParsedPermissionReply>
{
    // Manual implementation of /^\s*(y|yes|n|no)\s+([a-km-z]{5})\s*$/i
    // Skip leading whitespace
    std::size_t pos = 0;
    while (pos < reply.size() && std::isspace(static_cast<unsigned char>(reply[pos]))) {
        ++pos;
    }
    if (pos >= reply.size()) return std::nullopt;

    // Parse y/yes/n/no (case-insensitive)
    ChannelPermissionBehavior behavior;
    char first = static_cast<char>(std::tolower(static_cast<unsigned char>(reply[pos])));
    if (first == 'y') {
        behavior = ChannelPermissionBehavior::Allow;
        ++pos;
        // Optional "es" for "yes"
        if (pos < reply.size() && std::tolower(static_cast<unsigned char>(reply[pos])) == 'e') ++pos;
        if (pos < reply.size() && std::tolower(static_cast<unsigned char>(reply[pos])) == 's') ++pos;
    } else if (first == 'n') {
        behavior = ChannelPermissionBehavior::Deny;
        ++pos;
        // Optional "o" for "no"
        if (pos < reply.size() && std::tolower(static_cast<unsigned char>(reply[pos])) == 'o') ++pos;
    } else {
        return std::nullopt;
    }

    // Require whitespace separator
    if (pos >= reply.size() || !std::isspace(static_cast<unsigned char>(reply[pos]))) {
        return std::nullopt;
    }
    while (pos < reply.size() && std::isspace(static_cast<unsigned char>(reply[pos]))) {
        ++pos;
    }

    // Parse 5-letter ID: [a-km-z]{5} (no 'l')
    if (pos + 5 > reply.size()) return std::nullopt;
    std::string request_id;
    request_id.reserve(5);
    for (int i = 0; i < 5; ++i) {
        char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(reply[pos + i])));
        // Must be a-z and not 'l'
        if (ch < 'a' || ch > 'z' || ch == 'l') return std::nullopt;
        request_id += ch;
    }
    pos += 5;

    // Skip trailing whitespace
    while (pos < reply.size() && std::isspace(static_cast<unsigned char>(reply[pos]))) {
        ++pos;
    }

    // Must be at end of string
    if (pos != reply.size()) return std::nullopt;

    return ParsedPermissionReply{
        .request_id = std::move(request_id),
        .behavior   = behavior,
    };
}

// ============================================================================
// Channel Permission Store — persistent rules for MCP tool permissions
// ============================================================================
//
// TS REF: src/services/mcp/channelPermissions.ts (240 lines)
// NOTE: The TS file implements the channel *relay* system (sending prompts
// over Telegram/Discord). The permission *store* — persistent rules about
// which MCP servers/tools are Allowed/Denied/Prompt — is a CPP-side
// extension designed to match the UX described in the MCP security dialog
// (ui/mcp/mcp_security_dialog.cppm) and the --allowed-tools CLI surface.
//
// Rules are stored in ~/.cc-repl/mcp-channel-permissions.json with a
// most-specific-wins resolution (Tool > Server > Global > default Prompt).

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

// TS REF: conceptual — maps to "allow/deny/prompt" in the permission dialog
// The effective permission for a tool call: allow it silently, deny it
// silently, or show a permission prompt to the user.
enum class ChannelPermission : std::uint8_t {
    Allowed = 0,  // Silent allow — no dialog shown
    Denied  = 1,  // Silent deny — tool call rejected
    Prompt  = 2,  // Show permission dialog (default for unknown tools)
};

// TS REF: conceptual — scope granularity for permission rules
enum class ChannelPermissionScope : std::uint8_t {
    Global = 0,  // Applies to all servers and all tools
    Server = 1,  // Applies to all tools on a specific server
    Tool   = 2,  // Applies to a specific tool on a specific server
};

// ---------------------------------------------------------------------------
// ChannelPermissionRule
// ---------------------------------------------------------------------------

// TS REF: conceptual — a single persisted permission rule
// A rule associates a (scope, server_name?, tool_name?) tuple with a
// permission decision. Optional fields are empty strings when not set.
struct ChannelPermissionRule {
    ChannelPermissionScope scope       = ChannelPermissionScope::Global;
    std::string            server_name;  // Set when scope >= Server
    std::string            tool_name;    // Set when scope == Tool
    ChannelPermission      permission   = ChannelPermission::Prompt;
};

// ---------------------------------------------------------------------------
// ChannelPermissionStore
// ---------------------------------------------------------------------------

// TS REF: conceptual — persistent permission rule store
// Manages ChannelPermissionRules with JSON persistence and most-specific-wins
// resolution. Thread-safe: all public methods lock the internal mutex.
//
// File: ~/.cc-repl/mcp-channel-permissions.json
// Schema:
//   {
//     "version": 1,
//     "rules": [
//       { "scope": "global",                    "permission": "prompt" },
//       { "scope": "server", "server_name": "…", "permission": "allowed" },
//       { "scope": "tool",   "server_name": "…", "tool_name": "…",
//         "permission": "denied" }
//     ]
//   }
class ChannelPermissionStore {
public:
    // ------------------------------------------------------------------
    // Construction / lifetime
    // ------------------------------------------------------------------

    ChannelPermissionStore() = default;
    ~ChannelPermissionStore() = default;
    ChannelPermissionStore(const ChannelPermissionStore&) = delete;
    ChannelPermissionStore& operator=(const ChannelPermissionStore&) = delete;

    // ------------------------------------------------------------------
    // Persistence
    // ------------------------------------------------------------------

    // Load rules from the JSON file. Creates the file if it doesn't exist.
    // Safe to call multiple times — replaces in-memory rules with disk state.
    void load() {
        std::lock_guard lock(mutex_);
        load_locked();
    }

    // Save current rules to the JSON file (pretty-printed).
    void save() const {
        std::lock_guard lock(mutex_);
        save_locked();
    }

    // ------------------------------------------------------------------
    // Rule management
    // ------------------------------------------------------------------

    // TS REF: conceptual — checkPermission(server, tool)
    // Resolve the effective permission for a tool on a server using
    // most-specific-wins: Tool > Server > Global > default Prompt.
    [[nodiscard]] ChannelPermission check_permission(
        std::string_view server_name,
        std::string_view tool_name) const
    {
        std::lock_guard lock(mutex_);
        return resolve_permission_locked(server_name, tool_name);
    }

    // TS REF: conceptual — getEffectivePermission(server, tool)
    // Alias for check_permission — matches the TS naming convention.
    [[nodiscard]] ChannelPermission get_effective_permission(
        std::string_view server_name,
        std::string_view tool_name) const
    {
        return check_permission(server_name, tool_name);
    }

    // TS REF: conceptual — setPermission(rule)
    // Add or replace a rule. Rules are matched by (scope, server, tool)
    // identity — setting the same identity overwrites the permission.
    // Triggers an implicit save().
    void set_permission(const ChannelPermissionRule& rule) {
        std::lock_guard lock(mutex_);
        upsert_rule_locked(rule);
        save_locked();
    }

    // Remove a rule by identity. Returns true if a rule was removed.
    // Triggers an implicit save() on success.
    bool remove_rule(ChannelPermissionScope scope,
                     std::string_view server_name,
                     std::string_view tool_name)
    {
        std::lock_guard lock(mutex_);
        auto it = find_rule_locked(scope, server_name, tool_name);
        if (it == rules_.end()) return false;
        rules_.erase(it);
        save_locked();
        return true;
    }

    // Return all rules (for UI display / debugging).
    [[nodiscard]] std::vector<ChannelPermissionRule> get_all_rules() const {
        std::lock_guard lock(mutex_);
        return rules_;
    }

    // Number of stored rules (for diagnostics).
    [[nodiscard]] std::size_t rule_count() const {
        std::lock_guard lock(mutex_);
        return rules_.size();
    }

    // ------------------------------------------------------------------
    // Convenience factories
    // ------------------------------------------------------------------

    // Create a Global-scope rule.
    static ChannelPermissionRule make_global_rule(ChannelPermission perm) {
        return ChannelPermissionRule{
            .scope       = ChannelPermissionScope::Global,
            .server_name = {},
            .tool_name   = {},
            .permission  = perm,
        };
    }

    // Create a Server-scope rule.
    static ChannelPermissionRule make_server_rule(
        std::string_view server, ChannelPermission perm)
    {
        return ChannelPermissionRule{
            .scope       = ChannelPermissionScope::Server,
            .server_name = std::string(server),
            .tool_name   = {},
            .permission  = perm,
        };
    }

    // Create a Tool-scope rule.
    static ChannelPermissionRule make_tool_rule(
        std::string_view server, std::string_view tool, ChannelPermission perm)
    {
        return ChannelPermissionRule{
            .scope       = ChannelPermissionScope::Tool,
            .server_name = std::string(server),
            .tool_name   = std::string(tool),
            .permission  = perm,
        };
    }

    // ------------------------------------------------------------------
    // File path helper
    // ------------------------------------------------------------------

    // TS REF: ~/.cc-repl/mcp-channel-permissions.json
    // Resolve the path to the permissions JSON file.
    [[nodiscard]] static std::filesystem::path file_path() {
        namespace fs = std::filesystem;
        if (const char* home = std::getenv("HOME")) {
            return fs::path(home) / ".cc-repl" / "mcp-channel-permissions.json";
        }
        return fs::path(".cc-repl") / "mcp-channel-permissions.json";
    }

private:
    // ------------------------------------------------------------------
    // Internal helpers (caller must hold mutex_)
    // ------------------------------------------------------------------

    void load_locked() {
        namespace fs = std::filesystem;
        auto path = file_path();

        // Ensure parent directory exists.
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);

        // If file doesn't exist yet, start with empty rules + save defaults.
        if (!fs::exists(path)) {
            rules_.clear();
            save_locked();
            return;
        }

        // Read file contents.
        std::ifstream file(path);
        if (!file.is_open()) return;
        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();
        if (content.empty()) return;

        // Parse with yyjson.
        yyjson_doc* doc = yyjson_read(content.data(), content.size(), 0);
        if (!doc) return;
        yyjson_val* root = yyjson_doc_get_root(doc);
        if (!yyjson_is_obj(root)) {
            yyjson_doc_free(doc);
            return;
        }

        // Check version (forward-compatible — ignore unknown fields).
        // int64_t version = 0;
        // yyjson_val* ver = yyjson_obj_get(root, "version");
        // if (ver && yyjson_is_int(ver)) version = yyjson_get_sint(ver);

        // Parse rules array.
        rules_.clear();
        yyjson_val* rules_arr = yyjson_obj_get(root, "rules");
        if (rules_arr && yyjson_is_arr(rules_arr)) {
            yyjson_arr_iter iter;
            yyjson_arr_iter_init(rules_arr, &iter);
            yyjson_val* item;
            while ((item = yyjson_arr_iter_next(&iter)) != nullptr) {
                if (!yyjson_is_obj(item)) continue;
                auto rule = parse_rule_json(item);
                if (rule) rules_.push_back(std::move(*rule));
            }
        }

        yyjson_doc_free(doc);
    }

    void save_locked() const {
        namespace fs = std::filesystem;
        auto path = file_path();

        // Ensure parent directory exists.
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) return;

        // Build JSON document.
        yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
        if (!doc) return;

        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        // Version field.
        yyjson_mut_obj_add_int(doc, root, "version", 1);

        // Rules array.
        yyjson_mut_val* rules_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "rules", rules_arr);

        for (const auto& rule : rules_) {
            yyjson_mut_val* obj = yyjson_mut_obj(doc);

            // scope
            yyjson_mut_obj_add_str(doc, obj, "scope",
                scope_to_string(rule.scope).data());

            // server_name (only for Server / Tool scope)
            if (rule.scope >= ChannelPermissionScope::Server &&
                !rule.server_name.empty())
            {
                yyjson_mut_obj_add_str(doc, obj, "server_name",
                    rule.server_name.c_str());
            }

            // tool_name (only for Tool scope)
            if (rule.scope == ChannelPermissionScope::Tool &&
                !rule.tool_name.empty())
            {
                yyjson_mut_obj_add_str(doc, obj, "tool_name",
                    rule.tool_name.c_str());
            }

            // permission
            yyjson_mut_obj_add_str(doc, obj, "permission",
                permission_to_string(rule.permission).data());

            yyjson_mut_arr_append(rules_arr, obj);
        }

        // Write to file.
        size_t len = 0;
        char* json = yyjson_mut_write(doc,
            YYJSON_WRITE_PRETTY | YYJSON_WRITE_ESCAPE_SLASHES, &len);
        yyjson_mut_doc_free(doc);

        if (!json) return;

        std::ofstream out(path, std::ios::trunc);
        if (out.is_open()) {
            out.write(json, len);
        }
        free(json);
    }

    // Most-specific-wins resolution.
    ChannelPermission resolve_permission_locked(
        std::string_view server_name,
        std::string_view tool_name) const noexcept
    {
        ChannelPermission best_perm   = ChannelPermission::Prompt;  // default
        int               best_score  = -1;

        for (const auto& rule : rules_) {
            int score = rule_match_score(rule, server_name, tool_name);
            if (score > best_score) {
                best_score = score;
                best_perm  = rule.permission;
            }
        }

        return best_perm;
    }

    // Score how well a rule matches. Higher = more specific.
    //   Tool match   = 3
    //   Server match = 2
    //   Global match = 1
    //   No match     = -1
    static int rule_match_score(const ChannelPermissionRule& rule,
                                std::string_view server_name,
                                std::string_view tool_name) noexcept
    {
        switch (rule.scope) {
            case ChannelPermissionScope::Tool:
                if (rule.server_name == server_name &&
                    rule.tool_name == tool_name) return 3;
                return -1;

            case ChannelPermissionScope::Server:
                if (rule.server_name == server_name) return 2;
                return -1;

            case ChannelPermissionScope::Global:
                return 1;
        }
        return -1;
    }

    // Find a rule by exact identity. Returns rules_.end() if not found.
    std::vector<ChannelPermissionRule>::iterator find_rule_locked(
        ChannelPermissionScope scope,
        std::string_view server_name,
        std::string_view tool_name) noexcept
    {
        return std::find_if(rules_.begin(), rules_.end(),
            [&](const ChannelPermissionRule& r) {
                if (r.scope != scope) return false;
                switch (scope) {
                    case ChannelPermissionScope::Global:
                        return true;
                    case ChannelPermissionScope::Server:
                        return r.server_name == server_name;
                    case ChannelPermissionScope::Tool:
                        return r.server_name == server_name &&
                               r.tool_name == tool_name;
                }
                return false;
            });
    }

    // Insert or update a rule by identity.
    void upsert_rule_locked(const ChannelPermissionRule& rule) {
        auto it = find_rule_locked(rule.scope, rule.server_name, rule.tool_name);
        if (it != rules_.end()) {
            it->permission = rule.permission;
        } else {
            rules_.push_back(rule);
        }
    }

    // ------------------------------------------------------------------
    // JSON parsing helpers
    // ------------------------------------------------------------------

    static std::optional<ChannelPermissionRule> parse_rule_json(
        yyjson_val* obj)
    {
        if (!yyjson_is_obj(obj)) return std::nullopt;

        // scope (required)
        yyjson_val* scope_val = yyjson_obj_get(obj, "scope");
        if (!scope_val || !yyjson_is_str(scope_val)) return std::nullopt;
        std::string_view scope_str(yyjson_get_str(scope_val));
        ChannelPermissionScope scope = scope_from_string(scope_str);

        // permission (required)
        yyjson_val* perm_val = yyjson_obj_get(obj, "permission");
        if (!perm_val || !yyjson_is_str(perm_val)) return std::nullopt;
        std::string_view perm_str(yyjson_get_str(perm_val));
        ChannelPermission perm = permission_from_string(perm_str);

        // server_name (optional)
        std::string server_name;
        yyjson_val* server_val = yyjson_obj_get(obj, "server_name");
        if (server_val && yyjson_is_str(server_val)) {
            server_name = std::string(
                yyjson_get_str(server_val), yyjson_get_len(server_val));
        }

        // tool_name (optional)
        std::string tool_name;
        yyjson_val* tool_val = yyjson_obj_get(obj, "tool_name");
        if (tool_val && yyjson_is_str(tool_val)) {
            tool_name = std::string(
                yyjson_get_str(tool_val), yyjson_get_len(tool_val));
        }

        return ChannelPermissionRule{
            .scope       = scope,
            .server_name = std::move(server_name),
            .tool_name   = std::move(tool_name),
            .permission  = perm,
        };
    }

    // ------------------------------------------------------------------
    // String conversion helpers
    // ------------------------------------------------------------------

    static std::string_view scope_to_string(
        ChannelPermissionScope scope) noexcept
    {
        switch (scope) {
            case ChannelPermissionScope::Global: return "global";
            case ChannelPermissionScope::Server: return "server";
            case ChannelPermissionScope::Tool:   return "tool";
        }
        return "global";
    }

    static ChannelPermissionScope scope_from_string(
        std::string_view s) noexcept
    {
        if (s == "server") return ChannelPermissionScope::Server;
        if (s == "tool")   return ChannelPermissionScope::Tool;
        return ChannelPermissionScope::Global;
    }

    static std::string_view permission_to_string(
        ChannelPermission perm) noexcept
    {
        switch (perm) {
            case ChannelPermission::Allowed: return "allowed";
            case ChannelPermission::Denied:  return "denied";
            case ChannelPermission::Prompt:  return "prompt";
        }
        return "prompt";
    }

    static ChannelPermission permission_from_string(
        std::string_view s) noexcept
    {
        if (s == "allowed") return ChannelPermission::Allowed;
        if (s == "denied")  return ChannelPermission::Denied;
        return ChannelPermission::Prompt;
    }

    // ------------------------------------------------------------------
    // Data members
    // ------------------------------------------------------------------

    mutable std::mutex                    mutex_;
    std::vector<ChannelPermissionRule>    rules_;
};

// ============================================================================
// Factory
// ============================================================================

// TS REF: conceptual — create a loaded ChannelPermissionStore
// Creates a store and immediately loads rules from disk.
inline auto create_channel_permission_store()
    -> std::shared_ptr<ChannelPermissionStore>
{
    auto store = std::make_shared<ChannelPermissionStore>();
    store->load();
    return store;
}

// ============================================================================
// String conversion utilities (exported for UI / logging)
// ============================================================================

// Convert ChannelPermission to a human-readable string.
[[nodiscard]] inline auto channel_permission_to_string(
    ChannelPermission perm) -> std::string_view
{
    switch (perm) {
        case ChannelPermission::Allowed: return "Allowed";
        case ChannelPermission::Denied:  return "Denied";
        case ChannelPermission::Prompt:  return "Prompt";
    }
    return "Unknown";
}

// Convert ChannelPermissionScope to a human-readable string.
[[nodiscard]] inline auto channel_permission_scope_to_string(
    ChannelPermissionScope scope) -> std::string_view
{
    switch (scope) {
        case ChannelPermissionScope::Global: return "Global";
        case ChannelPermissionScope::Server: return "Server";
        case ChannelPermissionScope::Tool:   return "Tool";
    }
    return "Unknown";
}

} // namespace cc::services::mcp
