/// @file session.cppm
/// @brief Session management module for conversation lifecycle.
/// Handles session creation, persistence, resume, and history tracking.
module;

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <expected>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <algorithm>
#include <ranges>
#include <random>
#include <mutex>

export module cc.session.session;

import cc.types.types;
import cc.config.config;
import cc.utils.json;

export namespace cc::core {

// ============================================================
// Session state and metadata
// ============================================================

/// Current lifecycle state of a session
enum class SessionState : std::uint8_t {
    Created,    // Freshly created, no messages yet
    Active,     // In-progress conversation
    Paused,     // Suspended, can be resumed
    Completed,  // Finished normally (user exited)
    Expired,    // Exceeded TTL without activity
    Error,      // Terminated due to unrecoverable error
};

/// Convert session state to display string
[[nodiscard]] constexpr std::string_view session_state_to_string(SessionState state) noexcept {
    switch (state) {
        case SessionState::Created:   return "created";
        case SessionState::Active:    return "active";
        case SessionState::Paused:    return "paused";
        case SessionState::Completed: return "completed";
        case SessionState::Expired:   return "expired";
        case SessionState::Error:     return "error";
    }
    return "unknown";
}

/// Metadata about a session (stored alongside session data)
struct SessionMetadata {
    SessionId id;
    std::string title;                               // Auto-generated or user-set title
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
    SessionState state = SessionState::Created;
    std::uint32_t message_count = 0;                 // Total messages in conversation
    TokenUsage total_usage;                          // Cumulative token usage
    std::optional<std::string> working_directory;    // Project root for this session
    std::optional<std::string> model;                // Model used in this session
    std::optional<std::string> error_message;        // Error details if state == Error
};

/// Compact session summary for listing (no message content)
struct SessionSummary {
    SessionId id;
    std::string title;
    SessionState state;
    std::chrono::system_clock::time_point updated_at;
    std::uint32_t message_count;

    /// Format as a one-line display string
    [[nodiscard]] std::string format_line() const {
        return std::format("[{}] {} ({}, {} msgs)",
            id.str().substr(0, 8),
            title,
            session_state_to_string(state),
            message_count
        );
    }
};

// ============================================================
// Session class - manages a single conversation lifecycle
// ============================================================

/// Represents a single interactive session with full state management.
/// Owns the conversation history and provides persistence operations.
class Session {
    SessionMetadata metadata_;
    std::vector<Message> messages_;          // Complete conversation history
    mutable std::mutex mutex_;               // Thread safety for concurrent access

public:
    /// Create a new session with a generated ID
    Session()
        : metadata_{
            .id = SessionId{generate_session_id()},
            .title = "New Session",
            .created_at = std::chrono::system_clock::now(),
            .updated_at = std::chrono::system_clock::now(),
            .state = SessionState::Created,
        } {}

    /// Create a session with explicit metadata (for deserialization)
    explicit Session(SessionMetadata metadata, std::vector<Message> messages = {})
        : metadata_(std::move(metadata))
        , messages_(std::move(messages)) {}

    // Non-copyable, movable (mutex is recreated for the destination object)
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&& other) noexcept {
        std::lock_guard lock(other.mutex_);
        metadata_ = std::move(other.metadata_);
        messages_ = std::move(other.messages_);
    }
    Session& operator=(Session&& other) noexcept {
        if (this == &other) return *this;
        std::scoped_lock lock(mutex_, other.mutex_);
        metadata_ = std::move(other.metadata_);
        messages_ = std::move(other.messages_);
        return *this;
    }

    // --------------------------------------------------------
    // Message management
    // --------------------------------------------------------

    /// Append a message to the conversation
    void add_message(Message msg) {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(msg));
        metadata_.message_count = static_cast<std::uint32_t>(messages_.size());
        metadata_.updated_at = std::chrono::system_clock::now();

