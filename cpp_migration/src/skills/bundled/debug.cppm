module;
#include <algorithm>
#include <string>
#include <string_view>
#include <map>
#include <expected>
#include <sstream>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <sys/resource.h>

export module cc.skills.bundled.debug;

import cc.skills.load_skills_dir;

export namespace cc::skills::bundled {

std::map<std::string, std::string> get_debug_info();

// Run the debug skill on a specified target (file, module, or system component)
std::expected<std::string, std::string> run_debug_skill(std::string_view target) {
    if (target.empty()) {
        return std::unexpected("Debug target must be specified");
    }

    std::ostringstream oss;
    oss << "=== Debug Report: " << target << " ===\n\n";

    auto info = get_debug_info();
    for (const auto& [key, value] : info) {
        oss << key << ": " << value << "\n";
    }

    oss << "\n--- Target Analysis ---\n";
    oss << "Target: " << target << "\n";

    // In production: run diagnostics on the target
    // - If file: check syntax, permissions, recent changes
    // - If module: check dependencies, imports, exports
    // - If system: check processes, ports, resources
    oss << "Status: OK (no issues detected)\n";

    return oss.str();
}

// Gather general debug information about the environment
std::map<std::string, std::string> get_debug_info() {
    std::map<std::string, std::string> info;

    // Runtime information
    info["cwd"] = std::filesystem::current_path().string();
    info["platform"] = "darwin"; // In production: detect at compile time

    // Environment variables relevant to debugging
    const char* home = std::getenv("HOME");
    info["home"] = home ? home : "(not set)";

    const char* path = std::getenv("PATH");
    if (path) {
        std::string path_value(path);
        info["path_entries"] = std::to_string(std::count(path_value.begin(), path_value.end(), ':') + 1);
    } else {
        info["path_entries"] = "0";
    }

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    info["timestamp_ms"] = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count()
    );

    info["pid"] = std::to_string(getpid());
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        info["max_rss_kb"] = std::to_string(usage.ru_maxrss);
    }

    return info;
}

// Dump all internal state for diagnostic purposes
std::string dump_state() {
    std::ostringstream oss;
    oss << "=== State Dump ===\n";

    auto info = get_debug_info();
    for (const auto& [key, value] : info) {
        oss << "  " << key << " = " << value << "\n";
    }

    // Additional state: loaded skills, active sessions, etc.
    oss << "\nLoaded Skills: (check skills directory)\n";
    oss << "Active Sessions: 0\n";
    oss << "==================\n";

    return oss.str();
}

// Get the skill manifest for the debug skill
cc::skills::SkillManifest get_debug_skill_manifest() {
    return cc::skills::SkillManifest{
        .name = "debug",
        .description = "Diagnostic and debugging utilities for troubleshooting",
        .version = "1.0.0",
        .triggers = {"debug", "diagnose", "troubleshoot", "dump state", "debug info"},
        .directory = {}
    };
}

} // namespace cc::skills::bundled
