module;
#include <algorithm>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.model.validate_model;

export namespace cc::utils {

// Simplified ModelInfo for this module (avoids cross-module dependency)
struct ValidatedModelInfo {
    std::string id;
    std::string display_name;
    int context_window;
    int max_output;
};

namespace detail {
    inline const std::vector<ValidatedModelInfo>& known_models() {
        static const std::vector<ValidatedModelInfo> models = {
            {"claude-sonnet-4-20250514", "Claude Sonnet 4", 200000, 16384},
            {"claude-opus-4-20250514",   "Claude Opus 4",   200000, 32768},
            {"claude-haiku-4-20250514",  "Claude Haiku 4",  200000, 8192},
        };
        return models;
    }

    // Compute edit distance for fuzzy matching
    inline int levenshtein(std::string_view a, std::string_view b) {
        int m = static_cast<int>(a.size());
        int n = static_cast<int>(b.size());
        std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));

        for (int i = 0; i <= m; ++i) dp[i][0] = i;
        for (int j = 0; j <= n; ++j) dp[0][j] = j;

        for (int i = 1; i <= m; ++i) {
            for (int j = 1; j <= n; ++j) {
                int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                dp[i][j] = std::min({
                    dp[i - 1][j] + 1,
                    dp[i][j - 1] + 1,
                    dp[i - 1][j - 1] + cost
                });
            }
        }
        return dp[m][n];
    }
} // namespace detail

std::expected<ValidatedModelInfo, std::string> validate_model(std::string_view model_id) {
    for (auto& m : detail::known_models()) {
        if (m.id == model_id) {
            return m;
        }
    }
    return std::unexpected("Unknown model: " + std::string(model_id));
}

std::optional<std::string> suggest_model(std::string_view invalid_input) {
    int best_distance = std::numeric_limits<int>::max();
    std::string best_match;

    for (auto& m : detail::known_models()) {
        int dist = detail::levenshtein(invalid_input, m.id);
        if (dist < best_distance) {
            best_distance = dist;
            best_match = m.id;
        }
    }

    // Only suggest if the edit distance is reasonable (less than half the target length)
    if (best_distance <= static_cast<int>(best_match.size()) / 2) {
        return best_match;
    }
    return std::nullopt;
}

} // namespace cc::utils
