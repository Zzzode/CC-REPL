// ToolSearchTool - Searches available tools by keyword
module;
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module cc.tools.tool_search;


export namespace cc::tools {


enum class ToolSearchError {
    QueryEmpty,
    QueryTooShort,
    NoResults,
    RegistryUnavailable,
};

constexpr auto format_error(ToolSearchError err) -> std::string_view {
    switch (err) {
        case ToolSearchError::QueryEmpty:          return "Search query is empty";
        case ToolSearchError::QueryTooShort:       return "Search query too short (min 2 chars)";
        case ToolSearchError::NoResults:           return "No matching tools found";
        case ToolSearchError::RegistryUnavailable: return "Tool registry is unavailable";
        default:                                   return "Unknown tool search error";
    }
}


struct ToolDescriptor {
    std::string name;
    std::string description;
    std::string schema_json;
    std::vector<std::string> tags;
    bool lazy_loaded{false};
};


struct SearchMatch {
    std::string tool_name;
    std::string description;
    std::string schema_json;
    double relevance_score{0.0};
};


struct ToolSearchRequest {
    std::string query;
    size_t max_results{10};
    bool include_schemas{true};
    std::optional<std::string> tag_filter;
};


struct ToolSearchResult {
    std::vector<SearchMatch> matches;
    size_t total_tools_searched{0};
    std::chrono::microseconds search_duration{0};
};


class RelevanceScorer {
public:

    static auto score(std::string_view query, std::string_view text) -> double {
        if (query.empty() || text.empty()) return 0.0;

        auto query_lower = to_lower(query);
        auto text_lower = to_lower(text);


        if (text_lower.find(query_lower) != std::string::npos) {
            return 1.0;
        }


        auto query_words = split_words(query_lower);
        size_t hits = 0;
        for (const auto& word : query_words) {
            if (text_lower.find(word) != std::string::npos) {
                ++hits;
            }
        }

        return query_words.empty() ? 0.0 : static_cast<double>(hits) / query_words.size();
    }

private:
    static auto to_lower(std::string_view sv) -> std::string {
        std::string result(sv);
        for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return result;
    }

    static auto split_words(std::string_view text) -> std::vector<std::string> {
        std::vector<std::string> words;
        std::string current;
        for (char c : text) {
            if (c == ' ' || c == '_' || c == '-') {
                if (!current.empty()) {
                    words.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) words.push_back(std::move(current));
        return words;
    }
};


class ToolIndex {
public:

    void register_tool(ToolDescriptor descriptor) {
        tools_.push_back(std::move(descriptor));
    }


    void register_tools(std::vector<ToolDescriptor> descriptors) {
        tools_.insert(tools_.end(),
            std::make_move_iterator(descriptors.begin()),
            std::make_move_iterator(descriptors.end()));
    }


    auto search(const ToolSearchRequest& request) const -> ToolSearchResult {
        auto start = std::chrono::steady_clock::now();

        std::vector<SearchMatch> matches;

        for (const auto& tool : tools_) {

            if (request.tag_filter) {
                bool has_tag = false;
                for (const auto& tag : tool.tags) {
                    if (tag == *request.tag_filter) { has_tag = true; break; }
                }
                if (!has_tag) continue;
            }


            double name_score = RelevanceScorer::score(request.query, tool.name) * 2.0;
            double desc_score = RelevanceScorer::score(request.query, tool.description);
            double tag_score = 0.0;
            for (const auto& tag : tool.tags) {
                tag_score = std::max(tag_score, RelevanceScorer::score(request.query, tag));
            }

            double total_score = std::min(1.0, (name_score + desc_score + tag_score) / 3.0);

            if (total_score > 0.1) {
                matches.push_back(SearchMatch{
                    .tool_name = tool.name,
                    .description = tool.description,
                    .schema_json = request.include_schemas ? tool.schema_json : "",
                    .relevance_score = total_score,
                });
            }
        }


        std::ranges::sort(matches, [](const auto& a, const auto& b) {
            return a.relevance_score > b.relevance_score;
        });


        if (matches.size() > request.max_results) {
            matches.resize(request.max_results);
        }

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);

        return ToolSearchResult{
            .matches = std::move(matches),
            .total_tools_searched = tools_.size(),
            .search_duration = duration,
        };
    }

    [[nodiscard]] size_t size() const { return tools_.size(); }

private:
    std::vector<ToolDescriptor> tools_;
};


inline ToolIndex& global_tool_index() {
    static ToolIndex index;
    return index;
}


class ToolSearchTool {
public:
    static constexpr std::string_view name = "tool_search";
    static constexpr std::string_view description = "Search available tools by keyword across names and descriptions";
    static constexpr size_t kMinQueryLength = 2;

    auto validate(const ToolSearchRequest& request) const -> std::expected<void, ToolSearchError> {
        if (request.query.empty()) {
            return std::unexpected(ToolSearchError::QueryEmpty);
        }
        if (request.query.size() < kMinQueryLength) {
            return std::unexpected(ToolSearchError::QueryTooShort);
        }
        return {};
    }

    auto execute(ToolSearchRequest request) -> std::expected<ToolSearchResult, ToolSearchError> {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        auto result = global_tool_index().search(request);

        if (result.matches.empty()) {
            return std::unexpected(ToolSearchError::NoResults);
        }
        return result;
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "query": {{ "type": "string", "description": "Search keyword(s) to match against tool names and descriptions" }},
      "max_results": {{ "type": "integer", "description": "Maximum number of results to return (default 10)" }},
      "include_schemas": {{ "type": "boolean", "description": "Whether to include full JSON schemas in results" }}
    }},
    "required": ["query"]
  }}
}})json", name, description);
    }
};

} // namespace cc::tools
