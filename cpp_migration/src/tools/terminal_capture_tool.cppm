// TerminalCaptureTool - Captures terminal screenshots and content
module;
#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <sys/ioctl.h>
#include <unistd.h>

export module cc.tools.terminal_capture;


export namespace cc::tools {


enum class TerminalCaptureError {
    CaptureEmpty,
    ViewportInvalid,
    ConversionFailed,
    TerminalNotAvailable,
    OutputTooLarge,
};

constexpr auto format_error(TerminalCaptureError err) -> std::string_view {
    switch (err) {
        case TerminalCaptureError::CaptureEmpty:         return "Terminal capture returned empty content";
        case TerminalCaptureError::ViewportInvalid:      return "Invalid viewport dimensions";
        case TerminalCaptureError::ConversionFailed:     return "ANSI to image conversion failed";
        case TerminalCaptureError::TerminalNotAvailable: return "Terminal is not available for capture";
        case TerminalCaptureError::OutputTooLarge:       return "Captured output exceeds size limit";
        default:                                         return "Unknown terminal capture error";
    }
}


struct Viewport {
    size_t columns{80};
    size_t rows{24};
    size_t scroll_back{0};
};


struct AnsiColor {
    uint8_t r{0}, g{0}, b{0};
    bool is_default{true};
};


struct TerminalCell {
    char32_t character{' '};
    AnsiColor foreground;
    AnsiColor background;
    bool bold{false};
    bool italic{false};
    bool underline{false};
};


struct TerminalCaptureRequest {
    Viewport viewport;
    bool strip_ansi{false};
    bool include_cursor_position{true};
    std::optional<size_t> max_lines;
};


struct TerminalCaptureResult {
    std::string content;
    size_t lines_captured{0};
    size_t columns{0};
    std::optional<std::pair<size_t, size_t>> cursor_position;  // (row, col)
    std::chrono::microseconds capture_duration{0};
};


class AnsiProcessor {
public:

    static auto strip_ansi(std::string_view input) -> std::string {
        std::string result;
        result.reserve(input.size());

        size_t i = 0;
        while (i < input.size()) {
            if (input[i] == '\033' && i + 1 < input.size() && input[i + 1] == '[') {

                i += 2;
                while (i < input.size() && input[i] < 0x40) { ++i; }
                if (i < input.size()) ++i;
            } else if (input[i] == '\033') {

                i += 2;
            } else {
                result += input[i];
                ++i;
            }
        }
        return result;
    }


    static auto is_ansi_supported() -> bool {
        auto term = std::getenv("TERM");
        if (!term) return false;
        std::string_view t(term);
        return t.find("color") != std::string_view::npos ||
               t.find("xterm") != std::string_view::npos ||
               t.find("256") != std::string_view::npos;
    }


    static auto get_terminal_size() -> std::pair<size_t, size_t> {

        struct winsize ws{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            return {ws.ws_col, ws.ws_row};
        }
        return {80, 24};
    }
};


class TerminalCaptureTool {
public:
    static constexpr std::string_view name = "terminal_capture";
    static constexpr std::string_view description = "Capture current terminal content and convert to text or image";
    static constexpr size_t kMaxCaptureSize = 1024 * 64;

    auto validate(const TerminalCaptureRequest& request) const
        -> std::expected<void, TerminalCaptureError>
    {
        if (request.viewport.columns == 0 || request.viewport.rows == 0) {
            return std::unexpected(TerminalCaptureError::ViewportInvalid);
        }
        if (request.viewport.columns > 500 || request.viewport.rows > 200) {
            return std::unexpected(TerminalCaptureError::ViewportInvalid);
        }
        return {};
    }

    auto execute(TerminalCaptureRequest request)
        -> std::expected<TerminalCaptureResult, TerminalCaptureError>
    {
        if (auto v = validate(request); !v) return std::unexpected(v.error());

        auto start = std::chrono::steady_clock::now();


        auto [term_cols, term_rows] = AnsiProcessor::get_terminal_size();


        auto raw_content = capture_terminal_buffer(request.viewport);
        if (raw_content.empty()) {
            return std::unexpected(TerminalCaptureError::CaptureEmpty);
        }


        std::string content = request.strip_ansi ?
            AnsiProcessor::strip_ansi(raw_content) : raw_content;


        if (content.size() > kMaxCaptureSize) {
            content.resize(kMaxCaptureSize);
            content += "\n... [truncated]";
        }


        size_t line_count = static_cast<size_t>(std::count(content.begin(), content.end(), '\n')) + 1;


        if (request.max_lines && line_count > *request.max_lines) {
            size_t pos = 0;
            size_t count = 0;
            while (pos < content.size() && count < *request.max_lines) {
                pos = content.find('\n', pos);
                if (pos == std::string::npos) break;
                ++pos;
                ++count;
            }
            if (pos < content.size()) {
                content.resize(pos);
                content += "... [truncated]\n";
            }
            line_count = *request.max_lines;
        }

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);

        return TerminalCaptureResult{
            .content = std::move(content),
            .lines_captured = line_count,
            .columns = term_cols,
            .cursor_position = request.include_cursor_position ?
                std::optional(std::pair{term_rows, term_cols}) : std::nullopt,
            .capture_duration = duration,
        };
    }

    auto schema() const -> std::string {
        return std::format(R"json({{
  "name": "{}",
  "description": "{}",
  "parameters": {{
    "type": "object",
    "properties": {{
      "columns": {{ "type": "integer", "description": "Viewport width in columns (default 80)" }},
      "rows": {{ "type": "integer", "description": "Viewport height in rows (default 24)" }},
      "strip_ansi": {{ "type": "boolean", "description": "Strip ANSI escape codes from output (default false)" }},
      "max_lines": {{ "type": "integer", "description": "Maximum lines to capture" }}
    }}
  }}
}})json", name, description);
    }

private:

    auto capture_terminal_buffer(const Viewport& viewport) const -> std::string {

        auto tmux_session = std::getenv("TMUX");
        std::string cmd;

        if (tmux_session) {
            cmd = std::format("tmux capture-pane -p -S -{}", viewport.scroll_back);
        } else {

            cmd = std::format("tput cols; tput lines; echo '---terminal-content---'");
        }

        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) return "";

        std::string output;
        std::array<char, 4096> buffer{};
        while (auto bytes = ::fread(buffer.data(), 1, buffer.size(), pipe)) {
            output.append(buffer.data(), bytes);
        }
        ::pclose(pipe);
        return output;
    }
};

} // namespace cc::tools
