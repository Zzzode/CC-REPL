module;
#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <functional>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

export module cc.commands.release_notes;

export namespace cc::commands {

auto release_notes_cache_path() -> std::filesystem::path {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path{home} / ".cc-repl" / "release-notes.txt";
    return std::filesystem::path{".cc-repl"} / "release-notes.txt";
}

auto show_whats_new() -> std::string;

// 获取指定版本的发布说明
auto get_release_notes(std::optional<std::string> version = std::nullopt)
    -> std::expected<std::string, std::string> {
    // 如果未指定版本，获取当前版本的说明
    std::string target_version = version.value_or("latest");

    std::ifstream input{release_notes_cache_path()};
    if (input) return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return std::string{"Release notes for " + target_version + "\n" + show_whats_new()};
}

// 获取自指定版本以来的变更日志
auto get_changelog_since(std::string_view version) -> std::string {
    if (version.empty()) {
        return "No version specified";
    }
    auto notes = get_release_notes();
    if (notes) return "Changes since " + std::string(version) + ":\n" + *notes;
    return notes.error();
}

// 显示"最新功能"摘要
auto show_whats_new() -> std::string {
    std::string content = "What's New:\n";
    content += "  - Improved context window management\n";
    content += "  - New /compact command for token optimization\n";
    content += "  - Enhanced tool permission system\n";
    content += "  - MCP server auto-discovery\n";
    return content;
}

// 检查是否有未读的发布说明
auto has_unread_release_notes() -> bool {
    return std::filesystem::exists(release_notes_cache_path());
}

} // namespace cc::commands
