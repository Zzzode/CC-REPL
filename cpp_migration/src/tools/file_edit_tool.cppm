// FileEditTool - Edits files using string replacement.
// Agent 9: audit completed 2026-06-09.
//   - Rewritten to mirror TS FileEditTool.ts validateInput() branches 1:1
//     (error codes 0..10 + meta field passthrough).
//   - Integrated cc.tools.file_edit_types: ValidationOutcome /
//     ValidationErrorCode / FileEditInput / FileEditOutput.
//   - Integrated cc.utils.file_edit: find_actual_string,
//     preserve_quote_style, get_patch_for_edit, read_file_for_edit,
//     normalize_file_edit_input, are_file_edits_inputs_equivalent,
//     compute_structured_patch.
//   - Integrated cc.tools.file_edit_prompt: user_facing_name,
//     format_tool_result_block, format_edit_preview.
//   - Integrated cc.tools.sed_edit_parser: try_parse_sed_in_place() now
//     delegates to parse_sed_edit_command() for real parsing.
//   - NOTE: React UI components in UI.tsx (JSX renderers) deferred to
//     Phase 4 / FTXUI. Only the pure text-formatting helpers were ported.
module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module cc.tools.file_edit;

import cc.tools.tool;
import cc.tools.file_edit_types;
import cc.tools.file_edit_prompt;
import cc.utils.file;
import cc.utils.error;
import cc.utils.json;
import cc.utils.file_edit;
import cc.utils.file_read_cache;
import cc.utils.string_utils;
import cc.utils.path;
import cc.tools.sed_edit_parser;

export namespace cc::tools::file_edit {

using cc::core::ToolInput;
using cc::core::ToolResult;
using cc::core::ToolDefinition;
using cc::core::ToolPermission;
using cc::core::InputSchema;
using cc::core::SchemaProperty;
using cc::utils::Result;

namespace fs = std::filesystem;

// =========================================================================
// Constants (from FileEditTool.ts top-level)
// =========================================================================

/// V8/Bun string-length guard. TS: MAX_EDIT_FILE_SIZE.
inline constexpr std::uint64_t kMaxEditFileSize = 1ULL * 1024 * 1024 * 1024;

/// Structured-patch context lines (matches TS diff display pipeline).
inline constexpr int kPatchContextLines = 3;

// =========================================================================
// Input parsing (= TS z.output<FileEditInput> after semanticBoolean)
// =========================================================================

struct ParsedInput {
    fs::path file_path;
    std::string old_string;
    std::string new_string;
    bool replace_all = false;

