/// @file buddy_companion.cppm
/// Companion generation: roll, hash, stats, and retrieval.
module;

#include <string>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <algorithm>

export module cc.buddy.buddy_companion;

import cc.buddy.buddy_types;

export namespace cc::buddy {

// -- Mulberry32 seeded PRNG -------------------------------------------------

class Mulberry32 {
public:
    explicit Mulberry32(uint32_t seed) : state_(seed) {}

    auto next() -> double {
        state_ += 0x6d2b79f5u;
        uint32_t t = state_;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t * (t ^ (t >> 7))) * 0x00d9aa0du;
        // Above approximates: t = (t + imul(t ^ (t>>>7), 61|t)) ^ t
        return static_cast<double>(t >> 0) / 4294967296.0;
    }

private:
    uint32_t state_;
};

// -- FNV-1a hash ------------------------------------------------------------

inline auto hash_string(const std::string& s) -> uint32_t {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
        h *= 16777619u;
    }
    return h;
}

// -- Internal helpers -------------------------------------------------------

namespace detail {

inline auto pick_species(Mulberry32& rng) -> Species {
    auto idx = static_cast<size_t>(rng.next() * static_cast<double>(ALL_SPECIES.size()));
    if (idx >= ALL_SPECIES.size()) idx = ALL_SPECIES.size() - 1;
    return ALL_SPECIES[idx];
}

inline auto pick_hat(Mulberry32& rng) -> Hat {
    auto idx = static_cast<size_t>(rng.next() * static_cast<double>(ALL_HATS.size()));
    if (idx >= ALL_HATS.size()) idx = ALL_HATS.size() - 1;
    return ALL_HATS[idx];
}

inline auto pick_eye(Mulberry32& rng) -> const std::string& {
    auto idx = static_cast<size_t>(rng.next() * static_cast<double>(EYES.size()));
    if (idx >= EYES.size()) idx = EYES.size() - 1;
    return EYES[idx];
}

inline auto pick_stat(Mulberry32& rng) -> StatName {
    auto idx = static_cast<size_t>(rng.next() * static_cast<double>(ALL_STATS.size()));
    if (idx >= ALL_STATS.size()) idx = ALL_STATS.size() - 1;
    return ALL_STATS[idx];
}

} // namespace detail

// -- Roll rarity ------------------------------------------------------------

inline auto roll_rarity(Mulberry32& rng) -> Rarity {
    double roll = rng.next() * static_cast<double>(TOTAL_RARITY_WEIGHT);
    for (int i = 0; i < 5; ++i) {
        roll -= static_cast<double>(RARITY_WEIGHTS[i]);
        if (roll < 0.0) return static_cast<Rarity>(i);
    }
    return Rarity::common;
}

// -- Roll stats -------------------------------------------------------------

inline auto roll_stats(Mulberry32& rng, Rarity rarity) -> std::unordered_map<StatName, int> {
    int rarity_idx = static_cast<int>(rarity);
    int floor = RARITY_FLOOR[rarity_idx];

    StatName peak = detail::pick_stat(rng);
    StatName dump = detail::pick_stat(rng);
    while (dump == peak) dump = detail::pick_stat(rng);

    std::unordered_map<StatName, int> stats;
    for (auto name : ALL_STATS) {
        if (name == peak) {
            stats[name] = std::min(100, floor + 50 + static_cast<int>(rng.next() * 30));
        } else if (name == dump) {
            stats[name] = std::max(1, floor - 10 + static_cast<int>(rng.next() * 15));
        } else {
            stats[name] = floor + static_cast<int>(rng.next() * 40);
        }
    }
    return stats;
}

// -- Roll from RNG ----------------------------------------------------------

inline auto roll_from(Mulberry32& rng) -> Roll {
    Rarity rarity = roll_rarity(rng);
    CompanionBones bones{
        .rarity  = rarity,
        .species = detail::pick_species(rng),
        .eye     = detail::pick_eye(rng),
        .hat     = (rarity == Rarity::common) ? Hat::none : detail::pick_hat(rng),
        .shiny   = rng.next() < 0.01,
        .stats   = roll_stats(rng, rarity),
    };
    return Roll{
        .bones           = std::move(bones),
        .inspiration_seed = static_cast<int64_t>(rng.next() * 1e9),
    };
}

// -- Salt for deterministic roll --------------------------------------------

inline constexpr const char* SALT = "friend-2026-401";

// -- Roll cache -------------------------------------------------------------

struct RollCache {
    std::string key;
    Roll value;
};

// -- Public API: roll by user ID --------------------------------------------

inline auto roll(const std::string& user_id) -> Roll {
    // Simple file-level cache via static
    static RollCache cache;
    std::string key = user_id + SALT;
    if (cache.key == key) return cache.value;

    Mulberry32 rng(hash_string(key));
    Roll value = roll_from(rng);

    cache.key   = std::move(key);
    cache.value = value;
    return cache.value;
}

// -- Public API: roll with explicit seed ------------------------------------

inline auto roll_with_seed(const std::string& seed) -> Roll {
    Mulberry32 rng(hash_string(seed));
    return roll_from(rng);
}

// -- Public API: get companion (bones + soul merged) ------------------------

inline auto get_companion(const std::optional<StoredCompanion>& stored,
                          const std::string& user_id) -> std::optional<Companion> {
    if (!stored.has_value()) return std::nullopt;

    Roll r = roll(user_id);
    Companion c;
    static_cast<CompanionBones&>(c) = std::move(r.bones);
    static_cast<CompanionSoul&>(c)  = static_cast<const CompanionSoul&>(*stored);
    c.hatched_at = stored->hatched_at;
    return c;
}

} // namespace cc::buddy
