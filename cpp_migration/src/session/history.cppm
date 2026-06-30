/// @file history.cppm
/// @brief Conversation history module for managing message sequences, persistence,
/// compression, and context window management.
/// Handles conversation history, serialization, and trimming to stay within context limits.
module;

#include <cstdint>
#include <exception>
#include <expected>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <fstream>
#include <sstream>
#include <format>
#include <chrono>
#include <filesystem>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

export module cc.session.history;

import cc.types.types;
import cc.utils.json;

export namespace cc::core {

// ============================================================
// Conversation
// ============================================================

/// A full conversation history
class Conversation {
    std::deque<Message> messages_;
    std::optional<ConversationId> id_;
    std::string title_;
    std::chrono::system_clock::time_point created_at_;
    std::chrono::system_clock::time_point updated_at_;
    std::size_t max_messages_;
    [[maybe_unused]] std::size_t max_total_tokens_;
    std::mutex mutex_;

public:
    Conversation()
        : id_(), created_at_(std::chrono::system_clock::now()),
        updated_at_(std::chrono::system_clock::now()),
        max_messages_(100), max_total_tokens_(200000) {}

    explicit Conversation(ConversationId id)
        : id_(std::move(id)),
        created_at_(std::chrono::system_clock::now()),
        updated_at_(std::chrono::system_clock::now()),
        max_messages_(100), max_total_tokens_(200000) {}

    /// Add message
    void add_message(Message msg) {
        std::lock_guard lock(mutex_);
        messages_.push_back(std::move(msg));
        updated_at_ = std::chrono::system_clock::now();
        trim_if_needed();
    }

    /// Get all messages (read-only)
    [[nodiscard]] std::vector<Message> get_messages() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return std::vector<Message>(messages_.begin(), messages_.end());
    }

    /// Get a slice of the most recent messages
    [[nodiscard]] std::vector<Message> get_recent_messages(std::size_t count) const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        std::vector<Message> result;
        auto start = (messages_.size() > count) ? (messages_.size() - count) : 0;
        for (std::size_t i = start; i < messages_.size(); ++i) {
            result.push_back(messages_[i]);
        }
        return result;
    }

    /// Clear conversation
    void clear() {
        std::lock_guard lock(mutex_);
        messages_.clear();
        updated_at_ = std::chrono::system_clock::now();
    }

    /// Get conversation ID
    [[nodiscard]] std::optional<ConversationId> get_id() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return id_;
    }

    /// Set conversation title
    void set_title(std::string title) {
        std::lock_guard lock(mutex_);
        title_ = std::move(title);
        updated_at_ = std::chrono::system_clock::now();
    }

    /// Get conversation title
    [[nodiscard]] std::string get_title() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return title_;
    }

    /// Get message count
    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return messages_.size();
    }

    /// Get created timestamp
    [[nodiscard]] std::chrono::system_clock::time_point get_created_at() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return created_at_;
    }

    /// Get updated timestamp
    [[nodiscard]] std::chrono::system_clock::time_point get_updated_at() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return updated_at_;
    }

private:
    /// Trim conversation stays within limits, starting from oldest messages
    void trim_if_needed() {
        while (messages_.size() > max_messages_) {
            messages_.pop_front();
        }
    }
};

// ============================================================
// Conversation Store
// ============================================================

/// Manages multiple conversations
class ConversationStore {
    std::unordered_map<std::string, std::unique_ptr<Conversation>> conversations_;
    std::optional<std::string> active_conversation_id_;
    std::string storage_path_;
    std::mutex mutex_;

public:
    ConversationStore() = default;

    explicit ConversationStore(std::string storage_path)
        : storage_path_(std::move(storage_path)) {}

    /// Create a new conversation
    Conversation* create_conversation() {
        std::lock_guard lock(mutex_);
        return create_conversation_locked();
    }

    /// Get an existing conversation by ID, or create it and make it active.
    [[nodiscard]] Conversation* get_or_create_conversation(std::string id) {
        std::lock_guard lock(mutex_);
        if (id.empty()) return create_conversation_locked();
        if (auto it = conversations_.find(id); it != conversations_.end()) {
            active_conversation_id_ = id;
            return it->second.get();
        }

        auto conversation = std::make_unique<Conversation>(ConversationId{id});
        auto* ptr = conversation.get();
        conversations_[id] = std::move(conversation);
        active_conversation_id_ = std::move(id);
        return ptr;
    }

