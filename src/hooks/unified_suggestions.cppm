/// @file unified_suggestions.cppm
/// @brief Merge suggestions from multiple sources (files, MCP resources, agents).
/// Provides merge/dedup/sort utilities with priority-based ranking.
/// Faithful port of src/hooks/unifiedSuggestions.ts.
module;

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

export module cc.hooks.unified_suggestions;

export namespace cc::hooks::unified_suggestions {

// =========================================================================
// Constants
// =========================================================================

/// Maximum number of unified suggestions returned.
/// TS REF: src/hooks/unifiedSuggestions.ts:70 (MAX_UNIFIED_SUGGESTIONS = 15)
constexpr std::size_t MAX_UNIFIED_SUGGESTIONS = 50;

/// Maximum length for suggestion descriptions before truncation.
/// TS REF: src/hooks/unifiedSuggestions.ts:71 (DESCRIPTION_MAX_LENGTH = 60)
constexpr std::size_t DESCRIPTION_MAX_LENGTH = 60;

// =========================================================================
// Suggestion source types
// =========================================================================

/// The origin/source type of a unified suggestion.
/// TS REF: src/hooks/unifiedSuggestions.ts:12-41 (FileSuggestionSource,
///          McpResourceSuggestionSource, AgentSuggestionSource)
enum class SuggestionSourceType : std::uint8_t {
    File,        ///< File path suggestion (from @-mention autocomplete)
    McpResource, ///< MCP server resource suggestion
    Agent,       ///< Agent/teammate suggestion
    Command,     ///< Slash command suggestion
    History,     ///< Command history suggestion
    Skill,       ///< Skill suggestion
    Snippet,     ///< Code snippet suggestion
};

/// Convert source type to string for display/ID purposes.
[[nodiscard]] inline auto source_type_to_string(SuggestionSourceType type) -> std::string {
    switch (type) {
        case SuggestionSourceType::File:        return "file";
        case SuggestionSourceType::McpResource: return "mcp_resource";
        case SuggestionSourceType::Agent:       return "agent";
        case SuggestionSourceType::Command:     return "command";
        case SuggestionSourceType::History:     return "history";
        case SuggestionSourceType::Skill:       return "skill";
        case SuggestionSourceType::Snippet:     return "snippet";
    }
    return "unknown";
}

// =========================================================================
// UnifiedSuggestion struct
// =========================================================================

/// A single unified suggestion from any source.
/// TS REF: src/hooks/unifiedSuggestions.ts:46-68 (createSuggestionFromSource)
///          src/components/PromptInput/PromptInputFooterSuggestions.ts (SuggestionItem)
struct UnifiedSuggestion {
    std::string id;                    ///< Unique identifier (e.g. "file-path", "mcp-server__uri")
    std::string display_text;          ///< Text shown in the suggestion list
    std::string description;           ///< Optional description/subtitle
    std::string insert_text;           ///< Text to insert when accepted (defaults to display_text)
    std::string icon;                  ///< Icon character or label for display
    SuggestionSourceType source_type;  ///< Origin of this suggestion
    double priority;                   ///< Sort priority (higher = more relevant, 0.0 - 1.0)
    double score;                      ///< Match quality score (0.0 - 1.0, from fuzzy matcher)
    std::optional<std::string> source_name; ///< Name of source (e.g. MCP server name, agent type)
    std::optional<std::string> color;       ///< Optional display color hint

