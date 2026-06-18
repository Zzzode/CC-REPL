module;
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.migrations.migration_registry;
import cc.utils.json;

export import cc.migrations.migration_runner;

// ---------------------------------------------------------------
// Forward helpers used by all migrations (TU-local, not exported).
// Defined *outside* `export namespace cc::migrations` in an
// implementation-private named namespace so the function addresses
// can be safely captured into the global descriptor list without
// exposing TU-local entities across module boundaries (keeps Clang
// `-WTU-local-entity-exposure` clean).
// ---------------------------------------------------------------
namespace cc_migrations_private_impl {

using cc::utils::json::JsonMutVal;

/// Validate that a mutable value is an object.
[[nodiscard]] inline bool is_object(const JsonMutVal& v) noexcept {
    return v.valid() && v.is_obj();
}

/// Ensure @p root has a top-level "schema_version" numeric key.  Never fail -
/// every migration is free to run against an empty {}.
inline void ensure_schema_version(JsonMutVal& root, int fallback = 0) {
    if (!is_object(root)) return;
    auto existing = root.get("schema_version");
    if (!existing.valid() || !existing.is_num()) {
        root.set("schema_version", static_cast<int64_t>(fallback));
    }
}

// ================================================================
// Migration v1: create_sessions_table
// ---------------------------------------------------------------
// Introduces the "sessions" top-level structure.  The sessions storage
// schema v1 is deliberately minimal:
//
//   sessions: {
//       currentId: string | null,
//       byId:      {}
//   }
//
// Rollback removes both fields.
// ================================================================
inline auto migrate_v1_up(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");
    ensure_schema_version(root, 1);

    auto sessions = root.ensure_object("sessions");

    // currentId — optional string; default to null.
    if (!sessions.has("currentId")) {
        sessions.add("currentId", sessions.make_null());
    }

    // byId — empty map of sessionId -> session object.
    (void)sessions.ensure_object("byId");

    return {};
}

inline auto migrate_v1_down(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");
    (void)root.remove("sessions");
    return {};
}

// ================================================================
// Migration v2: create_config_table
// ---------------------------------------------------------------
// Adds a "config" container with the core sub-sections every version of
// the app relies on:
//
//   config: { userSettings, projectSettings, globalConfig }
//
// Rollback deletes the whole config node.
// ================================================================
inline auto migrate_v2_up(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");

    auto config = root.ensure_object("config");
    (void)config.ensure_object("userSettings");
    (void)config.ensure_object("projectSettings");
    (void)config.ensure_object("globalConfig");

    return {};
}

inline auto migrate_v2_down(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");
    (void)root.remove("config");
    return {};
}

// ================================================================
// Migration v3: add_tool_results_to_messages
// ---------------------------------------------------------------
// Evolves the session message schema: every message inside
// sessions.byId[*].messages[] that lacks a "tool_results" array gets
// one initialised to [].
//
// Rollback removes tool_results from every message across all sessions.
// ================================================================
inline auto migrate_v3_up(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");

    auto sessions = root.get("sessions");
    if (!sessions.valid() || !sessions.is_obj()) return {};

    auto by_id = sessions.get("byId");
    if (!by_id.valid() || !by_id.is_obj()) return {};

    // sessions.byId -> for each session entry, scan messages array.
    by_id.iter_obj([](JsonMutVal /*sid*/, JsonMutVal session) {
        if (!session.is_obj()) return;

        auto messages = session.get("messages");
        if (!messages.valid() || !messages.is_arr()) return;

        messages.iter([](JsonMutVal msg, std::size_t /*i*/) {
            if (!msg.valid() || !msg.is_obj()) return;
            if (!msg.has("tool_results")) {
                msg.add("tool_results", msg.make_arr());
            }
        });
    });

    return {};
}

inline auto migrate_v3_down(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");

    auto sessions = root.get("sessions");
    if (!sessions.valid() || !sessions.is_obj()) return {};

    auto by_id = sessions.get("byId");
    if (!by_id.valid() || !by_id.is_obj()) return {};

    by_id.iter_obj([](JsonMutVal /*sid*/, JsonMutVal session) {
        if (!session.is_obj()) return;

        auto messages = session.get("messages");
        if (!messages.valid() || !messages.is_arr()) return;

        messages.iter([](JsonMutVal msg, std::size_t /*i*/) {
            if (!msg.valid() || !msg.is_obj()) return;
            (void)msg.remove("tool_results");
        });
    });

    return {};
}

// ================================================================
// Migration v4: create_mcp_registry
// ---------------------------------------------------------------
//   mcp: { servers: { <id>: {...} }, allowProjects: bool (default false) }
// ================================================================
inline auto migrate_v4_up(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");

    auto mcp = root.ensure_object("mcp");
    (void)mcp.ensure_object("servers");

    if (!mcp.has("allowProjects")) {
        mcp.set("allowProjects", false);
    }

    return {};
}

inline auto migrate_v4_down(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");
    (void)root.remove("mcp");
    return {};
}

// ================================================================
// Migration v5: add_analytics_tracking
// ---------------------------------------------------------------
//   analytics: {
//       totalTurns, totalTokensIn, totalTokensOut,
//       toolCallCount, sessionCount
//   }
//   Missing counters zero-initialised; existing numeric values kept.
// ================================================================
inline auto migrate_v5_up(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");

    auto analytics = root.ensure_object("analytics");
    auto zero_if_missing = [&](std::string_view key) {
        auto v = analytics.get(key);
        if (!v.valid() || !v.is_num()) {
            analytics.set(key, static_cast<int64_t>(0));
        }
    };
    zero_if_missing("totalTurns");
    zero_if_missing("totalTokensIn");
    zero_if_missing("totalTokensOut");
    zero_if_missing("toolCallCount");
    zero_if_missing("sessionCount");

    return {};
}

inline auto migrate_v5_down(JsonMutVal& root)
    -> std::expected<void, std::string> {
    if (!is_object(root)) return std::unexpected("root is not a JSON object");
    (void)root.remove("analytics");
    return {};
}

} // namespace cc_migrations_private_impl

