// C++23 Module: Unified diff parsing, change tracking, and patch generation for IDE integration
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>

export module cc.hooks.diff_data;


export namespace cc::hooks {

// diff 中单行的类型
enum class DiffLineType {
    context,    // 上下文行（未修改）
    added,      // 新增行
    deleted,    // 删除行
};

// diff 中的一行
struct DiffLine {
    DiffLineType type{DiffLineType::context};
    std::string content;

    [[nodiscard]] auto prefix() const -> char {
        switch (type) {
            case DiffLineType::context: return ' ';
            case DiffLineType::added:   return '+';
            case DiffLineType::deleted: return '-';
        }
        return ' ';
    }
};

// 一个 diff hunk（变更块）
struct DiffHunk {
    std::uint32_t old_start{0};   // 原文件起始行号
    std::uint32_t old_count{0};   // 原文件行数
    std::uint32_t new_start{0};   // 新文件起始行号
    std::uint32_t new_count{0};   // 新文件行数
    std::vector<DiffLine> lines;

    // 生成 hunk header（@@ -old_start,old_count +new_start,new_count @@）
    [[nodiscard]] auto header() const -> std::string {
        return std::format("@@ -{},{} +{},{} @@", old_start, old_count, new_start, new_count);
    }

    // 统计新增和删除行数
    [[nodiscard]] auto lines_added() const -> std::size_t {
        return std::count_if(lines.begin(), lines.end(),
            [](const auto& l) { return l.type == DiffLineType::added; });
    }
    [[nodiscard]] auto lines_deleted() const -> std::size_t {
        return std::count_if(lines.begin(), lines.end(),
            [](const auto& l) { return l.type == DiffLineType::deleted; });
    }
};

// 文件变更状态
enum class FileStatus { added, modified, deleted, renamed, copied };

// 单个文件的 diff
struct FileDiff {
    std::string path;              // 文件路径
    std::string old_path;          // 重命名前的路径（仅 renamed 状态）
    FileStatus status{FileStatus::modified};
    std::vector<DiffHunk> hunks;
    bool is_binary{false};

    // 总新增行数
    [[nodiscard]] auto total_additions() const -> std::size_t {
        std::size_t total = 0;
        for (const auto& hunk : hunks) total += hunk.lines_added();
        return total;
    }
    // 总删除行数
    [[nodiscard]] auto total_deletions() const -> std::size_t {
        std::size_t total = 0;
        for (const auto& hunk : hunks) total += hunk.lines_deleted();
        return total;
    }
};

// diff 统计摘要
struct DiffSummary {
    std::size_t files_added{0};
    std::size_t files_modified{0};
    std::size_t files_deleted{0};
    std::size_t lines_added{0};
    std::size_t lines_deleted{0};

    // 格式化为可读字符串
    [[nodiscard]] auto format() const -> std::string {
        return std::format("{} file(s) changed, {} insertion(s)(+), {} deletion(s)(-)",
            files_added + files_modified + files_deleted, lines_added, lines_deleted);
    }
};

// 每个 turn（对话轮次）的 diff 记录
struct TurnDiff {
    std::string turn_id;
    std::vector<FileDiff> file_diffs;
    DiffSummary summary;
    std::chrono::system_clock::time_point timestamp;
};

// DiffDataHook: 解析和管理 diff 数据
class DiffDataHook {
public:
    DiffDataHook() = default;

    /**
     * 解析 unified diff 格式文本，返回文件级 diff 列表。
     * 支持标准的 git diff 输出格式。
     */
    [[nodiscard]] auto parse_unified_diff(std::string_view text)
        -> std::vector<FileDiff> {
        std::vector<FileDiff> results;
        auto lines = split_lines(text);

        std::size_t i = 0;
        while (i < lines.size()) {
            // 查找 "diff --git" 或 "---" 行
            if (lines[i].starts_with("diff --git")) {
                auto file_diff = parse_file_diff(lines, i);
                if (file_diff) {
                    results.push_back(std::move(*file_diff));
                }
            } else if (lines[i].starts_with("--- ") && i + 1 < lines.size() &&
                       lines[i + 1].starts_with("+++ ")) {
                auto file_diff = parse_plain_diff(lines, i);
                if (file_diff) {
                    results.push_back(std::move(*file_diff));
                }
            } else {
                ++i;
            }
        }
        return results;
    }

    // 记录某个 turn 的 diff
    auto record_turn_diff(std::string_view turn_id, std::vector<FileDiff> diffs) -> void {
        auto summary = compute_summary(diffs);
        turn_diffs_.push_back(TurnDiff{
            .turn_id = std::string(turn_id),
            .file_diffs = std::move(diffs),
            .summary = summary,
            .timestamp = std::chrono::system_clock::now()
        });

        // 更新文件索引
        for (const auto& fd : turn_diffs_.back().file_diffs) {
            file_index_[fd.path] = turn_diffs_.size() - 1;
        }
    }

