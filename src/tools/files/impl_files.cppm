/// @file impl_files.cppm
/// @brief Real filesystem primitives used by File{Read,Write,Edit} and
///        Glob / Grep tools in Phase 3.
///
/// Design notes:
///   * Read/Write use <fstream> / POSIX open() for atomic ExclusiveNew.
///   * Edit runs a 2-pass linear scan (count → optionally replace) so the
///     `replace_all=false` semantic of "require exactly 1 match" is precise.
///   * Glob walks <filesystem> recursively, matching leaves with fnmatch(3).
///   * Grep supports literal (string::find) or POSIX ECMAScript <regex>; it
///     skips binary files and honours max_results / max_depth /
///     max_bytes_per_file so deep trees (/proc, node_modules) stay bounded.
module;

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <climits>
#include <cctype>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <fnmatch.h>
#include <system_error>
#include <cerrno>

export module cc.tools.files.impl;

export namespace cc::tools::files::impl {

namespace fs = std::filesystem;

// --------------------------------------------------------------------------
// Read
// --------------------------------------------------------------------------

struct ReadOptions {
    std::string           path;
    std::optional<size_t> max_bytes;
    std::optional<int>    offset_line; // 1-based; nullopt = line 1
    std::optional<int>    limit_lines;
    bool                  binary = false;
};

struct ReadResult {
    bool        ok = false;
    std::string content;
    size_t      bytes_read = 0;
    int         lines_read = 0;
    std::string error;
};

[[nodiscard]] inline ReadResult ReadFile(ReadOptions opts) {
    ReadResult r;
    std::error_code ec;
    auto file_size = fs::file_size(opts.path, ec);
    if (ec) { r.error = "stat failed: " + ec.message(); return r; }

    size_t to_read = static_cast<size_t>(file_size);
    if (opts.max_bytes.has_value() && *opts.max_bytes < to_read) {
        to_read = *opts.max_bytes;
    }

    // Binary gate: read first chunk (up to 8KB), check for NUL when
    // opts.binary==false.  If rejected we return an error immediately.
    std::ifstream in(opts.path, std::ios::binary);
    if (!in) { r.error = "open failed for read"; return r; }

    // Stream into content respecting to_read.
    std::string raw;
    raw.resize(to_read);
    in.read(raw.data(), static_cast<std::streamsize>(to_read));
    const auto got = static_cast<size_t>(in.gcount());
    raw.resize(got);
    r.bytes_read = got;

    if (!opts.binary) {
        const size_t probe = std::min<size_t>(8192, got);
        for (size_t i = 0; i < probe; ++i) {
            if (raw[i] == '\0') {
                r.error = "file appears to be binary (NUL in first 8KB); set binary=true to read";
                return r;
            }
        }
    }

    // If caller asked for line-based slicing, split.
    const int off = opts.offset_line.value_or(1);
    if (opts.offset_line.has_value() || opts.limit_lines.has_value()) {
        std::string out;
        int         line_no   = 0;
        int         included  = 0;
        const int   limit     = opts.limit_lines.value_or(INT32_MAX);
        size_t      start     = 0;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\n') {
                ++line_no;
                const bool take = line_no >= off && included < limit;
                if (take) {
                    out.append(raw, start, i - start + 1);
                    ++included;
                    r.lines_read = included;
                }
                start = i + 1;
                if (included >= limit) break;
            }
        }
        // Final unterminated line.
        if (start < raw.size() && included < limit) {
            ++line_no;
            if (line_no >= off) {
                out.append(raw, start, raw.size() - start);
                ++included;
                r.lines_read = included;
            }
        }
        r.content = std::move(out);
    } else {
        r.content = std::move(raw);
        // Count lines for convenience.
        int cnt = 0;
        for (char c : r.content) if (c == '\n') ++cnt;
        if (!r.content.empty() && r.content.back() != '\n') ++cnt;
        r.lines_read = cnt;
    }

    r.ok = true;
    return r;
}

// --------------------------------------------------------------------------
// Write
// --------------------------------------------------------------------------

enum class WriteMode { Overwrite, Append, ExclusiveNew };

struct WriteOptions {
    std::string path;
    std::string content;
    WriteMode   mode = WriteMode::Overwrite;
    bool        make_parent_dirs = true;
    int         mode_bits = 0644;
};

