// WebSearchTool - Web search via search engine
module;

#include <format>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <expected>
#include <vector>

export module cc.tools.web_search;

import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::web_search {

namespace detail {
struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

[[nodiscard]] inline auto url_encode(std::string_view s) -> std::string {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
            out += static_cast<char>(ch);
        } else if (ch == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 0x0F];
        }
    }
    return out;
}

[[nodiscard]] inline auto hex_value(char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

[[nodiscard]] inline auto url_decode(std::string_view encoded) -> std::string {
    std::string out;
    out.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            const int hi = hex_value(encoded[i + 1]);
            const int lo = hex_value(encoded[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += encoded[i] == '+' ? ' ' : encoded[i];
    }
    return out;
}

[[nodiscard]] inline auto html_decode(std::string_view html) -> std::string {
    std::string out;
    out.reserve(html.size());
    for (std::size_t i = 0; i < html.size(); ++i) {
        if (html[i] != '&') {
            out += html[i];
            continue;
        }

        const auto semi = html.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out += html[i];
            continue;
        }

        const auto entity = html.substr(i + 1, semi - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos" || entity == "#39" || entity == "#x27") out += '\'';
        else {
            out += '&';
            continue;
        }
        i = semi;
    }
    return out;
}

[[nodiscard]] inline auto normalize_space(std::string_view text) -> std::string {
    std::string out;
    bool pending_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out += ' ';
            pending_space = false;
        }
        out += static_cast<char>(ch);
    }
    return out;
}

[[nodiscard]] inline auto strip_tags(std::string_view html) -> std::string {
    std::string out;
    bool in_tag = false;
    for (char ch : html) {
        if (ch == '<') {
            in_tag = true;
            out += ' ';
            continue;
        }
        if (ch == '>') {
            in_tag = false;
            out += ' ';
            continue;
        }
        if (!in_tag) out += ch;
    }
    return normalize_space(html_decode(out));
}

[[nodiscard]] inline auto attr_value(std::string_view tag, std::string_view name) -> std::optional<std::string> {
    const auto attr = std::string(name) + "=";
    const auto pos = tag.find(attr);
    if (pos == std::string_view::npos) return std::nullopt;
    auto value_start = pos + attr.size();
    if (value_start >= tag.size()) return std::nullopt;
    const char quote = tag[value_start];
    if (quote != '"' && quote != '\'') return std::nullopt;
    ++value_start;
    const auto value_end = tag.find(quote, value_start);
    if (value_end == std::string_view::npos) return std::nullopt;
    return html_decode(tag.substr(value_start, value_end - value_start));
}

[[nodiscard]] inline auto extract_result_url(std::string_view href) -> std::string {
    auto decoded = html_decode(href);
    const auto marker = std::string_view{"uddg="};
    const auto pos = decoded.find(marker);
    if (pos == std::string::npos) return decoded;
    auto start = pos + marker.size();
    auto end = decoded.find('&', start);
    if (end == std::string::npos) end = decoded.size();
    return url_decode(std::string_view(decoded).substr(start, end - start));
}

