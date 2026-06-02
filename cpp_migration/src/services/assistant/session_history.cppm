/// @file session_history.cppm
/// @brief Session history persistence using NDJSON file storage.
/// Supports load, save, search, and pruning of session entries.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <format>

export module cc.services.assistant_session_history;

export namespace cc::services::assistant {

namespace fs = std::filesystem;

/// Represents a historical session entry
struct SessionHistoryEntry {
    std::string session_id;
    std::string title;
    std::string timestamp;
    std::uint64_t message_count{0};
    std::optional<std::string> project_path;
};

/// Session history storage configuration
struct SessionHistoryConfig {
    std::size_t max_entries{1000};
    bool auto_save{true};
    std::string storage_path;
};

/// Serialize a session entry to a single NDJSON line
[[nodiscard]] inline std::string serialize_entry(const SessionHistoryEntry& entry) {
    // Simple JSON serialization (no external lib)
    auto escape = [](std::string_view s) -> std::string {
        std::string result;
        result.reserve(s.size() + 8);
        for (char ch : s) {
            switch (ch) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:   result += ch; break;
            }
        }
        return result;
    };

    std::string json = std::format(
        R"({{"session_id":"{}","title":"{}","timestamp":"{}","message_count":{})",
        escape(entry.session_id), escape(entry.title),
        escape(entry.timestamp), entry.message_count);

    if (entry.project_path.has_value()) {
        json += std::format(R"(,"project_path":"{}")", escape(*entry.project_path));
    }
    json += '}';
    return json;
}

/// Parse a single NDJSON line into a session entry
[[nodiscard]] inline std::optional<SessionHistoryEntry> parse_entry(std::string_view line) {
    if (line.empty() || line[0] != '{') return std::nullopt;

    // Simple JSON field extraction
    auto extract_string = [&](std::string_view key) -> std::string {
        auto pattern = std::format(R"("{}":")", key);
        auto pos = line.find(pattern);
        if (pos == std::string_view::npos) return "";
        pos += pattern.size();
        std::string value;
        while (pos < line.size() && line[pos] != '"') {
            if (line[pos] == '\\' && pos + 1 < line.size()) {
                ++pos;
                switch (line[pos]) {
                    case 'n':  value += '\n'; break;
                    case 'r':  value += '\r'; break;
                    case 't':  value += '\t'; break;
                    case '"':  value += '"'; break;
                    case '\\': value += '\\'; break;
                    default:   value += line[pos]; break;
                }
            } else {
                value += line[pos];
            }
            ++pos;
        }
        return value;
    };

    auto extract_uint = [&](std::string_view key) -> std::uint64_t {
        auto pattern = std::format(R"("{}":)", key);
        auto pos = line.find(pattern);
        if (pos == std::string_view::npos) return 0;
        pos += pattern.size();
        std::uint64_t val = 0;
        while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
            val = val * 10 + (line[pos] - '0');
            ++pos;
        }
        return val;
    };

    SessionHistoryEntry entry;
    entry.session_id = extract_string("session_id");
    entry.title = extract_string("title");
    entry.timestamp = extract_string("timestamp");
    entry.message_count = extract_uint("message_count");

    auto project = extract_string("project_path");
    if (!project.empty()) {
        entry.project_path = std::move(project);
    }

    if (entry.session_id.empty()) return std::nullopt;
    return entry;
}

/// Get default storage path for session history
[[nodiscard]] inline fs::path default_history_path() {
    const char* home = std::getenv("HOME");
    if (home) {
        return fs::path(home) / ".claude" / "sessions" / "history.ndjson";
    }
    return fs::temp_directory_path() / "cc-repl-history.ndjson";
}

/// Load session history from disk
[[nodiscard]] inline std::vector<SessionHistoryEntry> load_session_history(
    std::string_view storage_path) {

    fs::path path = storage_path.empty()
        ? default_history_path()
        : fs::path(storage_path);

    std::vector<SessionHistoryEntry> entries;
    if (!fs::exists(path)) return entries;

    std::ifstream file(path);
    if (!file.is_open()) return entries;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (auto entry = parse_entry(line)) {
            entries.push_back(std::move(*entry));
        }
    }

    return entries;
}

/// Save a session entry to history (append to NDJSON file)
[[nodiscard]] inline bool save_session_entry(
    const SessionHistoryEntry& entry,
    std::string_view storage_path) {

    fs::path path = storage_path.empty()
        ? default_history_path()
        : fs::path(storage_path);

    // Ensure directory exists
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    // Append to file
    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) return false;

    file << serialize_entry(entry) << '\n';
    return file.good();
}

/// Search session history by keyword (matches title, session_id, project_path)
[[nodiscard]] inline std::vector<SessionHistoryEntry> search_sessions(
    std::string_view query, std::size_t limit = 20) {

    auto all = load_session_history("");
    if (query.empty()) {
        // Return most recent entries
        if (all.size() > limit) {
            all.erase(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(all.size() - limit));
        }
        return all;
    }

    std::vector<SessionHistoryEntry> matches;
    std::string query_lower(query);
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(),
        [](unsigned char c) { return std::tolower(c); });

    for (const auto& entry : all) {
        auto contains = [&](std::string_view field) -> bool {
            std::string lower(field);
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return std::tolower(c); });
            return lower.find(query_lower) != std::string::npos;
        };

        if (contains(entry.title) || contains(entry.session_id) ||
            (entry.project_path && contains(*entry.project_path))) {
            matches.push_back(entry);
            if (matches.size() >= limit) break;
        }
    }

    return matches;
}

/// Clear old sessions beyond max_entries, keeping most recent
/// Returns the number of entries pruned
[[nodiscard]] inline std::size_t prune_session_history(const SessionHistoryConfig& config) {
    fs::path path = config.storage_path.empty()
        ? default_history_path()
        : fs::path(config.storage_path);

    auto entries = load_session_history(path.string());
    if (entries.size() <= config.max_entries) return 0;

    std::size_t pruned = entries.size() - config.max_entries;

    // Keep the most recent entries (NDJSON is append-only, so later = newer)
    entries.erase(entries.begin(),
        entries.begin() + static_cast<std::ptrdiff_t>(pruned));

    // Rewrite the file with remaining entries
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return 0;

    for (const auto& entry : entries) {
        file << serialize_entry(entry) << '\n';
    }

    return pruned;
}

/// Delete a specific session from history by ID
/// Returns true if found and removed
[[nodiscard]] inline bool delete_session(
    std::string_view session_id,
    std::string_view storage_path = "") {

    fs::path path = storage_path.empty()
        ? default_history_path()
        : fs::path(storage_path);

    auto entries = load_session_history(path.string());
    auto original_size = entries.size();

    std::erase_if(entries, [&](const SessionHistoryEntry& e) {
        return e.session_id == session_id;
    });

    if (entries.size() == original_size) return false;

    // Rewrite
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;

    for (const auto& entry : entries) {
        file << serialize_entry(entry) << '\n';
    }
    return true;
}

} // namespace cc::services::assistant
