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


struct Diagnostic {
    std::filesystem::path file;
    int line = 0;
    int column = 0;
    std::string message;


    enum class Level { Error, Warning, Info } level = Level::Error;
};


inline auto parse_compiler_output(std::string_view output) -> std::vector<Diagnostic> {
    std::vector<Diagnostic> diagnostics;


    // GCC/Clang: file:line:col: error/warning: message
    // TypeScript: file(line,col): error TSxxxx: message
    // Python: File "file", line N


    static const std::regex gcc_pattern(
        R"(([^:\s]+):(\d+):(\d+):\s*(error|warning|note):\s*(.+))"
    );


    static const std::regex ts_pattern(
        R"(([^(]+)\((\d+),(\d+)\):\s*(error|warning)\s+\w+:\s*(.+))"
    );


    static const std::regex py_pattern(
        R"re(File "([^"]+)", line (\d+))re"
    );


    std::istringstream stream{std::string(output)};
    std::string line;

    while (std::getline(stream, line)) {
        std::smatch match;


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


        if (std::regex_search(line, match, py_pattern)) {
            Diagnostic diag;
            diag.file = match[1].str();
            diag.line = std::stoi(match[2].str());
            diag.column = 0;
            diag.level = Diagnostic::Level::Error;


            std::string next_line;
            if (std::getline(stream, next_line)) {

                if (std::getline(stream, next_line)) {
                    diag.message = next_line;
                }
            }

            diagnostics.push_back(std::move(diag));
        }
    }

    return diagnostics;
}


inline auto format_diagnostics(
    std::span<const Diagnostic> diagnostics,
    int max_display
) -> std::string {
    std::ostringstream oss;


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


    int displayed = 0;
    for (const auto& diag : diagnostics) {
        if (displayed >= max_display) {
            oss << "... and " << (diagnostics.size() - max_display) << " more\n";
            break;
        }


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


inline auto group_by_file(
    std::span<const Diagnostic> diagnostics
) -> std::map<std::filesystem::path, std::vector<Diagnostic>> {
    std::map<std::filesystem::path, std::vector<Diagnostic>> grouped;

    for (const auto& diag : diagnostics) {
        grouped[diag.file].push_back(diag);
    }


    for (auto& [file, diags] : grouped) {
        std::sort(diags.begin(), diags.end(),
            [](const Diagnostic& a, const Diagnostic& b) {
                return a.line < b.line;
            });
    }

    return grouped;
}

} // namespace cc::tools
