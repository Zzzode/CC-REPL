module;
#include <cstdlib>
#include <filesystem>
#include <format>
#include <unistd.h>
#include <string>
#include <string_view>
export module cc.commands.ant_trace;

import cc.constants.product;

export namespace cc::commands::ant_trace {
namespace fs = std::filesystem;

struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "ant_trace"; }

[[nodiscard]] inline auto run(std::string_view target = {}) -> CommandResponse {
    std::error_code ec;
    const auto cwd = fs::current_path(ec);
    const bool profile_enabled = std::getenv("CLAUDE_CODE_PROFILE_STARTUP") != nullptr;
    const bool debug_enabled = std::getenv("DEBUG") != nullptr || std::getenv("CC_REPL_DEBUG") != nullptr;
    return {.ok = true, .message = std::format(
        "ANT trace snapshot\n"
        "Target: {}\n"
        "Version: {}\n"
        "PID: {}\n"
        "CWD: {}\n"
        "Startup profiling: {}\n"
        "Debug logging: {}",
        target.empty() ? "current-session" : std::string(target),
        cc::constants::product::CC_REPL_VERSION,
        static_cast<long>(::getpid()),
        ec ? "<unavailable>" : cwd.string(),
        profile_enabled ? "enabled" : "disabled",
        debug_enabled ? "enabled" : "disabled")};
}
}