    /// Compute a combined ranking value (priority * 0.6 + score * 0.4)
    [[nodiscard]] auto ranking() const noexcept -> double {
        return priority * 0.6 + score * 0.4;
    }
};

// =========================================================================
// Source-specific suggestion data (for building UnifiedSuggestions)
// =========================================================================

/// File suggestion source data.
/// TS REF: src/hooks/unifiedSuggestions.ts:12-19 (FileSuggestionSource)
struct FileSuggestionSource {
    std::string path;
    std::string display_text;
    std::string description;
    std::string filename;
    double score{0.0};
};

/// MCP resource suggestion source data.
/// TS REF: src/hooks/unifiedSuggestions.ts:21-28 (McpResourceSuggestionSource)
struct McpResourceSuggestionSource {
    std::string display_text;
    std::string description;
    std::string server;
    std::string uri;
    std::string name;
};

/// Agent suggestion source data.
/// TS REF: src/hooks/unifiedSuggestions.ts:30-37 (AgentSuggestionSource)
struct AgentSuggestionSource {
    std::string display_text;
    std::string description;
    std::string agent_type;
    std::optional<std::string> color;
};

// =========================================================================
// Builder functions
// =========================================================================

namespace builders {

/// Build a UnifiedSuggestion from a FileSuggestionSource.
/// TS REF: src/hooks/unifiedSuggestions.ts:48-53 (file case in createSuggestionFromSource)
[[nodiscard]] inline auto from_file(const FileSuggestionSource& src) -> UnifiedSuggestion {
    return UnifiedSuggestion{
        .id = "file-" + src.path,
        .display_text = src.display_text,
        .description = src.description,
        .insert_text = src.display_text,
        .icon = "F",
        .source_type = SuggestionSourceType::File,
        .priority = 0.7,  // Files are high priority for @-mentions
        .score = src.score,
        .source_name = std::nullopt,
        .color = std::nullopt,
    };
}

/// Build a UnifiedSuggestion from an McpResourceSuggestionSource.
/// TS REF: src/hooks/unifiedSuggestions.ts:54-58 (mcp_resource case)
[[nodiscard]] inline auto from_mcp_resource(const McpResourceSuggestionSource& src) -> UnifiedSuggestion {
    return UnifiedSuggestion{
        .id = "mcp-resource-" + src.server + "__" + src.uri,
        .display_text = src.display_text,
        .description = src.description,
        .insert_text = src.display_text,
        .icon = "M",
        .source_type = SuggestionSourceType::McpResource,
        .priority = 0.5,  // MCP resources are medium priority
        .score = 0.5,     // Default score if not fuzzy-matched
        .source_name = src.server,
        .color = std::nullopt,
    };
}

/// Build a UnifiedSuggestion from an AgentSuggestionSource.
/// TS REF: src/hooks/unifiedSuggestions.ts:59-67 (agent case)
[[nodiscard]] inline auto from_agent(const AgentSuggestionSource& src) -> UnifiedSuggestion {
    return UnifiedSuggestion{
        .id = "agent-" + src.agent_type,
        .display_text = src.display_text,
        .description = src.description,
        .insert_text = "@" + src.agent_type,
        .icon = "A",
        .source_type = SuggestionSourceType::Agent,
        .priority = 0.6,  // Agents are high-medium priority
        .score = 0.5,
        .source_name = src.agent_type,
        .color = src.color,
    };
}

} // namespace builders

// =========================================================================
// Core merge/sort/dedup utilities
// =========================================================================

/// Truncate a description to the maximum display length.
/// TS REF: src/hooks/unifiedSuggestions.ts:73-75 (truncateDescription)
[[nodiscard]] inline auto truncate_description(std::string_view description) -> std::string {
    if (description.size() <= DESCRIPTION_MAX_LENGTH) {
        return std::string{description};
    }
    return std::string{description.substr(0, DESCRIPTION_MAX_LENGTH - 3)} + "...";
}

/// Merge multiple vectors of suggestions into a single combined list.
/// TS REF: src/hooks/unifiedSuggestions.ts:121-155 (combining fileSources, mcpSources, agentSources)
[[nodiscard]] inline auto merge_suggestions(std::vector<std::vector<UnifiedSuggestion>> source_lists)
    -> std::vector<UnifiedSuggestion>
{
    std::size_t total = 0;
    for (const auto& list : source_lists) total += list.size();

    std::vector<UnifiedSuggestion> merged;
    merged.reserve(total);
    for (auto& list : source_lists) {
        for (auto& s : list) {
            merged.push_back(std::move(s));
        }
    }
    return merged;
}

/// Sort suggestions by their combined ranking (priority + score).
/// Higher ranking values come first.
/// TS REF: src/hooks/unifiedSuggestions.ts:196 (scoredResults.sort by score)
inline auto sort_by_priority(std::vector<UnifiedSuggestion>& suggestions) -> void {
    std::sort(suggestions.begin(), suggestions.end(),
        [](const UnifiedSuggestion& a, const UnifiedSuggestion& b) {
            return a.ranking() > b.ranking();
        });
}

/// Remove duplicate suggestions based on their id field.
/// Keeps the first occurrence (highest priority if already sorted).
/// TS REF: implicit dedup via unique IDs in createSuggestionFromSource
[[nodiscard]] inline auto dedup_suggestions(const std::vector<UnifiedSuggestion>& suggestions)
    -> std::vector<UnifiedSuggestion>
{
    std::unordered_set<std::string> seen_ids;
    std::vector<UnifiedSuggestion> deduped;
    deduped.reserve(suggestions.size());

    for (const auto& s : suggestions) {
        if (seen_ids.insert(s.id).second) {
            deduped.push_back(s);
        }
    }
    return deduped;
}

/// Get the top N suggestions from a list (after sorting).
/// TS REF: src/hooks/unifiedSuggestions.ts:198-201 (.slice(0, MAX_UNIFIED_SUGGESTIONS))
[[nodiscard]] inline auto get_top_n(const std::vector<UnifiedSuggestion>& suggestions,
                                     std::size_t n = MAX_UNIFIED_SUGGESTIONS)
    -> std::vector<UnifiedSuggestion>
{
    if (suggestions.size() <= n) return suggestions;
    return std::vector<UnifiedSuggestion>(suggestions.begin(), suggestions.begin() + n);
}

// =========================================================================
// Fuzzy scoring for non-file sources
// =========================================================================

namespace detail {

/// Simple case-insensitive substring match score (0.0 - 1.0).
/// TS REF: src/hooks/unifiedSuggestions.ts:173-193 (Fuse.js scoring of non-file sources)
[[nodiscard]] inline auto fuzzy_score_text(std::string_view text, std::string_view query) noexcept -> double {
    if (query.empty()) return 0.5;
    if (text.empty()) return 0.0;

    // Case-insensitive search
    std::string text_lower, query_lower;
    text_lower.reserve(text.size());
    for (char c : text) text_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    query_lower.reserve(query.size());
    for (char c : query) query_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Exact substring match = high score
    if (text_lower.find(query_lower) != std::string::npos) {
        // Bonus for matches at word boundaries
        double base = 0.8;
        auto pos = text_lower.find(query_lower);
        if (pos == 0) base += 0.1;
        else if (pos > 0) {
            char prev = text_lower[pos - 1];
            if (prev == ' ' || prev == '_' || prev == '-' || prev == '/') {
                base += 0.05;
            }
        }
        return std::min(1.0, base);
    }

    // Subsequence match (all query chars found in order)
    std::size_t qi = 0;
    std::size_t matches = 0;
    for (std::size_t i = 0; i < text_lower.size() && qi < query_lower.size(); ++i) {
        if (text_lower[i] == query_lower[qi]) {
            ++matches;
            ++qi;
        }
    }
    if (qi == query_lower.size()) {
        return 0.4 + 0.2 * (static_cast<double>(matches) / static_cast<double>(text_lower.size()));
    }

    return 0.0;
}

} // namespace detail

// =========================================================================
// UnifiedSuggestionsHook — interactive suggestion manager
// =========================================================================

/// State of the unified suggestions UI component.
struct UnifiedSuggestionsState {
    std::vector<UnifiedSuggestion> items;
    std::optional<std::size_t> selected_index;
    std::string query;
    bool visible{false};
};

/// Configuration options for the hook.
struct UnifiedSuggestionsOptions {
    std::size_t max_results{MAX_UNIFIED_SUGGESTIONS};
    bool deduplicate{true};
    std::vector<SuggestionSourceType> enabled_sources;
};

/// Interactive hook that manages a unified suggestions popup.
/// Supports querying, selection navigation, and acceptance.
/// TS REF: src/hooks/unifiedSuggestions.ts (generateUnifiedSuggestions)
class UnifiedSuggestionsHook {
public:
    explicit UnifiedSuggestionsHook(const UnifiedSuggestionsOptions& opts = {})
        : options_(opts) {}