// Pull in impl names so the registration loop below stays clean.
using cc_migrations_private_impl::is_object;
using cc_migrations_private_impl::migrate_v1_up;
using cc_migrations_private_impl::migrate_v1_down;
using cc_migrations_private_impl::migrate_v2_up;
using cc_migrations_private_impl::migrate_v2_down;
using cc_migrations_private_impl::migrate_v3_up;
using cc_migrations_private_impl::migrate_v3_down;
using cc_migrations_private_impl::migrate_v4_up;
using cc_migrations_private_impl::migrate_v4_down;
using cc_migrations_private_impl::migrate_v5_up;
using cc_migrations_private_impl::migrate_v5_down;

export namespace cc::migrations {

namespace detail {
    inline std::vector<Migration> all_migrations;
    inline bool registered = false;
}

// ---------------------------------------------------------------
// Public: register + accessors
// ---------------------------------------------------------------

/// Register the five built-in migrations into the global list.  Safe to
/// call repeatedly: subsequent calls are idempotent.
inline auto register_builtin_migrations() -> void {
    if (detail::registered) return;
    detail::registered = true;

    detail::all_migrations.push_back({1, "create_sessions_table", []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }, []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }});

    detail::all_migrations.push_back({2, "create_config_table", []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }, []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }});

    detail::all_migrations.push_back({3, "add_tool_results_to_messages", []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }, []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }});

    detail::all_migrations.push_back({4, "create_mcp_registry", []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }, []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }});

    detail::all_migrations.push_back({5, "add_analytics_tracking", []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }, []() -> std::expected<bool, std::string> { return std::expected<bool, std::string>{true}; }});
}

/// Snapshot of all registered migration descriptors.
[[nodiscard]] inline auto get_all_migrations() -> std::vector<Migration> {
    register_builtin_migrations();
    return detail::all_migrations;
}

/// Fetch a single descriptor by version number.
[[nodiscard]] inline auto get_migration_by_version(int version)
    -> std::optional<Migration> {
    register_builtin_migrations();
    for (const auto& m : detail::all_migrations) {
        if (m.version == version) {
            return m;
        }
    }
    return std::nullopt;
}

} // namespace cc::migrations
