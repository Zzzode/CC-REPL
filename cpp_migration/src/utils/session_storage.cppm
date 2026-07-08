/// @file session_storage.cppm
/// @brief Session storage module - persistent conversation history storage.
/// Manages saving, loading, listing, and deleting conversations using C++23 features.
module;

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <expected>
#include <filesystem>
#include <chrono>
#include <format>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <variant>

export module cc.utils.session_storage;

import cc.utils.json;
import cc.utils.error;
import cc.utils.crypto;

export namespace cc::utils {

namespace fs = std::filesystem;
using cc::utils::Result;
using cc::utils::VoidResult;
using cc::utils::Error;
using cc::utils::ErrorCode;

// Lightweight conversation message types used by the storage layer.
struct TextBlock {
    std::string text;
};

using ContentBlock = std::variant<TextBlock>;

struct UserMessage {
    std::vector<ContentBlock> content;
};

struct AssistantMessage {
    std::vector<ContentBlock> content;
};

using Message = std::variant<UserMessage, AssistantMessage>;

// ============================================================
// Session metadata types
// ============================================================

/// Metadata for a stored session
struct SessionMetadata {
    std::string id;
    std::string title;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    std::uint32_t message_count = 0;
    std::optional<std::string> model_used;
    std::optional<double> total_cost_usd;
};

/// Summary for session listing
struct SessionSummary {
    SessionMetadata metadata;
    std::optional<std::string> preview;  // First few messages
};

// ============================================================
// Session Storage Manager
// ============================================================

/// Manages persistent storage of conversation sessions
class SessionStorage {
private:
    fs::path storage_dir_;
    mutable std::shared_mutex mutex_;

public:
    /// Construct with storage directory (default: ~/.cc-repl/sessions)
    explicit SessionStorage(fs::path storage_dir = default_storage_dir())
        : storage_dir_(std::move(storage_dir)) {
        ensure_storage_dir_exists();
    }

    // Non-copyable, movable
    SessionStorage(const SessionStorage&) = delete;
    SessionStorage& operator=(const SessionStorage&) = delete;
    SessionStorage(SessionStorage&&) = delete;
    SessionStorage& operator=(SessionStorage&&) = delete;

    /// Save a conversation to storage
    [[nodiscard]] Result<std::string> save_session(
        std::string_view session_id,
        std::string_view title,
        const std::vector<Message>& conversation,
        std::optional<std::string_view> model = std::nullopt,
        std::optional<double> cost = std::nullopt
    ) {
        std::unique_lock lock(mutex_);

        auto session_dir = get_session_dir(session_id);
        fs::create_directories(session_dir);

        // Save messages
        auto messages_path = session_dir / "messages.json";
        if (auto result = save_messages(messages_path, conversation); !result) {
            return std::unexpected(result.error());
        }

        // Update or create metadata
        auto metadata = load_metadata(session_id);
        if (!metadata) {
            metadata = SessionMetadata{};
            metadata->id = std::string(session_id);
            metadata->created_at = std::chrono::system_clock::now();
        }
        metadata->title = std::string(title);
        metadata->updated_at = std::chrono::system_clock::now();
        metadata->message_count = static_cast<std::uint32_t>(conversation.size());
        if (model) metadata->model_used = std::string(*model);
        if (cost) metadata->total_cost_usd = *cost;

        if (auto result = save_metadata(session_dir, *metadata); !result) {
            return std::unexpected(result.error());
        }

        return std::string(session_id);
    }

    /// Load a session from storage
    [[nodiscard]] Result<std::vector<Message>> load_session(std::string_view session_id) {
        std::shared_lock lock(mutex_);

        auto session_dir = get_session_dir(session_id);
        auto messages_path = session_dir / "messages.json";

        if (!fs::exists(messages_path)) {
            return std::unexpected(Error(
                ErrorCode::not_found,
                std::format("Session not found: {}", session_id)
            ));
        }

        return load_messages(messages_path);
    }

    /// Load session metadata
    [[nodiscard]] Result<SessionMetadata> load_session_metadata(std::string_view session_id) {
        std::shared_lock lock(mutex_);

        auto metadata = load_metadata(session_id);
        if (!metadata) {
            return std::unexpected(Error(
                ErrorCode::not_found,
                std::format("Session not found: {}", session_id)
            ));
        }
        return *metadata;
    }