        // Transition to Active on first message
        if (metadata_.state == SessionState::Created) {
            metadata_.state = SessionState::Active;
        }
    }

    /// Get all messages (thread-safe copy)
    [[nodiscard]] std::vector<Message> messages() const {
        std::lock_guard lock(mutex_);
        return messages_;
    }

    /// Get message count
    [[nodiscard]] std::uint32_t message_count() const noexcept {
        return metadata_.message_count;
    }

    /// Get the last N messages
    [[nodiscard]] std::vector<Message> recent_messages(std::size_t count) const {
        std::lock_guard lock(mutex_);
        if (messages_.size() <= count) return messages_;
        return std::vector<Message>(messages_.end() - static_cast<std::ptrdiff_t>(count),
                                     messages_.end());
    }

    // --------------------------------------------------------
    // State management
    // --------------------------------------------------------

    /// Get session metadata
    [[nodiscard]] const SessionMetadata& metadata() const noexcept { return metadata_; }

    /// Get session ID
    [[nodiscard]] const SessionId& id() const noexcept { return metadata_.id; }

    /// Get current state
    [[nodiscard]] SessionState state() const noexcept { return metadata_.state; }

    /// Set session title (auto-generated from first message or user-specified)
    void set_title(std::string title) {
        metadata_.title = std::move(title);
        metadata_.updated_at = std::chrono::system_clock::now();
    }

    /// Update token usage
    void add_usage(const TokenUsage& usage) {
        metadata_.total_usage += usage;
        metadata_.updated_at = std::chrono::system_clock::now();
    }

    /// Pause the session (can be resumed later)
    void pause() {
        if (metadata_.state == SessionState::Active) {
            metadata_.state = SessionState::Paused;
            metadata_.updated_at = std::chrono::system_clock::now();
        }
    }

    /// Resume a paused session
    [[nodiscard]] VoidResult resume() {
        if (metadata_.state != SessionState::Paused) {
            return std::unexpected(Error::make(
                ErrorCode::SessionNotFound,
                std::format("Cannot resume session in state: {}",
                           session_state_to_string(metadata_.state))
            ));
        }
        metadata_.state = SessionState::Active;
        metadata_.updated_at = std::chrono::system_clock::now();
        return {};
    }

    /// Mark session as completed (normal termination)
    void complete() {
        metadata_.state = SessionState::Completed;
        metadata_.updated_at = std::chrono::system_clock::now();
    }

    /// Mark session as errored
    void set_error(std::string error_message) {
        metadata_.state = SessionState::Error;
        metadata_.error_message = std::move(error_message);
        metadata_.updated_at = std::chrono::system_clock::now();
    }

    /// Auto-generate title from the first user message content
    void auto_title() {
        std::lock_guard lock(mutex_);
        for (const auto& msg : messages_) {
            if (auto* user_msg = std::get_if<UserMessage>(&msg)) {
                for (const auto& block : user_msg->content) {
                    if (auto* text = std::get_if<TextBlock>(&block)) {
                        // Take first 50 characters as title
                        auto title = text->text.substr(0, 50);
                        if (text->text.size() > 50) title += "...";
                        metadata_.title = std::move(title);
                        return;
                    }
                }
            }
        }
    }

private:
    /// Generate a unique session ID
    [[nodiscard]] static std::string generate_session_id() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<std::uint64_t> dist;
        return std::format("sess_{:016x}", dist(rng));
    }
};

// ============================================================
// Session Storage - handles persistence to disk
// ============================================================

/// Manages reading and writing sessions to the filesystem.
/// Sessions are stored as JSON files in a configured directory.
class SessionStorage {
    std::filesystem::path storage_dir_;

public:
    /// Initialize with the session storage directory
    explicit SessionStorage(std::filesystem::path dir)
        : storage_dir_(std::move(dir)) {}

    /// Default storage location (~/.config/claude/sessions/)
    SessionStorage()
        : storage_dir_(default_storage_path()) {}

