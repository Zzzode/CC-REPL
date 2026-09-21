// AT-12: nucleo/fuse-grade fuzzy ranking for autocomplete.
//
// app.cppm's `fuzzy_rank_ascii` buckets candidates into 4 coarse grades
// {0=exact, 1=prefix, 2=substring, 3=subsequence} with an alphabetical
// tiebreak. The existing tier-offset model relies on this NARROW 0..3 base
// range: alias adds +1, skill +4, plugin +6, so the four buckets are what
// keep categories separated under rank-ascending sort.
//
// To upgrade to nucleo/fuse quality WITHOUT breaking the offsets, this module
// keeps the identical {0..3} contract but lets a real fuzzy_match score
// (boundary/camel/consecutive/gap/path bonuses — ported from
// cc.utils.file_index) decide WHICH bucket a candidate lands in. A strong
// nucleo match (consecutive, at a path/camel boundary) can reach bucket 0/1
// even when it isn't an exact prefix; a weak scattered subsequence drops to 3.
//
// Heavy logic lives here (not in app.cppm) to respect the thin-module 2GB
// source-location budget. No filesystem, no transitive heavy imports.
module;

#include <cctype>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

export module cc.ui.prompt.fuzzy_rank_nucleo;

export namespace cc::ui::prompt::fuzzy_rank_nucleo {

// =========================================================================
// Scoring constants (fzf-v2 / nucleo compatible; mirrored from
// cc.utils.file_index so the two stay in sync without a hard import).
// =========================================================================

constexpr int SCORE_MATCH = 16;
constexpr int BONUS_BOUNDARY = 8;
constexpr int BONUS_CAMEL = 6;
constexpr int BONUS_CONSECUTIVE = 4;
constexpr int GAP_START = -3;
constexpr int GAP_EXTENSION = -1;
constexpr int BONUS_FIRST_CHAR = 2;
constexpr int BONUS_PATH_SEPARATOR = 10;

[[nodiscard]] inline bool is_boundary(char prev, char curr) noexcept {
    if (prev == '/' || prev == '\\' || prev == '.' || prev == '_' || prev == '-') {
        return true;
    }
    if (std::islower(static_cast<unsigned char>(prev)) &&
        std::isupper(static_cast<unsigned char>(curr))) {
        return true;  // camelCase boundary
    }
    return false;
}

[[nodiscard]] inline int position_bonus(std::string_view text, size_t pos) noexcept {
    if (pos == 0) return BONUS_FIRST_CHAR + BONUS_BOUNDARY;
    const char prev = text[pos - 1];
    const char curr = text[pos];
    if (prev == '/' || prev == '\\') return BONUS_PATH_SEPARATOR;
    if (is_boundary(prev, curr)) return BONUS_BOUNDARY;
    if (std::islower(static_cast<unsigned char>(prev)) &&
        std::isupper(static_cast<unsigned char>(curr))) {
        return BONUS_CAMEL;
    }
    return 0;
}

[[nodiscard]] inline std::string lowercase_ascii(std::string_view value) {
    std::string out(value);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

/// Greedy forward subsequence match + nucleo-quality score (higher = better).
/// Returns a very negative score when `query` is not a case-insensitive
/// subsequence of `candidate`.
struct NucleoScore {
    int score = std::numeric_limits<int>::min();
    int consecutive = 0;
    bool matched = false;
};

[[nodiscard]] inline NucleoScore nucleo_score(
    std::string_view candidate_lower,
    std::string_view query_lower) noexcept {
    NucleoScore result;
    if (query_lower.empty()) {
        return {.score = 0, .consecutive = 0, .matched = true};
    }
    if (candidate_lower.empty()) return result;

    // Quick reject + collect match positions in a single greedy forward pass.
    std::vector<size_t> positions;
    positions.reserve(query_lower.size());
    size_t pi = 0;
    for (size_t ti = 0; ti < candidate_lower.size() && pi < query_lower.size(); ++ti) {
        if (candidate_lower[ti] == query_lower[pi]) {
            positions.push_back(ti);
            ++pi;
        }
    }
    if (pi != query_lower.size()) return result;  // not a subsequence

    int score = 0;
    int consecutive = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        const size_t pos = positions[i];
        score += SCORE_MATCH;
        score += position_bonus(candidate_lower, pos);
        if (query_lower[i] == candidate_lower[pos]) score += 1;  // exact case
        if (i > 0) {
            if (positions[i] == positions[i - 1] + 1) {
                ++consecutive;
                score += BONUS_CONSECUTIVE * consecutive;
            } else {
                const int gap = static_cast<int>(positions[i] - positions[i - 1] - 1);
                score += GAP_START + GAP_EXTENSION * (gap - 1);
                consecutive = 0;
            }
        }
    }
    return {.score = score, .consecutive = consecutive, .matched = true};
}

/// AT-12: nucleo-grade fuzzy rank. Drop-in replacement for app.cppm's
/// `fuzzy_rank_ascii(candidate, query)`.
///
/// Contract preserved verbatim:
///   - empty query        -> 1000 (everything ties; alphabetical order dominates)
///   - case-insensitive exact -> 0
///   - returns an int in the NARROW base range {0,1,2,3} so the existing tier
///     offsets (alias +1, skill +4, plugin +6) still separate categories under
///     rank-ascending sort.
///
/// Hidden-command escape hatch (rank = -1000) is set directly at its call-site
/// and never flows through here, so it is unaffected.
[[nodiscard]] inline int fuzzy_rank_nucleo(
    std::string_view candidate,
    std::string_view query) {
    if (query.empty()) return 1000;
    const auto c = lowercase_ascii(candidate);
    const auto q = lowercase_ascii(query);
    if (c == q) return 0;

    const auto ns = nucleo_score(c, q);
    if (!ns.matched) return 3;  // defensive: callers pre-filter, but stay safe

    const bool is_prefix = c.starts_with(q);
    const bool is_substring = c.find(q) != std::string::npos;
    // all_consecutive is only meaningful for multi-char queries — a single-char
    // match has no consecutive run. Without this guard, a scattered "h" in
    // "branch" ties (rank 1) with a prefix "h" in "help", and alphabetical
    // tiebreak then buries /help below /branch. Require q.size() > 1 so a
    // prefix hit outranks a non-prefix single-char substring.
    const bool all_consecutive = q.size() > 1 &&
        ns.consecutive + 1 >= static_cast<int>(q.size());

    // Quality thresholds derived from the scoring constants: a pure contiguous
    // match with no bonuses sits at SCORE_MATCH * len; boundary/path bonuses
    // push strong matches well above that.
    const int len = static_cast<int>(q.size());
    const int strong_band = SCORE_MATCH * len + BONUS_BOUNDARY;
    const int medium_band = SCORE_MATCH * len - GAP_EXTENSION * len;

    if (is_prefix || all_consecutive || ns.score >= strong_band) return 1;
    if (is_substring || ns.score >= medium_band) return 2;
    return 3;
}

/// AT-12: boolean fuzzy predicate matching app.cppm's `fuzzy_match_ascii`
/// semantics (substring OR subsequence). Kept here so call-sites use a single
/// consistent rank + match pair sourced from one nucleo pass.
[[nodiscard]] inline bool fuzzy_match_nucleo(
    std::string_view candidate,
    std::string_view query) {
    if (query.empty()) return true;
    return nucleo_score(lowercase_ascii(candidate), lowercase_ascii(query)).matched;
}

}  // namespace cc::ui::prompt::fuzzy_rank_nucleo