    /// Get active conversation
    [[nodiscard]] Conversation* get_active_conversation() {
        std::lock_guard lock(mutex_);
        if (!active_conversation_id_) return create_conversation_locked();
        auto it = conversations_.find(*active_conversation_id_);
        return it != conversations_.end() ? it->second.get() : create_conversation_locked();
    }

    /// Get the active conversation ID, if one has been selected or loaded.
    [[nodiscard]] std::optional<std::string> active_conversation_id() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        return active_conversation_id_;
    }

    /// Switch to a conversation
    [[nodiscard]] bool switch_conversation(const std::string& id) {
        std::lock_guard lock(mutex_);
        if (conversations_.contains(id)) {
            active_conversation_id_ = id;
            return true;
        }
        return false;
    }

    /// Get all conversation IDs
    [[nodiscard]] std::vector<std::string> get_conversation_ids() const {
        std::lock_guard lock(const_cast<std::mutex&>(mutex_));
        std::vector<std::string> ids;
        ids.reserve(conversations_.size());
        for (const auto& [id, _] : conversations_) {
            ids.push_back(id);
        }
        return ids;
    }

    /// Delete a conversation
    bool delete_conversation(const std::string& id) {
        std::lock_guard lock(mutex_);
        if (conversations_.erase(id) > 0) {
            if (active_conversation_id_ == id) {
                active_conversation_id_.reset();
            }
            return true;
        }
        return false;
    }

    /// Save all conversations to disk
    [[nodiscard]] VoidResult save_all() const {
        if (storage_path_.empty()) {
            return {};
        }

        try {
            auto path = std::filesystem::path(storage_path_);
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }

            cc::utils::json::JsonMutDoc doc;
            auto root = doc.object();
            root.add("version", doc.number(static_cast<int64_t>(1)));
            if (active_conversation_id_) {
                root.add("active_conversation_id", doc.string(*active_conversation_id_));
            } else {
                root.add("active_conversation_id", doc.null());
            }

            auto conversations = doc.array();
            {
                std::lock_guard lock(const_cast<std::mutex&>(mutex_));
                for (const auto& [id, conversation] : conversations_) {
                    auto serialized = serialize_conversation(doc, id, *conversation);
                    conversations.append(serialized);
                }
            }
            root.add("conversations", conversations);
            doc.set_root(root);

            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open()) {
                return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                    std::format("Failed to open conversation history file '{}'", path.string())));
            }
            output << doc.to_pretty_string();
            if (!output.good()) {
                return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                    std::format("Failed to write conversation history file '{}'", path.string())));
            }
        } catch (const std::exception& e) {
            return std::unexpected(Error::make(ErrorCode::ConfigWriteError,
                std::format("Failed to save conversation history: {}", e.what())));
        }

        return {};
    }

    /// Load all conversations from disk
    [[nodiscard]] VoidResult load_all() {
        if (storage_path_.empty() || !std::filesystem::exists(storage_path_)) {
            return {};
        }

        auto parsed = cc::utils::json::parse_file(storage_path_);
        if (!parsed) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
                parsed.error().message()));
        }

        auto root = parsed->root();
        if (!root.is_obj()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
                "Conversation history root must be an object"));
        }

        auto conversations_value = root.get("conversations");
        if (!conversations_value.is_arr()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
                "Conversation history missing conversations array"));
        }

        std::unordered_map<std::string, std::unique_ptr<Conversation>> loaded;
        std::optional<std::string> loaded_active;
        if (auto active = root.get("active_conversation_id"); active.is_str()) {
            loaded_active = std::string(active.as_str());
        }

        std::optional<Error> parse_error;
        conversations_value.iter([&](cc::utils::json::JsonVal item) {
            if (parse_error) return;
            auto parsed_conversation = parse_conversation(item);
            if (!parsed_conversation) {
                parse_error = parsed_conversation.error();
                return;
            }
            auto [id, conversation] = std::move(*parsed_conversation);
            loaded[id] = std::move(conversation);
        });

        if (parse_error) {
            return std::unexpected(*parse_error);
        }

        if (loaded_active && !loaded.contains(*loaded_active)) {
            loaded_active.reset();
        }

        std::lock_guard lock(mutex_);
        conversations_ = std::move(loaded);
        active_conversation_id_ = std::move(loaded_active);
        return {};
    }

