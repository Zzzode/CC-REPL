module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>

export module cc.utils.context_helpers;

export namespace cc::utils::context_helpers {

struct ContextEntry {
    std::string source;
    std::string content;
    int priority{0};
};

struct ContextSuggestion {
    std::string text;
    std::string reason;
    double confidence{0.0};
};

struct QueryContext {
    std::string query;
    std::vector<std::string> relevant_files;
    std::optional<std::string> active_tool;
};

inline std::vector<ContextEntry> gather_context([[maybe_unused]] std::string_view working_dir) {
    return {};
}

inline std::vector<ContextSuggestion> get_context_suggestions([[maybe_unused]] std::string_view partial_input) {
    return {};
}

inline QueryContext build_query_context(std::string_view query) {
    return {std::string(query), {}, std::nullopt};
}

inline std::string format_context_for_prompt([[maybe_unused]] const std::vector<ContextEntry>& entries) {
    return "";
}

} // namespace cc::utils::context_helpers
