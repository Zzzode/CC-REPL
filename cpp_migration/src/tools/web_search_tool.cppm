// WebSearchTool - Web search via search engine
module;

#include <format>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <expected>

export module cc.tools.web_search;

import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::web_search {

namespace detail {
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
        std::string query;
        auto json_str = input.json();
        auto query_pos = json_str.find("\"query\"");
        if (query_pos != std::string::npos) {
            auto val_start = json_str.find(":", query_pos);
            auto quote_start = json_str.find("\"", val_start + 1);
            auto quote_end = json_str.find("\"", quote_start + 1);
            if (quote_start != std::string::npos && quote_end != std::string::npos) {
                query = std::string(json_str.substr(quote_start + 1, quote_end - quote_start - 1));
            }
        }
        
        if (query.empty()) {
            return ToolResult::error("Missing 'query' field");
        }
        
        const auto url = "https://duckduckgo.com/html/?q=" + detail::url_encode(query);
        auto html = detail::run_curl(url);
        if (!html) return ToolResult::error(std::format("Failed to search for: {}", query));
        return ToolResult::success(*html);
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
