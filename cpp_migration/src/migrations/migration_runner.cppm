module;
#include <string>
#include <vector>
#include <functional>
#include <expected>
#include <algorithm>

export module cc.migrations.migration_runner;

export namespace cc::migrations {

// A single migration with up/down operations
struct Migration {
    int version = 0;
    std::string name;
    std::function<std::expected<void, std::string>()> up;
    std::function<std::expected<void, std::string>()> down;
};

// Runs database migrations in order, tracking applied versions
class MigrationRunner {
public:
    MigrationRunner() = default;

    // Register a migration to be available for running
    auto register_migration(Migration migration) -> void {
        migrations_.push_back(std::move(migration));
        // Keep sorted by version
        std::sort(migrations_.begin(), migrations_.end(),
                  [](const Migration& a, const Migration& b) {
                      return a.version < b.version;
                  });
    }

    // Run all pending migrations that haven't been applied yet
    auto run_pending() -> std::expected<int, std::string> {
        int applied_count = 0;

        for (const auto& migration : migrations_) {
            if (migration.version <= current_version_) {
                continue; // Already applied
            }

            // Execute the up migration
            auto result = migration.up();
            if (!result.has_value()) {
                return std::unexpected(
                    "Migration v" + std::to_string(migration.version) +
                    " (" + migration.name + ") failed: " + result.error());
            }

            current_version_ = migration.version;
            ++applied_count;
        }

        return applied_count;
    }

    // Rollback the given number of migrations
    auto rollback(int steps = 1) -> std::expected<int, std::string> {
        int rolled_back = 0;

        // Find migrations to rollback (in reverse order)
        for (auto it = migrations_.rbegin(); it != migrations_.rend() && rolled_back < steps; ++it) {
            if (it->version > current_version_) continue;
            if (it->version <= current_version_ - rolled_back) continue;

            // Execute the down migration
            auto result = it->down();
            if (!result.has_value()) {
                return std::unexpected(
                    "Rollback of v" + std::to_string(it->version) +
                    " (" + it->name + ") failed: " + result.error());
            }

            ++rolled_back;
        }

        current_version_ -= rolled_back;
        if (current_version_ < 0) current_version_ = 0;

        return rolled_back;
    }

    // Get the current schema version
    [[nodiscard]] auto get_current_version() const -> int {
        return current_version_;
    }

    // Set the current version (for initialization from stored state)
    auto set_current_version(int version) -> void {
        current_version_ = version;
    }

    // Get all registered migrations
    [[nodiscard]] auto get_migrations() const -> const std::vector<Migration>& {
        return migrations_;
    }

private:
    std::vector<Migration> migrations_;
    int current_version_ = 0;
};

} // namespace cc::migrations
