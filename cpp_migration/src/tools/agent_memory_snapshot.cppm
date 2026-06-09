/// @file agent_memory_snapshot.cppm
/// @brief Project-level agent memory snapshots (initialization + sync tracking).
/// Migrated from src/tools/AgentTool/agentMemorySnapshot.ts.
///
/// A snapshot is a copy of an agent's memory files stored under
/// `<cwd>/.claude/agent-memory-snapshots/<agentType>/`.  The snapshot includes
/// a metadata file `snapshot.json` (with an `updatedAt` ISO timestamp) and one
/// or more `*.md` files.  When an agent is spawned, if the agent has no local
/// memory yet the snapshot is copied in (first-run initialization); if the
/// snapshot has been updated since the last sync, the local memory is either
/// refreshed or the user is prompted (prompt-update).
module;

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

export module cc.tools.agent_memory_snapshot;

import cc.tools.agent_memory;
import cc.utils.json;

export namespace cc::tools::agent_memory_snapshot {

namespace fs = std::filesystem;
using cc::tools::agent_memory::Scope;
using cc::tools::agent_memory::agent_memory_dir;
using cc::tools::agent_memory::sanitize_agent_type_for_path;

// ---------------------------------------------------------------------------
// Filenames / directory layout
// ---------------------------------------------------------------------------

inline constexpr std::string_view SNAPSHOT_BASE = "agent-memory-snapshots";
inline constexpr std::string_view SNAPSHOT_JSON = "snapshot.json";
inline constexpr std::string_view SYNCED_JSON   = ".snapshot-synced.json";

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

/// `<cwd>/.claude/agent-memory-snapshots/<agentType>/`
[[nodiscard]] inline fs::path snapshot_dir_for_agent(
    std::string_view agent_type,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    const auto cwd = working_dir ? *working_dir : fs::current_path();
    return cwd / ".claude" / SNAPSHOT_BASE / sanitize_agent_type_for_path(agent_type);
}

[[nodiscard]] inline fs::path snapshot_json_path(
    std::string_view agent_type,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    return snapshot_dir_for_agent(agent_type, working_dir) / SNAPSHOT_JSON;
}

[[nodiscard]] inline fs::path synced_json_path(
    std::string_view agent_type,
    Scope scope,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    return agent_memory_dir(agent_type, scope, working_dir) / SYNCED_JSON;
}

// ---------------------------------------------------------------------------
// Low-level JSON helpers
// ---------------------------------------------------------------------------

/// Best-effort extraction of an `updatedAt` / `syncedFrom` ISO-8601 timestamp
/// from a tiny JSON object of the form `{"updatedAt":"..."}`.  The TS code
/// uses Zod schemas; here we use an intentionally loose string search so that
/// a malformed file simply returns nullopt instead of crashing.
[[nodiscard]] inline std::optional<std::string> read_iso_timestamp(
    const fs::path& json_file,
    std::string_view field_name
) {
    std::error_code ec;
    if (!fs::is_regular_file(json_file, ec)) return std::nullopt;

    std::ifstream ifs(json_file, std::ios::binary);
    if (!ifs) return std::nullopt;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    const std::string content = ss.str();
    if (content.empty()) return std::nullopt;

    const std::string marker = std::string{"\""} + std::string{field_name} + "\":\"";
    auto pos = content.find(marker);
    if (pos == std::string::npos) return std::nullopt;
    pos += marker.size();
    auto end = content.find('"', pos);
    if (end == std::string::npos) return std::nullopt;
    return content.substr(pos, end - pos);
}

/// Write a tiny JSON object `{"<field>":"<value>"}` to a path.  Used for both
/// `snapshot.json` (by other writers) and `.snapshot-synced.json` (below).
inline void write_simple_json(
    const fs::path& path,
    std::string_view field_name,
    std::string_view value
) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return;
    ofs << "{\"" << field_name << "\":\"" << value << "\"}";
}

// ---------------------------------------------------------------------------
// File copy / remove primitives
// ---------------------------------------------------------------------------

/// Copy all `*.md` files (and any other files except `snapshot.json`) from the
/// snapshot directory into the agent's local memory directory.
inline void copy_snapshot_to_local(
    std::string_view agent_type,
    Scope scope,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    const auto snapshot_mem_dir = snapshot_dir_for_agent(agent_type, working_dir);
    const auto local_mem_dir    = agent_memory_dir(agent_type, scope, working_dir);

    std::error_code ec;
    fs::create_directories(local_mem_dir, ec);

    if (!fs::is_directory(snapshot_mem_dir, ec)) return;

    for (const auto& entry : fs::directory_iterator(snapshot_mem_dir, ec)) {
        if (ec) return;
        if (!entry.is_regular_file(ec)) continue;
        const auto filename = entry.path().filename();
        if (filename == SNAPSHOT_JSON) continue;
        fs::copy_file(
            entry.path(),
            local_mem_dir / filename,
            fs::copy_options::overwrite_existing,
            ec
        );
    }
}

