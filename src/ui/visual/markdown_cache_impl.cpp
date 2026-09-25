// markdown_cache_impl.cpp - impl unit for cc.ui.visual.markdown (RFC 0001
// Phase C batch 6). The single out-of-line definitions of TokenCache's
// ctor/find/put (CacheEntry + lru_/map_ stay private members declared in the
// interface - needed for class layout) and the SINGLE definition of
// global_token_cache() (Meyers function-local static; strong 'T' symbol,
// no inline duplicate).
module;

module cc.ui.visual.markdown;

import std;

namespace cc::ui {
namespace detail {

TokenCache::TokenCache(std::size_t max_size) : max_size_(max_size) {}

const std::vector<BlockToken>* TokenCache::find(std::string_view key) const {
        auto it = map_.find(std::string(key));
        if (it == map_.end()) return nullptr;
        // Promote to MRU
        lru_.splice(lru_.begin(), lru_, it->second);
        return &it->second->tokens;
    }

void TokenCache::put(std::string key, std::vector<BlockToken> tokens) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update existing and promote
            it->second->tokens = std::move(tokens);
            lru_.splice(lru_.begin(), lru_, it->second);
            return;
        }

        // Evict if at capacity
        if (lru_.size() >= max_size_) {
            auto& last = lru_.back();
            map_.erase(last.key);
            lru_.pop_back();
        }

        // Insert new entry at front (MRU)
        lru_.emplace_front(CacheEntry{key, std::move(tokens)});
        map_.emplace(std::move(key), lru_.begin());
    }

/// Global token cache instance
TokenCache& global_token_cache() {
    static TokenCache cache(256);
    return cache;
}

} // namespace detail
} // namespace cc::ui
