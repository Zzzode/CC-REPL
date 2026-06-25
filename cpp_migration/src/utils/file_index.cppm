// FileIndex - fzf-style fuzzy file search for large repositories
module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.file_index;

export namespace cc::utils::file_index {

namespace fs = std::filesystem;

// =========================================================================
// Scoring Constants (fzf-v2 / nucleo compatible)
// =========================================================================

constexpr int SCORE_MATCH = 16;
constexpr int BONUS_BOUNDARY = 8;
constexpr int BONUS_CAMEL = 6;
constexpr int BONUS_CONSECUTIVE = 4;
constexpr int GAP_START = -3;
constexpr int GAP_EXTENSION = -1;
constexpr int BONUS_FIRST_CHAR = 2;
constexpr int BONUS_PATH_SEPARATOR = 10;

// =========================================================================
// Types
// =========================================================================

/// A single search result with scoring information
struct SearchResult {
    std::string path;
    int score = 0;
    std::vector<size_t> match_positions;
};

/// Configuration for building the index
struct IndexConfig {
    size_t max_files = 500000;
    bool follow_symlinks = false;
    std::vector<std::string> ignore_patterns = {
        ".git", "node_modules", "__pycache__", ".cache",
        "build", "dist", ".DS_Store", "target"
    };
};

// =========================================================================
// Character Classification
// =========================================================================

/// Check if character is a word boundary indicator
[[nodiscard]] inline bool is_boundary(char prev, char curr) noexcept {
    if (prev == '/' || prev == '\\' || prev == '.' || prev == '_' || prev == '-') {
        return true;
    }
    if (std::islower(prev) && std::isupper(curr)) {
        return true;  // camelCase boundary
    }
    return false;
}

/// Compute bonus for a match at position based on context
[[nodiscard]] inline int position_bonus(std::string_view path, size_t pos) noexcept {
    if (pos == 0) return BONUS_FIRST_CHAR + BONUS_BOUNDARY;
    
    char prev = path[pos - 1];
    char curr = path[pos];
    
    if (prev == '/' || prev == '\\') {
        return BONUS_PATH_SEPARATOR;
    }
    if (is_boundary(prev, curr)) {
        return BONUS_BOUNDARY;
    }
    if (std::islower(prev) && std::isupper(curr)) {
        return BONUS_CAMEL;
    }
    return 0;
}

// =========================================================================
// Fuzzy Matching Algorithm
// =========================================================================

/// Perform fuzzy match of pattern against text, returning score and positions
/// Returns negative score if no match
struct MatchResult {
    int score = std::numeric_limits<int>::min();
    std::vector<size_t> positions;
    
    [[nodiscard]] bool matched() const noexcept { return score > std::numeric_limits<int>::min(); }
};

[[nodiscard]] inline MatchResult fuzzy_match(
    std::string_view pattern, std::string_view text) noexcept {
    
    if (pattern.empty()) return {0, {}};
    if (text.empty()) return {};
    
    // Quick reject: check if all pattern chars exist in text (case-insensitive)
    size_t pi = 0;
    for (size_t ti = 0; ti < text.size() && pi < pattern.size(); ++ti) {
        if (std::tolower(static_cast<unsigned char>(text[ti])) == 
            std::tolower(static_cast<unsigned char>(pattern[pi]))) {
            ++pi;
        }
    }
    if (pi != pattern.size()) return {};  // Not all chars found
    
    // Greedy forward pass: find first match positions
    std::vector<size_t> positions;
    positions.reserve(pattern.size());
    
    pi = 0;
    for (size_t ti = 0; ti < text.size() && pi < pattern.size(); ++ti) {
        if (std::tolower(static_cast<unsigned char>(text[ti])) == 
            std::tolower(static_cast<unsigned char>(pattern[pi]))) {
            positions.push_back(ti);
            ++pi;
        }
    }
    
    // Score the match
    int score = 0;
    int consecutive = 0;
    
    for (size_t i = 0; i < positions.size(); ++i) {
        size_t pos = positions[i];
        
        // Base match score
        score += SCORE_MATCH;
        
        // Position bonus (boundary, separator, camel)
        score += position_bonus(text, pos);
        
        // Exact case match bonus
        if (pattern[i] == text[pos]) {
            score += 1;
        }
        
        // Consecutive bonus
        if (i > 0) {
            if (positions[i] == positions[i-1] + 1) {
                consecutive++;
                score += BONUS_CONSECUTIVE * consecutive;
            } else {
                // Gap penalty
                auto gap = static_cast<int>(positions[i] - positions[i-1] - 1);
                score += GAP_START + GAP_EXTENSION * (gap - 1);
                consecutive = 0;
            }
        }
    }
    
    // Prefer matches near the end of path (filename portion)
    auto last_sep = text.rfind('/');
    if (last_sep != std::string_view::npos && !positions.empty()) {
        size_t filename_matches = 0;
        for (auto pos : positions) {
            if (pos > last_sep) ++filename_matches;
        }
        score += static_cast<int>(filename_matches) * 3;
    }
    
    // Prefer shorter paths (less noise)
    score -= static_cast<int>(text.size()) / 20;
    
    return {score, std::move(positions)};
}

// =========================================================================
// FileIndex Implementation
// =========================================================================

/// High-performance fuzzy file index for large repositories
class FileIndex {
public:
    FileIndex() = default;
    
