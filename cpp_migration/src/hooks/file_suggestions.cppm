/// @file file_suggestions.cppm
/// @brief File path autocomplete for @-mentions in input.
/// Provides fuzzy file search with .gitignore respect, TTL caching,
/// and file icon detection. Faithful port of src/hooks/fileSuggestions.ts.
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

export module cc.hooks.file_suggestions;

import cc.utils.file_index;
import cc.utils.cwd;
import cc.utils.path_utils;
import cc.utils.gitignore;

export namespace cc::hooks {

namespace fs = std::filesystem;

// =========================================================================
// Constants
// =========================================================================

/// Maximum number of file suggestions returned.
/// TS REF: src/hooks/fileSuggestions.ts:617 (MAX_SUGGESTIONS = 15)
constexpr std::size_t MAX_SUGGESTIONS = 50;

/// Cache TTL in milliseconds. Refresh file list at most once per 5s.
/// TS REF: src/hooks/fileSuggestions.ts:635 (REFRESH_THROTTLE_MS = 5_000)
constexpr auto CACHE_TTL_MS = std::chrono::milliseconds{5000};

// =========================================================================
// FileSuggestion struct
// =========================================================================

/// A single file suggestion result.
/// TS REF: src/hooks/fileSuggestions.ts:603-612 (createFileSuggestionItem)
///          src/components/PromptInput/PromptInputFooterSuggestions.ts (SuggestionItem)
struct FileSuggestion {
    std::string path;          ///< Relative path from CWD
    std::string name;          ///< Filename (basename)
    std::string extension;     ///< File extension (without dot)
    std::string icon;          ///< Icon character or emoji for display
    double score = 0.0;        ///< Match quality (0.0 - 1.0, higher = better)
    bool is_directory = false; ///< True if entry is a directory
};

// =========================================================================
// File icon detection
// =========================================================================

/// Get a display icon for a file based on its extension.
/// TS REF: inferred from TS UI rendering (devicons / file-type icons)
/// Returns a short string suitable for terminal display.
[[nodiscard]] inline auto get_file_icon(std::string_view path) noexcept -> std::string {
    // Extract extension
    auto dot_pos = path.rfind('.');
    std::string_view ext;
    if (dot_pos != std::string_view::npos && dot_pos > 0) {
        ext = path.substr(dot_pos + 1);
    }

    // Check if it looks like a directory (trailing slash)
    if (!path.empty() && path.back() == '/') {
        return "╶"; // light left box drawing (folder indicator)
    }

    // Map common extensions to display characters
    // Using Unicode symbols that render well in terminals
    static const std::array<std::pair<std::string_view, std::string_view>, 40> icon_map = {{
        {"ts",      "TS"},
        {"tsx",     "TSX"},
        {"js",      "JS"},
        {"jsx",     "JSX"},
        {"cpp",     "C++"},
        {"cc",      "C++"},
        {"cxx",     "C++"},
        {"h",       "H"},
        {"hpp",     "H++"},
        {"c",       "C"},
        {"py",      "PY"},
        {"rs",      "RS"},
        {"go",      "GO"},
        {"rb",      "RB"},
        {"java",    "JV"},
        {"kt",      "KT"},
        {"swift",   "SW"},
        {"md",      "MD"},
        {"json",    "{}"},
        {"yaml",    "YM"},
        {"yml",     "YM"},
        {"toml",    "TM"},
        {"xml",     "XM"},
        {"html",    "<>"},
        {"css",     "#."},
        {"scss",    "#."},
        {"sh",      "$_"},
        {"bash",    "$_"},
        {"zsh",     "$_"},
        {"cmake",   "CM"},
        {"txt",     "TXT"},
        {"log",     "LOG"},
        {"pdf",     "PDF"},
        {"png",     "IMG"},
        {"jpg",     "IMG"},
        {"jpeg",    "IMG"},
        {"gif",     "IMG"},
        {"svg",     "SVG"},
        {"make",    "MK"},
    }};

    for (const auto& [e, icon] : icon_map) {
        if (ext == e) return std::string{icon};
    }

    // Default: first 2-3 chars of extension uppercase, or generic file marker
    if (!ext.empty()) {
        std::string result;
        for (std::size_t i = 0; i < std::min<std::size_t>(ext.size(), 3); ++i) {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(ext[i])));
        }
        return result;
    }
    return "___"; // underscores for files without extension
}