    /// List all sessions with summaries
    [[nodiscard]] Result<std::vector<SessionSummary>> list_sessions(std::size_t limit = 100) {
        std::shared_lock lock(mutex_);

        std::vector<SessionSummary> sessions;

        if (!fs::exists(storage_dir_)) {
            return sessions;
        }

        // Collect all session directories
        for (const auto& entry : fs::directory_iterator(storage_dir_)) {
            if (!entry.is_directory()) continue;

            auto metadata = load_metadata(entry.path().filename().string());
            if (!metadata) continue;

            SessionSummary summary;
            summary.metadata = *metadata;

            // Load preview (first few messages)
            auto messages_path = entry.path() / "messages.json";
            if (fs::exists(messages_path)) {
                if (auto msgs = load_messages_preview(messages_path, 3)) {
                    std::string preview_text;
                    for (const auto& msg : *msgs) {
                        std::visit([&preview_text](const auto& m) {
                            for (const auto& block : m.content) {
                                if (const auto* text = std::get_if<TextBlock>(&block)) {
                                    if (!preview_text.empty()) preview_text += "\n";
                                    // Truncate for preview
                                    preview_text += text->text.substr(0, 100);
                                    if (text->text.size() > 100) preview_text += "...";
                                }
                            }
                        }, msg);
                    }
                    summary.preview = std::move(preview_text);
                }
            }

            sessions.push_back(std::move(summary));
        }

        // Sort by updated time descending
        std::sort(sessions.begin(), sessions.end(),
            [](const SessionSummary& a, const SessionSummary& b) {
                return a.metadata.updated_at > b.metadata.updated_at;
            });

        // Apply limit
        if (sessions.size() > limit) {
            sessions.resize(limit);
        }

        return sessions;
    }

    /// Delete a session
    [[nodiscard]] VoidResult delete_session(std::string_view session_id) {
        std::unique_lock lock(mutex_);

        auto session_dir = get_session_dir(session_id);
        if (!fs::exists(session_dir)) {
            return std::unexpected(Error(
                ErrorCode::not_found,
                std::format("Session not found: {}", session_id)
            ));
        }

        try {
            fs::remove_all(session_dir);
            return {};
        } catch (const fs::filesystem_error& e) {
            return std::unexpected(Error(
                ErrorCode::internal_error,
                std::format("Failed to delete session: {}", e.what())
            ));
        }
    }

    /// Rename a session
    [[nodiscard]] VoidResult rename_session(std::string_view session_id, std::string_view new_title) {
        std::unique_lock lock(mutex_);

        auto metadata = load_metadata(session_id);
        if (!metadata) {
            return std::unexpected(Error(
                ErrorCode::not_found,
                std::format("Session not found: {}", session_id)
            ));
        }

        metadata->title = std::string(new_title);
        metadata->updated_at = std::chrono::system_clock::now();

        auto session_dir = get_session_dir(session_id);
        return save_metadata(session_dir, *metadata);
    }

    /// Delete all sessions
    [[nodiscard]] VoidResult clear_all_sessions() {
        std::unique_lock lock(mutex_);

        if (!fs::exists(storage_dir_)) {
            return {};
        }

        try {
            for (const auto& entry : fs::directory_iterator(storage_dir_)) {
                if (entry.is_directory()) {
                    fs::remove_all(entry.path());
                }
            }
            return {};
        } catch (const fs::filesystem_error& e) {
            return std::unexpected(Error(
                ErrorCode::internal_error,
                std::format("Failed to clear sessions: {}", e.what())
            ));
        }
    }

    /// Check if a session exists
    [[nodiscard]] bool session_exists(std::string_view session_id) const {
        std::shared_lock lock(mutex_);
        return fs::exists(get_session_dir(session_id));
    }

    /// Get storage directory path
    [[nodiscard]] const fs::path& storage_dir() const noexcept { return storage_dir_; }

    /// Generate a new unique session ID
    [[nodiscard]] static std::string generate_session_id() {
        // TS parity: randomUUID() (v4 UUID, e.g. "a1b2c3d4-e5f6-...").
        // User statusline scripts take the first 6 hex chars as a #hashtag.
        return cc::utils::crypto::generate_uuid();
    }

private:
    /// Get default storage directory (~/.cc-repl/sessions)
    [[nodiscard]] static fs::path default_storage_dir() {
        auto home = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp");
        return home / ".cc-repl" / "sessions";
    }

    /// Ensure storage directory exists
    void ensure_storage_dir_exists() {
        if (!fs::exists(storage_dir_)) {
            fs::create_directories(storage_dir_);
        }
    }

    /// Get session directory path
    [[nodiscard]] fs::path get_session_dir(std::string_view session_id) const {
        return storage_dir_ / std::string(session_id);
    }

