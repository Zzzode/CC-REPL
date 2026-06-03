module;
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>
export module cc.commands.break_cache;
export namespace cc::commands::break_cache {
namespace fs = std::filesystem;

struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "break_cache"; }

[[nodiscard]] inline fs::path home_path(std::string_view suffix) {
    if (const char* home = std::getenv("HOME")) return fs::path(home) / suffix;
    return fs::path(suffix);
}

[[nodiscard]] inline std::vector<fs::path> cache_targets(std::string_view key) {
    std::vector<fs::path> targets;
    if (!key.empty() && key != "all") {
        targets.emplace_back(key);
        return targets;
    }
    targets.push_back(home_path(".cc-repl/cache"));
    targets.push_back(home_path(".cache/cc-repl"));
    targets.push_back(home_path(".claude/cache"));
    targets.push_back(fs::temp_directory_path() / "cc-repl");
    return targets;
}

[[nodiscard]] inline auto run(std::string_view key = {}) -> CommandResponse {
    std::uintmax_t removed = 0;
    std::size_t existing_targets = 0;
    std::vector<std::string> failures;

    for (const auto& target : cache_targets(key)) {
        std::error_code ec;
        if (!fs::exists(target, ec)) continue;
        ++existing_targets;
        removed += fs::remove_all(target, ec);
        if (ec) failures.push_back(std::format("{}: {}", target.string(), ec.message()));
    }

    if (!failures.empty()) {
        std::string message = "Cache removal failed:\n";
        for (const auto& failure : failures) message += "- " + failure + "\n";
        return {.ok = false, .message = std::move(message)};
    }

    return {.ok = true, .message = std::format(
        "Cache break complete: removed {} filesystem entries from {} target(s)",
        removed, existing_targets)};
}
}
