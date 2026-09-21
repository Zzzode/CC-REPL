/// @file buddy_types.cppm
/// Buddy/Companion system type definitions.
module;

#include <string>
#include <cstdint>
#include <unordered_map>
#include <array>
#include <vector>

export module cc.buddy.buddy_types;

export namespace cc::buddy {

// -- Enumerations -----------------------------------------------------------

enum class Rarity  { common, uncommon, rare, epic, legendary };
enum class Species { duck, goose, blob, cat, dragon, octopus, owl,
                     penguin, turtle, snail, ghost, axolotl, capybara,
                     cactus, robot, rabbit, mushroom, chonk };
enum class Hat     { none, crown, tophat, propeller, halo, wizard,
                     beanie, tinyduck };
enum class StatName{ DEBUGGING, PATIENCE, CHAOS, WISDOM, SNARK };

// -- Structs ----------------------------------------------------------------

struct CompanionBones {
    Rarity rarity;
    Species species;
    std::string eye;                           // one of: · ✦ × ◉ @ °
    Hat hat;
    bool shiny;
    std::unordered_map<StatName, int> stats;
};

struct CompanionSoul {
    std::string name;
    std::string personality;
};

struct Companion : CompanionBones, CompanionSoul {
    int64_t hatched_at;                        // unix timestamp
};

struct StoredCompanion : CompanionSoul {
    int64_t hatched_at;
};

struct Roll {
    CompanionBones bones;
    int64_t inspiration_seed;
};

// -- Constants --------------------------------------------------------------

inline constexpr int RARITY_WEIGHTS[] = { 60, 25, 10, 4, 1 }; // common..legendary
inline constexpr int RARITY_FLOOR[]   = { 5, 15, 25, 35, 50 };

inline constexpr int TOTAL_RARITY_WEIGHT = 60 + 25 + 10 + 4 + 1; // 100

inline constexpr std::array<Species, 18> ALL_SPECIES = {
    Species::duck, Species::goose, Species::blob, Species::cat,
    Species::dragon, Species::octopus, Species::owl, Species::penguin,
    Species::turtle, Species::snail, Species::ghost, Species::axolotl,
    Species::capybara, Species::cactus, Species::robot, Species::rabbit,
    Species::mushroom, Species::chonk
};

inline constexpr std::array<Hat, 8> ALL_HATS = {
    Hat::none, Hat::crown, Hat::tophat, Hat::propeller,
    Hat::halo, Hat::wizard, Hat::beanie, Hat::tinyduck
};

inline const std::vector<std::string> EYES = { "·", "✦", "×", "◉", "@", "°" };

inline constexpr std::array<StatName, 5> ALL_STATS = {
    StatName::DEBUGGING, StatName::PATIENCE, StatName::CHAOS,
    StatName::WISDOM, StatName::SNARK
};

inline const std::array<std::string, 5> STAT_NAMES = {
    "DEBUGGING", "PATIENCE", "CHAOS", "WISDOM", "SNARK"
};

inline const std::array<std::string, 5> RARITY_STARS = {
    "★", "★★", "★★★", "★★★★", "★★★★★"
};

// -- Enum helpers -----------------------------------------------------------

inline auto rarity_to_string(Rarity r) -> std::string_view {
    switch (r) {
        case Rarity::common:    return "common";
        case Rarity::uncommon:  return "uncommon";
        case Rarity::rare:      return "rare";
        case Rarity::epic:      return "epic";
        case Rarity::legendary: return "legendary";
    }
    return "common";
}

inline auto species_to_string(Species s) -> std::string_view {
    switch (s) {
        case Species::duck:     return "duck";
        case Species::goose:    return "goose";
        case Species::blob:     return "blob";
        case Species::cat:      return "cat";
        case Species::dragon:   return "dragon";
        case Species::octopus:  return "octopus";
        case Species::owl:      return "owl";
        case Species::penguin:  return "penguin";
        case Species::turtle:   return "turtle";
        case Species::snail:    return "snail";
        case Species::ghost:    return "ghost";
        case Species::axolotl:  return "axolotl";
        case Species::capybara: return "capybara";
        case Species::cactus:   return "cactus";
        case Species::robot:    return "robot";
        case Species::rabbit:   return "rabbit";
        case Species::mushroom: return "mushroom";
        case Species::chonk:    return "chonk";
    }
    return "duck";
}

inline auto hat_to_string(Hat h) -> std::string_view {
    switch (h) {
        case Hat::none:      return "none";
        case Hat::crown:     return "crown";
        case Hat::tophat:    return "tophat";
        case Hat::propeller: return "propeller";
        case Hat::halo:      return "halo";
        case Hat::wizard:    return "wizard";
        case Hat::beanie:    return "beanie";
        case Hat::tinyduck:  return "tinyduck";
    }
    return "none";
}

inline auto stat_name_to_string(StatName s) -> std::string_view {
    switch (s) {
        case StatName::DEBUGGING: return "DEBUGGING";
        case StatName::PATIENCE:  return "PATIENCE";
        case StatName::CHAOS:     return "CHAOS";
        case StatName::WISDOM:    return "WISDOM";
        case StatName::SNARK:     return "SNARK";
    }
    return "DEBUGGING";
}

} // namespace cc::buddy