    /// Activate the suggestions popup.
    auto activate() -> void { active_ = true; }

    /// Deactivate and hide the suggestions popup.
    auto deactivate() -> void {
        active_ = false;
        state_.visible = false;
    }

    /// Get the current state.
    [[nodiscard]] auto state() const -> const UnifiedSuggestionsState& { return state_; }

    /// Update the search query and re-rank suggestions.
    auto update_query(std::string query) -> void {
        state_.query = std::move(query);
        re_rank();
        notify();
    }

    /// Set the suggestion items directly (pre-built from external sources).
    auto set_items(std::vector<UnifiedSuggestion> items) -> void {
        if (options_.deduplicate) {
            items = dedup_suggestions(items);
        }
        sort_by_priority(items);
        if (items.size() > options_.max_results) {
            items.resize(options_.max_results);
        }
        state_.items = std::move(items);
        state_.visible = !state_.items.empty() && active_;
        state_.selected_index = state_.visible ? std::optional<std::size_t>(0) : std::nullopt;
        notify();
    }

    /// Navigate to the next suggestion.
    auto select_next() -> void {
        if (state_.items.empty()) return;
        state_.selected_index = (state_.selected_index.value_or(0) + 1) % state_.items.size();
        notify();
    }

    /// Navigate to the previous suggestion.
    auto select_prev() -> void {
        if (state_.items.empty()) return;
        auto idx = state_.selected_index.value_or(0);
        state_.selected_index = (idx == 0) ? state_.items.size() - 1 : idx - 1;
        notify();
    }

