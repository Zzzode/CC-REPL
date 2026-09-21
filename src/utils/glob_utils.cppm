// Glob pattern matching with gitignore-style exclusions
// Sources: glob.ts
module;

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.glob_utils;

import cc.utils.async;

export namespace cc::utils::glob {

// =========================================================================
// GlobOptions - Configuration for glob pattern matching
// =========================================================================
struct GlobOptions {
    std::filesystem::path cwd;
    bool dot;                              // include dotfiles
    bool absolute;                         // return absolute paths
    std::optional<size_t> max_depth;       // directory traversal depth limit
    std::vector<std::string> ignore;       // gitignore-style exclusion patterns
    bool follow_symlinks;                  // follow symbolic links
    bool only_files;                       // exclude directories from results
    bool only_directories;                 // exclude files from results
};

/// Overload: create GlobOptions with just cwd
[[nodiscard]] inline GlobOptions make_glob_options(std::filesystem::path cwd) {
    return GlobOptions{
        .cwd = std::move(cwd),
        .dot = false,
        .absolute = false,
        .max_depth = std::nullopt,
        .ignore = {},
        .follow_symlinks = true,
        .only_files = false,
        .only_directories = false,
    };
}

/// Overload: create GlobOptions with cwd and ignore list
[[nodiscard]] inline GlobOptions make_glob_options(
    std::filesystem::path cwd,
    std::vector<std::string> ignore
) {
    return GlobOptions{
        .cwd = std::move(cwd),
        .dot = false,
        .absolute = false,
        .max_depth = std::nullopt,
        .ignore = std::move(ignore),
        .follow_symlinks = true,
        .only_files = false,
        .only_directories = false,
    };
}

// =========================================================================
// Glob results
// =========================================================================
struct GlobResult {
    std::vector<std::filesystem::path> matches;
    size_t files_scanned;
    size_t dirs_scanned;
};

// =========================================================================
// Async glob - walks directory tree asynchronously
// =========================================================================

/// Match files against a glob pattern (async)
[[nodiscard]] async::Task<std::expected<GlobResult, std::string>>
glob(
    std::string_view pattern,
    const GlobOptions& options
);

/// Match files against multiple glob patterns (async)
[[nodiscard]] async::Task<std::expected<GlobResult, std::string>>
glob(
    const std::vector<std::string_view>& patterns,
    const GlobOptions& options
);

// =========================================================================
// Synchronous glob - for use in non-async contexts
// =========================================================================

/// Match files against a glob pattern (synchronous)
[[nodiscard]] std::expected<GlobResult, std::string>
glob_sync(
    std::string_view pattern,
    const GlobOptions& options
);

/// Match files against multiple glob patterns (synchronous)
[[nodiscard]] std::expected<GlobResult, std::string>
glob_sync(
    const std::vector<std::string_view>& patterns,
    const GlobOptions& options
);

// =========================================================================
// Pattern utilities
// =========================================================================

/// Test if a single path matches a glob pattern
[[nodiscard]] bool matches_pattern(
    std::string_view path,
    std::string_view pattern
);

/// Test if a path should be ignored based on gitignore-style patterns
[[nodiscard]] bool is_ignored(
    std::string_view path,
    const std::vector<std::string>& ignore_patterns
);

/// Convert a gitignore pattern to a glob pattern
[[nodiscard]] std::string gitignore_to_glob(std::string_view gitignore_pattern);

} // namespace cc::utils::glob