/// Save the `.snapshot-synced.json` metadata for a given agent + scope.
inline void save_synced_meta(
    std::string_view agent_type,
    Scope scope,
    std::string_view snapshot_timestamp,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    write_simple_json(
        synced_json_path(agent_type, scope, working_dir),
        "syncedFrom",
        snapshot_timestamp
    );
}

/// Return true if the agent's local memory directory contains any `*.md` files.
[[nodiscard]] inline bool has_local_memory_files(
    std::string_view agent_type,
    Scope scope,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    const auto local_mem_dir = agent_memory_dir(agent_type, scope, working_dir);
    std::error_code ec;
    if (!fs::is_directory(local_mem_dir, ec)) return false;
    for (const auto& entry : fs::directory_iterator(local_mem_dir, ec)) {
        if (ec) return false;
        if (entry.is_regular_file(ec) && entry.path().extension() == ".md") {
            return true;
        }
    }
    return false;
}

/// Compare two ISO-8601-ish timestamps lexicographically (the format produced
/// by `toISOString` sorts correctly as a string).
[[nodiscard]] inline bool iso_newer_than(
    std::string_view a,
    std::string_view b
) {
    return a > b;
}

// ---------------------------------------------------------------------------
// Public API (matches TS module exports 1:1)
// ---------------------------------------------------------------------------

/// Result of `check_agent_memory_snapshot` — what should happen next.
enum class SnapshotAction {
    None,          ///< No snapshot, or snapshot already in sync.
    Initialize,    ///< First-run: copy snapshot into empty local memory.
    PromptUpdate,  ///< Snapshot is newer than last sync; offer to replace.
};

struct SnapshotCheckResult {
    SnapshotAction action = SnapshotAction::None;
    std::optional<std::string> snapshot_timestamp;
};

/// Check if a snapshot exists and whether it is newer than the last sync.
[[nodiscard]] inline SnapshotCheckResult check_agent_memory_snapshot(
    std::string_view agent_type,
    Scope scope,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    const auto snapshot_ts = read_iso_timestamp(
        snapshot_json_path(agent_type, working_dir), "updatedAt"
    );
    if (!snapshot_ts) return {SnapshotAction::None, std::nullopt};

    if (!has_local_memory_files(agent_type, scope, working_dir)) {
        return {SnapshotAction::Initialize, snapshot_ts};
    }

    const auto synced_ts = read_iso_timestamp(
        synced_json_path(agent_type, scope, working_dir), "syncedFrom"
    );
    if (!synced_ts || iso_newer_than(*snapshot_ts, *synced_ts)) {
        return {SnapshotAction::PromptUpdate, snapshot_ts};
    }
    return {SnapshotAction::None, std::nullopt};
}

/// Initialize local agent memory from a snapshot (first-time setup).
inline void initialize_from_snapshot(
    std::string_view agent_type,
    Scope scope,
    std::string_view snapshot_timestamp,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    copy_snapshot_to_local(agent_type, scope, working_dir);
    save_synced_meta(agent_type, scope, snapshot_timestamp, working_dir);
}

/// Replace local agent memory with the snapshot.
/// First removes existing `*.md` files to avoid orphaned entries.
inline void replace_from_snapshot(
    std::string_view agent_type,
    Scope scope,
    std::string_view snapshot_timestamp,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    const auto local_mem_dir = agent_memory_dir(agent_type, scope, working_dir);
    std::error_code ec;
    if (fs::is_directory(local_mem_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(local_mem_dir, ec)) {
            if (ec) break;
            if (entry.is_regular_file(ec) && entry.path().extension() == ".md") {
                fs::remove(entry.path(), ec);
            }
        }
    }
    copy_snapshot_to_local(agent_type, scope, working_dir);
    save_synced_meta(agent_type, scope, snapshot_timestamp, working_dir);
}

/// Mark the current snapshot as synced without changing local memory.
inline void mark_snapshot_synced(
    std::string_view agent_type,
    Scope scope,
    std::string_view snapshot_timestamp,
    const std::optional<fs::path>& working_dir = std::nullopt
) {
    save_synced_meta(agent_type, scope, snapshot_timestamp, working_dir);
}

} // namespace cc::tools::agent_memory_snapshot
