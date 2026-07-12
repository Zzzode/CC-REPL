module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <chrono>

export module cc.ui.user_message;

// P0 user-prompt truncation: share the head/tail splitter used by
// RenderUserPromptMessage so the divergent-envelope path (UserMessageData
// payloads — teammate / plan / agent-notify / memory / resource) also caps
// displayed text at 10_000 chars.
// TS REF: src/components/messages/UserPromptMessage.tsx lines 28-70.
import cc.ui.messages.user_text_message;

export namespace cc::ui::messages {

// ─── Attachment types ────────────────────────────────────────────────

enum class AttachmentType {
    File,
    Image,
    Directory,
    Selection,
    URL
};

struct MessageAttachment {
    AttachmentType type;
    std::string name;
    std::string path;
    std::optional<std::size_t> size_bytes;
};

// ─── Message data ────────────────────────────────────────────────────

struct UserMessageData {
    std::string content;
    std::vector<MessageAttachment> attachments;
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::string> command_name;
};

// ─── Helper functions ────────────────────────────────────────────

inline std::string format_file_size(std::size_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    } else if (bytes < 1024 * 1024) {
        return std::to_string(bytes / 1024) + " KB";
    } else {
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    }
}

inline std::string render_attachment_badge(const MessageAttachment& attachment) {
    std::string badge = "[";
    switch (attachment.type) {
        case AttachmentType::File:      badge += "F "; break;
        case AttachmentType::Image:     badge += "I "; break;
        case AttachmentType::Directory: badge += "D "; break;
        case AttachmentType::Selection: badge += "S "; break;
        case AttachmentType::URL:       badge += "U "; break;
    }
    badge += attachment.name;
    if (attachment.size_bytes.has_value()) {
        badge += " (" + format_file_size(*attachment.size_bytes) + ")";
    }
    badge += "]";
    return badge;
}

inline std::string render_attachments(const std::vector<MessageAttachment>& attachments) {
    std::string result;
    for (const auto& attachment : attachments) {
        result += render_attachment_badge(attachment) + " ";
    }
    return result;
}

// ─── Rendering functions ─────────────────────────────────────────────

inline std::string render_user_message(const UserMessageData& data) {
    // Truncate long pastes for display (TS REF: UserPromptMessage.tsx lines 64-70).
    // The divergent-envelope path (UserMessageData) must apply the same 10K cap
    // as the faithful RenderUserPromptMessage path.
    std::string result = TruncateUserPromptText(data.content);
    if (!data.attachments.empty()) {
        result += "\n" + render_attachments(data.attachments);
    }
    return result;
}

inline bool is_image_attachment(const MessageAttachment& attachment) {
    return attachment.type == AttachmentType::Image;
}

} // namespace cc::ui::messages
