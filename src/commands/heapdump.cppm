/// @file heapdump.cppm
/// @brief HeapdumpCommand implementing the /heapdump slash command.
/// Reports process memory diagnostics via getrusage. The native C++ runtime
/// has no V8 heap to snapshot (unlike the TypeScript CLI), so this is the
/// closest available analogue for memory inspection.
module;

#include <string>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <array>
#include <sys/resource.h>

export module cc.commands.heapdump;

import cc.types.types;
import cc.commands.command;

// Module-internal helpers (module linkage; intentionally not exported).
namespace cc::commands {

/// Format a kilobyte count into a human-readable string.
[[nodiscard]] inline std::string format_kb(long kb) {
    if (kb < 0) return "n/a";
    if (kb < 1024) return std::format("{} KB", kb);
    return std::format("{:.2f} MB ({} KB)", kb / 1024.0, kb);
}

} // namespace cc::commands

export namespace cc::commands {

using namespace cc::core;

/// HeapdumpCommand implements the /heapdump slash command.
/// The native C++ runtime has no V8 heap; instead it reports process memory
/// via getrusage(2), which is the closest available analogue.
class HeapdumpCommand {
public:
    [[nodiscard]] static CommandDefinition definition() {
        return CommandDefinition{
            .name = "heapdump",
            .description = "Report process memory diagnostics",
            .args = {},
            .category = "debug",
            .aliases = {},
            .hidden = false,
        };
    }

    [[nodiscard]] static VoidResult validate(const CommandContext&) {
        return {};
    }

    [[nodiscard]] static Result<CommandResult> execute(const CommandContext&) {
        std::string report = "Memory diagnostics (native C++ build):\n";

        struct rusage ru{};
        if (getrusage(RUSAGE_SELF, &ru) == 0) {
            // ru_maxrss is in bytes on macOS, kilobytes on Linux.
#if defined(__APPLE__)
            long rss_kb = ru.ru_maxrss / 1024;
#else
            long rss_kb = ru.ru_maxrss;
#endif
            report += std::format("  Resident set size (RSS): {}\n", format_kb(rss_kb));
            report += std::format("  Page reclaims (soft faults): {}\n", ru.ru_minflt);
            report += std::format("  Page faults (hard): {}\n", ru.ru_majflt);
            report += std::format("  Voluntary context switches: {}\n", ru.ru_nvcsw);
            report += std::format("  Involuntary context switches: {}\n", ru.ru_nivcsw);
        } else {
            report += "  getrusage() unavailable on this platform.\n";
        }

        report += "\nNote: the TypeScript CLI writes a V8 heap snapshot to ~/Desktop. "
                  "The native C++ runtime has no V8 heap; this command reports "
                  "process-level memory via getrusage(2) instead. For a full heap "
                  "profile, rebuild with a sanitizer or a tcmalloc/jemalloc profiler.";
        return CommandResult::success(std::move(report));
    }

    [[nodiscard]] static std::vector<std::string> complete(std::string_view) {
        return {};
    }
};

} // namespace cc::commands
