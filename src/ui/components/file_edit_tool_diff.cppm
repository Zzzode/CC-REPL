/// @file file_edit_tool_diff.cppm
/// @brief Faithful port of FileEditToolDiff.tsx — renders a preview of a file
///        edit operation with structured diff hunks, word-level highlighting,
///        a dashed top/bottom frame, and ellipsis separators between hunks.
///
/// Visual structure (matching TS):
///   ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄  (dashed separator, subtle color)
///   @@ -10,7 +10,7 @@ function foo() {      (hunk header, cyan)
///    function foo() {                      (context line, dim)
///   -   old_code();                        (removed line, red bg)
///   +   new_code();                        (added line, green bg)
///    }                                     (context line)
///   ...                                    (ellipsis separator, dim)
///   @@ -42,3 +42,5 @@ function bar() {      (hunk header)
///   ...
///   ┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄┄  (dashed separator)
///
/// The component corresponds to the React component in
///   src/components/FileEditToolDiff.tsx
/// which composes:
///   - DiffFrame         (dashed border wrapper)
///   - StructuredDiffList (hunks + "..." separators)
///   - StructuredDiff    (per-hunk color diff)
// ───────────────────────────────────────────────────────────────────────────────
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <expected>
#include <format>
#include <algorithm>
#include <cstdint>

#include <ftxui/dom/elements.hpp>

export module cc.ui.components.file_edit_tool_diff;

import cc.utils.file_edit;
import cc.utils.string_utils;
import cc.ui.structured_diff;

export namespace cc::ui::components::file_edit_tool_diff {

using namespace ftxui;
namespace sd = ::cc::ui::structured_diff;
namespace fe = ::cc::utils::file_edit;

// ─── Props / types ───────────────────────────────────────────────────────────

/// Input props for FileEditToolDiff — matches TS FileEditToolDiffProps.
struct FileEditToolDiffProps {
    std::string file_path;
    std::vector<fe::FileEdit> edits;
};

/// Computed diff data ready for rendering — matches TS DiffData.
struct DiffData {
    std::vector<sd::StructuredPatchHunk> patch;
    std::optional<std::string> first_line;
    std::optional<std::string> file_content;
};

/// Subtle border color matching TS `subtle` theme token
/// (rgb(175,175,175) in dark theme — mapped to ANSI gray-light).
const Color kSubtleColor = Color::GrayLight;

// ─── DiffFrame ───────────────────────────────────────────────────────────────
/// Wraps content in a dashed top/bottom frame with subtle color.
/// Mirrors TS:
///   <Box borderColor="subtle" borderStyle="dashed" borderLeft={false} borderRight={false}>
///     {placeholder ? <Text dimColor>…</Text> : children}
///   </Box>
[[nodiscard]] Element diff_frame(Element content, bool placeholder = false) {
    auto top = separatorDashed() | color(kSubtleColor);
    auto bot = separatorDashed() | color(kSubtleColor);

    if (placeholder) {
        content = text("…") | dim;
    }

    return vbox({
        top,
        std::move(content),
        bot,
    });
}

// ─── StructuredDiffList ──────────────────────────────────────────────────────
/// Renders a list of hunks with dimmed "..." separators between them.
/// Mirrors TS StructuredDiffList which uses intersperse(hunks, …) with
/// <Text dimColor>...</Text> separators.
[[nodiscard]] Element structured_diff_list(
    const std::vector<sd::StructuredPatchHunk>& hunks,
    const sd::DiffSyntaxTheme& theme,
    [[maybe_unused]] std::string_view file_path,
    bool show_line_numbers = true)
{
    if (hunks.empty()) {
        return text("  (no changes)") | dim;
    }

    // Compute gutter width across all hunks for consistent alignment.
    int max_line = 0;
    for (const auto& hunk : hunks) {
        for (const auto& line : hunk.lines) {
            if (line.new_line_num) max_line = std::max(max_line, *line.new_line_num);
            if (line.old_line_num) max_line = std::max(max_line, *line.old_line_num);
        }
    }
    int gutter_width = std::max(3, static_cast<int>(std::format("{}", max_line).size()));

    Elements elements;
    for (size_t i = 0; i < hunks.size(); ++i) {
        const auto& hunk = hunks[i];

        // Hunk header
        elements.push_back(sd::RenderHunkHeader(hunk, theme));

        // Hunk lines
        for (const auto& line : hunk.lines) {
            elements.push_back(sd::RenderStructuredDiffLine(
                line, theme, show_line_numbers, gutter_width, /*selected=*/false));
        }

        // Ellipsis separator between hunks
        if (i + 1 < hunks.size()) {
            elements.push_back(text("...") | dim);
        }
    }

    return vbox(std::move(elements));
}

// ─── Helpers: edit normalization (mirrors TS normalizeEdit) ──────────────────

/// Normalize an edit: find the actual old_string in file content (handling
/// curly-quote normalization etc.) and preserve quote style in new_string.
/// Mirrors TS normalizeEdit() in FileEditToolDiff.tsx.
[[nodiscard]] inline fe::FileEdit normalize_edit(
    std::string_view file_content,
    const fe::FileEdit& edit)
{
    std::string actual_old = fe::find_actual_string(file_content, edit.old_string)
        .value_or(std::string(edit.old_string));
    std::string actual_new = fe::preserve_quote_style(
        edit.old_string, actual_old, edit.new_string);
    return {
        .old_string = std::move(actual_old),
        .new_string = std::move(actual_new),
        .replace_all = edit.replace_all,
    };
}

// ─── Diff data computation ───────────────────────────────────────────────────

/// Compute structured patch hunks from file content + edits.
/// Mirrors the core of TS loadDiffData() — the synchronous computation part.
/// Returns the structured diff hunks with word-level annotations, plus the
/// first line of the file for shebang-based language detection.
[[nodiscard]] inline std::expected<DiffData, std::string> compute_diff_data(
    [[maybe_unused]] std::string_view file_path,
    std::string_view file_content,
    const std::vector<fe::FileEdit>& edits)
{
    if (edits.empty()) {
        return std::unexpected("No edits provided");
    }

    // Normalize each edit (curly quotes, etc.)
    std::vector<fe::FileEdit> normalized;
    normalized.reserve(edits.size());
    for (const auto& e : edits) {
        normalized.push_back(normalize_edit(file_content, e));
    }

    // Compute patch
    auto result = fe::get_patch_for_edits(file_path, file_content, normalized);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }

    // Convert utils::file_edit::PatchHunk -> structured_diff::StructuredPatchHunk
    // and annotate with word-level changes.
    std::vector<sd::StructuredPatchHunk> sd_hunks;
    for (const auto& ph : result->patch) {
        sd::StructuredPatchHunk sh;
        sh.old_start = ph.old_start;
        sh.old_lines = ph.old_lines;
        sh.new_start = ph.new_start;
        sh.new_lines = ph.new_lines;
        // header: intentionally left empty (TS uses StructuredDiff which
        // generates the @@ line from start/line counts — we do the same via
        // RenderHunkHeader which builds it from old_start/old_lines/etc.)

        int ol = ph.old_start, nl = ph.new_start;
        for (const auto& l : ph.lines) {
            if (l.empty()) continue;
            sd::StructuredDiffLine sdl;
            char prefix = l[0];
            std::string content = l.size() > 1 ? l.substr(1) : "";

            if (prefix == '+') {
                sdl.type = sd::StructuredDiffLine::Type::Added;
                sdl.new_line_num = nl++;
            } else if (prefix == '-') {
                sdl.type = sd::StructuredDiffLine::Type::Removed;
                sdl.old_line_num = ol++;
            } else {
                sdl.type = sd::StructuredDiffLine::Type::Context;
                sdl.old_line_num = ol++;
                sdl.new_line_num = nl++;
            }
            sdl.content = std::move(content);
            sh.lines.push_back(std::move(sdl));
        }
        sd_hunks.push_back(std::move(sh));
    }

    // Annotate with word-level changes
    sd::annotate_word_changes(sd_hunks);

    // First line for language detection (shebang, etc.)
    std::optional<std::string> first_line;
    if (!file_content.empty()) {
        first_line = std::string(cc::utils::first_line_of(file_content));
    }

    return DiffData{
        .patch = std::move(sd_hunks),
        .first_line = std::move(first_line),
        .file_content = std::string(file_content),
    };
}

// ─── Main entry point: render from file content + edits ─────────────────────

/// Render the full FileEditToolDiff component from raw file content + edits.
/// This is the synchronous equivalent of TS FileEditToolDiff (without Suspense).
///
/// @param props        file_path + edits to show
/// @param file_content current content of the file
/// @param terminal_width available terminal width (for line wrapping)
/// @param placeholder  if true, show the dimmed "…" placeholder instead
/// @param theme        optional syntax theme override
[[nodiscard]] inline Element render_file_edit_tool_diff(
    const FileEditToolDiffProps& props,
    std::string_view file_content,
    [[maybe_unused]] int terminal_width,
    bool placeholder = false,
    std::optional<sd::DiffSyntaxTheme> theme = std::nullopt)
{
    if (placeholder) {
        return diff_frame(text(""), /*placeholder=*/true);
    }

    auto data = compute_diff_data(props.file_path, file_content, props.edits);
    if (!data.has_value()) {
        // Fallback: show error message inside frame
        return diff_frame(text("  " + data.error()) | color(Color::Red) | dim);
    }

    auto theme_val = theme.value_or(sd::default_theme());
    auto body = structured_diff_list(data->patch, theme_val, props.file_path);

    return diff_frame(std::move(body));
}

// ─── Entry point: render from pre-computed hunks ────────────────────────────

/// Render the component from pre-computed structured hunks (e.g. when the
/// caller already has a patch and wants to avoid recomputation).
/// Mirrors the path where FileEditToolUpdatedMessage passes structuredPatch
/// directly to StructuredDiffList.
[[nodiscard]] inline Element render_file_edit_diff_from_hunks(
    [[maybe_unused]] std::string_view file_path,
    const std::vector<sd::StructuredPatchHunk>& hunks,
    std::optional<sd::DiffSyntaxTheme> theme = std::nullopt)
{
    auto theme_val = theme.value_or(sd::default_theme());
    auto body = structured_diff_list(hunks, theme_val, file_path);
    return diff_frame(std::move(body));
}

} // namespace cc::ui::components::file_edit_tool_diff