// =========================================================================
// Fuzzy matching helpers
// =========================================================================

namespace detail {

/// Compute a simple fuzzy match score (0.0 - 1.0) for a candidate path against a query.
/// Uses subsequence matching with position bonuses (fzf-style simplified).
/// TS REF: src/hooks/fileSuggestions.ts:618-626 (findMatchingFiles uses FileIndex.search)
///          src/native-ts/file-index/index.ts (Rust nucleo fuzzy matcher)
[[nodiscard]] inline auto fuzzy_match_score(std::string_view candidate, std::string_view query) noexcept -> double {
    if (query.empty()) return 1.0;
    if (candidate.empty()) return 0.0;

    // Normalize both to lowercase for case-insensitive matching
    std::string cand_lower;
    cand_lower.reserve(candidate.size());
    for (char c : candidate) cand_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string query_lower;
    query_lower.reserve(query.size());
    for (char c : query) query_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Subsequence matching: find query chars in order within candidate
    std::size_t query_idx = 0;
    std::size_t last_match_pos = 0;
    int consecutive_bonus = 0;
    int boundary_bonus = 0;
    int total_matches = 0;

    for (std::size_t i = 0; i < cand_lower.size() && query_idx < query_lower.size(); ++i) {
        if (cand_lower[i] == query_lower[query_idx]) {
            ++total_matches;
            // Bonus for consecutive matches
            if (i == last_match_pos + 1) {
                consecutive_bonus += 2;
            }
            // Bonus for boundary matches (after /, ., _, -)
            if (i > 0) {
                char prev = candidate[i - 1];
                if (prev == '/' || prev == '\\' || prev == '.' || prev == '_' || prev == '-') {
                    boundary_bonus += 3;
                }
                // camelCase boundary
                if (std::islower(static_cast<unsigned char>(prev)) && std::isupper(static_cast<unsigned char>(candidate[i]))) {
                    boundary_bonus += 2;
                }
            } else {
                boundary_bonus += 4; // first char match
            }
            last_match_pos = i;
            ++query_idx;
        }
    }

    if (query_idx < query_lower.size()) return 0.0; // not all chars found in order

    // Compute score: base + bonuses, normalized
    double base_score = static_cast<double>(total_matches) / static_cast<double>(query_lower.size());
    double bonuses = static_cast<double>(consecutive_bonus + boundary_bonus) / 100.0;
    double length_penalty = 1.0 - (static_cast<double>(candidate.size()) / 500.0);
    length_penalty = std::max(0.5, length_penalty);

    return std::min(1.0, base_score * 0.6 + bonuses + length_penalty * 0.1);
}

} // namespace detail

// =========================================================================
// Directory scanning with .gitignore respect
// =========================================================================

namespace detail {

/// Recursively scan a directory for files, respecting .gitignore patterns.
/// Returns a list of relative paths from base_dir.
/// TS REF: src/hooks/fileSuggestions.ts:248-383 (getFilesUsingGit -> git ls-files)
///          src/hooks/fileSuggestions.ts:459-516 (getProjectFiles -> ripgrep fallback)
[[nodiscard]] inline auto scan_directory(const fs::path& base_dir, bool respect_gitignore = true)
    -> std::vector<std::string>
{
    std::vector<std::string> results;
    std::error_code ec;

    if (!fs::exists(base_dir, ec) || !fs::is_directory(base_dir, ec)) {
        return results;
    }

    // Load .gitignore patterns if requested
    std::vector<cc::utils::GitignorePattern> ignore_patterns;
    if (respect_gitignore) {
        auto gitignore_path = base_dir / ".gitignore";
        if (fs::exists(gitignore_path, ec)) {
            std::ifstream file(gitignore_path);
            std::string line;
            while (std::getline(file, line)) {
                auto parsed = cc::utils::parse_gitignore_line(line);
                if (parsed) {
                    ignore_patterns.push_back(*parsed);
                }
            }
        }
    }

    // Helper: check if a path should be ignored
    auto is_ignored = [&](const fs::path& rel_path) -> bool {
        if (!respect_gitignore) return false;
        std::string rel_str = rel_path.string();
        // Always skip .git directory
        if (rel_str.starts_with(".git")) return true;
        // Check against patterns (simplified: just substring match for now)
        for (const auto& pat : ignore_patterns) {
            if (pat.negated) continue; // simplified: skip negated patterns
            if (rel_str.find(pat.pattern) != std::string::npos) return true;
        }
        return false;
    };

    // Walk directory recursively
    auto walk = [&](auto&& self, const fs::path& dir, const fs::path& rel_prefix) -> void {
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            auto rel = rel_prefix / entry.path().filename();
            std::string rel_str = rel.generic_string();

            if (is_ignored(rel)) continue;

            if (entry.is_directory(ec)) {
                results.push_back(rel_str + "/");
                self(self, entry.path(), rel);
            } else {
                results.push_back(rel_str);
            }
        }
    };