    /// Save a session to disk
    [[nodiscard]] VoidResult save(const Session& session) {
        // Ensure storage directory exists
        std::error_code ec;
        std::filesystem::create_directories(storage_dir_, ec);
        if (ec) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to create session directory: {}", storage_dir_.string())
            ));
        }

        auto file_path = session_file_path(session.id());
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Cannot write session file: {}", file_path.string())
            ));
        }

        file << serialize_session(session);
        return {};
    }

    /// Load a session from disk by ID
    [[nodiscard]] Result<Session> load(const SessionId& id) {
        auto file_path = session_file_path(id);
        if (!std::filesystem::exists(file_path)) {
            return std::unexpected(Error::make(
                ErrorCode::SessionNotFound,
                std::format("Session not found: {}", id.str())
            ));
        }

        std::ifstream file(file_path);
        if (!file.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::SessionCorrupted,
                std::format("Cannot read session file: {}", file_path.string())
            ));
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        return deserialize_session(content);
    }

    /// Delete a session from disk
    [[nodiscard]] VoidResult remove(const SessionId& id) {
        auto file_path = session_file_path(id);
        std::error_code ec;
        std::filesystem::remove(file_path, ec);
        if (ec) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Failed to delete session: {}", id.str())
            ));
        }
        return {};
    }

    /// List all stored session summaries, sorted by last update (most recent first)
    [[nodiscard]] Result<std::vector<SessionSummary>> list_sessions() {
        std::vector<SessionSummary> summaries;

        if (!std::filesystem::exists(storage_dir_)) {
            return summaries;  // No sessions yet
        }

        for (const auto& entry : std::filesystem::directory_iterator(storage_dir_)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            // Load just the metadata (lightweight read)
            auto session_result = load_metadata(entry.path());
            if (session_result) {
                summaries.push_back(std::move(*session_result));
            }
        }

        // Sort by update time, most recent first
        std::ranges::sort(summaries, [](const auto& a, const auto& b) {
            return a.updated_at > b.updated_at;
        });

        return summaries;
    }

    /// Get the storage directory path
    [[nodiscard]] const std::filesystem::path& storage_path() const noexcept {
        return storage_dir_;
    }

    /// Clean up expired sessions older than the given duration
    [[nodiscard]] VoidResult cleanup(std::chrono::hours max_age = std::chrono::hours{720}) {
        auto now = std::chrono::system_clock::now();
        auto cutoff = now - max_age;

        if (!std::filesystem::exists(storage_dir_)) return {};

        for (const auto& entry : std::filesystem::directory_iterator(storage_dir_)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;

            auto meta = load_metadata(entry.path());
            if (meta && meta->updated_at < cutoff) {
                std::error_code ec;
                std::filesystem::remove(entry.path(), ec);
                // Ignore individual deletion errors during cleanup
            }
        }

        return {};
    }

