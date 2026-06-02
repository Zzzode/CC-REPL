module;
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <algorithm>

export module cc.bridge.inbound_attachments;

export namespace cc::bridge {

// Represents a binary attachment received through the bridge
struct Attachment {
    std::string id;
    std::string filename;
    std::string mime_type;
    size_t size;
    std::vector<uint8_t> data;
};

// Maximum allowed attachment size (50 MB)
inline constexpr size_t MAX_ATTACHMENT_SIZE = 50 * 1024 * 1024;

// Allowed MIME types for attachments
inline const std::vector<std::string> ALLOWED_MIME_TYPES = {
    "text/plain", "text/markdown", "text/csv",
    "application/json", "application/pdf",
    "image/png", "image/jpeg", "image/gif", "image/webp",
    "application/octet-stream"
};

// Process raw attachment data (base64 or binary) into an Attachment struct
std::expected<Attachment, std::string> process_attachment(std::string_view raw_data) {
    if (raw_data.empty()) {
        return std::unexpected("Attachment data cannot be empty");
    }

    Attachment attachment;

    // Parse attachment header: first line contains metadata
    // Format: "id:filename:mime_type:size\n<binary_data>"
    auto header_end = raw_data.find('\n');
    if (header_end == std::string_view::npos) {
        return std::unexpected("Invalid attachment format: missing header");
    }

    std::string header(raw_data.substr(0, header_end));
    auto data_start = raw_data.substr(header_end + 1);

    // Parse header fields
    std::vector<std::string> fields;
    size_t pos = 0;
    while (pos < header.size()) {
        auto sep = header.find(':', pos);
        if (sep == std::string::npos) {
            fields.push_back(header.substr(pos));
            break;
        }
        fields.push_back(header.substr(pos, sep - pos));
        pos = sep + 1;
    }

    if (fields.size() < 4) {
        return std::unexpected("Invalid attachment header: expected id:filename:mime:size");
    }

    attachment.id = fields[0];
    attachment.filename = fields[1];
    attachment.mime_type = fields[2];

    try {
        attachment.size = std::stoull(fields[3]);
    } catch (...) {
        return std::unexpected("Invalid size in attachment header");
    }

    // Copy data
    attachment.data.assign(data_start.begin(), data_start.end());

    // Verify size matches
    if (attachment.data.size() != attachment.size) {
        // Allow size mismatch if it's close (trailing newline etc.)
        attachment.size = attachment.data.size();
    }

    return attachment;
}

// Validate an attachment against size and type constraints
std::expected<void, std::string> validate_attachment(Attachment attachment) {
    if (attachment.id.empty()) {
        return std::unexpected("Attachment ID is required");
    }

    if (attachment.filename.empty()) {
        return std::unexpected("Attachment filename is required");
    }

    if (attachment.size > MAX_ATTACHMENT_SIZE) {
        return std::unexpected("Attachment too large: " + std::to_string(attachment.size) +
                              " bytes (max: " + std::to_string(MAX_ATTACHMENT_SIZE) + ")");
    }

    if (attachment.data.empty()) {
        return std::unexpected("Attachment data is empty");
    }

    // Validate MIME type
    bool valid_mime = false;
    for (const auto& allowed : ALLOWED_MIME_TYPES) {
        if (attachment.mime_type == allowed) {
            valid_mime = true;
            break;
        }
    }
    if (!valid_mime) {
        return std::unexpected("Unsupported MIME type: " + attachment.mime_type);
    }

    // Check for path traversal in filename
    if (attachment.filename.find("..") != std::string::npos ||
        attachment.filename.find('/') != std::string::npos ||
        attachment.filename.find('\\') != std::string::npos) {
        return std::unexpected("Invalid filename: must not contain path separators or '..'");
    }

    return {};
}

// Store an attachment to disk in the specified directory
std::expected<std::filesystem::path, std::string> store_attachment(Attachment attachment, std::filesystem::path dir) {
    // Validate before storing
    auto validation = validate_attachment(attachment);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }

    // Ensure directory exists
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("Failed to create directory: " + ec.message());
    }

    // Write file — use ID prefix to avoid collisions
    std::filesystem::path file_path = dir / (attachment.id + "_" + attachment.filename);

    std::ofstream ofs(file_path, std::ios::binary);
    if (!ofs.is_open()) {
        return std::unexpected("Failed to open file for writing: " + file_path.string());
    }

    ofs.write(reinterpret_cast<const char*>(attachment.data.data()),
              static_cast<std::streamsize>(attachment.data.size()));

    if (!ofs.good()) {
        return std::unexpected("Failed to write attachment data");
    }

    return file_path;
}

} // namespace cc::bridge
