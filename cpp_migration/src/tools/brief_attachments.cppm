module;

#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <cstdint>

export module cc.tools.brief_attachments;

export namespace cc::tools::brief_attachments {

enum class AttachmentKind { File, Image, Snippet, URL };

struct BriefAttachment {
    AttachmentKind kind;
    std::string name;
    std::string content_or_path;
    std::optional<size_t> size_bytes;
};

struct UploadResult {
    bool success;
    std::string url;
    std::optional<std::string> error;
};

inline std::expected<std::vector<BriefAttachment>, std::string> process_attachments(const std::vector<std::string>& paths) {
    return std::vector<BriefAttachment>{};
}

inline std::expected<UploadResult, std::string> upload_attachment(const BriefAttachment& attachment) {
    return UploadResult{true, "", std::nullopt};
}

inline bool is_supported_format(std::string_view filename) {
    return true;
}

inline std::optional<size_t> get_max_upload_size() {
    return 10 * 1024 * 1024;
}

} // namespace cc::tools::brief_attachments
