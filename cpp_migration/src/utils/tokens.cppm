module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module cc.utils.tokens;

export namespace cc::utils {

// ─── Token Usage ─────────────────────────────────────────────────────────────

/// Token usage data from an API response
struct TokenUsage {
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_creation_input_tokens = 0;
    int64_t cache_read_input_tokens = 0;
};

/// Get total context window tokens from usage data.
/// Includes input_tokens + cache tokens + output_tokens.
int64_t get_token_count_from_usage(const TokenUsage& usage);

/// Token estimation for a string (approximate tokens from character count)
int64_t estimate_token_count(std::string_view text);

/// Rough token count estimation for messages (sum of content sizes / bytes_per_token)
int64_t rough_token_count_estimation(const std::vector<std::string>& messages);

/// Get the token count from the last API response in a message list
int64_t token_count_from_last_api_response(
    const std::vector<TokenUsage>& usage_history);

/// Final context tokens from last response (for budget computation)
int64_t final_context_tokens_from_last_response(
    const std::vector<TokenUsage>& usage_history);

// ─── Token Budget Constants ──────────────────────────────────────────────────

/// Approximate bytes per token (for estimation)
inline constexpr int64_t BYTES_PER_TOKEN = 4;

/// Default maximum result size in characters
inline constexpr int64_t DEFAULT_MAX_RESULT_SIZE_CHARS = 50000;

/// Maximum tool result bytes
inline constexpr int64_t MAX_TOOL_RESULT_BYTES = 200000;

/// Maximum total tool results per message in characters
inline constexpr int64_t MAX_TOOL_RESULTS_PER_MESSAGE_CHARS = 800000;

// ─── Completion Cache ────────────────────────────────────────────────────────

/// A cached completion entry
struct CachedCompletion {
    std::string content;
    std::string model;
    std::chrono::steady_clock::time_point cached_at;
    int64_t token_count = 0;
};

/// Cache key for completion lookups
struct CompletionCacheKey {
    std::string prompt_hash;
    std::string model;

    bool operator==(const CompletionCacheKey& other) const {
        return prompt_hash == other.prompt_hash && model == other.model;
    }
};

/// Hash function for CompletionCacheKey
struct CompletionCacheKeyHash {
    size_t operator()(const CompletionCacheKey& key) const {
        size_t h1 = std::hash<std::string>{}(key.prompt_hash);
        size_t h2 = std::hash<std::string>{}(key.model);
        return h1 ^ (h2 << 1);
    }
};

/// Configuration for the completion cache
struct CompletionCacheConfig {
    size_t max_entries = 1000;
    std::chrono::seconds ttl{3600};  // 1 hour default
};

/// Thread-safe completion response cache with LRU eviction and TTL.
class CompletionCache {
public:
    explicit CompletionCache(CompletionCacheConfig config);

    /// Default configuration
    CompletionCache();

    /// Look up a cached completion
    std::optional<CachedCompletion> get(const CompletionCacheKey& key);

    /// Store a completion in the cache
    void put(const CompletionCacheKey& key, CachedCompletion entry);

    /// Invalidate a specific cache entry
    void invalidate(const CompletionCacheKey& key);

    /// Invalidate all entries for a given model
    void invalidate_model(std::string_view model);

    /// Clear the entire cache
    void clear();

    /// Get current number of entries
    size_t size() const;

    /// Get cache hit statistics
    struct CacheStats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
    };
    CacheStats stats() const;

private:
    void evict_expired();
    void evict_lru();

    CompletionCacheConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<CompletionCacheKey, CachedCompletion, CompletionCacheKeyHash> entries_;
    // LRU tracking: most recently used keys at the back
    std::vector<CompletionCacheKey> lru_order_;
    mutable CacheStats stats_;
};

} // namespace cc::utils