    static std::expected<ParsedInput, std::string> from_json(std::string_view json_sv) {
        using namespace cc::utils::json;
        auto doc = parse(json_sv);
        if (!doc) return std::unexpected("Invalid JSON input");
        auto root = doc->root();
        if (!root.is_obj()) return std::unexpected("Expected JSON object");

        ParsedInput in;
        auto path_node = root.get("file_path");
        if (!path_node.is_str()) return std::unexpected("Missing 'file_path' field");
        in.file_path = std::string(path_node.as_str());

        auto old_n = root.get("old_string");
        auto new_n = root.get("new_string");
        if (!old_n.is_str() || !new_n.is_str()) {
            return std::unexpected("Missing 'old_string' or 'new_string' field");
        }
        in.old_string = std::string(old_n.as_str());
        in.new_string = std::string(new_n.as_str());

        // semanticBoolean — accept bool, numeric, string variants.
        auto ra_node = root.get("replace_all");
        if (ra_node.is_bool()) in.replace_all = ra_node.as_bool();
        else if (ra_node.is_num()) in.replace_all = ra_node.as_int() != 0;
        else if (ra_node.is_str()) {
            std::string_view s = ra_node.as_str();
            in.replace_all = (s == "true" || s == "1" || s == "yes" || s == "on");
        }

        if (in.file_path.empty()) return std::unexpected("Missing 'file_path' field");
        return in;
    }
};

// =========================================================================
// Read timestamp tracking (= TS readFileState map entries)
// =========================================================================

struct ReadTimestamp {
    std::chrono::system_clock::time_point timestamp;
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> limit;
    std::optional<std::string> content;
    bool is_partial_view = false;
};

class ReadFileState {
public:
    std::optional<ReadTimestamp> get(const fs::path& p) const {
        auto it = map_.find(p.string());
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }
    void set(const fs::path& p, ReadTimestamp ts) {
        map_[p.string()] = std::move(ts);
    }
private:
    std::unordered_map<std::string, ReadTimestamp> map_;
};

// =========================================================================
// Sed parser integration (delegates to cc.tools.sed_edit_parser)
// =========================================================================

/// Try to extract a FileEditInput from a raw `sed -i` command string.
/// Enables the Edit UI / permission pipeline to handle BashTool-style
/// in-place edits transparently.
///
/// Parses the command via cc::tools::sed_edit_parser::parse_sed_edit_command
/// and converts the result into a ParsedInput suitable for FileEditTool.
/// Returns std::nullopt if the command is not a valid sed in-place edit
/// (e.g. not a sed command, missing -i flag, multiple files, etc.).
[[nodiscard]] inline std::optional<ParsedInput>
try_parse_sed_in_place(std::string_view sed_command) {
    auto result = cc::tools::sed_edit_parser::parse_sed_edit_command(sed_command);
    if (!result) return std::nullopt;

    const auto& info = *result;
    // Only simple substitution commands can be mapped to FileEditTool input.
    if (info.pattern.empty()) return std::nullopt;

    ParsedInput out;
    out.file_path = fs::path{info.file_path};
    out.old_string = info.pattern;
    out.new_string = info.replacement;
    out.replace_all = (info.flags.find('g') != std::string::npos);
    return out;
}

// =========================================================================
// Core class
// =========================================================================

class FileEditTool {
public:
    // migrated: uses kToolName from types (= TS FILE_EDIT_TOOL_NAME).
    static constexpr std::string_view kName = kToolName;

    static ToolDefinition definition() {
        return ToolDefinition{
            .name = std::string(kName),
            .description = "Performs exact string replacements in files.",
            .input_schema = InputSchema{
                .properties = {
                    SchemaProperty{.name = "file_path", .type = "string",
                        .description = "The absolute path to the file to modify",
                        .required = true,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt},
                    SchemaProperty{.name = "old_string", .type = "string",
                        .description = "The text to replace",
                        .required = true,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt},
                    SchemaProperty{.name = "new_string", .type = "string",
                        .description = "The text to replace it with (must be different from old_string)",
                        .required = true,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt},
                    SchemaProperty{.name = "replace_all", .type = "boolean",
                        .description = "Replace all occurrences of old_string (default false)",
                        .required = false,
                        .default_value = std::nullopt,
                        .enum_values = std::nullopt},
                }
            },
            .permission = ToolPermission::Write,
            .category = "filesystem",
        };
    }

    FileEditTool() = default;

    // --- configuration ---------------------------------------------------
    void set_allowed_directories(std::vector<std::string> dirs) {
        allowed_directories_ = std::move(dirs);
    }
    void set_file_read_cache(utils::FileReadCache* cache) { cache_ = cache; }
    ReadFileState& read_file_state() { return read_state_; }
    const ReadFileState& read_file_state() const { return read_state_; }

    // --- optional hooks (cross-cutting concerns; may be left null) -------
    using FileHistoryHook   = std::function<void(const fs::path&)>;
    using LspNotifyHook     = std::function<void(const fs::path&, std::string_view)>;
    using LogEventHook      = std::function<void(std::string_view)>;
    using DenyRuleFn        = std::function<std::string(const fs::path&)>;
    using ExtraValidationFn = std::function<ValidationOutcome(
        const fs::path&, std::string_view content,
        std::string_view actual_old, std::string_view new_s, bool replace_all)>;
    using GitDiffFn         = std::function<std::optional<GitDiffInfo>(const fs::path&)>;

