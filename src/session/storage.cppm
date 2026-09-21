/// @file storage.cppm
/// @brief Session storage and persistence.
/// Migrated from session.cppm supplement - session persistence logic
module;

#include <string>
#include <vector>
#include <filesystem>
#include <optional>
#include <chrono>
#include <fstream>
#include <algorithm>

#include <yyjson.h>

export module cc.session.storage;

import cc.utils.json;

export namespace cc::session {

[[nodiscard]] inline std::chrono::system_clock::time_point file_time_to_system_time(
    std::filesystem::file_time_type file_time
) {
    const auto now_file = std::filesystem::file_time_type::clock::now();
    const auto now_system = std::chrono::system_clock::now();
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - now_file + now_system
    );
}

/// Session metadata stored on disk
struct SessionMetadata {
    std::string session_id;
    std::string model;
    std::filesystem::path cwd;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_active;
    int message_count = 0;
    std::optional<std::string> title;
    bool is_archived = false;
};

/// List recent sessions from disk
[[nodiscard]] inline std::vector<SessionMetadata> list_recent_sessions(
    const std::filesystem::path& sessions_dir,
    std::size_t limit = 20
) {
    std::vector<SessionMetadata> sessions;
    
    if (!std::filesystem::exists(sessions_dir)) return sessions;
    
    for (const auto& entry : std::filesystem::directory_iterator(sessions_dir)) {
        if (!entry.is_directory()) continue;
        
        auto metadata_path = entry.path() / "metadata.json";
        if (!std::filesystem::exists(metadata_path)) continue;
        
        SessionMetadata meta;
        meta.session_id = entry.path().filename().string();
        meta.last_active = file_time_to_system_time(std::filesystem::last_write_time(metadata_path));

        // Parse JSON metadata
        auto doc_result = cc::utils::json::parse_file(metadata_path);
        if (doc_result) {
            auto root = doc_result->root();
            if (root.valid() && root.is_obj()) {
                auto model_val = root.get("model");
                if (model_val.is_str()) meta.model = std::string(model_val.as_str());

                auto cwd_val = root.get("cwd");
                if (cwd_val.is_str()) meta.cwd = std::string(cwd_val.as_str());

                auto title_val = root.get("title");
                if (title_val.is_str()) meta.title = std::string(title_val.as_str());

                auto count_val = root.get("message_count");
                if (count_val.is_num()) meta.message_count = static_cast<int>(count_val.as_int());

                auto archived_val = root.get("is_archived");
                if (archived_val.is_bool()) meta.is_archived = archived_val.as_bool();

                auto created_val = root.get("created_at");
                if (created_val.is_num()) {
                    meta.created_at = std::chrono::system_clock::time_point(
                        std::chrono::seconds(created_val.as_int()));
                }

                auto active_val = root.get("last_active");
                if (active_val.is_num()) {
                    meta.last_active = std::chrono::system_clock::time_point(
                        std::chrono::seconds(active_val.as_int()));
                }
            }
        }

        sessions.push_back(std::move(meta));
    }
    
    // Sort by last_active descending
    std::sort(sessions.begin(), sessions.end(),
        [](const auto& a, const auto& b) { return a.last_active > b.last_active; });
    
    if (sessions.size() > limit) {
        sessions.resize(limit);
    }
    
    return sessions;
}

/// Get session directory path
[[nodiscard]] inline std::filesystem::path get_session_dir(
    const std::filesystem::path& sessions_dir,
    std::string_view session_id
) {
    return sessions_dir / session_id;
}

/// Get the messages file path for a session
[[nodiscard]] inline std::filesystem::path get_messages_path(
    const std::filesystem::path& sessions_dir,
    std::string_view session_id
) {
    return get_session_dir(sessions_dir, session_id) / "messages.jsonl";
}