private:
    /// Get the file path for a session by ID
    [[nodiscard]] std::filesystem::path session_file_path(const SessionId& id) const {
        return storage_dir_ / (id.str() + ".json");
    }

    /// Serialize a session to JSON string (metadata + messages)
    [[nodiscard]] std::string serialize_session(const Session& session) const {
        const auto& meta = session.metadata();
        auto messages = session.messages();

        std::string json;
        json += "{\n";
        json += std::format("  \"id\": \"{}\",\n", meta.id.str());
        json += std::format("  \"title\": \"{}\",\n", escape_json(meta.title));
        json += std::format("  \"state\": \"{}\",\n", session_state_to_string(meta.state));
        json += std::format("  \"message_count\": {},\n", meta.message_count);
        json += std::format("  \"created_at\": \"{}\",\n", format_timepoint(meta.created_at));
        json += std::format("  \"updated_at\": \"{}\",\n", format_timepoint(meta.updated_at));

        if (meta.working_directory) {
            json += std::format("  \"working_directory\": \"{}\",\n", escape_json(*meta.working_directory));
        }
        if (meta.model) {
            json += std::format("  \"model\": \"{}\",\n", escape_json(*meta.model));
        }

        // Token usage
        json += "  \"usage\": {\n";
        json += std::format("    \"input_tokens\": {},\n", meta.total_usage.input_tokens);
        json += std::format("    \"output_tokens\": {}\n", meta.total_usage.output_tokens);
        json += "  },\n";

        // Messages array
        json += "  \"messages\": [\n";
        for (std::size_t i = 0; i < messages.size(); ++i) {
            json += "    ";
            json += serialize_message(messages[i]);
            if (i + 1 < messages.size()) json += ",";
            json += "\n";
        }
        json += "  ]\n";
        json += "}";

        return json;
    }

    /// Write session transcript in JSONL format (one JSON object per line).
    /// This format is append-friendly and matches the TS implementation.
    [[nodiscard]] VoidResult write_transcript_jsonl(const Session& session) const {
        auto jsonl_path = session_file_path(session.id());
        jsonl_path.replace_extension(".jsonl");

        std::ofstream file(jsonl_path, std::ios::trunc);
        if (!file.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Cannot write transcript file: {}", jsonl_path.string())
            ));
        }

        auto messages = session.messages();
        for (const auto& msg : messages) {
            file << serialize_message(msg) << '\n';
        }

        return {};
    }

    /// Append a single message to the JSONL transcript (for incremental writes).
    [[nodiscard]] VoidResult append_message_jsonl(const SessionId& id, const Message& msg) const {
        auto jsonl_path = session_file_path(id);
        jsonl_path.replace_extension(".jsonl");

        std::ofstream file(jsonl_path, std::ios::app);
        if (!file.is_open()) {
            return std::unexpected(Error::make(
                ErrorCode::ConfigWriteError,
                std::format("Cannot append to transcript file: {}", jsonl_path.string())
            ));
        }

        file << serialize_message(msg) << '\n';
        return {};
    }

    /// Deserialize a session from JSON content
    [[nodiscard]] Result<Session> deserialize_session(std::string_view content) const {
        auto doc_result = cc::utils::json::parse(content);
        if (!doc_result) {
            return std::unexpected(Error::make(
                ErrorCode::SessionCorrupted,
                "Failed to parse session JSON: " + doc_result.error().message()
            ));
        }

        auto root = doc_result->root();
        if (!root || !root.is_obj()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted, "Session JSON root must be an object"));
        }

        auto id_val = root.get("id");
        if (!id_val || !id_val.is_str()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted, "Session JSON missing string id"));
        }

        auto now = std::chrono::system_clock::now();
        SessionMetadata metadata{
            .id = SessionId{std::string(id_val.as_str())},
            .title = read_string(root, "title").value_or("Restored Session"),
            .created_at = now,
            .updated_at = now,
            .state = parse_session_state(read_string(root, "state").value_or("created")),
            .message_count = static_cast<std::uint32_t>(read_int(root, "message_count").value_or(0)),
        };
        return Session(std::move(metadata));
    }

    /// Load only metadata from a session file (for listing)
    [[nodiscard]] std::optional<SessionSummary> load_metadata(
        const std::filesystem::path& path
    ) const {
        auto doc_result = cc::utils::json::parse_file(path);
        if (!doc_result) return std::nullopt;

        auto root = doc_result->root();
        auto id_val = root.get("id");
        if (!root || !root.is_obj() || !id_val || !id_val.is_str()) return std::nullopt;

        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(path, ec);
        auto updated_at = std::chrono::system_clock::now();
        if (!ec) {
            updated_at = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                mtime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
        }

        return SessionSummary{
            .id = SessionId{std::string(id_val.as_str())},
            .title = read_string(root, "title").value_or("Untitled Session"),
            .state = parse_session_state(read_string(root, "state").value_or("created")),
            .updated_at = updated_at,
            .message_count = static_cast<std::uint32_t>(read_int(root, "message_count").value_or(0)),
        };
    }

    [[nodiscard]] static std::optional<std::string> read_string(cc::utils::json::JsonVal root, std::string_view key) {
        auto value = root.get(key);
        if (!value || !value.is_str()) return std::nullopt;
        return std::string(value.as_str());
    }

    [[nodiscard]] static std::optional<int64_t> read_int(cc::utils::json::JsonVal root, std::string_view key) {
        auto value = root.get(key);
        if (!value || !value.is_num()) return std::nullopt;
        return value.as_int();
    }

    [[nodiscard]] static SessionState parse_session_state(std::string_view state) {
        if (state == "active") return SessionState::Active;
        if (state == "paused") return SessionState::Paused;
        if (state == "completed") return SessionState::Completed;
        if (state == "expired") return SessionState::Expired;
        if (state == "error") return SessionState::Error;
        return SessionState::Created;
    }

    /// Escape a string for JSON output
    [[nodiscard]] static std::string escape_json(std::string_view input) {
        std::string result;
        result.reserve(input.size());
        for (char c : input) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        result += std::format("\\u{:04x}", static_cast<unsigned>(c));
                    } else {
                        result += c;
                    }
            }
        }
        return result;
    }

    /// Serialize a single content block to JSON
    [[nodiscard]] static std::string serialize_content_block(const ContentBlock& block) {
        return std::visit([](const auto& b) -> std::string {
            using T = std::remove_cvref_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                return std::format(R"({{"type":"text","text":"{}"}})", escape_json(b.text));
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                return std::format(R"({{"type":"tool_use","id":"{}","name":"{}","input":{}}})",
                    b.id.str(), escape_json(b.name),
                    b.input_json.empty() ? "{}" : b.input_json);
            } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                return std::format(R"({{"type":"tool_result","tool_use_id":"{}","content":"{}","is_error":{}}})",
                    b.tool_use_id.str(), escape_json(b.content),
                    b.is_error ? "true" : "false");
            } else if constexpr (std::is_same_v<T, ImageBlock>) {
                return std::format(R"({{"type":"image","media_type":"{}","data":"{}"}})",
                    escape_json(b.media_type), escape_json(b.data));
            } else if constexpr (std::is_same_v<T, DocumentBlock>) {
                return std::format(R"({{"type":"document","media_type":"{}","data":"{}"}})",
                    escape_json(b.media_type), escape_json(b.data));
            } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                return std::format(R"({{"type":"thinking","thinking":"{}","signature":"{}"}})",
                    escape_json(b.thinking), escape_json(b.signature));
            } else {
                return "{}";
            }
        }, block);
    }

    /// Serialize a message variant to a single-line JSON string
    [[nodiscard]] static std::string serialize_message(const Message& msg) {
        return std::visit([](const auto& m) -> std::string {
            using T = std::remove_cvref_t<decltype(m)>;

            std::string role_str(role_to_string(T::role));
            std::string content_arr = "[";
            for (std::size_t i = 0; i < m.content.size(); ++i) {
                if (i > 0) content_arr += ",";
                content_arr += serialize_content_block(m.content[i]);
            }
            content_arr += "]";

            std::string json = std::format(
                R"({{"role":"{}","id":"{}","timestamp":"{}","content":{})",
                role_str, m.id.str(), format_timepoint(m.timestamp), content_arr);

            // Add type-specific fields
            if constexpr (std::is_same_v<T, AssistantMessage>) {
                if (m.stop_reason) {
                    json += std::format(R"(,"stop_reason":"{}")", escape_json(*m.stop_reason));
                }
                if (m.model) {
                    json += std::format(R"(,"model":"{}")", escape_json(*m.model));
                }
            } else if constexpr (std::is_same_v<T, SystemMessage>) {
                if (m.cache_control) {
                    json += std::format(R"(,"cache_control":"{}")", escape_json(*m.cache_control));
                }
                if (m.subtype) {
                    json += std::format(R"(,"subtype":"{}")", escape_json(*m.subtype));
                }
                if (m.compact_metadata) {
                    json += std::format(
                        R"(,"compact_metadata":{{"trigger":"{}","pre_tokens":{})",
                        escape_json(m.compact_metadata->trigger),
                        m.compact_metadata->pre_tokens);
                    if (m.compact_metadata->preserved_segment) {
                        json += std::format(
                            R"(,"preserved_segment":{{"head_uuid":"{}","anchor_uuid":"{}","tail_uuid":"{}"}})",
                            escape_json(m.compact_metadata->preserved_segment->head_uuid),
                            escape_json(m.compact_metadata->preserved_segment->anchor_uuid),
                            escape_json(m.compact_metadata->preserved_segment->tail_uuid));
                    }
                    json += "}";
                }
                if (m.snip_metadata) {
                    json += R"(,"snip_metadata":{"removed_uuids":[)";
                    for (std::size_t i = 0; i < m.snip_metadata->removed_uuids.size(); ++i) {
                        if (i > 0) json += ",";
                        json += std::format(R"("{}")", escape_json(m.snip_metadata->removed_uuids[i]));
                    }
                    json += "]}";
                }
            } else if constexpr (std::is_same_v<T, ToolUseMessage>) {
                json += std::format(R"(,"tool_name":"{}","tool_input":{})",
                    escape_json(m.tool_name),
                    m.tool_input_json.empty() ? "{}" : m.tool_input_json);
            } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
                json += std::format(R"(,"tool_use_id":"{}","is_error":{})",
                    m.tool_use_id.str(), m.is_error ? "true" : "false");
            }

            json += "}";
            return json;
        }, msg);
    }

    /// Format a time_point as ISO 8601 string
    [[nodiscard]] static std::string format_timepoint(
        std::chrono::system_clock::time_point tp
    ) {
        auto time_t_val = std::chrono::system_clock::to_time_t(tp);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t_val));
        return buf;
    }

    /// Default session storage path
    [[nodiscard]] static std::filesystem::path default_storage_path() {
        if (auto* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".config" / "claude" / "sessions";
        }
        return std::filesystem::current_path() / ".claude" / "sessions";
    }
};

