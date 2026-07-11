/**
 * @file channel_notification.cppm
 * @brief MCP channel notifications — lets an MCP server push user messages into
 *        the conversation. A "channel" (Discord, Slack, SMS, etc.) is just an
 *        MCP server that exposes tools for outbound messages and sends
 *        `notifications/claude/channel` notifications for inbound messages.
 *
 *        Faithful C++ port of src/services/mcp/channelNotification.ts (316 lines).
 *
 *        The notification handler wraps inbound content in a <channel> tag and
 *        enqueues it. The model sees where the message came from and decides
 *        which tool to reply with (the channel's MCP tool, SendUserMessage, or
 *        both).
 *
 *        feature('KAIROS') || feature('KAIROS_CHANNELS'). Runtime gate
 *        tengu_harbor. Requires claude.ai OAuth auth — API key users are
 *        blocked until console gets a channelsEnabled admin surface.
 *        Teams/Enterprise orgs must explicitly opt in via channelsEnabled: true
 *        in managed settings.
 */

module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module cc.services.mcp.channel_notification;

import cc.constants.xml;
import cc.services.mcp.types;
import cc.utils.json;
import cc.utils.plugin_identifier;

export namespace cc::services::mcp {

using namespace std::string_view_literals;
using cc::utils::json::JsonVal;
using cc::utils::plugin_identifier::ParsedPluginIdentifier;
using cc::utils::plugin_identifier::parse_plugin_identifier;

// =========================================================================
// JSON-RPC method constants
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:39
// Inbound: server → CC — a channel message (user typed something in Slack, etc.)
inline constexpr std::string_view CHANNEL_MESSAGE_METHOD =
    "notifications/claude/channel";

// TS REF: src/services/mcp/channelNotification.ts:62-63
// Inbound: server → CC — a structured permission reply (user approved/denied)
inline constexpr std::string_view CHANNEL_PERMISSION_METHOD =
    "notifications/claude/channel/permission";

// TS REF: src/services/mcp/channelNotification.ts:85-86
// Outbound: CC → server — ask the human for permission via the channel
inline constexpr std::string_view CHANNEL_PERMISSION_REQUEST_METHOD =
    "notifications/claude/channel/permission_request";

// =========================================================================
// Channel notification types
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:37-47
// Parsed params from a notifications/claude/channel notification
struct ChannelMessageParams {
    std::string content;
    // Opaque passthrough — thread_id, user, whatever the channel wants the
    // model to see. Rendered as attributes on the <channel> tag.
    std::optional<std::map<std::string, std::string>> meta;
};

// TS REF: src/services/mcp/channelNotification.ts:64-72
// Parsed params from a notifications/claude/channel/permission notification
struct ChannelPermissionParams {
    std::string request_id;
    // "allow" or "deny"
    std::string behavior;
};

// TS REF: src/services/mcp/channelNotification.ts:87-95
// Params CC sends in notifications/claude/channel/permission_request
struct ChannelPermissionRequestParams {
    std::string request_id;
    std::string tool_name;
    std::string description;
    /** JSON-stringified tool input, truncated to 200 chars with …. Full
     *  input is in the local terminal dialog; this is a phone-sized preview. */
    std::string input_preview;
};

// Kinds of channel-related notifications that can flow through the bus
enum class ChannelNotificationType {
    ServerStarted,       // server connected and initialized
    ServerStopped,       // server disconnected
    ToolListChanged,     // server's tools/list changed
    ResourceListChanged, // server's resources/list changed
    PromptListChanged,   // server's prompts/list changed
    ServerError,         // server health-check failure
    ChannelMessage,      // notifications/claude/channel inbound
    ChannelPermission,   // notifications/claude/channel/permission inbound
};

// Convert notification type to a readable string (for logging/debug)
[[nodiscard]] inline auto to_string(ChannelNotificationType type) -> std::string_view {
    switch (type) {
        case ChannelNotificationType::ServerStarted:       return "server_started";
        case ChannelNotificationType::ServerStopped:       return "server_stopped";
        case ChannelNotificationType::ToolListChanged:     return "tool_list_changed";
        case ChannelNotificationType::ResourceListChanged: return "resource_list_changed";
        case ChannelNotificationType::PromptListChanged:   return "prompt_list_changed";
        case ChannelNotificationType::ServerError:         return "server_error";
        case ChannelNotificationType::ChannelMessage:      return "channel_message";
        case ChannelNotificationType::ChannelPermission:   return "channel_permission";
    }
    return "unknown";
}

// A notification event flowing through the channel bus
struct ChannelNotification {
    ChannelNotificationType type;
    std::string server_name;
    // Wall-clock timestamp of when the notification was created
    std::chrono::system_clock::time_point timestamp;
    // Raw JSON payload — interpretation depends on type:
    //   ChannelMessage    → JSON of ChannelMessageParams
    //   ChannelPermission → JSON of ChannelPermissionParams
    //   ServerError       → error message string
    //   list-changed      → optional serialized new list
    std::string data_json;
};

// Callback signature for notification handlers
using ChannelNotificationHandler = std::function<void(const ChannelNotification&)>;

// =========================================================================
// Channel entry types (--channels flag parsing)
// =========================================================================

// TS REF: src/bootstrap/state.ts:37-39
// An entry from the user's --channels flag.
enum class ChannelEntryKind { Plugin, Server };

struct ChannelEntry {
    ChannelEntryKind kind;
    std::string name;
    std::optional<std::string> marketplace; // only for Plugin kind
    bool dev = false;                        // --dangerously-load-development-channels
};

// TS REF: src/services/mcp/channelAllowlist.ts:23-26
// An entry on the approved-channels allowlist (plugin-only schema).
struct ChannelAllowlistEntry {
    std::string marketplace;
    std::string plugin;
};

// Source of the effective allowlist
enum class AllowlistSource { Org, Ledger };

// =========================================================================
// Channel gate result
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:140-153
// Result of gating a server's channel-notification path.
enum class ChannelGateAction { Register, Skip };

enum class ChannelGateSkipKind {
    Capability,   // server did not declare claude/channel capability
    Disabled,     // channels feature killswitch off
    Auth,         // not OAuth-authenticated
    Policy,       // org policy blocks channels
    Session,      // server not in --channels list
    Marketplace,  // installed plugin marketplace doesn't match request
    Allowlist,    // server/plugin not on approved channels list
};

struct ChannelGateResult {
    ChannelGateAction action;
    // For Skip results: why it was skipped
    std::optional<ChannelGateSkipKind> skip_kind;
    // Human-readable reason for logging
    std::string reason;
};

// =========================================================================
// Safe meta key validation
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:104
// Meta keys become XML attribute NAMES — a crafted key like
// `x="" injected="y` would break out of the attribute structure. Only
// accept keys that look like plain identifiers. This is stricter than
// the XML spec (which allows `:`, `.`, `-`) but channel servers only
// send `chat_id`, `user`, `thread_ts`, `message_id` in practice.
[[nodiscard]] inline auto is_safe_meta_key(std::string_view key) -> bool {
    if (key.empty()) return false;
    unsigned char first = static_cast<unsigned char>(key[0]);
    if (!std::isalpha(first) && first != '_') return false;
    for (std::size_t i = 1; i < key.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(key[i]);
        if (!std::isalnum(ch) && ch != '_') return false;
    }
    return true;
}

// =========================================================================
// XML attribute escaping
// =========================================================================

// TS REF: src/utils/xml.ts (escapeXmlAttr)
// Escape a value for safe use inside an XML attribute (double-quoted).
[[nodiscard]] inline auto escape_xml_attr(std::string_view value) -> std::string {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&':  result += "&amp;"; break;
            case '<':  result += "&lt;"; break;
            case '>':  result += "&gt;"; break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default:   result.push_back(ch); break;
        }
    }
    return result;
}

