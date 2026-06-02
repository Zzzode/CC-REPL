module;
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.session_restore;

export namespace cc::utils {

namespace fs = std::filesystem;

// Simplified message structure for session restore
struct Message {
    std::string role;   // "user", "assistant"
    std::string content;
};

struct SessionData {
    std::string id;
    std::vector<Message> messages;
    std::map<std::string, std::string> metadata;
};

namespace detail {
    inline fs::path get_sessions_base_dir() {
        const char* home = std::getenv("HOME");
        if (!home) return fs::temp_directory_path() / "claude-code" / "sessions";
        return fs::path(home) / ".claude" / "sessions";
    }
} // namespace detail

// Check if a session can be restored
bool can_restore_session(std::string_view session_id) {
    auto dir = detail::get_sessions_base_dir() / std::string(session_id);
    // Session must have directory and messages file
    return fs::exists(dir) && fs::exists(dir / "messages.jsonl");
}

// Restore a session from disk
std::expected<SessionData, std::string> restore_session(std::string_view session_id) {
    auto dir = detail::get_sessions_base_dir() / std::string(session_id);

    if (!fs::exists(dir)) {
        return std::unexpected("Session directory not found: " + std::string(session_id));
    }

    SessionData data;
    data.id = std::string(session_id);

    // Load messages (JSONL format - simplified parser)
    auto messages_path = dir / "messages.jsonl";
    if (fs::exists(messages_path)) {
        std::ifstream file(messages_path);
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            // Simple extraction of role and content from JSON line
            Message msg;
            auto role_pos = line.find("\"role\":");
            if (role_pos != std::string::npos) {
                auto val_start = line.find('"', role_pos + 7);
                auto val_end = line.find('"', val_start + 1);
                if (val_start != std::string::npos && val_end != std::string::npos) {
                    msg.role = line.substr(val_start + 1, val_end - val_start - 1);
                }
            }

            auto content_pos = line.find("\"content\":");
            if (content_pos != std::string::npos) {
                auto val_start = line.find('"', content_pos + 10);
                auto val_end = line.rfind('"');
                if (val_start != std::string::npos && val_end > val_start) {
                    msg.content = line.substr(val_start + 1, val_end - val_start - 1);
                }
            }

            if (!msg.role.empty()) {
                data.messages.push_back(std::move(msg));
            }
        }
    }

    // Load metadata
    auto meta_path = dir / "metadata.json";
    if (fs::exists(meta_path)) {
        std::ifstream file(meta_path);
        std::string line;
        while (std::getline(file, line)) {
            auto key_start = line.find('"');
            if (key_start == std::string::npos) continue;
            auto key_end = line.find('"', key_start + 1);
            if (key_end == std::string::npos) continue;
            auto colon = line.find(':', key_end);
            if (colon == std::string::npos) continue;
            auto val_start = line.find('"', colon);
            if (val_start == std::string::npos) continue;
            auto val_end = line.find('"', val_start + 1);
            if (val_end == std::string::npos) continue;

            std::string key = line.substr(key_start + 1, key_end - key_start - 1);
            std::string value = line.substr(val_start + 1, val_end - val_start - 1);
            data.metadata[key] = value;
        }
    }

    return data;
}

} // namespace cc::utils
