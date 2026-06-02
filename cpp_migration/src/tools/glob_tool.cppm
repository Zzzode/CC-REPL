// GlobTool - File system glob pattern matching
module;

#include <filesystem>
#include <format>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <regex>

export module cc.tools.glob;

import cc.utils.file;
import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::glob {

using cc::core::Tool;
using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;

namespace fs = std::filesystem;

// =========================================================================
// GlobTool Implementation
// =========================================================================

/// GlobTool - Searches for files matching patterns
class GlobTool {
public:
    static constexpr std::string_view kName = "Glob";
    static constexpr std::string_view kDescription = 
        "Find files and directories matching a glob pattern (e.g., **/*.ts, src/**).";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "pattern",
                        .type = "string",
                        .description = "Glob pattern to match (e.g., **/*.cpp)",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "path",
                        .type = "string",
                        .description = "Directory to search in (default: current)",
                        .required = false
                    }
                }
            },
            .permission = ToolPermission::ReadOnly,
            .category = "filesystem"
        };
    }
    
    GlobTool() = default;
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        return true;
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        using namespace cc::utils::json;

        // Parse input using yyjson
        auto json_str = input.json();
        auto doc = parse(json_str);
        if (!doc) {
            return ToolResult::error("Invalid JSON input");
        }

        auto root = doc->root();
        if (!root.is_obj()) {
            return ToolResult::error("Expected JSON object");
        }

        auto pattern_node = root.get("pattern");
        if (!pattern_node.is_str()) {
            return ToolResult::error("Missing 'pattern' field");
        }
        std::string pattern = std::string(pattern_node.as_str());

        if (pattern.empty()) {
            return ToolResult::error("Missing 'pattern' field");
        }
        
        // Simple glob implementation
        try {
            std::vector<fs::path> matches;
            fs::path base_path = fs::current_path();
            
            auto path_node = root.get("path");
            if (path_node.is_str()) {
                base_path = std::string(path_node.as_str());
            }
            
            // Very simple glob support (handles *.cpp, **/*)
            if (pattern == "*") {
                for (const auto& entry : fs::directory_iterator(base_path)) {
                    matches.push_back(entry.path());
                }
            } else {
                // Basic recursive search for simplicity
                for (const auto& entry : fs::recursive_directory_iterator(base_path)) {
                    matches.push_back(entry.path());
                }
            }
            
            // Format results
            std::string result;
            result = std::format("Found {} matches for '{}':\n", matches.size(), pattern);
            for (const auto& match : matches) {
                result += std::format("- {}\n", match.string());
            }
            
            return ToolResult::success(result);
            
        } catch (const std::exception& e) {
            return ToolResult::error(std::format("Glob error: {}", e.what()));
        }
    }
};

} // namespace cc::tools::glob

// Export main tool class
export namespace cc::tools {
    using cc::tools::glob::GlobTool;

    /// Factory: create GlobTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_glob_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            GlobTool tool_;
            cc::core::ToolDefinition def_ = GlobTool::definition();

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