    void set_file_history_hook(FileHistoryHook h)    { file_history_   = std::move(h); }
    void set_lsp_change_hook(LspNotifyHook h)        { lsp_change_     = std::move(h); }
    void set_lsp_save_hook(LspNotifyHook h)          { lsp_save_       = std::move(h); }
    void set_log_event_hook(LogEventHook h)          { log_event_      = std::move(h); }
    void set_deny_rule_callback(DenyRuleFn f)        { deny_rule_cb_   = std::move(f); }
    void set_extra_validation_callback(ExtraValidationFn f) { extra_valid_cb_ = std::move(f); }
    void set_git_diff_hook(GitDiffFn f)              { git_diff_hook_  = std::move(f); }

    // migrated: prompt() delegates to the full prompt builder.
    std::string prompt() const { return get_edit_tool_description(); }

    // --- permission ------------------------------------------------------
    [[nodiscard]] bool check_permission(const ToolInput& input) const {
        auto parsed = ParsedInput::from_json(input.json());
        if (!parsed) return false;
        return is_path_allowed(parsed->file_path);
    }
    [[nodiscard]] bool is_path_allowed(const fs::path& path) const {
        if (allowed_directories_.empty()) return true;
        std::string abs;
        try { abs = fs::absolute(path).string(); }
        catch (...) { abs = path.string(); }
        for (const auto& d : allowed_directories_) {
            if (abs.starts_with(d)) return true;
        }
        return false;
    }

    // --- inputs-equivalent (dedup) --------------------------------------
    [[nodiscard]] bool inputs_equivalent(std::string_view json_a,
                                         std::string_view json_b) const
    {
        auto a = ParsedInput::from_json(json_a);
        auto b = ParsedInput::from_json(json_b);
        if (!a || !b) return false;
        cc::utils::file_edit::NormalizedFileEditInput na{
            a->file_path, { cc::utils::file_edit::FileEdit{
                a->old_string, a->new_string, a->replace_all } }};
        cc::utils::file_edit::NormalizedFileEditInput nb{
            b->file_path, { cc::utils::file_edit::FileEdit{
                b->old_string, b->new_string, b->replace_all } }};
        return are_file_edits_inputs_equivalent(na, nb, cache_);
    }

    // =====================================================================
    // validate_input — 1:1 with TS FileEditTool.validateInput() branches.
    // Each return site is annotated with `// migrated: <code> <desc>`.
    // =====================================================================

    ValidationOutcome validate_input(const ParsedInput& in) const {
        const fs::path& file_path = in.file_path;
        const std::string& old_s = in.old_string;
        const std::string& new_s = in.new_string;
        const bool replace_all   = in.replace_all;

        // expand path (mirrors TS expandPath call).
        std::string full = expand_path(file_path.string());
        fs::path full_path{full};

        // migrated: errorCode=1 — old_string == new_string
        if (old_s == new_s) {
            return ValidationOutcome::ask(
                "No changes to make: old_string and new_string are exactly the same.",
                ValidationErrorCode::OldEqualsNew);
        }

        // migrated: errorCode=2 — permission deny rule
        if (deny_rule_cb_) {
            if (auto msg = deny_rule_cb_(full_path); !msg.empty()) {
                return ValidationOutcome::ask(
                    "File is in a directory that is denied by your permission settings.",
                    ValidationErrorCode::PermissionDeniedDir);
            }
        }

        // migrated: UNC path guard — skip filesystem ops to prevent NTLM leak.
        std::string full_s = full_path.string();
        if (full_s.starts_with("\\\\") || full_s.starts_with("//")) {
            return ValidationOutcome::ok();
        }

        // migrated: errorCode=10 — MAX_EDIT_FILE_SIZE (1 GiB)
        try {
            std::error_code ec;
            const auto sz = fs::file_size(full_path, ec);
            if (!ec && sz > kMaxEditFileSize) {
                return ValidationOutcome::ask(
                    std::format(
                        "File is too large to edit ({}). Maximum editable file size is 1 GiB.",
                        format_file_size(sz)),
                    ValidationErrorCode::FileTooLarge);
            }
        } catch (...) { /* ENOENT handled below */ }

        // --- read bytes + BOM/encoding detection (mirrors TS readFileBytes) ---
        std::optional<std::string> file_content;
        bool file_exists = false;
        try {
            std::ifstream f(full_path, std::ios::binary);
            if (f) {
                std::ostringstream ss; ss << f.rdbuf();
                std::string raw = ss.str();
                file_exists = true;
                if (raw.size() >= 2 &&
                    static_cast<unsigned char>(raw[0]) == 0xff &&
                    static_cast<unsigned char>(raw[1]) == 0xfe) {
                    // Likely UTF-16 LE — keep raw bytes; match/compare below
                    // will probably fail, which is safer than corrupting.
                    file_content = std::move(raw);
                } else {
                    // CRLF → LF normalisation (mirrors TS replaceAll call).
                    std::string norm;
                    norm.reserve(raw.size());
                    for (size_t i = 0; i < raw.size(); ++i) {
                        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
                            norm += '\n'; ++i;
                        } else {
                            norm += raw[i];
                        }
                    }
                    file_content = std::move(norm);
                }
            }
        } catch (...) {
            file_content = std::nullopt;
        }