struct WriteResult {
    bool     ok = false;
    uint64_t bytes_written = 0;
    bool     created_new = false;
    std::string error;
};

[[nodiscard]] inline WriteResult WriteFile(WriteOptions opts) {
    WriteResult r;
    std::error_code ec;

    if (opts.make_parent_dirs) {
        fs::path p{opts.path};
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
            if (ec) { r.error = "mkdirs failed: " + ec.message(); return r; }
        }
    }

    // Check existence BEFORE opening for ExclusiveNew/Overwrite.
    bool existed = fs::exists(opts.path, ec);
    (void)ec;

    if (opts.mode == WriteMode::ExclusiveNew) {
        int fd = ::open(opts.path.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL,
                        static_cast<mode_t>(opts.mode_bits));
        if (fd < 0) {
            r.error = std::string{"open(O_EXCL) failed: "} + std::strerror(errno);
            return r;
        }
        const char* data = opts.content.data();
        size_t      left = opts.content.size();
        while (left > 0) {
            ssize_t w = ::write(fd, data, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                r.error = std::string{"write failed: "} + std::strerror(errno);
                return r;
            }
            data += w;
            left -= static_cast<size_t>(w);
        }
        if (::close(fd) != 0) { r.error = std::strerror(errno); return r; }
        r.bytes_written = opts.content.size();
        r.created_new   = true;
        r.ok = true;
        return r;
    }

    auto fmode = std::ios::binary;
    if (opts.mode == WriteMode::Overwrite) fmode |= std::ios::out | std::ios::trunc;
    else                                   fmode |= std::ios::out | std::ios::app;

    std::ofstream ofs(opts.path, fmode);
    if (!ofs) { r.error = "open failed for write"; return r; }
    ofs.write(opts.content.data(), static_cast<std::streamsize>(opts.content.size()));
    ofs.flush();
    if (!ofs) { r.error = "ostream write failed"; return r; }
    ofs.close();
    r.bytes_written = opts.content.size();
    r.created_new   = !existed;
    r.ok = true;
    return r;
}

// --------------------------------------------------------------------------
// Edit
// --------------------------------------------------------------------------

struct EditOptions {
    std::string path;
    std::string old_string;
    std::string new_string;
    bool        replace_all = false;
    bool        dry_run = false;
};

struct EditResult {
    bool        ok = false;
    int         replacements = 0;
    std::string preview;
    std::string error;
};

[[nodiscard]] inline EditResult EditFile(EditOptions opts) {
    EditResult r;
    if (opts.old_string.empty()) { r.error = "old_string must be non-empty"; return r; }

    // 1. Read entire file (respecting the binary gate like ReadFile).
    ReadOptions ro;
    ro.path = opts.path;
    auto rr = ReadFile(std::move(ro));
    if (!rr.ok) { r.error = std::move(rr.error); return r; }

    const std::string& src = rr.content;
    const std::string& needle = opts.old_string;

    // 2. First pass: count matches.
    std::vector<size_t> match_positions;
    size_t cursor = 0;
    while (true) {
        size_t f = src.find(needle, cursor);
        if (f == std::string::npos) break;
        match_positions.push_back(f);
        cursor = f + needle.size();
        if (!opts.replace_all && match_positions.size() > 1) break;
    }

    if (match_positions.empty()) { r.error = "old_string not found"; return r; }
    if (!opts.replace_all && match_positions.size() != 1) {
        r.error = "expected exactly 1 match, found " + std::to_string(match_positions.size());
        return r;
    }

    // 3. Build final content.
    std::string final_str;
    final_str.reserve(src.size() + match_positions.size() *
                      (opts.new_string.size() > needle.size()
                           ? (opts.new_string.size() - needle.size())
                           : 0));
    size_t last = 0;
    for (size_t pos : match_positions) {
        final_str.append(src, last, pos - last);
        final_str.append(opts.new_string);
        last = pos + needle.size();
    }
    final_str.append(src, last, src.size() - last);
    r.replacements = static_cast<int>(match_positions.size());

    if (opts.dry_run) {
        r.preview = std::move(final_str);
        r.ok = true;
        return r;
    }

    // 4. Write back atomically-ish: write to tmp + rename.
    fs::path p{opts.path};
    std::string tmp_path = opts.path + ".edit-tmp";
    WriteOptions wo;
    wo.path              = tmp_path;
    wo.content           = std::move(final_str);
    wo.mode              = WriteMode::Overwrite;
    wo.make_parent_dirs  = false;
    // try to preserve original mode
    std::error_code ec;
    auto s = fs::status(opts.path, ec);
    wo.mode_bits = ec ? 0644 : static_cast<int>(s.permissions());
    if (wo.mode_bits < 0100) wo.mode_bits = 0644;
    auto wr = WriteFile(std::move(wo));
    if (!wr.ok) { r.error = std::move(wr.error); return r; }

    std::error_code ec2;
    fs::rename(tmp_path, opts.path, ec2);
    if (ec2) {
        fs::remove(tmp_path, ec);
        r.error = "rename failed: " + ec2.message();
        return r;
    }
    r.ok = true;
    return r;
}

