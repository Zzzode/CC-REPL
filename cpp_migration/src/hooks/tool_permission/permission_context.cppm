module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <chrono>
#include <expected>
#include <cstdint>

export module cc.hooks.permission_context;

export namespace cc::hooks::tool_permission {

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class PermissionLevel {
    Denied,
    AskEveryTime,
    AllowOnce,
    AllowSession,
    AllowAlways,
};

enum class PermissionScope {
    Global,
    Project,
    Session,
};

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

struct PermissionEntry {
    std::string tool_name;
    PermissionLevel level;
    PermissionScope scope;
    std::optional<std::string> pattern;
    std::chrono::system_clock::time_point granted_at;
};

struct PermissionQuery {
    std::string tool_name;
    std::string operation;
    std::vector<std::string> args;
    std::optional<std::string> working_directory;
};

struct PermissionDecision {
    PermissionLevel level;
    bool user_prompted;
    std::optional<std::string> reason;
};

struct PermissionLogEntry {
    PermissionQuery query;
    PermissionDecision decision;
    std::chrono::system_clock::time_point timestamp;
    std::string session_id;
};

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

/// Thread-local storage for permission entries and logs
namespace detail {
    inline std::vector<PermissionEntry>& entries_store() {
        static std::vector<PermissionEntry> entries;
        return entries;
    }
    inline std::vector<PermissionLogEntry>& log_store() {
        static std::vector<PermissionLogEntry> logs;
        return logs;
    }
    inline std::size_t& allowed_count() { static std::size_t c = 0; return c; }
    inline std::size_t& denied_count() { static std::size_t c = 0; return c; }
}

inline PermissionDecision query_permission(PermissionQuery query) {
    auto& entries = detail::entries_store();
    for (const auto& entry : entries) {
        if (entry.tool_name == query.tool_name ||
            entry.tool_name == "*") {
            // Check scope match (pattern matching)
            if (entry.pattern && !query.args.empty()) {
                // Simple prefix match for path patterns
                bool matches = query.args[0].starts_with(*entry.pattern);
                if (!matches) continue;
            }
            return PermissionDecision{
                .level = entry.level,
                .user_prompted = false,
                .reason = "matched rule"
            };
        }
    }
    return PermissionDecision{
        .level = PermissionLevel::AskEveryTime,
        .user_prompted = false,
        .reason = std::nullopt,
    };
}

inline void grant_permission(
    std::string_view tool_name,
    PermissionLevel level,
    PermissionScope scope,
    std::optional<std::string_view> pattern
) {
    detail::entries_store().push_back(PermissionEntry{
        .tool_name = std::string(tool_name),
        .level = level,
        .scope = scope,
        .pattern = pattern ? std::optional<std::string>(std::string(*pattern)) : std::nullopt,
        .granted_at = std::chrono::system_clock::now()
    });
}

inline void revoke_permission(
    std::string_view tool_name,
    PermissionScope scope
) {
    auto& entries = detail::entries_store();
    std::erase_if(entries, [&](const PermissionEntry& e) {
        return e.tool_name == tool_name && e.scope == scope;
    });
}

inline std::vector<PermissionEntry> get_permission_entries(
    std::optional<std::string_view> tool_name
) {
    auto& entries = detail::entries_store();
    if (!tool_name) return entries;
    std::vector<PermissionEntry> result;
    for (const auto& e : entries) {
        if (e.tool_name == *tool_name) result.push_back(e);
    }
    return result;
}

inline void clear_session_permissions() {
    auto& entries = detail::entries_store();
    std::erase_if(entries, [](const PermissionEntry& e) {
        return e.scope == PermissionScope::Session;
    });
}

inline void log_permission_decision(PermissionLogEntry entry) {
    if (entry.decision.level == PermissionLevel::Denied) {
        detail::denied_count()++;
    } else {
        detail::allowed_count()++;
    }
    detail::log_store().push_back(std::move(entry));
    // Cap log at 1000 entries
    if (detail::log_store().size() > 1000) {
        detail::log_store().erase(detail::log_store().begin());
    }
}

inline std::vector<PermissionLogEntry> get_permission_log(
    std::optional<std::size_t> limit
) {
    auto& logs = detail::log_store();
    if (!limit || *limit >= logs.size()) return logs;
    return std::vector<PermissionLogEntry>(logs.end() - static_cast<std::ptrdiff_t>(*limit), logs.end());
}

inline std::pair<std::size_t, std::size_t> get_permission_stats() {
    return {detail::allowed_count(), detail::denied_count()};
}

inline std::string export_permission_config() {
    std::string json = "[";
    bool first = true;
    for (const auto& e : detail::entries_store()) {
        if (!first) json += ",";
        first = false;
        json += std::format(R"({{"tool":"{}","level":{},"scope":{}}})",
            e.tool_name, static_cast<int>(e.level), static_cast<int>(e.scope));
    }
    json += "]";
    return json;
}

inline std::expected<void, std::string> import_permission_config(
    std::string_view json
) {
    // Basic validation
    if (json.empty() || json.front() != '[') {
        return std::unexpected("Invalid JSON: expected array");
    }
    return {};
}

} // namespace cc::hooks::tool_permission
