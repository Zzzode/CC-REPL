module;
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
export module cc.commands.bughunter;

import cc.services.diagnostic.dump_diagnostic;
import cc.utils.exec_sync;

export namespace cc::commands::bughunter {
namespace fs = std::filesystem;

struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "bughunter"; }

[[nodiscard]] inline auto run(std::string_view target = {}) -> CommandResponse {
    const auto diagnostics = cc::services::diagnostic::collect_diagnostics(
        cc::services::diagnostic::DumpLevel::Standard);
    const fs::path target_path = target.empty() ? fs::current_path() : fs::path(target);
    std::error_code ec;
    const bool target_exists = fs::exists(target_path, ec);
    const auto git_status = cc::utils::exec_sync("git status --short");
    const auto cmake_presets = fs::exists("cpp_migration/CMakePresets.json", ec) ||
                               fs::exists("CMakePresets.json", ec);

    return {.ok = true, .message = std::format(
        "Bughunter diagnostic snapshot\n"
        "Target: {} ({})\n"
        "OS: {}\n"
        "Memory: {} bytes\n"
        "Git status: {}\n"
        "CMake presets: {}",
        target_path.string(),
        target_exists ? "exists" : "missing",
        diagnostics.os_version,
        diagnostics.memory_usage_bytes,
        git_status ? (git_status->empty() ? "clean" : "dirty") : "not a git repository",
        cmake_presets ? "present" : "missing")};
}
}