    // 获取所有 turn 的 diff
    [[nodiscard]] auto get_turn_diffs() const -> std::span<const TurnDiff> {
        return turn_diffs_;
    }

    // 计算全局摘要（所有 turn 的汇总）
    [[nodiscard]] auto get_summary() const -> DiffSummary {
        DiffSummary total;
        for (const auto& td : turn_diffs_) {
            total.files_added += td.summary.files_added;
            total.files_modified += td.summary.files_modified;
            total.files_deleted += td.summary.files_deleted;
            total.lines_added += td.summary.lines_added;
            total.lines_deleted += td.summary.lines_deleted;
        }
        return total;
    }

    // 获取特定文件的最新 diff
    [[nodiscard]] auto get_file_diff(std::string_view path) const -> std::optional<FileDiff> {
        auto it = file_index_.find(std::string(path));
        if (it == file_index_.end()) return std::nullopt;

        auto turn_idx = it->second;
        if (turn_idx >= turn_diffs_.size()) return std::nullopt;

        for (const auto& fd : turn_diffs_[turn_idx].file_diffs) {
            if (fd.path == path) return fd;
        }
        return std::nullopt;
    }

    // 从 FileDiff 列表生成 unified diff 格式的 patch 文本
    [[nodiscard]] auto generate_patch(std::span<const FileDiff> diffs) const -> std::string {
        std::string patch;
        for (const auto& fd : diffs) {
            // diff header
            if (fd.status == FileStatus::renamed) {
                patch += std::format("diff --git a/{} b/{}\n", fd.old_path, fd.path);
                patch += std::format("rename from {}\nrename to {}\n", fd.old_path, fd.path);
            } else {
                patch += std::format("diff --git a/{} b/{}\n", fd.path, fd.path);
            }

            if (fd.is_binary) {
                patch += "Binary files differ\n";
                continue;
            }

            // --- / +++ header
            if (fd.status == FileStatus::added) {
                patch += "--- /dev/null\n";
                patch += std::format("+++ b/{}\n", fd.path);
            } else if (fd.status == FileStatus::deleted) {
                patch += std::format("--- a/{}\n", fd.path);
                patch += "+++ /dev/null\n";
            } else {
                patch += std::format("--- a/{}\n", fd.path);
                patch += std::format("+++ b/{}\n", fd.path);
            }

            // hunks
            for (const auto& hunk : fd.hunks) {
                patch += hunk.header() + "\n";
                for (const auto& line : hunk.lines) {
                    patch += line.prefix();
                    patch += line.content;
                    patch += "\n";
                }
            }
        }
        return patch;
    }

    // 清除所有记录
    auto clear() -> void {
        turn_diffs_.clear();
        file_index_.clear();
    }

    // 获取所有被修改的文件路径
    [[nodiscard]] auto changed_files() const -> std::vector<std::string> {
        std::vector<std::string> files;
        files.reserve(file_index_.size());
        for (const auto& [path, _] : file_index_) {
            files.push_back(path);
        }
        std::sort(files.begin(), files.end());
        return files;
    }

private:
    std::vector<TurnDiff> turn_diffs_;
    std::unordered_map<std::string, std::size_t> file_index_; // path -> turn index

