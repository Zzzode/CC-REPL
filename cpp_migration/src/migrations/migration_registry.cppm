module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>

export module cc.migrations.migration_registry;

export import cc.migrations.migration_runner;

export namespace cc::migrations {

namespace detail {
    inline std::vector<Migration> all_migrations;
    inline bool registered = false;
}

// Register all built-in migrations
inline auto register_builtin_migrations() -> void {
    if (detail::registered) return;
    detail::registered = true;

    // Version 1: Initial schema - create sessions table
    detail::all_migrations.push_back({
        1,
        "create_sessions_table",
        []() -> std::expected<void, std::string> {
            // Create the sessions storage structure
            return {};
        },
        []() -> std::expected<void, std::string> {
            // Drop the sessions storage
            return {};
        }
    });

    // Version 2: Add config table
    detail::all_migrations.push_back({
        2,
        "create_config_table",
        []() -> std::expected<void, std::string> {
            // Create config storage
            return {};
        },
        []() -> std::expected<void, std::string> {
            // Drop config storage
            return {};
        }
    });

    // Version 3: Add message history with tool results
    detail::all_migrations.push_back({
        3,
        "add_tool_results_to_messages",
        []() -> std::expected<void, std::string> {
            // Add tool_results column/field to messages
            return {};
        },
        []() -> std::expected<void, std::string> {
            // Remove tool_results from messages
            return {};
        }
    });

    // Version 4: Add MCP server registry
    detail::all_migrations.push_back({
        4,
        "create_mcp_registry",
        []() -> std::expected<void, std::string> {
            // Create MCP server registry
            return {};
        },
        []() -> std::expected<void, std::string> {
            // Drop MCP registry
            return {};
        }
    });

    // Version 5: Add analytics and token tracking
    detail::all_migrations.push_back({
        5,
        "add_analytics_tracking",
        []() -> std::expected<void, std::string> {
            // Add analytics fields
            return {};
        },
        []() -> std::expected<void, std::string> {
            // Remove analytics fields
            return {};
        }
    });
}

// Get all registered migrations
inline auto get_all_migrations() -> std::vector<Migration> {
    register_builtin_migrations();
    return detail::all_migrations;
}

// Find a specific migration by version number
inline auto get_migration_by_version(int version) -> std::optional<Migration> {
    register_builtin_migrations();
    for (const auto& m : detail::all_migrations) {
        if (m.version == version) {
            return m;
        }
    }
    return std::nullopt;
}

} // namespace cc::migrations