// =========================================================================
// Channel message wrapping
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:106-116
// Wrap a channel message's content in a <channel> tag with source and meta
// attributes. Returns the wrapped XML string ready to enqueue.
[[nodiscard]] inline auto wrap_channel_message(
    std::string_view server_name,
    std::string_view content,
    const std::optional<std::map<std::string, std::string>>& meta = std::nullopt
) -> std::string {
    using cc::constants::xml::CHANNEL_TAG;

    std::string attrs;
    if (meta) {
        for (const auto& [key, value] : *meta) {
            if (is_safe_meta_key(key)) {
                attrs += std::format(" {}=\"{}\"", key, escape_xml_attr(value));
            }
        }
    }
    return std::format(
        "<{} source=\"{}\"{}>\n{}\n</{}>",
        CHANNEL_TAG,
        escape_xml_attr(server_name),
        attrs,
        content,
        CHANNEL_TAG
    );
}

// =========================================================================
// Channel entry matching
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:161-173
// Match a connected MCP server name against the user's parsed --channels
// entries. server-kind is exact match on bare name; plugin-kind matches on
// the second segment of plugin:X:Y. Returns the matching entry so callers
// can read its kind — that's the user's trust declaration, not inferred
// from runtime shape.
[[nodiscard]] inline auto find_channel_entry(
    std::string_view server_name,
    const std::vector<ChannelEntry>& channels
) -> std::optional<ChannelEntry> {
    // split unconditionally — for a bare name like 'slack', parts has size 1
    // and the plugin-kind branch correctly never matches.
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start < server_name.size()) {
        auto colon = server_name.find(':', start);
        if (colon == std::string_view::npos) {
            parts.push_back(server_name.substr(start));
            break;
        }
        parts.push_back(server_name.substr(start, colon - start));
        start = colon + 1;
    }

    for (const auto& entry : channels) {
        if (entry.kind == ChannelEntryKind::Server) {
            if (server_name == entry.name) return entry;
        } else {
            // Plugin kind: match parts[0] == "plugin" && parts[1] == entry.name
            if (parts.size() >= 2 && parts[0] == "plugin" && parts[1] == entry.name) {
                return entry;
            }
        }
    }
    return std::nullopt;
}

// =========================================================================
// Allowlist helpers
// =========================================================================

// Forward declaration — defined below with the GrowthBook stub section.
[[nodiscard]] auto get_channel_allowlist() -> std::vector<ChannelAllowlistEntry>;

// TS REF: src/services/mcp/channelNotification.ts:127-138
// Effective allowlist for the current session. Team/enterprise orgs can set
// allowedChannelPlugins in managed settings — when set, it REPLACES the
// GrowthBook ledger (admin owns the trust decision). std::nullopt falls
// back to the ledger. Unmanaged users always get the ledger.
[[nodiscard]] inline auto get_effective_channel_allowlist(
    bool is_managed_org,
    const std::optional<std::vector<ChannelAllowlistEntry>>& org_list
) -> std::pair<std::vector<ChannelAllowlistEntry>, AllowlistSource> {
    if (is_managed_org && org_list.has_value()) {
        return {*org_list, AllowlistSource::Org};
    }
    return {get_channel_allowlist(), AllowlistSource::Ledger};
}