    walk(walk, base_dir, fs::path{});
    return results;
}

/// Extract directory names from a list of file paths.
/// For ['src/index.js', 'src/utils/helpers.js'] returns ['src/', 'src/utils/'].
/// TS REF: src/hooks/fileSuggestions.ts:393-440 (getDirectoryNames)
[[nodiscard]] inline auto get_directory_names(const std::vector<std::string>& files)
    -> std::vector<std::string>
{
    std::unordered_set<std::string> dirs;
    for (const auto& f : files) {
        auto pos = f.find_last_of('/');
        while (pos != std::string::npos && pos > 0) {
            auto dir = f.substr(0, pos + 1); // include trailing slash
            if (dirs.contains(dir)) break; // already processed this dir and parents
            dirs.insert(dir);
            pos = dir.substr(0, dir.size() - 1).find_last_of('/');
        }
    }
    return {dirs.begin(), dirs.end()};
}

/// Get basename of a path.
[[nodiscard]] inline auto get_basename(std::string_view path) noexcept -> std::string {
    auto pos = path.find_last_of('/');
    if (pos == std::string_view::npos) return std::string{path};
    return std::string{path.substr(pos + 1)};
}

/// Get extension of a file path (without dot).
[[nodiscard]] inline auto get_extension(std::string_view path) noexcept -> std::string {
    auto name = get_basename(path);
    auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return name.substr(dot + 1);
}

} // namespace detail

// =========================================================================
// Cached file list with TTL
// =========================================================================

namespace detail {

/// Thread-safe cache of scanned file paths with TTL.
/// TS REF: src/hooks/fileSuggestions.ts:33-78 (fileIndex singleton + cache state)
struct FileListCache {
    std::mutex mtx;
    std::vector<std::string> paths;
    std::vector<std::string> directories;
    std::chrono::steady_clock::time_point last_refresh{};
    bool valid = false;
};

inline auto get_file_list_cache() -> FileListCache& {
    static FileListCache cache;
    return cache;
}

} // namespace detail

// =========================================================================
// FileSuggestionsHook — main class
// =========================================================================

/// Hook that provides file path autocomplete suggestions.
/// Faithful port of src/hooks/fileSuggestions.ts: generateFileSuggestions,
/// startBackgroundCacheRefresh, applyFileSuggestion.
class FileSuggestionsHook {
public:
    FileSuggestionsHook() = default;

