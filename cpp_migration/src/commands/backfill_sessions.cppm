module;
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
export module cc.commands.backfill_sessions;

import cc.services.assistant_session_history;

export namespace cc::commands::backfill_sessions {
namespace fs = std::filesystem;

struct CommandResponse { bool ok{true}; std::string message; };
[[nodiscard]] inline auto name() -> std::string_view { return "backfill_sessions"; }

struct LocalSessionMeta {
    std::string id;
    std::string title;
    std::string cwd;
    std::uint64_t updated_at_epoch{0};
    std::uint64_t message_count{0};
};

[[nodiscard]] inline fs::path default_sessions_dir() {
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".cc-repl" / "sessions";
    return fs::path(".cc-repl") / "sessions";
}

[[nodiscard]] inline std::uint64_t parse_u64(std::string_view value) {
    try {
        return static_cast<std::uint64_t>(std::stoull(std::string(value)));
    } catch (...) {
        return 0;
    }
}

[[nodiscard]] inline std::optional<LocalSessionMeta> parse_session_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;

    LocalSessionMeta meta;
    meta.id = path.stem().string();
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = std::string_view(line).substr(0, eq);
        const auto value = std::string_view(line).substr(eq + 1);
        if (key == "id") meta.id = std::string(value);
        else if (key == "title") meta.title = std::string(value);
        else if (key == "cwd") meta.cwd = std::string(value);
        else if (key == "message_count") meta.message_count = parse_u64(value);
        else if (key == "updated_at") meta.updated_at_epoch = parse_u64(value);
    }

    if (meta.id.empty()) return std::nullopt;
    return meta;
}

[[nodiscard]] inline std::vector<LocalSessionMeta> load_local_sessions(std::size_t limit) {
    std::vector<LocalSessionMeta> sessions;
    std::error_code ec;
    const auto dir = default_sessions_dir();
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return sessions;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        if (auto parsed = parse_session_file(entry.path())) sessions.push_back(std::move(*parsed));
    }

    std::ranges::sort(sessions, [](const auto& left, const auto& right) {
        return left.updated_at_epoch > right.updated_at_epoch;
    });
    if (sessions.size() > limit) sessions.resize(limit);
    return sessions;
}

[[nodiscard]] inline auto run(std::string_view scope = {}) -> CommandResponse {
    std::size_t limit = 500;
    if (!scope.empty() && scope != "all") {
        try {
            limit = static_cast<std::size_t>(std::stoull(std::string(scope)));
        } catch (...) {
            return {.ok = false, .message = "backfill-sessions expects an optional numeric limit or 'all'"};
        }
    }

    auto sessions = load_local_sessions(limit);
    auto history = cc::services::assistant::load_session_history("");
    std::unordered_set<std::string> known_ids;
    known_ids.reserve(history.size());
    for (const auto& entry : history) {
        known_ids.insert(entry.session_id);
    }

    std::size_t added = 0;
    for (const auto& session : sessions) {
        if (known_ids.contains(session.id)) continue;
        cc::services::assistant::SessionHistoryEntry entry{
            .session_id = session.id,
            .title = session.title.empty() ? session.id : session.title,
            .timestamp = std::to_string(session.updated_at_epoch),
            .message_count = static_cast<std::uint64_t>(session.message_count),
            .project_path = session.cwd.empty() ? std::nullopt : std::optional<std::string>{session.cwd},
        };
        if (cc::services::assistant::save_session_entry(entry, "")) {
            ++added;
            known_ids.insert(session.id);
        }
    }

    return {.ok = true, .message = std::format(
        "Session history backfill complete: scanned {}, added {}, history path {}",
        sessions.size(), added, cc::services::assistant::default_history_path().string())};
}
}
