/// @file messages.cppm
/// @brief Inbound message processing and attachments
module;

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <functional>
#include <unordered_map>

export module cc.bridge.messages;

import cc.types.types;

export namespace cc::bridge {

/// Content block types
enum class ContentBlockType {
    Text,
    Image,
    ToolUse,
    ToolResult
};

/// Base64 image source
struct Base64ImageSource {
    std::string media_type;
    std::string data;
};

/// Image content block
struct ImageBlock {
    ContentBlockType type = ContentBlockType::Image;
    Base64ImageSource source;
};

/// Text content block
struct TextBlock {
    ContentBlockType type = ContentBlockType::Text;
    std::string text;
};

/// Content block (variant)
using ContentBlock = std::variant<TextBlock, ImageBlock>;

/// SDK message from bridge
struct SDKMessage {
    std::string type;
    struct {
        std::optional<std::variant<std::string, std::vector<ContentBlock>>> content;
    } message;
    std::optional<std::string> uuid;
};

/// Extracted inbound message fields
struct ExtractedMessage {
    std::variant<std::string, std::vector<ContentBlock>> content;
    std::optional<std::string> uuid;
};

/// Check if image block is malformed (missing media_type)
bool is_malformed_base64_image(const ContentBlock& block) {
    if (!std::holds_alternative<ImageBlock>(block)) {
        return false;
    }
    const auto& img = std::get<ImageBlock>(block);
    return img.source.media_type.empty();
}

/// Detect image format from base64 data
std::string detect_image_format_from_base64(std::string_view data) {
    if (data.find("/9j/") == 0) return "image/jpeg";
    if (data.find("iVBOR") == 0) return "image/png";
    if (data.find("R0lGOD") == 0) return "image/gif";
    if (data.find("UklGR") == 0) return "image/webp";
    if (data.find("Qk") == 0) return "image/bmp";
    return "image/jpeg"; // Default
}

/// Normalize image blocks (fix media_type vs mediaType)
std::vector<ContentBlock> normalize_image_blocks(const std::vector<ContentBlock>& blocks) {
    // Fast path: no malformed blocks, return original
    bool has_malformed = false;
    for (const auto& block : blocks) {
        if (is_malformed_base64_image(block)) {
            has_malformed = true;
            break;
        }
    }
    if (!has_malformed) {
        return blocks;
    }
    
    // Normalize
    std::vector<ContentBlock> normalized;
    normalized.reserve(blocks.size());
    for (const auto& block : blocks) {
        if (!is_malformed_base64_image(block)) {
            normalized.push_back(block);
            continue;
        }
        
        // Fix media type
        auto img = std::get<ImageBlock>(block);
        if (img.source.media_type.empty()) {
            img.source.media_type = detect_image_format_from_base64(img.source.data);
        }
        normalized.push_back(img);
    }
    return normalized;
}

/// Extract inbound message fields
std::optional<ExtractedMessage> extract_inbound_message_fields(const SDKMessage& msg) {
    if (msg.type != "user") {
        return std::nullopt;
    }
    
    if (!msg.message.content) {
        return std::nullopt;
    }
    
    // Check if content is empty array
    if (std::holds_alternative<std::vector<ContentBlock>>(*msg.message.content)) {
        const auto& arr = std::get<std::vector<ContentBlock>>(*msg.message.content);
        if (arr.empty()) {
            return std::nullopt;
        }
    }
    
    ExtractedMessage result;
    if (std::holds_alternative<std::vector<ContentBlock>>(*msg.message.content)) {
        result.content = normalize_image_blocks(
            std::get<std::vector<ContentBlock>>(*msg.message.content)
        );
    } else {
        result.content = *msg.message.content;
    }
    result.uuid = msg.uuid;
    
    return result;
}

/// Flush gate for message buffering
class MessageFlushGate {
    bool is_open_ = false;
    std::vector<SDKMessage> buffer_;
    std::function<void(const SDKMessage&)> handler_;
    
public:
    explicit MessageFlushGate(std::function<void(const SDKMessage&)> handler = nullptr)
        : handler_(std::move(handler)) {}

    void set_handler(std::function<void(const SDKMessage&)> handler) {
        handler_ = std::move(handler);
    }

    /// Open the gate - flush buffer and let future messages pass
    void open() {
        is_open_ = true;
        if (handler_) {
            for (const auto& msg : buffer_) handler_(msg);
        }
        buffer_.clear();
    }
    
    /// Close the gate - buffer messages
    void close() {
        is_open_ = false;
    }
    
    /// Check if gate is open
    bool is_open() const { return is_open_; }
    
    /// Enqueue a message (or pass through if open)
    void enqueue(const SDKMessage& msg) {
        if (is_open_) {
            if (handler_) handler_(msg);
            else buffer_.push_back(msg);
        } else {
            buffer_.push_back(msg);
        }
    }
    
    /// Get buffered messages
    const std::vector<SDKMessage>& buffer() const { return buffer_; }
    
    /// Clear buffer
    void clear_buffer() { buffer_.clear(); }
};

} // namespace cc::bridge