        // migrated: file-not-found branch — errorCode=4, or ok if new file
        if (!file_exists || !file_content) {
            if (old_s.empty()) {
                // empty old_string → create new file (valid)
                return ValidationOutcome::ok();
            }
            return ValidationOutcome::ask(
                std::format("File does not exist. Check your current working directory: {}.",
                    get_cwd_safe()),
                ValidationErrorCode::FileNotFound);
        }

        // migrated: errorCode=3 — old_string == '' but file has content
        if (old_s.empty()) {
            if (file_content->find_first_not_of(" \t\n\r\f\v") != std::string::npos) {
                return ValidationOutcome::ask(
                    "Cannot create new file - file already exists.",
                    ValidationErrorCode::FileAlreadyExists);
            }
            return ValidationOutcome::ok();
        }

        // migrated: errorCode=5 — .ipynb → use NotebookEditTool
        std::string lower = full_path.extension().string();
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == ".ipynb") {
            return ValidationOutcome::ask(
                "File is a Jupyter Notebook. Use the NotebookEdit tool to edit this file.",
                ValidationErrorCode::NotebookBlocked);
        }

        // migrated: errorCode=6 — file has not been read yet
        auto last_read = read_state_.get(full_path);
        if (!last_read || last_read->is_partial_view) {
            auto o = ValidationOutcome::ask(
                "File has not been read yet. Read it first before writing to it.",
                ValidationErrorCode::FileNotReadYet);
            o.meta.emplace_back("isFilePathAbsolute",
                full_path.is_absolute() ? "true" : "false");
            return o;
        }

        // migrated: errorCode=7 — modified since read; content-fallback on Windows
        if (last_read) {
            auto last_write = get_file_mtime(full_path);
            if (last_write > last_read->timestamp) {
                const bool is_full_read =
                    !last_read->offset && !last_read->limit;
                const bool content_unchanged =
                    is_full_read && last_read->content &&
                    *last_read->content == *file_content;
                if (!content_unchanged) {
                    return ValidationOutcome::ask(
                        "File has been modified since read, either by the user or by a linter. "
                        "Read it again before attempting to write it.",
                        ValidationErrorCode::FileModifiedSinceRead);
                }
            }
        }

        // migrated: findActualString — handles curly-quote normalisation
        auto actual_old = cc::utils::file_edit::find_actual_string(
            *file_content, old_s);

