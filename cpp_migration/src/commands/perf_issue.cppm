module;
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
export module cc.commands.perf_issue;

import cc.services.diagnostic.dump_diagnostic;

export namespace cc::commands::perf_issue {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "perf_issue"; }

[[nodiscard]] inline auto run(std::string_view target = {}) -> CommandResponse {
    const auto info = cc::services::diagnostic::collect_diagnostics(
        cc::services::diagnostic::DumpLevel::Verbose);
    return {.ok = true, .message = std::format(
        "Performance issue snapshot\n"
        "Target: {}\n"
        "Version: {}\n"
        "OS: {}\n"
        "Memory: {} bytes\n"
        "Working directory: {}\n"
        "Startup profiling hint: run with CLAUDE_CODE_PROFILE_STARTUP=1 for phase timings.",
        target.empty() ? "current-session" : std::string(target),
        info.app_version,
        info.os_version,
        info.memory_usage_bytes,
        info.working_directory)};
}
}
