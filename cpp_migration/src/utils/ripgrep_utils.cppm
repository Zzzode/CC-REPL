module;
#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module cc.utils.ripgrep_utils;
import cc.utils.bash_execution;

export namespace cc::utils {

namespace fs = std::filesystem;

struct RgMatch {
    fs::path file;
    int line;
    int column;
    std::string content;
};

struct RgOptions {
    bool case_insensitive = false;
    std::optional<std::string> glob;
    std::optional<int> max_count;
    bool regex = true;
};

// Get path to vendored ripgrep binary
fs::path get_rg_path() {
    // Check if rg is vendored alongside the binary
    const char* exe_dir = std::getenv("CLAUDE_CODE_DIR");
    if (exe_dir) {
        fs::path vendored = fs::path(exe_dir) / "vendor" / "ripgrep" / "rg";
        if (fs::exists(vendored)) return vendored;
    }

    // Fallback: check PATH
    const char* path_env = std::getenv("PATH");
    if (path_env) {
        std::string_view path_sv(path_env);
        std::size_t pos = 0;
        while (pos < path_sv.size()) {
            auto colon = path_sv.find(':', pos);
            if (colon == std::string_view::npos) colon = path_sv.size();

            fs::path candidate = fs::path(std::string(path_sv.substr(pos, colon - pos))) / "rg";
            if (fs::exists(candidate)) return candidate;

            pos = colon + 1;
        }
    }

    // Last resort
    return "/usr/local/bin/rg";
}

// Build command-line arguments for ripgrep
namespace detail {
    inline std::string build_rg_command(std::string_view pattern, const fs::path& dir, const RgOptions& opts) {
        std::string cmd = get_rg_path().string();
        cmd += " --json"; // Machine-readable output

        if (opts.case_insensitive) cmd += " -i";
        if (!opts.regex) cmd += " -F"; // Fixed string
        if (opts.max_count) cmd += " -m " + std::to_string(*opts.max_count);
        if (opts.glob) cmd += " -g '" + *opts.glob + "'";

        cmd += " -- '";
        // Escape single quotes in pattern
        for (char c : pattern) {
            if (c == '\'') cmd += "'\\''";
            else cmd += c;
        }
        cmd += "' ";
        cmd += dir.string();

        return cmd;
    }

    inline std::vector<RgMatch> parse_rg_output(const std::string& output) {
        std::vector<RgMatch> matches;

        // Parse JSON lines output
        std::size_t pos = 0;
        while (pos < output.size()) {
            auto line_end = output.find('\n', pos);
            if (line_end == std::string::npos) line_end = output.size();
            std::string_view line(output.data() + pos, line_end - pos);

            // Look for match type entries
            if (line.find("\"type\":\"match\"") != std::string_view::npos) {
                RgMatch match;

                // Extract file path
                auto path_pos = line.find("\"path\":{\"text\":\"");
                if (path_pos != std::string_view::npos) {
                    auto val_start = path_pos + 16;
                    auto val_end = line.find('"', val_start);
                    if (val_end != std::string_view::npos) {
                        match.file = std::string(line.substr(val_start, val_end - val_start));
                    }
                }

                // Extract line number
                auto line_num_pos = line.find("\"line_number\":");
                if (line_num_pos != std::string_view::npos) {
                    auto num_start = line_num_pos + 14;
                    match.line = std::stoi(std::string(line.substr(num_start)));
                }

                // Extract content
                auto text_pos = line.find("\"lines\":{\"text\":\"");
                if (text_pos != std::string_view::npos) {
                    auto val_start = text_pos + 17;
                    auto val_end = line.find('"', val_start);
                    if (val_end != std::string_view::npos) {
                        match.content = std::string(line.substr(val_start, val_end - val_start));
                    }
                }

                match.column = 0;
                matches.push_back(std::move(match));
            }

            pos = line_end + 1;
        }

        return matches;
    }
} // namespace detail

// Search for a pattern using ripgrep
std::vector<RgMatch> rg_search(std::string_view pattern, fs::path dir, RgOptions opts) {
    std::string cmd = detail::build_rg_command(pattern, dir, opts);
    cmd += " 2>/dev/null";

    // Execute ripgrep and capture output
    std::array<char, 4096> buffer;
    std::string output;

    auto pipe_cap = cc::utils::bash::exec_capture(cmd.c_str());
    if (!pipe_cap) return {};
    output = std::move(pipe_cap->output);

    return detail::parse_rg_output(output);
}

// List files using ripgrep (rg --files)
std::vector<fs::path> rg_files(fs::path dir, std::optional<std::string> glob) {
    std::string cmd = get_rg_path().string() + " --files";
    if (glob) cmd += " -g '" + *glob + "'";
    cmd += " " + dir.string() + " 2>/dev/null";

    std::vector<fs::path> files;
    std::array<char, 4096> buffer;

    auto pipe_cap = cc::utils::bash::exec_capture(cmd.c_str());
    if (!pipe_cap) return files;
    std::string output = std::move(pipe_cap->output);

    // Parse newline-separated paths
    std::size_t pos = 0;
    while (pos < output.size()) {
        auto end = output.find('\n', pos);
        if (end == std::string::npos) end = output.size();
        if (end > pos) {
            files.emplace_back(output.substr(pos, end - pos));
        }
        pos = end + 1;
    }

    return files;
}

} // namespace cc::utils