    // 按行分割文本
    [[nodiscard]] static auto split_lines(std::string_view text) -> std::vector<std::string_view> {
        std::vector<std::string_view> lines;
        std::size_t start = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            if (end == std::string_view::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return lines;
    }

    // 解析 git diff 格式的文件级 diff
    [[nodiscard]] auto parse_file_diff(const std::vector<std::string_view>& lines,
                                        std::size_t& i) -> std::optional<FileDiff> {
        FileDiff fd;
        // 从 "diff --git a/path b/path" 提取路径
        auto line = lines[i];
        auto a_pos = line.find("a/");
        auto b_pos = line.find("b/", a_pos);
        if (a_pos == std::string_view::npos || b_pos == std::string_view::npos) {
            ++i;
            return std::nullopt;
        }
        fd.path = std::string(line.substr(b_pos + 2));
        ++i;

        // 解析元数据行（new file, deleted file, rename, binary）
        while (i < lines.size() && !lines[i].starts_with("---") &&
               !lines[i].starts_with("diff --git") && !lines[i].starts_with("@@")) {
            if (lines[i].starts_with("new file")) fd.status = FileStatus::added;
            else if (lines[i].starts_with("deleted file")) fd.status = FileStatus::deleted;
            else if (lines[i].starts_with("rename from")) {
                fd.status = FileStatus::renamed;
                fd.old_path = std::string(lines[i].substr(12));
            }
            else if (lines[i].find("Binary") != std::string_view::npos) fd.is_binary = true;
            ++i;
        }

        // 跳过 --- 和 +++ 行
        if (i < lines.size() && lines[i].starts_with("---")) ++i;
        if (i < lines.size() && lines[i].starts_with("+++")) ++i;

        // 解析 hunks
        while (i < lines.size() && lines[i].starts_with("@@")) {
            auto hunk = parse_hunk(lines, i);
            if (hunk) fd.hunks.push_back(std::move(*hunk));
        }

        return fd;
    }

    // 解析非 git 格式的 plain diff
    [[nodiscard]] auto parse_plain_diff(const std::vector<std::string_view>& lines,
                                         std::size_t& i) -> std::optional<FileDiff> {
        FileDiff fd;
        // --- a/path
        auto old_line = lines[i].substr(4);
        if (old_line.starts_with("a/")) old_line = old_line.substr(2);
        ++i;
        // +++ b/path
        auto new_line = lines[i].substr(4);
        if (new_line.starts_with("b/")) new_line = new_line.substr(2);
        fd.path = std::string(new_line);
        ++i;

        if (old_line == "/dev/null") fd.status = FileStatus::added;
        else if (new_line == "/dev/null") fd.status = FileStatus::deleted;

        while (i < lines.size() && lines[i].starts_with("@@")) {
            auto hunk = parse_hunk(lines, i);
            if (hunk) fd.hunks.push_back(std::move(*hunk));
        }
        return fd;
    }

    // 解析单个 hunk
    [[nodiscard]] auto parse_hunk(const std::vector<std::string_view>& lines,
                                   std::size_t& i) -> std::optional<DiffHunk> {
        if (!lines[i].starts_with("@@")) return std::nullopt;

        DiffHunk hunk;
        // 解析 @@ -old_start,old_count +new_start,new_count @@
        auto header = lines[i];
        parse_hunk_header(header, hunk);
        ++i;

        // 解析行内容
        while (i < lines.size()) {
            if (lines[i].starts_with("@@") || lines[i].starts_with("diff --git")) break;
            if (lines[i].empty()) { ++i; continue; }

            DiffLine dl;
            char prefix = lines[i][0];
            switch (prefix) {
                case '+': dl.type = DiffLineType::added; break;
                case '-': dl.type = DiffLineType::deleted; break;
                case ' ': dl.type = DiffLineType::context; break;
                default:  dl.type = DiffLineType::context; break;
            }
            dl.content = std::string(lines[i].substr(1));
            hunk.lines.push_back(std::move(dl));
            ++i;
        }
        return hunk;
    }

    // 解析 hunk header 中的行号信息
    static auto parse_hunk_header(std::string_view header, DiffHunk& hunk) -> void {
        // @@ -1,5 +1,7 @@
        auto minus_pos = header.find('-');
        auto plus_pos = header.find('+');
        if (minus_pos == std::string_view::npos || plus_pos == std::string_view::npos) return;

        auto parse_range = [](std::string_view s, std::uint32_t& start, std::uint32_t& count) {
            auto comma = s.find(',');
            if (comma != std::string_view::npos) {
                start = parse_uint(s.substr(0, comma));
                count = parse_uint(s.substr(comma + 1));
            } else {
                start = parse_uint(s);
                count = 1;
            }
        };

        auto old_range = header.substr(minus_pos + 1, plus_pos - minus_pos - 2);
        auto space_pos = old_range.find(' ');
        if (space_pos != std::string_view::npos) old_range = old_range.substr(0, space_pos);
        parse_range(old_range, hunk.old_start, hunk.old_count);

        auto new_end = header.find(' ', plus_pos + 1);
        auto new_range = header.substr(plus_pos + 1,
            new_end != std::string_view::npos ? new_end - plus_pos - 1 : std::string_view::npos);
        parse_range(new_range, hunk.new_start, hunk.new_count);
    }

    // 简单的无符号整数解析
    [[nodiscard]] static auto parse_uint(std::string_view s) -> std::uint32_t {
        std::uint32_t result = 0;
        for (char c : s) {
            if (c >= '0' && c <= '9') {
                result = result * 10 + (c - '0');
            } else {
                break;
            }
        }
        return result;
    }

    // 计算一组 FileDiff 的汇总统计
    [[nodiscard]] static auto compute_summary(const std::vector<FileDiff>& diffs) -> DiffSummary {
        DiffSummary summary;
        for (const auto& fd : diffs) {
            switch (fd.status) {
                case FileStatus::added:   ++summary.files_added; break;
                case FileStatus::deleted: ++summary.files_deleted; break;
                default:                  ++summary.files_modified; break;
            }
            summary.lines_added += fd.total_additions();
            summary.lines_deleted += fd.total_deletions();
        }
        return summary;
    }
};

} // namespace cc::hooks