        // migrated: errorCode=8 — old_string not found
        if (!actual_old) {
            auto o = ValidationOutcome::ask(
                std::format("String to replace not found in file.\nString: {}", old_s),
                ValidationErrorCode::OldStringNotFound);
            o.meta.emplace_back("isFilePathAbsolute",
                full_path.is_absolute() ? "true" : "false");
            return o;
        }

        // migrated: count occurrences
        std::size_t matches = 0;
        {
            const std::string& needle = *actual_old;
            size_t pos = 0;
            while ((pos = file_content->find(needle, pos)) != std::string::npos) {
                ++matches;
                pos += needle.size();
            }
        }

        // migrated: errorCode=9 — N matches but replace_all is false
        if (matches > 1 && !replace_all) {
            auto o = ValidationOutcome::ask(
                std::format(
                    "Found {} matches of the string to replace, but replace_all is false. "
                    "To replace all occurrences, set replace_all to true. "
                    "To replace only one occurrence, please provide more context "
                    "to uniquely identify the instance.\nString: {}",
                    matches, old_s),
                ValidationErrorCode::MultipleMatchesNoReplaceAll);
            o.meta.emplace_back("isFilePathAbsolute",
                full_path.is_absolute() ? "true" : "false");
            o.meta.emplace_back("actualOldString", *actual_old);
            return o;
        }

        // migrated: settings-file validation (generic hook until settings ported)
        if (extra_valid_cb_) {
            auto extra = extra_valid_cb_(full_path, *file_content,
                *actual_old, new_s, replace_all);
            if (!extra.passed) return extra;
        }

