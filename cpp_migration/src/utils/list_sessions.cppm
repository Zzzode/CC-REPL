module;
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.list_sessions;

export namespace cc::utils {

namespace fs = std::filesystem;

using TimePoint = std::chrono::system_clock::time_point;

struct SessionSummary {
    std::string id;
    std::string name;
    TimePoint created;
    TimePoint last_active;
    std::size_t message_count;
    std::string model;
};

namespace detail {
    inline fs::path get_sessions_dir() {
        const char* home = std::getenv("HOME");
        if (!home) return fs::temp_directory_path() / "claude-code" / "sessions";
        return fs::path(home) / ".claude" / "sessions";
    }

    inline std::optional<SessionSummary> load_session_summary(const fs::path& dir) {
        SessionSummary summary;
        summary.id = dir.filename().string();
        summary.name = summary.id; // Default name is the ID
        summary.message_count = 0;

        // Read metadata
        auto meta_path = dir / "metadata.json";
        if (fs::exists(meta_path)) {
            std::ifstream f(meta_path);
            std::string line;
            while (std::getline(f, line)) {
                // Extract model
                auto pos = line.find("\"model\"");
                if (pos != std::string::npos) {
                    auto val_start = line.find('"', pos + 8);
                    auto val_end = line.find('"', val_start + 1);
                    if (val_start != std::string::npos && val_end != std::string::npos) {
                        summary.model = line.substr(val_start + 1, val_end - val_start - 1);
                    }
                }
            }
        }

        // Count messages
        auto msg_path = dir / "messages.jsonl";
        if (fs::exists(msg_path)) {
            std::ifstream f(msg_path);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) ++summary.message_count;
            }
        }

        // Get timestamps from filesystem
        std::error_code ec;
        auto write_time = fs::last_write_time(dir, ec);
        if (!ec) {
            auto d = write_time.time_since_epoch();
            auto sctp = std::chrono::system_clock::time_point{
                std::chrono::duration_cast<std::chrono::system_clock::duration>(d)
            };
            summary.last_active = sctp;
            summary.created = sctp; // Approximation
        } else {
            summary.last_active = std::chrono::system_clock::now();
            summary.created = summary.last_active;
        }

        return summary;
    }
} // namespace detail

// List all sessions, optionally limited
std::vector<SessionSummary> list_sessions(std::optional<std::size_t> limit) {
    std::vector<SessionSummary> sessions;
    auto base = detail::get_sessions_dir();

    if (!fs::exists(base)) return sessions;

    for (auto& entry : fs::directory_iterator(base)) {
        if (!entry.is_directory()) continue;

        if (auto summary = detail::load_session_summary(entry.path())) {
            sessions.push_back(std::move(*summary));
        }
    }

    // Sort by last_active descending
    std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
        return a.last_active > b.last_active;
    });

    // Apply limit
    if (limit && sessions.size() > *limit) {
        sessions.resize(*limit);
    }

    return sessions;
}

// Search sessions by query (matches id or name)
std::vector<SessionSummary> search_sessions(std::string_view query) {
    auto all = list_sessions(std::nullopt);
    std::vector<SessionSummary> results;

    for (auto& s : all) {
        if (s.id.find(query) != std::string::npos ||
            s.name.find(query) != std::string::npos ||
            s.model.find(query) != std::string::npos) {
            results.push_back(std::move(s));
        }
    }

    return results;
}

// Get total number of sessions
std::size_t get_session_count() {
    auto base = detail::get_sessions_dir();
    if (!fs::exists(base)) return 0;

    std::size_t count = 0;
    for (auto& entry : fs::directory_iterator(base)) {
        if (entry.is_directory()) ++count;
    }
    return count;
}

} // namespace cc::utils