// =========================================================================
// Channel server gating
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:191-316
// Gate an MCP server's channel-notification path. Gate order:
//   capability → runtime gate (tengu_harbor) → auth (OAuth only) →
//   org policy → session --channels → allowlist.
//
//   skip      Not a channel server, or managed org hasn't opted in, or
//             not in session --channels. Connection stays up; handler
//             not registered.
//   register  Subscribe to notifications/claude/channel.
//
// Which servers can connect at all is governed by allowedMcpServers —
// this gate only decides whether the notification handler registers.
//
// Parameters that are stubbed in CPP (wired to real state later):
//   - channels_enabled: corresponds to isChannelsEnabled() (tengu_harbor feature flag)
//   - has_oauth_token: whether user has claude.ai OAuth tokens
//   - subscription_type: "free", "team", "enterprise", etc.
//   - managed_channels_enabled: org policy channelsEnabled flag
//   - allowed_channels: user's --channels entries
//   - org_allowed_plugins: org-managed allowedChannelPlugins
//   - plugin_source: the plugin's marketplace source (for plugin-kind entries)
[[nodiscard]] inline auto gate_channel_server(
    std::string_view server_name,
    const ServerCapabilities& capabilities,
    bool channels_enabled,
    bool has_oauth_token,
    bool is_managed_org,
    bool managed_channels_enabled,
    const std::vector<ChannelEntry>& allowed_channels,
    const std::optional<std::vector<ChannelAllowlistEntry>>& org_allowed_plugins,
    const std::optional<std::string>& plugin_source = std::nullopt
) -> ChannelGateResult {
    // TS REF: channelNotification.ts:200-206
    // Channel servers declare `experimental['claude/channel']: {}` (MCP's
    // presence-signal idiom — same as `tools: {}`). Presence in the map
    // covers `{}` and `true`; absent/undefined/explicit-`false` all fail.
    auto exp_it = capabilities.experimental.find("claude/channel");
    if (exp_it == capabilities.experimental.end()) {
        return ChannelGateResult{
            .action = ChannelGateAction::Skip,
            .skip_kind = ChannelGateSkipKind::Capability,
            .reason = "server did not declare claude/channel capability"
        };
    }

    // TS REF: channelNotification.ts:211-217
    // Overall runtime gate. After capability so normal MCP servers never hit
    // this path. Before auth/policy so the killswitch works regardless of
    // session state.
    if (!channels_enabled) {
        return ChannelGateResult{
            .action = ChannelGateAction::Skip,
            .skip_kind = ChannelGateSkipKind::Disabled,
            .reason = "channels feature is not currently available"
        };
    }

    // TS REF: channelNotification.ts:222-228
    // OAuth-only. API key users (console) are blocked — there's no
    // channelsEnabled admin surface in console yet, so the policy opt-in
    // flow doesn't exist for them.
    if (!has_oauth_token) {
        return ChannelGateResult{
            .action = ChannelGateAction::Skip,
            .skip_kind = ChannelGateSkipKind::Auth,
            .reason = "channels requires claude.ai authentication (run /login)"
        };
    }

    // TS REF: channelNotification.ts:235-245
    // Teams/Enterprise opt-in. Managed orgs must explicitly enable channels.
    // Default OFF — absent or false blocks.
    if (is_managed_org && !managed_channels_enabled) {
        return ChannelGateResult{
            .action = ChannelGateAction::Skip,
            .skip_kind = ChannelGateSkipKind::Policy,
            .reason = "channels not enabled by org policy (set channelsEnabled: true in managed settings)"
        };
    }

    // TS REF: channelNotification.ts:250-257
    // User-level session opt-in. A server must be explicitly listed in
    // --channels to push inbound this session.
    auto entry = find_channel_entry(server_name, allowed_channels);
    if (!entry.has_value()) {
        return ChannelGateResult{
            .action = ChannelGateAction::Skip,
            .skip_kind = ChannelGateSkipKind::Session,
            .reason = std::format("server {} not in --channels list for this session", server_name)
        };
    }

    const auto& matched = *entry;

    // TS REF: channelNotification.ts:259-302
    if (matched.kind == ChannelEntryKind::Plugin) {
        // Marketplace verification: the tag is intent (plugin:slack@anthropic),
        // the runtime name is just plugin:slack:X — could be slack@anthropic or
        // slack@evil depending on what's installed. Verify they match before
        // trusting the tag for the allowlist check below.
        std::optional<std::string> actual_marketplace;
        if (plugin_source.has_value()) {
            auto parsed = parse_plugin_identifier(*plugin_source);
            actual_marketplace = parsed.marketplace;
        }

        if (actual_marketplace != matched.marketplace) {
            return ChannelGateResult{
                .action = ChannelGateAction::Skip,
                .skip_kind = ChannelGateSkipKind::Marketplace,
                .reason = std::format(
                    "you asked for plugin:{}@{} but the installed {} plugin is from {}",
                    matched.name,
                    matched.marketplace.value_or("unknown"),
                    matched.name,
                    actual_marketplace.value_or("an unknown source")
                )
            };
        }

        // Approved-plugin allowlist. entry.dev bypasses — so accepting the dev
        // dialog for one entry doesn't leak allowlist-bypass to --channels entries.
        if (!matched.dev) {
            auto [entries, source] = get_effective_channel_allowlist(
                is_managed_org, org_allowed_plugins
            );
            bool found = std::any_of(entries.begin(), entries.end(),
                [&](const ChannelAllowlistEntry& e) {
                    return e.plugin == matched.name &&
                           e.marketplace == matched.marketplace.value_or("");
                });
            if (!found) {
                std::string reason;
                if (source == AllowlistSource::Org) {
                    reason = std::format(
                        "plugin {}@{} is not on your org's approved channels list (set allowedChannelPlugins in managed settings)",
                        matched.name, matched.marketplace.value_or("")
                    );
                } else {
                    reason = std::format(
                        "plugin {}@{} is not on the approved channels allowlist (use --dangerously-load-development-channels for local dev)",
                        matched.name, matched.marketplace.value_or("")
                    );
                }
                return ChannelGateResult{
                    .action = ChannelGateAction::Skip,
                    .skip_kind = ChannelGateSkipKind::Allowlist,
                    .reason = std::move(reason)
                };
            }
        }
    } else {
        // TS REF: channelNotification.ts:303-313
        // server-kind: allowlist schema is {marketplace, plugin} — a server entry
        // can never match. Without this, --channels server:plugin:foo:bar would
        // match a plugin's runtime name and register with no allowlist check.
        if (!matched.dev) {
            return ChannelGateResult{
                .action = ChannelGateAction::Skip,
                .skip_kind = ChannelGateSkipKind::Allowlist,
                .reason = std::format(
                    "server {} is not on the approved channels allowlist (use --dangerously-load-development-channels for local dev)",
                    matched.name
                )
            };
        }
    }

    return ChannelGateResult{
        .action = ChannelGateAction::Register,
        .skip_kind = std::nullopt,
        .reason = ""
    };
}