/// Load session metadata from a single session directory
[[nodiscard]] inline std::optional<SessionMetadata> load_session_metadata(
    const std::filesystem::path& sessions_dir,
    std::string_view session_id
) {
    auto dir = get_session_dir(sessions_dir, session_id);
    auto metadata_path = dir / "metadata.json";
    if (!std::filesystem::exists(metadata_path)) return std::nullopt;

    auto doc_result = cc::utils::json::parse_file(metadata_path);
    if (!doc_result) return std::nullopt;

    auto root = doc_result->root();
    if (!root.valid() || !root.is_obj()) return std::nullopt;

    SessionMetadata meta;
    meta.session_id = std::string(session_id);

    auto model_val = root.get("model");
    if (model_val.is_str()) meta.model = std::string(model_val.as_str());

    auto cwd_val = root.get("cwd");
    if (cwd_val.is_str()) meta.cwd = std::string(cwd_val.as_str());

    auto title_val = root.get("title");
    if (title_val.is_str()) meta.title = std::string(title_val.as_str());

    auto count_val = root.get("message_count");
    if (count_val.is_num()) meta.message_count = static_cast<int>(count_val.as_int());

    auto archived_val = root.get("is_archived");
    if (archived_val.is_bool()) meta.is_archived = archived_val.as_bool();

    auto created_val = root.get("created_at");
    if (created_val.is_num()) {
        meta.created_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(created_val.as_int()));
    }

    auto active_val = root.get("last_active");
    if (active_val.is_num()) {
        meta.last_active = std::chrono::system_clock::time_point(
            std::chrono::seconds(active_val.as_int()));
    }

    return meta;
}

/// Save session metadata to disk as JSON
inline bool save_session_metadata(
    const std::filesystem::path& sessions_dir,
    const SessionMetadata& meta
) {
    auto dir = get_session_dir(sessions_dir, meta.session_id);
    std::filesystem::create_directories(dir);

    cc::utils::json::JsonMutDoc doc;
    auto root = doc.object();

    root.add("session_id", doc.string(meta.session_id));
    root.add("model", doc.string(meta.model));
    root.add("cwd", doc.string(meta.cwd.string()));
    root.add("message_count", doc.number(static_cast<int64_t>(meta.message_count)));
    root.add("is_archived", doc.boolean(meta.is_archived));

    auto created_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        meta.created_at.time_since_epoch()).count();
    root.add("created_at", doc.number(static_cast<int64_t>(created_epoch)));

    auto active_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        meta.last_active.time_since_epoch()).count();
    root.add("last_active", doc.number(static_cast<int64_t>(active_epoch)));

    if (meta.title) {
        root.add("title", doc.string(*meta.title));
    }

    doc.set_root(root);

    auto metadata_path = dir / "metadata.json";
    std::ofstream ofs(metadata_path, std::ios::trunc);
    if (!ofs.is_open()) return false;
    ofs << doc.to_pretty_string();
    return ofs.good();
}

/// Append a single message (JSON line) to the session's messages file
inline bool append_message(
    const std::filesystem::path& sessions_dir,
    std::string_view session_id,
    std::string_view message_json
) {
    auto path = get_messages_path(sessions_dir, session_id);
    std::filesystem::create_directories(path.parent_path());

    std::ofstream ofs(path, std::ios::app);
    if (!ofs.is_open()) return false;
    ofs << message_json;
    if (!message_json.empty() && message_json.back() != '\n') {
        ofs << '\n';
    }
    return ofs.good();
}

/// Load all messages from a session's JSONL file
[[nodiscard]] inline std::vector<cc::utils::json::JsonDoc> load_messages(
    const std::filesystem::path& sessions_dir,
    std::string_view session_id
) {
    std::vector<cc::utils::json::JsonDoc> messages;
    auto path = get_messages_path(sessions_dir, session_id);

    if (!std::filesystem::exists(path)) return messages;

    std::ifstream ifs(path);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        auto doc_result = cc::utils::json::parse(line);
        if (doc_result) {
            messages.push_back(std::move(*doc_result));
        }
    }
    return messages;
}

} // namespace cc::session
