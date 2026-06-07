module;
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
export module cc.commands.onboarding;
export namespace cc::commands::onboarding {
namespace fs = std::filesystem;

struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "onboarding"; }

[[nodiscard]] inline auto run(std::string_view step = {}) -> CommandResponse {
    std::error_code ec;
    auto exists = [&](std::string_view path) {
        return fs::exists(fs::path(path), ec);
    };

    if (!step.empty() && step != "status") {
        return {.ok = false, .message = "onboarding supports: status"};
    }

    const bool has_agent_docs = exists("AGENTS.md") || exists("CLAUDE.md");
    const bool has_package = exists("package.json");
    const bool has_native_binary = exists("dist/cc-repl");

    return {.ok = true, .message = std::format(
        "Onboarding status\n"
        "Project instructions: {}\n"
        "Package manifest: {}\n"
        "Native binary: {}",
        has_agent_docs ? "present" : "missing",
        has_package ? "present" : "missing",
        has_native_binary ? "present" : "missing")};
}
}