// --------------------------------------------------------------------------
// Glob
// --------------------------------------------------------------------------

struct GlobOptions {
    std::string pattern;
    std::string base_dir;
    int         max_results = 1000;
    bool        include_dirs = false;
    bool        follow_symlinks = false;
};

struct GlobResult {
    std::vector<std::string> matches;
    int                      total_scanned = 0;
    std::string              error;
};

[[nodiscard]] inline GlobResult Glob(GlobOptions opts) {
    GlobResult r;
    fs::path base = opts.base_dir.empty() ? fs::current_path() : fs::path{opts.base_dir};
    std::error_code ec;
    if (!fs::exists(base, ec)) { r.error = "base_dir does not exist"; return r; }

    auto match_pattern = [&](const fs::path& p) -> bool {
        std::string name = p.filename().string();
        // If pattern contains ** or /, fnmatch on full relative path too.
        bool has_slash = (opts.pattern.find('/') != std::string::npos);
        if (has_slash) {
            std::error_code ec_rel;
            auto rel = fs::relative(p, base, ec_rel);
            if (!ec_rel) {
                std::string rel_str = rel.string();
                // fnmatch FNM_PATHNAME requires path separators match literally.
                if (::fnmatch(opts.pattern.c_str(), rel_str.c_str(), FNM_PATHNAME) == 0)
                    return true;
            }
        }
        return ::fnmatch(opts.pattern.c_str(), name.c_str(), 0) == 0;
    };

    try {
        auto it_opts = opts.follow_symlinks
            ? fs::directory_options::follow_directory_symlink
            : fs::directory_options::none;
        for (auto it = fs::recursive_directory_iterator(base, it_opts, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) { r.error = "walk error: " + ec.message(); ec.clear(); continue; }
            if (static_cast<int>(r.matches.size()) >= opts.max_results) break;
            ++r.total_scanned;
            const auto& entry = *it;
            bool is_dir = entry.is_directory(ec);
            if (ec) { ec.clear(); continue; }
            if (!opts.include_dirs && is_dir) continue;
            if (match_pattern(entry.path())) {
                r.matches.push_back(entry.path().string());
            }
        }
    } catch (const std::exception& e) {
        if (r.error.empty()) r.error = std::string{"glob exception: "} + e.what();
    }
    return r;
}

// --------------------------------------------------------------------------
// Grep
// --------------------------------------------------------------------------

struct GrepOptions {
    std::string pattern;
    std::string path;
    bool        fixed_strings = false;
    bool        case_sensitive = true;
    bool        include_filename = true;
    int         max_results = 200;
    int         max_bytes_per_file = 2'000'000;
    size_t      max_depth = 20;
};

struct GrepMatch {
    std::string file;
    int         line_no = 0;
    std::string line;
    int         col_start = -1;
};

struct GrepResult {
    std::vector<GrepMatch> matches;
    int                    files_scanned = 0;
    std::string            error;
};

