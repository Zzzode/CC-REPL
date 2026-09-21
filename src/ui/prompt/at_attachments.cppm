/// @file at_attachments.cppm
/// @brief AT-02: materialize @-mention file references into message content
/// blocks at submit time, so the model actually sees file contents (not the
/// literal "@path" string the C++ port used to send). Faithful to the @-file
/// path of TS utils/attachments.ts.
///
/// MVP scope: text files with a size guard plus quoted/relative/~/ path
/// expansion (covers AT-03 quoted and AT-04 path-prefix). Image (ImageBlock),
/// PDF (DocumentBlock), line-range (:a-b) and deny-list handling layer on top
/// of this same materialize_at_mentions interface later.
module;

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

export module cc.ui.prompt.at_attachments;

import cc.types.types;

export namespace cc::ui::prompt::at_attachments {

namespace fs = std::filesystem;
namespace core = cc::core;

/// Maximum bytes of a single text file to inline as an attachment. Larger
/// files are referenced-but-not-inlined (TS uses a token budget; a byte guard
/// is a faithful-enough proxy for the MVP).
inline constexpr std::size_t kMaxInlineFileBytes = 256 * 1024;

/// AT-04: expand a raw @-mention path (~, ./, ../, /) against cwd. Returns
/// nullopt for empty/unresolvable input.
[[nodiscard]] inline std::optional<fs::path> expand_at_path(
    std::string_view raw, const fs::path& cwd) {
    if (raw.empty()) return std::nullopt;
    std::string s(raw);
    if (s.starts_with('~')) {
        const char* home = std::getenv("HOME");
        if (!home) return std::nullopt;
        s = std::string(home) + s.substr(1);
        return fs::path(s);
    }
    const fs::path p(s);
    return p.is_absolute() ? p : (cwd / p);
}

/// Result of materializing the @-mentions in a user input.
struct MaterializeResult {
    /// The original input text (mentions are kept as references; the model
    /// correlates them with the attached content blocks).
    std::string text;
    /// One content block per readable file mention, in order of appearance.
    std::vector<core::ContentBlock> blocks;
};

// AT-10/AT-11: defined in at_attachments_impl.cpp (impl unit) to keep the heavy
// cc.tools.agent_runtime + cc.tools.mcp imports out of this module's BMI (clang
// 2GB source-location budget — app.cppm transitively imports this module).
[[nodiscard]] std::optional<core::ContentBlock> try_attach_agent(
    std::string_view name, const fs::path& cwd);
[[nodiscard]] std::optional<core::ContentBlock> try_attach_mcp_resource(
    std::string_view server, std::string_view uri);

/// AT-02: scan @a input for file mentions (@path, @"quoted path"), read each
/// readable file, and return TextBlock attachments. Non-file mentions (agents,
/// MCP server:uri) naturally fail the regular-file check and are skipped, so
/// this stays out of the way of AT-10/AT-11.
[[nodiscard]] inline MaterializeResult materialize_at_mentions(
    std::string_view input, std::string_view cwd_text) {
    MaterializeResult result;
    result.text = std::string(input);
    if (input.empty()) return result;

    const fs::path cwd = cwd_text.empty()
        ? fs::current_path()
        : fs::path(std::string(cwd_text));

    std::set<fs::path> already_attached;  // dedupe within one submit

    std::size_t i = 0;
    const std::size_t n = input.size();
    while (i < n) {
        const std::size_t at = input.find('@', i);
        if (at == std::string_view::npos) break;
        const std::size_t p = at + 1;

        std::string raw_path;
        if (p < n && input[p] == '"') {
            // AT-03: quoted path @"...". If unterminated, stop scanning.
            const std::size_t close = input.find('"', p + 1);
            if (close == std::string_view::npos) break;
            raw_path = std::string(input.substr(p + 1, close - p - 1));
            i = close + 1;
        } else {
            const std::size_t end = input.find_first_of(" \t\r\n", p);
            const std::size_t token_end = (end == std::string_view::npos) ? n : end;
            raw_path = std::string(input.substr(p, token_end - p));
            i = token_end;
        }
        if (raw_path.empty()) continue;

        const auto resolved = expand_at_path(raw_path, cwd);
        if (!resolved) continue;

        // AT-10: agent mention (impl in at_attachments_impl.cpp).
        if (auto agent_block = try_attach_agent(raw_path, cwd)) {
            result.blocks.push_back(*std::move(agent_block));
            continue;
        }
        // AT-11: MCP resource mention — @server:uri (impl in at_attachments_impl.cpp;
        // pre-validated against listed resources so submit can't block on an
        // unknown/slow server). A token with a colon never falls through to file.
        if (const auto colon = raw_path.find(':');
            colon != std::string::npos && colon > 0 && colon + 1 < raw_path.size()) {
            if (auto mcp_block = try_attach_mcp_resource(
                    raw_path.substr(0, colon), raw_path.substr(colon + 1))) {
                result.blocks.push_back(*std::move(mcp_block));
            }
            continue;
        }

        std::error_code ec;
        if (!fs::is_regular_file(*resolved, ec)) continue;

        const fs::path abs = fs::canonical(*resolved, ec);
        if (ec) continue;
        if (already_attached.contains(abs)) continue;
        already_attached.insert(abs);

        std::string rel = fs::relative(abs, cwd, ec).string();
        if (ec || rel.empty()) rel = abs.string();

        const auto size = fs::file_size(abs, ec);
        if (ec) continue;
        if (size > kMaxInlineFileBytes) {
            result.blocks.push_back(core::ContentBlock{core::TextBlock{
                std::format("The user attached the file `{}` but it is too large "
                            "to inline ({} bytes > {} limit).",
                            rel, size, kMaxInlineFileBytes)}});
            continue;
        }

        std::ifstream f(abs, std::ios::binary);
        if (!f) continue;
        std::ostringstream ss;
        ss << f.rdbuf();

        result.blocks.push_back(core::ContentBlock{core::TextBlock{
            std::format("The user attached this file:\n<file path=\"{}\">\n{}\n"
                        "</file>",
                        rel, ss.str())}});
    }
    return result;
}

}  // namespace cc::ui::prompt::at_attachments