[[nodiscard]] inline auto parse_duckduckgo_results(std::string_view html, std::size_t max_results = 5) -> std::vector<SearchResult> {
    std::vector<SearchResult> results;
    std::size_t cursor = 0;
    while (results.size() < max_results) {
        const auto class_pos = html.find("result__a", cursor);
        if (class_pos == std::string_view::npos) break;
        const auto tag_start = html.rfind("<a", class_pos);
        const auto tag_end = html.find('>', class_pos);
        const auto close = html.find("</a>", tag_end == std::string_view::npos ? class_pos : tag_end);
        if (tag_start == std::string_view::npos || tag_end == std::string_view::npos || close == std::string_view::npos) {
            cursor = class_pos + 9;
            continue;
        }

        const auto tag = html.substr(tag_start, tag_end - tag_start + 1);
        auto href = attr_value(tag, "href");
        auto title = strip_tags(html.substr(tag_end + 1, close - tag_end - 1));
        if (!href || title.empty()) {
            cursor = close + 4;
            continue;
        }

        SearchResult result{
            .title = std::move(title),
            .url = extract_result_url(*href),
            .snippet = {}
        };

        const auto snippet_class = html.find("result__snippet", close);
        const auto next_result = html.find("result__a", close + 4);
        if (snippet_class != std::string_view::npos &&
            (next_result == std::string_view::npos || snippet_class < next_result)) {
            const auto snippet_tag_start = html.rfind('<', snippet_class);
            const auto snippet_start = html.find('>', snippet_class);
            std::size_t snippet_end = std::string_view::npos;
            if (snippet_tag_start != std::string_view::npos) {
                const auto snippet_tag = html.substr(snippet_tag_start, snippet_start - snippet_tag_start + 1);
                if (snippet_tag.find("<a") != std::string_view::npos) {
                    snippet_end = html.find("</a>", snippet_start);
                } else {
                    snippet_end = html.find("</div>", snippet_start);
                }
            }
            if (snippet_start != std::string_view::npos && snippet_end != std::string_view::npos) {
                result.snippet = strip_tags(html.substr(snippet_start + 1, snippet_end - snippet_start - 1));
            }
        }

        const auto duplicate = std::ranges::any_of(results, [&](const SearchResult& existing) {
            return existing.url == result.url;
        });
        if (!duplicate) results.push_back(std::move(result));
        cursor = close + 4;
    }
    return results;
}

[[nodiscard]] inline auto format_results(std::string_view query, std::string_view html) -> std::string {
    const auto results = parse_duckduckgo_results(html);
    if (results.empty()) {
        auto excerpt = strip_tags(html);
        if (excerpt.size() > 2000) excerpt = excerpt.substr(0, 2000) + "\n[truncated]";
        return std::format("No parseable search results found for: {}\n\n{}", query, excerpt);
    }

    std::string out = std::format("Search results for: {}\n\n", query);
    for (std::size_t i = 0; i < results.size(); ++i) {
        out += std::format("{}. {}\n   {}\n", i + 1, results[i].title, results[i].url);
        if (!results[i].snippet.empty()) {
            out += std::format("   {}\n", results[i].snippet);
        }
        if (i + 1 < results.size()) out += "\n";
    }
    return out;
}

[[nodiscard]] inline auto parse_query(std::string_view json) -> std::expected<std::string, std::string> {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Invalid JSON input");
    }

    auto query = parsed->root().get("query");
    if (!query.is_str() || query.as_str().empty()) {
        return std::unexpected("Missing 'query' field");
    }

    return std::string(query.as_str());
}

[[nodiscard]] inline auto run_curl(std::string_view url) -> std::optional<std::string> {
    const auto cmd = "curl -fsSL --max-time 30 '" + std::string(url) + "' 2>&1";
    std::array<char, 4096> buf{};
    std::string out;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) out += buf.data();
    const int status = pclose(pipe);
    if (status != 0) return std::nullopt;
    return out;
}
}

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;

// =========================================================================
// WebSearchTool Implementation
// =========================================================================

/// WebSearchTool - Performs web searches
class WebSearchTool {
public:
    static constexpr std::string_view kName = "WebSearch";
    static constexpr std::string_view kDescription = 
        "Search the web for a query and return results.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "query",
                        .type = "string",
                        .description = "Search query string",
                        .required = true
                    }
                }
            },
            .permission = ToolPermission::Network,
            .category = "network"
        };
    }
    
    WebSearchTool() = default;
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        return true;
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed_query = detail::parse_query(input.json());
        if (!parsed_query) {
            return ToolResult::error(parsed_query.error());
        }

        const auto& query = *parsed_query;
        const auto url = "https://duckduckgo.com/html/?q=" + detail::url_encode(query);
        auto html = detail::run_curl(url);
        if (!html) return ToolResult::error(std::format("Failed to search for: {}", query));
        return ToolResult::success(detail::format_results(query, *html));
    }
};

} // namespace cc::tools::web_search

// Export main tool class
export namespace cc::tools {
    using cc::tools::web_search::WebSearchTool;

    /// Factory: create WebSearchTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_web_search_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            WebSearchTool tool_;
            cc::core::ToolDefinition def_ = WebSearchTool::definition();

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(const cc::core::ToolInput& input) override {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed, result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>();
    }
}
