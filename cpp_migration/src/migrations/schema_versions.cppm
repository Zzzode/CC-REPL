module;
#include <string>
#include <fstream>
#include <filesystem>

export module cc.migrations.schema_versions;

export namespace cc::migrations {

// The target schema version for the current application build
inline constexpr int CURRENT_SCHEMA_VERSION = 5;

namespace detail {

// Get the path to the schema version file
inline auto get_version_file_path() -> std::filesystem::path {
    auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
    auto config_dir = home / ".config" / "cc-repl";
    return config_dir / "schema_version";
}

} // namespace detail

// Read the stored schema version from disk
inline auto get_stored_version() -> int {
    auto path = detail::get_version_file_path();
    if (!std::filesystem::exists(path)) {
        return 0; // No version file means fresh install
    }

    std::ifstream f(path);
    int version = 0;
    f >> version;
    return version;
}

// Write the schema version to disk
inline auto set_stored_version(int version) -> void {
    auto path = detail::get_version_file_path();

    // Ensure directory exists
    std::filesystem::create_directories(path.parent_path());

    std::ofstream f(path);
    f << version;
}

// Check if the database needs migration
inline auto needs_migration() -> bool {
    return get_stored_version() < CURRENT_SCHEMA_VERSION;
}

} // namespace cc::migrations