private:
    [[nodiscard]] Conversation* create_conversation_locked() {
        auto id = std::format("conv_{}",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto conversation = std::make_unique<Conversation>(ConversationId{id});
        auto* ptr = conversation.get();
        conversations_[id] = std::move(conversation);
        active_conversation_id_ = id;
        return ptr;
    }

    [[nodiscard]] static int64_t to_epoch_ms(std::chrono::system_clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            time.time_since_epoch()).count();
    }

    [[nodiscard]] static std::chrono::system_clock::time_point from_epoch_ms(int64_t ms) {
        return std::chrono::system_clock::time_point{std::chrono::milliseconds(ms)};
    }

    [[nodiscard]] static cc::utils::json::JsonMutVal serialize_content_block(
        cc::utils::json::JsonMutDoc& doc,
        const ContentBlock& block) {
        auto obj = doc.object();
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                obj.add("type", doc.string("text"));
                obj.add("text", doc.string(value.text));
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                obj.add("type", doc.string("tool_use"));
                obj.add("id", doc.string(value.id.value));
                obj.add("name", doc.string(value.name));
                obj.add("input_json", doc.string(value.input_json));
            } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                obj.add("type", doc.string("tool_result"));
                obj.add("tool_use_id", doc.string(value.tool_use_id.value));
                obj.add("content", doc.string(value.content));
                obj.add("is_error", doc.boolean(value.is_error));
            } else if constexpr (std::is_same_v<T, ImageBlock>) {
                obj.add("type", doc.string("image"));
                obj.add("media_type", doc.string(value.media_type));
                obj.add("data", doc.string(value.data));
            } else if constexpr (std::is_same_v<T, DocumentBlock>) {
                obj.add("type", doc.string("document"));
                obj.add("media_type", doc.string(value.media_type));
                obj.add("data", doc.string(value.data));
            } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                obj.add("type", doc.string("thinking"));
                obj.add("thinking", doc.string(value.thinking));
                obj.add("signature", doc.string(value.signature));
            }
        }, block);
        return obj;
    }

    [[nodiscard]] static cc::utils::json::JsonMutVal serialize_content_blocks(
        cc::utils::json::JsonMutDoc& doc,
        const std::vector<ContentBlock>& blocks) {
        auto arr = doc.array();
        for (const auto& block : blocks) {
            arr.append(serialize_content_block(doc, block));
        }
        return arr;
    }

    [[nodiscard]] static cc::utils::json::JsonMutVal serialize_compact_metadata(
        cc::utils::json::JsonMutDoc& doc,
        const CompactMetadata& metadata) {
        auto obj = doc.object();
        obj.add("trigger", doc.string(metadata.trigger));
        obj.add("pre_tokens", doc.number(static_cast<int64_t>(metadata.pre_tokens)));
        if (metadata.preserved_segment) {
            auto preserved = doc.object();
            preserved.add("head_uuid", doc.string(metadata.preserved_segment->head_uuid));
            preserved.add("anchor_uuid", doc.string(metadata.preserved_segment->anchor_uuid));
            preserved.add("tail_uuid", doc.string(metadata.preserved_segment->tail_uuid));
            obj.add("preserved_segment", preserved);
        }
        return obj;
    }

    [[nodiscard]] static cc::utils::json::JsonMutVal serialize_snip_metadata(
        cc::utils::json::JsonMutDoc& doc,
        const SnipMetadata& metadata) {
        auto obj = doc.object();
        auto removed = doc.array();
        for (const auto& uuid : metadata.removed_uuids) {
            removed.append(doc.string(uuid));
        }
        obj.add("removed_uuids", removed);
        return obj;
    }

    [[nodiscard]] static cc::utils::json::JsonMutVal serialize_message(
        cc::utils::json::JsonMutDoc& doc,
        const Message& message) {
        auto obj = doc.object();
        obj.add("role", doc.string(std::string(role_to_string(get_role(message)))));
        std::visit([&](const auto& value) {
            obj.add("id", doc.string(value.id.value));
            obj.add("timestamp_ms", doc.number(to_epoch_ms(value.timestamp)));
            obj.add("content", serialize_content_blocks(doc, value.content));

            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AssistantMessage>) {
                if (value.stop_reason) obj.add("stop_reason", doc.string(*value.stop_reason));
                if (value.model) obj.add("model", doc.string(*value.model));
            } else if constexpr (std::is_same_v<T, SystemMessage>) {
                if (value.cache_control) obj.add("cache_control", doc.string(*value.cache_control));
                if (value.subtype) obj.add("subtype", doc.string(*value.subtype));
                if (value.compact_metadata) {
                    obj.add("compact_metadata", serialize_compact_metadata(doc, *value.compact_metadata));
                }
                if (value.snip_metadata) {
                    obj.add("snip_metadata", serialize_snip_metadata(doc, *value.snip_metadata));
                }
            } else if constexpr (std::is_same_v<T, ToolUseMessage>) {
                obj.add("tool_name", doc.string(value.tool_name));
                obj.add("tool_input_json", doc.string(value.tool_input_json));
            } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
                obj.add("tool_use_id", doc.string(value.tool_use_id.value));
                obj.add("is_error", doc.boolean(value.is_error));
            }
        }, message);
        return obj;
    }

    [[nodiscard]] static cc::utils::json::JsonMutVal serialize_conversation(
        cc::utils::json::JsonMutDoc& doc,
        const std::string& id,
        const Conversation& conversation) {
        auto obj = doc.object();
        obj.add("id", doc.string(id));
        obj.add("title", doc.string(conversation.get_title()));
        obj.add("created_at_ms", doc.number(to_epoch_ms(conversation.get_created_at())));
        obj.add("updated_at_ms", doc.number(to_epoch_ms(conversation.get_updated_at())));

        auto messages = doc.array();
        for (const auto& message : conversation.get_messages()) {
            messages.append(serialize_message(doc, message));
        }
        obj.add("messages", messages);
        return obj;
    }

    [[nodiscard]] static Result<ContentBlock> parse_content_block(cc::utils::json::JsonVal block) {
        if (!block.is_obj()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
                "Conversation content block must be an object"));
        }
        auto type = block.get_string("type");
        if (type == "text") {
            return TextBlock{block.get_string("text")};
        }
        if (type == "tool_use") {
            return ToolUseBlock{
                ToolUseId{block.get_string("id")},
                block.get_string("name"),
                block.get_string("input_json")};
        }
        if (type == "tool_result") {
            return ToolResultBlock{
                ToolUseId{block.get_string("tool_use_id")},
                block.get_string("content"),
                block.get("is_error").is_bool() && block.get("is_error").as_bool()};
        }
        if (type == "image") {
            ImageBlock ib;
            ib.media_type = block.get_string("media_type");
            ib.data       = block.get_string("data");
            return ib;
        }
        if (type == "document") {
            return DocumentBlock{block.get_string("media_type"), block.get_string("data")};
        }
        if (type == "thinking") {
            return ThinkingBlock{block.get_string("thinking"), block.get_string("signature")};
        }
        return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
            std::format("Unknown conversation content block type '{}'", type)));
    }

    [[nodiscard]] static Result<std::vector<ContentBlock>> parse_content_blocks(
        cc::utils::json::JsonVal content) {
        std::vector<ContentBlock> blocks;
        if (!content.valid() || !content.is_arr()) {
            return blocks;
        }

        std::optional<Error> parse_error;
        content.iter([&](cc::utils::json::JsonVal item) {
            if (parse_error) return;
            auto parsed = parse_content_block(item);
            if (!parsed) {
                parse_error = parsed.error();
                return;
            }
            blocks.push_back(std::move(*parsed));
        });
        if (parse_error) {
            return std::unexpected(*parse_error);
        }
        return blocks;
    }

    [[nodiscard]] static std::optional<CompactMetadata> parse_compact_metadata(
        cc::utils::json::JsonVal value) {
        if (!value.valid() || !value.is_obj()) return std::nullopt;
        CompactMetadata metadata;
        metadata.trigger = value.get_string("trigger");
        auto pre_tokens = value.get("pre_tokens");
        if (pre_tokens.is_num()) {
            metadata.pre_tokens = static_cast<std::uint32_t>(pre_tokens.as_int());
        }
        auto preserved = value.get("preserved_segment");
        if (preserved.valid() && preserved.is_obj()) {
            metadata.preserved_segment = CompactPreservedSegment{
                .head_uuid = preserved.get_string("head_uuid"),
                .anchor_uuid = preserved.get_string("anchor_uuid"),
                .tail_uuid = preserved.get_string("tail_uuid"),
            };
        }
        return metadata;
    }

    [[nodiscard]] static std::optional<SnipMetadata> parse_snip_metadata(
        cc::utils::json::JsonVal value) {
        if (!value.valid() || !value.is_obj()) return std::nullopt;
        auto removed = value.get("removed_uuids");
        if (!removed.is_arr()) return std::nullopt;

        SnipMetadata metadata;
        removed.iter([&](cc::utils::json::JsonVal item) {
            if (item.is_str()) {
                metadata.removed_uuids.emplace_back(item.as_str());
            }
        });
        if (metadata.removed_uuids.empty()) return std::nullopt;
        return metadata;
    }

    [[nodiscard]] static Result<Message> parse_message(cc::utils::json::JsonVal value) {
        if (!value.is_obj()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
                "Conversation message must be an object"));
        }

        auto content = parse_content_blocks(value.get("content"));
        if (!content) return std::unexpected(content.error());

        MessageBase base{
            MessageId{value.get_string("id")},
            from_epoch_ms(value.get_int("timestamp_ms")),
            std::move(*content)};

        auto role = value.get_string("role");
        if (role == "user") {
            return UserMessage{std::move(base)};
        }
        if (role == "assistant") {
            auto tool_name = value.get_string("tool_name");
            if (!tool_name.empty()) {
                return ToolUseMessage{std::move(base), tool_name, value.get_string("tool_input_json")};
            }
            std::optional<std::string> stop_reason;
            std::optional<std::string> model;
            if (auto stop = value.get("stop_reason"); stop.is_str()) stop_reason = std::string(stop.as_str());
            if (auto model_value = value.get("model"); model_value.is_str()) model = std::string(model_value.as_str());
            return AssistantMessage{std::move(base), std::move(stop_reason), std::move(model)};
        }
        if (role == "system") {
            std::optional<std::string> cache_control;
            if (auto cache = value.get("cache_control"); cache.is_str()) cache_control = std::string(cache.as_str());
            std::optional<std::string> subtype;
            if (auto subtype_value = value.get("subtype"); subtype_value.is_str()) {
                subtype = std::string(subtype_value.as_str());
            }
            return SystemMessage{
                std::move(base),
                std::move(cache_control),
                std::move(subtype),
                parse_compact_metadata(value.get("compact_metadata")),
                parse_snip_metadata(value.get("snip_metadata")),
            };
        }
        if (role == "tool") {
            return ToolResultMessage{
                std::move(base),
                ToolUseId{value.get_string("tool_use_id")},
                value.get("is_error").is_bool() && value.get("is_error").as_bool()};
        }

        return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
            std::format("Unknown conversation message role '{}'", role)));
    }

    [[nodiscard]] static Result<std::pair<std::string, std::unique_ptr<Conversation>>> parse_conversation(
        cc::utils::json::JsonVal value) {
        if (!value.is_obj()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
                "Conversation entry must be an object"));
        }

        auto id = value.get_string("id");
        if (id.empty()) {
            return std::unexpected(Error::make(ErrorCode::SessionCorrupted,
                "Conversation entry missing id"));
        }

        auto conversation = std::make_unique<Conversation>(ConversationId{id});
        conversation->set_title(value.get_string("title"));

        std::optional<Error> parse_error;
        auto messages = value.get("messages");
        if (messages.valid() && messages.is_arr()) {
            messages.iter([&](cc::utils::json::JsonVal item) {
                if (parse_error) return;
                auto parsed = parse_message(item);
                if (!parsed) {
                    parse_error = parsed.error();
                    return;
                }
                conversation->add_message(std::move(*parsed));
            });
        }

        if (parse_error) {
            return std::unexpected(*parse_error);
        }
        return std::pair<std::string, std::unique_ptr<Conversation>>{id, std::move(conversation)};
    }
};

} // namespace cc::core
