// GrepTool - File content grep search
module;

#include <filesystem>
#include <expected>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <regex>

export module cc.tools.grep;

import cc.utils.file;
import cc.utils.error;
import cc.tools.tool;
import cc.utils.json;

export namespace cc::tools::grep {

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
// GrepTool Implementation
// =========================================================================

/// GrepTool - Searches file contents
class GrepTool {
public:
    static constexpr std::string_view kName = "Grep";
    static constexpr std::string_view kDescription = 
        "Search file contents for a pattern (supports regex).";
    
    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = std::string(kDescription),
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{
                        .name = "pattern",
                        .type = "string",
                        .description = "Search pattern (string or regex)",
                        .required = true
                    },
                    SchemaProperty{
                        .name = "path",
                        .type = "string",
                        .description = "Directory or file to search in",
                        .required = false
                    }
                }
            },
            .permission = ToolPermission::ReadOnly,
            .category = "filesystem",
            .max_result_size_chars = 20'000
        };
    }
    
    GrepTool() = default;
    
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        return true;
    }
    
    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        using namespace cc::utils::json;

        std::string pattern;
        std::string search_path = fs::current_path().string();

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
        if (pattern_node.is_str()) {
            pattern = std::string(pattern_node.as_str());
        }

        auto path_node = root.get("path");
        if (path_node.is_str()) {
            search_path = std::string(path_node.as_str());
        }

        if (pattern.empty()) {
            return ToolResult::error("Missing 'pattern' field");
        }

        try {
            std::vector<std::pair<fs::path, std::vector<std::pair<int, std::string>>>> matches;
            std::regex regex_pattern(pattern);
            std::size_t match_count = 0;

            auto search_file = [&](const fs::path& file) {
                auto file_read = cc::utils::file::read_file(file);
                if (!file_read) return;

                std::istringstream iss(*file_read);
                std::string line;
                int line_num = 0;
                std::vector<std::pair<int, std::string>> file_matches;

                while (std::getline(iss, line)) {
                    ++line_num;
                    if (std::regex_search(line, regex_pattern)) {
                        file_matches.push_back({line_num, line});
                        if (++match_count >= 200) break;
                    }
                }

                if (!file_matches.empty()) {
                    matches.push_back({file, std::move(file_matches)});
                }
            };

            fs::path root_path = search_path;
            if (fs::is_regular_file(root_path)) {
                search_file(root_path);
            } else {
                for (const auto& entry : fs::recursive_directory_iterator(root_path, fs::directory_options::skip_permission_denied)) {
                    if (entry.is_regular_file()) search_file(entry.path());
                    if (match_count >= 200) break;
                }
            }

            std::string result;
            result = std::format("Searching for '{}':\n\n", pattern);
            
            for (const auto& [file, lines] : matches) {
                result += std::format("In file {}:\n", file.string());
                for (const auto& [line_num, line] : lines) {
                    result += std::format("  {}: {}\n", line_num, line);
                }
                result += "\n";
            }

            if (match_count >= 200) {
                result += "[results truncated at 200 matches]\n";
            }

            if (matches.empty()) {
                result += "No matches found.\n";
            }

            return ToolResult::success(result);

        } catch (const std::regex_error& e) {
            return ToolResult::error(std::format("Invalid regex pattern: {}", e.what()));
        } catch (const std::exception& e) {
            return ToolResult::error(std::format("Grep error: {}", e.what()));
        }
    }
};

} // namespace cc::tools::grep

// Export main tool class
export namespace cc::tools {
    using cc::tools::grep::GrepTool;

    /// Factory: create GrepTool wrapped as ITool (adapts Result types across modules)
    [[nodiscard]] auto make_grep_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            GrepTool tool_;
            cc::core::ToolDefinition def_ = GrepTool::definition();

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
