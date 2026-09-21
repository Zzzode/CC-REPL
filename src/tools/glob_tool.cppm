// GlobTool - File system glob pattern matching
module;

#include <filesystem>
#include <format>
#include <expected>
#include <memory>
#include <optional>
#include <algorithm>
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

[[nodiscard]] std::string slash_path(const fs::path& path) {
    auto text = path.generic_string();
    return text.empty() ? "." : text;
}

[[nodiscard]] std::string glob_to_regex(std::string_view pattern) {
    std::string out = "^";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                out += ".*";
                ++i;
            } else {
                out += "[^/]*";
            }
        } else if (c == '?') {
            out += "[^/]";
        } else if (std::string_view{R"(\.^$+{}[]()|)"}.find(c) != std::string_view::npos) {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    out += "$";
    return out;
}

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
                        .required = true,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt
                    },
                    SchemaProperty{
                        .name = "path",
                        .type = "string",
                        .description = "Directory to search in (default: current)",
                        .required = false,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt
                    }
                }
            },
            .permission = ToolPermission::ReadOnly,
            .category = "filesystem"
        };
    }
    
    GlobTool() = default;
    
    [[nodiscard]] bool check_permission([[maybe_unused]] const ToolInput& input) const {
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
            
            auto regex_pattern = std::regex(glob_to_regex(pattern));
            auto recursive = pattern.find("**") != std::string::npos || pattern.find('/') != std::string::npos;
            auto consider = [&](const fs::directory_entry& entry) {
                auto rel = slash_path(fs::relative(entry.path(), base_path));
                auto name = entry.path().filename().generic_string();
                if (std::regex_match(rel, regex_pattern) || std::regex_match(name, regex_pattern)) {
                    matches.push_back(entry.path());
                }
            };

            if (recursive) {
                for (const auto& entry : fs::recursive_directory_iterator(base_path, fs::directory_options::skip_permission_denied)) {
                    consider(entry);
                    if (matches.size() >= 1000) break;
                }
            } else {
                for (const auto& entry : fs::directory_iterator(base_path, fs::directory_options::skip_permission_denied)) {
                    consider(entry);
                    if (matches.size() >= 1000) break;
                }
            }

            std::ranges::sort(matches);
            
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