    /// Generate file suggestions for a partial path query.
    /// @param partial_path The partial file path typed by the user (e.g. "src/hooks/f")
    /// @param show_on_empty If true, return top-level entries even when partial_path is empty
    /// @param max_results Maximum number of results (capped at MAX_SUGGESTIONS)
    /// @return Vector of FileSuggestion sorted by match quality
    /// TS REF: src/hooks/fileSuggestions.ts:715-784 (generateFileSuggestions)
    [[nodiscard]] auto suggest(std::string_view partial_path, bool show_on_empty = false,
                               std::size_t max_results = MAX_SUGGESTIONS) const
        -> std::vector<FileSuggestion>
    {
        // If input is empty and we don't want to show on empty, return nothing
        if (partial_path.empty() && !show_on_empty) return {};

        max_results = std::min(max_results, MAX_SUGGESTIONS);

        // Ensure cache is fresh (non-blocking check)
        refresh_cache_if_stale();

        // Normalize the partial path
        std::string normalized{partial_path};
        // Strip leading "./" or ".\"
        if (normalized.starts_with("./")) {
            normalized = normalized.substr(2);
        }
        // Expand tilde
        if (normalized.starts_with('~')) {
            auto expanded = cc::utils::expand_tilde(normalized);
            normalized = expanded.string();
        }

        auto& cache = detail::get_file_list_cache();
        std::lock_guard<std::mutex> lock(cache.mtx);

        // If partial path is empty or just dot, return top-level entries
        if (normalized.empty() || normalized == "." || normalized == "./") {
            return top_level_suggestions(max_results);
        }

        // Build candidate list: files + directories
        std::vector<std::pair<std::string, double>> scored;
        scored.reserve(cache.paths.size() + cache.directories.size());

        // Score files
        for (const auto& p : cache.paths) {
            double score = detail::fuzzy_match_score(p, normalized);
            if (score > 0.0) {
                scored.emplace_back(p, score);
            }
        }

        // Score directories (slight boost for directories)
        for (const auto& d : cache.directories) {
            double score = detail::fuzzy_match_score(d, normalized);
            if (score > 0.0) {
                scored.emplace_back(d, score + 0.05); // directory boost
            }
        }

        // Sort by score descending
        std::sort(scored.begin(), scored.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        // Build result vector
        std::vector<FileSuggestion> results;
        results.reserve(std::min(max_results, scored.size()));
        for (std::size_t i = 0; i < scored.size() && results.size() < max_results; ++i) {
            const auto& [path, score] = scored[i];
            bool is_dir = !path.empty() && path.back() == '/';
            results.push_back(FileSuggestion{
                .path = path,
                .name = detail::get_basename(path),
                .extension = detail::get_extension(path),
                .icon = get_file_icon(path),
                .score = score,
                .is_directory = is_dir,
            });
        }

        return results;
    }

    /// Force a refresh of the file index cache.
    /// TS REF: src/hooks/fileSuggestions.ts:636-686 (startBackgroundCacheRefresh)
    auto refresh_index() -> void {
        auto& cache = detail::get_file_list_cache();
        std::lock_guard<std::mutex> lock(cache.mtx);
        build_cache_locked(cache);
    }

    /// Clear all cached data.
    /// TS REF: src/hooks/fileSuggestions.ts:84-99 (clearFileSuggestionCaches)
    auto clear_cache() -> void {
        auto& cache = detail::get_file_list_cache();
        std::lock_guard<std::mutex> lock(cache.mtx);
        cache.paths.clear();
        cache.directories.clear();
        cache.valid = false;
        cache.last_refresh = {};
    }

    /// Apply a file suggestion to the input text.
    /// Replaces the partial_path at start_pos with the suggestion text.
    /// TS REF: src/hooks/fileSuggestions.ts:789-811 (applyFileSuggestion)
    /// @param suggestion The selected suggestion text (file path)
    /// @param input Current input text
    /// @param partial_path The partial path that was matched
    /// @param start_pos Position in input where partial_path begins
    /// @return Pair of {new_input, new_cursor_pos}
    [[nodiscard]] static auto apply_suggestion(std::string_view suggestion,
                                               std::string_view input,
                                               std::string_view partial_path,
                                               std::size_t start_pos)
        -> std::pair<std::string, std::size_t>
    {
        std::string new_input;
        new_input.reserve(input.size() + suggestion.size());
        // Text before the partial path
        new_input.append(input.substr(0, start_pos));
        // The suggestion
        new_input.append(suggestion);
        // Text after the partial path
        auto end_pos = start_pos + partial_path.size();
        if (end_pos < input.size()) {
            new_input.append(input.substr(end_pos));
        }
        auto new_cursor = start_pos + suggestion.size();
        return {std::move(new_input), new_cursor};
    }

    /// Check if the cache is currently valid and within TTL.
    [[nodiscard]] auto is_cache_valid() const -> bool {
        auto& cache = detail::get_file_list_cache();
        std::lock_guard<std::mutex> lock(cache.mtx);
        if (!cache.valid) return false;
        auto now = std::chrono::steady_clock::now();
        return (now - cache.last_refresh) < CACHE_TTL_MS;
    }

private:
    // ─── Cache management ──────────────────────────────────────

    /// Refresh the cache if it's past TTL. Non-blocking for stale reads.
    auto refresh_cache_if_stale() const -> void {
        auto& cache = detail::get_file_list_cache();
        std::lock_guard<std::mutex> lock(cache.mtx);
        if (!cache.valid) {
            build_cache_locked(cache);
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if ((now - cache.last_refresh) >= CACHE_TTL_MS) {
            build_cache_locked(cache);
        }
    }

    /// Build the cache from filesystem scan. Caller must hold cache.mtx.
    auto build_cache_locked(detail::FileListCache& cache) const -> void {
        try {
            auto cwd = cc::utils::get_cwd();
            auto files = detail::scan_directory(cwd, /*respect_gitignore=*/true);
            auto dirs = detail::get_directory_names(files);

            cache.paths = std::move(files);
            cache.directories = std::move(dirs);
            cache.last_refresh = std::chrono::steady_clock::now();
            cache.valid = true;
        } catch (...) {
            cache.valid = false;
        }
    }

    /// Return top-level directory entries as suggestions. Caller must hold cache.mtx.
    [[nodiscard]] auto top_level_suggestions(std::size_t max_results) const
        -> std::vector<FileSuggestion>
    {
        auto& cache = detail::get_file_list_cache();
        // cache.mtx is already held by suggest()

        std::vector<FileSuggestion> results;
        // Collect top-level files and dirs from cache
        std::vector<std::string> top_level;
        for (const auto& p : cache.paths) {
            if (p.find('/') == std::string::npos) {
                top_level.push_back(p);
            }
        }
        for (const auto& d : cache.directories) {
            // Top-level dirs have exactly one slash at the end
            auto slash_count = std::count(d.begin(), d.end(), '/');
            if (slash_count == 1) {
                top_level.push_back(d);
            }
        }

        // Sort alphabetically
        std::sort(top_level.begin(), top_level.end());

        for (std::size_t i = 0; i < top_level.size() && results.size() < max_results; ++i) {
            const auto& path = top_level[i];
            bool is_dir = !path.empty() && path.back() == '/';
            results.push_back(FileSuggestion{
                .path = path,
                .name = detail::get_basename(path),
                .extension = detail::get_extension(path),
                .icon = get_file_icon(path),
                .score = 1.0,
                .is_directory = is_dir,
            });
        }
        return results;
    }
};

// =========================================================================
// Free functions for convenience
// =========================================================================

/// Generate file suggestions (convenience free function).
/// TS REF: src/hooks/fileSuggestions.ts:715 (generateFileSuggestions)
[[nodiscard]] inline auto generate_file_suggestions(std::string_view partial_path,
                                                     bool show_on_empty = false,
                                                     std::size_t max_results = MAX_SUGGESTIONS)
    -> std::vector<FileSuggestion>
{
    static FileSuggestionsHook hook;
    return hook.suggest(partial_path, show_on_empty, max_results);
}

/// Clear all file suggestion caches.
/// TS REF: src/hooks/fileSuggestions.ts:84 (clearFileSuggestionCaches)
inline auto clear_file_suggestion_caches() -> void {
    static FileSuggestionsHook hook;
    hook.clear_cache();
}

/// Find the longest common prefix among suggestion display texts.
/// TS REF: src/hooks/fileSuggestions.ts:587-598 (findLongestCommonPrefix)
[[nodiscard]] inline auto find_longest_common_prefix(const std::vector<FileSuggestion>& suggestions)
    -> std::string
{
    if (suggestions.empty()) return "";

    std::string prefix = suggestions[0].path;
    for (std::size_t i = 1; i < suggestions.size(); ++i) {
        const auto& current = suggestions[i].path;
        std::size_t j = 0;
        while (j < std::min(prefix.size(), current.size()) && prefix[j] == current[j]) ++j;
        prefix = prefix.substr(0, j);
        if (prefix.empty()) return "";
    }
    return prefix;
}

} // namespace cc::hooks