// =========================================================================
// Notification parameter parsing
// =========================================================================

// TS REF: src/services/mcp/channelNotification.ts:37-47
// Parse a notifications/claude/channel notification's params from JSON.
// Returns std::nullopt if the JSON is malformed or missing required fields.
[[nodiscard]] inline auto parse_channel_message_params(
    const std::string& params_json
) -> std::optional<ChannelMessageParams> {
    auto doc = cc::utils::json::parse(params_json);
    if (!doc) return std::nullopt;

    auto root = doc->root();
    if (!root.is_obj()) return std::nullopt;

    auto content_node = root.get("content");
    if (!content_node.is_str()) return std::nullopt;

    ChannelMessageParams params;
    params.content = std::string(content_node.as_str());

    auto meta_node = root.get("meta");
    if (meta_node.is_obj()) {
        std::map<std::string, std::string> meta;
        meta_node.iter_obj([&](JsonVal key, JsonVal val) {
            if (val.is_str()) {
                meta[std::string(key.as_str())] = std::string(val.as_str());
            }
        });
        params.meta = std::move(meta);
    }

    return params;
}

// TS REF: src/services/mcp/channelNotification.ts:64-72
// Parse a notifications/claude/channel/permission notification's params.
[[nodiscard]] inline auto parse_channel_permission_params(
    const std::string& params_json
) -> std::optional<ChannelPermissionParams> {
    auto doc = cc::utils::json::parse(params_json);
    if (!doc) return std::nullopt;

    auto root = doc->root();
    if (!root.is_obj()) return std::nullopt;

    auto req_id_node = root.get("request_id");
    if (!req_id_node.is_str()) return std::nullopt;

    auto behavior_node = root.get("behavior");
    if (!behavior_node.is_str()) return std::nullopt;

    auto behavior = std::string(behavior_node.as_str());
    if (behavior != "allow" && behavior != "deny") return std::nullopt;

    return ChannelPermissionParams{
        .request_id = std::string(req_id_node.as_str()),
        .behavior = std::move(behavior)
    };
}

// Serialize a permission request to JSON for sending to the channel server.
[[nodiscard]] inline auto serialize_permission_request_params(
    const ChannelPermissionRequestParams& params
) -> std::string {
    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();
    root.add("request_id", doc.string(params.request_id));
    root.add("tool_name", doc.string(params.tool_name));
    root.add("description", doc.string(params.description));
    root.add("input_preview", doc.string(params.input_preview));
    doc.set_root(root);
    return doc.to_string();
}

// =========================================================================
// ChannelNotificationBus — thread-safe pub/sub for channel events
// =========================================================================

// TS REF: implied by the subscribe/emit pattern in channelNotification.ts
// A bus that lets code subscribe to channel notifications per-server,
// per-type, or globally. Emissions are dispatched synchronously to all
// matching handlers.

// RAII subscription handle — destroys the subscription when it goes out of scope.
class SubscriptionHandle {
public:
    SubscriptionHandle() = default;

    SubscriptionHandle(int id, std::function<void(int)> unsub_fn)
        : id_(id), unsub_fn_(std::move(unsub_fn)) {}

    ~SubscriptionHandle() { reset(); }

    SubscriptionHandle(const SubscriptionHandle&) = delete;
    auto operator=(const SubscriptionHandle&) -> SubscriptionHandle& = delete;

    SubscriptionHandle(SubscriptionHandle&& other) noexcept
        : id_(other.id_), unsub_fn_(std::move(other.unsub_fn_)) {
        other.id_ = -1;
        other.unsub_fn_ = nullptr;
    }

    auto operator=(SubscriptionHandle&& other) noexcept -> SubscriptionHandle& {
        if (this != &other) {
            reset();
            id_ = other.id_;
            unsub_fn_ = std::move(other.unsub_fn_);
            other.id_ = -1;
            other.unsub_fn_ = nullptr;
        }
        return *this;
    }

    void reset() {
        if (id_ >= 0 && unsub_fn_) {
            unsub_fn_(id_);
        }
        id_ = -1;
        unsub_fn_ = nullptr;
    }

    [[nodiscard]] auto id() const -> int { return id_; }
    [[nodiscard]] auto valid() const -> bool { return id_ >= 0; }

private:
    int id_ = -1;
    std::function<void(int)> unsub_fn_;
};

namespace detail {

// Internal subscription record
struct SubscriptionRecord {
    int id;
    std::optional<std::string> server_filter; // nullopt = all servers
    ChannelNotificationType type_filter;
    ChannelNotificationHandler handler;
};

inline std::mutex bus_mutex;
inline std::atomic<int> next_subscription_id{1};
inline std::map<int, SubscriptionRecord> bus_subscriptions;
// Track which servers have active channel handlers (for get_active_servers)
inline std::set<std::string> active_servers;

} // namespace detail

