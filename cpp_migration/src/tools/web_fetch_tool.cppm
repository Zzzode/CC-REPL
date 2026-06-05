// WebFetchTool - Fetches web content from URLs
module;

#include <format>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <optional>
#include <memory>
#include <expected>

export module cc.tools.web_fetch;

import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::web_fetch {

namespace detail {
[[nodiscard]] inline auto shell_quote(std::string_view s) -> std::string {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += R"('\'')";
        else out += ch;
    }
    out += "'";
    return out;
}

[[nodiscard]] inline auto run_curl(std::string_view url) -> std::optional<std::string> {
    const auto cmd = "curl -fsSL --max-time 30 " + shell_quote(url) + " 2>&1";
    std::array<char, 4096> buf{};
    std::string out;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) out += buf.data();
    const int status = pclose(pipe);
    if (status != 0) return std::nullopt;
    return out;
}

[[nodiscard]] inline auto parse_url(std::string_view json) -> std::expected<std::string, std::string> {
    auto parsed = cc::utils::json::parse(json);
    if (!parsed || !parsed->root().is_obj()) {
        return std::unexpected("Invalid JSON input");
    }

    auto url = parsed->root().get("url");
    if (!url.is_str() || url.as_str().empty()) {
        return std::unexpected("Missing 'url' field");
    }

    return std::string(url.as_str());
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
// WebFetchTool Implementation
// =========================================================================

/// WebFetchTool - Fetches web content
class WebFetchTool {
public:
    static constexpr std::string_view kName = "WebFetch";
    static constexpr std::string_view kDescription = 
        "Fetch the contents of a web page from a URL.";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "url",
                        .type = "string",
                        .description = "URL to fetch content from",
                        .required = true
                    }
                }
            },
            .permission = ToolPermission::Network,
            .category = "network"
        };
    }
    
    WebFetchTool() = default;
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        return true;
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed_url = detail::parse_url(input.json());
        if (!parsed_url) {
            return ToolResult::error(parsed_url.error());
        }

        const auto& url = *parsed_url;
        if (!url.starts_with("http://") && !url.starts_with("https://")) {
            return ToolResult::error("URL must start with http:// or https://");
        }
        auto content = detail::run_curl(url);
        if (!content) return ToolResult::error(std::format("Failed to fetch URL: {}", url));
        return ToolResult::success(*content);
    }
};

} // namespace cc::tools::web_fetch

// Export main tool class
export namespace cc::tools {
    using cc::tools::web_fetch::WebFetchTool;

    /// Factory: create WebFetchTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_web_fetch_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            WebFetchTool tool_;
            cc::core::ToolDefinition def_ = WebFetchTool::definition();

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