// ============================================================
// Session Manager - high-level session lifecycle orchestration
// ============================================================

/// High-level manager coordinating session creation, selection, and persistence.
class SessionManager {
    SessionStorage storage_;
    std::unique_ptr<Session> current_session_;

public:
    /// Initialize with default storage
    SessionManager() = default;

    /// Initialize with explicit storage
    explicit SessionManager(SessionStorage storage)
        : storage_(std::move(storage)) {}

    /// Create and activate a new session
    [[nodiscard]] Session& create_session() {
        // Save current session before creating new one
        if (current_session_) {
            current_session_->pause();
            [[maybe_unused]] auto _ = storage_.save(*current_session_);
        }

        current_session_ = std::make_unique<Session>();
        return *current_session_;
    }

    /// Resume an existing session by ID
    [[nodiscard]] Result<Session*> resume_session(const SessionId& id) {
        auto loaded = storage_.load(id);
        if (!loaded) return std::unexpected(loaded.error());

        // Save current session first
        if (current_session_) {
            current_session_->pause();
            [[maybe_unused]] auto _ = storage_.save(*current_session_);
        }

        current_session_ = std::make_unique<Session>(std::move(*loaded));
        auto resume_result = current_session_->resume();
        if (!resume_result) return std::unexpected(resume_result.error());

        return current_session_.get();
    }

    /// Get current active session (if any)
    [[nodiscard]] Session* current() noexcept { return current_session_.get(); }
    [[nodiscard]] const Session* current() const noexcept { return current_session_.get(); }

    /// Save the current session to disk
    [[nodiscard]] VoidResult save_current() {
        if (!current_session_) {
            return std::unexpected(Error::make(
                ErrorCode::SessionNotFound, "No active session to save"
            ));
        }
        return storage_.save(*current_session_);
    }

    /// List all available sessions
    [[nodiscard]] Result<std::vector<SessionSummary>> list_sessions() {
        return storage_.list_sessions();
    }

    /// End the current session normally
    void end_session() {
        if (current_session_) {
            current_session_->complete();
            [[maybe_unused]] auto _ = storage_.save(*current_session_);
            current_session_.reset();
        }
    }

    /// Clean up old sessions
    [[nodiscard]] VoidResult cleanup(std::chrono::hours max_age = std::chrono::hours{720}) {
        return storage_.cleanup(max_age);
    }
};

} // namespace cc::core