// Subscribe to notifications from a specific server + type.
// Returns a RAII SubscriptionHandle that unsubscribes on destruction.
[[nodiscard]] inline auto subscribe_channel_notification(
    std::string_view server_name,
    ChannelNotificationType type,
    ChannelNotificationHandler handler
) -> SubscriptionHandle {
    std::lock_guard lock(detail::bus_mutex);
    int id = detail::next_subscription_id.fetch_add(1);
    detail::bus_subscriptions[id] = detail::SubscriptionRecord{
        .id = id,
        .server_filter = std::string(server_name),
        .type_filter = type,
        .handler = std::move(handler)
    };
    detail::active_servers.insert(std::string(server_name));

    return SubscriptionHandle(id, [](int sub_id) {
        std::lock_guard lock2(detail::bus_mutex);
        auto it = detail::bus_subscriptions.find(sub_id);
        if (it != detail::bus_subscriptions.end()) {
            // Remove from active_servers if no more subs for this server
            auto srv = it->second.server_filter;
            detail::bus_subscriptions.erase(it);
            if (srv.has_value()) {
                bool still_active = false;
                for (const auto& [_, rec] : detail::bus_subscriptions) {
                    if (rec.server_filter == srv) {
                        still_active = true;
                        break;
                    }
                }
                if (!still_active) {
                    detail::active_servers.erase(*srv);
                }
            }
        }
    });
}

// Subscribe to notifications of a given type from ALL servers.
[[nodiscard]] inline auto subscribe_all_channel_notifications(
    ChannelNotificationType type,
    ChannelNotificationHandler handler
) -> SubscriptionHandle {
    std::lock_guard lock(detail::bus_mutex);
    int id = detail::next_subscription_id.fetch_add(1);
    detail::bus_subscriptions[id] = detail::SubscriptionRecord{
        .id = id,
        .server_filter = std::nullopt,
        .type_filter = type,
        .handler = std::move(handler)
    };

    return SubscriptionHandle(id, [](int sub_id) {
        std::lock_guard lock2(detail::bus_mutex);
        detail::bus_subscriptions.erase(sub_id);
    });
}

// Emit a channel notification to all matching handlers.
// Handlers are called synchronously under the bus lock — keep handlers light.
inline auto emit_channel_notification(
    std::string_view server_name,
    const ChannelNotification& notification
) -> void {
    // Copy the matching handlers under the lock, then release before calling
    // them to avoid deadlocks if a handler tries to subscribe/unsubscribe.
    std::vector<ChannelNotificationHandler> matching;
    {
        std::lock_guard lock(detail::bus_mutex);
        for (const auto& [id, rec] : detail::bus_subscriptions) {
            if (rec.type_filter != notification.type) continue;
            if (rec.server_filter.has_value() && *rec.server_filter != server_name) continue;
            matching.push_back(rec.handler);
        }
    }
    for (const auto& handler : matching) {
        handler(notification);
    }
}

// Convenience: emit a ServerStarted notification
inline auto emit_server_started(std::string_view server_name) -> void {
    ChannelNotification notif{
        .type = ChannelNotificationType::ServerStarted,
        .server_name = std::string(server_name),
        .timestamp = std::chrono::system_clock::now(),
        .data_json = "{}"
    };
    emit_channel_notification(server_name, notif);
}

// Convenience: emit a ServerStopped notification
inline auto emit_server_stopped(std::string_view server_name) -> void {
    ChannelNotification notif{
        .type = ChannelNotificationType::ServerStopped,
        .server_name = std::string(server_name),
        .timestamp = std::chrono::system_clock::now(),
        .data_json = "{}"
    };
    emit_channel_notification(server_name, notif);
}

// Convenience: emit a ToolListChanged notification
inline auto emit_tool_list_changed(std::string_view server_name) -> void {
    ChannelNotification notif{
        .type = ChannelNotificationType::ToolListChanged,
        .server_name = std::string(server_name),
        .timestamp = std::chrono::system_clock::now(),
        .data_json = "{}"
    };
    emit_channel_notification(server_name, notif);
}

// Convenience: emit a ServerError notification
inline auto emit_server_error(std::string_view server_name, std::string_view error_message) -> void {
    ChannelNotification notif{
        .type = ChannelNotificationType::ServerError,
        .server_name = std::string(server_name),
        .timestamp = std::chrono::system_clock::now(),
        .data_json = std::string(error_message)
    };
    emit_channel_notification(server_name, notif);
}

// Get the set of server names that have active channel subscriptions.
[[nodiscard]] inline auto get_active_channel_servers() -> std::vector<std::string> {
    std::lock_guard lock(detail::bus_mutex);
    return {detail::active_servers.begin(), detail::active_servers.end()};
}

// =========================================================================
// Stub: allowlist source (GrowthBook tengu_harbor_ledger)
// =========================================================================

// TS REF: src/services/mcp/channelAllowlist.ts:37-44
// Returns the current channel allowlist from the feature flag.
// Stubbed to return empty in CPP until GrowthBook integration lands.
// The real implementation reads tengu_harbor_ledger feature value and
// validates against ChannelAllowlistSchema.
[[nodiscard]] inline auto get_channel_allowlist() -> std::vector<ChannelAllowlistEntry> {
    // TODO: wire to GrowthBook tengu_harbor_ledger feature value
    return {};
}

// TS REF: src/services/mcp/channelAllowlist.ts:51-53
// Overall channels on/off. Checked before any per-server gating.
// Stubbed to return false until GrowthBook integration lands.
[[nodiscard]] inline auto is_channels_enabled() -> bool {
    // TODO: wire to GrowthBook tengu_harbor feature value
    return false;
}

// =========================================================================
// Legacy compatibility (from original 43-line stub)
// =========================================================================

// Notify all subscribers of a channel update event (legacy string-based API).
// Kept for backward compatibility — prefer emit_channel_notification for new code.
auto notify_channel_update(std::string_view channel, std::string_view event) -> void {
    ChannelNotification notif{
        .type = ChannelNotificationType::ChannelMessage,
        .server_name = std::string(channel),
        .timestamp = std::chrono::system_clock::now(),
        .data_json = std::string(event)
    };
    emit_channel_notification(channel, notif);
}