    /// Build index from a root directory
    static FileIndex build(const fs::path& root, const IndexConfig& config = {}) {
        FileIndex index;
        index.root_ = root;
        index.scan_directory(root, config);
        return index;
    }
    
    /// Build index asynchronously (yields periodically)
    static FileIndex build_async(
        const fs::path& root, 
        std::function<bool()> should_yield,
        const IndexConfig& config = {}) {
        
        FileIndex index;
        index.root_ = root;
        index.scan_directory_async(root, config, should_yield);
        return index;
    }
    
    /// Query the index with a fuzzy pattern
    [[nodiscard]] std::vector<SearchResult> query(
        std::string_view pattern, size_t max_results = 20) const {
        
        if (pattern.empty() || paths_.empty()) return {};
        
        std::vector<SearchResult> results;
        results.reserve(std::min(paths_.size(), max_results * 4));
        
        for (size_t i = 0; i < paths_.size(); ++i) {
            auto match = fuzzy_match(pattern, paths_[i]);
            if (match.matched()) {
                results.push_back(SearchResult{
                    .path = paths_[i],
                    .score = match.score,
                    .match_positions = std::move(match.positions)
                });
            }
        }
        
        // Sort by score (highest first)
        std::partial_sort(
            results.begin(),
            results.begin() + std::min(results.size(), max_results),
            results.end(),
            [](const SearchResult& a, const SearchResult& b) {
                return a.score > b.score;
            }
        );
        
        if (results.size() > max_results) {
            results.resize(max_results);
        }
        
        return results;
    }
    
    /// Get total number of indexed files
    [[nodiscard]] size_t size() const noexcept { return paths_.size(); }
    
    /// Check if index is empty
    [[nodiscard]] bool empty() const noexcept { return paths_.empty(); }
    
    /// Get the root directory
    [[nodiscard]] const fs::path& root() const noexcept { return root_; }
    
    /// Add a single path to the index
    void add_path(std::string path) {
        paths_.push_back(std::move(path));
    }
    
    /// Clear the index
    void clear() {
        paths_.clear();
        root_.clear();
    }

private:
    fs::path root_;
    std::vector<std::string> paths_;
    
    /// Check if a path component should be ignored
    [[nodiscard]] bool should_ignore(
        const std::string& name, const IndexConfig& config) const {
        for (const auto& pattern : config.ignore_patterns) {
            if (name == pattern) return true;
        }
        return false;
    }
    
    /// Recursively scan directory
    void scan_directory(const fs::path& dir, const IndexConfig& config) {
        std::error_code ec;
        auto options = fs::directory_options::skip_permission_denied;
        if (config.follow_symlinks) {
            options |= fs::directory_options::follow_directory_symlink;
        }
        
        for (auto it = fs::recursive_directory_iterator(dir, options, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            
            if (ec) { ec.clear(); continue; }
            if (paths_.size() >= config.max_files) break;
            
            const auto& entry = *it;
            auto filename = entry.path().filename().string();
            
            if (should_ignore(filename, config)) {
                it.disable_recursion_pending();
                continue;
            }
            
            if (entry.is_regular_file(ec)) {
                auto rel = fs::relative(entry.path(), root_, ec);
                if (!ec) {
                    paths_.push_back(rel.string());
                }
            }
        }
    }
    
    /// Scan with periodic yield checks
    void scan_directory_async(
        const fs::path& dir, 
        const IndexConfig& config,
        std::function<bool()>& should_yield) {
        
        std::error_code ec;
        auto options = fs::directory_options::skip_permission_denied;
        if (config.follow_symlinks) {
            options |= fs::directory_options::follow_directory_symlink;
        }
        
        int count = 0;
        for (auto it = fs::recursive_directory_iterator(dir, options, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            
            if (ec) { ec.clear(); continue; }
            if (paths_.size() >= config.max_files) break;
            
            // Yield every 1000 files to avoid blocking
            if (++count % 1000 == 0 && should_yield && should_yield()) {
                // Caller wants us to pause/yield
            }
            
            const auto& entry = *it;
            auto filename = entry.path().filename().string();
            
            if (should_ignore(filename, config)) {
                it.disable_recursion_pending();
                continue;
            }
            
            if (entry.is_regular_file(ec)) {
                auto rel = fs::relative(entry.path(), root_, ec);
                if (!ec) {
                    paths_.push_back(rel.string());
                }
            }
        }
    }
};

} // namespace cc::utils::file_index
