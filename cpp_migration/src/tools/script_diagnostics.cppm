module;
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <filesystem>
#include <sstream>
#include <regex>
#include <span>
#include <algorithm>

export module cc.tools.script_diagnostics;

export namespace cc::tools {

// 诊断信息条目
struct Diagnostic {
    std::filesystem::path file;  // 源文件路径
    int line = 0;                // 行号
    int column = 0;              // 列号
    std::string message;         // 诊断消息

    // 诊断级别
    enum class Level { Error, Warning, Info } level = Level::Error;
};

// 解析编译器输出，提取诊断信息
inline auto parse_compiler_output(std::string_view output) -> std::vector<Diagnostic> {
    std::vector<Diagnostic> diagnostics;

    // 支持多种编译器输出格式：
    // GCC/Clang: file:line:col: error/warning: message
    // TypeScript: file(line,col): error TSxxxx: message
    // Python: File "file", line N

    // GCC/Clang 格式正则
    static const std::regex gcc_pattern(
        R"(([^:\s]+):(\d+):(\d+):\s*(error|warning|note):\s*(.+))"
    );

    // TypeScript 格式正则
    static const std::regex ts_pattern(
        R"(([^(]+)\((\d+),(\d+)\):\s*(error|warning)\s+\w+:\s*(.+))"
    );

    // Python 格式正则
    static const std::regex py_pattern(
        R"re(File "([^"]+)", line (\d+))re"
    );

    // 逐行解析
    std::istringstream stream{std::string(output)};
    std::string line;

    while (std::getline(stream, line)) {
        std::smatch match;

        // 尝试 GCC/Clang 格式
        if (std::regex_search(line, match, gcc_pattern)) {
            Diagnostic diag;
            diag.file = match[1].str();
            diag.line = std::stoi(match[2].str());
            diag.column = std::stoi(match[3].str());
            diag.message = match[5].str();

            std::string level_str = match[4].str();
            if (level_str == "error") diag.level = Diagnostic::Level::Error;
            else if (level_str == "warning") diag.level = Diagnostic::Level::Warning;
            else diag.level = Diagnostic::Level::Info;

            diagnostics.push_back(std::move(diag));
            continue;
        }

        // 尝试 TypeScript 格式
        if (std::regex_search(line, match, ts_pattern)) {
            Diagnostic diag;
            diag.file = match[1].str();
            diag.line = std::stoi(match[2].str());
            diag.column = std::stoi(match[3].str());
            diag.message = match[5].str();

            std::string level_str = match[4].str();
            diag.level = (level_str == "error")
                ? Diagnostic::Level::Error
                : Diagnostic::Level::Warning;

            diagnostics.push_back(std::move(diag));
            continue;
        }

        // 尝试 Python 格式
        if (std::regex_search(line, match, py_pattern)) {
            Diagnostic diag;
            diag.file = match[1].str();
            diag.line = std::stoi(match[2].str());
            diag.column = 0;
            diag.level = Diagnostic::Level::Error;

            // Python 错误消息通常在后续行
            std::string next_line;
            if (std::getline(stream, next_line)) {
                // 跳过代码行，取错误行
                if (std::getline(stream, next_line)) {
                    diag.message = next_line;
                }
            }

            diagnostics.push_back(std::move(diag));
        }
    }

    return diagnostics;
}

// 格式化诊断信息为可读字符串
inline auto format_diagnostics(
    std::span<const Diagnostic> diagnostics,
    int max_display
) -> std::string {
    std::ostringstream oss;

    // 统计各级别数量
    int errors = 0, warnings = 0, infos = 0;
    for (const auto& diag : diagnostics) {
        switch (diag.level) {
            case Diagnostic::Level::Error:   ++errors;   break;
            case Diagnostic::Level::Warning: ++warnings; break;
            case Diagnostic::Level::Info:    ++infos;    break;
        }
    }

    oss << "Found " << diagnostics.size() << " diagnostic(s): "
        << errors << " error(s), "
        << warnings << " warning(s), "
        << infos << " info(s)\n\n";

    // 显示指定数量的诊断条目
    int displayed = 0;
    for (const auto& diag : diagnostics) {
        if (displayed >= max_display) {
            oss << "... and " << (diagnostics.size() - max_display) << " more\n";
            break;
        }

        // 级别标记
        switch (diag.level) {
            case Diagnostic::Level::Error:   oss << "ERROR"; break;
            case Diagnostic::Level::Warning: oss << "WARN "; break;
            case Diagnostic::Level::Info:    oss << "INFO "; break;
        }

        oss << " " << diag.file.string()
            << ":" << diag.line
            << ":" << diag.column
            << " - " << diag.message << "\n";

        ++displayed;
    }

    return oss.str();
}

// 按文件分组诊断信息
inline auto group_by_file(
    std::span<const Diagnostic> diagnostics
) -> std::map<std::filesystem::path, std::vector<Diagnostic>> {
    std::map<std::filesystem::path, std::vector<Diagnostic>> grouped;

    for (const auto& diag : diagnostics) {
        grouped[diag.file].push_back(diag);
    }

    // 每个文件内按行号排序
    for (auto& [file, diags] : grouped) {
        std::sort(diags.begin(), diags.end(),
            [](const Diagnostic& a, const Diagnostic& b) {
                return a.line < b.line;
            });
    }

    return grouped;
}

} // namespace cc::tools