// Subscribe to events on a specific channel (legacy string-based API).
// Returns a subscription ID for unsubscribe().
// Prefer subscribe_channel_notification + SubscriptionHandle for new code.
auto subscribe_channel_events(
    std::string_view channel,
    std::function<void(std::string)> callback
) -> int {
    std::lock_guard lock(detail::bus_mutex);
    int id = detail::next_subscription_id.fetch_add(1);
    detail::bus_subscriptions[id] = detail::SubscriptionRecord{
        .id = id,
        .server_filter = std::string(channel),
        .type_filter = ChannelNotificationType::ChannelMessage,
        .handler = [cb = std::move(callback)](const ChannelNotification& n) {
            cb(n.data_json);
        }
    };
    detail::active_servers.insert(std::string(channel));
    return id;
}

// Unsubscribe from channel events by legacy subscription ID.
// Prefer SubscriptionHandle RAII for new code.
auto unsubscribe(int id) -> void {
    std::lock_guard lock(detail::bus_mutex);
    auto it = detail::bus_subscriptions.find(id);
    if (it != detail::bus_subscriptions.end()) {
        auto srv = it->second.server_filter;
        detail::bus_subscriptions.erase(it);
        if (srv.has_value()) {
            bool still_active = false;
            for (const auto& [_, rec] : detail::bus_subscriptions) {
                if (rec.server_filter == srv) {
                    still_active = true;
                    break;
                }
            }
            if (!still_active) {
                detail::active_servers.erase(*srv);
            }
        }
    }
}

// =========================================================================
// Additional convenience emitters (completing the enum coverage)
// =========================================================================

// Convenience: emit a ResourceListChanged notification
// TS REF: implied by notifications/resources/list_changed handler in
// src/services/mcp/connectionManager.ts
inline auto emit_resource_list_changed(std::string_view server_name) -> void {
    ChannelNotification notif{
        .type = ChannelNotificationType::ResourceListChanged,
        .server_name = std::string(server_name),
        .timestamp = std::chrono::system_clock::now(),
        .data_json = "{}"
    };
    emit_channel_notification(server_name, notif);
}

// Convenience: emit a PromptListChanged notification
// TS REF: implied by notifications/prompts/list_changed handler in
// src/services/mcp/connectionManager.ts
inline auto emit_prompt_list_changed(std::string_view server_name) -> void {
    ChannelNotification notif{
        .type = ChannelNotificationType::PromptListChanged,
        .server_name = std::string(server_name),
        .timestamp = std::chrono::system_clock::now(),
        .data_json = "{}"
    };
    emit_channel_notification(server_name, notif);
}

// =========================================================================
// Server health monitoring
// =========================================================================

// TS REF: src/services/mcp/ — health monitoring is distributed across the
// MCP SDK's transport layer (SSE reconnect with exponential backoff in
// client.ts) and the connection manager's notification-refresh workers.
//
// CPP adds an explicit health monitor because our transport layer is
// thinner than the TS SDK's. This monitor pings servers that have
// notifications capability, tracks consecutive failures, and emits
// ServerError after 3 failures so the UI can show degraded state.

// Per-server health state tracked by the monitor.
struct ServerHealthState {
    std::string server_name;
    int consecutive_failures = 0;
    // Wall-clock of last ping attempt (success or failure)
    std::chrono::steady_clock::time_point last_ping_attempt{};
    // Wall-clock of last successful ping
    std::chrono::steady_clock::time_point last_successful_ping{};
    // Whether we've already emitted a ServerError for the current failure
    // streak. Reset to false on the next successful ping.
    bool error_emitted = false;
    // Exponential backoff: next reconnect delay after N failures
    std::chrono::milliseconds current_backoff{0};
};

// ChannelHealthMonitor — background health-check for MCP servers that
// support notifications. Runs a single ping loop that wakes every
// kPingInterval seconds and checks all registered servers.
//
// Thread safety: all public methods are mutex-guarded. The monitor loop
// holds the lock only briefly while copying the server list and updating
// health state; ping functions execute outside the lock.
class ChannelHealthMonitor {
public:
    // A ping function takes a server name and returns true if the server
    // is healthy, false otherwise. Callers supply this (typically via
    // McpClient::ping() or a lightweight JSON-RPC request).
    using PingFunction = std::function<bool(const std::string&)>;

    // Reconnect function: called when the monitor decides a server needs
    // to be reconnected (after kErrorThreshold failures). Takes server
    // name, returns true if reconnect was initiated.
    using ReconnectFunction = std::function<bool(const std::string&)>;

    ChannelHealthMonitor() = default;

    ~ChannelHealthMonitor() {
        stop();
    }

    // Non-copyable, non-movable (owns a thread).
    ChannelHealthMonitor(const ChannelHealthMonitor&) = delete;
    auto operator=(const ChannelHealthMonitor&) -> ChannelHealthMonitor& = delete;
    ChannelHealthMonitor(ChannelHealthMonitor&&) = delete;
    auto operator=(ChannelHealthMonitor&&) -> ChannelHealthMonitor& = delete;

    // Start the health-monitor thread. Idempotent — calling start() on
    // an already-running monitor is a no-op.
    auto start() -> void {
        if (running_.exchange(true)) return; // already running
        monitor_thread_ = std::jthread([this](std::stop_token stoken) {
            monitor_loop(std::move(stoken));
        });
    }

    // Stop the health-monitor thread and join. Idempotent.
    auto stop() -> void {
        if (!running_.exchange(false)) return; // already stopped
        if (monitor_thread_.joinable()) {
            monitor_thread_.request_stop();
            monitor_thread_.join();
        }
    }

