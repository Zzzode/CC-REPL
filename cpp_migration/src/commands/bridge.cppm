module;
#include <format>
#include <string>
#include <string_view>
export module cc.commands.bridge;

import cc.utils.ide_integration;

export namespace cc::commands::bridge {
struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "bridge"; }

[[nodiscard]] inline auto run(std::string_view action = {}) -> CommandResponse {
    cc::utils::ide::IdeLockfileScanner scanner;
    auto lockfiles = scanner.scan();

    if (action.empty() || action == "status") {
        if (auto best = scanner.find_best_match()) {
            return {.ok = true, .message = std::format(
                "Bridge status: IDE lockfile detected\nIDE: {}\nPort: {}\nWorkspace: {}",
                best->name, best->port, best->workspace_folder.string())};
        }
        return {.ok = true, .message = std::format(
            "Bridge status: disconnected\nLockfile directory: {}\nActive IDE lockfiles: 0",
            scanner.lockfile_dir().string())};
    }

    if (action == "list") {
        std::string message = std::format("IDE bridge lockfiles: {}\n", lockfiles.size());
        for (const auto& lockfile : lockfiles) {
            message += std::format("- {} pid={} port={} workspace={}\n",
                lockfile.name, lockfile.pid, lockfile.port, lockfile.workspace_folder.string());
        }
        return {.ok = true, .message = std::move(message)};
    }

    if (action == "disconnect") {
        return {.ok = true, .message = "Bridge disconnect requested; no active in-process bridge transport is attached to this command"};
    }

    return {.ok = false, .message = "bridge supports: status, list, disconnect"};
}
}
