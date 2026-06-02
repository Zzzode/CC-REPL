/// @file memory_scan.cppm
/// @brief Memory file scanning and content extraction.
/// Migrated from src/memdir/memoryScan.ts, findRelevantMemories.ts, memoryAge.ts
module;

#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <optional>
#include <algorithm>
#include <sstream>

export module cc.memdir.memory_scan;

import cc.memdir.paths;

export namespace cc::memdir {

/// A single memory entry extracted from a CLAUDE.md file
struct MemoryEntry {
    std::string content;
    MemoryType source_type;
    std::filesystem::path source_path;
    std::chrono::system_clock::time_point last_modified;
};

/// Read and parse a CLAUDE.md file into its content
[[nodiscard]] inline std::optional<std::string> read_memory_file(
    const std::filesystem::path& path
) {
    if (!std::filesystem::exists(path)) return std::nullopt;
    
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;
    
    std::ostringstream ss;
    ss << file.rdbuf();
    auto content = ss.str();
    
    if (content.empty()) return std::nullopt;
    return content;
}

/// Scan all memory paths and collect entries
[[nodiscard]] inline std::vector<MemoryEntry> scan_all_memories(
    const std::filesystem::path& cwd,
    const std::filesystem::path& project_root
) {
    std::vector<MemoryEntry> entries;
    auto paths = resolve_all_memory_paths(cwd, project_root);
    
    for (const auto& mp : paths) {
        if (!mp.exists) continue;
        
        auto content = read_memory_file(mp.path);
        if (!content) continue;
        
        auto last_write = std::filesystem::last_write_time(mp.path);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            std::chrono::system_clock::now() +
            (last_write - std::filesystem::file_time_type::clock::now()));
        
        entries.push_back(MemoryEntry{
            .content = std::move(*content),
            .source_type = mp.type,
            .source_path = mp.path,
            .last_modified = sctp,
        });
    }
    
    return entries;
}

/// Get memory age description (e.g., "2 days ago", "just now")
[[nodiscard]] inline std::string get_memory_age_description(
    std::chrono::system_clock::time_point modified_at
) {
    auto now = std::chrono::system_clock::now();
    auto diff = now - modified_at;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(diff).count();
    
    if (hours < 1) return "just now";
    if (hours < 24) return std::to_string(hours) + " hours ago";
    auto days = hours / 24;
    if (days == 1) return "yesterday";
    if (days < 30) return std::to_string(days) + " days ago";
    auto months = days / 30;
    if (months == 1) return "1 month ago";
    return std::to_string(months) + " months ago";
}

} // namespace cc::memdir