    // Register a server for health monitoring. If the monitor is not yet
    // running, it is started automatically.
    auto register_server(
        std::string server_name,
        PingFunction ping_fn,
        ReconnectFunction reconnect_fn = nullptr
    ) -> void {
        {
            std::lock_guard lock(mutex_);
            ping_functions_[server_name] = std::move(ping_fn);
            reconnect_functions_[server_name] = std::move(reconnect_fn);

            // Initialize or reset health state on (re-)registration.
            auto& state = health_states_[server_name];
            state.server_name = server_name;
            state.consecutive_failures = 0;
            state.last_ping_attempt = std::chrono::steady_clock::now();
            state.last_successful_ping = std::chrono::steady_clock::now();
            state.error_emitted = false;
            state.current_backoff = std::chrono::milliseconds{0};
        }
        // Start monitor if not running (lazy start).
        start();
    }

    // Remove a server from health monitoring. Called on explicit
    // disconnect or server shutdown. Does NOT emit any notification.
    auto unregister_server(std::string_view server_name) -> void {
        std::lock_guard lock(mutex_);
        auto s = std::string(server_name);
        ping_functions_.erase(s);
        reconnect_functions_.erase(s);
        health_states_.erase(s);
    }

    // Check if a server is currently registered for health monitoring.
    [[nodiscard]] auto is_monitoring(std::string_view server_name) const -> bool {
        std::lock_guard lock(mutex_);
        return ping_functions_.contains(std::string(server_name));
    }

    // Get a snapshot of the current health state for a server.
    // Returns std::nullopt if the server is not registered.
    [[nodiscard]] auto get_health_state(std::string_view server_name) const
        -> std::optional<ServerHealthState> {
        std::lock_guard lock(mutex_);
        auto it = health_states_.find(std::string(server_name));
        if (it == health_states_.end()) return std::nullopt;
        return it->second;
    }

    // Get the number of consecutive failures for a server.
    // Returns 0 if the server is not registered.
    [[nodiscard]] auto get_failure_count(std::string_view server_name) const -> int {
        std::lock_guard lock(mutex_);
        auto it = health_states_.find(std::string(server_name));
        if (it == health_states_.end()) return 0;
        return it->second.consecutive_failures;
    }

    // Get all currently-monitored server names.
    [[nodiscard]] auto get_registered_servers() const -> std::vector<std::string> {
        std::lock_guard lock(mutex_);
        std::vector<std::string> result;
        result.reserve(health_states_.size());
        for (const auto& [name, _] : health_states_) {
            result.push_back(name);
        }
        return result;
    }

    // Ping interval between health checks.
    static constexpr std::chrono::seconds kPingInterval{30};
    // Number of consecutive failures before emitting ServerError.
    static constexpr int kErrorThreshold{3};
    // Initial backoff for reconnect attempts (doubles each attempt).
    static constexpr std::chrono::milliseconds kInitialBackoff{1000};
    // Maximum backoff cap.
    static constexpr std::chrono::milliseconds kMaxBackoff{60000};

private:
    // Main monitor loop — runs on monitor_thread_. Woken every
    // kPingInterval or on stop request.
    auto monitor_loop(std::stop_token stoken) -> void {
        while (!stoken.stop_requested()) {
            // Sleep for the ping interval, checking for stop requests.
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, stoken, kPingInterval,
                    [&]{ return stoken.stop_requested(); });
            }
            if (stoken.stop_requested()) break;

            // Snapshot the servers and their ping functions under the lock.
            struct ServerPingEntry {
                std::string name;
                PingFunction ping;
                ReconnectFunction reconnect;
            };
            std::vector<ServerPingEntry> entries;
            {
                std::lock_guard lock(mutex_);
                entries.reserve(ping_functions_.size());
                for (const auto& [name, fn] : ping_functions_) {
                    auto recon_it = reconnect_functions_.find(name);
                    entries.push_back(ServerPingEntry{
                        .name = name,
                        .ping = fn,
                        .reconnect = recon_it != reconnect_functions_.end()
                            ? recon_it->second : nullptr
                    });
                }
            }