        // All OK — pass actualOldString through meta so call() can reuse it.
        auto o = ValidationOutcome::ok();
        o.meta.emplace_back("actualOldString", *actual_old);
        return o;
    }

    // =====================================================================
    // call() — mirrors TS FileEditTool.call() main flow.
    // =====================================================================

    struct CallResult {
        FileEditOutput output;
        std::string error_what; // empty on success
    };

    CallResult call(const ParsedInput& in, bool user_modified = false) {
        const fs::path& file_path = in.file_path;
        const std::string& old_s = in.old_string;
        const std::string& new_s = in.new_string;
        const bool replace_all   = in.replace_all;

        CallResult cr;

        // --- 1. expand path + ensure parent directory ---
        std::string full = expand_path(file_path.string());
        fs::path abs_path{full};
        try {
            auto parent = abs_path.parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                fs::create_directories(parent, ec);
            }
        } catch (...) { /* ignored — write_text_content will fail cleanly */ }

        // --- 2. file-history hook (runs BEFORE staleness check, like TS) ---
        if (file_history_) {
            try { file_history_(abs_path); } catch (...) {}
        }

        // --- 3. critical section: read + staleness check + write ----------
        // (Async yields between read and write would break atomicity, so
        //  this section deliberately uses only synchronous std::filesystem.)
        auto read = cc::utils::file_edit::read_file_for_edit(abs_path);
        if (read.file_exists) {
            auto last_write = get_file_mtime(abs_path);
            auto last_read = read_state_.get(abs_path);
            const bool is_full_read =
                last_read && !last_read->offset && !last_read->limit;
            const bool content_unchanged =
                is_full_read && last_read->content &&
                *last_read->content == read.content;

            if ((!last_read || last_write > last_read->timestamp) &&
                !content_unchanged) {
                cr.error_what = std::string(kFileUnexpectedlyModifiedError);
                return cr;
            }
        }

        // --- 4. actual-match + quote-style preservation -------------------
        std::string actual_old =
            cc::utils::file_edit::find_actual_string(read.content, old_s)
                .value_or(old_s);
        std::string actual_new =
            cc::utils::file_edit::preserve_quote_style(
                old_s, actual_old, new_s);

        // --- 5. generate patch + apply edit ---
        auto patch = cc::utils::file_edit::get_patch_for_edit(
            abs_path.string(), read.content,
            actual_old, actual_new, replace_all);
        if (!patch) {
            cr.error_what = patch.error();
            return cr;
        }
        cc::utils::file_edit::PatchForEditsResult patch_result = std::move(*patch);

        // --- 6. atomic write with encoding + line-ending restoration ------
        auto write_ec = write_text_content(abs_path, patch_result.updated_file,
            read.encoding, read.line_endings);
        if (write_ec) {
            cr.error_what = std::format("Write failed: {}", write_ec.message());
            return cr;
        }

        // --- 7. LSP notifications (didChange + didSave) -------------------
        if (lsp_change_) {
            try { lsp_change_(abs_path, patch_result.updated_file); } catch (...) {}
        }
        if (lsp_save_) {
            try { lsp_save_(abs_path, patch_result.updated_file); } catch (...) {}
        }

        // --- 8. refresh readFileState so stale subsequent writes abort ----
        read_state_.set(abs_path, ReadTimestamp{
            .timestamp      = get_file_mtime(abs_path),
            .offset         = std::nullopt,
            .limit          = std::nullopt,
            .content        = patch_result.updated_file,
            .is_partial_view = false,
        });

        // --- 9. telemetry hooks ------------------------------------------
        if (log_event_) {
            if (abs_path.filename().string() == "CLAUDE.md") {
                log_event_("tengu_write_claudemd");
            }
            log_event_("tengu_edit_string_lengths");
        }

        // --- 10. build FileEditOutput -------------------------------------
        cr.output.file_path     = in.file_path;
        cr.output.old_string    = actual_old;
        cr.output.new_string    = new_s;
        cr.output.original_file = read.content;
        cr.output.user_modified = user_modified;
        cr.output.replace_all   = replace_all;
        for (const auto& h : patch_result.patch) {
            cr.output.structured_patch.push_back(PatchHunk{
                .old_start = h.old_start,
                .old_lines = h.old_lines,
                .new_start = h.new_start,
                .new_lines = h.new_lines,
                .lines     = h.lines,
            });
        }
        if (git_diff_hook_) {
            if (auto info = git_diff_hook_(abs_path)) {
                cr.output.git_diff = std::move(*info);
            }
        }
        return cr;
    }

    // =====================================================================
    // Backwards-compat: thin wrapper around the full pipeline used by
    // hosts that call execute() / ITool::execute() directly.
    // =====================================================================

    [[nodiscard]] Result<ToolResult> execute(const ToolInput& input) {
        auto parsed = ParsedInput::from_json(input.json());
        if (!parsed) {
            return std::unexpected(cc::utils::Error(
                cc::utils::ErrorCode::invalid_argument, parsed.error()));
        }
        auto v = validate_input(*parsed);
        if (!v.passed) {
            return ToolResult::error(std::format(
                "[FileEditTool:{}] {}", static_cast<int>(v.code), v.message));
        }
        auto cr = call(*parsed, false /* user_modified: not available in compat execute() */);
        if (!cr.error_what.empty()) {
            return ToolResult::error(std::format(
                "[FileEditTool] {}", cr.error_what));
        }
        auto content = format_tool_result_block(cr.output, "" /* tool_use_id: not available in compat execute() */);
        return ToolResult::success(content);
    }