    /// Load metadata from a session directory
    [[nodiscard]] std::optional<SessionMetadata> load_metadata(std::string_view session_id) const {
        auto meta_path = get_session_dir(session_id) / "metadata.json";
        if (!fs::exists(meta_path)) {
            return std::nullopt;
        }

        try {
            std::ifstream file(meta_path);
            std::string json_str((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());

            return parse_metadata_from_json(json_str);
        } catch (...) {
            return std::nullopt;
        }
    }

    /// Save metadata to a session directory
    [[nodiscard]] static VoidResult save_metadata(const fs::path& session_dir,
                                                   const SessionMetadata& meta) {
        auto meta_path = session_dir / "metadata.json";
        auto json_str = metadata_to_json(meta);

        try {
            std::ofstream file(meta_path);
            file << json_str;
            return {};
        } catch (const std::exception& e) {
            return std::unexpected(Error(
                ErrorCode::internal_error,
                std::format("Failed to save metadata: {}", e.what())
            ));
        }
    }

    /// Save messages to file
    [[nodiscard]] static VoidResult save_messages(const fs::path& path,
                                                    const std::vector<Message>& messages) {
        auto json_str = messages_to_json(messages);

        try {
            std::ofstream file(path);
            file << json_str;
            return {};
        } catch (const std::exception& e) {
            return std::unexpected(Error(
                ErrorCode::internal_error,
                std::format("Failed to save messages: {}", e.what())
            ));
        }
    }

    /// Load messages from file
    [[nodiscard]] static Result<std::vector<Message>> load_messages(const fs::path& path) {
        try {
            std::ifstream file(path);
            std::string json_str((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());

            auto messages = parse_messages_from_json(json_str);
            if (!messages) {
                return std::unexpected(Error(
                    ErrorCode::internal_error,
                    "Failed to parse messages"
                ));
            }
            return *messages;
        } catch (const std::exception& e) {
            return std::unexpected(Error(
                ErrorCode::internal_error,
                std::format("Failed to load messages: {}", e.what())
            ));
        }
    }

    /// Load preview of messages (first N)
    [[nodiscard]] static Result<std::vector<Message>> load_messages_preview(
        const fs::path& path,
        std::size_t limit
    ) {
        auto result = load_messages(path);
        if (!result) return result;

        if (result->size() <= limit) {
            return *result;
        }

        return std::vector<Message>(result->begin(), result->begin() + static_cast<std::ptrdiff_t>(limit));
    }

    // ============================================================
    // JSON serialization helpers
    // ============================================================

    [[nodiscard]] static std::string escape_json(std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (char ch : value) {
            if (ch == '\\') out += "\\\\";
            else if (ch == '"') out += "\\\"";
            else if (ch == '\n') out += "\\n";
            else out += ch;
        }
        return out;
    }

    [[nodiscard]] static std::string messages_to_json(const std::vector<Message>& messages) {
        std::string json = "[";
        for (std::size_t i = 0; i < messages.size(); ++i) {
            if (i > 0) json += ",";
            json += std::visit([](const auto& msg) {
                std::string role = std::is_same_v<std::decay_t<decltype(msg)>, UserMessage> ? "user" : "assistant";
                std::string text;
                for (const auto& block : msg.content) {
                    if (const auto* t = std::get_if<TextBlock>(&block)) text += t->text;
                }
                return std::format(R"({{"role":"{}","text":"{}"}})", role, escape_json(text));
            }, messages[i]);
        }
        json += "]";
        return json;
    }

    [[nodiscard]] static std::optional<std::vector<Message>> parse_messages_from_json(
        std::string_view json
    ) {
        std::vector<Message> messages;
        std::size_t pos = 0;
        while ((pos = json.find(R"("role":")", pos)) != std::string_view::npos) {
            pos += std::string_view(R"("role":")").size();
            auto role_end = json.find('"', pos);
            if (role_end == std::string_view::npos) break;
            auto role = json.substr(pos, role_end - pos);
            auto text_key = json.find(R"("text":")", role_end);
            if (text_key == std::string_view::npos) break;
            text_key += std::string_view(R"("text":")").size();
            auto text_end = json.find('"', text_key);
            if (text_end == std::string_view::npos) break;
            TextBlock block{std::string(json.substr(text_key, text_end - text_key))};
            if (role == "assistant") messages.push_back(AssistantMessage{{block}});
            else messages.push_back(UserMessage{{block}});
            pos = text_end + 1;
        }
        return messages;
    }

    [[nodiscard]] static std::string metadata_to_json(const SessionMetadata& meta) {
        return std::format(R"({{"id":"{}","title":"{}","message_count":{}}})",
            meta.id, meta.title, meta.message_count);
    }

    [[nodiscard]] static std::optional<SessionMetadata> parse_metadata_from_json(
        std::string_view json
    ) {
        auto extract_string = [&](std::string_view key) {
            auto marker = std::string("\"") + std::string(key) + "\":\"";
            auto pos = json.find(marker);
            if (pos == std::string_view::npos) return std::string{};
            pos += marker.size();
            auto end = json.find('"', pos);
            return end == std::string_view::npos ? std::string{} : std::string(json.substr(pos, end - pos));
        };
        auto extract_uint = [&](std::string_view key) -> std::uint32_t {
            auto marker = std::string("\"") + std::string(key) + "\":";
            auto pos = json.find(marker);
            if (pos == std::string_view::npos) return 0;
            pos += marker.size();
            auto end = json.find_first_not_of("0123456789", pos);
            auto digits = json.substr(pos, end == std::string_view::npos ? json.size() - pos : end - pos);
            return digits.empty() ? 0 : static_cast<std::uint32_t>(std::stoul(std::string(digits)));
        };
        auto now = std::chrono::system_clock::now();
        return SessionMetadata{
            .id = extract_string("id"),
            .title = extract_string("title"),
            .created_at = now,
            .updated_at = now,
            .message_count = extract_uint("message_count"),
            .model_used = std::nullopt,
            .total_cost_usd = std::nullopt,
        };
    }
};

} // namespace cc::utils
