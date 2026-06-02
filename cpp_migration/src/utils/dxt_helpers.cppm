module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.dxt_helpers;

// ---------------------------------------------------------------------------
// Constants — zip extraction safety limits
// ---------------------------------------------------------------------------

export inline constexpr std::size_t kMaxFileSize = 512ULL * 1024 * 1024;      // 512 MB per file
export inline constexpr std::size_t kMaxTotalSize = 1024ULL * 1024 * 1024;    // 1 GB total
export inline constexpr std::size_t kMaxFileCount = 100000;
export inline constexpr double kMaxCompressionRatio = 50.0;
export inline constexpr double kMinCompressionRatio = 0.5;

// ---------------------------------------------------------------------------
// Manifest types
// ---------------------------------------------------------------------------

/// Author metadata from a DXT manifest.
export struct DxtAuthor {
    std::string name;
    std::optional<std::string> email;
    std::optional<std::string> url;
};

/// Server transport configuration.
export struct DxtServerConfig {
    std::string type; // e.g. "stdio"
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
};

/// A user-configurable input field.
export struct DxtConfigInput {
    std::string name;
    std::string description;
    bool required{false};
    std::optional<std::string> default_value;
};

/// Represents a parsed and validated DXT (Desktop Extension Tool) manifest.
export struct DxtManifest {
    std::string name;
    std::string version;
    std::string description;
    DxtAuthor author;
    DxtServerConfig server;
    std::vector<DxtConfigInput> config_inputs;
    std::optional<std::string> license;
    std::optional<std::string> repository;
    std::optional<std::string> homepage;
};

// ---------------------------------------------------------------------------
// Manifest validation & parsing
// ---------------------------------------------------------------------------

/// Validates a parsed JSON manifest object (represented as string).
/// Returns the validated DxtManifest or an error.
export auto validate_manifest(std::string_view manifest_json)
    -> std::expected<DxtManifest, std::string>;

/// Parse and validate a DXT manifest from raw text.
export auto parse_and_validate_manifest_from_text(
    std::string_view manifest_text)
    -> std::expected<DxtManifest, std::string>;

/// Parse and validate a DXT manifest from raw bytes.
export auto parse_and_validate_manifest_from_bytes(
    std::span<std::uint8_t const> manifest_data)
    -> std::expected<DxtManifest, std::string>;

// ---------------------------------------------------------------------------
// Extension ID generation
// ---------------------------------------------------------------------------

/// Prefix for extension IDs (local install variants).
export enum class ExtensionIdPrefix {
    None,
    LocalUnpacked,
    LocalDxt,
};

/// Generate a stable extension ID from the manifest author and name.
export auto generate_extension_id(
    DxtManifest const& manifest,
    ExtensionIdPrefix prefix = ExtensionIdPrefix::None)
    -> std::string;

// ---------------------------------------------------------------------------
// Zip extraction
// ---------------------------------------------------------------------------

/// State tracked during zip validation.
export struct ZipValidationState {
    std::size_t file_count{0};
    std::size_t total_uncompressed_size{0};
    std::size_t compressed_size{0};
    std::vector<std::string> errors;
};

/// Metadata for a single file entry within a zip archive.
export struct ZipFileMetadata {
    std::string name;
    std::size_t original_size{0};
};

/// Result of validating a single file in a zip archive.
export struct FileValidationResult {
    bool is_valid{true};
    std::optional<std::string> error;
};

/// Represents a single extracted file (path + data).
export struct ExtractedFile {
    std::string path;
    std::vector<std::uint8_t> data;
};

/// Validates a file path to prevent path traversal attacks.
/// Returns true if the path is safe for extraction.
export auto is_path_safe(std::string_view file_path) -> bool;

/// Validates a single file during zip extraction against safety limits.
export auto validate_zip_file(
    ZipFileMetadata const& file,
    ZipValidationState& state)
    -> FileValidationResult;

/// Unzip raw bytes and return extracted files.
/// Validates against zip-bomb and path-traversal attacks.
export auto unzip_data(std::span<std::uint8_t const> zip_data)
    -> std::expected<std::vector<ExtractedFile>, std::string>;

/// Read a zip file from disk and unzip it.
export auto read_and_unzip_file(std::string_view file_path)
    -> std::expected<std::vector<ExtractedFile>, std::string>;

/// Parse Unix file modes from a zip's central directory.
/// Returns name → mode for entries created on a Unix host.
/// Entries from other hosts are omitted. Callers treat a missing
/// key as "use default mode".
export auto parse_zip_modes(std::span<std::uint8_t const> data)
    -> std::map<std::string, std::uint16_t>;