private:
    // ---- small helpers -------------------------------------------------
    static std::chrono::system_clock::time_point get_file_mtime(const fs::path& p) {
        std::error_code ec;
        auto ft = fs::last_write_time(p, ec);
        if (ec) return std::chrono::system_clock::time_point{};
        // libc++ on Apple Clang lacks std::chrono::clock_cast — approximate using time delta.
        using FileClock = std::filesystem::file_time_type::clock;
        auto delta = ft - FileClock::now();
        return std::chrono::system_clock::now() +
            std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    }

    static std::string expand_path(std::string_view p) {
        try {
            return cc::utils::path::expand_path(fs::path{std::string(p)}).string();
        } catch (...) {
            std::error_code ec;
            return fs::absolute(std::string(p), ec).string();
        }
    }

    static std::string format_file_size(std::uint64_t bytes) {
        if (bytes < 1024) return std::format("{} B", bytes);
        if (bytes < 1024ULL * 1024)
            return std::format("{:.1f} KiB", bytes / 1024.0);
        if (bytes < 1024ULL * 1024 * 1024)
            return std::format("{:.1f} MiB", bytes / (1024.0 * 1024.0));
        return std::format("{:.2f} GiB", bytes / (1024.0 * 1024.0 * 1024.0));
    }

    static std::string get_cwd_safe() {
        std::error_code ec;
        auto p = fs::current_path(ec);
        if (ec) return ".";
        return p.string();
    }

    // migrated: atomic temp + rename with line-ending restoration.
    // Mirrors TS writeTextContent(file, content, encoding, lineEndings).
    static std::error_code write_text_content(
        const fs::path& p,
        std::string_view content,
        std::string_view /*encoding*/,
        cc::utils::file_edit::LineEndingType le)
    {
        std::string to_write;
        if (le == cc::utils::file_edit::LineEndingType::LF) {
            to_write = std::string(content);
        } else {
            const std::string_view target =
                (le == cc::utils::file_edit::LineEndingType::CRLF) ? "\r\n" : "\r";
            to_write.reserve(content.size());
            for (size_t i = 0; i < content.size(); ++i) {
                if (content[i] == '\n') to_write += target;
                else to_write += content[i];
            }
        }

        std::error_code ec;
        auto tmp = p;
        tmp += ".tmpedit";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return std::make_error_code(std::errc::io_error);
            f.write(to_write.data(),
                    static_cast<std::streamsize>(to_write.size()));
            if (!f) return std::make_error_code(std::errc::io_error);
        }
        fs::rename(tmp, p, ec);
        // Clean up leftover temp on rename failure (best-effort).
        if (ec) { std::error_code _; fs::remove(tmp, _); }
        return ec;
    }

    // ---- fields --------------------------------------------------------
    std::vector<std::string> allowed_directories_;
    ReadFileState read_state_;
    utils::FileReadCache* cache_ = nullptr;

    FileHistoryHook   file_history_;
    LspNotifyHook     lsp_change_;
    LspNotifyHook     lsp_save_;
    LogEventHook      log_event_;
    DenyRuleFn        deny_rule_cb_;
    ExtraValidationFn extra_valid_cb_;
    GitDiffFn         git_diff_hook_;
};

} // namespace cc::tools::file_edit

// =========================================================================
// Public exports + ITool adapter
// =========================================================================

export namespace cc::tools {
    using cc::tools::file_edit::FileEditTool;
    using cc::tools::file_edit::ParsedInput;
    using cc::tools::file_edit::ReadTimestamp;
    using cc::tools::file_edit::ReadFileState;

    /// Factory: wrap FileEditTool as an ITool.
    [[nodiscard]] auto make_file_edit_tool() -> std::unique_ptr<cc::core::ITool> {
        struct Adapter final : cc::core::ITool {
            FileEditTool tool_;
            cc::core::ToolDefinition def_ = FileEditTool::definition();

            const cc::core::ToolDefinition& definition() const override { return def_; }
            std::expected<cc::core::ToolResult, cc::core::Error> execute(
                const cc::core::ToolInput& input) override
            {
                auto result = tool_.execute(input);
                if (result) return std::move(*result);
                return std::unexpected(cc::core::Error::make(
                    cc::core::ErrorCode::ToolExecutionFailed,
                    result.error().format()));
            }
            bool check_permission(const cc::core::ToolInput& input) const override {
                return tool_.check_permission(input);
            }
        };
        return std::make_unique<Adapter>();
    }

    /// Public wrapper for the sed-in-place parser. Delegates to
    /// cc::tools::sed_edit_parser::parse_sed_edit_command and converts
    /// the result into a ParsedInput for FileEditTool consumption.
    [[nodiscard]] inline auto try_parse_sed_in_place(std::string_view cmd) {
        return cc::tools::file_edit::try_parse_sed_in_place(cmd);
    }
}
