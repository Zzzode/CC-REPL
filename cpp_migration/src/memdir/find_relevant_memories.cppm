/// @file find_relevant_memories.cppm
/// @brief Find memory files relevant to a user query using LLM selection.
/// Migrated from: src/memdir/findRelevantMemories.ts
module;

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

export module cc.memdir.find_relevant_memories;

export namespace cc::memdir {

/// A memory file selected as relevant to the current query
struct RelevantMemory {
    std::string path;       // Absolute file path
    int64_t mtime_ms;      // Modification time in milliseconds
};

/// Header information scanned from a memory file
struct MemoryHeader {
    std::string filename;
    std::string file_path;
    int64_t mtime_ms;
    std::string description;
};

/// System prompt used for memory selection
inline constexpr std::string_view kSelectMemoriesSystemPrompt =
    "You are selecting memories that will be useful to Claude Code as it processes "
    "a user's query. You will be given the user's query and a list of available memory "
    "files with their filenames and descriptions.\n\n"
    "Return a list of filenames for the memories that will clearly be useful to Claude "
    "Code as it processes the user's query (up to 5). Only include memories that you are "
    "certain will be helpful based on their name and description.\n"
    "- If you are unsure if a memory will be useful in processing the user's query, then "
    "do not include it in your list. Be selective and discerning.\n"
    "- If there are no memories in the list that would clearly be useful, feel free to "
    "return an empty list.\n"
    "- If a list of recently-used tools is provided, do not select memories that are "
    "usage reference or API documentation for those tools (Claude Code is already "
    "exercising them). DO still select memories containing warnings, gotchas, or known "
    "issues about those tools — active use is exactly when those matter.";

/// Callback type for scanning memory files from a directory.
/// Implementations should filter out MEMORY.md and return headers for all .md files found.
using ScanMemoryFilesFn = std::function<std::vector<MemoryHeader>(
    const std::string& memory_dir)>;

/// Callback type for selecting relevant memories via an LLM side-query.
/// Given a query and available memories, returns filenames of selected memories.
using SelectMemoriesFn = std::function<std::vector<std::string>(
    const std::string& query,
    const std::vector<MemoryHeader>& memories,
    const std::vector<std::string>& recent_tools)>;

/// Find memory files relevant to a query.
///
/// Scans memory file headers from memory_dir and asks the LLM to select the
/// most relevant ones (up to 5). Excludes MEMORY.md (already in system prompt).
///
/// @param query          The user's query text
/// @param memory_dir     Path to the memory directory to scan
/// @param recent_tools   Tools recently used (to avoid selecting their docs)
/// @param already_surfaced  Paths already shown in prior turns (excluded from selection)
/// @param scan_fn        Function to scan memory files
/// @param select_fn      Function to select relevant memories via LLM
/// @return Vector of RelevantMemory with absolute paths and mtime
[[nodiscard]] inline std::vector<RelevantMemory> find_relevant_memories(
    const std::string& query,
    const std::string& memory_dir,
    const std::vector<std::string>& recent_tools,
    const std::set<std::string>& already_surfaced,
    ScanMemoryFilesFn scan_fn,
    SelectMemoriesFn select_fn
) {
    // Scan and filter out already-surfaced memories
    auto memories = scan_fn(memory_dir);
    std::erase_if(memories, [&already_surfaced](const MemoryHeader& m) {
        return already_surfaced.contains(m.file_path);
    });

    if (memories.empty()) {
        return {};
    }

    // Ask LLM to select relevant filenames
    auto selected_filenames = select_fn(query, memories, recent_tools);

    // Build filename -> header lookup
    std::unordered_map<std::string, const MemoryHeader*> by_filename;
    for (const auto& m : memories) {
        by_filename[m.filename] = &m;
    }

    // Map selected filenames back to RelevantMemory results
    std::vector<RelevantMemory> result;
    result.reserve(selected_filenames.size());
    for (const auto& filename : selected_filenames) {
        if (auto it = by_filename.find(filename); it != by_filename.end()) {
            result.push_back(RelevantMemory{
                .path = it->second->file_path,
                .mtime_ms = it->second->mtime_ms,
            });
        }
    }

    return result;
}

} // namespace cc::memdir