            // Ping each server OUTSIDE the lock to avoid blocking other
            // operations on slow pings.
            for (const auto& entry : entries) {
                ping_server(entry.name, entry.ping, entry.reconnect);
            }
        }
    }

    // Ping a single server and update its health state.
    auto ping_server(
        const std::string& server_name,
        const PingFunction& ping_fn,
        const ReconnectFunction& reconnect_fn
    ) -> void {
        if (!ping_fn) return;

        bool healthy = false;
        try {
            healthy = ping_fn(server_name);
        } catch (...) {
            healthy = false;
        }

        std::lock_guard lock(mutex_);
        auto it = health_states_.find(server_name);
        if (it == health_states_.end()) return; // unregistered while pinging

        auto& state = it->second;
        state.last_ping_attempt = std::chrono::steady_clock::now();

        if (healthy) {
            // Success: reset failure tracking.
            state.consecutive_failures = 0;
            state.last_successful_ping = state.last_ping_attempt;
            state.error_emitted = false;
            state.current_backoff = std::chrono::milliseconds{0};
        } else {
            // Failure: increment counter.
            state.consecutive_failures++;

            // After kErrorThreshold failures, emit ServerError once.
            if (state.consecutive_failures >= kErrorThreshold && !state.error_emitted) {
                state.error_emitted = true;
                emit_server_error(server_name,
                    std::format("server {} failed {} consecutive health checks",
                        server_name, state.consecutive_failures));
            }

            // Attempt reconnect with exponential backoff if a reconnect
            // function was provided.
            if (reconnect_fn && state.consecutive_failures >= kErrorThreshold) {
                if (state.current_backoff == std::chrono::milliseconds{0}) {
                    state.current_backoff = kInitialBackoff;
                } else {
                    // Double the backoff, capped at kMaxBackoff.
                    auto next = state.current_backoff.count() * 2;
                    state.current_backoff = std::chrono::milliseconds{
                        std::min(next, kMaxBackoff.count())
                    };
                }

                // Fire reconnect asynchronously so we don't block the
                // monitor loop. Detached thread is safe — the reconnect
                // function owns its own lifetime.
                std::thread([reconnect_fn, server_name,
                             delay = state.current_backoff]() {
                    std::this_thread::sleep_for(delay);
                    try {
                        (void)reconnect_fn(server_name);
                    } catch (...) {
                        // Reconnect failures are surfaced via the next
                        // ping cycle; swallow here to avoid thread abort.
                    }
                }).detach();
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::jthread monitor_thread_;
    std::atomic<bool> running_{false};

    // Server name → ping function
    std::unordered_map<std::string, PingFunction> ping_functions_;
    // Server name → reconnect function (optional)
    std::unordered_map<std::string, ReconnectFunction> reconnect_functions_;
    // Server name → health state
    std::unordered_map<std::string, ServerHealthState> health_states_;
};

// Get the process-wide singleton health monitor.
// TS REF: process-wide singleton pattern used by MCP connection manager
[[nodiscard]] inline auto get_global_health_monitor() -> ChannelHealthMonitor& {
    static ChannelHealthMonitor instance;
    return instance;
}

// =========================================================================
// Integration helpers — wire channel notifications to server lifecycle
// =========================================================================

// TS REF: src/services/mcp/connectionManager.ts — the TS connection manager
// emits lifecycle events that the channel notification system hooks into.
// In CPP we provide explicit integration functions so the connection
// manager (or any caller) can wire channel notifications to server events.

// Called when an MCP server successfully connects and is initialized.
// Emits ServerStarted and registers the server for health monitoring if
// it has notifications capability.
//
// Parameters:
//   server_name     — unique name of the connected server
//   has_notifications — whether the server declares notifications capability
//   ping_fn         — optional health-check function (required for monitoring)
//   reconnect_fn    — optional reconnect function for auto-recovery
inline auto on_mcp_server_connected(
    std::string_view server_name,
    bool has_notifications,
    ChannelHealthMonitor::PingFunction ping_fn = nullptr,
    ChannelHealthMonitor::ReconnectFunction reconnect_fn = nullptr
) -> void {
    // Always emit ServerStarted — even non-channel servers benefit from
    // UI awareness of connection state.
    emit_server_started(server_name);

    // Only register for health monitoring if the server supports
    // notifications and a ping function was provided.
    if (has_notifications && ping_fn) {
        get_global_health_monitor().register_server(
            std::string(server_name),
            std::move(ping_fn),
            std::move(reconnect_fn)
        );
    }
}

// Called when an MCP server disconnects (cleanly or due to error).
// Emits ServerStopped and removes the server from health monitoring.
inline auto on_mcp_server_disconnected(std::string_view server_name) -> void {
    get_global_health_monitor().unregister_server(server_name);
    emit_server_stopped(server_name);
}

// Called when the server's tools/list response changes (via
// notifications/tools/list_changed or a refresh cycle).
inline auto on_mcp_tools_changed(std::string_view server_name) -> void {
    emit_tool_list_changed(server_name);
}

// Called when the server's resources/list response changes.
inline auto on_mcp_resources_changed(std::string_view server_name) -> void {
    emit_resource_list_changed(server_name);
}

// Called when the server's prompts/list response changes.
inline auto on_mcp_prompts_changed(std::string_view server_name) -> void {
    emit_prompt_list_changed(server_name);
}

// Called when a server encounters a non-fatal error (e.g., a single
// tool call fails). Does NOT emit ServerError — that's reserved for
// health-check failures from the monitor.
inline auto on_mcp_server_error(
    std::string_view server_name,
    std::string_view error_message
) -> void {
    emit_server_error(server_name, error_message);
}

// =========================================================================
// Wire-to-connection-manager convenience helper
// =========================================================================

// TS REF: src/services/mcp/connectionManager.ts — in TS, the connection
// manager fires events that the channel notification module subscribes to.
// In CPP, we provide this helper so callers can wire a McpConnectionManager
// to the channel notification bus with a single call.
//
// Usage:
//   auto& mgr = get_connection_manager();
//   wire_channel_bus_to_manager(mgr,
//       [](const std::string& name) {
//           auto srv = mgr.get_server(name);
//           return srv && srv->get().client && srv->get().client->ping();
//       },
//       [&mgr](const std::string& name) {
//           return mgr.reconnect_server(name);
//       });
//
// Note: this is a template to avoid a hard dependency on
// McpConnectionManager (which lives in a different module). Callers must
// import both modules.
template <typename ConnectionManagerT>
auto wire_channel_bus_to_manager(
    ConnectionManagerT& mgr,
    ChannelHealthMonitor::PingFunction ping_fn,
    ChannelHealthMonitor::ReconnectFunction reconnect_fn = nullptr
) -> void {
    // Wire connect → emit ServerStarted + register health monitor
    mgr.set_server_connected_callback(
        [ping_fn, reconnect_fn](const std::string& server_name) {
            on_mcp_server_connected(
                server_name,
                /*has_notifications=*/true,
                ping_fn,
                reconnect_fn
            );
        }
    );

    // Wire disconnect → emit ServerStopped + unregister
    mgr.set_server_disconnected_callback(
        [](const std::string& server_name) {
            on_mcp_server_disconnected(server_name);
        }
    );

    // Wire tools updated → emit ToolListChanged
    mgr.set_tools_updated_callback(
        [](const std::string& server_name, const std::vector<std::string>& /*tools*/) {
            on_mcp_tools_changed(server_name);
        }
    );

    // Wire server error → emit ServerError
    mgr.set_server_error_callback(
        [](const std::string& server_name, const std::string& error) {
            on_mcp_server_error(server_name, error);
        }
    );
}

} // namespace cc::services::mcp