namespace detail {

inline bool looks_binary(std::string_view head) {
    const size_t probe = std::min<size_t>(8192, head.size());
    for (size_t i = 0; i < probe; ++i)
        if (head[i] == '\0') return true;
    return false;
}

inline void scan_text(std::string_view text,
                      const std::string& file,
                      const GrepOptions& opts,
                      std::vector<GrepMatch>& out) {
    const std::string& pat = opts.pattern;
    if (pat.empty()) return;

    // If regex: compile once (ECMAScript).
    std::regex re;
    bool have_re = false;
    if (!opts.fixed_strings) {
        try {
            auto flags = std::regex::ECMAScript;
            if (!opts.case_sensitive) flags |= std::regex::icase;
            re = std::regex(pat.begin(), pat.end(), flags);
            have_re = true;
        } catch (const std::regex_error&) {
            // Fall through to literal scan if regex is malformed.
            have_re = false;
        }
    }

    // Iterate lines.
    int         line_no = 0;
    size_t      start   = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool end_line = (i == text.size()) || (text[i] == '\n');
        if (!end_line) continue;
        ++line_no;
        std::string_view line{text.data() + start, i - start};
        // strip trailing \r
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        bool matched = false;
        int  col     = -1;

        if (opts.fixed_strings || !have_re) {
            // literal find, optional case-insensitive via lowercase copies.
            if (opts.case_sensitive) {
                auto pos = line.find(pat);
                if (pos != std::string_view::npos) { matched = true; col = static_cast<int>(pos) + 1; }
            } else {
                std::string low_line{line};
                std::string low_pat{pat};
                std::transform(low_line.begin(), low_line.end(), low_line.begin(), ::tolower);
                std::transform(low_pat.begin(),  low_pat.end(),  low_pat.begin(),  ::tolower);
                auto pos = low_line.find(low_pat);
                if (pos != std::string::npos) { matched = true; col = static_cast<int>(pos) + 1; }
            }
        } else {
            std::cmatch m;
            std::string line_str{line};
            if (std::regex_search(line_str.c_str(), m, re)) {
                matched = true;
                col = static_cast<int>(m.position()) + 1;
            }
        }

        if (matched) {
            GrepMatch gm;
            if (opts.include_filename) gm.file = file;
            gm.line_no   = line_no;
            gm.line      = std::string{line};
            gm.col_start = col;
            out.push_back(std::move(gm));
            if (static_cast<int>(out.size()) >= opts.max_results) return;
        }
        start = i + 1;
    }
}

} // namespace detail

[[nodiscard]] inline GrepResult Grep(GrepOptions opts) {
    GrepResult r;
    std::error_code ec;
    fs::path root{opts.path};
    if (!fs::exists(root, ec)) { r.error = "path does not exist: " + opts.path; return r; }

    auto collect_files = [&](auto&& self, const fs::path& dir, size_t depth) -> void {
        if (depth > opts.max_depth) return;
        if (static_cast<int>(r.matches.size()) >= opts.max_results) return;
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (static_cast<int>(r.matches.size()) >= opts.max_results) return;
            const auto& entry = *it;
            bool is_dir  = entry.is_directory(ec); if (ec) { ec.clear(); continue; }
            bool is_file = entry.is_regular_file(ec); if (ec) { ec.clear(); continue; }
            if (is_dir) {
                self(self, entry.path(), depth + 1);
                continue;
            }
            if (!is_file) continue;

            // Size cap before we even read.
            auto sz = entry.file_size(ec);
            if (ec) { ec.clear(); continue; }
            if (sz > static_cast<uintmax_t>(opts.max_bytes_per_file)) continue;

            std::ifstream in(entry.path(), std::ios::binary);
            if (!in) continue;
            std::string buf(static_cast<size_t>(sz), '\0');
            in.read(buf.data(), static_cast<std::streamsize>(sz));
            buf.resize(static_cast<size_t>(in.gcount()));
            ++r.files_scanned;

            if (detail::looks_binary(buf)) continue;
            detail::scan_text(buf, entry.path().string(), opts, r.matches);
        }
    };

    auto process_file = [&](const fs::path& p) {
        auto sz = fs::file_size(p, ec);
        if (ec) { r.error = "stat failed: " + ec.message(); return; }
        if (sz > static_cast<uintmax_t>(opts.max_bytes_per_file)) return;
        std::ifstream in(p, std::ios::binary);
        if (!in) return;
        std::string buf(static_cast<size_t>(sz), '\0');
        in.read(buf.data(), static_cast<std::streamsize>(sz));
        buf.resize(static_cast<size_t>(in.gcount()));
        ++r.files_scanned;
        if (detail::looks_binary(buf)) return;
        detail::scan_text(buf, p.string(), opts, r.matches);
    };

    bool is_dir = fs::is_directory(root, ec);
    bool is_file = fs::is_regular_file(root, ec);
    if (is_file) {
        process_file(root);
    } else if (is_dir) {
        collect_files(collect_files, root, 0);
    } else {
        r.error = "path is neither file nor directory";
    }
    return r;
}

} // namespace cc::tools::files::impl