    /// Dismiss/hide the suggestions popup.
    auto dismiss() -> void {
        state_.visible = false;
        state_.items.clear();
        state_.selected_index = std::nullopt;
        notify();
    }

    /// Accept the currently selected suggestion.
    /// Returns the accepted suggestion, or nullopt if nothing selected.
    [[nodiscard]] auto accept() -> std::optional<UnifiedSuggestion> {
        if (!state_.selected_index || *state_.selected_index >= state_.items.size()) {
            return std::nullopt;
        }
        auto result = state_.items[*state_.selected_index];
        dismiss();
        return result;
    }

    /// Register a change listener.
    auto on_change(std::function<void(const UnifiedSuggestionsState&)> callback) -> void {
        listeners_.push_back(std::move(callback));
    }

    /// Check if a specific source type is enabled.
    [[nodiscard]] auto is_source_enabled(SuggestionSourceType type) const -> bool {
        if (options_.enabled_sources.empty()) return true; // all enabled by default
        return std::find(options_.enabled_sources.begin(), options_.enabled_sources.end(), type)
               != options_.enabled_sources.end();
    }

private:
    auto re_rank() -> void {
        // Re-score existing items based on the new query
        for (auto& item : state_.items) {
            double text_score = detail::fuzzy_score_text(item.display_text, state_.query);
            double desc_score = detail::fuzzy_score_text(item.description, state_.query);
            item.score = std::max(text_score, desc_score * 0.7);
        }
        sort_by_priority(state_.items);
        state_.selected_index = state_.items.empty() ? std::nullopt : std::optional<std::size_t>(0);
    }

    auto notify() -> void {
        for (const auto& cb : listeners_) cb(state_);
    }

    UnifiedSuggestionsState state_;
    UnifiedSuggestionsOptions options_;
    std::vector<std::function<void(const UnifiedSuggestionsState&)>> listeners_;
    bool active_{false};
};

// =========================================================================
// Convenience: generate unified suggestions from all sources
// =========================================================================

/// Generate unified suggestions from pre-collected source lists.
/// This is the main entry point — callers provide file/MCP/agent source lists,
/// and this function merges, scores, deduplicates, and returns the top results.
/// TS REF: src/hooks/unifiedSuggestions.ts:111-202 (generateUnifiedSuggestions)
[[nodiscard]] inline auto generate_unified_suggestions(
    std::string_view query,
    const std::vector<FileSuggestionSource>& file_sources,
    const std::vector<McpResourceSuggestionSource>& mcp_sources,
    const std::vector<AgentSuggestionSource>& agent_sources,
    bool show_on_empty = false,
    std::size_t max_results = MAX_UNIFIED_SUGGESTIONS)
    -> std::vector<UnifiedSuggestion>
{
    if (query.empty() && !show_on_empty) return {};

    max_results = std::min(max_results, MAX_UNIFIED_SUGGESTIONS);

    // Build UnifiedSuggestion vectors from each source type
    std::vector<UnifiedSuggestion> files;
    files.reserve(file_sources.size());
    for (const auto& fs : file_sources) {
        auto u = builders::from_file(fs);
        if (!query.empty()) {
            u.score = detail::fuzzy_score_text(fs.display_text, query);
        }
        files.push_back(std::move(u));
    }

    std::vector<UnifiedSuggestion> mcps;
    mcps.reserve(mcp_sources.size());
    for (const auto& ms : mcp_sources) {
        auto u = builders::from_mcp_resource(ms);
        if (!query.empty()) {
            double name_score = detail::fuzzy_score_text(ms.name, query);
            double uri_score = detail::fuzzy_score_text(ms.uri, query);
            double desc_score = detail::fuzzy_score_text(ms.description, query);
            u.score = std::max({name_score, uri_score, desc_score * 0.7});
        }
        mcps.push_back(std::move(u));
    }

    std::vector<UnifiedSuggestion> agents;
    agents.reserve(agent_sources.size());
    for (const auto& as : agent_sources) {
        auto u = builders::from_agent(as);
        if (!query.empty()) {
            double type_score = detail::fuzzy_score_text(as.agent_type, query);
            double display_score = detail::fuzzy_score_text(as.display_text, query);
            u.score = std::max(type_score, display_score);
        }
        agents.push_back(std::move(u));
    }

    // Merge all sources
    auto merged = merge_suggestions({std::move(files), std::move(mcps), std::move(agents)});

    // Dedup
    merged = dedup_suggestions(merged);

    // Sort by ranking
    sort_by_priority(merged);

    // Return top N
    return get_top_n(merged, max_results);
}

} // namespace cc::hooks::unified_suggestions
